#ifndef HOST_COMPAT_H
#define HOST_COMPAT_H

/*
 * Declarations for POSIX functions that exist on ESP-IDF/Linux
 * but are missing from MinGW-w64 UCRT.
 */

#ifdef _WIN32
struct timeval;
struct timezone;

int settimeofday(const struct timeval *tv, const struct timezone *tz);

#endif /* _WIN32 */

#endif /* HOST_COMPAT_H */