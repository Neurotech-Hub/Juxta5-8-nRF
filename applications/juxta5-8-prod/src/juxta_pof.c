#include "juxta_pof.h"

#include <stddef.h>
#include <string.h>

#include <nrf.h>
#include <nrfx_power.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <zephyr/toolchain.h>

#include "juxta_time.h"

LOG_MODULE_REGISTER(juxta_pof, LOG_LEVEL_INF);

/* VDDH warning threshold.  The pack sits at ~3.0–4.35 V on VDDH; 3.6 V is
 * well below any healthy operating point yet far above the 2.75 V brownout,
 * so a trip means a genuine transient sag (radio TX + NOR program stack-up)
 * rather than an exhausted battery.
 *
 * The VDD threshold guards the *internal regulated rail*, which runs at the
 * REGOUT0 default of 1.8 V on this board — it MUST be set below that.  A
 * threshold at/above the rail keeps POFWARN permanently asserted, and the
 * errata-242 guard in soc_flash_nrf.c then cancels every internal-flash
 * (NVS) write with -ECANCELED.  1.7 V trips only on a genuine VDD collapse. */
#define JUXTA_POF_THRVDDH NRF_POWER_POFTHRVDDH_V36
#define JUXTA_POF_THR NRF_POWER_POFTHR_V17

/* Retained witness — same magic + CRC pattern as juxta_time_retained. */
#define JUXTA_POF_RETAINED_MAGIC 0x4A585057U /* 'JXPW' */
#define JUXTA_POF_RETAINED_VERSION 1U

struct juxta_pof_retained
{
	uint32_t magic;
	uint8_t version;
	uint8_t _pad[3];
	struct juxta_pof_witness w;
	uint32_t crc32;
};

static struct juxta_pof_retained s_retained __noinit;

/* Previous-life witness snapshotted before s_retained is reset for this
 * life; consumed by main.c once the clock is valid. */
static struct juxta_pof_witness s_prev_life;
static bool s_prev_life_valid;

static size_t retained_crc_span(void)
{
	return offsetof(struct juxta_pof_retained, crc32);
}

static uint32_t retained_compute_crc(const struct juxta_pof_retained *r)
{
	return crc32_ieee((const uint8_t *)r, retained_crc_span());
}

static bool retained_valid(const struct juxta_pof_retained *r)
{
	return r->magic == JUXTA_POF_RETAINED_MAGIC &&
		   r->version == JUXTA_POF_RETAINED_VERSION && r->w.count > 0U &&
		   r->crc32 == retained_compute_crc(r);
}

static void retained_reset(void)
{
	memset(&s_retained, 0, sizeof(s_retained));
	s_retained.magic = JUXTA_POF_RETAINED_MAGIC;
	s_retained.version = JUXTA_POF_RETAINED_VERSION;
	s_retained.crc32 = retained_compute_crc(&s_retained);
}

/* POWER_CLOCK IRQ context (dispatched via nrfx_power_irq_handler).  Keep it
 * to a struct update + CRC: no logging, no kernel objects. */
static void pof_warn_handler(void)
{
	int64_t up = k_uptime_get();
	uint32_t now = juxta_time_now();

	if (s_retained.w.count == 0U)
	{
		s_retained.w.first_uptime_ms = up;
		s_retained.w.first_unix = now;
	}
	s_retained.w.count++;
	s_retained.w.last_uptime_ms = up;
	s_retained.w.last_unix = now;
	s_retained.crc32 = retained_compute_crc(&s_retained);
}

void juxta_pof_init(void)
{
	if (retained_valid(&s_retained))
	{
		s_prev_life = s_retained.w;
		s_prev_life_valid = true;
		LOG_WRN("POF witness from previous life: count=%u first_up=%lld ms last_up=%lld ms",
				s_prev_life.count, (long long)s_prev_life.first_uptime_ms,
				(long long)s_prev_life.last_uptime_ms);
	}

	retained_reset();

	/* Bench guard: with a J-Link attached the board is typically powered
	 * without a battery, so VDDH sits below the 3.6 V threshold and
	 * POFWARN would assert permanently — spamming the witness and letting
	 * the errata-242 guard in soc_flash_nrf.c cancel NVS writes with
	 * -ECANCELED.  Same debugger gate as the battery checks in main.c. */
	if ((CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) != 0U)
	{
		LOG_INF("POF not armed (debugger attached)");
		return;
	}

	static const nrfx_power_pofwarn_config_t pof_config = {
		.handler = pof_warn_handler,
		.thr = JUXTA_POF_THR,
		.thrvddh = JUXTA_POF_THRVDDH,
	};

	nrfx_power_pof_init(&pof_config);
	nrfx_power_pof_enable(&pof_config);
	LOG_INF("POF armed: VDDH thr 3.6 V, VDD thr 1.7 V");
}

bool juxta_pof_hit_this_life(void)
{
	return retained_valid(&s_retained);
}

bool juxta_pof_prev_life(struct juxta_pof_witness *out)
{
	if (!s_prev_life_valid || !out)
	{
		return false;
	}

	*out = s_prev_life;
	return true;
}

void juxta_pof_prev_life_clear(void)
{
	memset(&s_prev_life, 0, sizeof(s_prev_life));
	s_prev_life_valid = false;
}

void juxta_pof_disarm(void)
{
	nrfx_power_pof_disable();
}
