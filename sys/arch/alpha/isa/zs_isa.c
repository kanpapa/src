/* $NetBSD$ */

/*
 * Minimal polled 85C30 SCC console for DEC AXPvme 230, Channel A.
 *
 * SCC memory map (AXPvme TD Figure 1-43, section 1.12.2):
 *
 *   ctb_csr = UART_BASE_ADDR = 0x6000 (base of whole SCC)
 *
 *   ISA I/O offset   Read        Write
 *   +00              Ch B RR0    Ch B WR0  (control)
 *   +04              Ch B Rx     Ch B Tx   (data)
 *   +08              Ch A RR0    Ch A WR0  (control)  <- user console
 *   +0C              Ch A Rx     Ch A Tx   (data)
 *
 * Channel A is the user console; Channel B is uncommitted (TD 1.12.4).
 * ctb_csr points to the SCC base (Channel B address), so Channel A is
 * at base + 8 (control) and base + 12 (data).
 *
 * The SRM has already configured Channel A (baud rate, 8N1).
 * This driver reuses that configuration and provides polled I/O
 * without reconfiguring the chip.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>

#include <sys/bus.h>
#include <dev/cons.h>
#include <dev/ic/z8530reg.h>

#include <alpha/isa/zs_isa.h>

/*
 * The 85C30 requires 1.6 us recovery time between control-register
 * accesses.  Use 2 us for margin.
 */
#define ZSISA_DELAY()	DELAY(2)

/*
 * Channel A register offsets from UART_BASE_ADDR (ctb_csr).
 * These are ISA I/O byte offsets; each SCC register occupies one longword.
 */
#define ZSISA_CTRL	0x08	/* Ch A WR0 (write) / RR0 (read) */
#define ZSISA_DATA	0x0C	/* Ch A Tx data (write) / Rx data (read) */
#define ZSISA_MAPSIZE	0x10	/* must cover offsets 0x00 – 0x0F */

static bus_space_tag_t		zsisa_iot;
static bus_space_handle_t	zsisa_ioh;

static void	zsisa_cnputc(dev_t, int);
static int	zsisa_cngetc(dev_t);

static struct consdev zsisa_consdev = {
	.cn_getc  = zsisa_cngetc,
	.cn_putc  = zsisa_cnputc,
	.cn_pollc = nullcnpollc,
	.cn_dev   = NODEV,
	.cn_pri   = CN_REMOTE,
};

/*
 * zsisa_cnattach: map the SCC from baseaddr and claim Channel A as console.
 *
 * baseaddr is UART_BASE_ADDR (= ctb_csr = 0x6000).  Channel A registers
 * are at baseaddr+ZSISA_CTRL and baseaddr+ZSISA_DATA.
 */
int
zsisa_cnattach(bus_space_tag_t iot, bus_addr_t baseaddr)
{
	if (bus_space_map(iot, baseaddr, ZSISA_MAPSIZE, 0, &zsisa_ioh) != 0)
		return ENOMEM;
	zsisa_iot = iot;

	/* Reset Channel A External/Status interrupt latch (WR0 command). */
	ZSISA_DELAY();
	bus_space_write_1(iot, zsisa_ioh, ZSISA_CTRL, ZSWR0_RESET_STATUS);
	ZSISA_DELAY();

	/*
	 * Inherit cn_dev from the previous console so that cnopen() can
	 * open an underlying device.  Without a tty driver for the Z8530,
	 * cn_dev would be NODEV and cnopen() would panic.
	 * Kernel printf still reaches the Z8530 via our cn_putc.
	 */
	if (cn_tab != NULL)
		zsisa_consdev.cn_dev = cn_tab->cn_dev;

	cn_tab = &zsisa_consdev;
	return 0;
}

static void
zsisa_cnputc(dev_t dev, int c)
{
	int s, timo;

	s = splhigh();
	timo = 50000;		/* 100ms max; 9600 baud needs ~1ms/char */
	while (!(bus_space_read_1(zsisa_iot, zsisa_ioh, ZSISA_CTRL)
	    & ZSRR0_TX_READY)) {
		ZSISA_DELAY();
		if (--timo == 0)
			break;
	}
	if (timo > 0) {
		bus_space_write_1(zsisa_iot, zsisa_ioh, ZSISA_DATA, (uint8_t)c);
		ZSISA_DELAY();
	}
	splx(s);
}

static int
zsisa_cngetc(dev_t dev)
{
	while (!(bus_space_read_1(zsisa_iot, zsisa_ioh, ZSISA_CTRL)
	    & ZSRR0_RX_READY))
		ZSISA_DELAY();
	ZSISA_DELAY();
	return (int)(uint8_t)bus_space_read_1(zsisa_iot, zsisa_ioh, ZSISA_DATA);
}
