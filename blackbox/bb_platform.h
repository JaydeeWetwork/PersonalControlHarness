/* bb_platform.h - the few OS calls the Black Box prototype needs (thread, sleep,
 * fsync, monotonic clock) on POSIX and Windows (MSVC), so it builds on both. */
#ifndef LL_BB_PLATFORM_H
#define LL_BB_PLATFORM_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <process.h>
typedef HANDLE bb_thread;
typedef struct { void *(*fn)(void *); void *arg; } bb__thunk;
static inline unsigned __stdcall bb__trampoline(void *p) { bb__thunk t = *(bb__thunk *)p; free(p); t.fn(t.arg); return 0; }
static inline int bb_thread_start(bb_thread *t, void *(*fn)(void *), void *arg) {
  bb__thunk *k = (bb__thunk *)malloc(sizeof *k); if (!k) return -1; k->fn = fn; k->arg = arg;
  uintptr_t h = _beginthreadex(NULL, 0, bb__trampoline, k, 0, NULL);
  if (!h) { free(k); return -1; } *t = (HANDLE)h; return 0; }
static inline void bb_thread_join(bb_thread t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
/* Windows sleeps in whole ms; 0 would just yield, so round up to 1 ms. */
static inline void bb_sleep_us(unsigned us) { Sleep(us >= 1000 ? us / 1000 : 1); }
static inline void bb_fsync(FILE *f) { _commit(_fileno(f)); }
static inline double bb_now_s(void) { LARGE_INTEGER c, fq; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&fq); return (double)c.QuadPart / (double)fq.QuadPart; }
static inline long bb_pid(void) { return (long)_getpid(); }
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
typedef pthread_t bb_thread;
static inline int bb_thread_start(bb_thread *t, void *(*fn)(void *), void *arg) { return pthread_create(t, NULL, fn, arg) ? -1 : 0; }
static inline void bb_thread_join(bb_thread t) { pthread_join(t, NULL); }
static inline void bb_sleep_us(unsigned us) { struct timespec n = {us / 1000000u, (long)(us % 1000000u) * 1000L}; nanosleep(&n, NULL); }
static inline void bb_fsync(FILE *f) { fsync(fileno(f)); }
static inline double bb_now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static inline long bb_pid(void) { return (long)getpid(); }
#endif
#endif
