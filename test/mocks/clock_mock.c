#include "clock_mock.h"
#include <sys/time.h>

static time_t s_now;
static time_t s_last_settimeofday;

void mock_clock_reset(void)
{
    s_now = 0;
    s_last_settimeofday = 0;
}

void mock_clock_set(time_t t)
{
    s_now = t;
}

time_t mock_clock_get(void)
{
    return s_now;
}

time_t mock_clock_get_last_settimeofday(void)
{
    return s_last_settimeofday;
}

#ifdef _WIN32
time_t __wrap__time64(time_t *t)
#else
time_t __wrap_time(time_t *t)
#endif
{
    if (t) *t = s_now;
    return s_now;
}

int __wrap_settimeofday(const struct timeval *tv, const struct timezone *tz)
{
    (void)tz;
    if (tv) {
        s_now = tv->tv_sec;
        s_last_settimeofday = tv->tv_sec;
    }
    return 0;
}