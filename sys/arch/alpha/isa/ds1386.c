/* $NetBSD$ */

/*
 * DS1386 TOY clock driver for DEC AXPvme 230.
 *
 * The AXPvme 230 uses a Dallas DS1386 real-time clock at ISbus address
 * 0x8000.  Unlike the MC146818 (which uses indexed register access via
 * I/O ports 0x70/0x71), the DS1386 exposes each register as a directly
 * addressed byte — register N is read/written at bus offset N, similar
 * to memory-mapped I/O.
 *
 * This driver:
 *   1. Provides a todr_chip_handle for system time get/set.
 *   2. Calls clockattach() so that cpu_initclocks() can proceed.
 *      The DS1386 SQW output already runs at 1024 Hz (configured by SRM),
 *      driving HBEAT → SIO IRQ1 → PAL ALPHA_INTR_CLOCK, so no additional
 *      hardware initialisation is required in the clock_init callback.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/device.h>
#include <sys/systm.h>
#include <sys/cpu.h>

#include <sys/bus.h>
#include <dev/clock_subr.h>
#include <dev/isa/isavar.h>

#include <machine/rpb.h>	/* cputype, ST_DEC_AXPVME_64 */

#include <alpha/alpha/clockvar.h>

/* DS1386 register offsets (direct-mapped, one byte per register) */
#define DS1386_CSEC	0x00	/* 1/100 second, BCD (read-only) */
#define DS1386_SEC	0x01	/* seconds, BCD 00-59 */
#define DS1386_SECALRM	0x02	/* seconds alarm */
#define DS1386_MIN	0x03	/* minutes, BCD 00-59 */
#define DS1386_MINALRM	0x04	/* minutes alarm */
#define DS1386_HOUR	0x05	/* hours, BCD 00-23 (or 01-12 + AM/PM) */
#define DS1386_HOURALRM	0x06	/* hours alarm */
#define DS1386_DOW	0x07	/* day of week, BCD 01-07 */
#define DS1386_DOM	0x08	/* day of month, BCD 01-31 */
#define DS1386_MON	0x09	/* month, BCD 01-12; bit7=OSC_DIS, bit6=SQW_DIS */
#define DS1386_YEAR	0x0A	/* year, BCD 00-99 */
#define DS1386_CMDA	0x0B	/* command register A (interrupt flags/enables) */
#define DS1386_CMDB	0x0C	/* command register B (watchdog) */

#define DS1386_CLK_SIZE	0x10	/* bytes needed for clock registers */

/* Month register control bits */
#define DS1386_MON_OSC_DIS	0x80	/* set = oscillator disabled */
#define DS1386_MON_SQW_DIS	0x40	/* set = SQW output disabled */

/* Hours register mode bit */
#define DS1386_HOUR_12HR	0x40	/* set = 12-hour mode */
#define DS1386_HOUR_PM		0x20	/* set = PM (in 12-hour mode) */

struct ds1386_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
	struct todr_chip_handle	sc_todr;
};

static int	ds1386_match(device_t, cfdata_t, void *);
static void	ds1386_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(axprtc, sizeof(struct ds1386_softc),
    ds1386_match, ds1386_attach, NULL, NULL);

static int	ds1386_gettime(todr_chip_handle_t, struct clock_ymdhms *);
static int	ds1386_settime(todr_chip_handle_t, struct clock_ymdhms *);
static void	ds1386_clock_init(void *);

static int
ds1386_match(device_t parent, cfdata_t cf, void *aux)
{
	struct isa_attach_args *ia = aux;
	bus_space_handle_t ioh;

	/* DS1386 only present on AXPvme 230 */
	if (cputype != ST_DEC_AXPVME_64)
		return 0;

	if (ia->ia_nio < 1 ||
	    (ia->ia_io[0].ir_addr != ISA_UNKNOWN_PORT &&
	     ia->ia_io[0].ir_addr != 0x8000))
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

	if (bus_space_map(ia->ia_iot, 0x8000, DS1386_CLK_SIZE, 0, &ioh))
		return 0;

	bus_space_unmap(ia->ia_iot, ioh, DS1386_CLK_SIZE);

	ia->ia_nio = 1;
	ia->ia_io[0].ir_addr = 0x8000;
	ia->ia_io[0].ir_size = DS1386_CLK_SIZE;
	ia->ia_niomem = 0;
	ia->ia_nirq = 0;
	ia->ia_ndrq = 0;

	return 1;
}

static void
ds1386_attach(device_t parent, device_t self, void *aux)
{
	struct ds1386_softc *sc = device_private(self);
	struct isa_attach_args *ia = aux;
	uint8_t mon;

	sc->sc_dev = self;
	sc->sc_iot = ia->ia_iot;

	if (bus_space_map(sc->sc_iot, ia->ia_io[0].ir_addr,
	    ia->ia_io[0].ir_size, 0, &sc->sc_ioh)) {
		aprint_error(": can't map I/O space\n");
		return;
	}

	/*
	 * Ensure the oscillator and SQW output are enabled.
	 * SRM normally leaves these enabled (it uses the 1024 Hz SQW for
	 * HBEAT), but check and correct in case the battery was pulled.
	 */
	mon = bus_space_read_1(sc->sc_iot, sc->sc_ioh, DS1386_MON);
	if (mon & (DS1386_MON_OSC_DIS | DS1386_MON_SQW_DIS)) {
		mon &= ~(DS1386_MON_OSC_DIS | DS1386_MON_SQW_DIS);
		bus_space_write_1(sc->sc_iot, sc->sc_ioh, DS1386_MON, mon);
		aprint_normal(": enabling oscillator+SQW");
	}

	aprint_normal(": DS1386 real-time clock\n");

	/* Register as TOY clock */
	sc->sc_todr.todr_dev = self;
	sc->sc_todr.todr_devaux = sc;
	sc->sc_todr.todr_gettime_ymdhms = ds1386_gettime;
	sc->sc_todr.todr_settime_ymdhms = ds1386_settime;
	sc->sc_todr.todr_setwen = NULL;
	todr_attach(&sc->sc_todr);

	/* Register clock init with the Alpha clock framework */
	clockattach(ds1386_clock_init, sc);
}

static int
ds1386_gettime(todr_chip_handle_t h, struct clock_ymdhms *dt)
{
	struct ds1386_softc *sc = h->todr_devaux;
	bus_space_tag_t iot = sc->sc_iot;
	bus_space_handle_t ioh = sc->sc_ioh;
	uint8_t mon, hour;

	mon = bus_space_read_1(iot, ioh, DS1386_MON);
	if (mon & DS1386_MON_OSC_DIS)
		return EINVAL;	/* oscillator stopped, time invalid */

	hour = bus_space_read_1(iot, ioh, DS1386_HOUR);
	if (hour & DS1386_HOUR_12HR) {
		/* 12-hour mode: convert to 24-hour */
		int h12 = bcdtobin(hour & 0x1f);
		dt->dt_hour = h12 % 12 + ((hour & DS1386_HOUR_PM) ? 12 : 0);
	} else {
		dt->dt_hour = bcdtobin(hour & 0x3f);
	}

	dt->dt_sec  = bcdtobin(bus_space_read_1(iot, ioh, DS1386_SEC)  & 0x7f);
	dt->dt_min  = bcdtobin(bus_space_read_1(iot, ioh, DS1386_MIN)  & 0x7f);
	dt->dt_wday = bcdtobin(bus_space_read_1(iot, ioh, DS1386_DOW)  & 0x07);
	dt->dt_day  = bcdtobin(bus_space_read_1(iot, ioh, DS1386_DOM)  & 0x3f);
	dt->dt_mon  = bcdtobin(mon & 0x1f);

	int year = bcdtobin(bus_space_read_1(iot, ioh, DS1386_YEAR));
	dt->dt_year = year + (year < 70 ? 2000 : 1900);

	return 0;
}

static int
ds1386_settime(todr_chip_handle_t h, struct clock_ymdhms *dt)
{
	struct ds1386_softc *sc = h->todr_devaux;
	bus_space_tag_t iot = sc->sc_iot;
	bus_space_handle_t ioh = sc->sc_ioh;
	uint8_t mon;

	/* Preserve OSC_DIS and SQW_DIS bits in month register */
	mon = bus_space_read_1(iot, ioh, DS1386_MON) &
	    (DS1386_MON_OSC_DIS | DS1386_MON_SQW_DIS);

	bus_space_write_1(iot, ioh, DS1386_SEC,
	    bintobcd(dt->dt_sec));
	bus_space_write_1(iot, ioh, DS1386_MIN,
	    bintobcd(dt->dt_min));
	bus_space_write_1(iot, ioh, DS1386_HOUR,	/* 24-hour format */
	    bintobcd(dt->dt_hour));
	bus_space_write_1(iot, ioh, DS1386_DOW,
	    bintobcd(dt->dt_wday));
	bus_space_write_1(iot, ioh, DS1386_DOM,
	    bintobcd(dt->dt_day));
	bus_space_write_1(iot, ioh, DS1386_MON,
	    mon | bintobcd(dt->dt_mon));
	bus_space_write_1(iot, ioh, DS1386_YEAR,
	    bintobcd(dt->dt_year % 100));

	return 0;
}

/*
 * ds1386_clock_init: called from cpu_initclocks() on each CPU.
 *
 * The DS1386 SQW output is already configured at 1024 Hz by the SRM
 * firmware.  The ALPHA_INTR_CLOCK signal comes from the HBEAT latch
 * (DS1386 SQW rising edge → SIO IRQ1 → PAL), so no additional
 * hardware programming is required here.  We simply confirm that the
 * SQW output is enabled on the primary CPU.
 */
static void
ds1386_clock_init(void *arg)
{
	struct ds1386_softc *sc = arg;
	uint8_t mon;

	kpreempt_disable();

	if (CPU_IS_PRIMARY(curcpu())) {
		mon = bus_space_read_1(sc->sc_iot, sc->sc_ioh, DS1386_MON);
		mon &= ~(DS1386_MON_OSC_DIS | DS1386_MON_SQW_DIS);
		bus_space_write_1(sc->sc_iot, sc->sc_ioh, DS1386_MON, mon);
	}

	kpreempt_enable();
}
