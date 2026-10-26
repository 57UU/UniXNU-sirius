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
emmc_log(const char *text)
{
	while (*text != '\0') {
		sirius_usb_log_char(*text++);
	}
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
volatile uint32_t nemmc_reads;
volatile uint32_t nemmc_heartbeats;

static void
nemmc_heartbeat_thread(void *, wait_result_t)
{
	for (;;) {
		emmc_log_hex("NEMMC alive beats/reads ",
		    (((uint64_t)nemmc_heartbeats) << 32) | nemmc_reads);
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

class NativeEmmcBlock : public IOBlockStorageDevice
{
	OSDeclareDefaultStructors(NativeEmmcBlock);

	UInt64 fBlocks;
	IOLock *fLock;
	uint8_t *fBounce;

public:
	bool init(UInt64 blocks);
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
NativeEmmcBlock::init(UInt64 blocks)
{
	emmc_log("NativeEmmc: step 7b block base init\n");
	if (!IOBlockStorageDevice::init(nullptr)) {
		return false;
	}
	emmc_log("NativeEmmc: step 7c block base init done\n");
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
	if (fLock) {
		IOLockFree(fLock);
		fLock = nullptr;
	}
	IOBlockStorageDevice::free();
}

IOReturn
NativeEmmcBlock::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
    IOStorageAttributes *attributes, IOStorageCompletion *completion)
{
	(void)attributes;
	if (!buffer || !completion || !completion->action || count == 0) {
		return kIOReturnBadArgument;
	}
	if (block >= fBlocks || count > fBlocks - block) {
		return kIOReturnBadArgument;
	}
	if (buffer->getDirection() != kIODirectionIn) {
		return kIOReturnNotWritable;
	}
	if (buffer->getLength() < count * EMMC_BLOCK_SIZE) {
		return kIOReturnBadArgument;
	}

	uint32_t call = nemmc_reads++;
	if (call < 8 || (call & 0xffu) == 0) {
		emmc_log_hex("NEMMC read block ", block);
		emmc_log_hex("NEMMC read count ", count);
	}
	IOReturn result = kIOReturnSuccess;
	IOLockLock(fLock);
	for (UInt64 i = 0; i < count; i++) {
		if (sirius_emmc_read_block(block + i, fBounce) != 0) {
			result = kIOReturnIOError;
			break;
		}
		if (buffer->writeBytes(i * EMMC_BLOCK_SIZE, fBounce, EMMC_BLOCK_SIZE) !=
		    EMMC_BLOCK_SIZE) {
			result = kIOReturnIOError;
			break;
		}
	}
	IOLockUnlock(fLock);

	/* The transfer is synchronous, so the completion fires before returning. */
	completion->action(completion->target, completion->parameter, result,
	    result == kIOReturnSuccess ? count * EMMC_BLOCK_SIZE : 0);
	return result;
}

class NativeEmmc : public IOService
{
	OSDeclareDefaultStructors(NativeEmmc);

	NativeEmmcBlock *fBlock;

public:
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;
};

OSDefineMetaClassAndStructors(NativeEmmc, IOService);

bool
NativeEmmc::start(IOService *provider)
{
	fBlock = nullptr;
	emmc_log("NativeEmmc: step 4 base start\n");
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
	if (!fBlock || !fBlock->init(blocks)) {
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
	emmc_log("NativeEmmc: step 10 register the driver\n");
	registerService();
	emmc_log("NativeEmmc: step 11 done\n");
	return true;
}

void
NativeEmmc::stop(IOService *provider)
{
	if (fBlock) {
		fBlock->terminate(kIOServiceSynchronous);
		fBlock->release();
		fBlock = nullptr;
	}
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
