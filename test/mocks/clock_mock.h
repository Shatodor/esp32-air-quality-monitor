#ifndef CLOCK_MOCK_H
#define CLOCK_MOCK_H

#include <time.h>

void   mock_clock_reset(void);
void   mock_clock_set(time_t t);
time_t mock_clock_get(void);
time_t mock_clock_get_last_settimeofday(void);

#endif