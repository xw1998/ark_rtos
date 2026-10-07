/**
* @file arkrtos_win_compat.h
* @brief Windows style names for programs written against the Win32 API.
* @details Sits on top of arkrtos_posix.h so a snippet written for Windows can
*          be moved over with a changed include list instead of a rewrite.
*          Only the calls that have a real counterpart here are provided; the
*          ones that would need a process, a registry or a filesystem are
*          deliberately absent, so their use fails to compile rather than
*          failing mysteriously at run time.
*
*          Two mapping notes worth knowing up front:
*            - CreateThread's start routine is a DWORD(*)(LPVOID) while the
*              thread service wants void*(*)(void*). Both pass their argument
*              and result in r0 under AAPCS, so the pointer is converted and
*              called directly; WINAPI is empty on ARM.
*            - WaitForSingleObject only understands a thread handle joined
*              with INFINITE. Any other handle or timeout reports failure.
*
* @author xw
*/
#ifndef __ARKRTOS_WIN_COMPAT_H__
#define __ARKRTOS_WIN_COMPAT_H__

#include "arkrtos_posix.h"

typedef uint8  arkrtos_WORD;
typedef uint32 arkrtos_DWORD;
typedef int32  arkrtos_BOOL;
typedef void  *arkrtos_HANDLE;
typedef void  *arkrtos_LPVOID;
typedef const char *arkrtos_LPCSTR;

#define WINAPI
#define TRUE   (1)
#define FALSE  (0)
#define INFINITE  (0xFFFFFFFFu)

#define WAIT_OBJECT_0  (0u)
#define WAIT_TIMEOUT   (0x102u)

/* ---- time and scheduling --------------------------------------------- */

#define Sleep(ms)          arkrtos_task_wait((uint32)(ms))
#define GetTickCount()     arkrtos_get_time_ms()

/* ---- threads ---------------------------------------------------------- */

arkrtos_HANDLE arkrtos_win_CreateThread(void *security, uint32 stack_size,
                                    arkrtos_DWORD (*start)(arkrtos_LPVOID),
                                    arkrtos_LPVOID arg, uint32 flags,
                                    uint32 *thread_id);
arkrtos_DWORD  arkrtos_win_WaitForSingleObject(arkrtos_HANDLE h, uint32 ms);
arkrtos_BOOL   arkrtos_win_CloseHandle(arkrtos_HANDLE h);

#define CreateThread(sec, stk, fn, arg, flg, id) \
        arkrtos_win_CreateThread((sec), (stk), (fn), (arg), (flg), (id))
#define WaitForSingleObject(h, ms)  arkrtos_win_WaitForSingleObject((h), (ms))
#define CloseHandle(h)              arkrtos_win_CloseHandle(h)

/* ---- critical sections ------------------------------------------------ */

typedef struct
{
    arkrtos_pthread_mutex_t mtx;
} arkrtos_CRITICAL_SECTION;

arkrtos_BOOL arkrtos_win_InitializeCriticalSection(arkrtos_CRITICAL_SECTION *cs);
void       arkrtos_win_EnterCriticalSection(arkrtos_CRITICAL_SECTION *cs);
void       arkrtos_win_LeaveCriticalSection(arkrtos_CRITICAL_SECTION *cs);
void       arkrtos_win_DeleteCriticalSection(arkrtos_CRITICAL_SECTION *cs);

#define InitializeCriticalSection(cs)  arkrtos_win_InitializeCriticalSection(cs)
#define EnterCriticalSection(cs)       arkrtos_win_EnterCriticalSection(cs)
#define LeaveCriticalSection(cs)       arkrtos_win_LeaveCriticalSection(cs)
#define DeleteCriticalSection(cs)      arkrtos_win_DeleteCriticalSection(cs)

/* ---- bounds checked string helpers ------------------------------------ */

int32      arkrtos_win_strcpy_s(char *dst, uint32 dst_size, const char *src);
int32      arkrtos_win_strcat_s(char *dst, uint32 dst_size, const char *src);
int        arkrtos_win_snprintf_s(char *dst, uint32 dst_size, const char *fmt, ...);

#define strcpy_s(d, n, s)     arkrtos_win_strcpy_s((d), (n), (s))
#define strcat_s(d, n, s)     arkrtos_win_strcat_s((d), (n), (s))
#define sprintf_s(d, n, ...)  arkrtos_win_snprintf_s((d), (n), __VA_ARGS__)
#define ZeroMemory(p, n)      arkrtos_posix_memset((p), 0, (n))

#endif /* __ARKRTOS_WIN_COMPAT_H__ */
