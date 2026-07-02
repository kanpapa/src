/* $NetBSD$ */

/*
 * Polled 85C30 SCC console and TTY driver for DEC AXPvme 230, Channel A.
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
 *
 * Interrupt note (TD 1.17, Figure 1-64):
 *   Z8530 UART interrupt → SIO IRQ4.  On AXPvme 230, all ISA IRQs
 *   except IRQ1 and IRQ2 are masked (see pci_axpvme_64.c).  We
 *   therefore use a per-tick callout for RX polling instead of
 *   hardware interrupts.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/tty.h>
#include <sys/conf.h>
#include <sys/proc.h>
#include <sys/kauth.h>
#include <sys/callout.h>

#include <sys/bus.h>
#include <dev/cons.h>
#include <dev/ic/z8530reg.h>
#include <dev/isa/isavar.h>

#include <machine/rpb.h>	/* cputype, ST_DEC_AXPVME_64 */

#include <alpha/isa/zs_isa.h>

extern struct cfdriver zsisa_cd;

/*
 * The 85C30 requires 1.6 us recovery time between control-register
 * accesses.  Use 2 us for margin.
 */
#define ZSISA_DELAY()		DELAY(2)

/*
 * Channel A register offsets from UART_BASE_ADDR (ctb_csr).
 * These are ISA I/O byte offsets; each SCC register occupies one longword.
 */
#define ZSISA_BASE_PORT		0x6000	/* ctb_csr = UART_BASE_ADDR */
#define ZSISA_CTRL		0x08	/* Ch A WR0 (write) / RR0 (read) */
#define ZSISA_DATA		0x0C	/* Ch A Tx data (write) / Rx data (read) */
#define ZSISA_MAPSIZE		0x10	/* must cover offsets 0x00 – 0x0F */

/* Globals set by zsisa_cnattach(); used by console and tty code. */
static bus_space_tag_t		zsisa_iot;
static bus_space_handle_t	zsisa_ioh;

/* ===================================================================== */
/* Early polled console (claimed by dec_axpvme_64_cons_init before       */
/* autoconf runs, so kernel printf reaches the physical terminal)        */
/* ===================================================================== */

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
	 * When zsisa_attach() runs later, cn_dev is updated to the real tty.
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

/* ===================================================================== */
/* ISA autoconf                                                           */
/* ===================================================================== */

struct zsisa_softc {
	device_t	sc_dev;
	struct tty     *sc_tty;
	struct callout	sc_rx_co;	/* RX poll callout */
};

static int	zsisa_match(device_t, cfdata_t, void *);
static void	zsisa_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(zsisa, sizeof(struct zsisa_softc),
    zsisa_match, zsisa_attach, NULL, NULL);

/* Forward declarations for cdevsw */
dev_type_open(zsisa_open);
dev_type_close(zsisa_close);
dev_type_read(zsisa_read);
dev_type_write(zsisa_write);
dev_type_ioctl(zsisa_ioctl);
dev_type_stop(zsisa_stop);
dev_type_tty(zsisa_ttydev);
dev_type_poll(zsisa_poll);

const struct cdevsw zsisa_cdevsw = {
	.d_open    = zsisa_open,
	.d_close   = zsisa_close,
	.d_read    = zsisa_read,
	.d_write   = zsisa_write,
	.d_ioctl   = zsisa_ioctl,
	.d_stop    = zsisa_stop,
	.d_tty     = zsisa_ttydev,
	.d_poll    = zsisa_poll,
	.d_mmap    = nommap,
	.d_kqfilter = ttykqfilter,
	.d_discard = nodiscard,
	.d_flag    = D_TTY,
};

static void	zsisa_tty_start(struct tty *);
static int	zsisa_tty_param(struct tty *, struct termios *);
static void	zsisa_rx_poll(void *);

static int
zsisa_match(device_t parent, cfdata_t cf, void *aux)
{
	struct isa_attach_args *ia = aux;
	bus_space_handle_t ioh;
	uint8_t rr0;

	/* Only present on AXPvme 230. */
	if (cputype != ST_DEC_AXPVME_64)
		return 0;

	/* Check port address. */
	if (ia->ia_nio < 1 ||
	    (ia->ia_io[0].ir_addr != ISA_UNKNOWN_PORT &&
	     ia->ia_io[0].ir_addr != ZSISA_BASE_PORT))
		return 0;

	if (ia->ia_niomem > 0 &&
	    ia->ia_iomem[0].ir_addr != ISA_UNKNOWN_IOMEM)
		return 0;

	if (ia->ia_nirq > 0 &&
	    ia->ia_irq[0].ir_irq != ISA_UNKNOWN_IRQ)
		return 0;

	if (ia->ia_ndrq > 0 &&
	    ia->ia_drq[0].ir_drq != ISA_UNKNOWN_DRQ)
		return 0;

	/*
	 * If zsisa_cnattach() has already mapped the ports, the hardware is
	 * already verified — skip the TX_RDY probe.  During ISA autoconf the
	 * Z8530 is actively used by the kernel console; a single-shot read of
	 * RR0 would catch TX_RDY=0 while a character is being transmitted and
	 * incorrectly return "not found".
	 */
	if (zsisa_iot != NULL)
		goto found;

	if (bus_space_map(ia->ia_iot, ZSISA_BASE_PORT, ZSISA_MAPSIZE, 0, &ioh))
		return 0;

	/* Probe: read Ch A RR0.  TX_RDY (bit 2) should be set when idle. */
	DELAY(2);
	rr0 = bus_space_read_1(ia->ia_iot, ioh, ZSISA_CTRL);
	bus_space_unmap(ia->ia_iot, ioh, ZSISA_MAPSIZE);

	if (!(rr0 & ZSRR0_TX_READY))
		return 0;

found:

	ia->ia_io[0].ir_addr = ZSISA_BASE_PORT;
	ia->ia_io[0].ir_size = ZSISA_MAPSIZE;
	ia->ia_nio  = 1;
	ia->ia_niomem = 0;
	ia->ia_nirq  = 0;
	ia->ia_ndrq  = 0;

	return 1;
}

static void
zsisa_attach(device_t parent, device_t self, void *aux)
{
	struct zsisa_softc *sc = device_private(self);
	struct isa_attach_args *ia = aux;
	struct tty *tp;
	dev_t dev;
	int maj;

	sc->sc_dev = self;

	/*
	 * If zsisa_cnattach() already mapped the I/O ports, reuse that
	 * mapping.  Otherwise map them now.
	 */
	if (zsisa_iot == NULL) {
		if (bus_space_map(ia->ia_iot, ZSISA_BASE_PORT,
		    ZSISA_MAPSIZE, 0, &zsisa_ioh) != 0) {
			aprint_error(": can't map I/O space\n");
			return;
		}
		zsisa_iot = ia->ia_iot;
	}

	aprint_normal(": Z8530 SCC Channel A, 9600 baud, polled\n");

	maj = cdevsw_lookup_major(&zsisa_cdevsw);
	dev = makedev(maj, device_unit(self));

	tp = tty_alloc();
	tp->t_dev    = dev;
	tp->t_oproc  = zsisa_tty_start;
	tp->t_param  = zsisa_tty_param;
	tty_attach(tp);
	sc->sc_tty = tp;

	callout_init(&sc->sc_rx_co, CALLOUT_MPSAFE);

	/*
	 * If we are the current console, update cn_dev so that user-space
	 * opens of /dev/console are directed to our tty device.
	 */
	if (cn_tab == &zsisa_consdev)
		cn_tab->cn_dev = dev;
}

/* ===================================================================== */
/* TTY cdev operations                                                    */
/* ===================================================================== */

int
zsisa_open(dev_t dev, int flag, int mode, struct lwp *l)
{
	struct zsisa_softc *sc =
	    device_lookup_private(&zsisa_cd, minor(dev));
	struct tty *tp;
	int s, error;

	if (sc == NULL)
		return ENXIO;
	tp = sc->sc_tty;

	s = spltty();
	if (!ISSET(tp->t_state, TS_ISOPEN) && tp->t_wopen == 0) {
		tp->t_dev = dev;
		ttychars(tp);
		/* 9600 baud 8N1, CLOCAL (no modem control) */
		tp->t_ospeed = tp->t_ispeed = 9600;
		tp->t_cflag = CREAD | CS8 | CLOCAL;
		tp->t_iflag = TTYDEF_IFLAG;
		tp->t_oflag = TTYDEF_OFLAG;
		tp->t_lflag = TTYDEF_LFLAG;
		(*tp->t_param)(tp, &tp->t_termios);
		ttsetwater(tp);
		SET(tp->t_state, TS_CARR_ON);
	} else if (ISSET(tp->t_state, TS_XCLUDE) &&
	    kauth_authorize_device_tty(l->l_cred, KAUTH_DEVICE_TTY_OPEN, tp)) {
		splx(s);
		return EBUSY;
	}
	splx(s);

	error = (*tp->t_linesw->l_open)(dev, tp);
	if (error)
		return error;

	/* Start RX polling on first open. */
	if (!callout_pending(&sc->sc_rx_co))
		callout_reset(&sc->sc_rx_co, 1, zsisa_rx_poll, sc);

	return 0;
}

int
zsisa_close(dev_t dev, int flag, int mode, struct lwp *l)
{
	struct zsisa_softc *sc =
	    device_lookup_private(&zsisa_cd, minor(dev));
	struct tty *tp = sc->sc_tty;

	(*tp->t_linesw->l_close)(tp, flag);
	ttyclose(tp);
	return 0;
}

int
zsisa_read(dev_t dev, struct uio *uio, int flag)
{
	struct zsisa_softc *sc =
	    device_lookup_private(&zsisa_cd, minor(dev));

	return (*sc->sc_tty->t_linesw->l_read)(sc->sc_tty, uio, flag);
}

int
zsisa_write(dev_t dev, struct uio *uio, int flag)
{
	struct zsisa_softc *sc =
	    device_lookup_private(&zsisa_cd, minor(dev));

	return (*sc->sc_tty->t_linesw->l_write)(sc->sc_tty, uio, flag);
}

int
zsisa_ioctl(dev_t dev, u_long cmd, void *data, int flag, struct lwp *l)
{
	struct zsisa_softc *sc =
	    device_lookup_private(&zsisa_cd, minor(dev));
	struct tty *tp = sc->sc_tty;
	int error;

	error = (*tp->t_linesw->l_ioctl)(tp, cmd, data, flag, l);
	if (error != EPASSTHROUGH)
		return error;
	return ttioctl(tp, cmd, data, flag, l);
}

void
zsisa_stop(struct tty *tp, int flag)
{
	int s;

	s = spltty();
	if (ISSET(tp->t_state, TS_BUSY)) {
		if (!ISSET(tp->t_state, TS_TTSTOP))
			SET(tp->t_state, TS_FLUSH);
	}
	splx(s);
}

struct tty *
zsisa_ttydev(dev_t dev)
{
	struct zsisa_softc *sc =
	    device_lookup_private(&zsisa_cd, minor(dev));

	return sc->sc_tty;
}

int
zsisa_poll(dev_t dev, int events, struct lwp *l)
{
	struct zsisa_softc *sc =
	    device_lookup_private(&zsisa_cd, minor(dev));

	return (*sc->sc_tty->t_linesw->l_poll)(sc->sc_tty, events, l);
}

/* ===================================================================== */
/* Polled TX (t_oproc)                                                   */
/* ===================================================================== */

static void
zsisa_tty_start(struct tty *tp)
{
	int c, timo;

	if (ISSET(tp->t_state, TS_BUSY | TS_TIMEOUT | TS_TTSTOP))
		return;
	if (!ttypull(tp))
		return;

	SET(tp->t_state, TS_BUSY);

	while ((c = getc(&tp->t_outq)) != -1) {
		timo = 50000;
		while (!(bus_space_read_1(zsisa_iot, zsisa_ioh, ZSISA_CTRL)
		    & ZSRR0_TX_READY)) {
			DELAY(2);
			if (--timo == 0)
				break;
		}
		if (timo > 0) {
			bus_space_write_1(zsisa_iot, zsisa_ioh, ZSISA_DATA,
			    (uint8_t)c);
			DELAY(2);
		}
	}

	CLR(tp->t_state, TS_BUSY);
	ttypull(tp);
}

/* ===================================================================== */
/* t_param: SRM already configured 9600/8N1; we don't touch the chip     */
/* ===================================================================== */

static int
zsisa_tty_param(struct tty *tp, struct termios *t)
{
	tp->t_ispeed = tp->t_ospeed = 9600;
	ttsetwater(tp);
	return 0;
}

/* ===================================================================== */
/* RX poll callout: runs every tick (1024 Hz), feeds chars to tty        */
/* ===================================================================== */

static void
zsisa_rx_poll(void *arg)
{
	struct zsisa_softc *sc = arg;
	struct tty *tp = sc->sc_tty;
	int c, s;

	s = spltty();
	if (ISSET(tp->t_state, TS_ISOPEN)) {
		while (bus_space_read_1(zsisa_iot, zsisa_ioh, ZSISA_CTRL)
		    & ZSRR0_RX_READY) {
			ZSISA_DELAY();
			c = (int)(uint8_t)bus_space_read_1(zsisa_iot, zsisa_ioh,
			    ZSISA_DATA);
			(*tp->t_linesw->l_rint)(c, tp);
		}
	}
	splx(s);

	callout_schedule(&sc->sc_rx_co, 1);
}
