/* Shift only this test publisher's wall clock; monotonic timers are unchanged. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/time.h>
#include <time.h>

int clock_gettime(clockid_t clock, struct timespec *value)
{
  static int (*original)(clockid_t, struct timespec *);
  if (!original) original = dlsym(RTLD_NEXT, "clock_gettime");
  int result = original(clock, value);
  if (!result && (clock == CLOCK_REALTIME || clock == CLOCK_REALTIME_COARSE)) {
    value->tv_sec -= 60;
  }
  return result;
}

int gettimeofday(struct timeval *value, void *timezone)
{
  static int (*original)(struct timeval *, void *);
  if (!original) original = dlsym(RTLD_NEXT, "gettimeofday");
  int result = original(value, timezone);
  if (!result) value->tv_sec -= 60;
  return result;
}
