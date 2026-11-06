/*
 * The Exynos 850's eMMC (Samsung Galaxy A21s) as a block device for XNU: the Synopsys DesignWare
 * MMC controller DW MMC 0 at 0x12100000, read by PIO (CMD17, optionally CMD23 + CMD18). It is the
 * A21s loader's own reader (hakios-a21s/loader/dwmmc.h, which works on the phone) moved into the
 * kernel, and it is what lets the iOS system image -- too big for the 2 GiB XNU has there -- be
 * the root: read off the eMMC as needed.
 *
 *   rd=emmc0                       the root is /dev/emmc0 (IOFindBSDRoot asks hakios_dwmmc_root())
 *   emmc_lba=N emmc_sectors=M      the window of the eMMC that is /dev/emmc0: the system image in
 *                                  the free tail of `super` (hakios-a21s/tools/xnusys.py plan)
 *   wdt_run=S                      keep the loader's watchdog fed for S seconds, then let it reset
 *                                  the phone back to Android with this run's log (0: do not feed)
 *   emmc_multi=1                   compare 64 KiB with CMD17, then use bounded CMD18 reads
 *   emmc_write_multi=1             bounded CMD23 + CMD25 writes inside the verified var volume
 *   emmc_write=1                   permit writes ONLY to the separately checked /dev/emmc1
 *   emmc_var_lba=N emmc_var_sectors=M  MiB-aligned HFS+ window after APFS, inside the root room
 *   var_hfs=emmc1                  stage0 mounts this journaled data volume on /private/var
 *
 * S-Boot leaves the card powered, clocked and in the transfer state; nothing is initialised here.
 * The controller's internal DMA is turned off for PIO and stays off: XNU is alone on the phone.
 *
 * emmc0 remains read only. Writes require explicit emmc_write=1, a disjoint
 * window, verified user-area capacity and an existing journaled HFS+ volume.
 * There is no whole-card node, erase command or write path to any other region.
 *
 * Register offsets and bit meanings as documented for the DesignWare MMC and named in Linux's
 * drivers/mmc/host/dw_mmc.h and dw_mmc-exynos.h (read, not copied).
 */

#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/kalloc.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/hakios.dwmmc.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void IOSleep(unsigned int milliseconds);

#define DWMMC0_BASE      0x12100000ULL
#define DWMMC0_SIZE      0x1000

#define DW_CTRL          0x000
#define DW_CTRL_FIFO_RESET   (1u << 1)
#define DW_CTRL_DMA_RESET    (1u << 2)
#define DW_CTRL_INT_ENABLE   (1u << 4)
#define DW_CTRL_DMA_ENABLE   (1u << 5)
#define DW_CTRL_USE_IDMAC    (1u << 25)
#define DW_BLKSIZ        0x01c
#define DW_BYTCNT        0x020
#define DW_INTMASK       0x024
#define DW_CMDARG        0x028
#define DW_CMD           0x02c
#define DW_CMD_START         (1u << 31)
#define DW_CMD_USE_HOLD_REG  (1u << 29)
#define DW_CMD_PRV_DAT_WAIT  (1u << 13)
#define DW_CMD_STOP          (1u << 14)
#define DW_CMD_DAT_WRITE     (1u << 10)
#define DW_CMD_DAT_EXP       (1u << 9)
#define DW_CMD_RESP_CRC      (1u << 8)
#define DW_CMD_RESP_EXP      (1u << 6)
#define DW_RESP0         0x030
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
#define DW_STATUS_DATA_BUSY  (1u << 9)
#define DW_STATUS_FIFO_COUNT(s) (((s) >> 17) & 0x1fff)
#define DW_VERID         0x06c
#define DW_HCON          0x070
#define DW_HCON_DATA_WIDTH(h) (((h) >> 7) & 0x7)
#define DW_BMOD          0x080
#define DW_BMOD_DE           (1u << 7)
#define DW_DATA_OLD      0x100
#define DW_DATA_240A     0x200
#define EXY_CLKSEL       0x09c
#define EXY_CLKSEL64     0x0a8
#define EXY_CLKSEL_DRV_WD3(x) (((x) >> 16) & 0x7)

#define MMC_SEND_STATUS  13
#define MMC_READ_SINGLE  17
#define MMC_READ_MULTIPLE 18
#define MMC_SET_COUNT    23
#define MMC_STOP         12
#define MMC_WRITE_SINGLE 24
#define MMC_WRITE_MULTIPLE 25
#define MMC_SEND_EXT_CSD 8
#define MMC_SWITCH       6
#define MMC_R1_ERRORS    0xffff8080u /* card errors/locked/switch error; not current state */

#define SECTOR           512u
#define TRIES            3              /* a block that fails is asked for again, this many times */

/* The loader's watchdog (exyloader step 4: cluster 0's, about 82 s from full) */
#define WDT_CL0          0x10050000ULL
#define WTCON            0x00
#define WTDAT            0x04
#define WTCNT            0x08
#define WTCON_ENABLE     (1u << 5)

static LCK_GRP_DECLARE(dw_lck_grp, "hakios-dwmmc");
static LCK_MTX_DECLARE(dw_lock, &dw_lck_grp);

static struct {
	vm_offset_t base;
	vm_offset_t fifo;
	uint32_t    width;              /* FIFO width in bytes: 2, 4 or 8 */
	uint32_t    hold;
	uint64_t    lba, sectors;       /* the window */
	uint32_t    last_err;
	uint64_t    reads, errors, retries;
	uint64_t    read_starvations; /* HTO with no data-integrity error */
	uint64_t    bytes, ticks;       /* for the read speed in the log */
} dw;

static bool dw_ready;
static uint32_t dw_multiblock;      /* emmc_multi=1, checked against CMD17 before use */
static bool dw_var_ready, dw_cache_enabled;
static uint32_t dw_write_multiblock; /* explicit emmc_write_multi=1, separate from read probe */
static uint64_t dw_var_lba, dw_var_sectors, dw_writes;
static int dw_bmajor = -1, dw_cmajor = -1;

static inline uint32_t dw_rd(uint32_t off) { return *(volatile uint32_t *)(dw.base + off); }
static inline void dw_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(dw.base + off) = v; }

static inline uint64_t
dw_now(void)
{
	/* CNTPCT was non-monotonic in run 0015; XNU mach time remained valid.
	 * Use the same calibrated clock as the scheduler for both PIO deadlines
	 * and throughput, expressed in nanoseconds. */
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

static inline uint64_t
dw_hz(void)
{
	return NSEC_PER_SEC;
}

static bool
dw_wait_clear(uint32_t off, uint32_t mask, uint32_t ms)
{
	uint64_t end = dw_now() + dw_hz() / 1000 * ms;

	while (dw_rd(off) & mask) {
		if (dw_now() > end) {
			return false;
		}
	}
	return true;
}

static bool
dw_command(uint32_t index, uint32_t arg, uint32_t flags)
{
	uint64_t end;
	uint32_t st;

	dw_wr(DW_RINTSTS, 0xffffffffu);
	dw_wr(DW_CMDARG, arg);
	dw_wr(DW_CMD, DW_CMD_START | dw.hold | flags | (index & 0x3f));
	if (!dw_wait_clear(DW_CMD, DW_CMD_START, 100)) {
		dw.last_err = 0xdead0003;
		return false;
	}
	end = dw_now() + dw_hz() / 10;
	while (!((st = dw_rd(DW_RINTSTS)) & (DW_INT_CMD_DONE | DW_INT_CMD_ERR))) {
		if (dw_now() > end) {
			dw.last_err = 0xdead0004;
			return false;
		}
	}
	if (st & DW_INT_CMD_ERR) {
		dw.last_err = st;
		return false;
	}
	if (dw_rd(DW_RESP0) & MMC_R1_ERRORS) {
		dw.last_err = dw_rd(DW_RESP0);
		return false;
	}
	return true;
}

/*
 * The controller's own reset, for when emptying the FIFO is not enough: on 2026-11-05 (run 0016)
 * a CMD18 ended in HTO, and from then on every command stayed with START set (0xdead0003): the
 * command engine was stuck, 38 reads failed and iOS waited for them for good. What Linux's
 * dw_mmc does then (read, not copied): CTRL reset of the controller, FIFO and DMA interface; an
 * "update clock registers" command so the card clock runs again -- CLKDIV/CLKENA themselves are
 * not touched, S-Boot's values stay -- and a STOP, so a card left in the data state goes back to
 * transfer. Nothing here can write to the card's data.
 */
#define DW_CTRL_RESET        (1u << 0)
#define DW_CMD_UPD_CLK       (1u << 21)
static uint64_t dw_hard_recoveries;

static void
dw_hard_recover(void)
{
	uint32_t ctrl = dw_rd(DW_CTRL) & ~(DW_CTRL_RESET | DW_CTRL_FIFO_RESET | DW_CTRL_DMA_RESET);

	dw_hard_recoveries++;
	dw_wr(DW_CTRL, ctrl | DW_CTRL_RESET | DW_CTRL_FIFO_RESET | DW_CTRL_DMA_RESET);
	(void)dw_wait_clear(DW_CTRL, DW_CTRL_RESET | DW_CTRL_FIFO_RESET | DW_CTRL_DMA_RESET, 200);
	dw_wr(DW_CTRL, ctrl);
	dw_wr(DW_INTMASK, 0);
	dw_wr(DW_RINTSTS, 0xffffffffu);
	dw_wr(DW_CMDARG, 0);
	dw_wr(DW_CMD, DW_CMD_START | DW_CMD_UPD_CLK | DW_CMD_PRV_DAT_WAIT);
	bool clock_ok = dw_wait_clear(DW_CMD, DW_CMD_START, 200);
	dw_wr(DW_RINTSTS, 0xffffffffu);
	/* STOP without PRV_DAT_WAIT; its answer does not matter: in transfer state it is refused */
	(void)dw_command(MMC_STOP, 0, DW_CMD_STOP | DW_CMD_RESP_EXP | DW_CMD_RESP_CRC);
	(void)dw_wait_clear(DW_STATUS, DW_STATUS_DATA_BUSY, 1000);
	dw_wr(DW_CTRL, dw_rd(DW_CTRL) | DW_CTRL_FIFO_RESET);
	(void)dw_wait_clear(DW_CTRL, DW_CTRL_FIFO_RESET, 100);
	dw_wr(DW_RINTSTS, 0xffffffffu);
	if (dw_hard_recoveries <= 8) {
		printf("emmc0: controller reset #%llu (last error %#x): clock update %s, CMD %#x STATUS %#x\n",
		    dw_hard_recoveries, dw.last_err, clock_ok ? "done" : "STUCK", dw_rd(DW_CMD), dw_rd(DW_STATUS));
	}
}

/* Empty the FIFO and clear the interrupts after a failed transfer, before the next try. */
static void
dw_recover(void)
{
	bool idle = dw_wait_clear(DW_STATUS, DW_STATUS_DATA_BUSY, 500);
	dw_wr(DW_CTRL, dw_rd(DW_CTRL) | DW_CTRL_FIFO_RESET);
	bool fifo = dw_wait_clear(DW_CTRL, DW_CTRL_FIFO_RESET, 100);
	dw_wr(DW_RINTSTS, 0xffffffffu);
	/* a command engine that never took the last command, or a FIFO/data path that stays busy */
	if (!idle || !fifo || (dw_rd(DW_CMD) & DW_CMD_START) ||
	    dw.last_err == 0xdead0003 || dw.last_err == 0xdead0004) {
		dw_hard_recover();
	}
}

/* A bounded PIO receive. CMD23 makes CMD18 stop at exactly BYTCNT, without auto-stop races. */
static bool
dw_receive(uint32_t cmd, uint32_t arg, uint32_t count, uint8_t *buf)
{
	uint32_t got = 0, starved = 0;
	uint32_t bytes = count * SECTOR;
	uint64_t end;

	if (!dw_wait_clear(DW_STATUS, DW_STATUS_DATA_BUSY, 500)) {
		dw.last_err = 0xdead0005;
		return false;
	}
	dw_wr(DW_BLKSIZ, SECTOR);
	dw_wr(DW_BYTCNT, bytes);
	if (!dw_command(cmd, arg,
	    DW_CMD_PRV_DAT_WAIT | DW_CMD_DAT_EXP | DW_CMD_RESP_EXP | DW_CMD_RESP_CRC)) {
		return false;
	}
	end = dw_now() + dw_hz() * 2;
	for (;;) {
		uint32_t st = dw_rd(DW_RINTSTS);
		uint32_t n;

		/* HTO alone means the host left the receive FIFO full, stopping
		 * card clock. Drain the existing transfer to release that condition;
		 * sending STOP first can leave the command engine waiting for clock.
		 * CRC, end/start bit, overrun and card timeout remain fatal. The
		 * original software deadline is never extended by repeated HTO. */
		if (st & (DW_INT_DATA_ERR & ~DW_INT_HTO)) {
			dw.last_err = st;
			return false;
		}
		n = DW_STATUS_FIFO_COUNT(dw_rd(DW_STATUS));
		while (n-- && got < bytes) {
			if (dw.width == 8) {
				uint64_t v = *(volatile uint64_t *)dw.fifo;
				memcpy(buf + got, &v, 8);
			} else if (dw.width == 4) {
				uint32_t v = *(volatile uint32_t *)dw.fifo;
				memcpy(buf + got, &v, 4);
			} else {
				uint16_t v = *(volatile uint16_t *)dw.fifo;
				memcpy(buf + got, &v, 2);
			}
			got += dw.width;
		}
		if (st & DW_INT_HTO) {
			dw.read_starvations++;
			starved++;
		}
		dw_wr(DW_RINTSTS, DW_INT_RXDR | (st & DW_INT_HTO));
		if (got >= bytes && (st & DW_INT_DATA_OVER)) {
			break;
		}
		if (dw_now() > end) {
			dw.last_err = 0xdead0006 | (got << 16);
			return false;
		}
	}
	dw_wr(DW_RINTSTS, 0xffffffffu);
	/* Printing while the FIFO needs service could cause another starvation. */
	if (starved && dw.read_starvations <= 4) {
		printf("emmc0: resumed %u read HTO events, CMD%u LBA %u completed %u bytes, TMOUT %#x\n",
		    starved, cmd, arg, bytes, dw_rd(0x014));
	}
	return true;
}

static bool
dw_read_block(uint64_t lba, uint8_t *buf)
{
	return dw_receive(MMC_READ_SINGLE, (uint32_t)lba, 1, buf);
}

static bool
dw_stop(void)
{
	/* STOP must bypass PRV_DAT_WAIT: the very transfer being stopped may still be busy. */
	bool ok = dw_command(MMC_STOP, 0, DW_CMD_STOP | DW_CMD_RESP_EXP | DW_CMD_RESP_CRC);
	if (!dw_wait_clear(DW_STATUS, DW_STATUS_DATA_BUSY, 1000)) {
		ok = false;
	}
	dw_recover();
	return ok;
}

static bool
dw_read_blocks(uint64_t lba, uint32_t count, uint8_t *buf, bool *started)
{
	*started = false;
	if (!dw_command(MMC_SET_COUNT, count, DW_CMD_PRV_DAT_WAIT | DW_CMD_RESP_EXP | DW_CMD_RESP_CRC)) {
		return false;
	}
	*started = true;
	return dw_receive(MMC_READ_MULTIPLE, (uint32_t)lba, count, buf);
}

/*
 * ---- reads through the controller's internal DMA (IDMAC), emmc_dma=1 ----
 *
 * Imported, with the author's permission, from the ios-a135f port's bsd/dev/a135f_mmc.c (Schoosh /
 * simpleXNU, Galaxy A13, the same Exynos 850): the descriptor ring and a 1 MiB bounce buffer in a window
 * below 4 GiB that XNU never uses (emmc_dma_base=, default 0xc8000000, Android's RAM while XNU runs),
 * mapped as device memory -- so no cache maintenance -- and copied with aligned 64-bit accesses only.
 * HakiOS keeps it to reads: checked against PIO at start (dw_probe_dma), any failure turns it off for
 * the rest of the run and resets the controller; writes stay PIO.
 */
#define DW_PLDMND        0x084
#define DW_DBADDR        0x088          /* 64-bit IDMAC: DBADDRL 0x88 / DBADDRU 0x8c */
#define DW_DBADDRU       0x08c
#define DW_BMOD_SWRESET      (1u << 0)
#define DW_BMOD_FB           (1u << 1)
#define DW_HCON_ADDR64(h)    (((h) >> 27) & 1u)
#define DES0_DIC             (1u << 1)
#define DES0_LD              (1u << 2)
#define DES0_FS              (1u << 3)
#define DES0_CH              (1u << 4)
#define DES0_OWN             (1u << 31)
#define DMA_BOUNCE_SIZE      (1u << 20)
#define DMA_DESC_CHUNK       4096u      /* the descriptor's buffer-size field is 13 bits */
#define DMA_DESC_OFF         0x0000     /* 256 descriptors x 32 bytes (64-bit layout) = 8 KiB */
#define DMA_BOUNCE_OFF       0x2000
#define DMA_WINDOW           (DMA_BOUNCE_OFF + DMA_BOUNCE_SIZE)

static uint32_t dw_dma;                 /* emmc_dma=1 and checked against PIO */
static bool dw_dma64;
static uint64_t dw_dma_pa;
static vm_offset_t dw_dma_va;
static uint64_t dw_dma_reads, dw_dma_failures;

static void
dw_dma_desc(uint32_t idx, uint64_t buf, uint32_t size, bool first, bool last)
{
	uint32_t d0 = DES0_OWN | DES0_CH | (first ? DES0_FS : 0) | (last ? DES0_LD : 0) | (last ? 0 : DES0_DIC);
	volatile uint32_t *d;

	if (dw_dma64) {
		uint64_t next = dw_dma_pa + DMA_DESC_OFF + (uint64_t)(idx + 1) * 32;
		d = (volatile uint32_t *)(dw_dma_va + DMA_DESC_OFF + idx * 32);
		d[1] = 0; d[2] = size; d[3] = 0;
		d[4] = (uint32_t)buf; d[5] = (uint32_t)(buf >> 32);
		d[6] = (uint32_t)next; d[7] = (uint32_t)(next >> 32);
	} else {
		uint32_t next = (uint32_t)(dw_dma_pa + DMA_DESC_OFF + (uint64_t)(idx + 1) * 16);
		d = (volatile uint32_t *)(dw_dma_va + DMA_DESC_OFF + idx * 16);
		d[1] = size; d[2] = (uint32_t)buf; d[3] = next;
	}
	__builtin_arm_dsb(0xf);
	d[0] = d0;
}

/* count sectors at absolute lba into buf by IDMAC (CMD23 + CMD18, or CMD17 for one) */
static bool
dw_dma_read(uint64_t lba, uint32_t count, uint8_t *buf)
{
	uint32_t len = count * SECTOR, n = 0, st = 0;
	uint64_t end;
	bool ok = false;

	if (len == 0 || len > DMA_BOUNCE_SIZE || !dw_wait_clear(DW_STATUS, DW_STATUS_DATA_BUSY, 500)) {
		return false;
	}
	if (count > 1 && !dw_command(MMC_SET_COUNT, count, DW_CMD_PRV_DAT_WAIT | DW_CMD_RESP_EXP | DW_CMD_RESP_CRC)) {
		return false;
	}
	dw_wr(DW_CTRL, dw_rd(DW_CTRL) | DW_CTRL_FIFO_RESET | DW_CTRL_DMA_RESET);
	(void)dw_wait_clear(DW_CTRL, DW_CTRL_FIFO_RESET | DW_CTRL_DMA_RESET, 100);
	dw_wr(DW_BMOD, dw_rd(DW_BMOD) | DW_BMOD_SWRESET);
	for (uint32_t done = 0; done < len; done += DMA_DESC_CHUNK, n++) {
		uint32_t chunk = MIN(DMA_DESC_CHUNK, len - done);
		dw_dma_desc(n, dw_dma_pa + DMA_BOUNCE_OFF + done, chunk, done == 0, done + chunk >= len);
	}
	__builtin_arm_dsb(0xf);
	dw_wr(DW_DBADDR, (uint32_t)(dw_dma_pa + DMA_DESC_OFF));
	if (dw_dma64) {
		dw_wr(DW_DBADDRU, (uint32_t)((dw_dma_pa + DMA_DESC_OFF) >> 32));
	}
	dw_wr(DW_CTRL, dw_rd(DW_CTRL) | DW_CTRL_USE_IDMAC | DW_CTRL_DMA_ENABLE);
	dw_wr(DW_RINTSTS, 0xffffffffu);
	dw_wr(DW_BLKSIZ, SECTOR);
	dw_wr(DW_BYTCNT, len);
	dw_wr(DW_BMOD, DW_BMOD_DE | DW_BMOD_FB);
	dw_wr(DW_PLDMND, 1);
	if (dw_command(count > 1 ? MMC_READ_MULTIPLE : MMC_READ_SINGLE, (uint32_t)lba,
	    DW_CMD_PRV_DAT_WAIT | DW_CMD_DAT_EXP | DW_CMD_RESP_EXP | DW_CMD_RESP_CRC)) {
		end = dw_now() + dw_hz() * 2;
		for (;;) {
			st = dw_rd(DW_RINTSTS);
			if (st & DW_INT_DATA_ERR) {
				dw.last_err = st;
				break;
			}
			if (st & DW_INT_DATA_OVER) {
				ok = true;
				break;
			}
			if (dw_now() > end) {
				dw.last_err = 0xdead0009;
				break;
			}
		}
	}
	/* IDMAC off again: everything else in this driver is PIO */
	dw_wr(DW_BMOD, DW_BMOD_FB);
	dw_wr(DW_CTRL, dw_rd(DW_CTRL) & ~(DW_CTRL_USE_IDMAC | DW_CTRL_DMA_ENABLE));
	dw_wr(DW_RINTSTS, 0xffffffffu);
	if (!ok) {
		return false;
	}
	volatile const uint64_t *s = (volatile const uint64_t *)(dw_dma_va + DMA_BOUNCE_OFF);
	for (uint32_t i = 0; i < len / 8; i++) {
		uint64_t v = s[i];
		memcpy(buf + 8 * i, &v, 8);
	}
	dw_dma_reads++;
	return true;
}

/* count sectors from sector `first` of the window, under dw_lock */
static int
dw_read_at(uint64_t base, uint64_t sectors, uint64_t first, uint64_t count, uint8_t *buf)
{
	uint64_t t0 = dw_now();

	if (first >= sectors || count > sectors - first || base > UINT32_MAX ||
	    sectors > (uint64_t)UINT32_MAX + 1 - base) {
		return EINVAL;                  /* never outside the window */
	}
	for (uint64_t i = 0; i < count;) {
		int t;
		uint32_t chunk = (uint32_t)((count - i) > 256 ? 256 : count - i);
		if (dw_dma) {
			if (dw_dma_read(base + first + i, chunk, buf + i * SECTOR)) {
				i += chunk;
				continue;
			}
			dw_dma = 0;
			dw_dma_failures++;
			printf("emmc0: IDMAC read failed (err %#x): PIO from here on\n", dw.last_err);
			dw_hard_recover();
		}
		if (dw_multiblock && chunk > 1) {
			bool started;
			if (dw_read_blocks(base + first + i, chunk, buf + i * SECTOR, &started)) {
				i += chunk;
				continue;
			}
			dw.retries++;
			dw_multiblock = 0;
			printf("emmc0: CMD23/18 failed (err %#x), recovering before CMD17 fallback\n", dw.last_err);
			if (started && !dw_stop()) {
				dw.errors++;
				return EIO;
			}
			if (!started) {
				dw_recover();
			}
		}

		for (t = 0; t < TRIES; t++) {
			if (dw_read_block(base + first + i, buf + i * SECTOR)) {
				break;
			}
			dw.retries++;
			dw_recover();
		}
		if (t == TRIES) {
			dw.errors++;
			printf("emmc0: sector %llu (eMMC %llu) failed %d times, RINTSTS/err %#x\n",
			    first + i, base + first + i, TRIES, dw.last_err);
			return EIO;
		}
		i++;
	}
	dw.reads++;
	dw.bytes += count * SECTOR;
	dw.ticks += dw_now() - t0;
	return 0;
}

static int
dw_read(uint64_t first, uint64_t count, uint8_t *buf)
{
	return dw_read_at(dw.lba, dw.sectors, first, count, buf);
}

/* S-Boot handed over RCA 1. Require both READY_FOR_DATA and state TRANSFER. */
static bool
dw_card_ready(void)
{
	uint64_t end = dw_now() + dw_hz() * 5;
	do {
		if (!dw_command(MMC_SEND_STATUS, 1u << 16, DW_CMD_RESP_EXP | DW_CMD_RESP_CRC)) {
			return false;
		}
		uint32_t r1 = dw_rd(DW_RESP0);
		if ((r1 & (1u << 8)) && ((r1 >> 9) & 15) == 4) {
			return true;
		}
	} while (dw_now() < end);
	dw.last_err = 0xdead0007;
	return false;
}

static int
dw_flush(void)
{
	if (!dw_var_ready) {
		return EROFS;
	}
	/* EXT_CSD FLUSH_CACHE, write-byte access; never change CACHE_CTRL or timing. */
	if (dw_cache_enabled && !dw_command(MMC_SWITCH, 0x03200100u,
	    DW_CMD_PRV_DAT_WAIT | DW_CMD_RESP_EXP | DW_CMD_RESP_CRC)) {
		return EIO;
	}
	return dw_card_ready() ? 0 : EIO;
}

/* CMD23 bounds CMD25 to at most 128 KiB. A failed/partial write is never
 * replayed as CMD24: the journal must see EIO, not a hidden uncertain retry.
 * FIFO access retains the conservative empty-before-word behavior.
 */
static bool
dw_write_blocks(uint64_t lba, uint32_t count, const uint8_t *buf, bool *started)
{
	uint32_t sent = 0, bytes = count * SECTOR;
	uint64_t end;
	*started = false;
	if (count == 0 || count > 256 || lba > UINT32_MAX || count > (uint64_t)UINT32_MAX + 1 - lba) {
		return false;
	}
	if (!dw_wait_clear(DW_STATUS, DW_STATUS_DATA_BUSY, 1000)) {
		return false;
	}
	if (count > 1 && !dw_command(MMC_SET_COUNT, count,
	    DW_CMD_PRV_DAT_WAIT | DW_CMD_RESP_EXP | DW_CMD_RESP_CRC)) {
		return false;
	}
	dw_wr(DW_BLKSIZ, SECTOR);
	dw_wr(DW_BYTCNT, bytes);
	*started = true;
	if (!dw_command(count > 1 ? MMC_WRITE_MULTIPLE : MMC_WRITE_SINGLE, (uint32_t)lba, DW_CMD_PRV_DAT_WAIT | DW_CMD_DAT_EXP |
	    DW_CMD_DAT_WRITE | DW_CMD_RESP_EXP | DW_CMD_RESP_CRC)) {
		return false;
	}
	end = dw_now() + dw_hz() * 5;
	for (;;) {
		uint32_t st = dw_rd(DW_RINTSTS);
		if (st & DW_INT_DATA_ERR) {
			dw.last_err = st;
			return false;
		}
		if (sent < bytes && DW_STATUS_FIFO_COUNT(dw_rd(DW_STATUS)) == 0) {
			if (dw.width == 8) {
				uint64_t v;
				memcpy(&v, buf + sent, 8);
				*(volatile uint64_t *)dw.fifo = v;
			} else if (dw.width == 4) {
				uint32_t v;
				memcpy(&v, buf + sent, 4);
				*(volatile uint32_t *)dw.fifo = v;
			} else {
				uint16_t v;
				memcpy(&v, buf + sent, 2);
				*(volatile uint16_t *)dw.fifo = v;
			}
			sent += dw.width;
		}
		if (sent == bytes && (st & DW_INT_DATA_OVER)) {
			break;
		}
		if (dw_now() > end) {
			dw.last_err = 0xdead0008;
			return false;
		}
	}
	dw_wr(DW_RINTSTS, 0xffffffffu);
	return dw_card_ready();
}

/* The ONLY data-write entry: relative to emmc1, never the root or a raw disk. */
static int
dw_write(uint64_t first, uint64_t count, const uint8_t *buf)
{
	if (!dw_var_ready) {
		return EROFS;
	}
	if (first >= dw_var_sectors || count > dw_var_sectors - first ||
	    dw_var_lba > UINT32_MAX || dw_var_sectors > (uint64_t)UINT32_MAX + 1 - dw_var_lba) {
		return EINVAL;
	}
	if (count == 0) {
		return 0;
	}
	for (uint64_t i = 0; i < count;) {
		uint32_t chunk = dw_write_multiblock ? (uint32_t)(count - i > 256 ? 256 : count - i) : 1;
		bool started;
		if (!dw_write_blocks(dw_var_lba + first + i, chunk, buf + i * SECTOR, &started)) {
			dw.errors++;
			if (started) {
				(void)dw_stop();
			}
			return EIO; /* no blind retry of a write with uncertain completion */
		}
		dw_writes += chunk;
		i += chunk;
	}
	/* Write-through even when a filesystem omits a cache synchronization ioctl. */
	return dw_flush();
}

static bool
dw_var_valid(uint64_t image_sectors)
{
	/* One MiB after the rounded APFS end; leave the payload outside the root room. */
	if (image_sectors < 16 || image_sectors > dw.sectors ||
	    dw.lba > UINT32_MAX || dw.sectors > (uint64_t)UINT32_MAX + 1 - dw.lba) {
		return false;
	}
	uint64_t first = ((dw.lba + image_sectors + 2047) & ~2047ULL) + 2048;
	uint64_t end = dw.lba + dw.sectors;
	return dw_var_lba >= first && (dw_var_lba & 2047) == 0 &&
	       dw_var_lba < end && dw_var_sectors >= 131072 &&
	       (dw_var_sectors & 2047) == 0 && dw_var_sectors <= end - dw_var_lba;
}

/* ---- the block device ---- */

static int
dw_dev_open(dev_t dev, int flags, __unused int devtype, __unused proc_t p)
{
	if (!dw_ready || minor(dev) > 1 || (minor(dev) == 1 && !dw_var_ready)) {
		return ENXIO;
	}
	return (flags & FWRITE) && minor(dev) != 1 ? EROFS : 0;
}

static int
dw_dev_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

static void
dw_dev_strategy(buf_t bp)
{
	uint64_t offset, len;
	unsigned int unit = minor(buf_device(bp));
	bool reading = (buf_flags(bp) & B_READ) != 0;
	uint64_t sectors = unit == 1 ? dw_var_sectors : dw.sectors;
	caddr_t mapped;
	int error;

	if (!dw_ready || unit > 1 || (unit == 1 && !dw_var_ready)) {
		buf_seterror(bp, ENXIO);
		buf_biodone(bp);
		return;
	}
	if (!reading && unit != 1) {
		buf_seterror(bp, EROFS);
		buf_biodone(bp);
		return;
	}
	/* buf_blkno is in 512-byte device blocks. Reject negatives before multiplying. */
	if (buf_blkno(bp) < 0 || (uint64_t)buf_blkno(bp) > UINT64_MAX / SECTOR) {
		buf_seterror(bp, EINVAL);
		buf_biodone(bp);
		return;
	}
	offset = (uint64_t)buf_blkno(bp) * SECTOR;
	len = buf_count(bp);
	if (len == 0) {
		buf_setresid(bp, 0);
		buf_biodone(bp);
		return;
	}
	if (offset >= sectors * SECTOR) {
		buf_seterror(bp, reading && offset == sectors * SECTOR ? 0 : EINVAL);
		buf_setresid(bp, buf_count(bp));
		buf_biodone(bp);
		return;
	}
	if (!reading && ((len % SECTOR) || len > sectors * SECTOR - offset)) {
		buf_seterror(bp, EINVAL); /* never truncate a write or modify a partial sector */
		buf_biodone(bp);
		return;
	}
	if (len > sectors * SECTOR - offset) {
		len = sectors * SECTOR - offset;
	}
	if ((error = buf_map(bp, &mapped)) != 0) {
		buf_seterror(bp, error);
		buf_biodone(bp);
		return;
	}
	lck_mtx_lock(&dw_lock);
	{
		uint64_t whole = len / SECTOR, rest = len % SECTOR;

		uint64_t base = unit == 1 ? dw_var_lba : dw.lba;
		error = !reading ? dw_write(offset / SECTOR, whole, (uint8_t *)mapped) :
		    whole ? dw_read_at(base, sectors, offset / SECTOR, whole, (uint8_t *)mapped) : 0;
		if (reading && error == 0 && rest) {
			uint8_t tail[SECTOR];

			error = dw_read_at(base, sectors, offset / SECTOR + whole, 1, tail);
			if (error == 0) {
				memcpy((uint8_t *)mapped + whole * SECTOR, tail, (size_t)rest);
			}
		}
	}
	lck_mtx_unlock(&dw_lock);
	buf_setresid(bp, (uint32_t)(buf_count(bp) - (error ? 0 : len)));
	buf_unmap(bp);
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
dw_dev_ioctl(dev_t dev, u_long cmd, caddr_t data, __unused int fflag, __unused proc_t p)
{
	bool data_volume = minor(dev) == 1;
	if (!dw_ready || minor(dev) > 1 || (data_volume && !dw_var_ready)) {
		return ENXIO;
	}
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
	case DKIOCGETPHYSICALBLOCKSIZE:
		*(uint32_t *)data = SECTOR;
		return 0;
	case DKIOCSETBLOCKSIZE:
		return *(uint32_t *)data == SECTOR ? 0 : EINVAL;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = data_volume ? dw_var_sectors : dw.sectors;
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = data_volume ? 1 : 0;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*(uint64_t *)data = 256;        /* 128 KiB a request */
		return 0;
	case DKIOCGETMAXBLOCKCOUNTWRITE:
		*(uint64_t *)data = data_volume ? 256 : 0;
		return 0;
	case DKIOCSYNCHRONIZE:
	case DKIOCSYNCHRONIZECACHE:
		if (data_volume) {
			lck_mtx_lock(&dw_lock);
			int err = dw_flush();
			lck_mtx_unlock(&dw_lock);
			return err;
		}
		return 0;
	default:
		return ENOTTY;
	}
}

static int
dw_dev_size(__unused dev_t dev)
{
	return dw_ready ? (int)(minor(dev) == 1 ? dw_var_sectors : dw.sectors) : -1;
}

/* Raw I/O copies user pages via uiomove: never dereference a PAN-protected
 * user address, and never hold the hardware lock while a user-page fault runs.
 */
static int
dw_raw_io(dev_t dev, struct uio *uio, bool writing)
{
	unsigned int unit = minor(dev);
	uint64_t size = (unit == 1 ? dw_var_sectors : dw.sectors) * SECTOR;
	uint64_t base = unit == 1 ? dw_var_lba : dw.lba;
	int error = 0;
	if (!dw_ready || unit > 1 || (unit == 1 && !dw_var_ready)) {
		return ENXIO;
	}
	if (writing && unit != 1) {
		return EROFS;
	}
	if (uio_offset(uio) < 0 || (uint64_t)uio_offset(uio) > size) {
		return EINVAL;
	}
	if (writing && ((uio_offset(uio) % SECTOR) || (uio_resid(uio) % SECTOR) ||
	    (uint64_t)uio_resid(uio) > size - (uint64_t)uio_offset(uio))) {
		return EINVAL;
	}
	uint8_t *bounce = kalloc_data(65536, Z_WAITOK);
	if (bounce == NULL) {
		return ENOMEM;
	}
	while (!error && uio_resid(uio) > 0 && (uint64_t)uio_offset(uio) < size) {
		uint64_t at = (uint64_t)uio_offset(uio), skip = at % SECTOR;
		uint64_t n = MIN((uint64_t)uio_resid(uio), 65536 - skip);
		n = MIN(n, size - at);
		if (writing) {
			error = uiomove((caddr_t)bounce, (int)n, uio);
		}
		if (!error) {
			lck_mtx_lock(&dw_lock);
			error = writing ? dw_write(at / SECTOR, n / SECTOR, bounce) :
			    dw_read_at(base, size / SECTOR, at / SECTOR, (skip + n + SECTOR - 1) / SECTOR, bounce);
			lck_mtx_unlock(&dw_lock);
		}
		if (!writing && !error) {
			error = uiomove((caddr_t)bounce + skip, (int)n, uio);
		}
	}
	kfree_data(bounce, 65536);
	return error;
}

static int
dw_dev_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return dw_raw_io(dev, uio, false);
}

static int
dw_dev_write(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return dw_raw_io(dev, uio, true);
}

static const struct bdevsw dw_bdevsw = {
	.d_open     = dw_dev_open,
	.d_close    = dw_dev_close,
	.d_strategy = dw_dev_strategy,
	.d_ioctl    = dw_dev_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = dw_dev_size,
	.d_type     = D_DISK,
};

static const struct cdevsw dw_cdevsw = {
	.d_open       = dw_dev_open,
	.d_close      = dw_dev_close,
	.d_read       = dw_dev_read,
	.d_write      = dw_dev_write,
	.d_ioctl      = dw_dev_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

/* ---- the watchdog the loader left running (wdt_run=S) ---- */

static uint32_t wdt_run;
static vm_offset_t wdt_base;

static void
dw_wdt_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint64_t start = dw_now(), hz = dw_hz(), t0 = mach_absolute_time(), ns;

	/*
	 * Counted in sleeps of a second, not by reading a counter: on 2026-11-05 the counter's
	 * difference said 240 s 47 ms after the start, and the watchdog was left to fire at once.
	 * Both clocks are printed at the end so that the next run says which one was wrong.
	 */
	for (uint64_t s = 0;; s++) {
		if (s >= wdt_run) {
			absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
			printf("emmc0: %u s up: no more feeding the watchdog, back to Android in about 80 s "
			    "(%llu reads, %llu MB, %llu retries, %llu errors; clock Hz %llu, monotonic elapsed %llu s, "
			    "mach time %llu s)\n",
			    wdt_run, dw.reads, dw.bytes >> 20, dw.retries, dw.errors, hz, (dw_now() - start) / hz,
			    ns / NSEC_PER_SEC);
			return;
		}
		if (*(volatile uint32_t *)(wdt_base + WTCON) & WTCON_ENABLE) {
			*(volatile uint32_t *)(wdt_base + WTCNT) = *(volatile uint32_t *)(wdt_base + WTDAT);
		}
		if (s % 60 == 59) {
			uint64_t ms = dw.ticks / (hz / 1000);

			printf("emmc0: %llu s: %llu reads, %llu MB in %llu ms (%llu KiB/s), %llu retries, %llu errors; CMD18 %u, var %llu sectors written, read HTO %llu, controller resets %llu, DMA %u (%llu reads, %llu failures)\n",
			    s + 1, dw.reads, dw.bytes >> 20, ms, ms ? (dw.bytes >> 10) * 1000 / ms : 0,
			    dw.retries, dw.errors, dw_multiblock, dw_writes, dw.read_starvations, dw_hard_recoveries,
			    dw_dma, dw_dma_reads, dw_dma_failures);
		}
		IOSleep(1000);
	}
}

/* ---- bringing it up ---- */

static uint8_t dw_probe_single[65536], dw_probe_multi[65536];

static void
dw_probe_multiblock(void)
{
	uint32_t wanted = 0;
	if (!PE_parse_boot_argn("emmc_multi", &wanted, sizeof(wanted)) || wanted != 1 || dw.sectors < 128) {
		return;
	}
	dw_multiblock = 0;
	uint64_t t0 = dw_now();
	int single = dw_read(0, 128, dw_probe_single);
	uint64_t t1 = dw_now();
	if (single != 0) {
		printf("emmc0: CMD17 baseline failed; CMD18 stays off\n");
		return;
	}
	dw_multiblock = 1;
	int multi = dw_read(0, 128, dw_probe_multi);
	uint64_t t2 = dw_now();
	if (multi != 0 || !dw_multiblock || memcmp(dw_probe_single, dw_probe_multi, sizeof(dw_probe_single)) != 0) {
		dw_multiblock = 0;
		printf("emmc0: CMD18 comparison failed; CMD17 stays selected\n");
		return;
	}
	printf("emmc0: CMD18 verified against CMD17 (65536 bytes); single %llu us, multi %llu us\n",
	    (t1 - t0) * 1000000 / dw_hz(), (t2 - t1) * 1000000 / dw_hz());
}

/* emmc_dma=1: the IDMAC read path, used only if it reads the same 64 KiB as PIO does */
static void
dw_probe_dma(void)
{
	uint32_t wanted = 0, hcon = dw_rd(DW_HCON);
	uint64_t base = 0xc8000000ULL;

	if (!PE_parse_boot_argn("emmc_dma", &wanted, sizeof(wanted)) || wanted != 1 || dw.sectors < 128) {
		return;
	}
	(void)PE_parse_boot_argn("emmc_dma_base", &base, sizeof(base));
	if ((base & 0xfffff) || base + DMA_WINDOW > 0x100000000ULL) {
		printf("emmc0: emmc_dma_base %#llx must be 1 MiB aligned and below 4 GiB; DMA stays off\n", base);
		return;
	}
	dw_dma_pa = base;
	dw_dma64 = DW_HCON_ADDR64(hcon) != 0;
	dw_dma_va = ml_io_map(dw_dma_pa, DMA_WINDOW);
	if (dw_dma_va == 0) {
		printf("emmc0: cannot map the DMA window; DMA stays off\n");
		return;
	}
	uint32_t saved_multi = dw_multiblock;
	uint64_t t0 = dw_now();
	int pio = dw_read(0, 128, dw_probe_single);
	uint64_t t1 = dw_now();
	dw_dma = 1;
	int dma = dw_read(0, 128, dw_probe_multi);
	uint64_t t2 = dw_now();
	dw_multiblock = saved_multi;            /* a DMA failure must not have changed the PIO choice */
	if (pio != 0 || dma != 0 || !dw_dma || memcmp(dw_probe_single, dw_probe_multi, sizeof(dw_probe_single)) != 0) {
		dw_dma = 0;
		printf("emmc0: IDMAC comparison failed (pio %d, dma %d); PIO stays selected\n", pio, dma);
		return;
	}
	printf("emmc0: IDMAC (%s descriptors, window %#llx) verified against PIO (65536 bytes); pio %llu us, dma %llu us\n",
	    dw_dma64 ? "64-bit" : "32-bit", dw_dma_pa, (t1 - t0) * 1000000 / dw_hz(), (t2 - t1) * 1000000 / dw_hz());
}

/* The new node exists only after explicit arguments, a bounded layout, user-area
 * EXT_CSD, and an already formatted journaled HFS+ volume all agree.
 */
static void
dw_prepare_var(const uint8_t *root_header)
{
	uint32_t writes = 0, block;
	uint64_t count, image_sectors;
	uint8_t ext[SECTOR], hfs[SECTOR];
	if (!PE_parse_boot_argn("emmc_write", &writes, sizeof(writes)) || writes != 1) {
		return;
	}
	if (!PE_parse_boot_argn("emmc_var_lba", &dw_var_lba, sizeof(dw_var_lba)) ||
	    !PE_parse_boot_argn("emmc_var_sectors", &dw_var_sectors, sizeof(dw_var_sectors)) ||
	    memcmp(root_header + 32, "NXSB", 4) != 0) {
		printf("emmc1: missing window or APFS root; writes disabled\n");
		return;
	}
	memcpy(&block, root_header + 36, sizeof(block));
	memcpy(&count, root_header + 40, sizeof(count));
	if (block < 4096 || block > 65536 || (block & (block - 1)) || count < 2 ||
	    count > UINT64_MAX / block) {
		printf("emmc1: invalid APFS geometry; writes disabled\n");
		return;
	}
	image_sectors = count * block / SECTOR;
	if (!dw_var_valid(image_sectors)) {
		printf("emmc1: window overlaps root/payload or leaves its allowed region; writes disabled\n");
		return;
	}
	if (!dw_receive(MMC_SEND_EXT_CSD, 0, 1, ext) || (ext[179] & 7) != 0) {
		printf("emmc1: cannot verify eMMC user area; writes disabled\n");
		dw_recover();
		return;
	}
	uint32_t card_sectors;
	memcpy(&card_sectors, ext + 212, sizeof(card_sectors));
	if (dw.lba + dw.sectors > card_sectors ||
	    !dw_read_block(dw_var_lba + 2, hfs)) {
		printf("emmc1: window exceeds card or HFS header unreadable; writes disabled\n");
		return;
	}
	uint32_t attrs = ((uint32_t)hfs[4] << 24) | ((uint32_t)hfs[5] << 16) | ((uint32_t)hfs[6] << 8) | hfs[7];
	uint32_t hblock = ((uint32_t)hfs[40] << 24) | ((uint32_t)hfs[41] << 16) | ((uint32_t)hfs[42] << 8) | hfs[43];
	uint32_t hcount = ((uint32_t)hfs[44] << 24) | ((uint32_t)hfs[45] << 16) | ((uint32_t)hfs[46] << 8) | hfs[47];
	if (hfs[0] != 'H' || hfs[1] != '+' || !(attrs & (1u << 13)) ||
	    !(hfs[12] | hfs[13] | hfs[14] | hfs[15]) || hblock < SECTOR ||
	    (hblock & (hblock - 1)) || hcount == 0 || (uint64_t)hblock * hcount != dw_var_sectors * SECTOR) {
		printf("emmc1: no matching journaled HFS+ volume; format with xnusys.py var-write first\n");
		return;
	}
	dw_cache_enabled = (ext[33] & 1) != 0;
	dw.sectors = image_sectors; /* emmc0 can no longer even read the data window */
	dw_var_ready = true;
	uint32_t write_multi = 0;
	if (PE_parse_boot_argn("emmc_write_multi", &write_multi, sizeof(write_multi)) && write_multi == 1) {
		dw_write_multiblock = 1;
	}
	printf("emmc1: writer CMD%s, bounded to %u sectors per command\n",
	    dw_write_multiblock ? "23/25" : "24", dw_write_multiblock ? 256 : 1);
	printf("emmc1: WRITABLE window %llu + %llu sectors, HFS+ journaled, cache %s; root read only\n",
	    dw_var_lba, dw_var_sectors, dw_cache_enabled ? "write-through FLUSH_CACHE" : "off");
}

/*
 * Called by IOFindBSDRoot for rd=emmc0, before devfs exists: take the controller over, check the
 * window and register the block device. Returns its dev_t, or NODEV.
 */
dev_t
hakios_dwmmc_root(void)
{
	uint32_t verid, hcon, clksel, ctrl, bmod;
	uint64_t lba = 0, sectors = 0;
	uint8_t first[SECTOR];

	if (dw_ready) {
		return makedev(dw_bmajor, 0);
	}
	if (!PE_parse_boot_argn("emmc_lba", &lba, sizeof(lba)) ||
	    !PE_parse_boot_argn("emmc_sectors", &sectors, sizeof(sectors)) || lba == 0 || sectors == 0 ||
	    lba > UINT32_MAX || sectors > (uint64_t)UINT32_MAX + 1 - lba) {
		printf("emmc0: rd=emmc0 needs emmc_lba= and emmc_sectors= (xnusys.py plan)\n");
		return NODEV;
	}
	dw.lba = lba;
	dw.sectors = sectors;
	dw.base = ml_io_map(DWMMC0_BASE, DWMMC0_SIZE);
	verid = dw_rd(DW_VERID);
	hcon = dw_rd(DW_HCON);
	dw.width = 2u << DW_HCON_DATA_WIDTH(hcon);
	if (dw.width != 2 && dw.width != 4 && dw.width != 8) {
		printf("emmc0: unsupported FIFO width %u\n", dw.width);
		return NODEV;
	}
	dw.fifo = dw.base + ((verid & 0xffff) >= 0x240a ? DW_DATA_240A : DW_DATA_OLD);
	clksel = dw_rd(dw.width == 8 ? EXY_CLKSEL64 : EXY_CLKSEL);
	dw.hold = EXY_CLKSEL_DRV_WD3(clksel) ? DW_CMD_USE_HOLD_REG : 0;
	ctrl = dw_rd(DW_CTRL);
	bmod = dw_rd(DW_BMOD);
	printf("emmc0: DW MMC 0 at %#llx: VERID %#x HCON %#x (FIFO %u bytes wide) CTRL %#x BMOD %#x CLKSEL %#x\n",
	    DWMMC0_BASE, verid, hcon, dw.width, ctrl, bmod, clksel);

	if (!dw_wait_clear(DW_STATUS, DW_STATUS_DATA_BUSY, 500)) {
		printf("emmc0: the card stays busy\n");
		return NODEV;
	}
	/* PIO, no interrupts: the internal DMA off, the controller's interrupt output off */
	dw_wr(DW_INTMASK, 0);
	dw_wr(DW_BMOD, bmod & ~DW_BMOD_DE);
	dw_wr(DW_CTRL, (ctrl & ~(DW_CTRL_USE_IDMAC | DW_CTRL_DMA_ENABLE | DW_CTRL_INT_ENABLE)) |
	    DW_CTRL_FIFO_RESET | DW_CTRL_DMA_RESET);
	if (!dw_wait_clear(DW_CTRL, DW_CTRL_FIFO_RESET | DW_CTRL_DMA_RESET, 100)) {
		printf("emmc0: FIFO reset does not finish\n");
		return NODEV;
	}
	dw_wr(DW_RINTSTS, 0xffffffffu);

	/* the first block of the window: an APFS container has NXSB at byte 32 */
	if (dw_read(0, 1, first) != 0) {
		printf("emmc0: the first block of the window (eMMC %llu) cannot be read\n", lba);
		return NODEV;
	}
	printf("emmc0: window eMMC %llu + %llu sectors (%llu MB); its first block %s\n", lba, sectors,
	    sectors * SECTOR >> 20, memcmp(first + 32, "NXSB", 4) == 0 ? "is an APFS container" :
	    "is NOT an APFS container (is the system image there? xnusys.py verify)");
	dw_probe_multiblock();
	dw_probe_dma();
	dw_prepare_var(first);

	dw_bmajor = bdevsw_add(-1, &dw_bdevsw);
	if (dw_bmajor < 0) {
		printf("emmc0: bdevsw_add failed\n");
		return NODEV;
	}
	dw_cmajor = cdevsw_add_with_bdev(-1, &dw_cdevsw, dw_bmajor);
	if (dw_cmajor < 0) {
		printf("emmc0: cdevsw_add failed\n");
		return NODEV;
	}
	dw_ready = true;

	if (PE_parse_boot_argn("wdt_run", &wdt_run, sizeof(wdt_run)) && wdt_run) {
		thread_t th;

		wdt_base = ml_io_map(WDT_CL0, 0x1000);
		if (kernel_thread_start(dw_wdt_thread, NULL, &th) == KERN_SUCCESS) {
			thread_deallocate(th);
			printf("emmc0: feeding the loader's watchdog for %u s\n", wdt_run);
		}
	}
	return makedev(dw_bmajor, 0);
}

bool
hakios_dwmmc_dev(dev_t dev)
{
	return (dw_bmajor >= 0 && major(dev) == dw_bmajor) || (dw_cmajor >= 0 && major(dev) == dw_cmajor);
}

/* After devfs is mounted: /dev/emmc0 and /dev/remmc0, for whoever wants to look at the disk. */
void
hakios_dwmmc_devfs(void)
{
	if (!dw_ready) {
		return;
	}
	if (devfs_make_node(makedev(dw_bmajor, 0), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "emmc0") == NULL ||
	    devfs_make_node(makedev(dw_cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "remmc0") == NULL) {
		printf("emmc0: devfs_make_node failed\n");
	}
	if (dw_var_ready &&
	    (devfs_make_node(makedev(dw_bmajor, 1), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0600, "emmc1") == NULL ||
	    devfs_make_node(makedev(dw_cmajor, 1), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0600, "remmc1") == NULL)) {
		printf("emmc1: devfs_make_node failed\n");
	}
}
