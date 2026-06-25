/* $NetBSD: dec_axpvme_64.c,v 1.71 2025/03/09 01:06:41 thorpej Exp $ */

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
/*
 * Additional Copyright (c) 1997 by Matthew Jacob for NASA/Ames Research Center
 */

#include <sys/cdefs.h>			/* RCS ID & Copyright macro defns */

__KERNEL_RCSID(0, "$NetBSD: dec_axpvme_64.c,v 1.71 2025/03/09 01:06:41 thorpej Exp $");

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

static struct lca_config *lca_preinit(void);

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

	printf("axpvme: before lca_preinit\n");
	/* 未使用警告を避けるため、到達しないifの中で参照だけしておく。 */
	if (0)
		lca_preinit();
	//printf("axpvme: after lca_preinit, before lca_probe_bcache\n");
	printf("axpvme: skipped lca_init entirely\n");
	//lca_probe_bcache();
	printf("axpvme: skipped lca_probe_bcache\n");
}

static void
dec_axpvme_64_cons_init(void)
{
	/*
	 * SRMのPROMコールバックコンソール(promcons)に任せる。
	 * init_bootstrap_console()で既にcn_tab = &promconsが
	 * 設定済みなので、ここでは何もしない。
	 */
	printf("axpvme: cons_init called\n");
}

static void
dec_axpvme_64_device_register(device_t dev, void *aux)
{
	pci_find_bootdev(NULL, dev, aux);
}