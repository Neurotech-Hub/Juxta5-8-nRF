#ifndef JUXTA_TIME_H_
#define JUXTA_TIME_H_

#include <stdint.h>

void juxta_time_init(void);
void juxta_time_set(uint32_t unix_time);
uint32_t juxta_time_now(void);
void juxta_time_ymd(uint32_t unix_time, int *year, int *month, int *day);
void juxta_time_date_string(uint32_t unix_time, char out[9]);

#endif /* JUXTA_TIME_H_ */
