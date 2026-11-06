/*
 * HakiOS: the Galaxy A21s's touch screen (Omnivision/Synaptics TCM "TD4150" TDDI over SPI).
 *
 * Imported, with the author's permission, from the ios-a135f port (Galaxy A13, the same Exynos 850):
 * Schoosh / simpleXNU (https://github.com/simpleXNU/ios-a135f), with the A21s adaptation by
 * Bruhboy666dev; their bsd/dev/a135f_touch.c after patches 0023-0025. Changes for HakiOS, and only
 * these: the A135F board guard is gone (boot-arg touchscreen=4 starts it instead, from
 * hakios_input_init()), touches go to /dev/hakios-touch (hakios_touch_post, 0..65535 coordinates)
 * instead of their HID layer, and the firmware is the payload's "touchfw" item, which exyloader
 * publishes as the device tree node /a135f-touch, property "firmware" -- the node their driver reads.
 * The original description follows.
 */
/*
 * Touchscreen for the Galaxy A13: Omnivision/Synaptics TCM "TD4150" TDDI over SPI.
 *
 * Function-by-function port of the Linux driver (ovt_tcm: message layer, command
 * layer, ROM-boot "zeroflash" download, touch report bytecode), so a controller
 * that works on Linux behaves the same here. Differences from Linux, on purpose:
 *
 *  - Polled instead of interrupt driven: the attention line (gpa0-0, active low)
 *    is sampled by a kernel thread. If the line never asserts but the controller
 *    still answers reads, the thread falls back to polling blindly.
 *  - SPI is the S3C64xx/USIv2 block of USI_CMGP0 (0x11d00000), programmed like
 *    drivers/spi/spi-s3c64xx.c does for samsung,exynos850-spi in its polled path
 *    (63 byte chunks, TX FIFO filled before the channel is enabled, FIFO flush
 *    after every chunk, SPI mode 3, SCK = 24.576 MHz / 4 = 6.1 MHz <= 7 MHz).
 *    Chip select gpm3-0 is a plain GPIO, like cs-gpios in the DT.
 *  - Supplies are dummy regulators: uniLoader leaves the LDOs on.
 *  - The firmware is not read from /lib/firmware but from the ADT node
 *    /a135f-touch, property "firmware" (the raw ROMBOOT_APP_CODE area of the
 *    vendor image, extracted and CRC-checked by tools/mkadt.py --touch-fw).
 *  - Bring-up is retried (second and later tries start with CMD_RESET), and every
 *    failure prints one self-contained "A135F TOUCH:" diagnosis, which is repeated
 *    a few times because the console scrolls.
 *
 * The panel reset line is shared with the display and is never toggled.
 * Touch points are handed to the HID layer with
 *     void a135f_hid_touch(int finger, int down, int x, int y);
 * in 1080x2408 panel pixels.
 *
 * Boot-args: a135f_touch=0 disables the driver, a135f_touchdbg=1 logs the first
 * decoded touch reports.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/kalloc.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
#include <sys/hakios.input.h>
#include "hakios.touch_power.h"

/*
 * HakiOS: their HID entry, onto /dev/hakios-touch. x, y arrive in panel pixels: the A21s's
 * 720x1600 (its controller reports in that space), or the A13's 1080x2408.
 */
static bool hk_down[16];
static uint32_t hk_x[16], hk_y[16];
static uint32_t hk_panel_w = 720, hk_panel_h = 1600;

static void
a135f_hid_touch(int finger, int down, int x, int y)
{
	if (finger < 0 || finger >= 16) {
		return;
	}
	if (!down) {
		if (hk_down[finger]) {
			hakios_touch_post(HAKIOS_TOUCH_UP, (uint32_t)finger, hk_x[finger], hk_y[finger]);
			hk_down[finger] = false;
		}
		return;
	}
	if (x < 0) {
		x = 0;
	}
	if (y < 0) {
		y = 0;
	}
	hk_x[finger] = (uint32_t)MIN((uint64_t)x * 65535 / (hk_panel_w - 1), 65535);
	hk_y[finger] = (uint32_t)MIN((uint64_t)y * 65535 / (hk_panel_h - 1), 65535);
	hakios_touch_post(hk_down[finger] ? HAKIOS_TOUCH_MOVE : HAKIOS_TOUCH_DOWN, (uint32_t)finger,
	    hk_x[finger], hk_y[finger]);
	hk_down[finger] = true;
}
extern kern_return_t thread_terminate(thread_t);
void hakios_ovt_touch_init(void);

#define CMU_CMGP_PA     0x11c00000u
#define SYSREG_CMGP_PA  0x11c20000u
#define PINCTRL_CMGP_PA 0x11c30000u
#define PINCTRL_ALIVE_PA 0x11850000u
#define SPI_PA          0x11d00000u     /* SPI regs; USI block at +0xc0 */

/* CMU_CMGP (clk-exynos850.c): mux/div/gates; busy = bit 16, gate enable = bit 21 */
#define CMU_MUX_USI0    0x1004          /* 0 = clk_rco_cmgp (49.152 MHz) */
#define CMU_DIV_USI0    0x1804          /* divide by value + 1 */
#define CMU_GAT_USI0_IP 0x2044
#define CMU_GAT_USI0_P  0x2048
#define CMU_BUSY        (1u << 16)
#define CMU_GATE_EN     (1u << 21)
#define CMU_GATE_MANUAL (1u << 20)      /* "reserved"; overrides automatic gating */
#define CMU_GATE_HWACG  (1u << 28)      /* hardware automatic clock gating */
#define SYSREG_USI0_CONF 0x2000         /* USIv2 SW_CONF: BIT(1) = SPI */
#define USI_CON         0xc4            /* USI block (SPI_PA + 0xc0) + 0x04: BIT(0) = reset */

/* S3C64xx SPI (drivers/spi/spi-s3c64xx.c), samsung,exynos850-spi: clk_div 4, FIFO 64 */
#define SPI_CH_CFG      0x00
#define SPI_MODE_CFG    0x08
#define SPI_CS_REG      0x0c
#define SPI_INT_EN      0x10
#define SPI_STATUS      0x14
#define SPI_TX_DATA     0x18
#define SPI_RX_DATA     0x1c
#define SPI_PACKET_CNT  0x20
#define SPI_PENDING_CLR 0x24
#define SPI_SWAP_CFG    0x28
#define SPI_FB_CLK      0x2c
#define CH_HS_EN        (1u << 6)
#define CH_SW_RST       (1u << 5)
#define CH_CPOL         (1u << 3)
#define CH_CPHA         (1u << 2)
#define CH_RXON         (1u << 1)
#define CH_TXON         (1u << 0)
#define MODE_LOOPBACK   (1u << 3)
#define MODE_TXDMA      (1u << 1)
#define MODE_RXDMA      (1u << 2)
#define MODE_4BURST     (1u << 0)
#define MODE_TRAILCNT   (0x3ffu << 19)
#define CS_SIG_INACT    (1u << 0)
#define PKT_EN          (1u << 16)
#define ST_RX_LVL(s)    (((s) >> 15) & 0x1ffu)
#define ST_TX_LVL(s)    (((s) >> 6) & 0x1ffu)
#define ST_ERRORS       0x3cu           /* RX/TX under/overrun */
#define SPI_CHUNK       63              /* FIFO depth 64 - 1, as the Linux polled path */

/* TCM protocol (ovt_tcm) */
#define TCM_MSG_MARKER          0xa5
#define TCM_MSG_PADDING         0x5a
#define TCM_MSG_HEADER_SIZE     4
#define TCM_MIN_READ_LEN        9
#define TCM_MAX_READ_LEN        2048
#define TCM_WR_CHUNK_SIZE       512
#define TCM_RESPONSE_TIMEOUT_MS 1000
#define TCM_FW_DOWNLOAD_TIMEOUT_MS 10000
#define TCM_MODE_SWITCH_DELAY_MS 200
#define TCM_POWER_ON_DELAY_MS   10
#define TCM_FW_RESERVED_BYTES   14
#define TCM_MAX_OBJECTS         10

#define TCM_CMD_CONTINUE_WRITE          0x01
#define TCM_CMD_IDENTIFY                0x02
#define TCM_CMD_RESET                   0x04
#define TCM_CMD_RUN_APPLICATION_FIRMWARE 0x14
#define TCM_CMD_GET_APPLICATION_INFO    0x20
#define TCM_CMD_GET_TOUCH_REPORT_CONFIG 0x25
#define TCM_CMD_ROMBOOT_RUN_BOOTLOADER_FIRMWARE 0x42
#define TCM_CMD_ROMBOOT_DOWNLOAD        0x45

#define TCM_STATUS_IDLE                 0x00
#define TCM_STATUS_OK                   0x01
#define TCM_STATUS_BUSY                 0x02
#define TCM_STATUS_CONTINUED_READ       0x03
#define TCM_STATUS_ERROR                0x0f
#define TCM_STATUS_INVALID              0xff
#define TCM_REPORT_IDENTIFY             0x10
#define TCM_REPORT_TOUCH                0x11

#define TCM_MODE_APPLICATION_FIRMWARE   0x01
#define TCM_MODE_HOSTDOWNLOAD_FIRMWARE  0x02
#define TCM_MODE_ROMBOOTLOADER          0x04
#define TCM_IS_FW_MODE(m) ((m) == TCM_MODE_APPLICATION_FIRMWARE || (m) == TCM_MODE_HOSTDOWNLOAD_FIRMWARE)

#define TCM_CMD_STATUS_IDLE     0
#define TCM_CMD_STATUS_BUSY     1
#define TCM_CMD_STATUS_ERROR    (-1)

/* touch report configuration bytecode */
#define TCM_TOUCH_END                   0x00
#define TCM_TOUCH_FOREACH_ACTIVE_OBJECT 0x01
#define TCM_TOUCH_FOREACH_OBJECT        0x02
#define TCM_TOUCH_FOREACH_END           0x03
#define TCM_TOUCH_PAD_TO_NEXT_BYTE      0x04
#define TCM_TOUCH_OBJECT_N_INDEX        0x06
#define TCM_TOUCH_OBJECT_N_CLASSIFICATION 0x07
#define TCM_TOUCH_OBJECT_N_X_POSITION   0x08
#define TCM_TOUCH_OBJECT_N_Y_POSITION   0x09
#define TCM_TOUCH_OBJECT_N_Z            0x0a
#define TCM_TOUCH_NUM_OF_ACTIVE_OBJECTS 0x18
#define TCM_OBJECT_FINGER               1
#define TCM_OBJECT_GLOVED_FINGER        2

/* struct tcm_identify_info (packed): version, mode, part_number[16], build_id[4], max_write_size[2] */
#define ID_LEN          24
#define ID_MODE         1
#define ID_BUILD        18
#define ID_MAX_WRITE    22
/* struct tcm_app_info (packed): ..., customer_config_id[16] ends at 32 */
#define APP_MAX_X       32
#define APP_MAX_Y       34
#define APP_MAX_OBJECTS 36
#define APP_LEN         38

#define TOUCH_TRIES     5

struct tcm_object {
	unsigned status, x, y, z;
};

/* ---- dummy regulators: the rails are left on by uniLoader ---- */
struct dummy_reg {
	const char *name;
	int enabled;
};
/* ldo28 */
static struct dummy_reg vdd_supply = { "vdd", 0 };
/* vddio: not described in the DT either, the Linux core substitutes its dummy regulator */
static struct dummy_reg vddio_supply = { "vddio", 0 };

static int
dummy_reg_enable(struct dummy_reg *r)
{
	r->enabled = 1;
	return 0;
}

static void __unused
dummy_reg_disable(struct dummy_reg *r)
{
	r->enabled = 0;
}

/* ---- driver state (single controller) ---- */
static vm_offset_t spi, cmu, sysreg, pinc, alive;

static uint8_t in_buf[TCM_MAX_READ_LEN + 8];
static uint8_t cont_buf[TCM_MAX_READ_LEN + 8];
static uint8_t resp_buf[TCM_MAX_READ_LEN + 8];
static uint8_t report_config[TCM_MAX_READ_LEN];
static uint8_t id_info[ID_LEN];
static bool id_info_valid;
static unsigned touch_probe_mode;
static unsigned touch_inline_cont;
static uint8_t app_info[APP_LEN];
static unsigned read_length = TCM_MIN_READ_LEN;
/* Once ready, keep a complete advertised touch frame in one SPI transaction.
 * Otherwise a short lift report shrinks prediction to nine bytes, and a new
 * scan can replace the pending tail before the host reads its continuation. */
static unsigned touch_read_length = TCM_MIN_READ_LEN;
static unsigned wr_chunk_size;
static unsigned payload_length;
static uint8_t status_report_code;
static uint8_t command;
static int command_status = TCM_CMD_STATUS_IDLE;
static uint8_t response_code;
static unsigned resp_length;
static const uint8_t *report_buf;
static unsigned report_buf_len;
static unsigned report_config_size;
static unsigned max_x, max_y, max_objects;
static struct tcm_object objects[TCM_MAX_OBJECTS];
static bool objects_down[TCM_MAX_OBJECTS];

static bool touch_started, blind_mode, attn_seen, spi_loop_ok, spi_loop_done;
static bool msg_dispatched, last_marker_ok;
static int touch_dbg;
static unsigned reports_seen;
static int touch_chan;

/* diagnosis of the last failure, printed by diag_print() */
static struct {
	const char *stage;
	int err;
	unsigned reads, bad_marker, spi_errors, tries;
	unsigned bad_padding, retries, messages, bad_length, read_failures, idles;
	unsigned bad_continued, bad_reports;
	unsigned inline_continued;
	uint32_t spi_status;
	uint8_t last_rx[9];
	uint8_t status_code;
} diag;

static inline unsigned
le2_to_uint(const uint8_t *s)
{
	return (unsigned)s[0] | ((unsigned)s[1] << 8);
}

static inline unsigned
le4_to_uint(const uint8_t *s)
{
	return (unsigned)s[0] | ((unsigned)s[1] << 8) | ((unsigned)s[2] << 16) | ((unsigned)s[3] << 24);
}

static uint64_t
ms_to_abs(unsigned ms)
{
	uint64_t a;
	clock_interval_to_absolutetime_interval(ms, NSEC_PER_MSEC, &a);
	return a;
}

static void
wait_ms(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&touch_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

#ifndef A135F_TOUCH_TEST
/* ------------------------------------------------------------------
 * Register level: USI_CMGP0 as S3C64xx SPI master, CS and attention GPIOs
 * (replaced by a model of the controller in tools/test-touch-tcm.c)
 * ------------------------------------------------------------------ */

/* Variant flags, probed by spi_selftest() when the internal loopback does not work out of the box. */
#define F_MUX1          0x01    /* IPCLK from gout_clkcmu_cmgp_bus instead of the RCO */
#define F_FILLPOST      0x02    /* enable the channel first, fill the TX FIFO afterwards */
#define F_NOPKT         0x04    /* no packet count */
#define F_CS0           0x08    /* CS_REG = 0 instead of CS_SIG_INACT */
#define F_USIRST        0x10    /* pulse the USI reset after the clocks are on */
#define F_ALL           0x20
static unsigned cfg_flags;
static unsigned spin_limit = 20000;

static inline uint32_t rd32(vm_offset_t b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void wr32(vm_offset_t b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }

static void
cmu_wait(uint32_t off)
{
	for (int i = 0; i < 10000 && (rd32(cmu, off) & CMU_BUSY); i++) {
		delay(1);
	}
}

/* clk-exynos-arm64.c exynos_arm64_init_clocks(): every gate is put in manual mode (GATE_MANUAL set,
 * GATE_ENABLE_HWACG cleared) before clk_enable() sets bit 21. Left in automatic mode the USI clock
 * is gated again mid-transfer and the SPI FIFO stalls. */
static void
cmu_gate_on(uint32_t off)
{
	uint32_t v = rd32(cmu, off);

	v |= CMU_GATE_MANUAL | CMU_GATE_EN;
	v &= ~CMU_GATE_HWACG;
	wr32(cmu, off, v);
}

static void
pin_func(uint32_t bank, unsigned pin, unsigned func)
{
	uint32_t con = rd32(pinc, bank);
	con &= ~(0xfu << (pin * 4));
	con |= (func & 0xfu) << (pin * 4);
	wr32(pinc, bank, con);
	/* spi1_pins: no pull, drive strength LV1 (0) */
	wr32(pinc, bank + 0x08, rd32(pinc, bank + 0x08) & ~(3u << (pin * 2)));
	wr32(pinc, bank + 0x0c, rd32(pinc, bank + 0x0c) & ~(3u << (pin * 2)));
}

static void
cs_set(bool active)
{
	uint32_t d = rd32(pinc, 0x60 + 4);
	wr32(pinc, 0x60 + 4, active ? (d & ~1u) : (d | 1u));    /* gpm3-0, active low */
}

static bool
attn(void)
{
	return !(rd32(alive, 0x04) & 1u);       /* gpa0-0 DAT, active low */
}

static void
spi_flush_fifo(void)
{
	uint32_t v;
	int loops;

	wr32(spi, SPI_PACKET_CNT, 0);
	wr32(spi, SPI_CH_CFG, rd32(spi, SPI_CH_CFG) & ~(CH_RXON | CH_TXON));
	wr32(spi, SPI_CH_CFG, (rd32(spi, SPI_CH_CFG) | CH_SW_RST) & ~CH_HS_EN);
	for (loops = 0; loops < 1000 && ST_TX_LVL(rd32(spi, SPI_STATUS)); loops++) {
		delay(1);
	}
	for (loops = 0; loops < 1000; loops++) {
		if (!ST_RX_LVL(rd32(spi, SPI_STATUS))) {
			break;
		}
		(void)rd32(spi, SPI_RX_DATA);
	}
	v = rd32(spi, SPI_CH_CFG) & ~CH_SW_RST;
	wr32(spi, SPI_CH_CFG, v);
	wr32(spi, SPI_CH_CFG, v);
	wr32(spi, SPI_MODE_CFG, rd32(spi, SPI_MODE_CFG) & ~(MODE_TXDMA | MODE_RXDMA));
}

/* One transfer of n <= SPI_CHUNK bytes (full duplex; tx NULL = 0xff filler, rx NULL = discard). */
static int
spi_chunk(const uint8_t *tx, uint8_t *rx, unsigned n)
{
	int err = 0;
	unsigned spins = 0;
	uint32_t st;

	/* s3c64xx_enable_datapath(), polled: packet count, fill the TX FIFO, then channel on */
	if (!(cfg_flags & F_NOPKT)) {
		wr32(spi, SPI_PACKET_CNT, PKT_EN | n);
	}
	if (cfg_flags & F_FILLPOST) {
		wr32(spi, SPI_CH_CFG, (rd32(spi, SPI_CH_CFG) & ~(CH_SW_RST | CH_HS_EN)) | CH_RXON | CH_TXON);
	}
	for (unsigned i = 0; i < n; i++) {
		*(volatile uint8_t *)(spi + SPI_TX_DATA) = tx ? tx[i] : 0xff;
	}
	if (!(cfg_flags & F_FILLPOST)) {
		wr32(spi, SPI_CH_CFG, (rd32(spi, SPI_CH_CFG) & ~(CH_SW_RST | CH_HS_EN)) | CH_RXON | CH_TXON);
	}
	while (ST_RX_LVL(rd32(spi, SPI_STATUS)) < n) {
		if (++spins > spin_limit) {
			err = ETIMEDOUT;
			break;
		}
		delay(1);
	}
	st = rd32(spi, SPI_STATUS);
	diag.spi_status = st;
	if (st & ST_ERRORS) {
		diag.spi_errors++;
	}
	if (!err) {
		for (unsigned i = 0; i < n; i++) {
			uint8_t b = *(volatile uint8_t *)(spi + SPI_RX_DATA);
			if (rx) {
				rx[i] = b;
			}
		}
	}
	spi_flush_fifo();
	return err;
}

static int
spi_hw_init(void)
{
	/* CMU_CMGP: USI0 from the 49.152 MHz RCO, divide by 2 (24.576 MHz IPCLK, SCK = IPCLK / 4), gates on */
	wr32(cmu, CMU_MUX_USI0, (cfg_flags & F_MUX1) ? 1 : 0);
	cmu_wait(CMU_MUX_USI0);
	wr32(cmu, CMU_DIV_USI0, 1);
	cmu_wait(CMU_DIV_USI0);
	cmu_gate_on(CMU_GAT_USI0_P);
	cmu_gate_on(CMU_GAT_USI0_IP);
	/* USI: SPI mode, out of reset (exynos-usi.c) */
	wr32(sysreg, SYSREG_USI0_CONF, (1u << 1));
	if (cfg_flags & F_USIRST) {
		wr32(spi, USI_CON, rd32(spi, USI_CON) | 1u);
		delay(5);
	}
	wr32(spi, USI_CON, rd32(spi, USI_CON) & ~1u);
	delay(5);
	/* spi1_pins: gpm0-0..gpm2-0 SPI function 2; gpm3-0 (cs-gpios) = GPIO output, deasserted */
	for (unsigned b = 0; b < 3; b++) {
		pin_func(b * 0x20, 0, 2);
	}
	wr32(pinc, 0x60 + 4, rd32(pinc, 0x60 + 4) | 1u);
	pin_func(0x60, 0, 1);
	/* gpa0-0 (attention) as input */
	wr32(alive, 0, rd32(alive, 0) & ~0xfu);
	/* s3c64xx_spi_hwinit() + s3c64xx_spi_config(): 8 bit, mode 3, no swap, no DMA, no interrupts */
	wr32(spi, SPI_CS_REG, (cfg_flags & F_CS0) ? 0 : CS_SIG_INACT);
	wr32(spi, SPI_INT_EN, 0);
	wr32(spi, SPI_MODE_CFG, 0);
	wr32(spi, SPI_PACKET_CNT, 0);
	wr32(spi, SPI_PENDING_CLR, 0x1e);
	wr32(spi, SPI_PENDING_CLR, 0);
	wr32(spi, SPI_SWAP_CFG, 0);
	wr32(spi, SPI_FB_CLK, 0);
	wr32(spi, SPI_MODE_CFG, (rd32(spi, SPI_MODE_CFG) & ~MODE_4BURST) | MODE_TRAILCNT);
	wr32(spi, SPI_CH_CFG, CH_CPOL | CH_CPHA);
	spi_flush_fifo();
	return 0;
}

/* These are already powered and mapped by spi_hw_init(); never read RX_DATA
 * here (it consumes data). The fixed register set makes Linux/XNU comparison
 * possible without modifying a running vendor kernel. */
static void
touch_bus_snapshot(const char *stage)
{
	if (!touch_probe_mode) return;
	printf("A135F TOUCH: bus %s mux=%08x div=%08x pclk=%08x ipclk=%08x usi=%08x con=%08x\n",
	    stage, rd32(cmu, CMU_MUX_USI0), rd32(cmu, CMU_DIV_USI0),
	    rd32(cmu, CMU_GAT_USI0_P), rd32(cmu, CMU_GAT_USI0_IP),
	    rd32(sysreg, SYSREG_USI0_CONF), rd32(spi, USI_CON));
	printf("A135F TOUCH: bus ch=%08x mode=%08x cs=%08x status=%08x swap=%08x fb=%08x attn=%d\n",
	    rd32(spi, SPI_CH_CFG), rd32(spi, SPI_MODE_CFG), rd32(spi, SPI_CS_REG),
	    rd32(spi, SPI_STATUS), rd32(spi, SPI_SWAP_CFG), rd32(spi, SPI_FB_CLK), attn());
	for (unsigned b = 0; b < 4; b++) {
		uint32_t off = b * 0x20;
		printf("A135F TOUCH: gpm%u con=%08x dat=%08x pud=%08x drv=%08x\n", b,
		    rd32(pinc, off), rd32(pinc, off + 4), rd32(pinc, off + 8), rd32(pinc, off + 12));
	}
}

/* One internal-loopback transfer (MODE_CFG SELF_LOOPBACK, has_loopback on exynos850). */
static int
spi_loop_once(uint8_t *got)
{
	static const uint8_t pat[16] = { 0xa5, 0x5a, 0x00, 0xff, 0x12, 0x34, 0x56, 0x78,
		                         0x9a, 0xbc, 0xde, 0xf0, 0x01, 0x80, 0x7f, 0xc3 };
	int e;

	bzero(got, sizeof(pat));
	wr32(spi, SPI_MODE_CFG, rd32(spi, SPI_MODE_CFG) | MODE_LOOPBACK);
	e = spi_chunk(pat, got, (unsigned)sizeof(pat));
	wr32(spi, SPI_MODE_CFG, rd32(spi, SPI_MODE_CFG) & ~MODE_LOOPBACK);
	return (e == 0 && memcmp(pat, got, sizeof(pat)) == 0) ? 0 : (e ? e : EIO);
}

/* Watch a stalled transfer: status / packet counter / channel config after 10 us, 200 us, 5 ms. */
static void
spi_stall_probe(unsigned flags)
{
	uint32_t st[3], pk[3], ch[3];
	unsigned old = cfg_flags;

	cfg_flags = flags;
	spi_hw_init();
	wr32(spi, SPI_MODE_CFG, rd32(spi, SPI_MODE_CFG) | MODE_LOOPBACK);
	wr32(spi, SPI_PACKET_CNT, PKT_EN | 16);
	for (unsigned i = 0; i < 16; i++) {
		*(volatile uint8_t *)(spi + SPI_TX_DATA) = (uint8_t)i;
	}
	wr32(spi, SPI_CH_CFG, (rd32(spi, SPI_CH_CFG) & ~(CH_SW_RST | CH_HS_EN)) | CH_RXON | CH_TXON);
	delay(10);
	st[0] = rd32(spi, SPI_STATUS); pk[0] = rd32(spi, SPI_PACKET_CNT); ch[0] = rd32(spi, SPI_CH_CFG);
	delay(190);
	st[1] = rd32(spi, SPI_STATUS); pk[1] = rd32(spi, SPI_PACKET_CNT); ch[1] = rd32(spi, SPI_CH_CFG);
	delay(4800);
	st[2] = rd32(spi, SPI_STATUS); pk[2] = rd32(spi, SPI_PACKET_CNT); ch[2] = rd32(spi, SPI_CH_CFG);
	wr32(spi, SPI_MODE_CFG, rd32(spi, SPI_MODE_CFG) & ~MODE_LOOPBACK);
	spi_flush_fifo();
	printf("A135F TOUCH: stall f=%02x st %08x %08x %08x pkt %08x %08x %08x ch %08x\n",
	    flags, st[0], st[1], st[2], pk[0], pk[1], pk[2], ch[2]);
	cfg_flags = old;
}

/* Internal loopback tells whether the SoC side (clocks, USI mode, FIFO handling) works, independent of
 * the touch controller. If it does not, try every variant of the clock/USI/transfer setup and keep the
 * first one that passes. */
static void
spi_selftest(void)
{
	uint8_t got[16];
	uint32_t pass = 0;
	int e, first = -1;

	e = spi_loop_once(got);
	printf("A135F TOUCH: SPI regs gatip %08x gatp %08x mux %08x div %08x usicon %08x sysreg %08x ch %08x mode %08x st %08x\n",
	    rd32(cmu, CMU_GAT_USI0_IP), rd32(cmu, CMU_GAT_USI0_P), rd32(cmu, CMU_MUX_USI0), rd32(cmu, CMU_DIV_USI0),
	    rd32(spi, USI_CON), rd32(sysreg, SYSREG_USI0_CONF), rd32(spi, SPI_CH_CFG), rd32(spi, SPI_MODE_CFG),
	    rd32(spi, SPI_STATUS));
	if (e) {
		unsigned saved = spin_limit;

		spi_stall_probe(0);
		spi_stall_probe(F_MUX1);
		spin_limit = 400;
		for (unsigned f = 0; f < F_ALL; f++) {
			cfg_flags = f;
			spi_hw_init();
			if (!spi_loop_once(got)) {
				pass |= 1u << f;
				if (first < 0) {
					first = (int)f;
				}
			}
		}
		spin_limit = saved;
		printf("A135F TOUCH: SPI variants passing %08x first %d (flags 1=mux1 2=fillpost 4=nopkt 8=cs0 10=usirst)\n",
		    pass, first);
		cfg_flags = first >= 0 ? (unsigned)first : 0;
		spi_hw_init();
		e = spi_loop_once(got);
	}
	spi_loop_ok = (e == 0);
	spi_loop_done = true;
	printf("A135F TOUCH: SPI loopback %s (err %d, flags %02x, rx %02x %02x %02x %02x)\n",
	    spi_loop_ok ? "ok" : "FAILED", e, cfg_flags, got[0], got[1], got[2], got[3]);
}
#endif /* !A135F_TOUCH_TEST */

/* ------------------------------------------------------------------
 * Raw SPI byte transport (ovt_tcm_spi_read / ovt_tcm_spi_write)
 *
 * Plain full-duplex byte stream: to read N bytes the host clocks out N
 * filler 0xff bytes while capturing MISO. Chip select stays asserted for the
 * whole transfer, which is split in FIFO sized chunks.
 * ------------------------------------------------------------------ */

/* A21s stock DT: ROM/UBL requests 20 us between bytes. Opt-in while measuring
 * external communication; keep the old burst path when the argument is absent. */
static unsigned touch_byte_delay_us;

static int
spi_xfer(const uint8_t *tx, uint8_t *rx, unsigned len)
{
	int err = 0;

	cs_set(true);
	delay(10);
	while (len && !err) {
		unsigned n = touch_byte_delay_us ? 1 : MIN(len, (unsigned)SPI_CHUNK);
		err = spi_chunk(tx, rx, n);
		if (tx) {
			tx += n;
		}
		if (rx) {
			rx += n;
		}
		len -= n;
		if (len && !err && touch_byte_delay_us) delay(touch_byte_delay_us);
	}
	delay(5);
	cs_set(false);
	delay(20);
	return err;
}

static int
ovt_tcm_spi_read(uint8_t *buf, unsigned len)
{
	return spi_xfer(NULL, buf, len);
}

static int
ovt_tcm_spi_write(const uint8_t *buf, unsigned len)
{
	return spi_xfer(buf, NULL, len);
}

/* ------------------------------------------------------------------
 * TCM message layer
 * ------------------------------------------------------------------ */

static void ovt_tcm_report_touch(void);

/* A partial report must not reuse a previous mode or maximum write size. */
static int
ovt_tcm_accept_identify(const uint8_t *data, unsigned length)
{
	id_info_valid = false;
	if (length < ID_LEN ||
	    (!TCM_IS_FW_MODE(data[ID_MODE]) && data[ID_MODE] != TCM_MODE_ROMBOOTLOADER &&
	    (data[ID_MODE] < 0x0b || data[ID_MODE] > 0x0e))) {
		printf("A135F TOUCH: rejected identity: length=%u mode=0x%02x\n",
		    length, length > ID_MODE ? data[ID_MODE] : 0xff);
		bzero(id_info, sizeof(id_info));
		wr_chunk_size = 0;
		return -ENODEV;
	}
	memcpy(id_info, data, ID_LEN);
	wr_chunk_size = MIN(le2_to_uint(&id_info[ID_MAX_WRITE]), (unsigned)TCM_WR_CHUNK_SIZE);
	id_info_valid = true;
	return 0;
}

static void
ovt_tcm_dispatch_report(void)
{
	report_buf = &in_buf[TCM_MSG_HEADER_SIZE];
	report_buf_len = payload_length;

	if (status_report_code == TCM_REPORT_TOUCH) {
		ovt_tcm_report_touch();
	} else if (touch_probe_mode && status_report_code == 0x1b && report_buf_len >= 2) {
		/* Host-download status is diagnostic only. Never send unrequested
		 * application/display configuration, and do not silently discard
		 * the information needed to diagnose a sensor that cannot start. */
		static unsigned logged;
		unsigned state = le2_to_uint(report_buf);
		if (logged++ < 8) printf("A135F TOUCH: firmware status=0x%04x invalid-config=%u need-display=%u need-app=%u hdl=%u\n",
		    state, state & 1, (state >> 1) & 1, (state >> 2) & 1, (state >> 3) & 15);
	}
}

static void
ovt_tcm_dispatch_response(void)
{
	if (command_status != TCM_CMD_STATUS_BUSY) {
		return;
	}

	response_code = status_report_code;
	if (payload_length) {
		memcpy(resp_buf, &in_buf[TCM_MSG_HEADER_SIZE], MIN(payload_length, (unsigned)TCM_MAX_READ_LEN));
	}
	resp_length = payload_length;
	command_status = TCM_CMD_STATUS_IDLE;
}

static void
ovt_tcm_dispatch_message(void)
{
	if (status_report_code == TCM_REPORT_IDENTIFY) {
		if (ovt_tcm_accept_identify(&in_buf[TCM_MSG_HEADER_SIZE], payload_length)) {
			if (command_status == TCM_CMD_STATUS_BUSY) command_status = TCM_CMD_STATUS_ERROR;
			return;
		}

		if (command_status == TCM_CMD_STATUS_BUSY) {
			if (command == TCM_CMD_RESET || command == TCM_CMD_IDENTIFY ||
			    command == TCM_CMD_RUN_APPLICATION_FIRMWARE ||
			    command == TCM_CMD_ROMBOOT_RUN_BOOTLOADER_FIRMWARE) {
				/* These commands reboot the controller into a new mode and answer with
				 * a REPORT_IDENTIFY instead of a plain STATUS_OK: same payload, deliver
				 * it like a normal response. */
				if (payload_length) {
					memcpy(resp_buf, &in_buf[TCM_MSG_HEADER_SIZE],
					    MIN(payload_length, (unsigned)TCM_MAX_READ_LEN));
				}
				resp_length = payload_length;
				response_code = TCM_STATUS_OK;
				command_status = TCM_CMD_STATUS_IDLE;
			} else {
				printf("A135F TOUCH: unsolicited device reset while command 0x%02x pending\n", command);
				command_status = TCM_CMD_STATUS_ERROR;
			}
			return;
		}
	}

	if (status_report_code >= TCM_REPORT_IDENTIFY) {
		ovt_tcm_dispatch_report();
	} else {
		ovt_tcm_dispatch_response();
	}
}

/* Fetch the remainder of a message that did not fit in the initial read_length sized read.
 * total_length includes the 4 byte header and the trailing padding byte. Continuation packets
 * are prefixed with their own 2 byte header (marker + STATUS_CONTINUED_READ). */
static int
ovt_tcm_continued_read(unsigned total_length)
{
	unsigned remaining = total_length - read_length;
	int ret;

	ret = ovt_tcm_spi_read(cont_buf, remaining + 2);
	if (ret) {
		return -ret;
	}
	if (cont_buf[0] != TCM_MSG_MARKER || cont_buf[1] != TCM_STATUS_CONTINUED_READ) {
		static unsigned logged;
		diag.bad_continued++;
		if (logged++ < 8) printf("A135F TOUCH: bad continued-read header (0x%02x, 0x%02x)\n", cont_buf[0], cont_buf[1]);
		return -EIO;
	}
	memcpy(&in_buf[read_length], &cont_buf[2], remaining);
	return 0;
}

/* Some physical A21s frames contain a TCM continued-read prefix after the
 * nine-byte predictive prefix even in one FIFO-sized host read. Accept only
 * this exact transport framing: a touch report, one a5/03 prefix, a terminal
 * padding byte in the adjusted position and padding throughout the remainder.
 * Gate separately until physical counters establish the behavior. */
static bool
ovt_tcm_inline_continuation(unsigned total_length)
{
	if (!touch_inline_cont || !touch_probe_mode || status_report_code != 0x11 ||
	    read_length > SPI_CHUNK || total_length <= TCM_MIN_READ_LEN ||
	    total_length + 2 > read_length ||
	    in_buf[TCM_MIN_READ_LEN] != TCM_MSG_MARKER ||
	    in_buf[TCM_MIN_READ_LEN + 1] != TCM_STATUS_CONTINUED_READ) {
		return false;
	}
	for (unsigned i = total_length + 1; i < read_length; i++) {
		if (in_buf[i] != TCM_MSG_PADDING) return false;
	}
	memmove(in_buf + TCM_MIN_READ_LEN, in_buf + TCM_MIN_READ_LEN + 2,
	    total_length - TCM_MIN_READ_LEN);
	diag.inline_continued++;
	return true;
}

/* Read and dispatch one message. 0 or a negative errno. */
static int
ovt_tcm_read_message(void)
{
	unsigned total_length;
	bool retry = touch_probe_mode != 0;
	int ret;

read_header:
	read_length = MAX(read_length, touch_read_length);
	/* A previous five-contact frame or long command response must not
	 * expand the ready sensor's next read across a FIFO reset boundary. */
	if (touch_probe_mode && touch_read_length > TCM_MIN_READ_LEN) {
		read_length = MIN(read_length, (unsigned)SPI_CHUNK);
	}
	diag.reads++;
	msg_dispatched = false;
	last_marker_ok = false;
	ret = ovt_tcm_spi_read(in_buf, read_length);
	memcpy(diag.last_rx, in_buf, sizeof(diag.last_rx));
	if (ret) {
		diag.read_failures++;
		ret = -ret;
		if (retry) {
			retry = false;
			diag.retries++;
			wait_ms(5);
			goto read_header;
		}
		goto out;
	}

	if (in_buf[0] != TCM_MSG_MARKER) {
		diag.bad_marker++;
		ret = -ENXIO;
		if (retry) {
			/* Samsung's TCM driver retries one header after 5--10 ms.
			 * The A21s recovery log shows 0x7f on the first read and a
			 * valid ROM identity on the next. Never resend a command here. */
			retry = false;
			diag.retries++;
			wait_ms(5);
			goto read_header;
		}
		goto out;
	}

	last_marker_ok = true;
	status_report_code = in_buf[1];
	payload_length = le2_to_uint(&in_buf[2]);

	if (status_report_code <= TCM_STATUS_ERROR || status_report_code == TCM_STATUS_INVALID) {
		switch (status_report_code) {
		case TCM_STATUS_OK:
			break;
		case TCM_STATUS_IDLE:
		case TCM_STATUS_BUSY:
		case TCM_STATUS_CONTINUED_READ:
			/* nothing pending / out-of-sync continuation: not an error, nothing to dispatch */
			diag.idles++;
			ret = 0;
			goto out;
		default:
			printf("A135F TOUCH: bad status code 0x%02x\n", status_report_code);
			diag.status_code = status_report_code;
			payload_length = 0;
			break;
		}
	}

	total_length = TCM_MSG_HEADER_SIZE + payload_length + 1;
	if (total_length > TCM_MAX_READ_LEN) {
		diag.bad_length++;
		printf("A135F TOUCH: implausible payload length %u\n", payload_length);
		ret = -EIO;
		goto out;
	}

	if (total_length <= read_length) {
		/* already have everything */
	} else if (total_length == read_length + 1) {
		in_buf[total_length - 1] = TCM_MSG_PADDING;
	} else {
		ret = ovt_tcm_continued_read(total_length);
		if (ret) {
			goto out;
		}
	}

	if (in_buf[total_length - 1] != TCM_MSG_PADDING &&
	    !ovt_tcm_inline_continuation(total_length)) {
		static unsigned logged;
		diag.bad_padding++;
		if (logged++ < 8) {
			printf("A135F TOUCH: bad message padding 0x%02x code=0x%02x payload=%u read=%u spi=%08x\n",
			    in_buf[total_length - 1], status_report_code, payload_length, read_length, diag.spi_status);
			/* Capture the bytes actually clocked, at most once for each of
			 * the first eight failures. Never accept a shifted/guessed frame. */
			unsigned capture = MIN(read_length, 64u);
			printf("A135F TOUCH: bad wire %u:", capture);
			for (unsigned i = 0; i < capture; i++) printf(" %02x", in_buf[i]);
			printf("\n");
		}
		ret = -EIO;
		/* A corrupt packet is never dispatched. Re-read one complete message
		 * using the same bounded budget as a bad marker; never resend a command. */
		if (retry) {
			retry = false;
			diag.retries++;
			wait_ms(5);
			goto read_header;
		}
		goto out;
	}

	/* predictive read sizing for next time */
	read_length = MAX(touch_read_length, MAX((unsigned)TCM_MIN_READ_LEN, MIN(total_length, (unsigned)TCM_MAX_READ_LEN)));
	if (touch_probe_mode && touch_read_length > TCM_MIN_READ_LEN) {
		read_length = MIN(read_length, (unsigned)SPI_CHUNK);
	}

	msg_dispatched = true;
	diag.messages++;
	ovt_tcm_dispatch_message();
	ret = 0;

out:
	if (ret < 0 && command_status == TCM_CMD_STATUS_BUSY) {
		command_status = TCM_CMD_STATUS_ERROR;
	}
	return ret;
}

/* Poll once: read a message if the attention line is asserted (or, in blind mode, always). */
static int
ovt_tcm_poll(void)
{
	msg_dispatched = false;
	if (attn()) {
		attn_seen = true;
		return ovt_tcm_read_message();
	}
	if (blind_mode) {
		return ovt_tcm_read_message();
	}
	return 1;       /* nothing to do */
}

/*
 * ovt_tcm_write_message() - send a command and wait for its response. On success resp and resp_len
 * (if non-NULL) point at resp_buf, valid until the next call.
 */
static int
ovt_tcm_write_message(uint8_t cmd, const uint8_t *payload, unsigned length,
    const uint8_t **resp, unsigned *resp_len)
{
	unsigned remaining = length + 2;
	unsigned chunk_space, chunks, idx, xfer_len;
	unsigned timeout_ms = TCM_RESPONSE_TIMEOUT_MS;
	uint64_t start, ms_abs = ms_to_abs(1), last_probe = 0;
	uint8_t *out;
	int ret;

	/* CMD_ROMBOOT_DOWNLOAD ignores the normal write-chunk-size cap and goes out as one single
	 * (large) transfer, and needs much longer than a normal command to answer. */
	if (cmd == TCM_CMD_ROMBOOT_DOWNLOAD) {
		chunk_space = remaining;
		timeout_ms = TCM_FW_DOWNLOAD_TIMEOUT_MS;
	} else {
		chunk_space = (wr_chunk_size > 3) ? wr_chunk_size - 1 : remaining;
	}
	chunks = MAX(1u, (remaining + chunk_space - 1) / chunk_space);

	out = kalloc_data(chunk_space + 1, Z_WAITOK | Z_ZERO);
	if (!out) {
		return -ENOMEM;
	}

	command = cmd;
	command_status = TCM_CMD_STATUS_BUSY;

	for (idx = 0; idx < chunks; idx++) {
		xfer_len = MIN(remaining, chunk_space);

		if (idx == 0) {
			out[0] = cmd;
			out[1] = (uint8_t)length;
			out[2] = (uint8_t)(length >> 8);
			if (xfer_len > 2 && length) {
				memcpy(&out[3], payload, xfer_len - 2);
			}
		} else {
			out[0] = TCM_CMD_CONTINUE_WRITE;
			memcpy(&out[1], payload + (idx * chunk_space - 2), xfer_len);
		}

		ret = ovt_tcm_spi_write(out, xfer_len + 1);
		if (ret) {
			ret = -ret;
			goto done;
		}
		remaining -= xfer_len;

		if (chunks > 1) {
			delay(700);
		}
	}

	/* wait for the response (Linux: wait_for_completion_timeout() while the IRQ thread reads) */
	start = mach_absolute_time();
	while (command_status == TCM_CMD_STATUS_BUSY) {
		uint64_t elapsed = (mach_absolute_time() - start) / ms_abs;

		if (elapsed >= timeout_ms) {
			printf("A135F TOUCH: timed out waiting for response to command 0x%02x\n", cmd);
			ret = -ETIMEDOUT;
			goto done;
		}
		(void)ovt_tcm_poll();
		/* attention never seen: probe the bus anyway, the pin may not be what we think it is */
		if (command_status == TCM_CMD_STATUS_BUSY && !attn_seen && !blind_mode &&
		    elapsed >= 50 && elapsed - last_probe >= 10) {
			last_probe = elapsed;
			(void)ovt_tcm_read_message();
			if (last_marker_ok) {
				blind_mode = true;
				printf("A135F TOUCH: attention line never asserted but the controller answers: polling blindly\n");
			}
		}
		if (command_status == TCM_CMD_STATUS_BUSY) {
			wait_ms(1);
		}
	}

	if (command_status != TCM_CMD_STATUS_IDLE) {
		ret = -EIO;
		goto done;
	}
	if (response_code != TCM_STATUS_OK) {
		printf("A135F TOUCH: command 0x%02x failed, status 0x%02x\n", cmd, response_code);
		diag.status_code = response_code;
		ret = -EIO;
		goto done;
	}
	if (resp) {
		*resp = resp_buf;
	}
	if (resp_len) {
		*resp_len = resp_length;
	}
	ret = 0;

done:
	command = 0;
	command_status = TCM_CMD_STATUS_IDLE;
	kfree_data(out, chunk_space + 1);
	return ret;
}

/* ------------------------------------------------------------------
 * Touch report parsing
 *
 * The firmware describes its own report layout as a small bytecode (fetched once with
 * CMD_GET_TOUCH_REPORT_CONFIG) that is walked bit by bit against each REPORT_TOUCH payload.
 * ------------------------------------------------------------------ */

static int
tcm_get_report_bits(const uint8_t *report, unsigned report_len, unsigned offset,
    unsigned bits, unsigned *out)
{
	unsigned byte_off, bit_off, avail, take, got = 0, val = 0;

	if (!bits || bits > 32) {
		return -EINVAL;
	}
	if (offset + bits > report_len * 8) {
		return -EINVAL;
	}
	byte_off = offset / 8;
	bit_off = offset % 8;
	while (got < bits) {
		unsigned byte = (unsigned)report[byte_off] >> bit_off;

		avail = 8 - bit_off;
		take = MIN(avail, bits - got);
		byte &= (1u << take) - 1u;
		val |= byte << got;
		got += take;
		bit_off = 0;
		byte_off++;
	}
	*out = val;
	return 0;
}

static bool
ovt_tcm_parse_touch_report(void)
{
	const uint8_t *config = report_config;
	unsigned config_size = report_config_size;
	const uint8_t *report = report_buf;
	unsigned report_len = report_buf_len;
	unsigned idx = 0, offset = 0, obj = 0, next = 0;
	unsigned active_seen = 0, num_active = 0, end_of_foreach = 0;
	bool active_only = false, have_num_active = false;
	unsigned steps = 0;

	if (!config_size) {
		return false;
	}
	bzero(objects, sizeof(objects));

	while (idx < config_size) {
		/* A corrupt loop with no consuming fields must not pin a CPU. */
		if (++steps > config_size * (TCM_MAX_OBJECTS + 1)) return false;
		uint8_t code = config[idx++];
		unsigned bits, data = 0;

		switch (code) {
		case TCM_TOUCH_END:
			return true;

		case TCM_TOUCH_FOREACH_ACTIVE_OBJECT:
			obj = 0;
			next = idx;
			active_only = true;
			if (!have_num_active && offset == report_len * 8) {
				/* No-count reports describe objects until payload exhaustion.
				 * A prefix-only frame is the valid all-fingers-up report.
				 * Walk opcodes, not bytes: a field width may itself be 0x03. */
				unsigned depth = 1;
				while (idx < config_size && depth) {
					uint8_t skip = config[idx++];
					if (skip == TCM_TOUCH_FOREACH_ACTIVE_OBJECT || skip == TCM_TOUCH_FOREACH_OBJECT) depth++;
					else if (skip == TCM_TOUCH_FOREACH_END) depth--;
					else if (skip == TCM_TOUCH_END) return false;
					else if (skip != TCM_TOUCH_PAD_TO_NEXT_BYTE) {
						if (idx >= config_size) return false;
						idx++;
					}
				}
				if (depth) return false;
			}
			break;

		case TCM_TOUCH_FOREACH_OBJECT:
			obj = 0;
			next = idx;
			active_only = false;
			break;

		case TCM_TOUCH_FOREACH_END:
			end_of_foreach = idx;
			if (active_only) {
				if (have_num_active) {
					active_seen++;
					if (active_seen < num_active) {
						idx = next;
					}
				} else if (offset < report_len * 8) {
					idx = next;
				}
			} else {
				obj++;
				if (obj < max_objects) {
					idx = next;
				}
			}
			break;

		case TCM_TOUCH_PAD_TO_NEXT_BYTE:
			offset = (offset + 7) & ~7u;
			if (offset > report_len * 8) return false;
			break;

		case TCM_TOUCH_OBJECT_N_INDEX:
			bits = (idx < config_size) ? config[idx++] : 0;
			if (tcm_get_report_bits(report, report_len, offset, bits, &obj) || obj >= max_objects) return false;
			offset += bits;
			break;

		case TCM_TOUCH_OBJECT_N_CLASSIFICATION:
		case TCM_TOUCH_OBJECT_N_X_POSITION:
		case TCM_TOUCH_OBJECT_N_Y_POSITION:
		case TCM_TOUCH_OBJECT_N_Z:
			bits = (idx < config_size) ? config[idx++] : 0;
			if (tcm_get_report_bits(report, report_len, offset, bits, &data)) return false;
			if (obj < max_objects) {
				if (code == TCM_TOUCH_OBJECT_N_CLASSIFICATION) {
					objects[obj].status = data;
				} else if (code == TCM_TOUCH_OBJECT_N_X_POSITION) {
					objects[obj].x = data;
				} else if (code == TCM_TOUCH_OBJECT_N_Y_POSITION) {
					objects[obj].y = data;
				} else {
					objects[obj].z = data;
				}
			}
			offset += bits;
			break;

		case TCM_TOUCH_NUM_OF_ACTIVE_OBJECTS:
			bits = (idx < config_size) ? config[idx++] : 0;
			if (tcm_get_report_bits(report, report_len, offset, bits, &data) || data > max_objects) return false;
			num_active = data;
			have_num_active = true;
			offset += bits;
			if (!num_active) {
				if (!end_of_foreach) {
					return true;
				}
				idx = end_of_foreach;
			}
			break;

		default:
			/* field we do not care about: still skip the right number of bits to stay in sync */
			bits = (idx < config_size) ? config[idx++] : 0;
			if (!bits || offset + bits > report_len * 8) return false;
			offset += bits;
			break;
		}
	}
	return true;
}

static void
ovt_tcm_report_touch(void)
{
	if (!max_objects || !max_x || !max_y) {
		return;         /* report before the application info is known: ignore */
	}
	if (!ovt_tcm_parse_touch_report()) {
		static unsigned rejected;
		diag.bad_reports++;
		if (rejected++ < 4) printf("A135F TOUCH: rejected malformed touch report (%u payload bytes)\n", report_buf_len);
		return; /* retain prior finger state; never turn truncated data into a phantom tap/lift */
	}

	for (unsigned i = 0; i < max_objects && i < TCM_MAX_OBJECTS; i++) {
		struct tcm_object *o = &objects[i];

		if (o->status == TCM_OBJECT_FINGER || o->status == TCM_OBJECT_GLOVED_FINGER) {
			uint32_t raw_x = MIN(o->x, max_x);
			uint32_t raw_y = MIN(o->y, max_y);
			int px, py;
			if (max_x < 800) {
				/* A21s app-info is already in its native 720x1600 pixel space. */
				px = (int)raw_x;
				py = (int)raw_y;
				hk_panel_w = 720;
				hk_panel_h = 1600;
			} else {
				hk_panel_w = 1080;
				hk_panel_h = 2408;
				/* Preserve the A13 scale for its wider native digitizer. */
				px = (int)((uint64_t)raw_x * 1079 / max_x);
				py = (int)((uint64_t)raw_y * 2407 / max_y);
			}

			if (touch_dbg && reports_seen < 30) {
				printf("A135F TOUCH: obj %u status %u raw %u,%u z %u -> %d,%d\n",
				    i, o->status, o->x, o->y, o->z, px, py);
			}
			a135f_hid_touch((int)i, 1, px, py);
			objects_down[i] = true;
		} else if (objects_down[i]) {
			a135f_hid_touch((int)i, 0, 0, 0);
			objects_down[i] = false;
		}
	}
	if (reports_seen++ == 0) {
		printf("A135F TOUCH: first touch report\n");
	}
}

/* ------------------------------------------------------------------
 * Controller bring-up
 * ------------------------------------------------------------------ */

/* Only the explicit A21s PMIC test uses this channel. Protocol addresses from
 * Exynos850 E850-96 U-Boot; never access another PMIC register or change voltage.
 */
static unsigned touch_pmic_mode;
static struct { vm_offset_t mailbox, shared; } touch_power_maps;
static uint32_t touch_power_read(void *c,unsigned space,uint32_t offset)
{
    (void)c;
    __builtin_arm_dsb(0xf);
    return rd32(space?touch_power_maps.shared:touch_power_maps.mailbox,offset);
}
static void touch_power_write(void *c,unsigned space,uint32_t offset,uint32_t value)
{
    (void)c;
    wr32(space?touch_power_maps.shared:touch_power_maps.mailbox,offset,value);
    __builtin_arm_dsb(0xf);
}
static uint64_t touch_power_time(void *c)
{
    uint64_t n;(void)c;absolutetime_to_nanoseconds(mach_absolute_time(),&n);return n;
}
static void touch_power_relax(void *c) { (void)c;delay(1); }
static int touch_power_prepare(void)
{
    if(!touch_pmic_mode)return 0;
    if(!touch_power_maps.mailbox) {
        touch_power_maps.mailbox=ml_io_map(0x11900000,0x1000);
        vm_offset_t page=ml_io_map(0x0203c000,0x4000);
        if(!touch_power_maps.mailbox||!page)return -ENOMEM;
        touch_power_maps.shared=page+0x200;
    }
    struct hakios_touch_power_io io={NULL,touch_power_read,touch_power_write,touch_power_time,touch_power_relax};
    uint8_t before=0,after=0;
    printf("A135F TOUCH: ACPM channel 10, LDO28 mode %u: read before action\n",touch_pmic_mode);
    int e=hakios_touch_power(&io,touch_pmic_mode,&before,&after);
    printf("A135F TOUCH: LDO28 CTRL 0x%02x -> 0x%02x, result %d; selector bits preserved\n",before,after,e);
    if(!e && touch_pmic_mode==2)wait_ms(20);
    return e;
}

static int
ovt_tcm_power_on(void)
{
	int ret;

	ret = touch_power_prepare();
	if(ret)return ret;
	ret = dummy_reg_enable(&vdd_supply);
	if (ret) {
		return ret;
	}
	ret = dummy_reg_enable(&vddio_supply);
	if (ret) {
		return ret;
	}
	/* rails settle before the controller is talked to (harmless for bootloader managed rails) */
	wait_ms(TCM_POWER_ON_DELAY_MS);
	return 0;
}

static int
ovt_tcm_identify(void)
{
	const uint8_t *resp;
	unsigned resp_len;
	int ret;

	ret = ovt_tcm_write_message(TCM_CMD_IDENTIFY, NULL, 0, &resp, &resp_len);
	if (ret) {
		return ret;
	}
	return ovt_tcm_accept_identify(resp, resp_len);
}

static int
ovt_tcm_get_app_info(void)
{
	const uint8_t *resp;
	unsigned resp_len;
	int ret;

	for (unsigned attempt = 0; ; attempt++) {
		ret = ovt_tcm_write_message(TCM_CMD_GET_APPLICATION_INFO, NULL, 0, &resp, &resp_len);
		if (ret) return ret;
		if (resp_len < APP_LEN) {
			printf("A135F TOUCH: short application info: %u bytes, need %u\n", resp_len, APP_LEN);
			return -EINVAL;
		}
		memcpy(app_info, resp, APP_LEN);
		unsigned status = le2_to_uint(&app_info[2]);
		if (!touch_probe_mode || status == 0) break;
		/* Protocol status 1/2 means firmware is still booting/updating.
		 * Poll the read-only info command for at most one second; never
		 * declare ready merely because stale dimensions are present. */
		if (status != 1 && status != 2) {
			printf("A135F TOUCH: application not ready: status 0x%04x\n", status);
			return -EINVAL;
		}
		if (attempt == 10) {
			printf("A135F TOUCH: application startup timed out: status 0x%04x\n", status);
			return -ETIMEDOUT;
		}
		if (attempt == 0) printf("A135F TOUCH: waiting for application status 0x%04x\n", status);
		wait_ms(100);
	}

	max_x = le2_to_uint(&app_info[APP_MAX_X]);
	max_y = le2_to_uint(&app_info[APP_MAX_Y]);
	max_objects = le2_to_uint(&app_info[APP_MAX_OBJECTS]);
	if (max_objects > TCM_MAX_OBJECTS) {
		max_objects = TCM_MAX_OBJECTS;
	}
	if (!max_x || !max_y || !max_objects) {
		printf("A135F TOUCH: implausible application info (%ux%u, %u objects)\n", max_x, max_y, max_objects);
		return -EINVAL;
	}
	return 0;
}

static int
ovt_tcm_get_report_config(void)
{
	const uint8_t *resp;
	unsigned resp_len;
	int ret;

	ret = ovt_tcm_write_message(TCM_CMD_GET_TOUCH_REPORT_CONFIG, NULL, 0, &resp, &resp_len);
	if (ret) {
		return ret;
	}
	if (!resp_len) {
		return -EINVAL;
	}
	memcpy(report_config, resp, MIN(resp_len, (unsigned)sizeof(report_config)));
	report_config_size = MIN(resp_len, (unsigned)sizeof(report_config));
	return 0;
}

/*
 * ROM-bootloader firmware download (zeroflash). The controller has no persistent flash for the
 * application firmware: on every cold boot it comes up in TCM_MODE_ROMBOOTLOADER and must be
 * pushed a firmware image before application mode exists at all.
 */
static int
ovt_tcm_romboot_download(const uint8_t *fw_data, unsigned fw_len)
{
	const uint8_t *resp;
	unsigned resp_len, out_len;
	uint8_t *out;
	int ret;

	out_len = TCM_FW_RESERVED_BYTES + fw_len;
	out = kalloc_data(out_len, Z_WAITOK | Z_ZERO);
	if (!out) {
		return -ENOMEM;
	}
	/* CMD_ROMBOOT_DOWNLOAD payload: 14 reserved bytes (top byte of the 24-bit image size, rest zero)
	 * followed by the raw firmware bytes */
	out[0] = (uint8_t)(fw_len >> 16);
	memcpy(out + TCM_FW_RESERVED_BYTES, fw_data, fw_len);

	printf("A135F TOUCH: downloading %u byte application firmware\n", fw_len);
	diag.stage = "fw-download";
	ret = ovt_tcm_write_message(TCM_CMD_ROMBOOT_DOWNLOAD, out, out_len, &resp, &resp_len);
	kfree_data(out, out_len);
	if (ret) {
		printf("A135F TOUCH: firmware download failed: %d\n", ret);
		return ret;
	}

	diag.stage = "fw-run";
	id_info_valid = false;
	ret = ovt_tcm_write_message(TCM_CMD_ROMBOOT_RUN_BOOTLOADER_FIRMWARE, NULL, 0, &resp, &resp_len);
	if (ret) {
		printf("A135F TOUCH: failed to run downloaded bootloader firmware: %d\n", ret);
		return ret;
	}
	wait_ms(TCM_MODE_SWITCH_DELAY_MS);
	return 0;
}

/* Messages the controller announced on its own (the identify report after power-up, touch reports):
 * Linux reads them from the IRQ thread, which is registered before init_device(). */
static void
ovt_tcm_drain(void)
{
	for (int i = 0; i < 8 && attn(); i++) {
		attn_seen = true;
		if (ovt_tcm_read_message() < 0) {
			break;
		}
	}
}

static int
ovt_tcm_init_device(const uint8_t *fw, unsigned fw_len, bool soft_reset)
{
	const uint8_t *resp;
	unsigned resp_len;
	int ret;

	id_info_valid = false;
	bzero(id_info, sizeof(id_info));
	if (touch_probe_mode && !soft_reset) {
		/* Linux sensor detection reads the power-on report before sending commands.
		 * Read even without ATTN, since the pin may not yet reflect controller state.
		 * This gate adds no reset or firmware write before the first read. */
		diag.stage = "startup-read";
		ret = ovt_tcm_read_message();
		printf("A135F TOUCH: startup read result=%d identity=%d\n", ret, id_info_valid);
	} else {
		ovt_tcm_drain();
	}

	if (soft_reset) {
		diag.stage = "reset";
		id_info_valid = false;
		(void)ovt_tcm_write_message(TCM_CMD_RESET, NULL, 0, &resp, &resp_len);
		wait_ms(TCM_MODE_SWITCH_DELAY_MS);
	}

	diag.stage = "identify";
	ret = (touch_probe_mode && id_info_valid) ? 0 : ovt_tcm_identify();
	if (ret) {
		return ret;
	}
	printf("A135F TOUCH: controller mode 0x%02x, max write %u\n", id_info[ID_MODE], wr_chunk_size);

	if (id_info[ID_MODE] == TCM_MODE_ROMBOOTLOADER) {
		if (!fw || !fw_len) {
			printf("A135F TOUCH: zeroflash controller needs firmware (ADT /a135f-touch \"firmware\")\n");
			diag.stage = "no-firmware";
			return -ENOENT;
		}
		printf("A135F TOUCH: controller in ROM bootloader, pushing application firmware\n");
		ret = ovt_tcm_romboot_download(fw, fw_len);
		if (ret) {
			return ret;
		}
		diag.stage = "identify2";
		/* RUN already answers with a validated REPORT_IDENTIFY. The real
		 * A21s entered mode 0x02 here, then timed out on a redundant IDENTIFY.
		 * Only query again when the transition did not supply its identity. */
		ret = (touch_probe_mode && id_info_valid) ? 0 : ovt_tcm_identify();
		if (ret) {
			return ret;
		}
		printf("A135F TOUCH: after firmware run: mode 0x%02x, identity %s\n",
		    id_info[ID_MODE], id_info_valid ? "validated" : "unavailable");
	}

	if (!TCM_IS_FW_MODE(id_info[ID_MODE])) {
		/* some other bootloader mode: the firmware may already be resident and just needs a nudge */
		printf("A135F TOUCH: controller in mode 0x%02x, requesting application firmware\n", id_info[ID_MODE]);
		diag.stage = "run-app";
		id_info_valid = false;
		ret = ovt_tcm_write_message(TCM_CMD_RUN_APPLICATION_FIRMWARE, NULL, 0, &resp, &resp_len);
		if (ret) {
			return ret;
		}
		wait_ms(TCM_MODE_SWITCH_DELAY_MS);
		diag.stage = "identify3";
		ret = (touch_probe_mode && id_info_valid) ? 0 : ovt_tcm_identify();
		if (ret) {
			return ret;
		}
		if (!TCM_IS_FW_MODE(id_info[ID_MODE])) {
			printf("A135F TOUCH: controller stuck in mode 0x%02x after firmware download\n", id_info[ID_MODE]);
			diag.stage = "stuck-mode";
			return -ENODEV;
		}
	}

	diag.stage = "app-info";
	ret = ovt_tcm_get_app_info();
	if (ret) {
		return ret;
	}
	diag.stage = "report-config";
	ret = ovt_tcm_get_report_config();
	if (ret) {
		return ret;
	}
	diag.stage = "ready";
	if (touch_probe_mode) {
		unsigned maximum = le2_to_uint(&app_info[14]);
		/* The advertised 256 is a capacity, not every report's length.
		 * A21s reports are 9 + 11*N bytes including header/padding (N<=5).
		 * Stay in one 63-byte FIFO burst; a final padding byte needs no
		 * continuation, as handled above. Larger layouts retain continuation.
		 * Do not assume every firmware uses the A21s's 11-byte object layout. */
		touch_read_length = MAX((unsigned)TCM_MIN_READ_LEN,
		    MIN(maximum + TCM_MSG_HEADER_SIZE + 1, (unsigned)SPI_CHUNK));
		read_length = touch_read_length;
		printf("A135F TOUCH: touch read window %u bytes (advertised payload %u)\n", touch_read_length, maximum);
		printf("A135F TOUCH: report config %u bytes:", report_config_size);
		for (unsigned i = 0; i < MIN(report_config_size, 64u); i++) printf(" %02x", report_config[i]);
		printf("\n");
	}
	printf("A135F TOUCH: TD4150 ready: %ux%u, %u objects, fw build %u\n",
	    max_x, max_y, max_objects, le4_to_uint(&id_info[ID_BUILD]));
	return 0;
}

/* One diagnosis line pair; everything needed to tell SoC-side from controller-side trouble. */
static void
diag_print(const char *prefix)
{
	printf("A135F TOUCH: %s stage=%s err=%d try=%u attn=%d seen=%d blind=%d loop=%s\n",
	    prefix, diag.stage ? diag.stage : "?", diag.err, diag.tries, attn() ? 1 : 0, attn_seen ? 1 : 0,
	    blind_mode ? 1 : 0, !spi_loop_done ? "n/a" : (spi_loop_ok ? "ok" : "BAD"));
	printf("A135F TOUCH:   reads=%u badmarker=%u spierr=%u spist=%08x status=%02x rx=%02x %02x %02x %02x %02x %02x\n",
	    diag.reads, diag.bad_marker, diag.spi_errors, diag.spi_status, diag.status_code,
	    diag.last_rx[0], diag.last_rx[1], diag.last_rx[2], diag.last_rx[3], diag.last_rx[4], diag.last_rx[5]);
	printf("A135F TOUCH:   totals messages=%u frames=%u padding=%u continued=%u malformed=%u retries=%u busfail=%u idle=%u badlength=%u inline=%u\n",
	    diag.messages, reports_seen, diag.bad_padding, diag.bad_continued, diag.bad_reports,
	    diag.retries, diag.read_failures, diag.idles, diag.bad_length, diag.inline_continued);
}

/* Power on and bring the controller to application mode, retrying a few times. */
static int
touch_bringup(const uint8_t *fw, unsigned fw_len)
{
	int ret;

	ret = ovt_tcm_power_on();
	if (ret) {
		diag.stage = "power";
		diag.err = ret;
		return ret;
	}
	for (unsigned t = 1; t <= TOUCH_TRIES; t++) {
		diag.tries = t;
		if (t > 2) {
			spi_hw_init();
		}
		bzero(diag.last_rx, sizeof(diag.last_rx));
		read_length = TCM_MIN_READ_LEN;
		touch_read_length = TCM_MIN_READ_LEN;
		command_status = TCM_CMD_STATUS_IDLE;
		ret = ovt_tcm_init_device(fw, fw_len, t > 1);
		if (!ret) {
			return 0;
		}
		diag.err = ret;
		diag_print("try failed:");
		if (t < TOUCH_TRIES) {
			wait_ms(500 * t);
		}
	}
	return ret;
}

static bool
touch_recovery_needed(int result, unsigned *errors)
{
	if (result < 0) {
		(*errors)++;
	} else if (msg_dispatched) {
		*errors = 0;
	}
	/* An idle attention pin is not a successful transfer. Clearing errors
	 * on idle polls prevented recovery from recurring framing failures. */
	return *errors >= (touch_probe_mode ? 8u : 100u);
}

static void
touch_release_contacts(void)
{
	for (unsigned i = 0; i < max_objects && i < TCM_MAX_OBJECTS; i++) {
		a135f_hid_touch((int)i, 0, 0, 0);
		objects_down[i] = false;
	}
}

#ifndef A135F_TOUCH_TEST
static void
touch_thread(void *arg, wait_result_t result)
{
	const uint8_t *fw = NULL;
	unsigned fw_len = 0;
	DTEntry node;
	void const *prop;
	unsigned int size;
	unsigned consecutive_errors = 0;
	uint64_t next_diag;
	(void)arg;
	(void)result;

	if (SecureDTLookupEntry(NULL, "/a135f-touch", &node) == kSuccess &&
	    SecureDTGetProperty(node, "firmware", &prop, &size) == kSuccess) {
		fw = prop;
		fw_len = size;
		printf("A135F TOUCH: firmware in ADT: %u bytes, starts %02x %02x\n",
		    fw_len, fw_len > 0 ? fw[0] : 0, fw_len > 1 ? fw[1] : 0);
	} else {
		printf("A135F TOUCH: no firmware in the ADT (/a135f-touch)\n");
	}

	spi_selftest();
	touch_bus_snapshot("before-startup");

	if (touch_bringup(fw, fw_len) != 0) {
		/* the console scrolls: repeat the diagnosis for a while so one photo is enough */
		for (int i = 0; i < 12; i++) {
			diag_print("bring-up FAILED, touch disabled:");
			wait_ms(10000);
		}
		thread_terminate(current_thread());
		return;
	}

	clock_interval_to_deadline(60, NSEC_PER_SEC, &next_diag);
	for (;;) {
		int r = ovt_tcm_poll();
		if (touch_probe_mode && mach_absolute_time() >= next_diag) {
			diag_print("running:");
			clock_interval_to_deadline(60, NSEC_PER_SEC, &next_diag);
		}

		if (touch_recovery_needed(r, &consecutive_errors)) {
			printf("A135F TOUCH: stream failed %u times, releasing contacts and re-initializing\n", consecutive_errors);
			touch_release_contacts();
			consecutive_errors = 0;
			if (touch_bringup(fw, fw_len) != 0) {
				diag_print("re-init FAILED:");
			}
		}
		if (!msg_dispatched) {
			wait_ms(blind_mode ? 8 : 1);
		}
	}
}

void
hakios_ovt_touch_init(void)
{
	int enabled = 1;
	uint32_t touchscreen = 0;
	thread_t thread;

	if (touch_started) {
		return;
	}
	/* HakiOS: touchscreen=4 is this driver (1: Goodix/FocalTech, 3: the A32's FTS) */
	if (!PE_parse_boot_argn("touchscreen", &touchscreen, sizeof(touchscreen)) || touchscreen != 4) {
		return;
	}
	touch_started = true;
	(void)PE_parse_boot_argn("a135f_touch", &enabled, sizeof(enabled));
	(void)PE_parse_boot_argn("a135f_touchdbg", &touch_dbg, sizeof(touch_dbg));
    (void)PE_parse_boot_argn("a135f_touch_pmic", &touch_pmic_mode, sizeof(touch_pmic_mode));
    if(touch_pmic_mode>2) {printf("A135F TOUCH: invalid PMIC mode; disabled\n");return;}
    (void)PE_parse_boot_argn("a135f_touch_probe", &touch_probe_mode, sizeof(touch_probe_mode));
    if(touch_probe_mode>1) {printf("A135F TOUCH: invalid probe mode; disabled\n");return;}
    (void)PE_parse_boot_argn("touch_inline_cont", &touch_inline_cont, sizeof(touch_inline_cont));
    if(touch_inline_cont>1) {printf("A135F TOUCH: invalid inline framing mode; disabled\n");return;}
    if(touch_inline_cont)printf("A135F TOUCH: inline continuation framing enabled, bounded to FIFO 63\n");
    (void)PE_parse_boot_argn("a135f_touch_bdelay", &touch_byte_delay_us, sizeof(touch_byte_delay_us));
    if(touch_byte_delay_us>100) {
        printf("A135F TOUCH: byte delay exceeds 100 us; disabled\n");
        return;
    }
    printf("A135F TOUCH: external SPI byte delay %u us\n",touch_byte_delay_us);
	if (!enabled) {
		printf("A135F TOUCH: disabled (a135f_touch=0)\n");
		return;
	}
	spi = ml_io_map(SPI_PA, 0x1000);
	cmu = ml_io_map(CMU_CMGP_PA, 0x8000);
	sysreg = ml_io_map(SYSREG_CMGP_PA, 0x4000);
	pinc = ml_io_map(PINCTRL_CMGP_PA, 0x1000);
	alive = ml_io_map(PINCTRL_ALIVE_PA, 0x1000);
	if (!spi || !cmu || !sysreg || !pinc || !alive) {
		printf("A135F TOUCH: MMIO mapping failed\n");
		return;
	}
	spi_hw_init();
	touch_bus_snapshot("initialized");
	if (kernel_thread_start(touch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* !A135F_TOUCH_TEST */
