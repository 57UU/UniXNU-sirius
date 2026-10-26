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
#define MSM_CORE_VENDOR_SPEC 0x10cu

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
		uint16_t status = sdhci_read16(SDHCI_INT_STATUS);
		if ((status & mask) != 0) {
			return (int)(status & mask);
		}
		if ((status & SDHCI_INT_ERROR) != 0) {
			return -1;
		}
		spin_us(10);
	}
	return 0;
}

static int
sdhci_command_capture(uint16_t command, uint32_t argument, uint32_t *response)
{
	for (unsigned waited = 0; waited < 100000; waited += 10) {
		if ((sdhci_read32(SDHCI_PRESENT_STATE) & (SDHCI_PS_CMD_INHIBIT | SDHCI_PS_DAT_INHIBIT)) == 0) {
			break;
		}
		spin_us(10);
	}
	sdhci_write16(SDHCI_INT_STATUS, 0xffffu);
	sdhci_write16(SDHCI_ERR_INT_STATUS, 0xffffu);
	sdhci_write32(SDHCI_ARGUMENT, argument);
	sdhci_write16(SDHCI_COMMAND, command);
	if (sdhci_wait(SDHCI_INT_CMD_COMPLETE, 100000) <= 0) {
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

/* Single-block read in PIO mode. Returns 0 when the whole 512-byte block was collected. */
static int
sdhci_read_block(uint32_t lba, uint8_t *out)
{
	sdhci_write16(SDHCI_BLOCK_SIZE, 512u);
	sdhci_write16(SDHCI_BLOCK_COUNT, 1u);
	sdhci_write16(SDHCI_TRANSFER_MODE, SDHCI_TM_READ);
	/* The command index lives in bits 13:8 (SDHCI_MAKE_CMD in Linux); writing it into the flag
	 * bits sends command 0 and the card never answers. */
	if (sdhci_command((uint16_t)((17u << 8) | SDHCI_CMD_DATA_PRESENT | SDHCI_CMD_RESP_48 |
	    SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK), lba) != 0) {
		return -1;      /* CMD17, READ_SINGLE_BLOCK */
	}
	unsigned received = 0;
	while (received < 512) {
		int ready = sdhci_wait(SDHCI_INT_BUFFER_READ_READY | SDHCI_INT_TRANSFER_COMPLETE, 100000);
		if (ready <= 0) {
			return -1;
		}
		if ((ready & SDHCI_INT_BUFFER_READ_READY) != 0) {
			uint32_t value = sdhci_read32(SDHCI_BUFFER);
			out[received + 0] = (uint8_t)(value & 0xffu);
			out[received + 1] = (uint8_t)((value >> 8) & 0xffu);
			out[received + 2] = (uint8_t)((value >> 16) & 0xffu);
			out[received + 3] = (uint8_t)((value >> 24) & 0xffu);
			received += 4;
			sdhci_write16(SDHCI_INT_STATUS, SDHCI_INT_BUFFER_READ_READY);
		}
		if ((ready & SDHCI_INT_TRANSFER_COMPLETE) != 0) {
			sdhci_write16(SDHCI_INT_STATUS, SDHCI_INT_TRANSFER_COMPLETE);
			break;
		}
	}
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
		sdhci_command((uint16_t)(0u << 8), 0u);
		uint32_t ocr = 0;
		int ready = -1;
		for (unsigned tries = 0; tries < 200; tries++) {
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
		ring_puts("EMMC init ocr=");
		ring_hex(ocr, 8);
		ring_puts(" ready=");
		ring_puts(ready == 0 ? "yes" : "no");

		uint32_t cid[4] = { 0, 0, 0, 0 };
		int cid_ok = -1;
		for (unsigned tries = 0; tries < 3 && cid_ok != 0; tries++) {
			cid_ok = sdhci_command_capture((uint16_t)((2u << 8) | SDHCI_CMD_RESP_136 |
			    SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK), 0u, cid);
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
