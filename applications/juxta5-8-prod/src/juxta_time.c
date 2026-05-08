#include "juxta_time.h"

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

static atomic_t base_unix;
static int64_t base_uptime_ms;

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
