#ifndef COMPAT_H
#define COMPAT_H

#ifdef _WIN32

#ifdef _MSC_BUILD
#include <windows.h>
#include <process.h>

// MSVC lacks these POSIX headers - provide replacements

#ifndef __UNISTD_H__
#define __UNISTD_H__
#include <io.h>
#define access _access
#define R_OK 04
#define W_OK 02
#define F_OK 00
#endif

// Replace sys/time.h - timeval already defined by winsock.h on Windows
static inline int compat_gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    FILETIME ft;
    unsigned long long t;
    GetSystemTimeAsFileTime(&ft);
    t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ULL;
    t /= 10;
    tv->tv_sec = (long)(t / 1000000);
    tv->tv_usec = (long)(t % 1000000);
    return 0;
}
#define gettimeofday compat_gettimeofday

// Replace pthread.h with Windows threads
typedef struct {
    HANDLE handle;
    unsigned id;
    void *(*start_routine)(void *);
    void *arg;
    void *result;
} pthread_t;

typedef CRITICAL_SECTION pthread_mutex_t;
typedef CRITICAL_SECTION pthread_cond_t;

#define PTHREAD_MUTEX_INITIALIZER {0}

static unsigned __stdcall thread_wrapper(void *arg) {
    pthread_t *pt = (pthread_t *)arg;
    pt->result = pt->start_routine(pt->arg);
    return 0;
}

static inline int pthread_create(pthread_t *thread, void *attr, void *(*start_routine)(void *), void *arg) {
    (void)attr;
    thread->start_routine = start_routine;
    thread->arg = arg;
    thread->handle = (HANDLE)_beginthreadex(NULL, 0, thread_wrapper, thread, 0, &thread->id);
    return thread->handle ? 0 : -1;
}

static inline int pthread_join(pthread_t thread, void **retval) {
    WaitForSingleObject(thread.handle, INFINITE);
    if (retval) *retval = thread.result;
    CloseHandle(thread.handle);
    return 0;
}

static inline int pthread_detach(pthread_t thread) {
    CloseHandle(thread.handle);
    return 0;
}

static inline int pthread_mutex_init(pthread_mutex_t *mutex, void *attr) {
    (void)attr;
    InitializeCriticalSection(mutex);
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *mutex) {
    EnterCriticalSection(mutex);
    return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    LeaveCriticalSection(mutex);
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    DeleteCriticalSection(mutex);
    return 0;
}

static inline int pthread_cond_init(pthread_cond_t *cond, void *attr) {
    (void)attr;
    InitializeCriticalSection(cond);
    return 0;
}

static inline int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    LeaveCriticalSection(mutex);
    EnterCriticalSection(cond);
    Sleep(100);
    LeaveCriticalSection(cond);
    EnterCriticalSection(mutex);
    return 0;
}

static inline int pthread_cond_signal(pthread_cond_t *cond) {
    (void)cond;
    return 0;
}

static inline int pthread_cond_destroy(pthread_cond_t *cond) {
    DeleteCriticalSection(cond);
    return 0;
}

#endif // _MSC_BUILD

#else
// Linux/macOS: POSIX headers available natively
#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>
#endif // _WIN32

#endif // COMPAT_H
