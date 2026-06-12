#ifndef JUXTA_TIME_H_
#define JUXTA_TIME_H_

#include <stdbool.h>
#include <stdint.h>

void juxta_time_init(void);
void juxta_time_set(uint32_t unix_time);
uint32_t juxta_time_now(void);
void juxta_time_ymd(uint32_t unix_time, int *year, int *month, int *day);
void juxta_time_date_string(uint32_t unix_time, char out[9]);

/* Retained-RAM RTC checkpoint --------------------------------------------
 *
 * The struct lives in a `.noinit` section so it survives soft resets
 * (watchdog, lockup, soft-request reboot) but is wiped by System OFF and
 * cold boot — exactly the boundary the production-recovery boot branch
 * needs.  A 1 s k_timer in main.c calls juxta_time_retained_update() to
 * keep the checkpoint fresh; juxta_time_retained_valid() guards the
 * recovery path with a magic + version + CRC32 check, and
 * juxta_time_retained_unix() projects the snapshot forward using the
 * kernel uptime so the recovered RTC is accurate to ≤1 s. */
void juxta_time_retained_update(void);
bool juxta_time_retained_valid(void);
uint32_t juxta_time_retained_unix(void);
void juxta_time_retained_invalidate(void);

#endif /* JUXTA_TIME_H_ */
