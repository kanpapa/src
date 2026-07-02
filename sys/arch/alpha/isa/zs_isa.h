/* $NetBSD$ */

#ifndef _ALPHA_ISA_ZS_ISA_H_
#define _ALPHA_ISA_ZS_ISA_H_

#include <sys/bus.h>
#include <sys/conf.h>

int zsisa_cnattach(bus_space_tag_t, bus_addr_t);

extern const struct cdevsw zsisa_cdevsw;

#endif /* _ALPHA_ISA_ZS_ISA_H_ */
