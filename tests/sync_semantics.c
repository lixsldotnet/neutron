/* neutron headless test: NT synchronization semantics (events, semaphores,
 * mutexes, multi-object waits, timeouts, APCs, SignalObjectAndWait,
 * cross-process objects, process and thread handles, stress).
 * Windows is the reference. Every wait has a finite timeout, a watchdog ends
 * the process after 60 s, so a broken implementation fails instead of hanging.
 * Console only, no windows.
 *
 * Prints "ok <name>", "FAIL <name>: <detail>" or, for checks where Wine itself
 * deviates from Windows, "skip <name>: wine" (only when running on Wine).
 * Last line: "sync_semantics: <passed>/<total> passed".
 * The exe also runs as its own child process ("child <mode> <arg>").
 *
 * Build: x86_64-w64-mingw32-clang -O2 -o sync_semantics.exe sync_semantics.c
 * Run:   tool/neutron runinprefix sync_semantics.exe
 * Exit code: 0 = all checks passed (skips allowed), 1 = a check failed,
 * 2 = watchdog. */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- ntdll (loaded at runtime, no import lib needed) ---- */

typedef LONG nt_status;
#define NT_SUCCESS_          0x00000000L
#define NT_ALERTED           0x00000101L
#define NT_TIMEOUT           0x00000102L
#define NT_USER_APC          0x000000C0L
#define NT_INVALID_PARAMETER     ((LONG)0xC000000DL)
#define NT_INVALID_PARAMETER_MIX ((LONG)0xC0000030L)

typedef struct { int EventType; LONG EventState; } ev_info;          /* EVENT_BASIC_INFORMATION */
typedef struct { ULONG CurrentCount, MaximumCount; } sem_info;       /* SEMAPHORE_BASIC_INFORMATION */
typedef struct { LONG CurrentCount; BOOLEAN OwnedByCaller; BOOLEAN AbandonedState; } mut_info; /* MUTANT_BASIC_INFORMATION */

static nt_status (WINAPI *pNtSetEvent)(HANDLE, LONG *);
static nt_status (WINAPI *pNtResetEvent)(HANDLE, LONG *);
static nt_status (WINAPI *pNtPulseEvent)(HANDLE, LONG *);
static nt_status (WINAPI *pNtQueryEvent)(HANDLE, int, void *, ULONG, ULONG *);
static nt_status (WINAPI *pNtQuerySemaphore)(HANDLE, int, void *, ULONG, ULONG *);
static nt_status (WINAPI *pNtQueryMutant)(HANDLE, int, void *, ULONG, ULONG *);
static nt_status (WINAPI *pNtWaitForSingleObject)(HANDLE, BOOLEAN, LARGE_INTEGER *);
static nt_status (WINAPI *pNtWaitForMultipleObjects)(ULONG, const HANDLE *, int, BOOLEAN, LARGE_INTEGER *);
static nt_status (WINAPI *pNtQuerySystemTime)(LARGE_INTEGER *);
static nt_status (WINAPI *pNtAlertThread)(HANDLE);

static int g_is_wine;

static void load_ntdll(void)
{
    HMODULE nt = GetModuleHandleA("ntdll.dll");
#define LOAD(f) p##f = (void *)GetProcAddress(nt, #f)
    LOAD(NtSetEvent); LOAD(NtResetEvent); LOAD(NtPulseEvent); LOAD(NtQueryEvent);
    LOAD(NtQuerySemaphore); LOAD(NtQueryMutant); LOAD(NtWaitForSingleObject);
    LOAD(NtWaitForMultipleObjects); LOAD(NtQuerySystemTime); LOAD(NtAlertThread);
#undef LOAD
    g_is_wine = GetProcAddress(nt, "wine_get_version") != NULL;
}

static ev_info query_event(HANDLE h)
{
    ev_info i = { -1, -1 };
    pNtQueryEvent(h, 0, &i, sizeof(i), NULL);
    return i;
}

static sem_info query_sem(HANDLE h)
{
    sem_info i = { 0xffffffff, 0xffffffff };
    pNtQuerySemaphore(h, 0, &i, sizeof(i), NULL);
    return i;
}

static mut_info query_mutant(HANDLE h)
{
    mut_info i = { -99, 99, 99 };
    pNtQueryMutant(h, 0, &i, sizeof(i), NULL);
    return i;
}

/* ---- check bookkeeping ---- */

static int g_pass, g_total, g_skip, g_fail;

typedef struct { const char *name; int bad; int known_wine; char detail[400]; char info[120]; } check_t;

static void check_fail(check_t *c, const char *fmt, ...)
{
    va_list ap;
    if (c->bad) return; /* keep the first failure */
    c->bad = 1;
    va_start(ap, fmt);
    vsnprintf(c->detail, sizeof(c->detail), fmt, ap);
    va_end(ap);
}

static void check_end(check_t *c)
{
    if (!c->bad) {
        g_total++; g_pass++;
        if (c->info[0]) printf("ok %s: %s\n", c->name, c->info);
        else printf("ok %s\n", c->name);
    } else if (c->known_wine && g_is_wine) {
        g_skip++;
        printf("skip %s: wine (%s)\n", c->name, c->detail);
    } else {
        g_total++; g_fail++;
        printf("FAIL %s: %s\n", c->name, c->detail);
    }
    fflush(stdout);
}

#define BEGIN(n) check_t c_ = { n, 0, 0, "", "" }
#define KNOWN_WINE() (c_.known_wine = 1)
#define EXPECT(cond, ...) do { if (!(cond)) check_fail(&c_, __VA_ARGS__); } while (0)
#define INFO(...) snprintf(c_.info, sizeof(c_.info), __VA_ARGS__)
#define END() check_end(&c_)

/* ---- helpers ---- */

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static HANDLE start_thread(LPTHREAD_START_ROUTINE fn, void *arg)
{
    return CreateThread(NULL, 0, fn, arg, 0, NULL);
}

/* Wait for a thread and close it. On timeout the thread is killed, so it can
 * not touch stack data of a finished check. Returns 1 if it ended in time. */
static int join(HANDLE t, DWORD ms)
{
    int ok = WaitForSingleObject(t, ms) == WAIT_OBJECT_0;
    if (!ok) {
        TerminateThread(t, 99);
        WaitForSingleObject(t, 2000);
    }
    CloseHandle(t);
    return ok;
}

static LONG wait_count(volatile LONG *v, LONG target, DWORD ms)
{
    double end = now_ms() + ms;
    while (*v < target && now_ms() < end) Sleep(1);
    return *v;
}

static const char *wr(DWORD r)
{
    static char buf[4][32];
    static int n;
    char *b = buf[n++ & 3];
    switch (r) {
    case WAIT_OBJECT_0: return "WAIT_OBJECT_0";
    case WAIT_TIMEOUT: return "WAIT_TIMEOUT";
    case WAIT_ABANDONED: return "WAIT_ABANDONED";
    case WAIT_IO_COMPLETION: return "WAIT_IO_COMPLETION";
    case WAIT_FAILED: snprintf(b, 32, "WAIT_FAILED(err %lu)", GetLastError()); return b;
    default: snprintf(b, 32, "0x%lx", r); return b;
    }
}

/* Generic waiter thread. */
struct waiter {
    HANDLE h;
    DWORD timeout;
    volatile LONG *woken;
    DWORD result;
};

static DWORD WINAPI waiter_proc(void *p)
{
    struct waiter *w = p;
    w->result = WaitForSingleObject(w->h, w->timeout);
    if (w->result == WAIT_OBJECT_0 && w->woken) InterlockedIncrement(w->woken);
    return 0;
}

static void start_waiters(struct waiter *w, HANDLE *th, int n, HANDLE h, DWORD timeout, volatile LONG *woken)
{
    for (int i = 0; i < n; i++) {
        w[i].h = h; w[i].timeout = timeout; w[i].woken = woken; w[i].result = 0xdead;
        th[i] = start_thread(waiter_proc, &w[i]);
    }
    Sleep(150); /* let them block */
}

static char g_exe[MAX_PATH];

static void obj_name(char *buf, size_t n, const char *tag)
{
    snprintf(buf, n, "neutron_sync_%lu_%s", GetCurrentProcessId(), tag);
}

static int spawn_child(const char *mode, const char *arg, DWORD flags, PROCESS_INFORMATION *pi)
{
    char cmd[MAX_PATH * 2];
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    snprintf(cmd, sizeof(cmd), "\"%s\" child %s %s", g_exe, mode, arg);
    return CreateProcessA(g_exe, cmd, NULL, NULL, FALSE, flags, NULL, NULL, &si, pi);
}

static DWORD exit_code(HANDLE proc)
{
    DWORD c = 0xdead;
    GetExitCodeProcess(proc, &c);
    return c;
}

/* APC bookkeeping */
static volatile LONG g_apc_count;
static DWORD g_apc_tid;
static ULONG_PTR g_apc_order[8];

static void CALLBACK apc_fn(ULONG_PTR p)
{
    LONG i = InterlockedIncrement(&g_apc_count);
    if (i <= 8) g_apc_order[i - 1] = p;
    g_apc_tid = GetCurrentThreadId();
}

static void apc_reset(void)
{
    g_apc_count = 0; g_apc_tid = 0;
    memset(g_apc_order, 0, sizeof(g_apc_order));
}

/* ---- events ---- */

static void test_event_auto(void)
{
    BEGIN("event_auto_reset_after_wait");
    HANDLE e = CreateEventA(NULL, FALSE, FALSE, NULL);
    SetEvent(e);
    EXPECT(query_event(e).EventState == 1, "state after SetEvent %ld", query_event(e).EventState);
    DWORD r1 = WaitForSingleObject(e, 0), r2 = WaitForSingleObject(e, 0);
    EXPECT(r1 == WAIT_OBJECT_0, "first wait %s", wr(r1));
    EXPECT(r2 == WAIT_TIMEOUT, "second wait %s (not reset)", wr(r2));
    EXPECT(query_event(e).EventState == 0, "state after wait %ld", query_event(e).EventState);
    CloseHandle(e);
    END();
}

static void test_event_auto_one_waiter(void)
{
    BEGIN("event_auto_set_wakes_one");
    HANDLE e = CreateEventA(NULL, FALSE, FALSE, NULL), th[2];
    struct waiter w[2];
    volatile LONG woken = 0;
    start_waiters(w, th, 2, e, 3000, &woken);
    SetEvent(e);
    wait_count(&woken, 1, 2000);
    Sleep(100);
    EXPECT(woken == 1, "after 1 SetEvent %ld waiters woke", woken);
    EXPECT(query_event(e).EventState == 0, "state after wake %ld", query_event(e).EventState);
    SetEvent(e);
    wait_count(&woken, 2, 2000);
    EXPECT(woken == 2, "after 2 SetEvent %ld waiters woke", woken);
    EXPECT(join(th[0], 4000) & join(th[1], 4000), "waiter did not end");
    EXPECT(w[0].result == WAIT_OBJECT_0 && w[1].result == WAIT_OBJECT_0, "results %s %s", wr(w[0].result), wr(w[1].result));
    EXPECT(WaitForSingleObject(e, 0) == WAIT_TIMEOUT, "event still signaled at the end");
    CloseHandle(e);
    END();
}

static void test_event_manual_all(void)
{
    BEGIN("event_manual_set_wakes_all");
    HANDLE e = CreateEventA(NULL, TRUE, FALSE, NULL), th[3];
    struct waiter w[3];
    volatile LONG woken = 0;
    start_waiters(w, th, 3, e, 3000, &woken);
    SetEvent(e);
    wait_count(&woken, 3, 2000);
    EXPECT(woken == 3, "%ld of 3 waiters woke", woken);
    for (int i = 0; i < 3; i++) join(th[i], 4000);
    DWORD r1 = WaitForSingleObject(e, 0), r2 = WaitForSingleObject(e, 0);
    EXPECT(r1 == WAIT_OBJECT_0 && r2 == WAIT_OBJECT_0, "manual event not staying signaled: %s %s", wr(r1), wr(r2));
    CloseHandle(e);
    END();
}

static void test_event_reset(void)
{
    BEGIN("event_reset");
    HANDLE m = CreateEventA(NULL, TRUE, TRUE, NULL), a = CreateEventA(NULL, FALSE, TRUE, NULL);
    EXPECT(ResetEvent(m) && ResetEvent(a), "ResetEvent failed err %lu", GetLastError());
    EXPECT(WaitForSingleObject(m, 0) == WAIT_TIMEOUT, "manual still signaled");
    EXPECT(WaitForSingleObject(a, 0) == WAIT_TIMEOUT, "auto still signaled");
    EXPECT(ResetEvent(m), "ResetEvent on unsignaled failed");
    EXPECT(query_event(m).EventState == 0, "manual state %ld", query_event(m).EventState);
    CloseHandle(m); CloseHandle(a);
    END();
}

static void test_pulse_manual(void)
{
    BEGIN("pulse_manual_wakes_all");
    HANDLE e = CreateEventA(NULL, TRUE, FALSE, NULL), th[3];
    struct waiter w[3];
    volatile LONG woken = 0;
    start_waiters(w, th, 3, e, 2000, &woken);
    PulseEvent(e);
    wait_count(&woken, 3, 1500);
    EXPECT(woken == 3, "%ld of 3 waiters woke", woken);
    EXPECT(query_event(e).EventState == 0, "state after pulse %ld", query_event(e).EventState);
    EXPECT(WaitForSingleObject(e, 0) == WAIT_TIMEOUT, "event signaled after pulse");
    for (int i = 0; i < 3; i++) join(th[i], 4000);
    CloseHandle(e);
    END();
}

static void test_pulse_auto(void)
{
    BEGIN("pulse_auto_wakes_one");
    HANDLE e = CreateEventA(NULL, FALSE, FALSE, NULL), th[2];
    struct waiter w[2];
    volatile LONG woken = 0;
    start_waiters(w, th, 2, e, 3000, &woken);
    PulseEvent(e);
    wait_count(&woken, 1, 1500);
    Sleep(100);
    EXPECT(woken == 1, "%ld waiters woke (expected 1)", woken);
    EXPECT(query_event(e).EventState == 0, "state after pulse %ld", query_event(e).EventState);
    SetEvent(e); /* release the other one */
    join(th[0], 4000); join(th[1], 4000);
    EXPECT(woken == 2, "second waiter not released by SetEvent (%ld)", woken);
    CloseHandle(e);
    END();
}

static void test_pulse_no_waiter(void)
{
    BEGIN("pulse_no_waiter");
    HANDLE m = CreateEventA(NULL, TRUE, FALSE, NULL), a = CreateEventA(NULL, FALSE, FALSE, NULL);
    PulseEvent(m); PulseEvent(a);
    EXPECT(WaitForSingleObject(m, 0) == WAIT_TIMEOUT, "manual signaled after pulse");
    EXPECT(WaitForSingleObject(a, 0) == WAIT_TIMEOUT, "auto signaled after pulse");
    LONG prev = -1;
    SetEvent(m);
    nt_status s = pNtPulseEvent(m, &prev);
    EXPECT(s == 0 && prev == 1, "NtPulseEvent on signaled: status 0x%lx prev %ld", s, prev);
    EXPECT(query_event(m).EventState == 0, "pulse on signaled manual event left state %ld", query_event(m).EventState);
    CloseHandle(m); CloseHandle(a);
    END();
}

static void test_nt_set_reset_prev(void)
{
    BEGIN("nt_set_reset_prev_state");
    for (int manual = 0; manual < 2; manual++) {
        HANDLE e = CreateEventA(NULL, manual, FALSE, NULL);
        LONG p1 = -1, p2 = -1, p3 = -1, p4 = -1;
        nt_status s1 = pNtSetEvent(e, &p1), s2 = pNtSetEvent(e, &p2);
        nt_status s3 = pNtResetEvent(e, &p3), s4 = pNtResetEvent(e, &p4);
        EXPECT(!s1 && !s2 && !s3 && !s4, "status %lx %lx %lx %lx", s1, s2, s3, s4);
        EXPECT(p1 == 0 && p2 == 1 && p3 == 1 && p4 == 0,
               "%s: prev set %ld,%ld reset %ld,%ld (expected 0,1 1,0)", manual ? "manual" : "auto", p1, p2, p3, p4);
        CloseHandle(e);
    }
    END();
}

static void test_nt_query_event(void)
{
    BEGIN("nt_query_event");
    HANDLE m = CreateEventA(NULL, TRUE, TRUE, NULL), a = CreateEventA(NULL, FALSE, FALSE, NULL);
    ev_info i;
    ULONG len = 0;
    nt_status s = pNtQueryEvent(m, 0, &i, sizeof(i), &len);
    EXPECT(s == 0 && len == sizeof(i), "status 0x%lx len %lu", s, len);
    EXPECT(i.EventType == 0 && i.EventState == 1, "manual set: type %d state %ld (expected 0 1)", i.EventType, i.EventState);
    i = query_event(a);
    EXPECT(i.EventType == 1 && i.EventState == 0, "auto unset: type %d state %ld (expected 1 0)", i.EventType, i.EventState);
    SetEvent(a);
    EXPECT(query_event(a).EventState == 1, "auto after set: state %ld", query_event(a).EventState);
    EXPECT(query_event(a).EventState == 1, "query consumed the auto event");
    CloseHandle(m); CloseHandle(a);
    END();
}

/* ---- semaphores ---- */

static void test_semaphore_basic(void)
{
    BEGIN("semaphore_release_prev_and_max");
    HANDLE s = CreateSemaphoreA(NULL, 2, 5, NULL);
    sem_info q = query_sem(s);
    EXPECT(q.CurrentCount == 2 && q.MaximumCount == 5, "query initial %lu/%lu", q.CurrentCount, q.MaximumCount);
    LONG prev = -1;
    EXPECT(ReleaseSemaphore(s, 1, &prev) && prev == 2, "release 1: prev %ld", prev);
    prev = -1;
    SetLastError(0);
    BOOL ok = ReleaseSemaphore(s, 3, &prev);
    DWORD err = GetLastError();
    EXPECT(!ok && err == ERROR_TOO_MANY_POSTS, "release over max: ret %d err %lu", ok, err);
    q = query_sem(s);
    EXPECT(q.CurrentCount == 3 && q.MaximumCount == 5, "count after failed release %lu/%lu", q.CurrentCount, q.MaximumCount);
    EXPECT(ReleaseSemaphore(s, 2, &prev) && prev == 3, "release up to max: prev %ld", prev);
    EXPECT(!ReleaseSemaphore(s, 1, NULL) && GetLastError() == ERROR_TOO_MANY_POSTS, "release at max did not fail");
    CloseHandle(s);
    END();
}

static void test_semaphore_wait(void)
{
    BEGIN("semaphore_wait_decrements");
    HANDLE s = CreateSemaphoreA(NULL, 3, 10, NULL);
    for (ULONG i = 3; i > 0; i--) {
        DWORD r = WaitForSingleObject(s, 0);
        EXPECT(r == WAIT_OBJECT_0, "wait at count %lu: %s", i, wr(r));
        EXPECT(query_sem(s).CurrentCount == i - 1, "count %lu after wait (expected %lu)", query_sem(s).CurrentCount, i - 1);
    }
    EXPECT(WaitForSingleObject(s, 0) == WAIT_TIMEOUT, "wait at count 0 did not time out");
    CloseHandle(s);
    END();
}

static void test_semaphore_wakes_n(void)
{
    BEGIN("semaphore_release_wakes_n");
    HANDLE s = CreateSemaphoreA(NULL, 0, 10, NULL), th[3];
    struct waiter w[3];
    volatile LONG woken = 0;
    start_waiters(w, th, 3, s, 3000, &woken);
    ReleaseSemaphore(s, 2, NULL);
    wait_count(&woken, 2, 2000);
    Sleep(100);
    EXPECT(woken == 2, "release 2 woke %ld", woken);
    EXPECT(query_sem(s).CurrentCount == 0, "count %lu after wake", query_sem(s).CurrentCount);
    ReleaseSemaphore(s, 1, NULL);
    wait_count(&woken, 3, 2000);
    EXPECT(woken == 3, "release 1 more: woke %ld", woken);
    for (int i = 0; i < 3; i++) join(th[i], 4000);
    CloseHandle(s);
    END();
}

/* ---- mutexes ---- */

static void test_mutex_recursion(void)
{
    BEGIN("mutex_recursion");
    HANDLE m = CreateMutexA(NULL, FALSE, NULL);
    mut_info q = query_mutant(m);
    EXPECT(q.CurrentCount == 1 && !q.OwnedByCaller && !q.AbandonedState, "free: count %ld owned %d abandoned %d",
           q.CurrentCount, q.OwnedByCaller, q.AbandonedState);
    for (int i = 0; i < 3; i++) EXPECT(WaitForSingleObject(m, 0) == WAIT_OBJECT_0, "acquire %d failed", i + 1);
    q = query_mutant(m);
    EXPECT(q.CurrentCount == -2 && q.OwnedByCaller && !q.AbandonedState, "owned 3x: count %ld owned %d abandoned %d",
           q.CurrentCount, q.OwnedByCaller, q.AbandonedState);
    for (int i = 0; i < 3; i++) EXPECT(ReleaseMutex(m), "release %d failed err %lu", i + 1, GetLastError());
    SetLastError(0);
    BOOL ok = ReleaseMutex(m);
    DWORD err = GetLastError();
    EXPECT(!ok && err == ERROR_NOT_OWNER, "4th release: ret %d err %lu", ok, err);
    q = query_mutant(m);
    EXPECT(q.CurrentCount == 1 && !q.OwnedByCaller, "after release: count %ld owned %d", q.CurrentCount, q.OwnedByCaller);
    CloseHandle(m);

    m = CreateMutexA(NULL, TRUE, NULL);
    q = query_mutant(m);
    EXPECT(q.CurrentCount == 0 && q.OwnedByCaller, "initial owner: count %ld owned %d", q.CurrentCount, q.OwnedByCaller);
    EXPECT(ReleaseMutex(m), "release of initially owned failed");
    CloseHandle(m);
    END();
}

struct nonowner { HANDLE m; BOOL rel; DWORD err; mut_info q; DWORD wait; };

static DWORD WINAPI nonowner_proc(void *p)
{
    struct nonowner *n = p;
    SetLastError(0);
    n->rel = ReleaseMutex(n->m);
    n->err = GetLastError();
    n->q = query_mutant(n->m);
    n->wait = WaitForSingleObject(n->m, 50);
    return 0;
}

static void test_mutex_nonowner(void)
{
    BEGIN("mutex_release_by_non_owner");
    struct nonowner n;
    memset(&n, 0, sizeof(n));
    n.m = CreateMutexA(NULL, FALSE, NULL);
    WaitForSingleObject(n.m, 0);
    EXPECT(join(start_thread(nonowner_proc, &n), 3000), "thread hung");
    EXPECT(!n.rel && n.err == ERROR_NOT_OWNER, "non-owner release: ret %d err %lu", n.rel, n.err);
    EXPECT(n.q.CurrentCount == 0 && !n.q.OwnedByCaller, "other thread query: count %ld owned %d", n.q.CurrentCount, n.q.OwnedByCaller);
    EXPECT(n.wait == WAIT_TIMEOUT, "other thread wait %s", wr(n.wait));
    EXPECT(ReleaseMutex(n.m), "owner release failed");
    CloseHandle(n.m);
    END();
}

static DWORD WINAPI abandon_proc(void *p)
{
    return WaitForSingleObject((HANDLE)p, 2000); /* exit while owning */
}

static int abandon(HANDLE m)
{
    HANDLE t = start_thread(abandon_proc, m);
    DWORD code = 0xdead;
    int ok = WaitForSingleObject(t, 3000) == WAIT_OBJECT_0;
    GetExitCodeThread(t, &code);
    CloseHandle(t);
    return ok && code == WAIT_OBJECT_0;
}

static void test_mutex_abandoned(void)
{
    BEGIN("mutex_abandoned");
    HANDLE m = CreateMutexA(NULL, FALSE, NULL);
    EXPECT(abandon(m), "helper thread failed to acquire");
    mut_info q = query_mutant(m);
    EXPECT(q.CurrentCount == 1 && !q.OwnedByCaller && q.AbandonedState, "before acquire: count %ld owned %d abandoned %d",
           q.CurrentCount, q.OwnedByCaller, q.AbandonedState);
    DWORD r = WaitForSingleObject(m, 1000);
    EXPECT(r == WAIT_ABANDONED, "wait %s", wr(r));
    q = query_mutant(m);
    EXPECT(q.CurrentCount == 0 && q.OwnedByCaller && !q.AbandonedState, "after acquire: count %ld owned %d abandoned %d",
           q.CurrentCount, q.OwnedByCaller, q.AbandonedState);
    r = WaitForSingleObject(m, 0);
    EXPECT(r == WAIT_OBJECT_0, "recursive wait after abandon %s", wr(r));
    EXPECT(ReleaseMutex(m) && ReleaseMutex(m), "release after abandon failed err %lu", GetLastError());
    r = WaitForSingleObject(m, 0);
    EXPECT(r == WAIT_OBJECT_0, "wait after clean release %s (abandoned twice?)", wr(r));
    ReleaseMutex(m);
    CloseHandle(m);
    END();
}

static void test_mutex_abandoned_wfmo(void)
{
    BEGIN("mutex_abandoned_wfmo_index");
    for (int idx = 0; idx < 3; idx++) {
        HANDLE h[3], m = CreateMutexA(NULL, FALSE, NULL);
        for (int i = 0; i < 3; i++) h[i] = i == idx ? m : CreateEventA(NULL, FALSE, FALSE, NULL);
        EXPECT(abandon(m), "helper thread failed");
        DWORD r = WaitForMultipleObjects(3, h, FALSE, 1000);
        EXPECT(r == WAIT_ABANDONED_0 + idx, "abandoned at index %d: got %s", idx, wr(r));
        EXPECT(query_mutant(m).OwnedByCaller, "not owned after WAIT_ABANDONED at %d", idx);
        ReleaseMutex(m);
        for (int i = 0; i < 3; i++) CloseHandle(h[i]);
    }
    END();
}

/* ---- WaitForMultipleObjects ---- */

static void test_wfmo_any(void)
{
    BEGIN("wfmo_any_lowest_index_consumes_one");
    HANDLE e[3];
    for (int i = 0; i < 3; i++) e[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
    SetEvent(e[2]); SetEvent(e[1]);
    DWORD r = WaitForMultipleObjects(3, e, FALSE, 0);
    EXPECT(r == WAIT_OBJECT_0 + 1, "events: got %s (expected index 1)", wr(r));
    EXPECT(query_event(e[1]).EventState == 0, "index 1 not consumed");
    EXPECT(query_event(e[2]).EventState == 1, "index 2 consumed too");
    for (int i = 0; i < 3; i++) CloseHandle(e[i]);

    HANDLE s[3];
    s[0] = CreateSemaphoreA(NULL, 0, 5, NULL);
    s[1] = CreateSemaphoreA(NULL, 1, 5, NULL);
    s[2] = CreateSemaphoreA(NULL, 1, 5, NULL);
    r = WaitForMultipleObjects(3, s, FALSE, 0);
    EXPECT(r == WAIT_OBJECT_0 + 1, "semaphores: got %s (expected index 1)", wr(r));
    EXPECT(query_sem(s[1]).CurrentCount == 0 && query_sem(s[2]).CurrentCount == 1,
           "counts after wait-any %lu %lu (expected 0 1)", query_sem(s[1]).CurrentCount, query_sem(s[2]).CurrentCount);
    for (int i = 0; i < 3; i++) CloseHandle(s[i]);
    END();
}

static void test_wfmo_all_atomic(void)
{
    BEGIN("wfmo_all_atomic");
    HANDLE e[2] = { CreateEventA(NULL, FALSE, FALSE, NULL), CreateEventA(NULL, FALSE, FALSE, NULL) };
    SetEvent(e[0]);
    DWORD r = WaitForMultipleObjects(2, e, TRUE, 50);
    EXPECT(r == WAIT_TIMEOUT, "one of two signaled: %s", wr(r));
    EXPECT(query_event(e[0]).EventState == 1, "signaled event consumed by failed wait-all");
    SetEvent(e[1]);
    r = WaitForMultipleObjects(2, e, TRUE, 0);
    EXPECT(r == WAIT_OBJECT_0, "both signaled: %s", wr(r));
    EXPECT(query_event(e[0]).EventState == 0 && query_event(e[1]).EventState == 0, "not both reset");
    CloseHandle(e[0]); CloseHandle(e[1]);

    HANDLE h[2] = { CreateSemaphoreA(NULL, 2, 5, NULL), CreateEventA(NULL, FALSE, FALSE, NULL) };
    r = WaitForMultipleObjects(2, h, TRUE, 0);
    EXPECT(r == WAIT_TIMEOUT && query_sem(h[0]).CurrentCount == 2, "sem+event: %s count %lu", wr(r), query_sem(h[0]).CurrentCount);
    SetEvent(h[1]);
    r = WaitForMultipleObjects(2, h, TRUE, 0);
    EXPECT(r == WAIT_OBJECT_0 && query_sem(h[0]).CurrentCount == 1, "sem+event signaled: %s count %lu", wr(r), query_sem(h[0]).CurrentCount);
    CloseHandle(h[0]); CloseHandle(h[1]);
    END();
}

struct waitall { HANDLE *h; DWORD n, timeout, result; };

static DWORD WINAPI waitall_proc(void *p)
{
    struct waitall *w = p;
    w->result = WaitForMultipleObjects(w->n, w->h, TRUE, w->timeout);
    return 0;
}

static void test_wfmo_all_blocked(void)
{
    BEGIN("wfmo_all_blocked_waiter_wakes");
    HANDLE e[2] = { CreateEventA(NULL, FALSE, FALSE, NULL), CreateEventA(NULL, FALSE, FALSE, NULL) };
    struct waitall w = { e, 2, 3000, 0xdead };
    HANDLE t = start_thread(waitall_proc, &w);
    Sleep(150);
    SetEvent(e[0]);
    Sleep(100);
    EXPECT(w.result == 0xdead, "wait-all returned early: %s", wr(w.result));
    EXPECT(query_event(e[0]).EventState == 1, "first event consumed while second unsignaled");
    SetEvent(e[1]);
    EXPECT(join(t, 3000), "waiter hung");
    EXPECT(w.result == WAIT_OBJECT_0, "result %s", wr(w.result));
    EXPECT(query_event(e[0]).EventState == 0 && query_event(e[1]).EventState == 0, "events not consumed");
    CloseHandle(e[0]); CloseHandle(e[1]);
    END();
}

struct holder { HANDLE m, ready, go; };

static DWORD WINAPI holder_proc(void *p)
{
    struct holder *h = p;
    if (WaitForSingleObject(h->m, 2000) != WAIT_OBJECT_0) return 1;
    SetEvent(h->ready);
    WaitForSingleObject(h->go, 3000);
    ReleaseMutex(h->m);
    return 0;
}

static void test_wfmo_all_mutex_other_owner(void)
{
    BEGIN("wfmo_all_mutex_owned_elsewhere");
    struct holder hd = { CreateMutexA(NULL, FALSE, NULL), CreateEventA(NULL, FALSE, FALSE, NULL), CreateEventA(NULL, FALSE, FALSE, NULL) };
    HANDLE ev = CreateEventA(NULL, TRUE, TRUE, NULL);
    HANDLE t = start_thread(holder_proc, &hd);
    EXPECT(WaitForSingleObject(hd.ready, 2000) == WAIT_OBJECT_0, "holder did not start");
    HANDLE h[2] = { ev, hd.m };
    DWORD r = WaitForMultipleObjects(2, h, TRUE, 100);
    EXPECT(r == WAIT_TIMEOUT, "wait-all with foreign-owned mutex: %s", wr(r));
    SetEvent(hd.go);
    r = WaitForMultipleObjects(2, h, TRUE, 3000);
    EXPECT(r == WAIT_OBJECT_0, "after owner released: %s", wr(r));
    EXPECT(query_mutant(hd.m).OwnedByCaller, "mutex not owned after wait-all");
    ReleaseMutex(hd.m);
    join(t, 3000);
    CloseHandle(hd.m); CloseHandle(hd.ready); CloseHandle(hd.go); CloseHandle(ev);
    END();
}

static void test_wfmo_dup_handle(void)
{
    {
        BEGIN("wfmo_any_same_handle_twice");
        HANDLE e = CreateEventA(NULL, TRUE, TRUE, NULL), h[2] = { e, e };
        DWORD r = WaitForMultipleObjects(2, h, FALSE, 0);
        EXPECT(r == WAIT_OBJECT_0, "wait-any {h,h}: %s", wr(r));
        CloseHandle(e);
        END();
    }
    {
        BEGIN("wfmo_all_same_handle_twice");
        KNOWN_WINE();
        HANDLE e = CreateEventA(NULL, TRUE, TRUE, NULL), h[2] = { e, e };
        SetLastError(0);
        DWORD r = WaitForMultipleObjects(2, h, TRUE, 0);
        DWORD err = GetLastError();
        LARGE_INTEGER zero = { 0 };
        nt_status s = pNtWaitForMultipleObjects(2, h, 0 /* WaitAll */, FALSE, &zero);
        EXPECT(r == WAIT_FAILED && err == ERROR_INVALID_PARAMETER, "wait-all {h,h} returned 0x%lx err %lu (Windows: WAIT_FAILED, 87)", r, err);
        EXPECT(s == NT_INVALID_PARAMETER || s == NT_INVALID_PARAMETER_MIX, "NtWaitForMultipleObjects WaitAll {h,h}: 0x%lx", s);
        CloseHandle(e);
        END();
    }
}

static void test_wfmo_64(void)
{
    BEGIN("wfmo_64_handles");
    HANDLE e[MAXIMUM_WAIT_OBJECTS + 1];
    for (int i = 0; i <= MAXIMUM_WAIT_OBJECTS; i++) e[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
    SetEvent(e[63]);
    DWORD r = WaitForMultipleObjects(64, e, FALSE, 1000);
    EXPECT(r == WAIT_OBJECT_0 + 63, "only last signaled: %s", wr(r));
    EXPECT(query_event(e[63]).EventState == 0, "last not consumed");
    SetLastError(0);
    r = WaitForMultipleObjects(65, e, FALSE, 0);
    DWORD err = GetLastError();
    EXPECT(r == WAIT_FAILED && err == ERROR_INVALID_PARAMETER, "65 handles: %s err %lu", wr(r), err);
    for (int i = 0; i < 64; i++) SetEvent(e[i]);
    r = WaitForMultipleObjects(64, e, TRUE, 1000);
    EXPECT(r == WAIT_OBJECT_0, "wait-all 64 signaled: %s", wr(r));
    int left = 0;
    for (int i = 0; i < 64; i++) left += query_event(e[i]).EventState != 0;
    EXPECT(left == 0, "%d of 64 still signaled after wait-all", left);
    for (int i = 0; i <= MAXIMUM_WAIT_OBJECTS; i++) CloseHandle(e[i]);
    END();
}

/* ---- timeouts ---- */

static void test_timeouts(void)
{
    HANDLE e = CreateEventA(NULL, TRUE, FALSE, NULL);
    {
        BEGIN("timeout_zero_immediate");
        double t0 = now_ms();
        int bad = 0;
        for (int i = 0; i < 100; i++) bad += WaitForSingleObject(e, 0) != WAIT_TIMEOUT;
        double dt = now_ms() - t0;
        EXPECT(!bad, "%d of 100 zero waits did not return WAIT_TIMEOUT", bad);
        EXPECT(dt < 500, "100 zero waits took %.1f ms", dt);
        INFO("%.1f us per wait", dt * 10.0);
        END();
    }
    {
        BEGIN("timeout_50ms");
        double t0 = now_ms();
        DWORD r = WaitForSingleObject(e, 50);
        double dt = now_ms() - t0;
        EXPECT(r == WAIT_TIMEOUT, "%s", wr(r));
        EXPECT(dt >= 45 && dt <= 500, "took %.1f ms", dt);
        HANDLE h[3] = { e, e, e };
        t0 = now_ms();
        r = WaitForMultipleObjects(3, h, FALSE, 50);
        dt = now_ms() - t0;
        EXPECT(r == WAIT_TIMEOUT && dt >= 45 && dt <= 500, "wfmo: %s after %.1f ms", wr(r), dt);
        END();
    }
    {
        BEGIN("timeout_nt_relative");
        LARGE_INTEGER to;
        to.QuadPart = -500000; /* 50 ms */
        double t0 = now_ms();
        nt_status s = pNtWaitForSingleObject(e, FALSE, &to);
        double dt = now_ms() - t0;
        EXPECT(s == NT_TIMEOUT, "status 0x%lx", s);
        EXPECT(dt >= 45 && dt <= 500, "took %.1f ms", dt);
        END();
    }
    {
        BEGIN("timeout_nt_absolute");
        LARGE_INTEGER to;
        pNtQuerySystemTime(&to);
        to.QuadPart += 500000; /* now + 50 ms */
        double t0 = now_ms();
        nt_status s = pNtWaitForSingleObject(e, FALSE, &to);
        double dt = now_ms() - t0;
        EXPECT(s == NT_TIMEOUT, "status 0x%lx", s);
        EXPECT(dt >= 30 && dt <= 600, "took %.1f ms", dt);
        pNtQuerySystemTime(&to);
        to.QuadPart -= 10000000; /* 1 s in the past */
        t0 = now_ms();
        s = pNtWaitForSingleObject(e, FALSE, &to);
        dt = now_ms() - t0;
        EXPECT(s == NT_TIMEOUT && dt < 100, "past absolute time: 0x%lx after %.1f ms", s, dt);
        END();
    }
    CloseHandle(e);
}

/* ---- APCs ---- */

struct alw { int kind; HANDLE h, h2; DWORD timeout; DWORD result; };

static DWORD WINAPI alertable_proc(void *p)
{
    struct alw *a = p;
    HANDLE hs[2] = { a->h, a->h2 };
    LARGE_INTEGER to;
    switch (a->kind) {
    case 0: a->result = WaitForSingleObjectEx(a->h, a->timeout, TRUE); break;
    case 1: a->result = SleepEx(a->timeout, TRUE); break;
    case 2: a->result = WaitForMultipleObjectsEx(2, hs, FALSE, a->timeout, TRUE); break;
    case 3: a->result = SignalObjectAndWait(a->h, a->h2, a->timeout, TRUE); break;
    case 4:
        to.QuadPart = -(LONGLONG)a->timeout * 10000;
        a->result = (DWORD)pNtWaitForSingleObject(a->h, TRUE, &to);
        break;
    }
    return 0;
}

static void apc_to_waiting_thread(const char *name, int kind)
{
    BEGIN(name);
    struct alw a = { kind, CreateEventA(NULL, FALSE, FALSE, NULL), CreateEventA(NULL, FALSE, FALSE, NULL), 3000, 0xdead };
    apc_reset();
    double t0 = now_ms();
    HANDLE t = start_thread(alertable_proc, &a);
    DWORD tid = GetThreadId(t);
    Sleep(150);
    EXPECT(a.result == 0xdead, "wait returned early: %s", wr(a.result));
    EXPECT(QueueUserAPC(apc_fn, t, 7), "QueueUserAPC failed err %lu", GetLastError());
    EXPECT(join(t, 4000), "thread hung");
    double dt = now_ms() - t0;
    EXPECT(a.result == WAIT_IO_COMPLETION, "result %s", wr(a.result));
    EXPECT(g_apc_count == 1 && g_apc_tid == tid, "apc ran %ld times, on tid %lu (waiter %lu)", g_apc_count, g_apc_tid, tid);
    EXPECT(dt < 2000, "returned after %.0f ms", dt);
    CloseHandle(a.h); CloseHandle(a.h2);
    END();
}

static void test_apc(void)
{
    apc_to_waiting_thread("apc_alertable_wait", 0);
    apc_to_waiting_thread("apc_sleepex_alertable", 1);
    apc_to_waiting_thread("apc_wfmo_alertable", 2);

    HANDLE e = CreateEventA(NULL, TRUE, FALSE, NULL);
    {
        BEGIN("apc_queued_before_wait");
        apc_reset();
        QueueUserAPC(apc_fn, GetCurrentThread(), 1);
        double t0 = now_ms();
        DWORD r = WaitForSingleObjectEx(e, 1000, TRUE);
        double dt = now_ms() - t0;
        EXPECT(r == WAIT_IO_COMPLETION, "result %s", wr(r));
        EXPECT(g_apc_count == 1, "apc ran %ld times", g_apc_count);
        EXPECT(dt < 200, "returned after %.0f ms", dt);
        END();
    }
    {
        BEGIN("apc_not_run_in_nonalertable_wait");
        apc_reset();
        QueueUserAPC(apc_fn, GetCurrentThread(), 1);
        DWORD r = WaitForSingleObject(e, 50);
        EXPECT(r == WAIT_TIMEOUT, "non-alertable wait %s", wr(r));
        Sleep(10);
        EXPECT(g_apc_count == 0, "apc ran in a non-alertable wait");
        r = SleepEx(0, TRUE);
        EXPECT(r == WAIT_IO_COMPLETION && g_apc_count == 1, "SleepEx(0, TRUE) %s, apc count %ld", wr(r), g_apc_count);
        END();
    }
    {
        BEGIN("apc_multiple_fifo");
        apc_reset();
        for (ULONG_PTR i = 1; i <= 3; i++) QueueUserAPC(apc_fn, GetCurrentThread(), i);
        DWORD r = SleepEx(1000, TRUE);
        EXPECT(r == WAIT_IO_COMPLETION, "result %s", wr(r));
        EXPECT(g_apc_count == 3, "%ld of 3 apcs ran in one alertable wait", g_apc_count);
        EXPECT(g_apc_order[0] == 1 && g_apc_order[1] == 2 && g_apc_order[2] == 3, "order %lu %lu %lu",
               (unsigned long)g_apc_order[0], (unsigned long)g_apc_order[1], (unsigned long)g_apc_order[2]);
        SleepEx(0, TRUE); /* drain leftovers */
        END();
    }
    {
        BEGIN("nt_alert_thread");
        KNOWN_WINE();
        struct alw a = { 4, e, NULL, 500, 0xdead };
        HANDLE t = start_thread(alertable_proc, &a);
        Sleep(100);
        nt_status s = pNtAlertThread(t);
        join(t, 2000);
        EXPECT(s == 0, "NtAlertThread status 0x%lx", s);
        EXPECT(a.result == NT_ALERTED, "alertable NtWaitForSingleObject returned 0x%lx (expected STATUS_ALERTED)", a.result);
        END();
    }
    CloseHandle(e);
}

/* ---- SignalObjectAndWait ---- */

struct pingpong { HANDLE ping, pong; int n; int bad; };

static DWORD WINAPI pong_proc(void *p)
{
    struct pingpong *pp = p;
    for (int i = 0; i < pp->n; i++) {
        if (WaitForSingleObject(pp->ping, 2000) != WAIT_OBJECT_0) { pp->bad = i + 1; return 1; }
        SetEvent(pp->pong);
    }
    return 0;
}

struct mtxgrab { HANDLE m, done; DWORD r; };

static DWORD WINAPI mtxgrab_proc(void *p)
{
    struct mtxgrab *g = p;
    g->r = WaitForSingleObject(g->m, 3000);
    if (g->r == WAIT_OBJECT_0) { SetEvent(g->done); ReleaseMutex(g->m); }
    return 0;
}

static void test_soaw(void)
{
    {
        BEGIN("soaw_event_pingpong");
        HANDLE a = CreateEventA(NULL, FALSE, FALSE, NULL), b = CreateEventA(NULL, TRUE, TRUE, NULL);
        DWORD r = SignalObjectAndWait(a, b, 0, FALSE);
        EXPECT(r == WAIT_OBJECT_0 && query_event(a).EventState == 1, "signal a, wait b: %s, a state %ld", wr(r), query_event(a).EventState);
        CloseHandle(a); CloseHandle(b);

        struct pingpong pp = { CreateEventA(NULL, FALSE, FALSE, NULL), CreateEventA(NULL, FALSE, FALSE, NULL), 2000, 0 };
        HANDLE t = start_thread(pong_proc, &pp);
        int bad = 0;
        double t0 = now_ms();
        for (int i = 0; i < pp.n && !bad; i++)
            if (SignalObjectAndWait(pp.ping, pp.pong, 2000, FALSE) != WAIT_OBJECT_0) bad = i + 1;
        double dt = now_ms() - t0;
        EXPECT(!bad && !pp.bad, "ping-pong broke at %d / %d", bad, pp.bad);
        EXPECT(join(t, 3000), "pong thread hung");
        INFO("%d round trips in %.0f ms", pp.n, dt);
        CloseHandle(pp.ping); CloseHandle(pp.pong);
        END();
    }
    {
        BEGIN("soaw_semaphore");
        HANDLE s = CreateSemaphoreA(NULL, 0, 2, NULL), e = CreateEventA(NULL, FALSE, FALSE, NULL);
        DWORD r = SignalObjectAndWait(s, e, 50, FALSE);
        EXPECT(r == WAIT_TIMEOUT && query_sem(s).CurrentCount == 1, "1st: %s count %lu", wr(r), query_sem(s).CurrentCount);
        r = SignalObjectAndWait(s, e, 0, FALSE);
        EXPECT(r == WAIT_TIMEOUT && query_sem(s).CurrentCount == 2, "2nd: %s count %lu", wr(r), query_sem(s).CurrentCount);
        SetEvent(e);
        SetLastError(0);
        r = SignalObjectAndWait(s, e, 0, FALSE);
        DWORD err = GetLastError();
        EXPECT(r == WAIT_FAILED && err == ERROR_TOO_MANY_POSTS, "over max: %s err %lu", wr(r), err);
        EXPECT(query_sem(s).CurrentCount == 2, "count %lu after failed signal", query_sem(s).CurrentCount);
        EXPECT(query_event(e).EventState == 1, "wait object consumed although the signal failed");
        CloseHandle(s); CloseHandle(e);
        END();
    }
    {
        BEGIN("soaw_mutex_release");
        HANDLE m = CreateMutexA(NULL, TRUE, NULL), e = CreateEventA(NULL, FALSE, FALSE, NULL);
        DWORD r = SignalObjectAndWait(m, e, 50, FALSE);
        mut_info q = query_mutant(m);
        EXPECT(r == WAIT_TIMEOUT, "release+wait %s", wr(r));
        EXPECT(q.CurrentCount == 1 && !q.OwnedByCaller, "after: count %ld owned %d", q.CurrentCount, q.OwnedByCaller);
        SetLastError(0);
        r = SignalObjectAndWait(m, e, 0, FALSE);
        DWORD err = GetLastError();
        EXPECT(r == WAIT_FAILED && err == ERROR_NOT_OWNER, "not owned: %s err %lu", wr(r), err);

        /* handoff: release to a blocked thread and wait for its answer */
        WaitForSingleObject(m, 0);
        struct mtxgrab g = { m, e, 0xdead };
        HANDLE t = start_thread(mtxgrab_proc, &g);
        Sleep(100);
        r = SignalObjectAndWait(m, e, 3000, FALSE);
        EXPECT(r == WAIT_OBJECT_0 && g.r == WAIT_OBJECT_0, "handoff: %s, thread %s", wr(r), wr(g.r));
        join(t, 3000);
        CloseHandle(m); CloseHandle(e);
        END();
    }
    {
        BEGIN("soaw_alertable_apc");
        struct alw a = { 3, CreateSemaphoreA(NULL, 0, 5, NULL), CreateEventA(NULL, FALSE, FALSE, NULL), 3000, 0xdead };
        apc_reset();
        HANDLE t = start_thread(alertable_proc, &a);
        DWORD tid = GetThreadId(t);
        DWORD r = WaitForSingleObject(a.h, 2000);
        EXPECT(r == WAIT_OBJECT_0, "semaphore not signaled by the thread: %s", wr(r));
        Sleep(100);
        QueueUserAPC(apc_fn, t, 9);
        EXPECT(join(t, 4000), "thread hung");
        EXPECT(a.result == WAIT_IO_COMPLETION, "result %s", wr(a.result));
        EXPECT(g_apc_count == 1 && g_apc_tid == tid, "apc ran %ld times", g_apc_count);
        CloseHandle(a.h); CloseHandle(a.h2);
        END();
    }
}

/* ---- cross-process ---- */

static int child_main(int argc, char **argv)
{
    char n1[200], n2[200];
    const char *mode = argv[2], *arg = argc > 3 ? argv[3] : "";
    if (!strcmp(mode, "ev")) {
        snprintf(n1, sizeof(n1), "%s_req", arg);
        snprintf(n2, sizeof(n2), "%s_ack", arg);
        HANDLE req = OpenEventA(SYNCHRONIZE, FALSE, n1), ack = OpenEventA(EVENT_MODIFY_STATE, FALSE, n2);
        if (!req || !ack) return 10;
        if (WaitForSingleObject(req, 5000) != WAIT_OBJECT_0) return 11;
        return SetEvent(ack) ? 0 : 12;
    }
    if (!strcmp(mode, "sem")) {
        LONG prev = -1;
        HANDLE s = OpenSemaphoreA(SEMAPHORE_MODIFY_STATE | SYNCHRONIZE, FALSE, arg);
        if (!s) return 10;
        if (WaitForSingleObject(s, 5000) != WAIT_OBJECT_0) return 11;
        if (!ReleaseSemaphore(s, 3, &prev)) return 12;
        return prev == 0 ? 0 : 20 + (int)prev;
    }
    if (!strcmp(mode, "mtx")) {
        HANDLE m = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, arg);
        if (!m) return 10;
        return WaitForSingleObject(m, 5000) == WAIT_OBJECT_0 ? 0 : 11; /* exit while owning */
    }
    if (!strcmp(mode, "mtxbusy")) {
        HANDLE m = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, arg);
        if (!m) return 10;
        return WaitForSingleObject(m, 100) == WAIT_TIMEOUT ? 0 : 11;
    }
    if (!strcmp(mode, "dup")) {
        HANDLE map = OpenFileMappingA(FILE_MAP_READ, FALSE, arg);
        if (!map) return 10;
        volatile ULONG64 *v = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
        if (!v) return 11;
        return SetEvent((HANDLE)(ULONG_PTR)*v) ? 0 : 12;
    }
    if (!strcmp(mode, "waitsem")) {
        HANDLE sm = OpenSemaphoreA(SYNCHRONIZE, FALSE, arg);
        if (!sm) return 10;
        return WaitForSingleObject(sm, 20000) == WAIT_OBJECT_0 ? 0 : 11; /* killed by the parent first */
    }
    if (!strcmp(mode, "waitev")) {
        HANDLE e = OpenEventA(SYNCHRONIZE, FALSE, arg);
        if (!e) return 10;
        return WaitForSingleObject(e, 5000) == WAIT_OBJECT_0 ? 7 : 11;
    }
    return 99;
}

static void test_xproc(void)
{
    char name[128], n1[160], n2[160];
    PROCESS_INFORMATION pi;
    {
        BEGIN("xproc_named_event");
        obj_name(name, sizeof(name), "ev");
        snprintf(n1, sizeof(n1), "%s_req", name);
        snprintf(n2, sizeof(n2), "%s_ack", name);
        HANDLE req = CreateEventA(NULL, FALSE, FALSE, n1), ack = CreateEventA(NULL, FALSE, FALSE, n2);
        if (spawn_child("ev", name, 0, &pi)) {
            CloseHandle(pi.hThread);
            SetEvent(req);
            DWORD r = WaitForSingleObject(ack, 10000);
            EXPECT(r == WAIT_OBJECT_0, "ack from child %s", wr(r));
            EXPECT(WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0, "child did not exit");
            EXPECT(exit_code(pi.hProcess) == 0, "child exit code %lu", exit_code(pi.hProcess));
            EXPECT(query_event(req).EventState == 0, "request not consumed by the child");
            CloseHandle(pi.hProcess);
        } else EXPECT(0, "CreateProcess failed err %lu", GetLastError());
        CloseHandle(req); CloseHandle(ack);
        END();
    }
    {
        BEGIN("xproc_named_semaphore");
        obj_name(name, sizeof(name), "sem");
        HANDLE s = CreateSemaphoreA(NULL, 1, 10, name);
        if (spawn_child("sem", name, 0, &pi)) {
            CloseHandle(pi.hThread);
            EXPECT(WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0, "child did not exit");
            EXPECT(exit_code(pi.hProcess) == 0, "child exit code %lu", exit_code(pi.hProcess));
            EXPECT(query_sem(s).CurrentCount == 3, "count %lu (expected 3)", query_sem(s).CurrentCount);
            CloseHandle(pi.hProcess);
        } else EXPECT(0, "CreateProcess failed err %lu", GetLastError());
        CloseHandle(s);
        END();
    }
    {
        BEGIN("xproc_named_mutex_busy");
        obj_name(name, sizeof(name), "mtxb");
        HANDLE m = CreateMutexA(NULL, TRUE, name);
        if (spawn_child("mtxbusy", name, 0, &pi)) {
            CloseHandle(pi.hThread);
            EXPECT(WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0, "child did not exit");
            EXPECT(exit_code(pi.hProcess) == 0, "child exit code %lu (11 = got the mutex)", exit_code(pi.hProcess));
            CloseHandle(pi.hProcess);
        } else EXPECT(0, "CreateProcess failed err %lu", GetLastError());
        EXPECT(ReleaseMutex(m), "release failed");
        CloseHandle(m);
        END();
    }
    {
        BEGIN("xproc_named_mutex_abandoned");
        obj_name(name, sizeof(name), "mtx");
        HANDLE m = CreateMutexA(NULL, FALSE, name);
        if (spawn_child("mtx", name, 0, &pi)) {
            CloseHandle(pi.hThread);
            EXPECT(WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0, "child did not exit");
            EXPECT(exit_code(pi.hProcess) == 0, "child exit code %lu", exit_code(pi.hProcess));
            mut_info q = query_mutant(m);
            EXPECT(q.AbandonedState && q.CurrentCount == 1, "after child exit: count %ld abandoned %d", q.CurrentCount, q.AbandonedState);
            DWORD r = WaitForSingleObject(m, 2000);
            EXPECT(r == WAIT_ABANDONED, "wait %s", wr(r));
            if (r == WAIT_ABANDONED || r == WAIT_OBJECT_0) ReleaseMutex(m);
            CloseHandle(pi.hProcess);
        } else EXPECT(0, "CreateProcess failed err %lu", GetLastError());
        CloseHandle(m);
        END();
    }
    {
        HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL), remote = NULL;
        obj_name(name, sizeof(name), "map");
        HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, name);
        volatile ULONG64 *mv = map ? MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
        int started = mv && spawn_child("dup", name, CREATE_SUSPENDED, &pi);
        BOOL dup_ok = started && DuplicateHandle(GetCurrentProcess(), ev, pi.hProcess, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS);
        {
            BEGIN("xproc_duplicate_back");
            HANDLE back = NULL;
            EXPECT(started, "CreateProcess failed err %lu", GetLastError());
            EXPECT(dup_ok, "DuplicateHandle into child failed err %lu", GetLastError());
            if (dup_ok) {
                EXPECT(DuplicateHandle(pi.hProcess, remote, GetCurrentProcess(), &back, 0, FALSE, DUPLICATE_SAME_ACCESS),
                       "DuplicateHandle from child failed err %lu", GetLastError());
                if (back) {
                    SetEvent(back);
                    EXPECT(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0, "handle from child is not the same object");
                    CloseHandle(back);
                }
            }
            END();
        }
        {
            BEGIN("xproc_duplicated_unnamed_event");
            if (started) {
                *mv = (ULONG64)(ULONG_PTR)remote;
                ResumeThread(pi.hThread);
                CloseHandle(pi.hThread);
                DWORD r = WaitForSingleObject(ev, 10000);
                EXPECT(r == WAIT_OBJECT_0, "child signal %s", wr(r));
                EXPECT(WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0, "child did not exit");
                EXPECT(exit_code(pi.hProcess) == 0, "child exit code %lu", exit_code(pi.hProcess));
                CloseHandle(pi.hProcess);
            } else EXPECT(0, "child not started");
            END();
        }
        if (mv) UnmapViewOfFile((void *)mv);
        if (map) CloseHandle(map);
        CloseHandle(ev);
    }
}

/* ---- process and thread handles ---- */

static DWORD WINAPI exit42_proc(void *p)
{
    WaitForSingleObject((HANDLE)p, 3000);
    return 42;
}

static void test_proc_thread_handles(void)
{
    {
        BEGIN("wait_thread_handle");
        HANDLE go = CreateEventA(NULL, FALSE, FALSE, NULL);
        HANDLE t = start_thread(exit42_proc, go);
        DWORD r0 = WaitForSingleObject(t, 0), code = 0;
        SetEvent(go);
        DWORD r1 = WaitForSingleObject(t, 3000);
        GetExitCodeThread(t, &code);
        EXPECT(r0 == WAIT_TIMEOUT, "running thread: %s", wr(r0));
        EXPECT(r1 == WAIT_OBJECT_0 && code == 42, "exited thread: %s code %lu", wr(r1), code);
        EXPECT(WaitForSingleObject(t, 0) == WAIT_OBJECT_0, "thread handle not staying signaled");
        CloseHandle(t); CloseHandle(go);
        END();
    }
    char name[128];
    PROCESS_INFORMATION pi;
    obj_name(name, sizeof(name), "procev");
    HANDLE quit = CreateEventA(NULL, TRUE, FALSE, name);
    if (!spawn_child("waitev", name, 0, &pi)) {
        BEGIN("wait_process_handle");
        EXPECT(0, "CreateProcess failed err %lu", GetLastError());
        END();
        CloseHandle(quit);
        return;
    }
    CloseHandle(pi.hThread);
    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    {
        BEGIN("wfmo_event_and_running_process");
        HANDLE h[2] = { ev, pi.hProcess };
        DWORD r = WaitForMultipleObjects(2, h, FALSE, 100);
        EXPECT(r == WAIT_TIMEOUT, "nothing signaled: %s", wr(r));
        SetEvent(ev);
        r = WaitForMultipleObjects(2, h, FALSE, 0);
        EXPECT(r == WAIT_OBJECT_0, "event signaled: %s", wr(r));
        r = WaitForMultipleObjects(2, h, TRUE, 50);
        EXPECT(r == WAIT_TIMEOUT, "wait-all with running process: %s", wr(r));
        ResetEvent(ev);
        END();
    }
    {
        BEGIN("wait_process_handle");
        DWORD r0 = WaitForSingleObject(pi.hProcess, 0);
        SetEvent(quit);
        DWORD r1 = WaitForSingleObject(pi.hProcess, 10000);
        EXPECT(r0 == WAIT_TIMEOUT, "running child: %s", wr(r0));
        EXPECT(r1 == WAIT_OBJECT_0, "child exit: %s", wr(r1));
        EXPECT(exit_code(pi.hProcess) == 7, "exit code %lu (expected 7)", exit_code(pi.hProcess));
        END();
    }
    {
        BEGIN("wfmo_event_and_exited_process");
        HANDLE h[2] = { ev, pi.hProcess };
        DWORD r = WaitForMultipleObjects(2, h, FALSE, 1000);
        EXPECT(r == WAIT_OBJECT_0 + 1, "wait-any: %s (expected index 1)", wr(r));
        SetEvent(ev);
        r = WaitForMultipleObjects(2, h, TRUE, 1000);
        EXPECT(r == WAIT_OBJECT_0, "wait-all: %s", wr(r));
        END();
    }
    CloseHandle(ev); CloseHandle(quit); CloseHandle(pi.hProcess);
}

/* ---- handle closed while waiting ---- */

static void test_close_while_waiting(void)
{
    BEGIN("handle_closed_while_waiting");
    HANDLE h = CreateEventA(NULL, FALSE, FALSE, NULL), h2 = NULL, th;
    struct waiter w;
    volatile LONG woken = 0;
    start_waiters(&w, &th, 1, h, 3000, &woken);
    Sleep(50);
    DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &h2, 0, FALSE, DUPLICATE_SAME_ACCESS);
    EXPECT(CloseHandle(h), "CloseHandle failed");
    Sleep(50);
    EXPECT(w.result == 0xdead, "waiter returned on close: %s", wr(w.result));
    SetEvent(h2);
    EXPECT(join(th, 4000), "waiter hung");
    EXPECT(w.result == WAIT_OBJECT_0, "waiter result %s", wr(w.result));
    CloseHandle(h2);
    END();
}

/* ---- waiters killed while blocked: their wait must not take a later signal ---- */

struct wfmo_args { HANDLE h[2]; BOOL all; DWORD r; };
static DWORD WINAPI wfmo_proc(void *p)
{
    struct wfmo_args *a = p;
    a->r = WaitForMultipleObjects(2, a->h, a->all, 20000);
    return 0;
}

static void test_killed_waiters(void)
{
    {
        BEGIN("terminated_waiter_thread");
        HANDLE sem = CreateSemaphoreA(NULL, 0, 10, NULL), ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        struct wfmo_args a = { { ev, sem }, FALSE, 0xdead };
        HANDLE t1 = start_thread(wfmo_proc, &a);
        Sleep(150);
        TerminateThread(t1, 5);
        EXPECT(WaitForSingleObject(t1, 3000) == WAIT_OBJECT_0, "terminated thread did not end");
        CloseHandle(t1);
        struct waiter w;
        HANDLE t2;
        volatile LONG woken = 0;
        start_waiters(&w, &t2, 1, sem, 3000, &woken);
        ReleaseSemaphore(sem, 1, NULL);
        EXPECT(join(t2, 4000), "second waiter hung");
        EXPECT(w.result == WAIT_OBJECT_0, "second waiter %s (release taken by the dead wait?)", wr(w.result));
        SetEvent(ev);
        EXPECT(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0, "event taken by the dead wait");
        CloseHandle(sem); CloseHandle(ev);
        END();
    }
    {
        BEGIN("terminated_waitall_thread");
        HANDLE e1 = CreateEventA(NULL, FALSE, FALSE, NULL), e2 = CreateEventA(NULL, FALSE, FALSE, NULL);
        struct wfmo_args a = { { e1, e2 }, TRUE, 0xdead };
        HANDLE t1 = start_thread(wfmo_proc, &a);
        Sleep(150);
        TerminateThread(t1, 5);
        EXPECT(WaitForSingleObject(t1, 3000) == WAIT_OBJECT_0, "terminated thread did not end");
        CloseHandle(t1);
        SetEvent(e1); SetEvent(e2);
        EXPECT(WaitForSingleObject(e1, 0) == WAIT_OBJECT_0 && WaitForSingleObject(e2, 0) == WAIT_OBJECT_0,
               "events taken by the dead wait-all");
        CloseHandle(e1); CloseHandle(e2);
        END();
    }
    {
        BEGIN("terminated_waiter_process");
        char name[128];
        PROCESS_INFORMATION pi;
        obj_name(name, sizeof(name), "ksem");
        HANDLE sem = CreateSemaphoreA(NULL, 0, 10, name);
        if (spawn_child("waitsem", name, 0, &pi)) {
            CloseHandle(pi.hThread);
            Sleep(700); /* child starts and blocks */
            TerminateProcess(pi.hProcess, 1);
            EXPECT(WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0, "child did not end");
            EXPECT(exit_code(pi.hProcess) == 1, "child exit code %lu (0 = it got the semaphore early)", exit_code(pi.hProcess));
            CloseHandle(pi.hProcess);
            struct waiter w;
            HANDLE t2;
            volatile LONG woken = 0;
            start_waiters(&w, &t2, 1, sem, 3000, &woken);
            ReleaseSemaphore(sem, 1, NULL);
            EXPECT(join(t2, 4000), "waiter hung");
            EXPECT(w.result == WAIT_OBJECT_0, "waiter %s (release taken by the dead process?)", wr(w.result));
        } else EXPECT(0, "CreateProcess failed err %lu", GetLastError());
        CloseHandle(sem);
        END();
    }
}

/* ---- named objects ---- */

static void test_named(void)
{
    BEGIN("named_wrong_type");
    char name[128], missing[128];
    obj_name(name, sizeof(name), "typed");
    obj_name(missing, sizeof(missing), "missing");
    HANDLE s = CreateSemaphoreA(NULL, 0, 1, name);
    SetLastError(0);
    HANDLE e = OpenEventA(EVENT_ALL_ACCESS, FALSE, name);
    DWORD err = GetLastError();
    EXPECT(!e, "OpenEvent on a semaphore name succeeded");
    EXPECT(err == ERROR_INVALID_HANDLE, "OpenEvent on a semaphore name: err %lu (Windows: 6)", err);
    if (e) CloseHandle(e);
    SetLastError(0);
    e = CreateEventA(NULL, FALSE, FALSE, name);
    err = GetLastError();
    EXPECT(!e && err == ERROR_INVALID_HANDLE, "CreateEvent on a semaphore name: %p err %lu", (void *)e, err);
    if (e) CloseHandle(e);
    HANDLE m = OpenMutexA(SYNCHRONIZE, FALSE, name);
    EXPECT(!m, "OpenMutex on a semaphore name succeeded");
    if (m) CloseHandle(m);
    SetLastError(0);
    e = OpenEventA(EVENT_ALL_ACCESS, FALSE, missing);
    err = GetLastError();
    EXPECT(!e && err == ERROR_FILE_NOT_FOUND, "OpenEvent on a missing name: %p err %lu", (void *)e, err);
    HANDLE s2;
    SetLastError(0);
    s2 = CreateSemaphoreA(NULL, 0, 1, name);
    err = GetLastError();
    EXPECT(s2 && err == ERROR_ALREADY_EXISTS, "second CreateSemaphore: %p err %lu", (void *)s2, err);
    if (s2) {
        ReleaseSemaphore(s2, 1, NULL);
        EXPECT(query_sem(s).CurrentCount == 1, "second handle is not the same object");
        CloseHandle(s2);
    }
    CloseHandle(s);
    END();
}

/* ---- stress ---- */

#define PC_ITERS 20000
#define MX_ITERS 50000
#define RING_ROUNDS 5000

struct pc { HANDLE slots, items; volatile LONG *produced, *consumed; int producer; int bad; };

static DWORD WINAPI pc_proc(void *p)
{
    struct pc *c = p;
    for (int i = 0; i < PC_ITERS; i++) {
        if (WaitForSingleObject(c->producer ? c->slots : c->items, 5000) != WAIT_OBJECT_0) { c->bad = i + 1; return 1; }
        InterlockedIncrement(c->producer ? c->produced : c->consumed);
        if (!ReleaseSemaphore(c->producer ? c->items : c->slots, 1, NULL)) { c->bad = -(i + 1); return 1; }
    }
    return 0;
}

struct mx { HANDLE m; volatile LONG *counter; int bad; };

static DWORD WINAPI mx_proc(void *p)
{
    struct mx *x = p;
    for (int i = 0; i < MX_ITERS; i++) {
        if (WaitForSingleObject(x->m, 5000) != WAIT_OBJECT_0) { x->bad = i + 1; return 1; }
        LONG v = *x->counter; /* deliberately not atomic */
        if ((i & 255) == 0) YieldProcessor();
        *x->counter = v + 1;
        if (!ReleaseMutex(x->m)) { x->bad = -(i + 1); return 1; }
    }
    return 0;
}

struct ring { HANDLE *ev; int idx; volatile LONG *token; int bad; };

static DWORD WINAPI ring_proc(void *p)
{
    struct ring *r = p;
    for (int i = 0; i < RING_ROUNDS; i++) {
        if (WaitForSingleObject(r->ev[r->idx], 5000) != WAIT_OBJECT_0) { r->bad = i + 1; return 1; }
        LONG t = *r->token;
        if (t % 4 != r->idx) { r->bad = -(i + 1); SetEvent(r->ev[(r->idx + 1) % 4]); return 1; }
        *r->token = t + 1;
        SetEvent(r->ev[(r->idx + 1) % 4]);
    }
    return 0;
}

/* Token conservation under mixed wait-all / wait-any consumers: every released
 * semaphore token must be consumed exactly once or still be there at the end. */
#define WA_SEMS 3
#define WA_ROUNDS 6000
struct wa {
    HANDLE *sem;
    volatile LONG *released, *taken;
    volatile LONG *stop;
    int id, bad;
};

static DWORD WINAPI wa_producer(void *p)
{
    struct wa *a = p;
    unsigned x = 12345 + a->id;
    for (int i = 0; i < WA_ROUNDS; i++) {
        x = x * 1103515245 + 12345;
        int k = (x >> 16) % WA_SEMS;
        if (ReleaseSemaphore(a->sem[k], 1, NULL)) InterlockedIncrement(&a->released[k]);
        else if (GetLastError() != ERROR_TOO_MANY_POSTS) { a->bad = i + 1; return 1; }
        if (!(i & 63)) Sleep(0);
    }
    return 0;
}

static DWORD WINAPI wa_consumer(void *p)
{
    struct wa *a = p;
    unsigned x = 777 + a->id;
    while (!*a->stop) {
        x = x * 1103515245 + 12345;
        int k = (x >> 16) % WA_SEMS, k2 = (k + 1) % WA_SEMS;
        HANDLE h[2] = { a->sem[k], a->sem[k2] };
        if (a->id & 1) {
            DWORD r = WaitForMultipleObjects(2, h, TRUE, 2);
            if (r == WAIT_OBJECT_0) { InterlockedIncrement(&a->taken[k]); InterlockedIncrement(&a->taken[k2]); }
            else if (r != WAIT_TIMEOUT) { a->bad = (int)r | 0x10000; return 1; }
        } else {
            DWORD r = WaitForMultipleObjects(2, h, FALSE, 2);
            if (r == WAIT_OBJECT_0) InterlockedIncrement(&a->taken[k]);
            else if (r == WAIT_OBJECT_0 + 1) InterlockedIncrement(&a->taken[k2]);
            else if (r != WAIT_TIMEOUT) { a->bad = (int)r | 0x10000; return 1; }
        }
    }
    return 0;
}

static void test_stress_waitall(void)
{
    BEGIN("stress_waitall_conservation");
    HANDLE sem[WA_SEMS], th[8];
    volatile LONG released[WA_SEMS] = { 0 }, taken[WA_SEMS] = { 0 }, stop = 0;
    struct wa a[8];
    for (int i = 0; i < WA_SEMS; i++) sem[i] = CreateSemaphoreA(NULL, 0, 1000000, NULL);
    double t0 = now_ms();
    for (int i = 0; i < 8; i++) {
        a[i] = (struct wa){ sem, released, taken, &stop, i, 0 };
        th[i] = start_thread(i < 3 ? wa_producer : wa_consumer, &a[i]);
    }
    int hung = 0;
    for (int i = 0; i < 3; i++) hung += !join(th[i], 30000);
    Sleep(100);
    stop = 1;
    for (int i = 3; i < 8; i++) hung += !join(th[i], 10000);
    double dt = now_ms() - t0;
    EXPECT(!hung, "%d threads hung", hung);
    for (int i = 0; i < 8; i++) EXPECT(!a[i].bad, "thread %d failed: %#x", i, a[i].bad);
    LONG total_taken = 0;
    for (int i = 0; i < WA_SEMS; i++) {
        LONG left = query_sem(sem[i]).CurrentCount;
        total_taken += taken[i];
        EXPECT(released[i] == taken[i] + left, "sem %d: released %ld, taken %ld + left %ld", i, released[i], taken[i], left);
    }
    INFO("%ld tokens taken in %.0f ms", total_taken, dt);
    for (int i = 0; i < WA_SEMS; i++) CloseHandle(sem[i]);
    END();
}

static void test_stress(void)
{
    {
        BEGIN("stress_semaphore_producer_consumer");
        volatile LONG produced = 0, consumed = 0;
        HANDLE slots = CreateSemaphoreA(NULL, 16, 16, NULL), items = CreateSemaphoreA(NULL, 0, 16, NULL), th[4];
        struct pc c[4];
        double t0 = now_ms();
        for (int i = 0; i < 4; i++) {
            c[i] = (struct pc){ slots, items, &produced, &consumed, i < 2, 0 };
            th[i] = start_thread(pc_proc, &c[i]);
        }
        int hung = 0;
        for (int i = 0; i < 4; i++) hung += !join(th[i], 30000);
        double dt = now_ms() - t0;
        EXPECT(!hung, "%d threads hung", hung);
        for (int i = 0; i < 4; i++) EXPECT(!c[i].bad, "thread %d failed at %d", i, c[i].bad);
        EXPECT(produced == 2 * PC_ITERS && consumed == 2 * PC_ITERS, "produced %ld consumed %ld (expected %d)", produced, consumed, 2 * PC_ITERS);
        sem_info qs = query_sem(slots), qi = query_sem(items);
        EXPECT(qs.CurrentCount == 16 && qi.CurrentCount == 0, "final counts slots %lu items %lu (expected 16 0)", qs.CurrentCount, qi.CurrentCount);
        INFO("4 threads x %d in %.0f ms", PC_ITERS, dt);
        CloseHandle(slots); CloseHandle(items);
        END();
    }
    {
        BEGIN("stress_mutex_counter");
        volatile LONG counter = 0;
        HANDLE m = CreateMutexA(NULL, FALSE, NULL), th[4];
        struct mx x[4];
        double t0 = now_ms();
        for (int i = 0; i < 4; i++) {
            x[i] = (struct mx){ m, &counter, 0 };
            th[i] = start_thread(mx_proc, &x[i]);
        }
        int hung = 0;
        for (int i = 0; i < 4; i++) hung += !join(th[i], 30000);
        double dt = now_ms() - t0;
        EXPECT(!hung, "%d threads hung", hung);
        for (int i = 0; i < 4; i++) EXPECT(!x[i].bad, "thread %d failed at %d", i, x[i].bad);
        EXPECT(counter == 4 * MX_ITERS, "counter %ld (expected %d)", counter, 4 * MX_ITERS);
        mut_info q = query_mutant(m);
        EXPECT(q.CurrentCount == 1 && !q.AbandonedState, "final mutex count %ld abandoned %d", q.CurrentCount, q.AbandonedState);
        INFO("4 threads x %d in %.0f ms", MX_ITERS, dt);
        CloseHandle(m);
        END();
    }
    {
        BEGIN("stress_event_ring");
        volatile LONG token = 0;
        HANDLE ev[4], th[4];
        struct ring r[4];
        for (int i = 0; i < 4; i++) ev[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
        double t0 = now_ms();
        for (int i = 0; i < 4; i++) {
            r[i] = (struct ring){ ev, i, &token, 0 };
            th[i] = start_thread(ring_proc, &r[i]);
        }
        SetEvent(ev[0]);
        int hung = 0;
        for (int i = 0; i < 4; i++) hung += !join(th[i], 30000);
        double dt = now_ms() - t0;
        EXPECT(!hung, "%d threads hung", hung);
        for (int i = 0; i < 4; i++) EXPECT(!r[i].bad, "thread %d failed at %d", i, r[i].bad);
        EXPECT(token == 4 * RING_ROUNDS, "token %ld (expected %d)", token, 4 * RING_ROUNDS);
        INFO("%d handoffs in %.0f ms", 4 * RING_ROUNDS, dt);
        for (int i = 0; i < 4; i++) CloseHandle(ev[i]);
        END();
    }
}

/* ---- main ---- */

static DWORD WINAPI watchdog_proc(void *p)
{
    Sleep((DWORD)(ULONG_PTR)p);
    printf("FAIL watchdog\n");
    fflush(stdout);
    ExitProcess(2);
    return 0;
}

int main(int argc, char **argv)
{
    load_ntdll();
    if (argc >= 3 && !strcmp(argv[1], "child")) {
        CloseHandle(start_thread(watchdog_proc, (void *)(ULONG_PTR)30000));
        ExitProcess(child_main(argc, argv));
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    GetModuleFileNameA(NULL, g_exe, sizeof(g_exe));
    CloseHandle(start_thread(watchdog_proc, (void *)(ULONG_PTR)60000));
    double t0 = now_ms();

    test_event_auto();
    test_event_auto_one_waiter();
    test_event_manual_all();
    test_event_reset();
    test_pulse_manual();
    test_pulse_auto();
    test_pulse_no_waiter();
    test_nt_set_reset_prev();
    test_nt_query_event();
    test_semaphore_basic();
    test_semaphore_wait();
    test_semaphore_wakes_n();
    test_mutex_recursion();
    test_mutex_nonowner();
    test_mutex_abandoned();
    test_mutex_abandoned_wfmo();
    test_wfmo_any();
    test_wfmo_all_atomic();
    test_wfmo_all_blocked();
    test_wfmo_all_mutex_other_owner();
    test_wfmo_dup_handle();
    test_wfmo_64();
    test_timeouts();
    test_apc();
    test_soaw();
    test_xproc();
    test_proc_thread_handles();
    test_close_while_waiting();
    test_killed_waiters();
    test_named();
    test_stress();
    test_stress_waitall();

    if (g_skip) printf("skipped %d known Wine deviations\n", g_skip);
    printf("runtime %.1f s%s\n", (now_ms() - t0) / 1000.0, g_is_wine ? " (wine)" : "");
    printf("sync_semantics: %d/%d passed\n", g_pass, g_total);
    fflush(stdout);
    return g_fail ? 1 : 0;
}
