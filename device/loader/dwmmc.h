/*
 * Reading the A21s's eMMC from exyloader: the Synopsys DesignWare MMC controller of the Exynos 850
 * (DW MMC 0, 0x12100000, "androidboot.boot_devices=12100000.dwmmc0").
 *
 * S-Boot has powered the card, set the bus and the clock up and read `boot` off it by the time we
 * run, so this initialises nothing: it turns the controller's internal DMA off for the length of
 * our reads, empties its FIFO, issues plain single-block reads (CMD17) to the card S-Boot left in
 * the transfer state, takes the data out of the FIFO by PIO, and puts CTRL and BMOD back as they
 * were. Android resets the controller anyway when its own driver starts. Read only by
 * construction: no write command is ever sent.
 *
 * Register offsets and bit meanings as documented for the DesignWare MMC (and named in Linux's
 * drivers/mmc/host/dw_mmc.h and dw_mmc-exynos.h, which were read, not copied). Device memory with
 * the MMU off: aligned 32/64-bit accesses only.
 */
#ifndef DWMMC_H
#define DWMMC_H

#include <stdint.h>

#define DWMMC0_BASE      0x12100000ULL

#define DW_CTRL          0x000
#define DW_CTRL_RESET        (1u << 0)
#define DW_CTRL_FIFO_RESET   (1u << 1)
#define DW_CTRL_DMA_RESET    (1u << 2)
#define DW_CTRL_INT_ENABLE   (1u << 4)
#define DW_CTRL_DMA_ENABLE   (1u << 5)
#define DW_CTRL_USE_IDMAC    (1u << 25)
#define DW_PWREN         0x004
#define DW_CLKDIV        0x008
#define DW_CLKENA        0x010
#define DW_TMOUT         0x014
#define DW_CTYPE         0x018
#define DW_BLKSIZ        0x01c
#define DW_BYTCNT        0x020
#define DW_INTMASK       0x024
#define DW_CMDARG        0x028
#define DW_CMD           0x02c
#define DW_CMD_START         (1u << 31)
#define DW_CMD_USE_HOLD_REG  (1u << 29)
#define DW_CMD_PRV_DAT_WAIT  (1u << 13)
#define DW_CMD_DAT_EXP       (1u << 9)
#define DW_CMD_RESP_CRC      (1u << 8)
#define DW_CMD_RESP_EXP      (1u << 6)
#define DW_RESP0         0x030
#define DW_MINTSTS       0x040
#define DW_RINTSTS       0x044
#define DW_INT_RE            (1u << 1)
#define DW_INT_CMD_DONE      (1u << 2)
#define DW_INT_DATA_OVER     (1u << 3)
#define DW_INT_RXDR          (1u << 5)
#define DW_INT_RCRC          (1u << 6)
#define DW_INT_DCRC          (1u << 7)
#define DW_INT_RTO           (1u << 8)
#define DW_INT_DRTO          (1u << 9)
#define DW_INT_HTO           (1u << 10)
#define DW_INT_FRUN          (1u << 11)
#define DW_INT_HLE           (1u << 12)
#define DW_INT_SBE           (1u << 13)
#define DW_INT_EBE           (1u << 15)
#define DW_INT_CMD_ERR       (DW_INT_RE | DW_INT_RCRC | DW_INT_RTO | DW_INT_HLE)
#define DW_INT_DATA_ERR      (DW_INT_DCRC | DW_INT_DRTO | DW_INT_HTO | DW_INT_FRUN | DW_INT_SBE | DW_INT_EBE)
#define DW_STATUS        0x048
#define DW_STATUS_FIFO_EMPTY (1u << 2)
#define DW_STATUS_DATA_BUSY  (1u << 9)
#define DW_STATUS_FIFO_COUNT(s) (((s) >> 17) & 0x1fff)
#define DW_FIFOTH        0x04c
#define DW_VERID         0x06c
#define DW_HCON          0x070
#define DW_HCON_DATA_WIDTH(h) (((h) >> 7) & 0x7)   /* 0: 16 bits, 1: 32, 2: 64 */
#define DW_BMOD          0x080
#define DW_BMOD_DE           (1u << 7)
#define DW_DATA_OLD      0x100                     /* the FIFO before version 2.40a */
#define DW_DATA_240A     0x200
/* Exynos additions: the CIU clock selection (sample/drive phases); for the hold register */
#define EXY_CLKSEL       0x09c
#define EXY_CLKSEL64     0x0a8
#define EXY_CLKSEL_DRV_WD3(x) (((x) >> 16) & 0x7)

#define MMC_SEND_STATUS  13
#define MMC_READ_SINGLE  17

struct dwmmc {
	uint64_t base;
	uint32_t verid, hcon, width;      /* FIFO width in bytes: 2, 4 or 8 */
	uint64_t fifo;
	uint32_t hold;                    /* DW_CMD_USE_HOLD_REG when the drive phase needs it */
	uint32_t saved_ctrl, saved_bmod;
	uint32_t last_err;                /* RINTSTS of the last failure */
	uint32_t last_resp;
};

static inline uint32_t dw_rd(struct dwmmc *h, uint32_t off) { return *(volatile uint32_t *)(h->base + off); }
static inline void dw_wr(struct dwmmc *h, uint32_t off, uint32_t v) { *(volatile uint32_t *)(h->base + off) = v; }

/* A bounded wait (the timer runs at CNTFRQ); the poll callback kicks the watchdogs. */
static inline uint64_t dw_now(void) { uint64_t v; __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v)); return v; }
static inline uint64_t dw_hz(void) { uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); return f ? f : 26000000u; }

static void (*dw_poll)(void);

static int dw_wait_clear(struct dwmmc *h, uint32_t off, uint32_t mask, uint32_t ms)
{
	uint64_t end = dw_now() + dw_hz() / 1000 * ms;
	while (dw_rd(h, off) & mask) {
		if (dw_now() > end) {
			return 0;
		}
		if (dw_poll) {
			dw_poll();
		}
	}
	return 1;
}

/* Look at the controller as S-Boot left it and take it over for PIO. */
static int dwmmc_open(struct dwmmc *h, uint64_t base)
{
	h->base = base;
	h->verid = dw_rd(h, DW_VERID);
	h->hcon = dw_rd(h, DW_HCON);
	h->width = 2u << DW_HCON_DATA_WIDTH(h->hcon);   /* 0 -> 2, 1 -> 4, 2 -> 8 */
	h->fifo = base + ((h->verid & 0xffff) >= 0x240a ? DW_DATA_240A : DW_DATA_OLD);
	uint32_t clksel = dw_rd(h, h->width == 8 ? EXY_CLKSEL64 : EXY_CLKSEL);
	h->hold = EXY_CLKSEL_DRV_WD3(clksel) ? DW_CMD_USE_HOLD_REG : 0;
	h->saved_ctrl = dw_rd(h, DW_CTRL);
	h->saved_bmod = dw_rd(h, DW_BMOD);
	h->last_err = 0;

	if (!dw_wait_clear(h, DW_STATUS, DW_STATUS_DATA_BUSY, 500)) {
		h->last_err = 0xdead0001;
		return 0;
	}
	/* PIO: the internal DMA off, its descriptors untouched; then the FIFO and DMA interface emptied */
	dw_wr(h, DW_BMOD, h->saved_bmod & ~DW_BMOD_DE);
	dw_wr(h, DW_CTRL, (h->saved_ctrl & ~(DW_CTRL_USE_IDMAC | DW_CTRL_DMA_ENABLE)) |
	    DW_CTRL_FIFO_RESET | DW_CTRL_DMA_RESET);
	if (!dw_wait_clear(h, DW_CTRL, DW_CTRL_FIFO_RESET | DW_CTRL_DMA_RESET, 100)) {
		h->last_err = 0xdead0002;
		return 0;
	}
	dw_wr(h, DW_RINTSTS, 0xffffffffu);
	return 1;
}

/* Give the controller back as it was (Android's driver resets it at probe in any case). */
static void dwmmc_close(struct dwmmc *h)
{
	dw_wait_clear(h, DW_STATUS, DW_STATUS_DATA_BUSY, 100);
	dw_wr(h, DW_CTRL, dw_rd(h, DW_CTRL) | DW_CTRL_FIFO_RESET);
	dw_wait_clear(h, DW_CTRL, DW_CTRL_FIFO_RESET, 100);
	dw_wr(h, DW_RINTSTS, 0xffffffffu);
	dw_wr(h, DW_BMOD, h->saved_bmod);
	dw_wr(h, DW_CTRL, h->saved_ctrl);
}

static int dw_command(struct dwmmc *h, uint32_t index, uint32_t arg, uint32_t flags)
{
	dw_wr(h, DW_RINTSTS, 0xffffffffu);
	dw_wr(h, DW_CMDARG, arg);
	dw_wr(h, DW_CMD, DW_CMD_START | h->hold | flags | (index & 0x3f));
	if (!dw_wait_clear(h, DW_CMD, DW_CMD_START, 100)) {
		h->last_err = 0xdead0003;
		return 0;
	}
	uint64_t end = dw_now() + dw_hz() / 10;
	uint32_t st;
	while (!((st = dw_rd(h, DW_RINTSTS)) & (DW_INT_CMD_DONE | DW_INT_CMD_ERR))) {
		if (dw_now() > end) {
			h->last_err = 0xdead0004;
			return 0;
		}
	}
	h->last_resp = dw_rd(h, DW_RESP0);
	if (st & DW_INT_CMD_ERR) {
		h->last_err = st;
		return 0;
	}
	return 1;
}

/* CMD13: the card's status register (R1); state in bits 12:9, 4 = transfer. */
static int dwmmc_status(struct dwmmc *h, uint32_t rca, uint32_t *r1)
{
	if (!dw_command(h, MMC_SEND_STATUS, rca << 16, DW_CMD_RESP_EXP | DW_CMD_RESP_CRC)) {
		return 0;
	}
	*r1 = h->last_resp;
	return 1;
}

/* CMD17, one 512-byte block at a sector address (an eMMC this size is sector addressed). */
static int dwmmc_read_block(struct dwmmc *h, uint64_t lba, void *buf)
{
	if (!dw_wait_clear(h, DW_STATUS, DW_STATUS_DATA_BUSY, 500)) {
		h->last_err = 0xdead0005;
		return 0;
	}
	dw_wr(h, DW_BLKSIZ, 512);
	dw_wr(h, DW_BYTCNT, 512);
	if (!dw_command(h, MMC_READ_SINGLE, (uint32_t)lba,
	    DW_CMD_PRV_DAT_WAIT | DW_CMD_DAT_EXP | DW_CMD_RESP_EXP | DW_CMD_RESP_CRC)) {
		return 0;
	}
	uint32_t got = 0;
	uint64_t end = dw_now() + dw_hz() / 2;
	for (;;) {
		uint32_t st = dw_rd(h, DW_RINTSTS);
		if (st & DW_INT_DATA_ERR) {
			h->last_err = st;
			return 0;
		}
		uint32_t n = DW_STATUS_FIFO_COUNT(dw_rd(h, DW_STATUS));
		while (n-- && got < 512) {
			if (h->width == 8) {
				*(uint64_t *)((uint8_t *)buf + got) = *(volatile uint64_t *)h->fifo;
			} else if (h->width == 4) {
				*(uint32_t *)((uint8_t *)buf + got) = *(volatile uint32_t *)h->fifo;
			} else {
				*(uint16_t *)((uint8_t *)buf + got) = *(volatile uint16_t *)h->fifo;
			}
			got += h->width;
		}
		dw_wr(h, DW_RINTSTS, DW_INT_RXDR);
		if (got >= 512 && (st & DW_INT_DATA_OVER)) {
			break;
		}
		if (dw_now() > end) {
			h->last_err = 0xdead0006 | (got << 16);
			return 0;
		}
		if (dw_poll) {
			dw_poll();
		}
	}
	dw_wr(h, DW_RINTSTS, 0xffffffffu);
	return 1;
}

static int dwmmc_read(struct dwmmc *h, uint64_t lba, uint32_t count, void *buf)
{
	for (uint32_t i = 0; i < count; i++) {
		if (!dwmmc_read_block(h, lba + i, (uint8_t *)buf + (uint64_t)i * 512)) {
			return 0;
		}
	}
	return 1;
}

#endif /* DWMMC_H */
