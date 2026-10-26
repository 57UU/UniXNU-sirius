/* Sirius USB debug console: CDC-ACM log streaming over DWC3.
 *
 * Port of the vermeer (ios-vermeer-lab) USB bring-up to the UniXNU Darwin 23
 * kernel. Log output only: the console mirror ring is streamed on the bulk IN
 * endpoint; there is no command processing or RX data path.
 *
 * Hardware notes (SM7250): the USB controller is a Synopsys DWC3 at
 * 0x0a600000 behind Qualcomm's dwc-usb3-msm wrapper (QSCRATCH at +0xf8800),
 * the same IP generation as SM8550/SM8650 whose constants vermeer validated.
 * The controller is only touched when GCC reports the USB30_PRIM power
 * domain on; the soft reset is bounded. The wrapper is told to use UTMI as
 * the PIPE clock because Sirius has no enabled SuperSpeed PHY, but no clock
 * rate or PHY power state is changed here. ABL/UEFI leave the controller
 * powered from the fastboot session. Boot-arg sirius_usb=1 probes state
 * only, 2 also runs the console.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <machine/atomic.h>
#include <arm/caches_internal.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern uint64_t kvtophys(vm_offset_t va);
extern void console_write_unbuffered(char);

/* ------------------------------------------------------------------ */
/* Console mirror ring: any context, best effort, never waits.        */

#define PURing_SIZE   (256 * 1024)
#define PURing_MASK   (PURing_SIZE - 1)

static struct {
	char        buffer[PURing_SIZE];
	uint64_t    total;
} pur_ring;
/* Boot-progress evidence: the panel's console section is an early snapshot, so the driver
 * publishes its progress as globals that package/BootProgress.c draws on its own row. */
volatile uint32_t sirius_usb_step;      /* 1 enabled, 2 GCC mapped, 3 power ok, 4 SMMU mapped,
                                         * 5 guard ok, 6 DWC3 mapped, 7 gadget started, 8 connected */
volatile uint32_t sirius_usb_gcc;       /* GCC_USB30_PRIM_GDSCR */
volatile uint32_t sirius_usb_smmu;      /* SMMU guard result, 1 = pass */
volatile uint32_t sirius_usb_dwc;       /* DWC3 GSNPSID */
volatile uint32_t sirius_usb_dsts;      /* DWC3 DSTS */
volatile uint32_t sirius_usb_clk;       /* (master|cfg|aggre) CBCR, bit31 = clock off */
volatile uint32_t sirius_usb_pll;       /* GPLL0 enable/status (GCC 0x52000) */
static vm_offset_t sirius_usb_gcc_va;   /* GCC mapping, shared with the PHY power-up */

static uint32_t _Atomic pur_lock;
static uint64_t _Atomic pur_dropped;
static bool pur_live;                   /* set once the TX thread streams */

void sirius_usb_log_char(char c);

void
sirius_usb_log_char(char c)
{
	if (!os_atomic_cmpxchg(&pur_lock, 0, 1, acquire)) {
		os_atomic_add(&pur_dropped, 1, relaxed);
		return;
	}
	pur_ring.buffer[pur_ring.total & PURing_MASK] = c;
	pur_ring.total++;
	os_atomic_store(&pur_lock, 0, release);
}

static uint32_t
pur_copy(uint64_t *pos, char *out, uint32_t max)
{
	uint32_t n = 0;
	if (!os_atomic_cmpxchg(&pur_lock, 0, 1, acquire)) {
		return 0;
	}
	if (pur_ring.total - *pos > PURing_SIZE) {
		*pos = pur_ring.total - PURing_SIZE;
	}
	n = (uint32_t)MIN((uint64_t)max, pur_ring.total - *pos);
	for (uint32_t i = 0; i < n; i++) {
		out[i] = pur_ring.buffer[(*pos + i) & PURing_MASK];
	}
	*pos += n;
	os_atomic_store(&pur_lock, 0, release);
	return n;
}

/* ------------------------------------------------------------------ */
/* DWC3 device mode, EP0 + one bulk IN pair. Register layout follows   */
/* the Synopsys databook as used by Linux drivers/usb/dwc3.           */

#define GCC_PA                  0x00100000u
#define GCC_SIZE                0x00090000u
#define GCC_USB30_PRIM_GDSCR    0x0f004u         /* bit 31: power on */
#define GCC_USB30_PRIM_MASTER   0x0f010u
#define GCC_USB30_PRIM_SLEEP    0x0f018u
#define GCC_USB30_PRIM_MOCK_UTMI 0x0f01cu
#define GCC_CFG_NOC_USB3_PRIM   0x0f07cu
#define GCC_AGGRE_USB3_PRIM     0x0f080u
#define GCC_USB3_PRIM_CLKREF    0x8c010u

#define SMMU_PA                 0x15000000u
#define SMMU_SIZE               0x000d0000u
#define USB_STREAM_ID           0x0740u

#define DWC3_PA                 0x0a600000u
#define DWC3_SIZE               0x00100000u
#define QSCRATCH_OFFSET         0x000f8800u
#define QSCRATCH_PA             (DWC3_PA + QSCRATCH_OFFSET)
#define QSCRATCH_GENERAL_CFG    0x08u
#define   PIPE_UTMI_CLK_SEL     (1u << 0)
#define   PIPE3_PHYSTATUS_SW    (1u << 3)
#define   PIPE_UTMI_CLK_DIS     (1u << 8)
#define QSCRATCH_HS_PHY_CTRL    0x10u
#define   UTMI_OTG_VBUS_VALID   (1u << 20)
#define   SW_SESSVLD_SEL        (1u << 28)
#define QSCRATCH_CGCTL          0x28u
#define QSCRATCH_SS_PHY_CTRL    0x30u
#define   LANE0_PWR_PRESENT     (1u << 24)
#define QSCRATCH_PWR_EVT_STAT   0x58u
#define QSCRATCH_PWR_EVT_MASK   0x5cu

#define GCTL            0xc110u
#define GUSB2PHYCFG0    0xc200u
#define GUSB3PIPECTL0   0xc2c0u
#define GEVNTADRLO      0xc400u
#define GEVNTADRHI      0xc404u
#define GEVNTSIZ        0xc408u
#define GEVNTCOUNT      0xc40cu
#define DCFG            0xc700u
#define DCTL            0xc704u
#define DEVTEN          0xc708u
#define DSTS            0xc70cu
#define DALEPENA        0xc720u
#define DEPCMDPAR2(n)   (0xc800u + 0x10u * (n))
#define DEPCMDPAR1(n)   (0xc804u + 0x10u * (n))
#define DEPCMDPAR0(n)   (0xc808u + 0x10u * (n))
#define DEPCMD(n)       (0xc80cu + 0x10u * (n))

#define DCTL_RUN_STOP   (1u << 31)
#define DCTL_CSFTRST    (1u << 30)
#define DSTS_DEVCTRLHLT (1u << 22)
#define GUSB2PHYCFG_SUSPHY   (1u << 6)
#define GUSB2PHYCFG_ENBLSLPM (1u << 8)
#define GUSB3PIPECTL_SUSPHY  (1u << 17)

#define DEPCMD_SETEPCONFIG      1u
#define DEPCMD_SETTRANSFRESOURCE 2u
#define DEPCMD_SETSTALL         4u
#define DEPCMD_STARTTRANSFER    6u
#define DEPCMD_DEPSTARTCFG      9u
#define DEPCMD_CMDACT           (1u << 10)

#define TRB_HWO         (1u << 0)
#define TRB_LST         (1u << 1)
#define TRB_ISP_IMI     (1u << 10)
#define TRB_IOC         (1u << 11)
#define TRBCTL_NORMAL   (1u << 4)
#define TRBCTL_SETUP    (2u << 4)
#define TRBCTL_STATUS2  (3u << 4)
#define TRBCTL_STATUS3  (4u << 4)
#define TRBCTL_DATA     (5u << 4)

/* physical endpoints: 0/1 = ep0 OUT/IN, 2 = bulk OUT, 3 = bulk IN */
#define EP_RX           2u
#define EP_TX           3u
#define EP_NOTIFY       5u

/* DMA area layout */
#define UD_EVT          0x0000u         /* event buffer, 4 KiB */
#define UD_EVT_SIZE     0x1000u
#define UD_TRB_OUT      0x1000u         /* ep0 OUT TRB */
#define UD_TRB_IN       0x1010u         /* ep0 IN TRB */
#define UD_TRB_TX       0x1030u         /* bulk IN TRB */
#define UD_TRB_NOTIFY   0x1020u         /* interrupt IN endpoint */
#define UD_SETUP        0x1040u         /* 8-byte setup packet */
#define UD_DATA         0x1100u         /* ep0 data stage, 512 bytes */
#define UD_NOTIFY       0x1800u         /* CDC serial-state notification */
#define UD_NOTIFY_SIZE  10u
#define UD_RX           0x3000u         /* bulk OUT: host writes land here (echo test) */
#define UD_ECHO         0x3400u         /* echo reply, copied out of UD_RX */
#define UD_TRB_RX       0x1060u         /* bulk OUT TRB */
#define UD_RX_SIZE      512u
#define UD_TX           0x2000u         /* bulk IN log chunk, 4 KiB */
#define UD_TX_SIZE      0x1000u
#define UD_SIZE         0x4000u

static void ud_rx_arm(void);

static struct {
	vm_offset_t dwc;
	vm_offset_t dma;
	uint64_t    dma_pa;
	uint32_t    evt_pos;
	int         sleep_chan;
	enum { EP0_SETUP, EP0_DATA, EP0_WAIT_STATUS2, EP0_WAIT_STATUS3, EP0_STATUS } state;
	bool        data_in;
	uint8_t     last_request;
	uint16_t    data_length;
	bool        configured;
	bool        tx_busy;
	bool        notify_busy;
	bool        trace_commands;
	bool        smmu_guard;
	bool        live;
	bool        hold;
	uint64_t    tx_pos;                  /* ring reader position */
	uint64_t    events, chars, polls;
	uint32_t    last_event[2];
	uint32_t    tx_delay_ms;
	bool        host_open;
	uint64_t    echoes;
	uint32_t    cmd_fail;
	uint32_t    tx_event;
	uint32_t    rx_event;
} ud;

static inline uint32_t
usb_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

static inline void
usb_wr(vm_offset_t base, uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * delay() can block on this early boot path before the scheduler is usable.
 * Use a bounded ARM yield loop for register settling and polling gaps.
 */
static void
sirius_usb_busy_wait(unsigned usec)
{
	while (usec-- != 0) {
		for (unsigned spin = 0; spin < 4096; spin++) {
			__asm__ volatile ("yield" ::: "memory");
		}
	}
}

static unsigned
sirius_smmu_addr_bits(unsigned code)
{
	switch (code) {
	case 0: return 32;
	case 1: return 36;
	case 2: return 40;
	case 3: return 42;
	case 4: return 44;
	case 5: return 48;
	default: return 0;
	}
}

static inline void
sirius_usb_smmu_write(vm_offset_t base, uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
sirius_usb_sync_mark(const char *text)
{
	while (*text != '\0') {
		console_write_unbuffered(*text++);
	}
	console_write_unbuffered('\n');
}

static void
sirius_usb_sync_mark_index(const char *text, unsigned index)
{
	char digits[10];
	unsigned count = 0;

	while (*text != '\0') {
		console_write_unbuffered(*text++);
	}
	do {
		digits[count++] = (char)('0' + index % 10);
		index /= 10;
	} while (index != 0);
	while (count != 0) {
		console_write_unbuffered(digits[--count]);
	}
	console_write_unbuffered('\n');
}

static void
sirius_usb_sync_hex(const char *text, uint32_t value)
{
	static const char hex[] = "0123456789abcdef";

	while (*text != '\0') {
		console_write_unbuffered(*text++);
	}
	console_write_unbuffered('0');
	console_write_unbuffered('x');
	for (int shift = 28; shift >= 0; shift -= 4) {
		console_write_unbuffered(hex[(value >> shift) & 0xfu]);
	}
	console_write_unbuffered('\n');
}

/*
 * Read-only QSCRATCH diagnostics. Each access has a synchronous breadcrumb
 * before it, so the last visible line identifies a stalled interconnect read.
 */
static void
sirius_usb_qscratch_probe(vm_offset_t qs)
{
	sirius_usb_sync_mark("SIRIUS USB: QPROBE enter");
	sirius_usb_busy_wait(1000);

	sirius_usb_sync_mark("SIRIUS USB: QPROBE GENERAL_CFG read begin");
	uint32_t general = usb_rd(qs, QSCRATCH_GENERAL_CFG);
	sirius_usb_sync_hex("SIRIUS USB: QPROBE GENERAL_CFG=", general);

	sirius_usb_sync_mark("SIRIUS USB: QPROBE HS_PHY_CTRL read begin");
	uint32_t hs = usb_rd(qs, QSCRATCH_HS_PHY_CTRL);
	sirius_usb_sync_hex("SIRIUS USB: QPROBE HS_PHY_CTRL=", hs);

	sirius_usb_sync_mark("SIRIUS USB: QPROBE SS_PHY_CTRL read begin");
	uint32_t ss = usb_rd(qs, QSCRATCH_SS_PHY_CTRL);
	sirius_usb_sync_hex("SIRIUS USB: QPROBE SS_PHY_CTRL=", ss);

	sirius_usb_sync_mark("SIRIUS USB: QPROBE CGCTL read begin");
	uint32_t cgctl = usb_rd(qs, QSCRATCH_CGCTL);
	sirius_usb_sync_hex("SIRIUS USB: QPROBE CGCTL=", cgctl);

	sirius_usb_sync_mark("SIRIUS USB: QPROBE PWR_EVT_STAT read begin");
	uint32_t pwr_stat = usb_rd(qs, QSCRATCH_PWR_EVT_STAT);
	sirius_usb_sync_hex("SIRIUS USB: QPROBE PWR_EVT_STAT=", pwr_stat);

	sirius_usb_sync_mark("SIRIUS USB: QPROBE PWR_EVT_MASK read begin");
	uint32_t pwr_mask = usb_rd(qs, QSCRATCH_PWR_EVT_MASK);
	sirius_usb_sync_hex("SIRIUS USB: QPROBE PWR_EVT_MASK=", pwr_mask);

	sirius_usb_sync_mark("SIRIUS USB: QPROBE GENERAL_CFG2 read begin");
	uint32_t general2 = usb_rd(qs, QSCRATCH_GENERAL_CFG);
	sirius_usb_sync_hex("SIRIUS USB: QPROBE GENERAL_CFG2=", general2);
	sirius_usb_sync_mark("SIRIUS USB: QPROBE complete");
}

/*
 * Minimal read-only experiment for the SMMU register block. The plan/apply
 * path stays untouched; synchronous breadcrumbs are emitted immediately
 * before each read so a bus hang identifies the exact register even when the
 * buffered console cannot drain.
 */
static bool
sirius_usb_smmu_probe_page(vm_offset_t smmu)
{
	sirius_usb_sync_mark("SIRIUS USB: PROBE enter");
	sirius_usb_sync_mark("SIRIUS USB: PROBE sCR0 begin");
	uint32_t scr0 = usb_rd(smmu, 0x000);
	sirius_usb_sync_mark("SIRIUS USB: PROBE sCR0 ok");
	printf("SIRIUS USB: SMMU probe sCR0=%08x\n", scr0);

	sirius_usb_sync_mark("SIRIUS USB: PROBE sGFSR begin");
	uint32_t gfsr = usb_rd(smmu, 0x048);
	sirius_usb_sync_mark("SIRIUS USB: PROBE sGFSR ok");
	printf("SIRIUS USB: SMMU probe sGFSR=%08x\n", gfsr);

	sirius_usb_sync_mark("SIRIUS USB: PROBE IDR begin");
	uint32_t idr0 = usb_rd(smmu, 0x020);
	uint32_t idr1 = usb_rd(smmu, 0x024);
	uint32_t idr2 = usb_rd(smmu, 0x028);
	sirius_usb_sync_mark("SIRIUS USB: PROBE IDR ok");
	printf("SIRIUS USB: SMMU probe idr=%08x/%08x/%08x\n",
	    idr0, idr1, idr2);

	unsigned slots = idr0 & 0xffu;
	if (!slots || slots > 128) {
		printf("SIRIUS USB: SMMU probe bad stream count=%u\n", slots);
		return false;
	}

	unsigned active = 0;
	unsigned free_slot = 0xffffu;
	for (unsigned i = 0; i < slots; i++) {
		sirius_usb_sync_mark_index("SIRIUS USB: PROBE SMR begin ",
		    i);
		uint32_t smr = usb_rd(smmu, 0x800u + 4u * i);
		if (smr & (1u << 31)) {
			active++;
		} else if (free_slot == 0xffffu) {
			free_slot = i;
		}
	}
	printf("SIRIUS USB: SMMU probe complete active=%u free=%u\n",
	    active, free_slot);
	return true;
}

/*
 * The Qualcomm firmware implements Linux's SMMU bypass quirk: a TYPE=BYPASS
 * S2CR reads back as FAULT, while TYPE=TRANSLATE to a disabled
 * S1_TRANS_S2_BYPASS context bank works. Bind only the USB SID, never change
 * sCR0, and leave every other stream route untouched. This is explicitly a
 * debug-boot operation and is lost on the next reset.
 */
struct sirius_usb_smmu_bind {
	unsigned slot;
	unsigned bank;
	uint32_t cbar;
	uint32_t cba2r;
	uint32_t sctlr;
	uint32_t sctlr_off;
};

static bool
sirius_usb_smmu_plan_bind(vm_offset_t smmu,
    struct sirius_usb_smmu_bind *plan, bool verbose)
{
	if (verbose) {
		sirius_usb_sync_mark("SIRIUS USB: PLAN sCR0 begin");
	}
	uint32_t scr0 = usb_rd(smmu, 0x000);
	if (verbose) {
		sirius_usb_sync_mark("SIRIUS USB: PLAN sGFSR begin");
	}
	uint32_t gfsr = usb_rd(smmu, 0x048);
	if (verbose) {
		sirius_usb_sync_mark("SIRIUS USB: PLAN IDR begin");
	}
	uint32_t idr0 = usb_rd(smmu, 0x020);
	uint32_t idr1 = usb_rd(smmu, 0x024);
	uint32_t idr2 = usb_rd(smmu, 0x028);
	unsigned slots = idr0 & 0xffu;
	unsigned banks = idr1 & 0xffu;
	unsigned pages = 1u << (((idr1 >> 28) & 7u) + 1u);
	bool exids = (idr0 & (1u << 8)) != 0;
	unsigned active = 0, free_slot = 0xffffu;
	uint8_t bank_users[128] = { 0 };
	uint32_t smrs[128];

	printf("SIRIUS USB: SMMU bind scr0=%08x gfsr=%08x idr=%08x/%08x/%08x exids=%u\n",
	    scr0, gfsr, idr0, idr1, idr2, exids);
	if (!slots || slots > 128 || !banks || banks > 128 || pages > 128 ||
	    (pages + banks) * 4096u > SMMU_SIZE) {
		printf("SIRIUS USB: SMMU bind unsupported geometry\n");
		return false;
	}
	if (exids) {
		printf("SIRIUS USB: SMMU bind refuses EXIDS layout\n");
		return false;
	}

	for (unsigned i = 0; i < slots; i++) {
		if (verbose) {
			sirius_usb_sync_mark_index(
			    "SIRIUS USB: PLAN SMR begin ", i);
		} else if ((i & 15u) == 0) {
			printf("SIRIUS USB: SMMU bind scan smr %u/%u\n", i, slots);
		}
		smrs[i] = usb_rd(smmu, 0x800u + 4u * i);
		if (smrs[i] & (1u << 31)) {
			active++;
		} else if (free_slot == 0xffffu) {
			free_slot = i;
		}
	}
	printf("SIRIUS USB: SMMU bind smr active=%u free=%u\n",
	    active, free_slot);
	if (free_slot == 0xffffu) {
		printf("SIRIUS USB: SMMU bind no free stream-match slot\n");
		return false;
	}

	for (unsigned i = 0; i < slots; i++) {
		if (!(smrs[i] & (1u << 31))) {
			continue;
		}
		if (verbose) {
			sirius_usb_sync_mark_index(
			    "SIRIUS USB: PLAN S2CR begin ", i);
		}
		uint32_t s2cr = usb_rd(smmu, 0xc00u + 4u * i);
		if (((s2cr >> 16) & 3u) == 0 && (s2cr & 0xffu) < banks) {
			bank_users[s2cr & 0xffu]++;
		}
	}
	printf("SIRIUS USB: SMMU bind s2cr scan done\n");

	for (unsigned attempt = 0; attempt < banks; attempt++) {
		unsigned bank = banks - 1 - attempt;
		if (bank_users[bank]) {
			printf("SIRIUS USB: SMMU bank %u users=%u skip\n",
			    bank, bank_users[bank]);
			continue;
		}
		if (verbose) {
			sirius_usb_sync_mark_index(
			    "SIRIUS USB: PLAN bank begin ", bank);
		}
		printf("SIRIUS USB: SMMU bank %u probe\n", bank);
		uint32_t cbar = usb_rd(smmu, 0x1000u + 4u * bank);
		printf("SIRIUS USB: SMMU bank %u cbar=%08x\n", bank, cbar);
		uint32_t cba2r = usb_rd(smmu, 0x1800u + 4u * bank);
		printf("SIRIUS USB: SMMU bank %u cba2r=%08x\n", bank, cba2r);
		uint32_t sctlr = usb_rd(smmu, (pages + bank) * 4096u);
		printf("SIRIUS USB: SMMU bank %u sctlr=%08x M=%u\n",
		    bank, sctlr, sctlr & 1u);
		if ((sctlr & 1u) != 0 ||
		    (cbar != 0 && cbar != 0x10000u)) {
			continue;
		}

		plan->slot = free_slot;
		plan->bank = bank;
		plan->cbar = cbar;
		plan->cba2r = cba2r | 1u;
		plan->sctlr = sctlr;
		plan->sctlr_off = (pages + bank) * 4096u;
		printf("SIRIUS USB: SMMU bind plan slot=%u bank=%u\n",
		    plan->slot, plan->bank);
		return true;
	}
	printf("SIRIUS USB: SMMU bind found no disabled bypass bank\n");
	return false;
}

static bool
sirius_usb_smmu_apply_bind(vm_offset_t smmu,
    const struct sirius_usb_smmu_bind *plan)
{
	printf("SIRIUS USB: SMMU bind apply slot=%u bank=%u\n",
	    plan->slot, plan->bank);
	if (plan->cbar != 0x10000u) {
		sirius_usb_smmu_write(smmu, 0x1000u + 4u * plan->bank,
		    0x10000u);
	}
	sirius_usb_smmu_write(smmu, 0x1800u + 4u * plan->bank,
	    plan->cba2r);
	uint32_t sctlr_after = usb_rd(smmu, plan->sctlr_off);
	if (usb_rd(smmu, 0x1000u + 4u * plan->bank) != 0x10000u ||
	    usb_rd(smmu, 0x1800u + 4u * plan->bank) != plan->cba2r ||
	    sctlr_after != plan->sctlr) {
		printf("SIRIUS USB: SMMU bank %u setup readback FAIL\n",
		    plan->bank);
		return false;
	}

	sirius_usb_smmu_write(smmu, 0xc00u + 4u * plan->slot, plan->bank);
	if (usb_rd(smmu, 0xc00u + 4u * plan->slot) != plan->bank) {
		printf("SIRIUS USB: SMMU bind S2CR readback FAIL\n");
		return false;
	}
	sirius_usb_smmu_write(smmu, 0x800u + 4u * plan->slot,
	    0x80000000u | USB_STREAM_ID);
	uint32_t smr = usb_rd(smmu, 0x800u + 4u * plan->slot);
	uint32_t s2cr = usb_rd(smmu, 0xc00u + 4u * plan->slot);
	printf("SIRIUS USB: SMMU bind route slot=%u smr=%08x s2cr=%08x bank=%u cbar=%08x cba2r=%08x sctlr=%08x\n",
	    plan->slot, smr, s2cr, plan->bank, plan->cbar, plan->cba2r,
	    plan->sctlr);
	if (smr != (0x80000000u | USB_STREAM_ID) || s2cr != plan->bank) {
		printf("SIRIUS USB: SMMU bind route readback FAIL\n");
		return false;
	}
	return true;
}

/*
 * Read-only guard for the apps SMMU. A debug-only DMA path may use an
 * existing direct/bypass route, but must never create or weaken one.
 */
static bool
sirius_usb_smmu_guard(vm_offset_t smmu, uint64_t dma, uint64_t bytes)
{
	uint32_t scr0 = usb_rd(smmu, 0x000);
	uint32_t idr0 = usb_rd(smmu, 0x020);
	uint32_t idr1 = usb_rd(smmu, 0x024);
	uint32_t idr2 = usb_rd(smmu, 0x028);
	uint32_t gfsr = usb_rd(smmu, 0x048);
	unsigned count = idr0 & 0xffu;
	unsigned banks = idr1 & 0xffu;
	unsigned pages = 1u << (((idr1 >> 28) & 7u) + 1u);
	unsigned matches = 0;
	unsigned match_index = 0;
	uint32_t match_smr = 0;
	uint32_t match_s2cr = 0;
	uint32_t match_mask = 0;

	printf("SIRIUS USB: SMMU scr0=%08x idr=%08x/%08x/%08x gfsr=%08x\n",
	    scr0, idr0, idr1, idr2, gfsr);
	printf("SIRIUS USB: SMMU streams=%u banks=%u pages=%u\n",
	    count, banks, pages);
	if (!count || !banks || pages > 128 ||
	    (pages + banks) * 4096u > SMMU_SIZE) {
		printf("SIRIUS USB: SID e0 guard FAIL: unsupported SMMU geometry\n");
		return false;
	}

	for (unsigned i = 0; i < count; i++) {
		uint32_t smr = usb_rd(smmu, 0x800u + 4u * i);
		if (!(smr & (1u << 31))) {
			continue;
		}
		uint32_t mask = (smr >> 16) & 0x7fffu;
		if (((smr ^ USB_STREAM_ID) & 0xffffu & ~mask) != 0) {
			continue;
		}
		matches++;
		if (matches <= 4) {
			printf("SIRIUS USB: SID e0 match %u smr=%08x s2cr=%08x\n",
			    i, smr, usb_rd(smmu, 0xc00u + 4u * i));
		}
		if (matches == 1) {
			match_index = i;
			match_smr = smr;
			match_mask = mask;
			match_s2cr = usb_rd(smmu, 0xc00u + 4u * i);
		}
	}
	if (matches != 1) {
		printf("SIRIUS USB: SID e0 guard FAIL: matches=%u (need exactly 1)\n",
		    matches);
		return false;
	}
	if (match_mask != 0) {
		printf("SIRIUS USB: SID e0 guard FAIL: non-exact mask=%04x\n",
		    match_mask);
		return false;
	}

	unsigned type = (match_s2cr >> 16) & 3u;
	unsigned bank = match_s2cr & 0xffu;
	uint32_t cbar = 0;
	uint32_t cba2r = 0;
	uint32_t sctlr = 0;
	bool identity = false;

	if (type == 0) {
		if (bank >= banks) {
			printf("SIRIUS USB: SID e0 guard FAIL: cb=%u banks=%u\n",
			    bank, banks);
			return false;
		}
		cbar = usb_rd(smmu, 0x1000u + 4u * bank);
		cba2r = usb_rd(smmu, 0x1800u + 4u * bank);
		sctlr = usb_rd(smmu, (pages + bank) * 4096u);
		identity = ((cbar >> 16) & 3u) == 1u && !(sctlr & 1u);
	} else if (type == 1) {
		identity = true;
	}
	printf("SIRIUS USB: SID e0 match=%u smr=%08x s2cr=%08x type=%u cb=%u\n",
	    match_index, match_smr, match_s2cr, type, bank);
	printf("SIRIUS USB: SID e0 cbar=%08x cba2r=%08x sctlr=%08x path=%s\n",
	    cbar, cba2r, sctlr, identity ? "identity" : "not-identity");
	if (!identity) {
		printf("SIRIUS USB: SID e0 guard FAIL: route is not direct identity\n");
		return false;
	}

	unsigned in_bits = sirius_smmu_addr_bits(idr2 & 0xf);
	unsigned out_bits = sirius_smmu_addr_bits((idr2 >> 4) & 0xf);
	unsigned mid_bits = sirius_smmu_addr_bits((idr2 >> 8) & 0xf);
	printf("SIRIUS USB: SMMU widths in/out/mid=%u/%u/%u DMA=0x%llx+0x%llx\n",
	    in_bits, out_bits, mid_bits, dma, bytes);
	if (bytes) {
		unsigned widths[3] = { in_bits, out_bits, mid_bits };
		for (unsigned i = 0; i < 3; i++) {
			if (!widths[i] || dma >= (1ULL << widths[i]) ||
			    bytes > (1ULL << widths[i]) - dma) {
				printf("SIRIUS USB: SID e0 guard FAIL: DMA exceeds width %u\n",
				    widths[i]);
				return false;
			}
		}
	}
	printf("SIRIUS USB: SID e0 guard PASS\n");
	return true;
}

static inline void
dwc_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(ud.dwc + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static inline uint32_t
dwc_rd(uint32_t off)
{
	return usb_rd(ud.dwc, off);
}

static void
ud_sync(uint32_t off, uint32_t len)
{
	flush_dcache64((addr64_t)(ud.dma_pa + off), len, TRUE);
}

static void
ud_sleep_ms(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&ud.sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static int
ud_epcmd(unsigned ep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
	if (ud.trace_commands) {
		printf("SIRIUS USB: ep%u cmd %u prepare\n", ep, cmd);
	}
	if (ud.trace_commands) printf("SIRIUS USB: ep%u cmd %u PAR0 write begin 0x%08x\n", ep, cmd, p0);
	dwc_wr(DEPCMDPAR0(ep), p0);
	if (ud.trace_commands) printf("SIRIUS USB: ep%u cmd %u PAR0 write returned\n", ep, cmd);
	if (ud.trace_commands) printf("SIRIUS USB: ep%u cmd %u PAR1 write begin 0x%08x\n", ep, cmd, p1);
	dwc_wr(DEPCMDPAR1(ep), p1);
	if (ud.trace_commands) printf("SIRIUS USB: ep%u cmd %u PAR1 write returned\n", ep, cmd);
	if (ud.trace_commands) printf("SIRIUS USB: ep%u cmd %u PAR2 write begin 0x%08x\n", ep, cmd, p2);
	dwc_wr(DEPCMDPAR2(ep), p2);
	if (ud.trace_commands) printf("SIRIUS USB: ep%u cmd %u PAR2 write returned\n", ep, cmd);
	if (ud.trace_commands) printf("SIRIUS USB: ep%u cmd %u command write begin 0x%08x\n", ep, cmd, cmd | DEPCMD_CMDACT);
	*(volatile uint32_t *)(ud.dwc + DEPCMD(ep)) = cmd | DEPCMD_CMDACT;
	if (ud.trace_commands) printf("SIRIUS USB: ep%u cmd %u command store emitted (no dsb)\n", ep, cmd);
	__asm__ volatile ("dsb sy" ::: "memory");
	if (ud.trace_commands) printf("SIRIUS USB: ep%u cmd %u command dsb returned\n", ep, cmd);
	if (ud.trace_commands) {
		printf("SIRIUS USB: ep%u cmd %u command write returned\n", ep, cmd);
		printf("SIRIUS USB: ep%u cmd %u command readback begin\n", ep, cmd);
		uint32_t submitted = dwc_rd(DEPCMD(ep));
		printf("SIRIUS USB: ep%u cmd %u submitted reg 0x%08x\n", ep, cmd, submitted);
	}
	for (int i = 0; i < 1000; i++) {
		uint32_t v = dwc_rd(DEPCMD(ep));
		if (!(v & DEPCMD_CMDACT)) {
			uint32_t status = (v >> 12) & 0xf;
			if (status) {
				ud.cmd_fail = (status << 12) | (ep << 4) | (cmd & 0xfu);
				printf("SIRIUS USB: ep%u cmd %u status %u\n", ep, cmd, status);
				return -1;
			}
			if (ud.trace_commands) {
				printf("SIRIUS USB: ep%u cmd %u complete reg 0x%08x polls %d\n", ep, cmd, v, i + 1);
			}
			return 0;
		}
		sirius_usb_busy_wait(1);
	}
	ud.cmd_fail = (0xfu << 12) | (ep << 4) | (cmd & 0xfu);   /* timeout */
	printf("SIRIUS USB: ep%u cmd %u timeout\n", ep, cmd);
	return -1;
}

/* One TRB (at DMA offset trb) on physical endpoint ep. */
static void
ud_start_trb(unsigned ep, uint32_t trb, uint32_t buf, uint32_t len, uint32_t ctl)
{
	volatile uint32_t *t = (volatile uint32_t *)(ud.dma + trb);
	uint64_t bpa = ud.dma_pa + buf;

	t[0] = (uint32_t)bpa;
	t[1] = (uint32_t)(bpa >> 32);
	t[2] = len;
	t[3] = ctl | TRB_HWO | TRB_LST | TRB_ISP_IMI | TRB_IOC;
	ud_sync(trb, 16);
	if (len) {
		ud_sync(buf, len);
	}
	uint64_t tpa = ud.dma_pa + trb;
	ud_epcmd(ep, DEPCMD_STARTTRANSFER, (uint32_t)(tpa >> 32), (uint32_t)tpa, 0);
}

static void
ud_start_setup(void)
{
	ud.state = EP0_SETUP;
	if (ud.trace_commands) {
		printf("SIRIUS USB: setup TRB prepare va %p pa 0x%llx\n",
		    (void *)(ud.dma + UD_TRB_OUT), ud.dma_pa + UD_TRB_OUT);
	}
	ud_start_trb(0, UD_TRB_OUT, UD_SETUP, 8, TRBCTL_SETUP);
	if (ud.trace_commands) {
		printf("SIRIUS USB: setup TRB command returned\n");
	}
}

static void
ud_stall(void)
{
	ud_epcmd(0, DEPCMD_SETSTALL, 0, 0, 0);
	ud_start_setup();
}

/* CDC-ACM descriptors. VID 0x1209/PID 0x0001 = pid.codes test ID. */
static const uint8_t ud_dev_desc[18] = {
	18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, 64,
	0x09, 0x12, 0x01, 0x00, 0x00, 0x01, 1, 2, 3, 1,
};
static const uint8_t ud_cfg_desc[67] = {
	9, 2, 67, 0, 2, 1, 0, 0x80, 100,                /* config: 2 interfaces, 200 mA */
	9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,             /* control iface 0: CDC ACM */
	5, 0x24, 0x00, 0x10, 0x01,                      /* header, CDC 1.10 */
	5, 0x24, 0x01, 0x00, 1,                         /* call management: data iface 1 */
	4, 0x24, 0x02, 0x02,                            /* ACM: line coding/control-line requests */
	5, 0x24, 0x06, 0, 1,                            /* union: control 0, data 1 */
	/* These are USB endpoint numbers, not physical ones: the core derives the physical endpoint
	 * as 2 * ep + direction, so 0x82 is physical 5, 0x81 is physical 3 and 0x01 is physical 2 -
	 * exactly what this driver configures. Naming 0x85/0x83/0x02 pointed the host at physical
	 * 11/7/4, which do not exist here, so no transfer ever completed. */
	7, 5, 0x82, 0x03, 0x10, 0x00, 16,               /* ep 0x82 interrupt IN, 16 bytes */
	9, 4, 1, 0, 2, 0x0a, 0x00, 0x00, 0,             /* data iface 1 alt 0: bulk IN/OUT */
	/* High-speed bulk endpoints must advertise 512 bytes, not 64. */
	7, 5, 0x81, 0x02, 0x00, 0x02, 0,                /* ep 0x81 bulk IN, 512 */
	7, 5, 0x01, 0x02, 0x00, 0x02, 0,                /* ep 0x01 bulk OUT, 512 */
};
static uint8_t ud_line_coding[7] = {
	0x00, 0xc2, 0x01, 0x00, 0, 0, 8,                /* 115200 8N1 (informational on USB) */
};
static const char *const ud_strings[] = {
	NULL, "sirius", "sirius XNU USB console", "SIRIUS01",
};

static uint32_t
ud_string(unsigned idx, uint8_t *out)
{
	if (idx == 0) {
		out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04;   /* en-US */
		return 4;
	}
	if (idx >= sizeof(ud_strings) / sizeof(ud_strings[0])) {
		return 0;
	}
	const char *str = ud_strings[idx];
	uint32_t n = 2;
	for (; *str && n < 126; str++) {
		out[n++] = (uint8_t)*str;
		out[n++] = 0;
	}
	out[0] = (uint8_t)n;
	out[1] = 3;
	return n;
}

/* SET_CONFIGURATION(1): configure the CDC notification and bulk endpoints. */
static void
ud_configure(void)
{
	if (ud.configured) {
		return;
	}
	if (ud_epcmd(0, DEPCMD_DEPSTARTCFG | (2u << 16), 0, 0, 0) ||
	    ud_epcmd(EP_NOTIFY, DEPCMD_SETEPCONFIG, (3u << 1) | (16u << 3) | (2u << 17), (1u << 8) | (EP_NOTIFY << 25), 0) ||
	    ud_epcmd(EP_NOTIFY, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ud_epcmd(EP_RX, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3), (1u << 8) | (EP_RX << 25), 0) ||
	    ud_epcmd(EP_RX, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ud_epcmd(EP_TX, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3) | (1u << 17),
	    (1u << 8) | (EP_TX << 25), 0) ||
	    ud_epcmd(EP_TX, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) {
		printf("SIRIUS USB: data endpoints setup failed\n");
		return;
	}
	/* Bulk OUT stays enabled but never armed: the host's input NAKs. */
	dwc_wr(DALEPENA, 0x3u | (1u << EP_RX) | (1u << EP_TX) | (1u << EP_NOTIFY));
	ud_rx_arm();
	ud.configured = true;
	ud.tx_busy = false;
	ud.notify_busy = false;
	printf("SIRIUS USB: configured, streaming the console\n");
	{
		uint8_t *notification = (uint8_t *)(ud.dma + UD_NOTIFY);
		notification[0] = 0xa1;       /* CDC class/interface, device-to-host */
		notification[1] = 0x20;       /* SERIAL_STATE */
		notification[2] = 0; notification[3] = 0;
		notification[4] = 0; notification[5] = 0;
		notification[6] = 2; notification[7] = 0;
		notification[8] = 0; notification[9] = 0;
		ud.notify_busy = true;
		ud_start_trb(EP_NOTIFY, UD_TRB_NOTIFY, UD_NOTIFY, UD_NOTIFY_SIZE, TRBCTL_NORMAL);
	}
}

static void
ud_rx_arm(void)
{
	ud_start_trb(EP_RX, UD_TRB_RX, UD_RX, UD_RX_SIZE, TRBCTL_NORMAL);
}

/* EP0 must be re-configured from scratch after every USB reset: the core clears the endpoint
 * configuration, so the SETUP TRB is never consumed until DEPSTARTCFG and the two SETEPCONFIG /
 * SETTRANSFRESOURCE commands have run again. */
static void
ud_ep0_config(void)
{
	ud_epcmd(0, DEPCMD_DEPSTARTCFG, 0, 0, 0);
	ud_epcmd(0, DEPCMD_SETEPCONFIG, (0u << 1) | (64u << 3), (1u << 8) | (1u << 10) | (0u << 25), 0);
	ud_epcmd(0, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0);
	ud_epcmd(1, DEPCMD_SETEPCONFIG, (0u << 1) | (64u << 3), (1u << 8) | (1u << 10) | (1u << 25), 0);
	ud_epcmd(1, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0);
	/* The CDC bulk/interrupt endpoints have to be re-configured and re-enabled too: writing only
	 * EP0's bits here cleared DALEPENA for EP_TX, so the host enlisted the device but never got a
	 * single byte of console data. */
	ud_epcmd(EP_NOTIFY, DEPCMD_SETEPCONFIG, (3u << 1) | (16u << 3) | (2u << 17),
	    (1u << 8) | (EP_NOTIFY << 25), 0);
	ud_epcmd(EP_NOTIFY, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0);
	ud_epcmd(EP_RX, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3),
	    (1u << 8) | (EP_RX << 25), 0);
	ud_epcmd(EP_RX, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0);
	ud_epcmd(EP_TX, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3) | (1u << 17),
	    (1u << 8) | (EP_TX << 25), 0);
	ud_epcmd(EP_TX, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0);
	dwc_wr(DALEPENA, 0x3u | (1u << EP_RX) | (1u << EP_TX) | (1u << EP_NOTIFY));
}

static void
ud_setup(void)
{
	const uint8_t *st = (const uint8_t *)(ud.dma + UD_SETUP);
	uint8_t *data = (uint8_t *)(ud.dma + UD_DATA);
	uint8_t type = st[0], req = st[1];
	uint16_t value = (uint16_t)(st[2] | st[3] << 8);
	uint16_t index = (uint16_t)(st[4] | st[5] << 8);
	uint16_t length = (uint16_t)(st[6] | st[7] << 8);
	uint32_t n = 0;
	bool ok = true;

	ud.data_in = (type & 0x80) != 0;
	ud.last_request = req;
	ud.data_length = length;
	/* Panel evidence: the exact setup packet the host sent and how many bytes we answered, so a
	 * silent host timeout can be told apart from a wrong response. */
	(void)value; (void)index;
	if ((type & 0x60) == 0x20) {   /* CDC class requests on control interface 0 */
		if ((type & 0x1f) != 1 || index != 0) {
			ok = false;
		} else if (req == 0x20 && type == 0x21 && length == sizeof(ud_line_coding)) {
			/* SET_LINE_CODING: copy the host's seven-byte OUT data stage below. */
		} else if (req == 0x21 && type == 0xa1) {
			memcpy(data, ud_line_coding, sizeof(ud_line_coding));
			n = sizeof(ud_line_coding);
		} else if ((req == 0x22 || req == 0x23) && type == 0x21 && length == 0) {
			/* SET_CONTROL_LINE_STATE carries DTR in wValue bit 0. The host raises it when the
			 * tty is opened, which is the only reliable signal that a reader exists: cdc_acm
			 * keeps no backlog for a closed port, so a burst sent earlier is simply lost. */
			if (req == 0x22 && (value & 1u)) {
				ud.host_open = true;
			}
		} else {
			ok = false;
		}
	} else if ((type & 0x60) != 0) {
		ok = false;
	} else if (req == 6) {          /* GET_DESCRIPTOR */
		switch (value >> 8) {
		case 1: memcpy(data, ud_dev_desc, sizeof(ud_dev_desc)); n = sizeof(ud_dev_desc); break;
		case 2: memcpy(data, ud_cfg_desc, sizeof(ud_cfg_desc)); n = sizeof(ud_cfg_desc); break;
		case 3: n = ud_string(value & 0xff, data); ok = n != 0; break;
		default: ok = false; break;
		}
	} else if (req == 5) {          /* SET_ADDRESS */
		uint32_t dcfg = dwc_rd(DCFG) & ~(0x7fu << 3);
		dwc_wr(DCFG, dcfg | ((uint32_t)(value & 0x7f) << 3));
	} else if (req == 9) {          /* SET_CONFIGURATION */
		if (value == 1) {
			ud_configure();
		}
	} else if (req == 8) {          /* GET_CONFIGURATION */
		data[0] = ud.configured ? 1 : 0; n = 1;
	} else if (req == 0) {          /* GET_STATUS */
		data[0] = 0; data[1] = 0; n = 2;
	} else if (req == 11 && (type & 0x1f) == 1 && index == 1 && value == 0) {
		/* SET_INTERFACE: the sole data-interface alternate setting is zero. */
	} else if (req == 10 && (type & 0x1f) == 1 && index == 1) {
		data[0] = 0; n = 1;          /* GET_INTERFACE: alternate setting zero */
	} else {
		ok = false;
	}
	if (!ok) {
		ud_stall();
		return;
	}
	if (length == 0) {
		ud.state = EP0_WAIT_STATUS2;
		return;
	}
	ud.state = EP0_DATA;
	if (ud.data_in) {
		if (n > length) {
			n = length;
		}
		ud_start_trb(1, UD_TRB_IN, UD_DATA, n, TRBCTL_DATA);
	} else {
		ud_start_trb(0, UD_TRB_OUT, UD_DATA, length, TRBCTL_DATA);
	}
}

static void
ud_tx_pump(void)
{
	if (!ud.configured || ud.tx_busy || ud.tx_delay_ms > 0) {
		return;
	}
	uint32_t n = pur_copy(&ud.tx_pos, (char *)(ud.dma + UD_TX), UD_TX_SIZE);
	if (n == 0) {
		return;
	}
	ud.tx_busy = true;
	ud.chars += n;
	ud_start_trb(EP_TX, UD_TRB_TX, UD_TX, n, TRBCTL_NORMAL);
}

static void
ud_event(uint32_t ev)
{
	if (ev & 1) {                   /* device event */
		uint32_t type = (ev >> 8) & 0xf;
		if (type == 1) {        /* USB reset */
			dwc_wr(DCFG, dwc_rd(DCFG) & ~(0x7fu << 3));
			ud.configured = false;
			ud.tx_busy = false;
			ud.host_open = false;
			ud.state = EP0_SETUP;
			ud_ep0_config();
			ud_start_setup();
		} else if (type == 2) { /* connect done */
			uint32_t speed = dwc_rd(DSTS) & 7;
			/* ep0 max packet: 64 at high/full speed (modify action) */
			ud_ep0_config();
			/* The core discards the EP0 setup TRB while the USB reset is being handled, so the
			 * transfer has to be armed again once the connect is complete; otherwise the host's
			 * GET_DESCRIPTOR is NAKed forever and enumeration ends with -110 (observed). */
			dwc_wr(DALEPENA, 0x3);
			ud.state = EP0_SETUP;
			ud_start_setup();
			printf("SIRIUS USB: connected, speed %u\n", speed);
		} else if (type == 0) {
			printf("SIRIUS USB: disconnect\n");
			ud.configured = false;
			ud.tx_busy = false;
			ud.host_open = false;
		}
		return;
	}
	unsigned ep = (ev >> 1) & 0x1f;
	unsigned type = (ev >> 6) & 0xf;
	unsigned status = (ev >> 12) & 0xf;
	if (ep == EP_RX && type == 1) {         /* something arrived from the host */
		ud.rx_event = ev;
		uint32_t n = (ev >> 16) & 0xffffu;
		if (n > UD_RX_SIZE) {
			n = UD_RX_SIZE;
		}
		ud_sync(UD_RX, UD_RX_SIZE);
		/* Any byte from the host means "send me the log again": the ring is otherwise only handed
		 * over once, at enumeration, so a capture that starts a moment too late sees nothing. */
		if (n != 0) {
			/* Rewind and immediately pump: the boot-time loop that normally drives the
			 * console is long gone by the time a host asks for the log again. */
			ud.tx_pos = 0;
			ud.tx_delay_ms = 0;
			ud_tx_pump();
		}
		if (n != 0 && !ud.tx_busy) {
			for (uint32_t i = 0; i < n; i++) {
				*(volatile char *)(ud.dma + UD_ECHO + i) =
				    *(volatile char *)(ud.dma + UD_RX + i);
			}
			ud.tx_busy = true;
			ud.echoes++;
			ud_start_trb(EP_TX, UD_TRB_TX, UD_ECHO, n, TRBCTL_NORMAL);
		}
		ud_rx_arm();
		return;
	}
	if (ep == EP_TX && type == 1) {         /* bulk IN complete */
		ud.tx_event = ev;
		ud.tx_busy = false;
		return;
	}
	if (ep == EP_NOTIFY && type == 1) {    /* CDC interrupt notification complete */
		ud.notify_busy = false;
		return;
	}
	if (ep > 1) {
		return;
	}
	if (type == 1) {                /* XferComplete */
		switch (ud.state) {
		case EP0_SETUP:
			ud_sync(UD_SETUP, 8);
			ud_setup();
			break;
		case EP0_DATA:
			if (!ud.data_in && ud.last_request == 0x20 &&
			    ud.data_length == sizeof(ud_line_coding)) {
				ud_sync(UD_DATA, ud.data_length);
				memcpy(ud_line_coding, (const void *)(ud.dma + UD_DATA), sizeof(ud_line_coding));
			}
			ud.state = EP0_WAIT_STATUS3;
			break;
		case EP0_STATUS:
			ud_start_setup();
			break;
		default:
			break;
		}
		return;
	}
	if (type == 3 && status == 2) { /* XferNotReady, control status phase */
		if (ud.state == EP0_WAIT_STATUS2 || ud.state == EP0_WAIT_STATUS3) {
			uint32_t ctl = ud.state == EP0_WAIT_STATUS3 ? TRBCTL_STATUS3 : TRBCTL_STATUS2;
			ud.state = EP0_STATUS;
			ud_start_trb(ep, ep ? UD_TRB_IN : UD_TRB_OUT, UD_DATA, 0, ctl);
		}
	}
}

static void
ud_poll(void)
{
	uint32_t count = dwc_rd(GEVNTCOUNT) & 0xffffu;
	if (count) {
		ud_sync(UD_EVT, UD_EVT_SIZE);
		for (uint32_t done = 0; done < count; done += 4) {
			uint32_t ev = *(volatile uint32_t *)(ud.dma + UD_EVT + ud.evt_pos);
			ud.evt_pos = (ud.evt_pos + 4) % UD_EVT_SIZE;
			ud.last_event[1] = ud.last_event[0];
			ud.last_event[0] = ev;
			if (ud.events++ < 200) {
				printf("SIRIUS USB: event 0x%08x\n", ev);
			}
			ud_event(ev);
		}
		dwc_wr(GEVNTCOUNT, count);
	}
	ud_tx_pump();
	ud.polls++;
	if (ud.live) {
		/* Live evidence for the panel: how many events the core delivered and how many are
		 * still queued. A stuck zero means the controller is not posting events at all. */
		sirius_usb_smmu = (uint32_t)ud.events;
		sirius_usb_pll = ud.cmd_fail != 0 ? ud.cmd_fail : (uint32_t)ud.echoes;
		/* FIFO RAM allocation: if these are zero the data endpoints have no FIFO, which makes
		 * SETTRANSFRESOURCE succeed while no transfer is ever executed. */
		sirius_usb_gcc = ud.tx_event;          /* last bulk IN completion event */
		sirius_usb_clk = ud.rx_event;          /* last bulk OUT completion event */
		sirius_usb_smmu = (uint32_t)ud.chars;  /* bytes submitted on bulk IN */
		sirius_usb_pll = (uint32_t)ud.echoes;  /* round trips from the echo path */
		sirius_usb_dwc = (uint32_t)ud.polls;
	}
}

/* Called from the boot-progress refresh, which demonstrably keeps running even when the poller
 * thread is never scheduled: without this the console mirror stops pumping the moment the inline
 * window closes, and the host sees a port that never delivers a byte. */
void
sirius_usb_poll(void)
{
	if (ud.tx_delay_ms > 0) {
		ud.tx_delay_ms--;
	}
	if (ud.live) {
		ud_poll();
	}
}

static void
ud_thread(void *arg __unused, wait_result_t wr __unused)
{
	unsigned ticks = 0;
	for (;;) {
		ud_poll();
		/* Once a second, rewind the ring so a host that attaches long after boot still gets the
		 * whole log. Without this the ring is handed over exactly once, inside the boot window,
		 * and every capture that starts a moment later sees nothing. */
		if (++ticks >= 500) {
			ticks = 0;
			ud.tx_pos = 0;
		}
		ud_sleep_ms(2);
	}
}

static void
sirius_usb_park(int hold)
{
	if (!hold) {
		return;
	}
	printf("SIRIUS USB: holding at bsd_init; SMMU guard %s\n",
	    ud.smmu_guard ? "PASS" : "FAIL");
	for (;;) {
		if (ud.live) {
			ud_poll();
			sirius_usb_busy_wait(1000);
		} else {
			ud_sleep_ms(1000);
		}
	}
}

static void
sirius_usb_select_utmi_clk(vm_offset_t qs)
{
	sirius_usb_sync_mark("SIRIUS USB: UTMI enter");
	sirius_usb_sync_mark("SIRIUS USB: UTMI before read begin");
	uint32_t before = usb_rd(qs, QSCRATCH_GENERAL_CFG);
	sirius_usb_sync_hex("SIRIUS USB: UTMI before=", before);

	sirius_usb_sync_mark("SIRIUS USB: UTMI disable write begin");
	usb_wr(qs, QSCRATCH_GENERAL_CFG, before | PIPE_UTMI_CLK_DIS);
	sirius_usb_sync_mark("SIRIUS USB: UTMI disable write done");
	sirius_usb_busy_wait(5);
	sirius_usb_sync_mark("SIRIUS USB: UTMI disabled read begin");
	uint32_t disabled = usb_rd(qs, QSCRATCH_GENERAL_CFG);
	sirius_usb_sync_hex("SIRIUS USB: UTMI disabled=", disabled);

	sirius_usb_sync_mark("SIRIUS USB: UTMI select write begin");
	usb_wr(qs, QSCRATCH_GENERAL_CFG,
	    disabled | PIPE_UTMI_CLK_SEL | PIPE3_PHYSTATUS_SW);
	sirius_usb_sync_mark("SIRIUS USB: UTMI select write done");
	sirius_usb_busy_wait(5);
	sirius_usb_sync_mark("SIRIUS USB: UTMI selected read begin");
	uint32_t selected = usb_rd(qs, QSCRATCH_GENERAL_CFG);
	sirius_usb_sync_hex("SIRIUS USB: UTMI selected=", selected);

	sirius_usb_sync_mark("SIRIUS USB: UTMI enable write begin");
	usb_wr(qs, QSCRATCH_GENERAL_CFG, selected & ~PIPE_UTMI_CLK_DIS);
	sirius_usb_sync_mark("SIRIUS USB: UTMI enable write done");
	sirius_usb_busy_wait(5);
	sirius_usb_sync_mark("SIRIUS USB: UTMI final read begin");
	uint32_t after = usb_rd(qs, QSCRATCH_GENERAL_CFG);
	sirius_usb_sync_hex("SIRIUS USB: UTMI final=", after);

	sirius_usb_sync_mark("SIRIUS USB: UTMI complete");
}

/* QUSB2 PHY at 0x88e2000 (USB2 only, the port has no SuperSpeed PHY). Registers and the init
 * table are the qusb2-v2 values from the Linux driver; without this the DWC3 has no PIPE clock and
 * its endpoint commands never complete (observed: the core stays halted at DEVCTRLHLT). */
#define PHY_PA 0x088e2000u
#define PHY_SIZE 0x400u
#define PHY_POWERDOWN 0x210u
#define PHY_PLL_STATUS 0x1a0u
#define PHY_PLL_LOCKED (1u << 5)

static bool
sirius_usb_phy_power(vm_offset_t gcc)
{
	/* AHB2PHY branch, then the QUSB2PHY_PRIM block reset: assert, 100 us, deassert. */
	usb_wr(gcc, 0x6a004, usb_rd(gcc, 0x6a004) | 1u);
	for (unsigned ms = 0; ms < 50; ms++) {
		if (!(usb_rd(gcc, 0x6a004) & (1u << 31))) { break; }
		sirius_usb_busy_wait(1000);
	}
	if (usb_rd(gcc, 0x6a004) & (1u << 31)) {
		sirius_usb_step = 64;                    /* AHB2PHY branch never left halt */
		sirius_usb_clk = usb_rd(gcc, 0x6a004);
		sirius_usb_smmu = usb_rd(gcc, 0x6a004);
		sirius_usb_pll = usb_rd(gcc, 0x6a000);
		return false;
	}
	/* USB3/2 PHY reference clock (CXO through GCC): the QUSB2 PLL never locks without it.
	 * SDM845 keeps this branch at 0x8c008 (the SM7250 port assumed 0x8c010, which is not a branch). */
	usb_wr(gcc, 0x8c008, usb_rd(gcc, 0x8c008) | 1u);
	for (unsigned ms = 0; ms < 50; ms++) {
		if (!(usb_rd(gcc, 0x8c008) & (1u << 31))) { break; }
		sirius_usb_busy_wait(1000);
	}
	if (usb_rd(gcc, 0x8c008) & (1u << 31)) {
		sirius_usb_step = 63;                    /* clkref branch stayed halted */
		sirius_usb_clk = usb_rd(gcc, 0x8c008);
		return false;
	}
	usb_wr(gcc, 0x12000, 1u);
	sirius_usb_busy_wait(200);
	usb_wr(gcc, 0x12000, 0u);
	sirius_usb_busy_wait(200);

	vm_offset_t phy = ml_io_map(PHY_PA, PHY_SIZE);
	if (phy == 0) {
		sirius_usb_step = 65;
		return false;
	}
	static const struct { uint32_t off, value; } table[] = {
		{0x04, 0x03}, {0x18c, 0x7c}, {0x2c, 0x80}, {0x184, 0x0a}, {0xb4, 0x19},
		{0x194, 0x40}, {0x198, 0x20}, {0x214, 0x21}, {0x220, 0x00}, {0x224, 0x58},
		{0x240, 0x30}, {0x244, 0x29}, {0x248, 0xca}, {0x24c, 0x04}, {0x250, 0x03},
		{0x23c, 0x00},
	};
	/* Hold the PHY down with the v2 disable bits while the table is programmed, and hold the core
	 * PLL in reset: CORE_PLL_EN_FROM_RESET | CORE_RESET | CORE_RESET_MUX at 0xa8. The Linux v2
	 * sequence always does this; without it the PLL never leaves reset and PLL_LOCKED stays 0. */
	usb_wr(phy, PHY_POWERDOWN, usb_rd(phy, PHY_POWERDOWN) | 0x23u);
	usb_wr(phy, 0xa8, usb_rd(phy, 0xa8) | 0x70u);
	for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		usb_wr(phy, table[i].off, table[i].value);
	}
	usb_wr(phy, PHY_POWERDOWN, usb_rd(phy, PHY_POWERDOWN) & ~1u);
	sirius_usb_busy_wait(200);
	usb_wr(phy, 0xa8, usb_rd(phy, 0xa8) & ~0x60u);   /* CORE_RESET | CORE_RESET_MUX off */
	sirius_usb_busy_wait(200);
	/* This PHY reports readiness in two places that the Linux v2 layout does not both name: the
	 * core-ready bit (0x1a4 bit 0) and the PLL lock bit, which on this part sits in 0x1ac rather
	 * than 0x1a0's bit 5. Accept either signal. */
	for (unsigned ms = 0; ms < 100; ms++) {
		if ((usb_rd(phy, 0x1a4) & 1u) || (usb_rd(phy, 0x1ac) & PHY_PLL_LOCKED) ||
		    (usb_rd(phy, PHY_PLL_STATUS) & PHY_PLL_LOCKED)) {
			return true;
		}
		sirius_usb_busy_wait(1000);
	}
	sirius_usb_step = 66;                            /* PHY programmed, PLL never locked */
	/* One boot, as much evidence as the panel can carry: which of the candidate PLL status
	 * registers has bit 5 set (0 = none), the status register itself, the next one, and the
	 * PLL_CORE_INPUT_OVERRIDE readback (bit 4/5/6 say whether the core-reset cycle stuck). */
	uint32_t found = 0;
	for (uint32_t off = 0x1a0; off <= 0x1b8; off += 4) {
		if (usb_rd(phy, off) & PHY_PLL_LOCKED) { found = off; break; }
	}
	sirius_usb_clk = found;
	sirius_usb_smmu = usb_rd(phy, PHY_PLL_STATUS);
	sirius_usb_pll = usb_rd(phy, 0x1a4);
	sirius_usb_dwc = usb_rd(phy, 0xa8);
	return false;
}

static void
sirius_usb_gadget_start(vm_offset_t dwc, vm_offset_t smmu)
{
	vm_offset_t qs;
	uint32_t devid = usb_rd(dwc, 0xc120);
	uint32_t hwparams0 = usb_rd(dwc, 0xc140);
	unsigned awidth = hwparams0 >> 24;
	if (!sirius_usb_phy_power(sirius_usb_gcc_va)) {
		sirius_usb_step = 68;
		return;
	}
	sirius_usb_step = 69;
	uint32_t gctl = usb_rd(dwc, GCTL);

	ud.dwc = dwc;
	printf("SIRIUS USB: controller id=%08x hwparams0=%08x awidth=%u gctl=%08x\n",
	    devid, hwparams0, awidth, gctl);
	if ((devid >> 16) != 0x5533 && (devid >> 16) != 0x3331) {
		printf("SIRIUS USB: unsupported DWC3 id; controller untouched\n");
		return;
	}
	if (((gctl >> 12) & 3u) != 2u) {
		printf("SIRIUS USB: not in device mode; controller untouched\n");
		return;
	}
	if (kmem_alloc_contig(kernel_map, &ud.dma, UD_SIZE, PAGE_MASK, 0, 0,
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("SIRIUS USB: no DMA memory\n");
		return;
	}
	/* pmap_find_phys can return 0 for freshly kmem-allocated pages on this
	 * kernel; kvtophys is the canonical VA-to-PA walk. Fail loudly either way. */
	ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)ud.dma);
	uint64_t pa = pn ? ptoa_64(pn) : (uint64_t)kvtophys(ud.dma);
	if (!pa) {
		printf("SIRIUS USB: cannot resolve DMA physical address at %p\n", (void *)ud.dma);
		return;
	}
	ud.dma_pa = pa;
	sirius_usb_step = 70;
	sirius_usb_clk = dwc_rd(GCTL);
	sirius_usb_smmu = dwc_rd(DCTL);
	sirius_usb_pll = dwc_rd(DSTS);
	for (unsigned off = 0; off < UD_SIZE; off += PAGE_SIZE) {
		ppnum_t page = pmap_find_phys(kernel_pmap, (addr64_t)(ud.dma + off));
		if (!page || ptoa_64(page) != ud.dma_pa + off) {
			printf("SIRIUS USB: DMA is not physically contiguous at +0x%x\n",
			    off);
			return;
		}
	}
	printf("SIRIUS USB: DMA %d pages at va %p pa 0x%llx\n",
	    (int)(UD_SIZE / PAGE_SIZE), (void *)ud.dma, ud.dma_pa);
	ud.smmu_guard = sirius_usb_smmu_guard(smmu, ud.dma_pa, UD_SIZE);
	if (!ud.smmu_guard) {
		printf("SIRIUS USB: SMMU guard refused; controller untouched\n");
		return;
	}
	if (!awidth || awidth < 32 || (awidth < 64 &&
	    ud.dma_pa + UD_SIZE > (1ULL << awidth))) {
		printf("SIRIUS USB: DMA exceeds DWC3 awidth=%u; controller untouched\n",
		    awidth);
		return;
	}
	printf("SIRIUS USB: hardware guards PASS; issuing controller reset\n");
	uint32_t usb2phy = usb_rd(dwc, GUSB2PHYCFG0);
	uint32_t usb3pipe = usb_rd(dwc, GUSB3PIPECTL0);
	printf("SIRIUS USB: pre-stop GCTL=%08x DCFG=%08x DCTL=%08x DSTS=%08x USB2PHYCFG=%08x USB3PIPECTL=%08x\n",
	    dwc_rd(GCTL), dwc_rd(DCFG), dwc_rd(DCTL), dwc_rd(DSTS), usb2phy,
	    usb3pipe);
	dwc_wr(DEVTEN, 0);
	dwc_wr(DCTL, dwc_rd(DCTL) & ~DCTL_RUN_STOP);
	unsigned wait;
	for (wait = 0; wait < 500 &&
	    !(dwc_rd(DSTS) & DSTS_DEVCTRLHLT); wait++) {
		sirius_usb_busy_wait(1000);
	}
	printf("SIRIUS USB: pre-reset halt %s after %u ms DSTS=%08x\n",
	    (dwc_rd(DSTS) & DSTS_DEVCTRLHLT) ? "done" : "TIMEOUT",
	    wait, dwc_rd(DSTS));
	if (!(dwc_rd(DSTS) & DSTS_DEVCTRLHLT)) {
		return;
	}
	sirius_usb_step = 71;
	sirius_usb_clk = dwc_rd(GCTL);
	sirius_usb_smmu = dwc_rd(DCTL);
	sirius_usb_pll = dwc_rd(DSTS);
	dwc_wr(GUSB2PHYCFG0, usb2phy &
	    ~(GUSB2PHYCFG_SUSPHY | GUSB2PHYCFG_ENBLSLPM));
	dwc_wr(GUSB3PIPECTL0, usb3pipe & ~GUSB3PIPECTL_SUSPHY);

	printf("SIRIUS USB: asserting soft reset\n");
	dwc_wr(DCTL, DCTL_CSFTRST);
	printf("SIRIUS USB: reset asserted, DCTL 0x%08x\n", dwc_rd(DCTL));
	int i;
	for (i = 0; i < 500 && (dwc_rd(DCTL) & DCTL_CSFTRST); i++) {
		sirius_usb_busy_wait(1000);
	}
	printf("SIRIUS USB: soft reset %s after %d ms, GCTL 0x%08x DCFG 0x%08x\n",
	    (dwc_rd(DCTL) & DCTL_CSFTRST) ? "STUCK" : "done", i, dwc_rd(GCTL), dwc_rd(DCFG));
	if (dwc_rd(DCTL) & DCTL_CSFTRST) {
		return;
	}
	sirius_usb_step = 72;
	sirius_usb_clk = dwc_rd(GCTL);
	sirius_usb_smmu = dwc_rd(DCTL);
	sirius_usb_pll = dwc_rd(DSTS);

	qs = dwc + QSCRATCH_OFFSET;
	printf("SIRIUS USB: QSCRATCH %p (wrapper map %p + 0x%x)\n",
	    (void *)qs, (void *)dwc, QSCRATCH_OFFSET);
	sirius_usb_select_utmi_clk(qs);
	sirius_usb_step = 73;
	sirius_usb_clk = dwc_rd(GCTL);
	sirius_usb_smmu = dwc_rd(DCTL);
	sirius_usb_pll = dwc_rd(DSTS);

	dwc_wr(GEVNTADRLO, (uint32_t)(ud.dma_pa + UD_EVT));
	dwc_wr(GEVNTADRHI, (uint32_t)((ud.dma_pa + UD_EVT) >> 32));
	dwc_wr(GEVNTSIZ, (1u << 31) | UD_EVT_SIZE);     /* interrupt masked: polled */
	dwc_wr(GEVNTCOUNT, dwc_rd(GEVNTCOUNT) & 0xffff);
	ud.evt_pos = 0;
	ud.trace_commands = true;
	dwc_wr(DCFG, dwc_rd(DCFG) & ~(7u | (0x7fu << 3)));

	printf("SIRIUS USB: ep0 DEPSTARTCFG\n");
	if (ud_epcmd(0, DEPCMD_DEPSTARTCFG, 0, 0, 0)) { return; }
	printf("SIRIUS USB: ep0 out config\n");
	if (ud_epcmd(0, DEPCMD_SETEPCONFIG, (0u << 1) | (64u << 3), (1u << 8) | (1u << 10) | (0u << 25), 0)) { return; }
	printf("SIRIUS USB: ep0 out fifo\n");
	if (ud_epcmd(0, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) { return; }
	printf("SIRIUS USB: ep0 in config\n");
	if (ud_epcmd(1, DEPCMD_SETEPCONFIG, (0u << 1) | (64u << 3), (1u << 8) | (1u << 10) | (1u << 25), 0)) { return; }
	printf("SIRIUS USB: ep0 in fifo\n");
	if (ud_epcmd(1, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) { return; }
	printf("SIRIUS USB: ep0 in resources ready\n");
	dwc_wr(DALEPENA, 0x3);
	sirius_usb_step = 74;
	sirius_usb_clk = dwc_rd(GCTL);
	sirius_usb_smmu = dwc_rd(DCTL);
	sirius_usb_pll = dwc_rd(DSTS);
	printf("SIRIUS USB: ep0 enabled\n");

	uint32_t hs = usb_rd(qs, QSCRATCH_HS_PHY_CTRL);
	uint32_t ss = usb_rd(qs, QSCRATCH_SS_PHY_CTRL);
	printf("SIRIUS USB: QSCRATCH HS_PHY_CTRL 0x%08x SS_PHY_CTRL 0x%08x\n", hs, ss);
	usb_wr(qs, QSCRATCH_HS_PHY_CTRL,
	    hs | UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL);
	usb_wr(qs, QSCRATCH_SS_PHY_CTRL, ss | LANE0_PWR_PRESENT);
	printf("SIRIUS USB: QSCRATCH after HS_PHY_CTRL 0x%08x SS_PHY_CTRL 0x%08x\n",
	    usb_rd(qs, QSCRATCH_HS_PHY_CTRL),
	    usb_rd(qs, QSCRATCH_SS_PHY_CTRL));
	sirius_usb_step = 75;
	sirius_usb_clk = dwc_rd(GCTL);
	sirius_usb_smmu = dwc_rd(DCTL);
	sirius_usb_pll = dwc_rd(DSTS);

	ud_start_setup();
	printf("SIRIUS USB: setup TRB armed before RUN (%u retries)\n",
	    (unsigned)usb_rd(dwc, DEPCMD(0)) >> 12 & 0xfu);
	dwc_wr(DEVTEN, 0x7u | (1u << 9) | (1u << 11));
	dwc_wr(DCTL, dwc_rd(DCTL) | DCTL_RUN_STOP);
	for (i = 0; i < 500 && (dwc_rd(DSTS) & DSTS_DEVCTRLHLT); i++) {
		sirius_usb_busy_wait(1000);
	}
	if (dwc_rd(DSTS) & DSTS_DEVCTRLHLT) {
		printf("SIRIUS USB: controller remained halted; not starting EP0 DMA\n");
		return;
	}
	ud.trace_commands = false;
	printf("SIRIUS USB: running (DSTS 0x%08x after %d ms), DMA at 0x%llx\n",
	    dwc_rd(DSTS), i, ud.dma_pa);
	ud.live = true;
	sirius_usb_step = 76;
	sirius_usb_clk = dwc_rd(DSTS);
	sirius_usb_dwc = dwc_rd(DCTL);
	sirius_usb_clk = dwc_rd(GCTL);
	sirius_usb_smmu = dwc_rd(DCTL);
	sirius_usb_pll = dwc_rd(DSTS);
	pur_live = true;

	static const unsigned sample_ms[] = { 0, 1, 10, 100, 500, 1000 };
	uint64_t previous = ~0ULL;
	unsigned elapsed_ms = 0;
	for (unsigned s = 0; s < sizeof(sample_ms) / sizeof(sample_ms[0]); s++) {
		if (sample_ms[s] > elapsed_ms) {
			sirius_usb_busy_wait((sample_ms[s] - elapsed_ms) * 1000);
			elapsed_ms = sample_ms[s];
		}
		ud_poll();
		uint32_t dsts = dwc_rd(DSTS);
		uint32_t dctl = dwc_rd(DCTL);
		uint32_t phycfg = dwc_rd(GUSB2PHYCFG0);
		uint32_t qcfg = usb_rd(qs, QSCRATCH_GENERAL_CFG);
		uint32_t event_count = usb_rd(dwc, GEVNTCOUNT) & 0xffffu;
		uint64_t state = ((uint64_t)dsts << 32) |
		    ((uint64_t)(uint32_t)ud.events << 16) | event_count;
		if (state != previous || s == 0 ||
		    s + 1 == sizeof(sample_ms) / sizeof(sample_ms[0])) {
			printf("SIRIUS USB: STATE t=%ums DSTS=%08x speed=%u link=%u "
			    "DCTL=%08x phy=%08x qcfg=%08x events=%llu/%u\n",
			    elapsed_ms, dsts, dsts & 7u, (dsts >> 18) & 0xfu,
			    dctl, phycfg, qcfg, ud.events, event_count);
		}
		previous = state;
	}
	/* Keep servicing EP0 inline. bsd_init runs synchronously here and, on boots where the
	 * secondaries did not come up, a background thread does not get scheduled at all - the host
	 * then sees a device that never answers its descriptor reads (error -110). Polling for a few
	 * seconds after RUN covers the whole enumeration window. */
	/* Give the host a moment to create the tty and let the capture attach: cdc_acm keeps no
	 * backlog for a port that is not open yet, so a burst sent at configure time is lost. */
	ud.tx_delay_ms = 3000;   /* fallback if the host never raises DTR; counted in the loop */
	/* Measured behaviour: this loop must exit as soon as the ring has been handed to the host.
	 * Long loops (120 s without the break) and a DTR-gated pump both delivered zero bytes, while
	 * this exact shape delivered the 3813-byte boot log on every attempt. Keep the break. */
	for (unsigned ms = 0; ms < 45000; ms++) {
		sirius_usb_busy_wait(1000);
		if (ud.tx_delay_ms > 0) {
			ud.tx_delay_ms--;
		}
		ud_poll();
		if (ud.chars >= pur_ring.total && ud.chars != 0) {
			break;
		}
	}
	if (!ud.hold) {
		thread_t thread;
		kern_return_t result = kernel_thread_start(ud_thread, NULL, &thread);
		if (result == KERN_SUCCESS) {
			thread_deallocate(thread);
			printf("SIRIUS USB: background poller started\n");
		} else {
			printf("SIRIUS USB: background poller failed (%d)\n", result);
		}
	}
}

void
sirius_usb_console_start(void)
{
	int enabled = 0;
	int hold = 0;
	int bind_smmu = 0;
	int power = 0;

	PE_parse_boot_argn("sirius_usb", &enabled, sizeof(enabled));
	PE_parse_boot_argn("sirius_usb_power", &power, sizeof(power));
	PE_parse_boot_argn("sirius_usb_hold", &hold, sizeof(hold));
	PE_parse_boot_argn("sirius_usb_smmu", &bind_smmu, sizeof(bind_smmu));
	ud.hold = hold != 0;
	if (!enabled) {
		return;
	}
	sirius_usb_step = 1;
	vm_offset_t gcc = ml_io_map(GCC_PA, GCC_SIZE);
	if (gcc == 0) {
		printf("SIRIUS USB: GCC map failed\n");
		return;
	}
	sirius_usb_step = 2;
	sirius_usb_gcc_va = gcc;
	uint32_t gdscr = usb_rd(gcc, GCC_USB30_PRIM_GDSCR);
	sirius_usb_gcc = gdscr;
	uint32_t master = usb_rd(gcc, GCC_USB30_PRIM_MASTER);
	uint32_t sleep = usb_rd(gcc, GCC_USB30_PRIM_SLEEP);
	uint32_t mock = usb_rd(gcc, GCC_USB30_PRIM_MOCK_UTMI);
	uint32_t cfg = usb_rd(gcc, GCC_CFG_NOC_USB3_PRIM);
	uint32_t aggre = usb_rd(gcc, GCC_AGGRE_USB3_PRIM);
	uint32_t clkref = usb_rd(gcc, GCC_USB3_PRIM_CLKREF);
	printf("SIRIUS USB: GCC power=%08x master=%08x cfg=%08x aggre=%08x\n",
	    gdscr, master, cfg, aggre);
	printf("SIRIUS USB: GCC sleep=%08x mock_utmi=%08x clkref=%08x\n",
	    sleep, mock, clkref);
	if (gdscr & (1u << 31)) {
		sirius_usb_step = 3;
	}
	if (power == 10) {
		/* Full clock bring-up in the order the experiments established: GDSC, the two AXI branches,
		 * then the USB30 master branch. The DWC3 is only read (GSNPSID, DCTL, DSTS) and the driver
		 * parks, so one boot tells whether the controller answers once every clock is running. */
		if (!(gdscr & (1u << 31))) {
			usb_wr(gcc, GCC_USB30_PRIM_GDSCR, gdscr & ~1u);
			sirius_usb_busy_wait(2000);
		}
		sirius_usb_gcc = usb_rd(gcc, GCC_USB30_PRIM_GDSCR);
		usb_wr(gcc, 0x502c, usb_rd(gcc, 0x502c) | 1u);
		usb_wr(gcc, 0x8201c, usb_rd(gcc, 0x8201c) | 1u);
		for (unsigned ms = 0; ms < 100; ms++) {
			uint32_t a = usb_rd(gcc, 0x502c), b = usb_rd(gcc, 0x8201c);
			if (!(a & (1u << 31)) && !(b & (1u << 31))) { break; }
			sirius_usb_busy_wait(1000);
		}
		usb_wr(gcc, 0xf00c, usb_rd(gcc, 0xf00c) | 1u);
		for (unsigned ms = 0; ms < 100; ms++) {
			if (!(usb_rd(gcc, 0xf00c) & (1u << 31))) { break; }
			sirius_usb_busy_wait(1000);
		}
		sirius_usb_dwc = usb_rd(gcc, 0xf00c);
		vm_offset_t dwc = ml_io_map(DWC3_PA, DWC3_SIZE);
		if (dwc == 0) {
			sirius_usb_step = 55;
			sirius_usb_park(hold);
			return;
		}
		sirius_usb_clk = usb_rd(dwc, 0xc120);   /* GSNPSID */
		sirius_usb_smmu = usb_rd(dwc, DCTL);
		sirius_usb_pll = usb_rd(dwc, DSTS);
		sirius_usb_step = 55;
		sirius_usb_park(hold);
		return;
	}
	if (power == 9) {
		/* Power the GDSC, then bring up only the two AXI branch clocks the survey located
		 * (0x502c cfg_noc, 0x8201c aggre). Master, sleep, mock_utmi and the PHY aux branch stay
		 * untouched, and the DWC3 is not accessed: this step answers whether the AXI branches can
		 * be enabled at all on this SoC. */
		if (!(gdscr & (1u << 31))) {
			usb_wr(gcc, GCC_USB30_PRIM_GDSCR, gdscr & ~1u);
			sirius_usb_busy_wait(2000);
		}
		sirius_usb_gcc = usb_rd(gcc, GCC_USB30_PRIM_GDSCR);
		usb_wr(gcc, 0x502c, usb_rd(gcc, 0x502c) | 1u);
		usb_wr(gcc, 0x8201c, usb_rd(gcc, 0x8201c) | 1u);
		for (unsigned ms = 0; ms < 100; ms++) {
			uint32_t a = usb_rd(gcc, 0x502c), b = usb_rd(gcc, 0x8201c);
			if (!(a & (1u << 31)) && !(b & (1u << 31))) { break; }
			sirius_usb_busy_wait(1000);
		}
		sirius_usb_clk = usb_rd(gcc, 0x502c);
		sirius_usb_smmu = usb_rd(gcc, 0x8201c);
		sirius_usb_dwc = usb_rd(gcc, 0xf00c);
		sirius_usb_step = 54;
		sirius_usb_park(hold);
		return;
	}
	if (power == 8) {
		/* Read-only hunt for the USB AXI branch clocks. SDM845 keeps cfg_noc_usb3_prim_axi_clk at
		 * 0x502c and aggre_usb3_prim_axi_clk at 0x8201c; the offsets this driver inherited from the
		 * SM7250 port (0xf07c/0xf080) read back as "off". Nothing is written here. */
		sirius_usb_clk = usb_rd(gcc, 0x502c);
		sirius_usb_smmu = usb_rd(gcc, 0x8201c);
		sirius_usb_pll = usb_rd(gcc, 0x5028);
		sirius_usb_dwc = usb_rd(gcc, 0xf00c);
		sirius_usb_gcc = usb_rd(gcc, GCC_USB30_PRIM_GDSCR);
		sirius_usb_step = 53;
		sirius_usb_park(hold);
		return;
	}
	if (power == 7) {
		/* GDSC + master branch (both proven safe), then read the DWC3 identification registers.
		 * clk/gsnpsid, smmu/DCTL and pll/DSTS land on the panel row, so one boot tells whether the
		 * core answers after its clock is up. */
		if (!(gdscr & (1u << 31))) {
			usb_wr(gcc, GCC_USB30_PRIM_GDSCR, gdscr & ~1u);
			sirius_usb_busy_wait(2000);
		}
		sirius_usb_gcc = usb_rd(gcc, GCC_USB30_PRIM_GDSCR);
		usb_wr(gcc, 0xf00c, usb_rd(gcc, 0xf00c) | 1u);
		for (unsigned ms = 0; ms < 50; ms++) {
			if (!(usb_rd(gcc, 0xf00c) & (1u << 31))) { break; }
			sirius_usb_busy_wait(1000);
		}
		sirius_usb_dwc = usb_rd(gcc, 0xf00c);
		vm_offset_t dwc = ml_io_map(DWC3_PA, DWC3_SIZE);
		if (dwc == 0) {
			sirius_usb_step = 52;
			sirius_usb_park(hold);
			return;
		}
		sirius_usb_clk = usb_rd(dwc, 0xc120);   /* GSNPSID */
		sirius_usb_smmu = usb_rd(dwc, DCTL);
		sirius_usb_pll = usb_rd(dwc, DSTS);
		sirius_usb_step = 52;
		sirius_usb_park(hold);
		return;
	}
	if (power == 6) {
		/* Single-step branch enable: only gcc_usb30_prim_master_clk (0xf00c). The read-only survey
		 * showed its RCG (0xf01c = 0x00100105) already selects GPLL0 with the 240 MHz divider, so
		 * the source is up and the branch is the only missing piece. Nothing past the branch is
		 * touched here: the halt bit is reported and the driver parks. */
		if (!(gdscr & (1u << 31))) {
			usb_wr(gcc, GCC_USB30_PRIM_GDSCR, gdscr & ~1u);
			sirius_usb_busy_wait(2000);
		}
		sirius_usb_gcc = usb_rd(gcc, GCC_USB30_PRIM_GDSCR);
		usb_wr(gcc, 0xf00c, usb_rd(gcc, 0xf00c) | 1u);
		for (unsigned ms = 0; ms < 50; ms++) {
			if (!(usb_rd(gcc, 0xf00c) & (1u << 31))) { break; }
			sirius_usb_busy_wait(1000);
		}
		sirius_usb_dwc = usb_rd(gcc, 0xf00c);
		sirius_usb_step = 51;
		sirius_usb_park(hold);
		return;
	}
	if (power == 5) {
		/* Read-only survey of the USB clock tree, because writes past the GDSC stall this SoC's bus
		 * when the sources are not running. Bit 0 of 0x52000 is GPLL0's enable; 0xf018 is the
		 * USB30_PRIM_MASTER_CLK_SRC command register and 0xf01c its config (source select and
		 * divider); 0xf00c is the master branch, whose bit 31 means "clock off". */
		sirius_usb_pll = usb_rd(gcc, 0x52000);
		sirius_usb_clk = usb_rd(gcc, 0xf018);
		sirius_usb_smmu = usb_rd(gcc, 0xf01c);
		sirius_usb_dwc = usb_rd(gcc, 0xf00c);
		sirius_usb_step = 50;
		sirius_usb_park(hold);
		return;
	}
	/* USB clock bring-up, in the order hardware testing established: the GDSC power domain, the
	 * two AXI branches (cfg_noc at 0x502c, aggre at 0x8201c), the USB30 master branch, then the
	 * sleep and mock_utmi branches the UTMI interface needs. Every branch is halt-checked: if bit 31
	 * never clears its source is not running, and touching the DWC3 without its AXI clock stalls the
	 * NoC and reboots the phone (observed twice). */
	if (!(gdscr & (1u << 31))) {
		usb_wr(gcc, GCC_USB30_PRIM_GDSCR, gdscr & ~1u);
		sirius_usb_busy_wait(2000);
		gdscr = usb_rd(gcc, GCC_USB30_PRIM_GDSCR);
		sirius_usb_gcc = gdscr;
	}
	{
		static const uint32_t usb_branches[] = {0x502c, 0x8201c, 0xf00c, 0xf010, 0xf014};
		const unsigned count = sizeof(usb_branches) / sizeof(usb_branches[0]);
		uint32_t halted = 0;
		for (unsigned i = 0; i < count; i++) {
			usb_wr(gcc, usb_branches[i], usb_rd(gcc, usb_branches[i]) | 1u);
		}
		for (unsigned ms = 0; ms < 200; ms++) {
			halted = 0;
			for (unsigned i = 0; i < count; i++) {
				if (usb_rd(gcc, usb_branches[i]) & (1u << 31)) { halted |= 1u << i; }
			}
			if (halted == 0) { break; }
			sirius_usb_busy_wait(1000);
		}
		sirius_usb_clk = halted;
		if (halted != 0) {
			printf("SIRIUS USB: branch clocks halted mask=%x; controller untouched\n", halted);
			sirius_usb_park(hold);
			return;
		}
	}
	if (!(gdscr & (1u << 31))) {
		printf("SIRIUS USB: GDSC did not power on; controller untouched\n");
		sirius_usb_park(hold);
		return;
	}
	vm_size_t smmu_size = bind_smmu == 2 ? PAGE_SIZE : SMMU_SIZE;
	vm_offset_t smmu = ml_io_map(SMMU_PA, smmu_size);
	if (smmu == 0) {
		printf("SIRIUS USB: SMMU map failed\n");
		sirius_usb_park(hold);
		return;
	}
	sirius_usb_step = 4;
	printf("SIRIUS USB: SMMU mapped %u bytes\n", (unsigned)smmu_size);
	if (bind_smmu == 3) {
		ud.smmu_guard = sirius_usb_smmu_probe_page(smmu);
		printf("SIRIUS USB: SMMU probe pre-DWC full-map %s\n",
		    ud.smmu_guard ? "PASS" : "FAIL");
		sirius_usb_park(hold);
		return;
	}
	vm_offset_t dwc = ml_io_map(DWC3_PA, DWC3_SIZE);
	if (dwc == 0) {
		printf("SIRIUS USB: DWC3 map failed\n");
		sirius_usb_park(hold);
		return;
	}
	ud.dwc = dwc;
	sirius_usb_step = 6;
	sirius_usb_dwc = usb_rd(dwc, 0xc120);
	sirius_usb_dsts = usb_rd(dwc, DSTS);
	printf("SIRIUS USB: DWC3 GSNPSID 0x%08x GCTL 0x%08x DCTL 0x%08x DSTS 0x%08x DCFG 0x%08x\n",
	    usb_rd(dwc, 0xc120), usb_rd(dwc, GCTL), usb_rd(dwc, DCTL),
	    usb_rd(dwc, DSTS), usb_rd(dwc, DCFG));
	if (bind_smmu == 7) {
		vm_offset_t qs = dwc + QSCRATCH_OFFSET;
		printf("SIRIUS USB: QSCRATCH read-only probe pa=0x%x va=%p\n",
		    QSCRATCH_PA, (void *)qs);
		sirius_usb_qscratch_probe(qs);
		sirius_usb_park(hold);
		return;
	}
	if (bind_smmu == 4) {
		ud.smmu_guard = sirius_usb_smmu_probe_page(smmu);
		printf("SIRIUS USB: SMMU probe post-DWC full-map %s\n",
		    ud.smmu_guard ? "PASS" : "FAIL");
		sirius_usb_park(hold);
		return;
	}
	if (bind_smmu == 2) {
		ud.smmu_guard = sirius_usb_smmu_probe_page(smmu);
		printf("SIRIUS USB: SMMU probe one-page %s\n",
		    ud.smmu_guard ? "PASS" : "FAIL");
		sirius_usb_park(hold);
		return;
	}
	if (bind_smmu) {
		struct sirius_usb_smmu_bind plan;
		bool plan_only = bind_smmu == 5;
		bool verbose_plan = bind_smmu == 5 || bind_smmu == 6;
		if (!sirius_usb_smmu_plan_bind(smmu, &plan, verbose_plan)) {
			printf("SIRIUS USB: SMMU bind plan FAIL\n");
			sirius_usb_park(hold);
			return;
		}
		printf("SIRIUS USB: SMMU bind plan PASS slot=%u bank=%u\n",
		    plan.slot, plan.bank);
		if (plan_only) {
			sirius_usb_park(hold);
			return;
		}
		printf("SIRIUS USB: quiescing DWC3 before SMMU bind\n");
		dwc_wr(DCTL, dwc_rd(DCTL) & ~DCTL_RUN_STOP);
		unsigned wait;
		for (wait = 0; wait < 500 &&
		    !(dwc_rd(DSTS) & DSTS_DEVCTRLHLT); wait++) {
			sirius_usb_busy_wait(1000);
		}
		printf("SIRIUS USB: DWC3 halt %s after %u ms DSTS=%08x\n",
		    (dwc_rd(DSTS) & DSTS_DEVCTRLHLT) ? "done" : "TIMEOUT",
		    wait, dwc_rd(DSTS));
		if (!(dwc_rd(DSTS) & DSTS_DEVCTRLHLT) ||
		    !sirius_usb_smmu_apply_bind(smmu, &plan)) {
			printf("SIRIUS USB: SMMU bind refused; controller untouched\n");
			sirius_usb_park(hold);
			return;
		}
	}
	ud.smmu_guard = sirius_usb_smmu_guard(smmu, 0, 0);
	sirius_usb_smmu = ud.smmu_guard ? 1u : 0u;
	if (ud.smmu_guard) {
		sirius_usb_step = 5;
	}
	if (!ud.smmu_guard) {
		printf("SIRIUS USB: SID e0 guard refused; controller untouched\n");
		sirius_usb_park(hold);
		return;
	}
	if (enabled >= 2) {
		sirius_usb_step = 7;
		sirius_usb_gadget_start(dwc, smmu);
	} else {
		printf("SIRIUS USB: route test complete; DWC3 remains halted\n");
	}
	sirius_usb_park(hold);
}
