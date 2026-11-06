/*
 * The volume keys of the Samsung Galaxy A21s (Exynos 850) as button records for /dev/hakios-touch
 * (sys/hakios.input.h). Only with boot-arg buttons=3 (hakios.buttons.c hands over to this).
 *
 * Both keys are plain pins of the SoC's "alive" pin controller (pinctrl@11850000), as the phone's
 * own device tree has them for gpio_keys: volume down GPA1[0], volume up GPA0[7], pulled up, the
 * key pulls the pin low. The power key is not a pin: it is the S2MPU12 PMIC's, behind ACPM, and
 * is not here yet.
 *
 * THIS DRIVER ONLY READS: the pins' data register, and their configuration register to check them
 * before it believes them (input, or external interrupt: what Android's gpio_keys leaves). Nothing
 * is written to the pin controller.
 *
 * A thread looks at both 60 times a second; a key has changed when two looks in a row say so, and
 * a key that reads as held before it has ever been seen released is not believed.
 *
 * Bank layout (each bank 0x20: CON +0x0, 4 bits a pin; DAT +0x4, 1 bit a pin): Linux's
 * drivers/pinctrl/samsung/pinctrl-exynos-arm64.c, exynos850_pin_banks0 (read, not copied).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void IOSleep(unsigned int milliseconds);

#define ALIVE_PINCTRL   0x11850000ULL
#define BANK_GPA0       0x000
#define BANK_GPA1       0x020
#define GP_CON          0x0
#define GP_DAT          0x4
#define CON_INPUT       0x0
#define CON_EINT        0xf

static vm_offset_t pins;

static struct key {
	const char *name;
	uint32_t    bank, pin;
	uint32_t    record;         /* HAKIOS_BUTTON_* */
	bool        usable;         /* its pin is set up as a key's */
	bool        live;           /* seen released once */
	bool        down;
	int         last;           /* the previous look: 1 held, 0 not, -1 none yet */
	uint32_t    presses;
} keys[] = {
	{ .name = "volume up",   .bank = BANK_GPA0, .pin = 7, .record = HAKIOS_BUTTON_VOLUME_UP,   .last = -1 },
	{ .name = "volume down", .bank = BANK_GPA1, .pin = 0, .record = HAKIOS_BUTTON_VOLUME_DOWN, .last = -1 },
};

static inline uint32_t
rd(uint32_t off)
{
	return *(volatile uint32_t *)(pins + off);
}

static void
key_look(struct key *k)
{
	int held = !((rd(k->bank + GP_DAT) >> k->pin) & 1);     /* active low */

	if (held != k->last) {                  /* the first of two looks */
		k->last = held;
		return;
	}
	if (!k->live) {
		if (!held) {
			k->live = true;
			printf("exykeys: %s is released now: believed from here on\n", k->name);
		}
		return;
	}
	if (held == k->down) {
		return;
	}
	k->down = held;
	if (held) {
		k->presses++;
	}
	printf("exykeys: %s %s (%u presses)\n", k->name, held ? "PRESSED" : "released", k->presses);
	hakios_touch_post(held ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP, k->record, 0, 0);
}

/*
 * The way out of a run that keeps its log: volume up and volume down held together for 3 s. A
 * forced restart with the buttons (volume down + power) is a cold reset and the kernel's log in RAM
 * is lost with it (2026-11-05: the run at 12:20). This instead makes the loader's watchdog
 * (cluster 0, armed by exyloader, fed by hakios.dwmmc.c) fire at once: a warm reset, the RAM kept,
 * and the next boot hands this run's log to Android as after any run. Each finished line of the
 * log is in RAM already (loader_return.c); a journaled /private/var is whole at its next mount.
 */
#define WDT_CL0         0x10050000ULL
#define WTCON           0x00
#define WTDAT           0x04
#define WTCNT           0x08
#define WTCON_ENABLE    (1u << 5)
#define WTCON_RESET     (1u << 0)
#define BOTH_HELD_LOOKS (3 * 60)        /* 3 s of looks, 60 a second */

static void
both_keys_way_out(void)
{
	vm_offset_t wdt = ml_io_map(WDT_CL0, 0x1000);
	uint32_t con = *(volatile uint32_t *)(wdt + WTCON);

	printf("exykeys: volume up + volume down held for 3 s: a warm reset through the watchdog now, "
	    "the log is kept (WTCON %#x)\n", con);
	IOSleep(100);
	if (!(con & WTCON_ENABLE) || !(con & WTCON_RESET)) {
		printf("exykeys: the watchdog is not armed to reset (WTCON %#x): nothing done\n", con);
		return;
	}
	/* reload value and counter both 1: whoever feeds it next only reloads 1 again */
	*(volatile uint32_t *)(wdt + WTDAT) = 1;
	*(volatile uint32_t *)(wdt + WTCNT) = 1;
	__builtin_arm_dsb(0xf);
	IOSleep(2000);
	printf("exykeys: the watchdog did not reset the phone\n");
}

static void
exykeys_thread(__unused void *arg, __unused wait_result_t wr)
{
	unsigned n = 0, both = 0;

	for (;;) {
		for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			if (keys[i].usable) {
				key_look(&keys[i]);
			}
		}
		both = (keys[0].usable && keys[1].usable && keys[0].down && keys[1].down) ? both + 1 : 0;
		if (both == BOTH_HELD_LOOKS) {
			both_keys_way_out();
		}
		if (++n % (60 * 60) == 0) {     /* once a minute: the log's start may be lost */
			printf("exykeys: up %u, down %u presses so far\n", keys[0].presses, keys[1].presses);
		}
		IOSleep(16);
	}
}

void
hakios_exykeys_init(void)
{
	unsigned usable = 0;
	thread_t thread;

	pins = ml_io_map(ALIVE_PINCTRL, 0x1000);
	for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		struct key *k = &keys[i];
		uint32_t con = (rd(k->bank + GP_CON) >> (k->pin * 4)) & 0xf;
		uint32_t dat = (rd(k->bank + GP_DAT) >> k->pin) & 1;

		k->usable = con == CON_INPUT || con == CON_EINT;
		printf("exykeys: %s: GP%s[%u] CON %#x DAT %u: %s\n", k->name, k->bank == BANK_GPA0 ? "A0" : "A1",
		    k->pin, con, dat, k->usable ? (dat ? "an input, not held" : "an input, reads held") :
		    "NOT set up as an input: left alone");
		usable += k->usable;
	}
	if (usable && kernel_thread_start(exykeys_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
		printf("exykeys: watching %u keys, 60 times a second\n", usable);
	}
}
