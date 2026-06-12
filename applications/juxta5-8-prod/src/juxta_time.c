#include "juxta_time.h"

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

static atomic_t base_unix;
static int64_t base_uptime_ms;

/* Retained-RAM RTC checkpoint.  See juxta_time.h for design rationale.
 *
 * The struct is intentionally placed in `.noinit` (uninitialised section) so
 * it survives soft resets (DOG / LOCKUP / SREQ) but its contents are random
 * after a true cold boot — the magic + CRC pair below is what makes the
 * difference observable.
 *
 * Layout note: the CRC32 is the last field and covers all preceding bytes.
 * `_pad` is explicit so the struct shape is stable across compilers. */
#define JUXTA_TIME_RETAINED_MAGIC 0x4A585253U /* 'JXRS' */
#define JUXTA_TIME_RETAINED_VERSION 1U

struct juxta_time_retained
{
	uint32_t magic;
	uint8_t version;
	uint8_t _pad[3];
	uint32_t unix_time;
	int64_t uptime_ms;
	uint32_t crc32;
};

static struct juxta_time_retained s_retained __noinit;

static size_t retained_crc_span(void)
{
	return offsetof(struct juxta_time_retained, crc32);
}

static uint32_t retained_compute_crc(const struct juxta_time_retained *r)
{
	return crc32_ieee((const uint8_t *)r, retained_crc_span());
}

static bool is_leap(int year)
{
	return ((year % 4) == 0 && ((year % 100) != 0 || (year % 400) == 0));
}

void juxta_time_init(void)
{
	base_uptime_ms = k_uptime_get();
	atomic_set(&base_unix, 0);
}

void juxta_time_set(uint32_t unix_time)
{
	base_uptime_ms = k_uptime_get();
	atomic_set(&base_unix, (atomic_val_t)unix_time);
}

uint32_t juxta_time_now(void)
{
	uint32_t unix_time = (uint32_t)atomic_get(&base_unix);

	if (unix_time == 0U) {
		return 0U;
	}

	return unix_time + (uint32_t)((k_uptime_get() - base_uptime_ms) / 1000);
}

void juxta_time_ymd(uint32_t unix_time, int *year, int *month, int *day)
{
	static const uint8_t month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	uint32_t days = unix_time / 86400U;
	int y = 1970;

	while (true) {
		uint32_t diy = is_leap(y) ? 366U : 365U;

		if (days < diy) {
			break;
		}
		days -= diy;
		y++;
	}

	int m = 1;
	for (size_t i = 0; i < ARRAY_SIZE(month_days); i++) {
		uint32_t dim = month_days[i];

		if (i == 1 && is_leap(y)) {
			dim++;
		}
		if (days < dim) {
			m = (int)i + 1;
			break;
		}
		days -= dim;
	}

	*year = y;
	*month = m;
	*day = (int)days + 1;
}

void juxta_time_date_string(uint32_t unix_time, char out[9])
{
	int year;
	int month;
	int day;

	if (unix_time == 0U) {
		memcpy(out, "19700101", 9);
		return;
	}

	juxta_time_ymd(unix_time, &year, &month, &day);
	(void)snprintf(out, 9, "%04d%02d%02d", year, month, day);
}

void juxta_time_retained_update(void)
{
	uint32_t now = juxta_time_now();

	/* Never checkpoint a placeholder clock; the recovery branch would not
	 * meaningfully restore anything from an unset RTC. */
	if (now == 0U) {
		return;
	}

	s_retained.magic = JUXTA_TIME_RETAINED_MAGIC;
	s_retained.version = JUXTA_TIME_RETAINED_VERSION;
	s_retained._pad[0] = 0U;
	s_retained._pad[1] = 0U;
	s_retained._pad[2] = 0U;
	s_retained.unix_time = now;
	s_retained.uptime_ms = k_uptime_get();
	s_retained.crc32 = retained_compute_crc(&s_retained);
}

bool juxta_time_retained_valid(void)
{
	if (s_retained.magic != JUXTA_TIME_RETAINED_MAGIC) {
		return false;
	}
	if (s_retained.version != JUXTA_TIME_RETAINED_VERSION) {
		return false;
	}
	if (s_retained.unix_time == 0U) {
		return false;
	}
	return s_retained.crc32 == retained_compute_crc(&s_retained);
}

uint32_t juxta_time_retained_unix(void)
{
	if (!juxta_time_retained_valid()) {
		return 0U;
	}

	/* Project the snapshot forward by however long ago it was taken,
	 * relative to the *current* kernel uptime.  The kernel clock is
	 * preserved across the snapshot boundary only when the reset path
	 * leaves the SoC powered (DOG / LOCKUP / SREQ); after such a reset
	 * uptime_ms restarts at 0, so the delta is simply k_uptime_get(). */
	int64_t delta_ms = k_uptime_get();
	if (delta_ms < 0) {
		delta_ms = 0;
	}

	return s_retained.unix_time + (uint32_t)(delta_ms / 1000);
}

void juxta_time_retained_invalidate(void)
{
	/* Clearing the magic is enough to make juxta_time_retained_valid()
	 * fail without paying for a CRC recompute; zero the rest so RTT
	 * dumps are not misleading. */
	s_retained.magic = 0U;
	s_retained.version = 0U;
	s_retained.unix_time = 0U;
	s_retained.uptime_ms = 0;
	s_retained.crc32 = 0U;
}
