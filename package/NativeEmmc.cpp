/*
 * Native eMMC block driver for the Xiaomi sirius (SDM710 / sdm670 SDCC1).
 *
 * The transport itself lives in sirius_emmc.c, which the boot probe also uses: that file owns the
 * controller bring-up, the 400 kHz card clock, the ADMA2 transfer and the cache maintenance the
 * data engine needs. This file only turns it into an IOService plus an IOBlockStorageDevice so
 * IOStorageFamily publishes IOMedia and the kernel can find the root window.
 *
 * Read-only by construction: the transfer path has no write command and reportWriteProtection()
 * says so.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <pexpert/pexpert.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>

extern "C" {
/* From sirius_emmc.c. prepare() is idempotent and also fills total_blocks from the GPT. */
int sirius_emmc_prepare(void);
int sirius_emmc_read_block(unsigned long long lba, void *buffer);
extern unsigned long long sirius_emmc_total_blocks;
}

#define EMMC_BLOCK_SIZE 512

extern "C" void sirius_usb_log_char(char c);

/* The kernel console stops feeding the USB ring before bsd_init prints anything (see
 * sirius_emmc.c), so IOLog and printf never reach the host from here. The reports of this file
 * write the characters into the ring directly, which is the same path the eMMC probe uses. */
static void
emmc_log(const char *text) __attribute__((noinline));

static void
emmc_log(const char *text)
{
	while (*text != '\0') {
		sirius_usb_log_char(*text++);
	}
}

static void
emmc_log_dec(const char *label, uint64_t value)
{
	char digits[24];
	unsigned used = 0;
	emmc_log(label);
	if (value == 0) {
		sirius_usb_log_char('0');
	} else {
		while (value != 0 && used < sizeof(digits)) {
			digits[used++] = (char)('0' + (value % 10));
			value /= 10;
		}
		while (used != 0) {
			sirius_usb_log_char(digits[--used]);
		}
	}
	sirius_usb_log_char('\n');
}

static void
emmc_log_hex(const char *label, uint64_t value)
{
	static const char digits[] = "0123456789abcdef";
	emmc_log(label);
	for (int shift = 60; shift >= 0; shift -= 4) {
		sirius_usb_log_char(digits[(value >> shift) & 0xf]);
	}
	sirius_usb_log_char('\n');
}

/* Observability: the drivers above this one log through IOLog, which does not reach the USB
 * ring, so the only way to see how far the storage stack got is to report what it asks for.
 * nemmc_reads counts the transfers the stack issues, and nemmc_heartbeats is written by a thread
 * so a stalled kernel is distinguishable from one that simply has nothing to print. */
/* The transport keeps one bounce buffer and one descriptor table for every device (see
 * sirius_emmc.c), so transfers must be serialised across all block devices, not per device:
 * APFS mounts several volumes at once and would otherwise interleave into the same table. */

/* The transport keeps one staging buffer and one descriptor table for every device (see
 * sirius_emmc.c), so a transfer must be single-flight across all block devices: APFS mounts
 * several volumes and interleaves reads. This is a plain atomic flag with a delay, not an
 * IOLock: the read path can run where sleeping is not allowed, and an IOLock there unbalances
 * the preemption count ("Taking non-sleepable RW lock with preemption enabled", lock_rw.c:1400). */
static volatile uint32_t nemmc_transport_busy;

static void
nemmc_transport_lock_acquire(void)
{
	while (__atomic_exchange_n(&nemmc_transport_busy, 1, __ATOMIC_ACQ_REL) != 0) {
		IODelay(20);
	}
}

static void
nemmc_transport_lock_release(void)
{
	__atomic_store_n(&nemmc_transport_busy, 0, __ATOMIC_RELEASE);
}

volatile uint32_t nemmc_reads;
volatile uint32_t nemmc_blocks;
/* What the driver is doing right now: the heartbeat prints it once a transfer has been in flight
 * for more than a beat, which separates "stuck inside a transfer" from "the caller never asked
 * again" without guessing. */
volatile uint64_t nemmc_inflight_lba;
volatile uint32_t nemmc_inflight_count;
volatile uint32_t nemmc_inflight_beat;
static bool nemmc_panic_dumped;

extern "C" char boot_progress_panic_text[2048];
volatile uint32_t nemmc_heartbeats;

/* Post-exec probes (patches/xnu/sirius-post-exec-trace.patch). The init exec returns 0 and then
 * the stack asks for no block again, which cannot be told apart from "the launchd thread never
 * started" without looking at the other side of the exec. The kernel writes these counters from
 * the user-thread bootstrap, the pid 1 syscall path and the user fault path; the heartbeat below
 * prints them, so one boot says whether the image ran, how far, and where it stopped. */
extern "C" {
volatile int sirius_user_probe_armed;      /* set once the init exec succeeded */
volatile int sirius_user_faults;           /* user faults taken since then */
volatile int sirius_kernel_faults;         /* pid 1 faults taken in kernel mode */
/* Faults of the pid 1 thread, recorded in the handler and printed by the heartbeat: a kprintf
 * from inside the fault path can deadlock on the console the faulting thread already holds. */
volatile unsigned long long sirius_fault_log[8][5];
volatile int sirius_fault_log_count;
/* The environment of the last pid 1 fault: map, its pmap, the live TTBR0, the copyio recovery
 * entry, the fault address and its fault code. */
volatile unsigned long long sirius_fault_extra[10];
volatile unsigned long long sirius_fault_total;
volatile int sirius_init_syscalls;         /* syscalls entered by pid 1 */
volatile int sirius_init_syscall_returns;  /* ... and returned from */
volatile int sirius_init_asts;             /* bsd_ast runs for pid 1 */
volatile int sirius_user_thread_starts;    /* returns to user from a freshly made thread */
thread_t sirius_launchd_thread;            /* the thread the init exec starts */
thread_t sirius_init_thread;               /* the pid 1 thread that ran bsd_ast */

/* struct thread is private to the files compiled with MACH_KERNEL_PRIVATE, so the fields this
 * driver wants are read by two small accessors in osfmk/arm64/sleh.c. */
unsigned int sirius_thread_state(thread_t thread);
unsigned long long sirius_thread_wait_event(thread_t thread);
unsigned long long sirius_thread_user_pc(thread_t thread);
unsigned long long sirius_thread_kstack_word(thread_t thread, unsigned int index);

/* Called from thread_bootstrap_return, i.e. once per new user thread, before its first return
 * to user. Two lines mean the exec'd thread started; one means it never did. */
void
sirius_user_thread_probe(void)
{
	if (sirius_user_thread_starts < 8) {
		sirius_user_thread_starts++;
		emmc_log_hex("SIRIUS user thread start tid ", thread_tid(current_thread()));
		emmc_log_hex("SIRIUS user thread start pc ",
		    sirius_thread_user_pc(current_thread()));
	}
}
}

static unsigned int nemmc_stack_dumps;

/* PSTATE.PAN is set on every exception entry when SCTLR_EL1.SPAN is set, and a set PAN makes
 * every privileged access to a user page fault. This kernel is built with CONFIG_XNU_NATIVE_PAN=n,
 * so copyio.c's user_access_enable()/disable() are empty and nothing would ever clear it, which
 * turns the first kernel write into the init process's map into a fault that the fast fault path
 * resolves and immediately takes again. Clear it once, guarded by the feature check: on cores
 * without PAN the instruction is undefined. */
static int
nemmc_pan_implemented(void)
{
	uint64_t mmfr1;

	__asm__ volatile("mrs %0, ID_AA64MMFR1_EL1" : "=r"(mmfr1));
	return ((mmfr1 >> 20) & 0xf) != 0;
}

static void
nemmc_report_pan(void)
{
	uint64_t pan;

	emmc_log_dec("NEMMC pan implemented ", (uint64_t)nemmc_pan_implemented());
	if (!nemmc_pan_implemented()) {
		return;
	}
	/* PSTATE.PAN as a system register (S3_0_C4_C2_3): clang's named form does not accept it. */
	pan = (uint64_t)__builtin_arm_rsr("S3_0_C4_C2_3");
	emmc_log_dec("NEMMC pan before ", pan);
	if (pan != 0) {
		__builtin_arm_wsr("S3_0_C4_C2_3", 0);
		emmc_log_dec("NEMMC pan after ", (uint64_t)__builtin_arm_rsr("S3_0_C4_C2_3"));
	}
}

static void
nemmc_dump_init_stack(void)
{
	thread_t thread = sirius_init_thread;

	nemmc_stack_dumps++;
	emmc_log_hex("SIRIUS init thread ", (uint64_t)(uintptr_t)thread);
	emmc_log_hex("SIRIUS init state/wait ",
	    ((uint64_t)sirius_thread_state(thread) << 40) |
	    (sirius_thread_wait_event(thread) & 0xffffffffffULL));
	/* A kernel stack is 16 KiB; scan 4 KiB of it and keep only the words that point into
	 * kernel text, which is the call chain the thread is sitting in. */
	for (unsigned int i = 0; i < 512; i++) {
		uint64_t word = sirius_thread_kstack_word(thread, i);

		if (word >= 0xfffffff007004000ULL && word < 0xfffffff010000000ULL) {
			emmc_log_hex("SIRIUS pc ", word);
		}
	}
}

static void
nemmc_heartbeat_thread(void *, wait_result_t)
{
	for (;;) {
		/* The panic screen paints to the framebuffer and the banner then repaints over it, so a
		 * panic message is gone before anyone can read it. The screen code already copies the
		 * formatted message (with its return addresses) into this buffer, so mirror it into the
		 * USB ring the moment it appears: the ring survives, the screen does not. */
		if (!nemmc_panic_dumped && boot_progress_panic_text[0] != 0) {
			nemmc_panic_dumped = true;
			emmc_log("NEMMC PANIC TEXT BEGIN\n");
			for (unsigned i = 0; i < sizeof(boot_progress_panic_text) &&
			    boot_progress_panic_text[i] != 0; i++) {
				sirius_usb_log_char(boot_progress_panic_text[i]);
			}
			emmc_log("\nNEMMC PANIC TEXT END\n");
		}
		emmc_log_hex("NEMMC alive beats/reads ",
		    (((uint64_t)nemmc_heartbeats) << 32) | nemmc_reads);
		emmc_log_hex("NEMMC alive blocks ", nemmc_blocks);
		emmc_log_dec("NEMMC probe user thread starts ", sirius_user_thread_starts);
		emmc_log_dec("NEMMC probe user faults ", sirius_user_faults);
		emmc_log_dec("NEMMC probe init syscalls ", sirius_init_syscalls);
		emmc_log_dec("NEMMC probe init returns ", sirius_init_syscall_returns);
		emmc_log_dec("NEMMC probe init asts ", sirius_init_asts);
		emmc_log_dec("NEMMC probe init faults ", sirius_fault_total);
		{
			static int reported;

			if (sirius_fault_extra[0] != 0 && (nemmc_heartbeats % 8) == 0) {
				emmc_log_hex("SIRIUS fault map ", sirius_fault_extra[0]);
				emmc_log_hex("SIRIUS fault pmap ", sirius_fault_extra[1]);
				emmc_log_hex("SIRIUS fault ttbr0 ", sirius_fault_extra[2]);
				emmc_log_hex("SIRIUS fault recover ", sirius_fault_extra[3]);
				emmc_log_hex("SIRIUS fault addr ", sirius_fault_extra[4]);
				emmc_log_hex("SIRIUS fault code ", sirius_fault_extra[5]);
				emmc_log_hex("SIRIUS fault result ", sirius_fault_extra[6]);
				emmc_log_hex("SIRIUS fault pte ", sirius_fault_extra[7]);
				emmc_log_hex("SIRIUS fault par ", sirius_fault_extra[8]);
				emmc_log_hex("SIRIUS fault kernpte ", sirius_fault_extra[9]);
			}
			while (reported < sirius_fault_log_count && reported < 8) {
				emmc_log_hex("SIRIUS fault class ", sirius_fault_log[reported][0]);
				emmc_log_hex("SIRIUS fault esr ", sirius_fault_log[reported][1]);
				emmc_log_hex("SIRIUS fault far ", sirius_fault_log[reported][2]);
				emmc_log_hex("SIRIUS fault pc ", sirius_fault_log[reported][3]);
				emmc_log_dec("SIRIUS fault user ", sirius_fault_log[reported][4]);
				reported++;
			}
		}
		if (sirius_launchd_thread != nullptr) {
			emmc_log_hex("NEMMC launchd thread ", (uint64_t)(uintptr_t)sirius_launchd_thread);
			emmc_log_hex("NEMMC launchd state/wait ",
			    ((uint64_t)sirius_thread_state(sirius_launchd_thread) << 40) |
			    (sirius_thread_wait_event(sirius_launchd_thread) & 0xffffffffffULL));
		}
		/* The boot stops inside load_init_program_at_path: report where the pid 1 thread sits,
		 * a few times, by dumping the top of its kernel stack. The values are kernel text
		 * addresses, so the call chain can be symbolized against kernel.macho afterwards. */
		if (sirius_init_thread != nullptr && nemmc_stack_dumps < 3 &&
		    nemmc_heartbeats >= 20 + (uint32_t)nemmc_stack_dumps * 5) {
			nemmc_dump_init_stack();
		}
		uint64_t inflight = nemmc_inflight_lba;
		if (inflight != 0 && nemmc_heartbeats - nemmc_inflight_beat > 1) {
			emmc_log_hex("NEMMC inflight lba ", inflight);
			emmc_log_dec("NEMMC inflight count ", nemmc_inflight_count);
			emmc_log_dec("NEMMC inflight beats ", nemmc_heartbeats - nemmc_inflight_beat);
		}
		nemmc_heartbeats++;
		IOSleep(2000);
	}
	thread_terminate(current_thread());
}

static void
nemmc_report_media(void)
{
	OSDictionary *matching = IOService::serviceMatching("IOMedia");
	if (matching == nullptr) {
		emmc_log("NEMMC media: no matching dictionary\n");
		return;
	}
	OSIterator *iterator = IOService::getMatchingServices(matching);
	uint64_t count = 0;
	if (iterator != nullptr) {
		while (iterator->getNextObject() != nullptr) {
			count++;
		}
		iterator->release();
	}
	emmc_log_hex("NEMMC IOMedia count ", count);
	matching->release();
}

/* What the storage stack produced: every IOMedia with its UUID, size and BSD name. The layers
 * that create them log through IOLog, which does not reach the ring, so this is the only way to
 * see whether APFS published the OS volume of the image set. */
static void
nemmc_report_media_table(const char *stage)
{
	emmc_log(stage);
	OSDictionary *matching = IOService::serviceMatching("IOMedia");
	if (matching == nullptr) {
		return;
	}
	OSIterator *iterator = IOService::getMatchingServices(matching);
	unsigned index = 0;
	while (iterator != nullptr && index < 8) {
		IOService *media = OSDynamicCast(IOService, iterator->getNextObject());
		if (media == nullptr) {
			break;
		}
		OSString *uuid = OSDynamicCast(OSString, media->getProperty("UUID"));
		OSString *bsd = OSDynamicCast(OSString, media->getProperty("BSD Name"));
		OSNumber *size = OSDynamicCast(OSNumber, media->getProperty("Size"));
		emmc_log("NEMMC media uuid=");
		const char *text = uuid ? uuid->getCStringNoCopy() : "-";
		for (unsigned i = 0; text[i] != '\0' && i < 40; i++) {
			sirius_usb_log_char(text[i]);
		}
		emmc_log(" bsd=");
		text = bsd ? bsd->getCStringNoCopy() : "-";
		for (unsigned i = 0; text[i] != '\0' && i < 12; i++) {
			sirius_usb_log_char(text[i]);
		}
		sirius_usb_log_char('\n');
		emmc_log_dec("NEMMC media bytes ", size ? size->unsigned64BitValue() : 0);
		index++;
	}
	if (iterator != nullptr) {
		iterator->release();
	}
	matching->release();
	emmc_log_dec("NEMMC media count ", index);
}

/* The completion is fired from a thread call, never inside doAsyncReadWrite. Running it in the
 * caller's context re-enters IOStorageFamily while that stack still holds its own locks, which
 * ends in "Taking non-sleepable RW lock with preemption enabled" (lock_rw.c:1400). The interface
 * is asynchronous for exactly this reason, so the transfer and its completion both happen on the
 * thread call and the caller returns at once. */
struct NemmcRequest {
	IOMemoryDescriptor *buffer;
	UInt64 base;
	UInt64 block;
	UInt64 count;
	IOStorageCompletion completion;
	NemmcRequest *next;
};

/* Requests live on this queue and one worker thread drains it. Handing a request straight to a
 * thread call lost it whenever a call was already pending (a thread_call has a single slot and
 * thread_call_enter1 overwrites its parameter), and running the drain in interrupt context blocked
 * the queue outright once anything in it slept. */
static const unsigned NemmcRequestPoolSize = 32;
static NemmcRequest nemmc_pool[NemmcRequestPoolSize];
static NemmcRequest *nemmc_free_list;
static NemmcRequest *nemmc_pending_head;
static NemmcRequest *nemmc_pending_tail;
static volatile uint32_t nemmc_queue_busy;
static uint8_t *nemmc_bounce;

/* A dedicated kernel thread drains the queue. The thread call this used to run on is dispatched
 * in interrupt context: the completion, IOMemoryDescriptor::writeBytes() and the transfer itself
 * may block there, and a blocked interrupt thread simply stops servicing the queue - which looked
 * exactly like a hang (requests and block count frozen, no error, heartbeats still running). */
static thread_t nemmc_worker;

static void
nemmc_queue_lock(void)
{
	while (__atomic_exchange_n(&nemmc_queue_busy, 1, __ATOMIC_ACQ_REL) != 0) {
		IODelay(20);
	}
}

static void
nemmc_queue_unlock(void)
{
	__atomic_store_n(&nemmc_queue_busy, 0, __ATOMIC_RELEASE);
}

static void
nemmc_drain_queue(void)
{
	for (;;) {
		nemmc_queue_lock();
		NemmcRequest *request = nemmc_pending_head;
		if (request != nullptr) {
			nemmc_pending_head = request->next;
			if (nemmc_pending_head == nullptr) {
				nemmc_pending_tail = nullptr;
			}
		}
		nemmc_queue_unlock();
		if (request == nullptr) {
			return;
		}
		IOReturn result = kIOReturnSuccess;
		for (UInt64 i = 0; i < request->count; i++) {
			nemmc_transport_lock_acquire();
			nemmc_inflight_lba = request->base + request->block + i;
			nemmc_inflight_count = (uint32_t)request->count;
			nemmc_inflight_beat = nemmc_heartbeats;
			int read = sirius_emmc_read_block(request->base + request->block + i, nemmc_bounce);
			nemmc_transport_lock_release();
			nemmc_blocks++;
			if (read != 0) {
				emmc_log_hex("NEMMC transfer failed lba ", request->base + request->block + i);
				result = kIOReturnIOError;
				break;
			}
			if (request->buffer->writeBytes(i * EMMC_BLOCK_SIZE, nemmc_bounce,
			    EMMC_BLOCK_SIZE) != EMMC_BLOCK_SIZE) {
				result = kIOReturnIOError;
				break;
			}
		}
		nemmc_inflight_lba = 0;
		nemmc_inflight_count = 0;
		request->completion.action(request->completion.target, request->completion.parameter,
		    result, result == kIOReturnSuccess ? request->count * EMMC_BLOCK_SIZE : 0);
		nemmc_queue_lock();
		request->next = nemmc_free_list;
		nemmc_free_list = request;
		nemmc_queue_unlock();
	}
}

static void
nemmc_worker_thread(void *, wait_result_t)
{
	for (;;) {
		nemmc_queue_lock();
		bool empty = nemmc_pending_head == nullptr;
		nemmc_queue_unlock();
		if (empty) {
			assert_wait((event_t)&nemmc_worker, THREAD_UNINT);
			nemmc_queue_lock();
			empty = nemmc_pending_head == nullptr;
			nemmc_queue_unlock();
			if (empty) {
				thread_block(THREAD_CONTINUE_NULL);
			} else {
				clear_wait(current_thread(), THREAD_AWAKENED);
			}
			continue;
		}
		nemmc_drain_queue();
	}
}

class NativeEmmcBlock : public IOBlockStorageDevice
{
	OSDeclareDefaultStructors(NativeEmmcBlock);

	UInt64 fBase;
	UInt64 fBlocks;
	IOLock *fLock;
	uint8_t *fBounce;

public:
	bool init(UInt64 base, UInt64 blocks);
	bool start(IOService *provider) override;
	void free() override;

	IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
	IOReturn doFormatMedia(UInt64) override { return kIOReturnNotWritable; }
	UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
	char *getVendorString() override { return const_cast<char *>("Xiaomi"); }
	char *getProductString() override { return const_cast<char *>("sirius eMMC"); }
	char *getRevisionString() override { return const_cast<char *>("1.0"); }
	char *getAdditionalDeviceInfoString() override
	{
		return const_cast<char *>("SDCC1 eMMC, read-only, ADMA2");
	}
	IOReturn reportBlockSize(UInt64 *value) override
	{
		*value = EMMC_BLOCK_SIZE;
		return kIOReturnSuccess;
	}
	IOReturn reportEjectability(bool *value) override
	{
		*value = false;
		return kIOReturnSuccess;
	}
	IOReturn reportMaxValidBlock(UInt64 *value) override
	{
		*value = fBlocks ? fBlocks - 1 : 0;
		return kIOReturnSuccess;
	}
	IOReturn reportMediaState(bool *present, bool *changed) override
	{
		*present = fBlocks != 0;
		if (changed) {
			*changed = false;
		}
		return kIOReturnSuccess;
	}
	IOReturn reportRemovability(bool *value) override
	{
		*value = false;
		return kIOReturnSuccess;
	}
	IOReturn reportWriteProtection(bool *value) override
	{
		*value = true;
		return kIOReturnSuccess;
	}
	IOReturn doSynchronizeCache() override { return kIOReturnSuccess; }
	IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache(); }
	IOReturn getWriteCacheState(bool *value) override
	{
		*value = false;
		return kIOReturnSuccess;
	}
	IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }

	IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
	    IOStorageAttributes *attributes, IOStorageCompletion *completion) override;
};

OSDefineMetaClassAndStructors(NativeEmmcBlock, IOBlockStorageDevice);

bool
NativeEmmcBlock::init(UInt64 base, UInt64 blocks)
{
	/* One request is one run of single-block CMD17 reads at the card's current clock, which takes
	 * tens of milliseconds per block; without this limit IOStorageFamily hands over a whole
	 * megabyte at once and the drain is busy with it for minutes. */
	emmc_log("NativeEmmc: step 7b block base init\n");
	if (!IOBlockStorageDevice::init(nullptr)) {
		return false;
	}
	emmc_log("NativeEmmc: step 7c block base init done\n");
	/* One request is one run of single-block CMD17 reads at the card's current clock, tens of
	 * milliseconds per block; without this cap IOStorageFamily hands over a whole megabyte at
	 * once and the drain is busy with that single request for minutes. */
	setProperty("IOMaximumBlockCountRead", 64, 64);
	fBase = base;
	fBlocks = blocks;
	fLock = IOLockAlloc();
	fBounce = (uint8_t *)IOMalloc(EMMC_BLOCK_SIZE);
	if (!fLock || !fBounce) {
		return false;
	}
	return true;
}

bool
NativeEmmcBlock::start(IOService *provider)
{
	emmc_log("NativeEmmc: step 9a block base start\n");
	if (!fLock || !fBounce || !IOBlockStorageDevice::start(provider)) {
		return false;
	}
	emmc_log("NativeEmmc: step 9b block register\n");
	registerService();
	emmc_log("NativeEmmc: step 9c block registered\n");
	return true;
}

void
NativeEmmcBlock::free()
{
	if (fBounce) {
		IOFree(fBounce, EMMC_BLOCK_SIZE);
		fBounce = nullptr;
	}
	fLock = nullptr;
	IOBlockStorageDevice::free();
}

IOReturn
NativeEmmcBlock::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
    IOStorageAttributes *attributes, IOStorageCompletion *completion)
{
	(void)attributes;
	if (!buffer || !completion || !completion->action || count == 0) {
		emmc_log("NEMMC refuse: no buffer or completion\n");
		return kIOReturnBadArgument;
	}
	if (block >= fBlocks || count > fBlocks - block) {
		emmc_log_hex("NEMMC refuse range block ", block);
		emmc_log_dec("NEMMC refuse range count ", count);
		emmc_log_dec("NEMMC refuse range blocks ", fBlocks);
		return kIOReturnBadArgument;
	}
	if (buffer->getDirection() != kIODirectionIn) {
		emmc_log_dec("NEMMC refuse direction ", (uint64_t)buffer->getDirection());
		return kIOReturnNotWritable;
	}
	if (buffer->getLength() < count * EMMC_BLOCK_SIZE) {
		emmc_log_dec("NEMMC refuse length ", (uint64_t)buffer->getLength());
		emmc_log_dec("NEMMC refuse length need ", count * EMMC_BLOCK_SIZE);
		return kIOReturnBadArgument;
	}

	uint32_t call = nemmc_reads++;
	if (call < 8 || (call & 0xffu) == 0) {
		emmc_log_hex("NEMMC read block ", block);
		emmc_log_hex("NEMMC read count ", count);
	}
	nemmc_queue_lock();
	NemmcRequest *request = nemmc_free_list;
	if (request != nullptr) {
		nemmc_free_list = request->next;
	}
	nemmc_queue_unlock();
	if (request == nullptr) {
		emmc_log("NEMMC refuse: request pool empty\n");
		return kIOReturnNoResources;
	}
	request->buffer = buffer;
	request->base = fBase;
	request->block = block;
	request->count = count;
	request->completion = *completion;
	request->next = nullptr;

	nemmc_queue_lock();
	if (nemmc_pending_tail != nullptr) {
		nemmc_pending_tail->next = request;
	} else {
		nemmc_pending_head = request;
	}
	nemmc_pending_tail = request;
	nemmc_queue_unlock();

	/* The completion must run on the caller's work loop: completing from another thread re-enters
	 * IOStorageFamily while this request's caller still holds the loop, and the queue stops there
	 * (block count frozen, no error, worker asleep). Inline completion is the default; the worker
	 * stays behind sirius_emmc_async=1 for comparison. */
	unsigned async = 0;
	PE_parse_boot_argn("sirius_emmc_async", &async, sizeof(async));
	if (async != 0) {
		thread_wakeup((event_t)&nemmc_worker);
		return kIOReturnSuccess;
	}
	nemmc_drain_queue();
	return kIOReturnSuccess;
}

class NativeEmmc : public IOService
{
	OSDeclareDefaultStructors(NativeEmmc);

	NativeEmmcBlock *fBlock;
	NativeEmmcBlock *fWindow[4];
	char fWindowName[4][33];
	unsigned fWindows;
	int prepareWindows(UInt64 blocks);

public:
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;
	unsigned windowCount() const { return fWindows; }
	bool windowIs(unsigned index, const char *name) const;
	void publishWindow(unsigned index);
};

OSDefineMetaClassAndStructors(NativeEmmc, IOService);

/* The userdata partition starts with the SHNGROOT image-set manifest (scripts/unixnu/userdata.py):
 * "SHNGROOT", u32 version, u32 count, u64 image_bytes, then 128-byte entries
 * {char name[32]; u64 offset; u64 size; u32 block_size; u32 flags; u8 sha256[32]; u8 reserved[40]}
 * where offset is relative to the partition. The windows hold the unchanged OS and SystemOS APFS
 * images of the IPSW, so publishing each as its own read-only block device is what lets the
 * original APFS mount the system volume; this driver never interprets the images. */
#define USERDATA_FIRST_LBA 17301504ull   /* board.toml [userdata] first_lba */
#define MANIFEST_BYTES     4096u

int
NativeEmmc::prepareWindows(UInt64 blocks)
{
	static uint8_t manifest[MANIFEST_BYTES];
	fWindows = 0;
	if (blocks <= USERDATA_FIRST_LBA) {
		emmc_log("NEMMC manifest: no userdata window\n");
		return 0;
	}
	/* These reads share the single staging buffer with every block device, so they take the
	 * transport lock like the request path does. */
	for (unsigned block = 0; block < MANIFEST_BYTES / EMMC_BLOCK_SIZE; block++) {
		nemmc_transport_lock_acquire();
		int read = sirius_emmc_read_block(USERDATA_FIRST_LBA + block,
		    manifest + block * EMMC_BLOCK_SIZE);
		nemmc_transport_lock_release();
		if (read != 0) {
			emmc_log("NEMMC manifest read failed\n");
			return 0;
		}
	}
	const char magic[8] = { 'S', 'H', 'N', 'G', 'R', 'O', 'O', 'T' };
	for (unsigned i = 0; i < sizeof(magic); i++) {
		if (manifest[i] != (uint8_t)magic[i]) {
			emmc_log("NEMMC manifest: no SHNGROOT\n");
			return 0;
		}
	}
	uint32_t version = (uint32_t)manifest[8] | ((uint32_t)manifest[9] << 8) |
	    ((uint32_t)manifest[10] << 16) | ((uint32_t)manifest[11] << 24);
	uint32_t count = (uint32_t)manifest[12] | ((uint32_t)manifest[13] << 8) |
	    ((uint32_t)manifest[14] << 16) | ((uint32_t)manifest[15] << 24);
	emmc_log_dec("NEMMC manifest version ", version);
	emmc_log_dec("NEMMC manifest windows ", count);
	if (count > 4) {
		count = 4;
	}
	for (uint32_t index = 0; index < count; index++) {
		const uint8_t *entry = manifest + 0x18u + index * 128u;
		uint64_t offset = 0, size = 0;
		for (unsigned i = 0; i < 8; i++) {
			offset |= (uint64_t)entry[0x20u + i] << (8 * i);
			size |= (uint64_t)entry[0x28u + i] << (8 * i);
		}
		if (size == 0 || (offset % EMMC_BLOCK_SIZE) != 0 || (size % EMMC_BLOCK_SIZE) != 0 ||
		    offset / EMMC_BLOCK_SIZE >= blocks) {
			emmc_log("NEMMC manifest: malformed window\n");
			continue;
		}
		UInt64 base = USERDATA_FIRST_LBA + offset / EMMC_BLOCK_SIZE;
		UInt64 windowBlocks = size / EMMC_BLOCK_SIZE;
		if (base + windowBlocks > blocks) {
			emmc_log("NEMMC manifest: window outside the device\n");
			continue;
		}
		emmc_log("NEMMC window name=");
		for (unsigned i = 0; i < 32 && entry[i] != 0; i++) {
			sirius_usb_log_char((char)entry[i]);
		}
		sirius_usb_log_char('\n');
		emmc_log_dec("NEMMC window lba ", (uint64_t)base);
		emmc_log_dec("NEMMC window blocks ", windowBlocks);

		NativeEmmcBlock *window = new NativeEmmcBlock;
		if (window == nullptr || !window->init(base, windowBlocks)) {
			emmc_log("NEMMC window: allocation failed\n");
			continue;
		}
		/* Prepared only: the service is started by the publisher thread, the way ShengUfs
		 * publishes its root windows, so no service is created inside the caller's start. */
		if (!window->attach(this)) {
			emmc_log("NEMMC window: attach failed\n");
			window->release();
			continue;
		}
		for (unsigned i = 0; i < 32 && entry[i] != 0; i++) {
			fWindowName[fWindows][i] = (char)entry[i];
			if (i == 31) {
				fWindowName[fWindows][32] = '\0';
			}
		}
		fWindow[fWindows++] = window;
	}
	return (int)fWindows;
}

bool
NativeEmmc::windowIs(unsigned index, const char *name) const
{
	unsigned i = 0;
	for (; i < 32 && name[i] != '\0'; i++) {
		if (fWindowName[index][i] != name[i]) {
			return false;
		}
	}
	return name[i] == '\0' && fWindowName[index][i] == '\0';
}

void
NativeEmmc::publishWindow(unsigned index)
{
	if (index >= fWindows) {
		return;
	}
	emmc_log("NEMMC publish window=");
	for (unsigned i = 0; i < 32 && fWindowName[index][i] != '\0'; i++) {
		sirius_usb_log_char(fWindowName[index][i]);
	}
	sirius_usb_log_char('\n');
	if (!fWindow[index]->start(this)) {
		emmc_log("NEMMC publish: start failed\n");
	} else {
		emmc_log_dec("NEMMC published index ", index);
	}
}

/* The windows are published from here and not in start(): SystemOS first, the other windows
 * next and OS last, which is the order ShengUfs uses so the system volume of the image set is
 * available before the root volume is selected. */
static void
nemmc_window_publisher(void *owner, wait_result_t)
{
	NativeEmmc *driver = (NativeEmmc *)owner;
	unsigned count = driver->windowCount();
	unsigned order[4];
	unsigned used = 0;
	for (unsigned i = 0; i < count; i++) {
		if (driver->windowIs(i, "SystemOS")) {
			order[used++] = i;
		}
	}
	for (unsigned i = 0; i < count; i++) {
		if (!driver->windowIs(i, "SystemOS") && !driver->windowIs(i, "OS")) {
			order[used++] = i;
		}
	}
	for (unsigned i = 0; i < count; i++) {
		if (driver->windowIs(i, "OS")) {
			order[used++] = i;
		}
	}
	for (unsigned i = 0; i < used; i++) {
		driver->publishWindow(order[i]);
	}
	emmc_log_dec("NEMMC publisher done ", used);
	nemmc_report_media_table("NEMMC media table after publish");
	driver->release();
}

bool
NativeEmmc::start(IOService *provider)
{
	fBlock = nullptr;
	emmc_log("NativeEmmc: step 4 base start\n");
	nemmc_report_pan();
	if (!IOService::start(provider)) {
		return false;
	}
	emmc_log("NativeEmmc: step 5 prepare\n");
	if (sirius_emmc_prepare() != 0) {
		emmc_log("NativeEmmc: the eMMC did not come up\n");
		return false;
	}
	UInt64 blocks = sirius_emmc_total_blocks;
	if (blocks == 0) {
		emmc_log("NativeEmmc: no usable GPT on the device\n");
		return false;
	}
	emmc_log("NativeEmmc: step 6 allocate the block device\n");
	fBlock = new NativeEmmcBlock;
	emmc_log_hex("NEMMC step 7 block ", (uint64_t)(uintptr_t)fBlock);
	if (!fBlock || !fBlock->init(0, blocks)) {
		emmc_log("NativeEmmc: cannot allocate the block device\n");
		return false;
	}
	emmc_log("NativeEmmc: step 8 attach the block device\n");
	fBlock->attach(this);
	emmc_log("NativeEmmc: step 9 start the block device\n");
	if (!fBlock->start(this)) {
		emmc_log("NativeEmmc: the block device did not start\n");
		return false;
	}
	emmc_log("NEMMC published blocks:\n"); emmc_log_hex("NEMMC blocks ", blocks);
	/* Publishing the raw manifest windows is off by default: IOStorageFamily tears such a device
	 * down with a double release (see the panic in docs/HANDOFF.zh-CN.md), and the reference
	 * implementation only publishes windows it has verified first. sirius_emmc_win=1 enables the
	 * experiment without rebuilding the kernel. */
	unsigned windows = 0;
	if (PE_parse_boot_argn("sirius_emmc_win", &windows, sizeof(windows)) && windows != 0) {
		emmc_log_dec("NEMMC windows prepared ", (uint64_t)prepareWindows(blocks));
	} else {
		emmc_log("NEMMC windows: disabled (sirius_emmc_win=1 to publish)\n");
	}
	emmc_log("NativeEmmc: step 10 register the driver\n");
	registerService();
	emmc_log("NativeEmmc: step 11 done\n");
	retain();
	thread_t publisher;
	if (kernel_thread_start(nemmc_window_publisher, this, &publisher) == KERN_SUCCESS) {
		thread_deallocate(publisher);
		emmc_log("NEMMC publisher started\n");
	} else {
		release();
		emmc_log("NEMMC publisher thread failed\n");
	}
	return true;
}

void
NativeEmmc::stop(IOService *provider)
{
	/* Published services are never released here. They are still members of the registry, and
	 * releasing an object a collection holds is exactly the over-release that panics with
	 * "A kext releasing a(n) IOBlockStorageDriver has corrupted the registry" (OSObject.cpp:224).
	 * The reference implementation keeps the same rule: started objects retain their owner
	 * references for the lifetime of the kernel. */
	fBlock = nullptr;
	for (unsigned i = 0; i < fWindows; i++) {
		fWindow[i] = nullptr;
	}
	fWindows = 0;
	IOService::stop(provider);
}

/* Entry point of bsd_init (patches/xnu/sirius-usb-console.patch). No personality is installed:
 * the board owns the controller, so the probe creates and starts the driver once, the way the
 * PMGR nub of this port is started. It runs after the console starts, so the numbered steps are
 * in the console ring even if the driver does not return. */
extern "C" void
sirius_native_emmc_probe(void)
{
	static NativeEmmc *device = nullptr;

	unsigned enabled = 1;
	if (PE_parse_boot_argn("sirius_emmc_drv", &enabled, sizeof(enabled)) && enabled == 0) {
		emmc_log("NativeEmmc: disabled by boot arg\n");
		return;
	}
	if (device != nullptr) {
		return;
	}
	if (nemmc_worker == nullptr) {
		nemmc_bounce = (uint8_t *)IOMalloc(EMMC_BLOCK_SIZE);
		if (nemmc_bounce == nullptr) {
			emmc_log("NativeEmmc: no bounce buffer\n");
			return;
		}
		nemmc_free_list = nullptr;
		for (unsigned i = NemmcRequestPoolSize; i > 0; i--) {
			nemmc_pool[i - 1].next = nemmc_free_list;
			nemmc_free_list = &nemmc_pool[i - 1];
		}
		nemmc_pending_head = nullptr;
		nemmc_pending_tail = nullptr;
		thread_t worker = nullptr;
		if (kernel_thread_start(nemmc_worker_thread, nullptr, &worker) != KERN_SUCCESS) {
			emmc_log("NativeEmmc: no worker thread\n");
			return;
		}
		nemmc_worker = worker;
		thread_deallocate(worker);
	}
	emmc_log("NativeEmmc: step 0 enter\n");
	NativeEmmc *candidate = new NativeEmmc;
	emmc_log_hex("NEMMC step 1 instance ", (uint64_t)(uintptr_t)candidate);
	if (candidate == nullptr || !candidate->init()) {
		emmc_log("NativeEmmc: allocation failed\n");
		return;
	}
	emmc_log("NativeEmmc: step 2 init done\n");
	IOService *platform = IOService::getPlatform();
	emmc_log_hex("NEMMC step 3 platform ", (uint64_t)(uintptr_t)platform);
	if (platform != nullptr && !candidate->attach(platform)) {
		emmc_log("NativeEmmc: attach to the platform failed\n");
		candidate->release();
		return;
	}
	if (!candidate->start(platform)) {
		emmc_log("NativeEmmc: the driver did not start\n");
		candidate->detach(platform);
		candidate->release();
		return;
	}
	device = candidate;
	nemmc_report_media();
	thread_t thread;
	if (kernel_thread_start(nemmc_heartbeat_thread, nullptr, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
		emmc_log("NEMMC heartbeat started\n");
	} else {
		emmc_log("NEMMC heartbeat failed\n");
	}
}
