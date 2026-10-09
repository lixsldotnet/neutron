/* neutron headless test: QueryPerformanceCounter.
 * Checks that QPC is monotonic in one thread and across threads, that it agrees
 * with the NtQueryPerformanceCounter syscall (Wine's unix side), that the
 * frequency is right and that QPC, GetTickCount64, timeGetTime and the system
 * time run at the same rate. Prints the cost per call of each clock.
 * Console only, no windows.
 *
 * Prints "ok <name>" or "FAIL <name>: <detail>", then one "cost" line and
 * "qpc: <passed>/<total> passed".
 *
 * Build: x86_64-w64-mingw32-clang -O2 -o qpc.exe qpc.c
 *        (arm64ec-w64-mingw32-clang for the ARM64EC side, no FEX in between)
 * Run:   tool/neutron runinprefix qpc.exe [seconds for the rate check, default 3]
 * Exit code: 0 = all checks passed, 1 = a check failed. */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

typedef LONG (WINAPI *nt_qpc_fn)(LARGE_INTEGER *, LARGE_INTEGER *);
typedef BOOL (WINAPI *rtl_qpc_fn)(LARGE_INTEGER *);
typedef DWORD (WINAPI *tgt_fn)(void);

static nt_qpc_fn pNtQueryPerformanceCounter;
static rtl_qpc_fn pRtlQueryPerformanceCounter;
static tgt_fn ptimeGetTime;
static int g_is_wine, g_pass, g_total;

static void result(const char *name, int ok, const char *fmt, ...)
{
    g_total++;
    if (ok) { g_pass++; printf("ok %s\n", name); }
    else
    {
        va_list args;
        printf("FAIL %s: ", name);
        va_start(args, fmt); vprintf(fmt, args); va_end(args);
        printf("\n");
    }
    fflush(stdout);
}

static LONGLONG qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static LONGLONG nt_qpc(void) { LARGE_INTEGER t; pNtQueryPerformanceCounter(&t, NULL); return t.QuadPart; }
static LONGLONG rtl_qpc(void) { LARGE_INTEGER t; pRtlQueryPerformanceCounter(&t); return t.QuadPart; }

static LONGLONG system_time(void)
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return ((LONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

/* ---- monotonic across threads ----
 * Each thread reads the newest value any thread has published, then QPC, which
 * must not be older; then it publishes its own value. */
static volatile LONGLONG g_latest;
static volatile LONG g_go, g_violations;
static LONGLONG g_worst;

static DWORD WINAPI cross_thread(void *arg)
{
    int i, n = (int)(INT_PTR)arg;
    while (!g_go) YieldProcessor();
    for (i = 0; i < n; i++)
    {
        LONGLONG seen = InterlockedCompareExchange64(&g_latest, 0, 0), t, cur;
        t = (i & 1) ? qpc() : nt_qpc();
        if (t < seen)
        {
            InterlockedIncrement(&g_violations);
            if (seen - t > g_worst) g_worst = seen - t;
        }
        cur = seen;
        while (cur < t)
        {
            LONGLONG prev = InterlockedCompareExchange64(&g_latest, t, cur);
            if (prev == cur) break;
            cur = prev;
        }
    }
    return 0;
}

static double ns_per_call(LONGLONG (*fn)(void), int n)
{
    LONGLONG best = 0, sink = 0;
    int r, i;
    for (r = 0; r < 3; r++)
    {
        LONGLONG t0 = qpc(), t1;
        for (i = 0; i < n; i++) sink += fn();
        t1 = qpc();
        if (!r || t1 - t0 < best) best = t1 - t0;
    }
    if (sink == 42) printf(" ");
    return best * 1e9 / 10000000.0 / n;
}

static LONGLONG tick64(void) { return GetTickCount64(); }
static LONGLONG tgt(void) { return ptimeGetTime(); }

int main(int argc, char **argv)
{
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    double seconds = argc > 1 ? atof(argv[1]) : 3;
    LARGE_INTEGER freq;
    LONGLONG prev, t, a, b, c;
    HANDLE threads[8];
    int i, bad, nthreads = 8;

    pNtQueryPerformanceCounter = (nt_qpc_fn)GetProcAddress(nt, "NtQueryPerformanceCounter");
    pRtlQueryPerformanceCounter = (rtl_qpc_fn)GetProcAddress(nt, "RtlQueryPerformanceCounter");
    ptimeGetTime = (tgt_fn)GetProcAddress(LoadLibraryA("winmm.dll"), "timeGetTime");
    g_is_wine = GetProcAddress(nt, "wine_get_version") != NULL;
    if (!pNtQueryPerformanceCounter || !pRtlQueryPerformanceCounter || !ptimeGetTime)
    {
        printf("FAIL setup: missing exports\n");
        return 1;
    }

    QueryPerformanceFrequency(&freq);
    result("frequency", g_is_wine ? freq.QuadPart == 10000000 : freq.QuadPart > 0,
           "%lld", freq.QuadPart);
    {
        LARGE_INTEGER nf, nc;
        pNtQueryPerformanceCounter(&nc, &nf);
        result("frequency_nt", nf.QuadPart == freq.QuadPart, "%lld vs %lld", nf.QuadPart, freq.QuadPart);
    }

    /* one thread, 10M reads */
    prev = qpc(); bad = 0;
    for (i = 0; i < 10000000; i++)
    {
        t = qpc();
        if (t < prev) bad++;
        prev = t;
    }
    result("monotonic_thread", !bad, "%d decreases in 10M reads", bad);

    /* fast path between two syscalls: a <= b <= c */
    bad = 0;
    for (i = 0; i < 1000000; i++)
    {
        a = nt_qpc(); b = qpc(); c = nt_qpc();
        if (a > b || b > c) bad++;
    }
    result("between_syscalls", !bad, "%d of 1M reads outside [NtQPC, NtQPC]", bad);
    bad = 0;
    for (i = 0; i < 1000000; i++)
    {
        a = qpc(); b = nt_qpc(); c = rtl_qpc();
        if (a > b || b > c) bad++;
    }
    result("syscall_between", !bad, "%d of 1M NtQPC reads outside [QPC, RtlQPC]", bad);

    /* across threads, QPC and the syscall mixed */
    for (i = 0; i < nthreads; i++) threads[i] = CreateThread(NULL, 0, cross_thread, (void *)(INT_PTR)2000000, 0, NULL);
    Sleep(10);
    g_go = 1;
    WaitForMultipleObjects(nthreads, threads, TRUE, 60000);
    for (i = 0; i < nthreads; i++) CloseHandle(threads[i]);
    result("monotonic_threads", !g_violations, "%ld violations in %d threads x 2M reads, worst %lld ticks",
           g_violations, nthreads, g_worst);

    /* rates: QPC against the tick count, timeGetTime and the system clock */
    {
        LONGLONG q0 = qpc(), k0 = GetTickCount64(), s0 = system_time(), q1, k1, s1;
        DWORD m0 = ptimeGetTime(), m1;
        double dq, dk, dm, ds;
        int steps = (int)(seconds * 10), worst_tick = 0, worst_tgt = 0;
        for (i = 0; i < steps; i++)
        {
            Sleep(100);
            q1 = qpc(); k1 = GetTickCount64(); m1 = ptimeGetTime();
            dq = (q1 - q0) / 10000.0; dk = (double)(k1 - k0); dm = (double)(DWORD)(m1 - m0);
            if (abs((int)(dq - dk)) > worst_tick) worst_tick = abs((int)(dq - dk));
            if (abs((int)(dq - dm)) > worst_tgt) worst_tgt = abs((int)(dq - dm));
        }
        s1 = system_time();
        q1 = qpc();
        dq = (q1 - q0) / 10000.0; ds = (s1 - s0) / 10000.0;
        /* GetTickCount64 comes from KUSER_SHARED_DATA, which wineserver updates every 16 ms */
        result("rate_tickcount", worst_tick <= 40, "QPC and GetTickCount64 differ by up to %d ms", worst_tick);
        result("rate_timegettime", worst_tgt <= 2, "QPC and timeGetTime differ by up to %d ms", worst_tgt);
        result("rate_systemtime", (dq - ds) < 5 + ds * 0.001 && (ds - dq) < 5 + ds * 0.001,
               "QPC %.1f ms against system time %.1f ms", dq, ds);
        printf("rates over %.1f s: QPC %.1f ms, tick %+d ms, timeGetTime %+d ms (worst), system time %+.1f ms\n",
               seconds, dq, worst_tick, worst_tgt, ds - dq);
    }

    /* Sleep(50) seen through QPC */
    a = qpc(); Sleep(50); b = qpc();
    result("sleep_50ms", b - a >= 495000 && b - a < 2000000, "%.2f ms", (b - a) / 10000.0);

    printf("cost ns/call: QueryPerformanceCounter %.1f, RtlQueryPerformanceCounter %.1f, NtQueryPerformanceCounter %.1f, "
           "GetTickCount64 %.1f, timeGetTime %.1f\n",
           ns_per_call(qpc, 2000000), ns_per_call(rtl_qpc, 2000000), ns_per_call(nt_qpc, 2000000),
           ns_per_call(tick64, 2000000), ns_per_call(tgt, 2000000));

    printf("qpc: %d/%d passed\n", g_pass, g_total);
    return g_pass == g_total ? 0 : 1;
}
