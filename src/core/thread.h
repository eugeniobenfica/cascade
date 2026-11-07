/* Threads, locks and atomics over Win32 and POSIX threads: the little the library needs (the
 * paint bands, the first scan of the system fonts, the caches they share), and the workers that
 * run its parallel loops. Not installed. */
#ifndef CASCADE_THREAD_H
#define CASCADE_THREAD_H

#include "cascade/core.h"

#ifdef _WIN32
typedef struct CscdMutex { void* srw; } CscdMutex;       /* an SRWLOCK */
typedef struct CscdRwLock { void* srw; } CscdRwLock;
typedef struct CscdCond { void* cv; } CscdCond;          /* a CONDITION_VARIABLE */
typedef struct CscdThread { void* handle; } CscdThread;
#define CSCD_MUTEX_INIT {0}
#define CSCD_RWLOCK_INIT {0}
#define CSCD_COND_INIT {0}
#else
#include <pthread.h>
typedef struct CscdMutex { pthread_mutex_t m; } CscdMutex;
typedef struct CscdRwLock { pthread_rwlock_t l; } CscdRwLock;
typedef struct CscdCond { pthread_cond_t c; } CscdCond;
typedef struct CscdThread { pthread_t t; } CscdThread;
#define CSCD_MUTEX_INIT {PTHREAD_MUTEX_INITIALIZER}
#define CSCD_RWLOCK_INIT {PTHREAD_RWLOCK_INITIALIZER}
#define CSCD_COND_INIT {PTHREAD_COND_INITIALIZER}
#endif

/* Start each one with its Init function or its CSCD_..._INIT (a zeroed one is not enough on every
 * system); static ones need no destroy. */
void cscdMutexInit(CscdMutex* m);
void cscdMutexDestroy(CscdMutex* m);
void cscdMutexLock(CscdMutex* m);
bool cscdMutexTryLock(CscdMutex* m); /* false: another thread holds it */
void cscdMutexUnlock(CscdMutex* m);
void cscdRwLockInit(CscdRwLock* l);
void cscdRwLockDestroy(CscdRwLock* l);
void cscdRwLockRead(CscdRwLock* l);
void cscdRwUnlockRead(CscdRwLock* l);
void cscdRwLockWrite(CscdRwLock* l);
void cscdRwUnlockWrite(CscdRwLock* l);
void cscdCondInit(CscdCond* c);
void cscdCondDestroy(CscdCond* c);
void cscdCondWait(CscdCond* c, CscdMutex* m);
void cscdCondBroadcast(CscdCond* c);

/* A recursive lock (the font manager calls itself while it holds its lock). */
typedef struct CscdRecMutex { CscdMutex m; volatile size_t owner; int depth; } CscdRecMutex;
void cscdRecMutexLock(CscdRecMutex* m);
void cscdRecMutexUnlock(CscdRecMutex* m);

typedef void (*CscdThreadFn)(void* arg);
bool cscdThreadStart(CscdThread* t, CscdThreadFn fn, void* arg); /* false: no thread was made */
void cscdThreadJoin(CscdThread* t);
/* Names the calling thread for debuggers and profilers (cut to 15 bytes where the system wants). */
void cscdThreadSetName(const char* name);
/* The processors this process may run on (its affinity and its container's limit on Linux). */
unsigned cscdHardwareThreads(void);
uintptr_t cscdThreadId(void);

/* fn(ctx, 0) ... fn(ctx, count - 1), on up to threads threads (the calling one included), returning
 * when all are done. The workers are kept for the next loops. A loop asked for inside another, or
 * while another thread runs one, runs on the calling thread alone: it never waits for the workers
 * and never deadlocks. */
typedef void (*CscdParallelFn)(void* ctx, int i);
void cscdParallelFor(int count, int threads, CscdParallelFn fn, void* ctx);

/* Calls fn once, whatever the threads that get there (those that come while it runs wait for it,
 * asleep). */
typedef struct CscdOnce { volatile int state; } CscdOnce;
#define CSCD_ONCE_INIT {0}
void cscdCallOnce(CscdOnce* once, void (*fn)(void));
void cscdCallOnceArg(CscdOnce* once, void (*fn)(void* arg), void* arg);

/* Atomics, sequentially consistent. */
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
CSCD_INLINE int cscdAtomicLoad(volatile int* p) { return _InterlockedOr((volatile long*)p, 0); }
CSCD_INLINE void cscdAtomicStore(volatile int* p, int v) { _InterlockedExchange((volatile long*)p, v); }
CSCD_INLINE int cscdAtomicAdd(volatile int* p, int v) { return _InterlockedExchangeAdd((volatile long*)p, v) + v; }
CSCD_INLINE int cscdAtomicFetchAdd(volatile int* p, int v) { return _InterlockedExchangeAdd((volatile long*)p, v); }
CSCD_INLINE bool cscdAtomicCas(volatile int* p, int expected, int desired) { return _InterlockedCompareExchange((volatile long*)p, desired, expected) == expected; }
CSCD_INLINE uint32_t cscdAtomicLoadU32(volatile uint32_t* p) { return (uint32_t)_InterlockedOr((volatile long*)p, 0); }
CSCD_INLINE uint32_t cscdAtomicFetchAddU32(volatile uint32_t* p, uint32_t v) { return (uint32_t)_InterlockedExchangeAdd((volatile long*)p, (long)v); }
CSCD_INLINE size_t cscdAtomicFetchAddZ(volatile size_t* p, size_t v) { return (size_t)_InterlockedExchangeAdd64((volatile long long*)p, (long long)v); }
CSCD_INLINE size_t cscdAtomicLoadZ(volatile size_t* p) { return (size_t)_InterlockedOr64((volatile long long*)p, 0); }
CSCD_INLINE void cscdAtomicStoreZ(volatile size_t* p, size_t v) { _InterlockedExchange64((volatile long long*)p, (long long)v); }
#else
CSCD_INLINE int cscdAtomicLoad(volatile int* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
CSCD_INLINE void cscdAtomicStore(volatile int* p, int v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
CSCD_INLINE int cscdAtomicAdd(volatile int* p, int v) { return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST); }
CSCD_INLINE int cscdAtomicFetchAdd(volatile int* p, int v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
CSCD_INLINE bool cscdAtomicCas(volatile int* p, int expected, int desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
CSCD_INLINE uint32_t cscdAtomicLoadU32(volatile uint32_t* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
CSCD_INLINE uint32_t cscdAtomicFetchAddU32(volatile uint32_t* p, uint32_t v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
CSCD_INLINE size_t cscdAtomicFetchAddZ(volatile size_t* p, size_t v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
CSCD_INLINE size_t cscdAtomicLoadZ(volatile size_t* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
CSCD_INLINE void cscdAtomicStoreZ(volatile size_t* p, size_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
#endif

/* Thread-local storage for a variable of the library. */
#if defined(_MSC_VER) && !defined(__clang__)
#define CSCD_THREAD_LOCAL __declspec(thread)
#else
#define CSCD_THREAD_LOCAL _Thread_local
#endif

/* A monotonic clock, in seconds. */
double cscdNowSeconds(void);

/* Files by UTF-8 paths, the paths the library takes everywhere (files.c): on Windows through the
 * UTF-16 calls, so that names outside the system's code page open too. */
#include <stdio.h>
FILE* cscdFileOpen(CscdSv path, const char* mode);
/* An environment variable as UTF-8, empty when it is not set. */
CscdStr cscdEnv(const char* name);
#ifdef _WIN32
#include <wchar.h>
wchar_t* cscdWidePath(CscdSv path); /* (cscdFree it) */
void cscdAppendWide(CscdStr* out, const wchar_t* w);
#endif

#endif
