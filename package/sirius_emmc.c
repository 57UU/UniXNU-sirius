/*
 * Sirius eMMC bring-up: the SDCC1 controller at 0x7c4000 as an SDHCI host.
 *
 * The board stores its boot images on eMMC, not UFS, so the boot chain has already identified the
 * card and left SDCC1 programmed: the card is in the transfer state and the host controller has a
 * card clock, a bus width and power. That makes the first milestone cheap - read registers, then
 * read a block over PIO, which needs no DMA and no SMMU programming at all.
 *
 * Register layout: SD Host Controller Simplified Specification 3.0. Values are accessed with the
 * width the specification gives each register (8, 16 or 32 bits).
 */

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;
typedef unsigned long vm_offset_t;
typedef unsigned long long uint64_t;
typedef _Bool bool;
#define true 1
#define false 0

extern vm_offset_t ml_io_map(vm_offset_t phys, unsigned long size);
/* A spinning delay on purpose: delay() sleeps, and while the CPU is idle the GCC auto clock gating
 * turns the SDCC AHB clock off, after which every register reads back zero and no command ever
 * completes. */
static void
spin_us(unsigned usec)
{
	volatile unsigned ticks = usec * 100u;
	while (ticks > 0) {
		ticks--;
	}
}
extern void sirius_usb_log_char(char c);

/* The kernel console stops feeding the ring long before bsd_init prints anything, so a report that
 * only goes through printf never reaches the host. Writing the characters into the USB ring
 * directly makes the probe result part of the snapshot the host takes when it attaches. */
static void
ring_puts(const char *text)
{
	while (*text != '\0') {
		sirius_usb_log_char(*text++);
	}
}

static void
ring_hex(uint32_t value, unsigned digits)
{
	char buffer[9];
	static const char hex[] = "0123456789abcdef";
	if (digits > 8) {
		digits = 8;
	}
	for (unsigned i = 0; i < digits; i++) {
		buffer[digits - 1 - i] = hex[(value >> (4 * i)) & 0xfu];
	}
	buffer[digits] = '\0';
	ring_puts(buffer);
}

static void
ring_dec(uint32_t value)
{
	char buffer[11];
	unsigned i = 0;
	if (value == 0) {
		ring_puts("0");
		return;
	}
	while (value != 0) {
		buffer[i++] = (char)('0' + (value % 10));
		value /= 10;
	}
	while (i > 0) {
		sirius_usb_log_char(buffer[--i]);
	}
}

#define GCC_PA 0x00100000u
#define GCC_SIZE 0x00090000u

#define SDCC1_PA 0x007c4000u
#define SDCC1_SIZE 0x1000u

/* GCC branches that clock the controller. Offsets from gcc-sdm845.c, same block layout as sdm670. */
#define GCC_SDCC1_APPS_CBCR 0x26004u
#define GCC_SDCC1_AHB_CBCR 0x26008u
#define GCC_SDCC1_ICE_CBCR 0x2600cu
#define GCC_AGGRE_UFS_PHY_AXI_CBCR 0x82024u

/* SDCC1 apps RCG (cmd_rcgr 0x26028): CFG at +4 reports the selected source, the D/M/N counters
 * live at +8/+12/+16. The card clock on Qualcomm comes from here, not from the SDHCI divider. */
#define GCC_SDCC1_APPS_CMD 0x26028u
#define GCC_SDCC1_APPS_CFG 0x2602cu
#define GCC_SDCC1_APPS_M 0x26030u
#define GCC_SDCC1_APPS_N 0x26034u
#define GCC_SDCC1_APPS_D 0x26038u

/* Qualcomm SDHCI vendor registers (sdhci-msm.c): CORE_HC_MCLK_SEL and the pad power switch. */
/* sdhci-msm v5 vendor offsets (sdhci_msm_v5_offset). The 0x10c used earlier belongs to the older
 * MCI layout, which is why it read back zero. */
#define MSM_CORE_MCI_VERSION 0x318u
#define MSM_CORE_DLL_CONFIG 0x200u
#define MSM_CORE_DLL_STATUS 0x208u
#define MSM_CORE_VENDOR_SPEC 0x20cu
#define MSM_CORE_PWRCTL_STATUS 0x240u
#define MSM_CORE_VENDOR_SPEC3 0x250u
#define MSM_CORE_DLL_CONFIG_2 0x254u
#define MSM_CORE_DLL_USR_CTL 0x388u

/* SDHCI registers, byte offsets within the 4 KiB "hc" window. */
#define SDHCI_BLOCK_SIZE 0x04u
#define SDHCI_BLOCK_COUNT 0x06u
#define SDHCI_ARGUMENT 0x08u
#define SDHCI_TRANSFER_MODE 0x0cu
#define SDHCI_COMMAND 0x0eu
#define SDHCI_RESPONSE 0x10u
#define SDHCI_BUFFER 0x20u
#define SDHCI_PRESENT_STATE 0x24u
#define SDHCI_POWER_CONTROL 0x29u
#define SDHCI_HOST_CONTROL 0x28u
#define SDHCI_TIMEOUT_CONTROL 0x2eu
#define SDHCI_HOST_CONTROL2 0x3eu
#define SDHCI_DMA_ADDRESS 0x00u
#define SDHCI_CLOCK_CONTROL 0x2cu
#define SDHCI_SOFTWARE_RESET 0x2fu
#define SDHCI_INT_ENABLE 0x34u
#define SDHCI_INT_STATUS 0x30u
#define SDHCI_ERR_INT_ENABLE 0x3cu
#define SDHCI_ERR_INT_STATUS 0x38u
#define SDHCI_CAPABILITIES 0x40u
#define SDHCI_VERSION 0xfeu

#define SDHCI_RESET_ALL 0x01u
#define SDHCI_RESET_CMD 0x02u
#define SDHCI_RESET_DATA 0x04u

/* Present state bits used below. */
#define SDHCI_PS_CMD_INHIBIT (1u << 0)
#define SDHCI_PS_DAT_INHIBIT (1u << 1)
#define SDHCI_PS_CARD_INSERTED (1u << 16)
#define SDHCI_PS_BUFFER_READ_ENABLE (1u << 11)

/* Interrupt status bits. */
#define SDHCI_INT_CMD_COMPLETE (1u << 0)
#define SDHCI_INT_TRANSFER_COMPLETE (1u << 1)
#define SDHCI_INT_BUFFER_READ_READY (1u << 5)
#define SDHCI_INT_ERROR (1u << 15)

/* Command register fields. */
#define SDHCI_CMD_RESP_NONE (0u << 0)
#define SDHCI_CMD_RESP_136 (1u << 0)
#define SDHCI_CMD_RESP_48 (2u << 0)
#define SDHCI_CMD_RESP_48_BUSY (3u << 0)
#define SDHCI_CMD_CRC_CHECK (1u << 3)
#define SDHCI_CMD_INDEX_CHECK (1u << 4)
#define SDHCI_CMD_DATA_PRESENT (1u << 5)

/* Transfer mode fields. */
#define SDHCI_TM_READ (1u << 4)
#define SDHCI_TM_BLOCK_COUNT_ENABLE (1u << 1)

/* Clock control bits. */
#define SDHCI_CLK_INTERNAL_EN (1u << 0)
#define SDHCI_CLK_INTERNAL_STABLE (1u << 1)
#define SDHCI_CLK_SD_EN (1u << 2)

volatile uint32_t sirius_emmc_fail_int;
volatile uint32_t sirius_emmc_fail_err;
volatile uint32_t sirius_emmc_fail_state;
volatile uint32_t sirius_emmc_fail_resp;
volatile uint32_t sirius_emmc_fail_reset;
volatile uint32_t sirius_emmc_after_cmd_ver;
volatile uint32_t sirius_emmc_after_cmd_clk;
volatile uint32_t sirius_emmc_before_cmd_state;
volatile uint32_t sirius_emmc_dead_ver;
volatile uint32_t sirius_emmc_dead_clk;
volatile uint32_t sirius_emmc_dead_pwr;
volatile uint32_t sirius_emmc_dead_halts;
volatile uint32_t sirius_emmc_fail_clk;

static vm_offset_t emmc_base;
static vm_offset_t gcc_base;

static uint8_t
sdhci_read8(uint32_t offset)
{
	return *(volatile uint8_t *)(emmc_base + offset);
}

static uint16_t
sdhci_read16(uint32_t offset)
{
	return *(volatile uint16_t *)(emmc_base + offset);
}

static uint32_t
sdhci_read32(uint32_t offset)
{
	return *(volatile uint32_t *)(emmc_base + offset);
}

static void
sdhci_write8(uint32_t offset, uint8_t value)
{
	*(volatile uint8_t *)(emmc_base + offset) = value;
}

static void
sdhci_write16(uint32_t offset, uint16_t value)
{
	*(volatile uint16_t *)(emmc_base + offset) = value;
}

static void
sdhci_write32(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(emmc_base + offset) = value;
}

static uint32_t
gcc_read(uint32_t offset)
{
	if (gcc_base == 0) {
		return 0xffffffffu;
	}
	return *(volatile uint32_t *)(gcc_base + offset);
}

/* Command and PIO helpers. Polling, no DMA: the first milestone only has to prove the card answers
 * and that a block can be read, and polling keeps the SMMU and the ADMA engine out of the picture. */
static int
sdhci_wait(uint32_t mask, unsigned timeout_us)
{
	for (unsigned waited = 0; waited < timeout_us; waited += 10) {
		/* Only the requested completion bits are waited for. The error summary bit in INT_STATUS
		 * latches spuriously on this controller (the error status register reads all ones), and
		 * bailing out on it aborted commands that were still on their way to completing. */
		uint16_t status = sdhci_read16(SDHCI_INT_STATUS);
		if ((status & mask) != 0) {
			return (int)(status & mask);
		}
		spin_us(10);
	}
	return 0;
}

static int
sdhci_command_capture(uint16_t command, uint32_t argument, uint32_t *response)
{
	unsigned waited = 0;
	while ((sdhci_read32(SDHCI_PRESENT_STATE) & (SDHCI_PS_CMD_INHIBIT | SDHCI_PS_DAT_INHIBIT)) != 0) {
		if (waited > 50000) {
			/* Something left the command line wedged (a previous command that never
			 * completed). Reset the command and data circuits, as the kernel does on timeout,
			 * and carry on - otherwise every later command is refused as well. */
			sdhci_write8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
			for (unsigned spin = 0; spin < 10000; spin++) {
				if ((sdhci_read8(SDHCI_SOFTWARE_RESET) & (SDHCI_RESET_CMD | SDHCI_RESET_DATA)) == 0) {
					break;
				}
				spin_us(10);
			}
			break;
		}
		spin_us(10);
		waited += 10;
	}
	sdhci_write16(SDHCI_INT_STATUS, 0xffffu);
	sdhci_write16(SDHCI_ERR_INT_STATUS, 0xffffu);
	sdhci_write32(SDHCI_ARGUMENT, argument);
	sdhci_write16(SDHCI_COMMAND, command);
	/* A 136-bit identification response may take longer than the 100 ms used for short commands:
	 * wait a full second before declaring the command lost. */
	unsigned timeout = (command & 0x3u) == SDHCI_CMD_RESP_136 ? 1000000u : 100000u;
	if (sdhci_wait(SDHCI_INT_CMD_COMPLETE, timeout) <= 0) {
		return -1;
	}
	if (response != 0) {
		for (unsigned i = 0; i < 4; i++) {
			response[i] = sdhci_read32(SDHCI_RESPONSE + 4u * i);
		}
	}
	sdhci_write16(SDHCI_INT_STATUS, SDHCI_INT_CMD_COMPLETE);
	return 0;
}

static int
sdhci_command(uint16_t command, uint32_t argument)
{
	/* Nothing may be in flight: a command left over from an earlier attempt would otherwise be
	 * reported as ours. */
	for (unsigned waited = 0; waited < 100000; waited += 10) {
		if ((sdhci_read32(SDHCI_PRESENT_STATE) & (SDHCI_PS_CMD_INHIBIT | SDHCI_PS_DAT_INHIBIT)) == 0) {
			break;
		}
		spin_us(10);
	}
	sdhci_write16(SDHCI_INT_STATUS, 0xffffu);
	sdhci_write32(SDHCI_ARGUMENT, argument);
	sirius_emmc_before_cmd_state = sdhci_read32(SDHCI_PRESENT_STATE);
	sdhci_write16(SDHCI_COMMAND, command);
	/* Read two registers that must not change - the version is a constant and the clock control
	 * was just programmed - to tell a dead register window from a rejected command. */
	sirius_emmc_after_cmd_ver = sdhci_read16(SDHCI_VERSION);
	sirius_emmc_after_cmd_clk = sdhci_read16(SDHCI_CLOCK_CONTROL);
	if (sdhci_wait(SDHCI_INT_CMD_COMPLETE, 100000) <= 0) {
		sirius_emmc_fail_int = sdhci_read16(SDHCI_INT_STATUS);
		sirius_emmc_fail_err = sdhci_read16(SDHCI_ERR_INT_STATUS);
		sirius_emmc_fail_state = sdhci_read32(SDHCI_PRESENT_STATE);
		sirius_emmc_fail_resp = sdhci_read32(SDHCI_RESPONSE);
		sirius_emmc_fail_reset = sdhci_read8(SDHCI_SOFTWARE_RESET);
		sirius_emmc_fail_clk = sdhci_read16(SDHCI_CLOCK_CONTROL);
		sirius_emmc_dead_ver = sdhci_read16(SDHCI_VERSION);
		sirius_emmc_dead_clk = sdhci_read16(SDHCI_CLOCK_CONTROL);
		sirius_emmc_dead_pwr = sdhci_read8(SDHCI_POWER_CONTROL);
		sirius_emmc_dead_halts = ((gcc_read(GCC_SDCC1_APPS_CBCR) >> 31) & 1u) |
		    (((gcc_read(GCC_SDCC1_AHB_CBCR) >> 31) & 1u) << 1) |
		    (((gcc_read(GCC_SDCC1_ICE_CBCR) >> 31) & 1u) << 2) |
		    (((gcc_read(GCC_AGGRE_UFS_PHY_AXI_CBCR) >> 31) & 1u) << 3);
		return -1;
	}
	sdhci_write16(SDHCI_INT_STATUS, SDHCI_INT_CMD_COMPLETE);
	return 0;
}

/* The SDCC1 root clock was left in MND mode with empty counters (m=0 n=0 d=0xff), so the card
 * clock was meaningless and every command timed out. Program 400 kHz the way the Linux table does
 * for identification -BI_TCXO (19.2 MHz) / 12 * 1/4- by writing the MND registers and pulsing the
 * update bit. */
static int
emmc_set_card_clock_400k(void)
{
	if (gcc_base == 0) {
		return -1;
	}
	*(volatile uint32_t *)(gcc_base + GCC_SDCC1_APPS_CMD) = 0;      /* root disabled while editing */
	spin_us(10);
	/* src_sel = 0 (bi_tcxo), mode = MND (0), divider field unused in MND mode */
	*(volatile uint32_t *)(gcc_base + GCC_SDCC1_APPS_CFG) = 0u;
	*(volatile uint32_t *)(gcc_base + GCC_SDCC1_APPS_M) = 1u;
	*(volatile uint32_t *)(gcc_base + GCC_SDCC1_APPS_N) = 4u;
	*(volatile uint32_t *)(gcc_base + GCC_SDCC1_APPS_D) = 12u;
	spin_us(10);
	*(volatile uint32_t *)(gcc_base + GCC_SDCC1_APPS_CMD) = 1u;     /* update */
	for (unsigned waited = 0; waited < 100000; waited += 100) {
		if ((gcc_read(GCC_SDCC1_APPS_CMD) & 1u) == 0) {
			break;
		}
		spin_us(100);
	}
	/* Make sure the branch is on; the controller only sees the clock when both are set. */
	*(volatile uint32_t *)(gcc_base + GCC_SDCC1_APPS_CBCR) = gcc_read(GCC_SDCC1_APPS_CBCR) | 1u;
	spin_us(100);
	return (gcc_read(GCC_SDCC1_APPS_CMD) & 1u) == 0 ? 0 : -1;
}

/* The boot chain leaves SDCC1 clocked, but the branches get gated again once it goes idle: the
 * probe read clock=0x0407 and the block read found clock=0x0000, so every command was dropped for
 * want of a card clock. Re-enable the GCC branches and program the card clock before a transfer. */
static int
emmc_clock_on(uint16_t previous)
{
	static const uint32_t branches[] = {
		GCC_SDCC1_APPS_CBCR, GCC_SDCC1_AHB_CBCR, GCC_SDCC1_ICE_CBCR, GCC_AGGRE_UFS_PHY_AXI_CBCR
	};
	if (gcc_base == 0) {
		return -1;
	}
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		if ((gcc_read(branches[i]) & (1u << 31)) != 0) {
			*(volatile uint32_t *)(gcc_base + branches[i]) = gcc_read(branches[i]) | 1u;
		}
	}
	for (unsigned waited = 0; waited < 100000; waited += 100) {
		if ((sdhci_read16(SDHCI_CLOCK_CONTROL) & (SDHCI_CLK_INTERNAL_EN | SDHCI_CLK_SD_EN)) ==
		    (SDHCI_CLK_INTERNAL_EN | SDHCI_CLK_SD_EN)) {
			return 0;
		}
		spin_us(100);
	}
	/* Program it again: enable the internal clock, wait for it to stabilise, then the card clock.
	 * The frequency selector keeps the value the boot chain used. */
	sdhci_write16(SDHCI_CLOCK_CONTROL, (uint16_t)((previous & 0xff00u) | SDHCI_CLK_INTERNAL_EN));
	for (unsigned waited = 0; waited < 100000; waited += 100) {
		if ((sdhci_read16(SDHCI_CLOCK_CONTROL) & SDHCI_CLK_INTERNAL_STABLE) != 0) {
			break;
		}
		spin_us(100);
	}
	uint16_t value = sdhci_read16(SDHCI_CLOCK_CONTROL);
	sdhci_write16(SDHCI_CLOCK_CONTROL, (uint16_t)(value | SDHCI_CLK_SD_EN));
	spin_us(1000);
	return (sdhci_read16(SDHCI_CLOCK_CONTROL) & SDHCI_CLK_SD_EN) != 0 ? 0 : -1;
}

static void
ring_fail(const char *stage)
{
	ring_puts("EMMC ");
	ring_puts(stage);
	ring_puts(" FAIL int=");
	ring_hex(sdhci_read16(SDHCI_INT_STATUS), 4);
	ring_puts(" err=");
	ring_hex(sdhci_read16(SDHCI_ERR_INT_STATUS), 4);
	ring_puts(" st=");
	ring_hex(sdhci_read32(SDHCI_PRESENT_STATE), 8);
	ring_puts("\r\n");
}

extern vm_offset_t kvtophys(vm_offset_t va);

#define SDHCI_TM_DMA (1u << 0)

/* SDHCI ignores writes to the transfer mode register while the command line is inhibited, so a
 * single wedged command silently poisons every transfer that follows. Clear the line first. */
static int
emmc_clear_command_line(void)
{
	if ((sdhci_read32(SDHCI_PRESENT_STATE) & (SDHCI_PS_CMD_INHIBIT | SDHCI_PS_DAT_INHIBIT)) == 0) {
		return 0;
	}
	sdhci_write8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
	for (unsigned waited = 0; waited < 20000; waited += 10) {
		if ((sdhci_read8(SDHCI_SOFTWARE_RESET) & (SDHCI_RESET_CMD | SDHCI_RESET_DATA)) == 0) {
			return 0;
		}
		spin_us(10);
	}
	return -1;
}

/* A page-aligned static buffer is physically contiguous, which is all SDMA needs. */
static uint8_t sdma_buffer[512] __attribute__((aligned(4096)));

/* The eMMC controller's DMA goes through the apps SMMU with stream id 0x140; without a matching
 * stream mapping the data engine faults and the transfer never completes (DAT line stays active
 * forever, no transfer-complete event). Install an exact-match bypass entry the way the boot chain
 * does for the storage stream. */
#define SMMU_PA 0x15000000u
#define SMMU_SIZE 0x000d0000u
#define EMMC_STREAM_ID 0x0140u

/* Read-only: which stream mapping does the boot chain leave for the storage stream id? Writing
 * SMR/S2CR while the SMMU is live is not safe from here, and a wrong write hangs the SoC. */
static uint32_t emmc_smmu_report;
static uint32_t emmc_smmu_fixed;
static vm_offset_t emmc_smmu_base;
volatile uint32_t sirius_emmc_gfsr;

static int
emmc_smmu_probe(void)
{
	vm_offset_t smmu = ml_io_map(SMMU_PA, SMMU_SIZE);
	if (smmu == 0) {
		return -1;
	}
	emmc_smmu_base = smmu;
	/* GFSR records global faults (0x48); clear it so anything found later belongs to our transfer. */
	*(volatile uint32_t *)(smmu + 0x048u) = *(volatile uint32_t *)(smmu + 0x048u);
	uint32_t idr0 = *(volatile uint32_t *)(smmu + 0x020u);
	unsigned count = idr0 & 0xffu;
	if (count == 0 || count > 128) {
		return -1;
	}
	for (unsigned i = 0; i < count; i++) {
		uint32_t smr = *(volatile uint32_t *)(smmu + 0x800u + 4u * i);
		if ((smr & (1u << 31)) == 0) {
			continue;
		}
		uint32_t id = smr & 0xffffu;
		if (id != EMMC_STREAM_ID) {
			continue;
		}
		uint32_t s2cr = *(volatile uint32_t *)(smmu + 0xc00u + 4u * i);
		emmc_smmu_report = ((uint32_t)(i & 0xffu) << 16) | ((s2cr & 0xffu) << 8) |
		    ((s2cr >> 16) & 3u);
		/* The stream is already matched and valid, only its action is wrong (abort). Flip that
		 * one field to bypass: no SMR write and no new slot, which is the smallest change that
		 * makes the storage DMA legal. Nothing of this stream is in flight - the command line is
		 * idle and no transfer is armed at this point. */
		if (((s2cr >> 16) & 3u) == 0) {
			*(volatile uint32_t *)(smmu + 0xc00u + 4u * i) = 1u;
			__asm__ volatile ("dsb sy" ::: "memory");
			emmc_smmu_fixed = (*(volatile uint32_t *)(smmu + 0xc00u + 4u * i) == 1u) ? 1u : 0u;
		}
		return 0;
	}
	emmc_smmu_report = 0xffffffffu;   /* no mapping for 0x140 at all */
	return -1;
}

/* ADMA2 with 64-bit addressing is the mode the boot chain uses, and the core may not implement
 * plain SDMA at all. One descriptor covers the whole 512-byte block: TRAN|VALID|END, length, and
 * the 64-bit address of the buffer. The table has to be 64-byte aligned. */
#define ADMA2_TRAN_VALID 0x21u
#define ADMA2_END 0x02u

static uint8_t adma_desc[128] __attribute__((aligned(128)));

/* Everything a data command needs before the transfer is armed. U-Boot does the same two things
 * for every data command: force the data timeout control to 0x0e and set the block length. */
static void
emmc_prepare_transfer(void)
{
	if (sdhci_read8(SDHCI_TIMEOUT_CONTROL) != 0x0eu) {
		sdhci_write8(SDHCI_TIMEOUT_CONTROL, 0x0eu);
	}
	uint32_t status[1] = { 0 };
	(void)sdhci_command_capture((uint16_t)((16u << 8) | SDHCI_CMD_RESP_48 |
	    SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK), 512u, status);
}

static int
sdhci_read_block_adma2(uint32_t lba, uint8_t *out)
{
	vm_offset_t buf_pa = kvtophys((vm_offset_t)sdma_buffer);
	vm_offset_t desc_pa = kvtophys((vm_offset_t)adma_desc);
	if (buf_pa == 0 || desc_pa == 0) {
		return -1;
	}
	if (emmc_clear_command_line() != 0) {
		return -1;
	}
	ring_puts("EMMC timeout-in=");
	ring_hex(sdhci_read8(SDHCI_TIMEOUT_CONTROL), 2);
	ring_puts("\r\n");
	emmc_prepare_transfer();
	ring_puts("EMMC timeout-set=");
	ring_hex(sdhci_read8(SDHCI_TIMEOUT_CONTROL), 2);
	ring_puts("\r\n");
	for (unsigned i = 0; i < 512; i++) {
		sdma_buffer[i] = 0;
	}
	for (unsigned i = 0; i < sizeof(adma_desc); i++) {
		adma_desc[i] = 0;
	}
	uint16_t cmd = (uint16_t)(ADMA2_TRAN_VALID | ADMA2_END);
	uint16_t len = 512;
	*(volatile uint16_t *)(adma_desc + 0) = cmd;
	*(volatile uint16_t *)(adma_desc + 2) = len;
	*(volatile uint32_t *)(adma_desc + 4) = (uint32_t)buf_pa;
	*(volatile uint32_t *)(adma_desc + 8) = 0;
	__asm__ volatile ("dsb sy" ::: "memory");

	/* ADMA2 64-bit addressing: host control DMA select bits[4:3] = 0b11. */
	sdhci_write8(SDHCI_HOST_CONTROL, (uint8_t)((sdhci_read8(SDHCI_HOST_CONTROL) & 0x27u) | 0x18u));
	sdhci_write32(SDHCI_DMA_ADDRESS, (uint32_t)desc_pa);
	sdhci_write16(SDHCI_BLOCK_SIZE, 512u);
	sdhci_write16(SDHCI_BLOCK_COUNT, 1u);
	sdhci_write16(SDHCI_TRANSFER_MODE, SDHCI_TM_READ | SDHCI_TM_BLOCK_COUNT_ENABLE | SDHCI_TM_DMA);
	if ((sdhci_read16(SDHCI_TRANSFER_MODE) & (SDHCI_TM_READ | SDHCI_TM_DMA)) !=
	    (SDHCI_TM_READ | SDHCI_TM_DMA)) {
		return -1;
	}
	if (sdhci_command((uint16_t)((17u << 8) | SDHCI_CMD_DATA_PRESENT | SDHCI_CMD_RESP_48 |
	    SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK), lba) != 0) {
		return -1;
	}
	/* Sample the four DAT line levels while the clock runs: if they never change the card is not
	 * driving data at all, which is a pad/hardware problem rather than a sampling one. */
	uint32_t seen = 0;
	unsigned transitions = 0;
	uint32_t previous = (sdhci_read32(SDHCI_PRESENT_STATE) >> 20) & 0xfu;
	for (unsigned i = 0; i < 2000; i++) {
		uint32_t now = (sdhci_read32(SDHCI_PRESENT_STATE) >> 20) & 0xfu;
		seen |= 1u << now;
		if (now != previous) {
			transitions++;
			previous = now;
		}
		if ((sdhci_read16(SDHCI_INT_STATUS) & SDHCI_INT_TRANSFER_COMPLETE) != 0) {
			break;
		}
		spin_us(50);
	}
	/* The card cannot send anything without the card clock, so record whether the controller
	 * keeps it running while the data phase is pending. */
	uint16_t clock_during = sdhci_read16(SDHCI_CLOCK_CONTROL);
	uint32_t gcc_during = gcc_read(GCC_SDCC1_APPS_CBCR);
	ring_puts("EMMC clock=");
	ring_hex(sdhci_read16(SDHCI_CLOCK_CONTROL), 4);
	ring_puts("\r\n");
	ring_puts("EMMC clk-during=");
	ring_hex(clock_during, 4);
	ring_puts(" gcc=");
	ring_hex(gcc_during, 8);
	ring_puts("\r\n");
	ring_puts("EMMC dat seen=");
	ring_hex(seen, 4);
	ring_puts(" trans=");
	ring_dec(transitions);
	ring_puts("\r\n");
	if (sdhci_wait(SDHCI_INT_TRANSFER_COMPLETE, 2000000) <= 0) {
		ring_puts("EMMC adma2 no TC int=");
		ring_hex(sdhci_read16(SDHCI_INT_STATUS), 4);
		ring_puts(" err=");
		ring_hex(sdhci_read16(SDHCI_ERR_INT_STATUS), 4);
		ring_puts("\r\n");
		return -1;
	}
	sdhci_write16(SDHCI_INT_STATUS, 0xffffu);
	for (unsigned i = 0; i < 512; i++) {
		out[i] = sdma_buffer[i];
	}
	return 0;
}

/* Single-block read over SDMA: the boot chain left the controller in a DMA mode where the buffer
 * port only returns one stale word, so the block is fetched by the data engine into memory
 * instead of through the FIFO. */
static int
sdhci_read_block_sdma(uint32_t lba, uint8_t *out)
{
	vm_offset_t pa = kvtophys((vm_offset_t)sdma_buffer);
	if (pa == 0) {
		return -1;
	}
	for (unsigned i = 0; i < 512; i++) {
		sdma_buffer[i] = 0;
	}
	if (emmc_clear_command_line() != 0) {
		return -1;
	}
	emmc_smmu_probe();
	sdhci_write32(SDHCI_DMA_ADDRESS, (uint32_t)pa);
	sdhci_write16(SDHCI_BLOCK_SIZE, 512u);
	sdhci_write16(SDHCI_BLOCK_COUNT, 1u);
	sdhci_write16(SDHCI_TRANSFER_MODE, SDHCI_TM_READ | SDHCI_TM_BLOCK_COUNT_ENABLE | SDHCI_TM_DMA);
	/* If the mode write did not land the transfer would run with the boot chain's settings, so
	 * treat that as a failure instead of misreading whatever comes back. */
	uint16_t mode = sdhci_read16(SDHCI_TRANSFER_MODE);
	if ((mode & (SDHCI_TM_READ | SDHCI_TM_DMA)) != (SDHCI_TM_READ | SDHCI_TM_DMA)) {
		return -1;
	}
	uint32_t r1[4] = { 0, 0, 0, 0 };
	int cmd_ok = sdhci_command_capture((uint16_t)((17u << 8) | SDHCI_CMD_DATA_PRESENT |
	    SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK), lba, r1);
	if (cmd_ok != 0) {
		ring_puts("EMMC cmd17 fail int=");
		ring_hex(sdhci_read16(SDHCI_INT_STATUS), 4);
		ring_puts(" err=");
		ring_hex(sdhci_read16(SDHCI_ERR_INT_STATUS), 4);
		ring_puts(" st=");
		ring_hex(sdhci_read32(SDHCI_PRESENT_STATE), 8);
		ring_puts("\r\n");
		return -1;
	}
	int tc = sdhci_wait(SDHCI_INT_TRANSFER_COMPLETE, 2000000);
	if (tc <= 0) {
		ring_puts("EMMC smmu140=");
		ring_hex(emmc_smmu_report, 8);
		if (emmc_smmu_base != 0) {
			sirius_emmc_gfsr = *(volatile uint32_t *)(emmc_smmu_base + 0x048u);
			ring_puts(" gfsr=");
			ring_hex(sirius_emmc_gfsr, 8);
		}
		ring_puts(" fixed=");
		ring_dec(emmc_smmu_fixed);
		ring_puts(" r1=");
		ring_hex(r1[0], 8);
		ring_puts(" cmd17 ok, no TC int=");
		ring_hex(sdhci_read16(SDHCI_INT_STATUS), 4);
		ring_puts(" err=");
		ring_hex(sdhci_read16(SDHCI_ERR_INT_STATUS), 4);
		ring_puts(" st=");
		ring_hex(sdhci_read32(SDHCI_PRESENT_STATE), 8);
		ring_puts("\r\n");
		return -1;
	}
	sdhci_write16(SDHCI_INT_STATUS, 0xffffu);
	for (unsigned i = 0; i < 512; i++) {
		out[i] = sdma_buffer[i];
	}
	return 0;
}

/* Single-block read in PIO mode. Returns 0 when the whole 512-byte block was collected. */
static int
sdhci_read_block(uint32_t lba, uint8_t *out)
{
	/* Drop the SDHCI divider to one. The boot chain left 4 in bits[15:8], which halves the clock
	 * twice more on top of the 400 kHz programmed at the root, and a card that slow accepts
	 * commands but never streams data. The spec requires the SD clock to be off while the divider
	 * is written, otherwise the write is simply ignored. */
	{
		uint16_t ctl = sdhci_read16(SDHCI_CLOCK_CONTROL);
		sdhci_write16(SDHCI_CLOCK_CONTROL, (uint16_t)(ctl & ~SDHCI_CLK_SD_EN));
		spin_us(1000);
		sdhci_write16(SDHCI_CLOCK_CONTROL, (uint16_t)((ctl & 0x00c0u) | SDHCI_CLK_INTERNAL_EN));
		for (unsigned waited = 0; waited < 20000; waited += 100) {
			if ((sdhci_read16(SDHCI_CLOCK_CONTROL) & SDHCI_CLK_INTERNAL_STABLE) != 0) {
				break;
			}
			spin_us(100);
		}
		sdhci_write16(SDHCI_CLOCK_CONTROL, (uint16_t)((sdhci_read16(SDHCI_CLOCK_CONTROL) & 0x00c0u) |
		    SDHCI_CLK_INTERNAL_EN | SDHCI_CLK_SD_EN));
		spin_us(1000);
	}

	/* The board wires vqmmc to a 1.8 V rail (sdm710-xiaomi-sirius.dts), so the card's I/O runs at
	 * 1.8 V while the controller was left at 3.0 V. Commands survive that mismatch, multi-line
	 * data does not, which is exactly what the empty DAT lines look like. */
	{
		uint8_t pc = sdhci_read8(SDHCI_POWER_CONTROL);
		ring_puts("EMMC power-in=");
		ring_hex(pc, 2);
		ring_puts(" hc2=");
		ring_hex(sdhci_read8(SDHCI_HOST_CONTROL2), 2);
		ring_puts("\r\n");
		sdhci_write8(SDHCI_POWER_CONTROL, 0x0bu);            /* 1.8 V */
		uint8_t hc2 = sdhci_read8(SDHCI_HOST_CONTROL2);
		sdhci_write8(SDHCI_HOST_CONTROL2, (uint8_t)(hc2 | 0x08u));   /* 1.8 V signalling */
		spin_us(1000);
		ring_puts("EMMC power-out=");
		ring_hex(sdhci_read8(SDHCI_POWER_CONTROL), 2);
		ring_puts(" hc2=");
		ring_hex(sdhci_read8(SDHCI_HOST_CONTROL2), 2);
		ring_puts("\r\n");
	}

	/* Keep the bus width and high-speed bit the boot chain set, and only clear the DMA select.
	 * Switching to 1-bit here hung the boot: the probe runs before the USB console starts, so a
	 * stall in it costs the only log channel. */
	uint8_t hostctl = sdhci_read8(SDHCI_HOST_CONTROL);
	sdhci_write8(SDHCI_HOST_CONTROL, (uint8_t)(hostctl & 0x38u));
	if (emmc_clear_command_line() != 0) {
		return -1;
	}
	sdhci_write16(SDHCI_BLOCK_SIZE, 512u);
	sdhci_write16(SDHCI_BLOCK_COUNT, 1u);
	sdhci_write16(SDHCI_TRANSFER_MODE, SDHCI_TM_READ | SDHCI_TM_BLOCK_COUNT_ENABLE);
	/* PIO needs no DMA, so the SMMU - whose storage stream is configured to abort - stays out of
	 * the picture. Verify the mode write landed; SDHCI ignores it while the line is inhibited. */
	if ((sdhci_read16(SDHCI_TRANSFER_MODE) & SDHCI_TM_READ) == 0) {
		ring_puts("EMMC pio mode write ignored\r\n");
		return -1;
	}
	/* The command index lives in bits 13:8 (SDHCI_MAKE_CMD in Linux); writing it into the flag
	 * bits sends command 0 and the card never answers. */
	if (sdhci_command((uint16_t)((17u << 8) | SDHCI_CMD_DATA_PRESENT | SDHCI_CMD_RESP_48 |
	    SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK), lba) != 0) {
		return -1;      /* CMD17, READ_SINGLE_BLOCK */
	}
	/* PIO drain, the way the SDHCI spec describes it: the Buffer Read Enable bit in the present
	 * state says a word is available, and the transfer is done when all 512 bytes are out. Reading
	 * one word per interrupt-status poll lost data and reported failure even though the sector had
	 * arrived. */
	unsigned received = 0;
	unsigned idle = 0;
	while (received < 512) {
		uint32_t state = sdhci_read32(SDHCI_PRESENT_STATE);
		if ((state & SDHCI_PS_BUFFER_READ_ENABLE) != 0) {
			uint32_t value = sdhci_read32(SDHCI_BUFFER);
			out[received + 0] = (uint8_t)(value & 0xffu);
			out[received + 1] = (uint8_t)((value >> 8) & 0xffu);
			out[received + 2] = (uint8_t)((value >> 16) & 0xffu);
			out[received + 3] = (uint8_t)((value >> 24) & 0xffu);
			received += 4;
			idle = 0;
			continue;
		}
		if ((sdhci_read16(SDHCI_INT_STATUS) & SDHCI_INT_TRANSFER_COMPLETE) != 0) {
			sdhci_write16(SDHCI_INT_STATUS, SDHCI_INT_TRANSFER_COMPLETE);
			break;
		}
		if (++idle > 200000) {
			break;
		}
		spin_us(10);
	}
	sdhci_write16(SDHCI_INT_STATUS, 0xffffu);
	return received == 512 ? 0 : -1;
}

/* Published to the boot-progress panel: apart from the block read the probe is read-only, and its
 * result is the only evidence that the controller is reachable from XNU at all.
 * Steps: 1 mapped, 2 registers read, 3 card present, 4 block read failed, 5 read but no GPT,
 * 6 GPT header found. */
volatile uint32_t sirius_emmc_step;
volatile uint32_t sirius_emmc_version;
volatile uint32_t sirius_emmc_caps;
volatile uint32_t sirius_emmc_state;
volatile uint32_t sirius_emmc_clock;
volatile uint32_t sirius_emmc_power;
volatile uint32_t sirius_emmc_halts;
volatile uint32_t sirius_emmc_signature;

void
sirius_emmc_probe(void)
{
	sirius_emmc_step = 0;
	emmc_base = ml_io_map(SDCC1_PA, SDCC1_SIZE);
	if (emmc_base == 0) {
		sirius_emmc_step = 10;      /* mapping failed */
		return;
	}
	sirius_emmc_step = 1;

	/* The AHB and card clocks must be running before the register window is trustworthy; the boot
	 * chain used both, so a halted branch here means something else is wrong. */
	gcc_base = ml_io_map(GCC_PA, GCC_SIZE);
	uint32_t apps = gcc_read(GCC_SDCC1_APPS_CBCR);
	uint32_t ahb = gcc_read(GCC_SDCC1_AHB_CBCR);
	uint32_t ice = gcc_read(GCC_SDCC1_ICE_CBCR);
	uint32_t bus = gcc_read(GCC_AGGRE_UFS_PHY_AXI_CBCR);
	sirius_emmc_halts = ((apps >> 31) & 1u) | (((ahb >> 31) & 1u) << 1) |
	    (((ice >> 31) & 1u) << 2) | (((bus >> 31) & 1u) << 3);

	sirius_emmc_version = sdhci_read16(SDHCI_VERSION);
	sirius_emmc_caps = sdhci_read32(SDHCI_CAPABILITIES);
	sirius_emmc_state = sdhci_read32(SDHCI_PRESENT_STATE);
	sirius_emmc_clock = sdhci_read16(SDHCI_CLOCK_CONTROL);
	sirius_emmc_power = sdhci_read8(SDHCI_POWER_CONTROL);
	sirius_emmc_step = 2;

	ring_puts("EMMC rcg cfg=");
	ring_hex(gcc_read(GCC_SDCC1_APPS_CFG), 8);
	ring_puts(" m=");
	ring_hex(gcc_read(GCC_SDCC1_APPS_M), 8);
	ring_puts(" n=");
	ring_hex(gcc_read(GCC_SDCC1_APPS_N), 8);
	ring_puts(" d=");
	ring_hex(gcc_read(GCC_SDCC1_APPS_D), 8);
	ring_puts(" vspec=");
	ring_hex(sdhci_read32(MSM_CORE_VENDOR_SPEC), 8);
	ring_puts(" corever=");
	ring_hex(sdhci_read32(MSM_CORE_MCI_VERSION), 8);
	ring_puts(" dll=");
	ring_hex(sdhci_read32(MSM_CORE_DLL_CONFIG), 8);
	ring_puts(" dllst=");
	ring_hex(sdhci_read32(MSM_CORE_DLL_STATUS), 8);
	ring_puts(" pwrctl=");
	ring_hex(sdhci_read32(MSM_CORE_PWRCTL_STATUS), 8);
	ring_puts("\r\n");

	ring_puts("EMMC probe step=");
	ring_dec(sirius_emmc_step);
	ring_puts(" ver=");
	ring_hex(sirius_emmc_version, 4);
	ring_puts(" caps=");
	ring_hex(sirius_emmc_caps, 8);
	ring_puts(" state=");
	ring_hex(sirius_emmc_state, 8);
	ring_puts(" clock=");
	ring_hex(sirius_emmc_clock, 4);
	ring_puts(" power=");
	ring_hex(sirius_emmc_power, 2);
	ring_puts(" halts=");
	ring_hex(sirius_emmc_halts, 1);
	ring_puts("\r\n");

	/* The eMMC is non-removable and this controller does not wire CARD_INSERTED, so the bit is
	 * informational only: try the read and let the command result decide. */
	sirius_emmc_step = 3;

	/* Shortcut before any identification: the boot chain loaded this kernel from the eMMC, so the
	 * card is already selected and in the transfer state with an RCA of its choosing. A selected
	 * card answers CMD17 without caring about its RCA, so a plain block read can work without
	 * CMD0/CMD1/CMD2/CMD3/CMD7 at all. */
	if (emmc_clock_on((uint16_t)sirius_emmc_clock) == 0) {
		uint8_t direct[512];
		int direct_ok = -1;   /* see below: identify the card first, on a clean command line */
		if (direct_ok != 0) {
			direct_ok = sdhci_read_block(1, direct);
		}
		ring_puts("EMMC hostctl=");
	ring_hex(sdhci_read8(SDHCI_HOST_CONTROL), 2);
	ring_puts(" xfer=");
	ring_hex(sdhci_read16(SDHCI_TRANSFER_MODE), 4);
	ring_puts(" blk=");
	ring_hex(sdhci_read16(SDHCI_BLOCK_SIZE), 4);
	ring_puts(" dma=");
	ring_hex(sdhci_read32(SDHCI_DMA_ADDRESS), 8);
	ring_puts("\r\n");
		/* Does a write actually land? SDHCI_TRANSFER_MODE reads back 0x33 after writing 0x13,
		 * which means either the write is ignored or the vendor layer pins the mode. Write a
		 * recognisable value and read it back before trusting anything else. */
		{
			uint16_t before = sdhci_read16(SDHCI_TRANSFER_MODE);
			sdhci_write16(SDHCI_TRANSFER_MODE, 0x1234u);
			uint16_t after = sdhci_read16(SDHCI_TRANSFER_MODE);
			sdhci_write16(SDHCI_TRANSFER_MODE, before);
			ring_puts("EMMC xfer-write before=");
			ring_hex(before, 4);
			ring_puts(" wrote=1234 read=");
			ring_hex(after, 4);
			ring_puts("\r\n");
		}
		ring_puts("EMMC vspec3=");
		ring_hex(sdhci_read32(MSM_CORE_VENDOR_SPEC3), 8);
		ring_puts(" pwrctl-ctl=");
		ring_hex(sdhci_read32(0x24cu), 8);
		ring_puts("\r\n");
	ring_puts("EMMC direct-read=");
		ring_puts(direct_ok == 0 ? "ok" : "fail");
		ring_puts(" sig=");
		for (unsigned i = 0; i < 8; i++) {
			if (direct[i] >= 0x20 && direct[i] < 0x7f) {
				sirius_usb_log_char((char)direct[i]);
			} else {
				sirius_usb_log_char('.');
			}
		}
		ring_puts("\r\n");
		if (direct_ok == 0) {
			sirius_emmc_signature = ((uint32_t)direct[0] << 24) | ((uint32_t)direct[1] << 16) |
			    ((uint32_t)direct[2] << 8) | (uint32_t)direct[3];
			sirius_emmc_step = 6;

			/* Parse the GPT header: the entry array LBA lives at 0x48, the entry count at 0x50
			 * and the entry size at 0x54, all little endian. */
			uint64_t entries_lba = 0;
			for (unsigned i = 0; i < 8; i++) {
				entries_lba |= (uint64_t)direct[0x48u + i] << (8 * i);
			}
			uint32_t entries = (uint32_t)direct[0x50] | ((uint32_t)direct[0x51] << 8) |
			    ((uint32_t)direct[0x52] << 16) | ((uint32_t)direct[0x53] << 24);
			uint32_t entry_size = (uint32_t)direct[0x54] | ((uint32_t)direct[0x55] << 8) |
			    ((uint32_t)direct[0x56] << 16) | ((uint32_t)direct[0x57] << 24);
			ring_puts("EMMC fifo=");
			for (unsigned i = 0; i < 4; i++) {
				ring_hex(sdhci_read32(SDHCI_BUFFER), 8);
				ring_puts(" ");
			}
			ring_puts("\r\n");
			ring_puts("EMMC words=");
			for (unsigned i = 0; i < 8; i++) {
				uint32_t w = ((uint32_t)direct[4 * i] << 24) | ((uint32_t)direct[4 * i + 1] << 16) |
				    ((uint32_t)direct[4 * i + 2] << 8) | (uint32_t)direct[4 * i + 3];
				ring_hex(w, 8);
				ring_puts(" ");
			}
			ring_puts("\r\n");
			ring_puts("EMMC gpt entries_lba=");
			ring_hex((uint32_t)entries_lba, 8);
			ring_puts(" count=");
			ring_dec(entries);
			ring_puts(" size=");
			ring_dec(entry_size);
			ring_puts("\r\n");

			uint8_t table[512];
			if (entry_size >= 128u && entry_size <= 512u && entries != 0 &&
			    sdhci_read_block((uint32_t)entries_lba, table) == 0) {
				unsigned per_sector = 512u / entry_size;
				unsigned shown = 0;
				for (unsigned e = 0; e < per_sector && shown < 4; e++) {
					const uint8_t *entry = table + e * entry_size;
					bool used = false;
					for (unsigned i = 0; i < 16; i++) {
						if (entry[i] != 0) {
							used = true;
						}
					}
					if (!used) {
						continue;
					}
					uint64_t first = 0;
					for (unsigned i = 0; i < 8; i++) {
						first |= (uint64_t)entry[0x20u + i] << (8 * i);
					}
					ring_puts("EMMC part first=");
					ring_hex((uint32_t)first, 8);
					ring_puts(" name=");
					for (unsigned i = 0; i < 36; i++) {
						uint16_t ch = (uint16_t)entry[0x38u + 2u * i] |
						    ((uint16_t)entry[0x39u + 2u * i] << 8);
						if (ch == 0) {
							break;
						}
						sirius_usb_log_char(ch < 0x80 ? (char)ch : '?');
					}
					ring_puts("\r\n");
					shown++;
				}
			}
			return;
		}
	}


	/* The GPT header lives in LBA 1, so reading that sector both proves the data path and returns
	 * the first real piece of the partition table. */
	uint8_t sector[512];
	/* A no-data command first: if the block survives CMD13 the window is fine and the failure is
	 * specific to the data path; if it dies here too, the controller is being reset or powered
	 * down by something outside this driver. */
	{
		/* Clear whatever the boot chain left pending: a stuck command line would make every
		 * command we issue report an error of its own. */
		sdhci_write8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
		for (unsigned waited = 0; waited < 100000; waited += 100) {
			if ((sdhci_read8(SDHCI_SOFTWARE_RESET) & (SDHCI_RESET_CMD | SDHCI_RESET_DATA)) == 0) {
				break;
			}
			spin_us(100);
		}
		sdhci_write16(SDHCI_INT_STATUS, 0xffffu);
		sdhci_write16(SDHCI_ERR_INT_STATUS, 0xffffu);
		ring_puts("EMMC after reset st=");
		ring_hex(sdhci_read32(SDHCI_PRESENT_STATE), 8);
		ring_puts(" rst=");
		ring_hex(sdhci_read8(SDHCI_SOFTWARE_RESET), 2);
		ring_puts("\r\n");

		/* The error summary bit in INT_STATUS hides the command-complete bit whenever any error
		 * status is latched, so the error interrupt is masked and the command status is judged
		 * from the completion bit alone. */
		sdhci_write16(SDHCI_INT_ENABLE, 0xffffu);
		sdhci_write16(SDHCI_ERR_INT_ENABLE, 0x0000u);
		ring_puts("EMMC set400k=");
		ring_puts(emmc_set_card_clock_400k() == 0 ? "ok" : "fail");
		ring_puts(" cfg=");
		ring_hex(gcc_read(GCC_SDCC1_APPS_CFG), 8);
		ring_puts(" m=");
		ring_hex(gcc_read(GCC_SDCC1_APPS_M), 8);
		ring_puts(" n=");
		ring_hex(gcc_read(GCC_SDCC1_APPS_N), 8);
		ring_puts(" d=");
		ring_hex(gcc_read(GCC_SDCC1_APPS_D), 8);
		ring_puts("\r\n");

		/* Full eMMC identification, the same order the kernel uses: CMD0 to idle, CMD1 until the
		 * card leaves its power-up busy state, CMD2 for the CID, CMD3 for the RCA, CMD7 to select
		 * it and CMD16 to fix the block length at 512. */
		/* The card only starts answering once it has seen the card clock for a while: with the
		 * data path exercised first (which spun for seconds) CMD1 answered, without it every
		 * response came back all ones. Give it the clock and a plain wait before CMD0. */
		spin_us(300000);

		ring_puts("EMMC resp-before=");
		for (unsigned i = 0; i < 4; i++) {
			ring_hex(sdhci_read32(SDHCI_RESPONSE + 4u * i), 8);
			ring_puts(" ");
		}
		ring_puts("\r\n");
		int cmd0_ok = sdhci_command((uint16_t)(0u << 8), 0u);
		ring_puts("EMMC cmd0=");
		ring_puts(cmd0_ok == 0 ? "ok" : "fail");
		ring_puts(" resp-after-cmd0=");
		ring_hex(sdhci_read32(SDHCI_RESPONSE), 8);
		ring_puts("\r\n");
		uint32_t ocr = 0;
		int ready = -1;
		for (unsigned tries = 0; tries < 200; tries++) {
			if (tries == 50 || tries == 100 || tries == 150) {
				/* Re-arm: if the first CMD1 was sent while the card was still in its old
				 * state, repeating it after a pause is the cheap way back to idle. */
				(void)sdhci_command((uint16_t)(0u << 8), 0u);
				spin_us(2000);
			}
			if (sdhci_command_capture((uint16_t)((1u << 8) | SDHCI_CMD_RESP_48), 0x40ff8080u, 0) != 0) {
				break;
			}
			ocr = sdhci_read32(SDHCI_RESPONSE);
			if ((ocr & 0x80000000u) == 0) {
				ready = 0;
				break;
			}
			spin_us(1000);
		}
		ring_puts("EMMC resp-after-cmd1=");
		for (unsigned i = 0; i < 4; i++) {
			ring_hex(sdhci_read32(SDHCI_RESPONSE + 4u * i), 8);
			ring_puts(" ");
		}
		ring_puts("\r\n");
		ring_puts("EMMC init ocr=");
		ring_hex(ocr, 8);
		ring_puts(" ready=");
		ring_puts(ready == 0 ? "yes" : "no");

		uint32_t cid[4] = { 0, 0, 0, 0 };
		int cid_ok = -1;
		for (unsigned tries = 0; tries < 3 && cid_ok != 0; tries++) {
			/* A 136-bit response has no index field, and Linux only sets the index check for
			 * commands that carry MMC_RSP_OPCODE, so asking for it here makes the controller
			 * reject the response. CRC stays on. */
			ring_puts("EMMC cid try st=");
			ring_hex(sdhci_read32(SDHCI_PRESENT_STATE), 8);
			ring_puts("\r\n");
			cid_ok = sdhci_command_capture((uint16_t)((2u << 8) | SDHCI_CMD_RESP_136 |
			    SDHCI_CMD_CRC_CHECK), 0u, cid);
			if (cid_ok != 0) {
				spin_us(1000);
			}
		}
		/* CMD2 never answers on this part, but the CID is informational: report it and carry on,
		 * because CMD3/CMD7/CMD16 only need 48-bit responses. */
		ring_puts(cid_ok == 0 ? " cid=ok" : " cid=no");
		ring_puts(" cid=");
		ring_hex(cid[0], 8);

		uint32_t rca_resp[4] = { 0, 0, 0, 0 };
		if (sdhci_command_capture((uint16_t)((3u << 8) | SDHCI_CMD_RESP_48 |
		    SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK), 0u, rca_resp) != 0) {
			ring_fail("rca");
			sirius_emmc_step = 4;
			return;
		}
		uint32_t rca = rca_resp[0] >> 16;
		ring_puts(" rca=");
		ring_hex(rca, 4);

		if (sdhci_command((uint16_t)((7u << 8) | SDHCI_CMD_RESP_48_BUSY |
		    SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK), rca << 16) != 0) {
			ring_fail("select");
			sirius_emmc_step = 4;
			return;
		}
		if (sdhci_command((uint16_t)((16u << 8) | SDHCI_CMD_RESP_48 |
		    SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK), 512u) != 0) {
			ring_fail("blklen");
			sirius_emmc_step = 4;
			return;
		}
		uint32_t card_status[4] = { 0, 0, 0, 0 };
		int st13 = sdhci_command_capture((uint16_t)((13u << 8) | SDHCI_CMD_RESP_48 |
		    SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK), rca << 16, card_status);
		ring_puts(" selected status=");
		ring_puts(st13 == 0 ? "ok" : "fail");
		ring_puts(" cs=");
		ring_hex(card_status[0], 8);
		ring_puts("\r\n");
	}
	if (emmc_clock_on((uint16_t)sirius_emmc_clock) != 0) {
		sirius_emmc_step = 4;
		ring_puts("EMMC clock restart FAILED clock=");
		ring_hex(sdhci_read16(SDHCI_CLOCK_CONTROL), 4);
		ring_puts("\r\n");
		return;
	}
	if (sdhci_read_block(1, sector) != 0) {
		sirius_emmc_step = 4;
		ring_puts("EMMC block read FAILED int=");
		ring_hex(sirius_emmc_fail_int, 4);
		ring_puts(" err=");
		ring_hex(sirius_emmc_fail_err, 4);
		ring_puts(" st=");
		ring_hex(sirius_emmc_fail_state, 8);
		ring_puts(" resp0=");
		ring_hex(sirius_emmc_fail_resp, 8);
		ring_puts(" rst=");
		ring_hex(sirius_emmc_fail_reset, 2);
		ring_puts(" clk2=");
		ring_hex(sirius_emmc_fail_clk, 4);
		ring_puts(" ien=");
		ring_hex(sdhci_read16(SDHCI_INT_ENABLE), 4);
		ring_puts(" bc=");
		ring_hex(sirius_emmc_before_cmd_state, 8);
		ring_puts(" av=");
		ring_hex(sirius_emmc_after_cmd_ver, 4);
		ring_puts(" ac=");
		ring_hex(sirius_emmc_after_cmd_clk, 4);
		ring_puts(" dv=");
		ring_hex(sirius_emmc_dead_ver, 4);
		ring_puts(" dc=");
		ring_hex(sirius_emmc_dead_clk, 4);
		ring_puts(" dp=");
		ring_hex(sirius_emmc_dead_pwr, 2);
		ring_puts(" dh=");
		ring_hex(sirius_emmc_dead_halts, 1);
		ring_puts("\r\n");
		return;
	}
	static const uint8_t expected[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
	bool signature = true;
	for (unsigned i = 0; i < 8; i++) {
		if (sector[i] != expected[i]) {
			signature = false;
		}
	}
	sirius_emmc_signature = ((uint32_t)sector[0] << 24) | ((uint32_t)sector[1] << 16) |
	    ((uint32_t)sector[2] << 8) | (uint32_t)sector[3];
	sirius_emmc_step = signature ? 6 : 5;
	ring_puts("EMMC lba1=");
	for (unsigned i = 0; i < 8; i++) {
		if (sector[i] >= 0x20 && sector[i] < 0x7f) {
			sirius_usb_log_char((char)sector[i]);
		} else {
			sirius_usb_log_char('.');
		}
	}
	ring_puts(signature ? " GPT-OK" : " no-gpt");
	ring_puts("\r\n");
}
