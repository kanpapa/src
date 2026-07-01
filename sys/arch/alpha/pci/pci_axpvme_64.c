/* $NetBSD$ */

/*
 * PCI interrupt support for DEC AXPvme 230.
 *
 * The AXPvme 230 uses the LCA (21066A) chipset with an SIO (82378IB)
 * PCI-to-ISA bridge.  The SRM firmware for AXPvme 230 was designed for
 * VMEbus (VIC64 on CPU IRQ<2>) and does NOT perform PCI IACK cycles when
 * handling SIO interrupts on CPU IRQ<1>.  Without the IACK, any hardware
 * interrupt from the 8259 results in the wrong SCB vector being dispatched,
 * which calls scb_stray -> printf -> PROM callback -> Bcache invalidation
 * protocol hang.
 *
 * Solution: keep all ISA IRQs masked in the 8259 to suppress hardware
 * interrupt delivery.  Use a per-tick callout to poll registered interrupt
 * handlers directly, bypassing the 8259/IACK path entirely.
 *
 * Clock (MCLK) note: the AXPvme 230 SRM PAL uses the 1024 Hz heartbeat
 * (DS1386 -> SIO IRQ1) as its MCLK source.  It identifies MCLK by the
 * 8259 IACK vector, which is ICW2_base+1.  The SRM programs master 8259
 * with ICW2=0x08, so the MCLK IACK vector is 0x09.  sio_intr_setup()
 * reprograms ICW2 to 0x00 (standard), making the IACK vector 0x01.
 * The PAL no longer recognises it as MCLK, so hardclock() is never called
 * and callouts never fire.  We re-program the master 8259 with ICW2=0x08
 * after sio_intr_setup() and unmask only IRQ1 and IRQ2 (cascade).
 */

#include <sys/cdefs.h>

__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/device.h>
#include <sys/callout.h>

#include <machine/autoconf.h>
#include <machine/rpb.h>
#include <sys/bus.h>
#include <machine/intr.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <alpha/pci/lcavar.h>
#include <alpha/pci/siovar.h>

#include <dev/ic/i8259reg.h>
#include <dev/isa/isareg.h>

#include "sio.h"

/* ISA I/O bus_space tag saved from pickintr for use in intr_establish */
static bus_space_tag_t axpvme_iot;

/* Polling callout: one callout drives all established interrupt handlers */
static struct callout axpvme_poll_callout;

/*
 * Per-interrupt-handle poll list entry.
 * We maintain a small static array of established handlers to call each tick.
 */
#define AXPVME_MAX_POLL_IH	4
static struct alpha_shared_intrhand *axpvme_poll_ih[AXPVME_MAX_POLL_IH];
static int axpvme_poll_count;

static void
axpvme_poll(void *arg)
{
	static unsigned int axpvme_poll_ticks;
	int i;

	axpvme_poll_ticks++;

	/*
	 * Call ih_real_fn directly, bypassing alpha_shared_intr_wrapper.
	 * The wrapper acquires KERNEL_LOCK(1, NULL), which blocks when
	 * the NFS stack holds the kernel lock (up to 700 ms), reducing
	 * the effective poll rate from 1024 Hz to ~1.4 Hz.  On this UP
	 * system the kernel lock provides no mutual exclusion against
	 * concurrent CPUs; ih_real_fn (tlp_intr) uses its own sc_lock
	 * for internal serialisation, so bypassing the wrapper is safe.
	 */
	for (i = 0; i < axpvme_poll_count; i++) {
		struct alpha_shared_intrhand *ih = axpvme_poll_ih[i];
		if (ih != NULL && ih->ih_real_fn != NULL)
			(*ih->ih_real_fn)(ih->ih_real_arg);
	}
	callout_schedule(&axpvme_poll_callout, 1);
}

/*
 * Custom pc_intr_establish for AXPvme 230.
 *
 * Registers the handler via the normal SIO path (which sets up the SCB
 * and unmasks the IRQ in the 8259), then immediately re-masks the IRQ
 * to prevent hardware interrupts (which the AXPvme 230 SRM PAL cannot
 * handle correctly via IACK).  The handler is instead invoked each tick
 * by axpvme_poll().
 */
static void *
axpvme_64_intr_establish(pci_chipset_tag_t pc, pci_intr_handle_t ih,
    int level, int (*func)(void *), void *arg)
{
	struct alpha_shared_intrhand *cookie;
	bus_space_handle_t ioh_icu1;
	uint8_t ocw1;
	u_int irq = alpha_pci_intr_handle_get_irq(&ih);

	printf("axpvme_64_intr_establish: irq=%u\n", irq);

	/* Register handler through normal SIO path (SCB setup, unmask). */
	cookie = sio_pci_intr_establish(pc, ih, level, func, arg);
	printf("axpvme_64_intr_establish: sio_pci_intr_establish cookie=%p\n",
	    cookie);
	if (cookie == NULL)
		return NULL;

	/*
	 * Re-mask the IRQ in the 8259.  AXPvme 230's SRM PAL does not
	 * read the LCA PCI IACK register (0x1A0000000) when handling
	 * IRQ<1>, so the SCB gets an unknown vector -> scb_stray ->
	 * printf -> PROM Bcache-invalidation hang.  Masking here prevents
	 * the hardware interrupt from reaching the CPU at all; axpvme_poll
	 * services it via software polling instead.
	 */
	if (irq >= 0 && irq < 8) {
		/* Master 8259: I/O ports 0x20-0x21 */
		if (bus_space_map(axpvme_iot, IO_ICU1, 2, 0, &ioh_icu1) == 0) {
			ocw1 = bus_space_read_1(axpvme_iot, ioh_icu1, 1);
			bus_space_write_1(axpvme_iot, ioh_icu1, 1,
			    ocw1 | (uint8_t)(1 << irq));
			alpha_mb();
			bus_space_unmap(axpvme_iot, ioh_icu1, 2);
		}
	}
	/* IRQs 8-15 go through the slave 8259; add IO_ICU2 masking if needed */

	/* Register in our poll list if there is room. */
	if (axpvme_poll_count < AXPVME_MAX_POLL_IH) {
		axpvme_poll_ih[axpvme_poll_count++] = cookie;
		/* Start the callout on the first established handler. */
		if (axpvme_poll_count == 1)
			callout_reset(&axpvme_poll_callout, 1, axpvme_poll,
			    NULL);
	} else {
		printf("axpvme_64_intr_establish: poll table full, irq %d "
		    "will not be polled\n", irq);
	}

	return cookie;
}

static void
pci_axpvme_64_pickintr(void *core, bus_space_tag_t iot, bus_space_tag_t memt,
    pci_chipset_tag_t pc)
{
	/* Save ISA I/O tag for use in axpvme_64_intr_establish. */
	axpvme_iot = iot;

	pc->pc_intr_v = core;
	pc->pc_intr_map = alpha_pci_generic_intr_map;
	pc->pc_intr_string = sio_pci_intr_string;
	pc->pc_intr_evcnt = sio_pci_intr_evcnt;
	pc->pc_intr_establish = axpvme_64_intr_establish;
	pc->pc_intr_disestablish = sio_pci_intr_disestablish;

	/* Not supported on AXPvme 230 (no PCI IDE). */
	pc->pc_pciide_compat_intr_establish = NULL;

	pc->pc_intr_desc = "axpvme";
	pc->pc_nirq = 16;		/* ISA IRQs 0-15 via SIO 8259 */

	/* CALLOUT_MPSAFE: do not acquire kernel_lock before calling axpvme_poll */
	callout_init(&axpvme_poll_callout, CALLOUT_MPSAFE);
	axpvme_poll_count = 0;

#if NSIO
	sio_intr_setup(pc, iot);

	/*
	 * Re-initialise the master 8259 with the SRM's ICW2=0x08.
	 *
	 * sio_intr_setup() sets master ICW2=0x00 (vectors 0x00-0x07), but the
	 * AXPvme 230 SRM PAL was compiled expecting ICW2=0x08 (vectors 0x08-0x0F).
	 * The PAL identifies the 1024 Hz heartbeat (DS1386 -> SIO IRQ1) as the
	 * MCLK source by its IACK vector 0x09 (= 0x08 + IRQ1).  With ICW2=0x00
	 * the IACK vector is 0x01, which the PAL dispatches as ALPHA_INTR_DEVICE
	 * rather than ALPHA_INTR_CLOCK, so hardclock() is never called.
	 *
	 * Unmask only IRQ1 (heartbeat -> MCLK) and IRQ2 (cascade).  All other
	 * ISA IRQs, including IRQ6 (DECchip 21040 NIC), remain masked; the NIC
	 * is serviced by the axpvme_poll() per-tick callout instead.
	 */
	{
		bus_space_handle_t ioh_icu1;

		if (bus_space_map(axpvme_iot, IO_ICU1, 2, 0, &ioh_icu1) == 0) {
			bus_space_write_1(axpvme_iot, ioh_icu1, PIC_ICW1,
			    ICW1_SELECT | ICW1_IC4);
			bus_space_write_1(axpvme_iot, ioh_icu1, PIC_ICW2,
			    ICW2_VECTOR(0x08));		/* restore SRM's ICW2 */
			bus_space_write_1(axpvme_iot, ioh_icu1, PIC_ICW3,
			    ICW3_CASCADE(2));
			bus_space_write_1(axpvme_iot, ioh_icu1, PIC_ICW4,
			    ICW4_8086);
			bus_space_write_1(axpvme_iot, ioh_icu1, PIC_OCW1,
			    (uint8_t)~((1 << 1) | (1 << 2))); /* unmask IRQ1+IRQ2 */
			alpha_mb();
			bus_space_unmap(axpvme_iot, ioh_icu1, 2);
		}
		printf("axpvme: master 8259 ICW2=0x08, OCW1=0xf9 (IRQ1+IRQ2 unmasked)\n");
	}

	/*
	 * Clear any pending heartbeat interrupt latch (HBEAT_CLR_REG).
	 *
	 * The DS1386 SQW output drives SIO IRQ1 through a level-latch:
	 * each 1024 Hz rising edge SETS the latch (IRQ1=HIGH, asserted),
	 * and it stays HIGH until software writes HBEAT_CLR_REG.  The SRM
	 * PAL wrote HBEAT_CLR_REG on every MCLK tick, but stopped doing so
	 * when handing control to the kernel.  The 8259 is edge-triggered;
	 * if IRQ1 is stuck HIGH after re-initialization no rising edge
	 * occurs and no interrupt is ever delivered.
	 *
	 * Writing HBEAT_CLR_REG de-asserts IRQ1.  The next DS1386 1024 Hz
	 * pulse then presents a fresh rising edge; the 8259 detects it and
	 * the PAL delivers ALPHA_INTR_CLOCK (IACK=0x09 with ICW2=0x08).
	 * From that point on, the PAL's MCLK handler continues writing
	 * HBEAT_CLR_REG on every tick.
	 */
	{
		bus_space_handle_t ioh_hbeat;
		if (bus_space_map(axpvme_iot, 0x2000, 1, 0, &ioh_hbeat) == 0) {
			bus_space_write_1(axpvme_iot, ioh_hbeat, 0, 0);
			alpha_mb();
			bus_space_unmap(axpvme_iot, ioh_hbeat, 1);
			printf("axpvme: HBEAT_CLR_REG(0x2000) written, IRQ1 latch cleared\n");
		} else {
			printf("axpvme: HBEAT_CLR_REG map failed!\n");
		}
	}
#else
	panic("pci_axpvme_64_pickintr: no I/O interrupt handler (no sio)");
#endif
}
ALPHA_PCI_INTR_INIT(ST_DEC_AXPVME_64, pci_axpvme_64_pickintr)
