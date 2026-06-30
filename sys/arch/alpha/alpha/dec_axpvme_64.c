/* $NetBSD$ */

/*
 * Copyright (c) 1995, 1996, 1997 Carnegie-Mellon University.
 * All rights reserved.
 *
 * Author: Chris G. Demetriou
 *
 * Permission to use, copy, modify and distribute this software and
 * its documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 *
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND
 * FOR ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 *
 * Carnegie Mellon requests users of this software to return to
 *
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 *
 * any improvements or extensions that they make and grant Carnegie the
 * rights to redistribute these changes.
 */

#include <sys/cdefs.h>

__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>

#include <machine/rpb.h>
#include <machine/autoconf.h>
#include <machine/cpuconf.h>

#include <dev/pci/pcivar.h>

#include <alpha/pci/lcareg.h>
#include <alpha/pci/lcavar.h>

void dec_axpvme_64_init(void);
static void dec_axpvme_64_cons_init(void);
static void dec_axpvme_64_device_register(device_t, void *);

const struct alpha_variation_table dec_axpvme_64_variations[] = {
	{ 0, "DEC AXPvme 230" },
	{ 0, NULL },
};

/*
 * AXPvme 230 Module Control Register 1.
 * I/O port 0x2C00, bit 4: enable external hardware interrupts
 * (SIO IRQ<1> and VIC64 IRQ<2>) to reach the CPU.
 * This bit is disabled by default and must be set explicitly.
 */
#define AXPVME_MCR1_IOPORT	0x2C00
#define AXPVME_MCR1_IRQ_ENABLE	0x10	/* bit 4 */

static struct lca_config *
lca_preinit(void)
{
	extern struct lca_config lca_configuration;

	lca_init(&lca_configuration);
	return &lca_configuration;
}

void
dec_axpvme_64_init(void)
{
	uint64_t variation;
	struct lca_config *lcp;
	bus_space_tag_t iot;
	bus_space_handle_t ioh;

	platform.family = "AXPvme";

	if ((platform.model = alpha_dsr_sysname()) == NULL) {
		variation = hwrpb->rpb_variation & SV_ST_MASK;
		if ((platform.model = alpha_variation_name(variation,
		    dec_axpvme_64_variations)) == NULL)
			platform.model = alpha_unknown_sysname();
	}

	platform.iobus = "lca";
	platform.cons_init = dec_axpvme_64_cons_init;
	platform.device_register = dec_axpvme_64_device_register;

	lcp = lca_preinit();
	iot = &lcp->lc_iot;

	/*
	 * Enable external hardware interrupts (SIO and VIC64) to reach
	 * the CPU by setting bit 4 of Module Control Register 1.
	 */
	{
		int rv = bus_space_map(iot, AXPVME_MCR1_IOPORT, 1, 0, &ioh);
		if (rv == 0) {
			uint8_t before = bus_space_read_1(iot, ioh, 0);
			uint8_t after = before | AXPVME_MCR1_IRQ_ENABLE;
			bus_space_write_1(iot, ioh, 0, after);
			uint8_t readback = bus_space_read_1(iot, ioh, 0);
			printf("axpvme: MCR1[0x%04x]: 0x%02x -> wrote 0x%02x"
			    " readback=0x%02x\n",
			    AXPVME_MCR1_IOPORT, before, after, readback);
			/* leave mapped */
		} else {
			printf("axpvme: MCR1 bus_space_map failed rv=%d\n", rv);
		}
	}

	lca_probe_bcache();
}

static void
dec_axpvme_64_cons_init(void)
{
	/*
	 * The SRM PROM callback console (promcons) is used throughout.
	 * init_bootstrap_console() has already set cn_tab = &promcons.
	 */
}

static void
dec_axpvme_64_device_register(device_t dev, void *aux)
{
	pci_find_bootdev(NULL, dev, aux);
}
