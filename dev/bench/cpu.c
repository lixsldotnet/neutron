/* neutron bench: CPU and Windows API micro benchmarks for the Wine + FEX path.
 *
 * x86_64 Windows program, runs translated by FEX like game code. Every test does a
 * fixed amount of work and the program prints one JSON line:
 *   {"bench":"cpu","ns":{"<test>":<ns per op>,...}}
 *
 * Build: x86_64-w64-mingw32-clang -O2 -o cpu.exe cpu.c
 * Run:   wine cpu.exe [filter]   (filter: comma list, only tests whose name contains
 *                                  one of the words)
 *        dev/bench/run.sh runs it through tool/neutron and takes the median of K runs.
 *
 * Tests: pure x86 code (branchy integer, SSE float, scalar load/store, calls),
 * memory (memcpy/memset small and large, rep movsb), atomics and locks across
 * threads, wineserver round trips (events, semaphores, waits), clocks, virtual
 * memory, heap, threads, and calls into ARM64EC system DLLs (thunk cost). */
#include <windows.h>
#include <emmintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LARGE_INTEGER qpf;
static const char *filter;
static int first = 1;
static volatile uint64_t sink; /* keeps results alive */
#define BARRIER() __asm__ volatile("" ::: "memory") /* keeps repeated stores and calls */

/* filter is a comma list; a test runs when its name contains one of the words */
static int selected(const char *name)
{
    if (!filter) return 1;
    const char *p = filter;
    while (*p)
    {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        for (const char *n = name; *n; n++)
            if (len && !strncmp(n, p, len)) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

static double now_ns(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1e9 / (double)qpf.QuadPart;
}

static void report(const char *name, double ns_per_op)
{
    printf("%s\"%s\":%.2f", first ? "" : ",", name, ns_per_op);
    first = 0;
    fflush(stdout);
}

/* Runs fn(ops) once and reports ns per op. */
#define BENCH(name, ops, fn)                                       \
    do {                                                           \
        if (!selected(name)) break;                                \
        double t0_ = now_ns();                                     \
        fn(ops);                                                   \
        report(name, (now_ns() - t0_) / (double)(ops));            \
    } while (0)

/*---------------------------------------------------------------------------
 * pure x86 code
 *---------------------------------------------------------------------------*/

static void __attribute__((noinline)) int_branchy(uint64_t n)
{
    uint64_t x = 88172645463325252ull, acc = 0;
    for (uint64_t i = 0; i < n; i++)
    {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        switch (x & 7)
        {
        case 0: acc += x >> 3; break;
        case 1: acc ^= x * 3; break;
        case 2: acc -= (x & 0xffff); break;
        case 3: if (acc & 1) acc = acc * 5 + 1; else acc >>= 1; break;
        case 4: acc += __builtin_popcountll(x); break;
        default: acc = (acc << 1) | (x >> 63); break;
        }
    }
    sink = acc;
}

static float vbuf_a[4096] __attribute__((aligned(16)));
static float vbuf_b[4096] __attribute__((aligned(16)));

/* op = one pass over 4096 floats (16 KB, stays in L1) with SSE mul/add/sqrt/min/max */
static void __attribute__((noinline)) float_sse(uint64_t n)
{
    const __m128 k1 = _mm_set1_ps(1.0001f), k2 = _mm_set1_ps(0.5f), lo = _mm_set1_ps(-100.f), hi = _mm_set1_ps(100.f);
    __m128 acc = _mm_setzero_ps();
    for (uint64_t r = 0; r < n; r++)
    {
        for (int i = 0; i < 4096; i += 4)
        {
            __m128 a = _mm_load_ps(vbuf_a + i), b = _mm_load_ps(vbuf_b + i);
            __m128 v = _mm_add_ps(_mm_mul_ps(a, k1), _mm_mul_ps(b, k2));
            v = _mm_min_ps(_mm_max_ps(v, lo), hi);
            acc = _mm_add_ps(acc, _mm_sqrt_ps(_mm_mul_ps(v, v)));
            _mm_store_ps(vbuf_b + i, v);
        }
    }
    float out[4];
    _mm_storeu_ps(out, acc);
    sink = (uint64_t)out[0];
}

/* Scalar loads and stores (game-style struct updates): FEX turns every one into a
 * TSO-ordered access, so this is the test that shows the cost of the memory model. */
struct obj { int32_t hp, armor, x, y, vx, vy, flags, pad; };
static struct obj objs[1024];

static void __attribute__((noinline)) scalar_loadstore(uint64_t n)
{
    for (uint64_t r = 0; r < n; r++)
    {
        for (int i = 0; i < 1024; i++)
        {
            struct obj *o = &objs[i];
            o->x += o->vx;
            o->y += o->vy;
            if (o->x > 1000 || o->x < -1000) { o->vx = -o->vx; o->flags++; }
            if (o->y > 1000 || o->y < -1000) { o->vy = -o->vy; o->flags++; }
            o->hp -= (o->flags & 1);
        }
    }
    sink = (uint64_t)objs[7].x;
}

/* op = one pointer step through a shuffled 4 MB list (cache-missing dependent loads) */
static void __attribute__((noinline)) pointer_chase(uint64_t n)
{
    enum { N = 1 << 19 };
    uint64_t *next = malloc(N * sizeof(*next)), *perm = malloc(N * sizeof(*perm));
    for (uint64_t i = 0; i < N; i++) perm[i] = i;
    uint64_t x = 12345;
    for (uint64_t i = N - 1; i > 0; i--)
    {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        uint64_t j = x % (i + 1), t = perm[i];
        perm[i] = perm[j]; perm[j] = t;
    }
    for (uint64_t i = 0; i < N; i++) next[perm[i]] = perm[(i + 1) % N];
    double t0 = now_ns();
    uint64_t p = 0;
    for (uint64_t i = 0; i < n; i++) p = next[p];
    double t = now_ns() - t0;
    sink = p;
    free(next); free(perm);
    report("pointer_chase", t / (double)n);
}

typedef uint64_t (*vfn)(uint64_t);
static uint64_t __attribute__((noinline)) f0(uint64_t v) { return v * 3 + 1; }
static uint64_t __attribute__((noinline)) f1(uint64_t v) { return v ^ (v >> 3); }
static uint64_t __attribute__((noinline)) f2(uint64_t v) { return v + 0x9e3779b9; }
static uint64_t __attribute__((noinline)) f3(uint64_t v) { return (v << 1) | (v >> 63); }
static vfn vtable[4] = { f0, f1, f2, f3 };
static volatile int vsel = 1;

/* op = one indirect call (virtual dispatch: FEX looks up the target block) */
static void __attribute__((noinline)) indirect_call(uint64_t n)
{
    uint64_t v = 1;
    int s = vsel;
    for (uint64_t i = 0; i < n; i++) v = vtable[(i * s + (v & 1)) & 3](v);
    sink = v;
}

/* Binary call tree of depth 16: 131071 calls, none of them a tail call */
static uint64_t __attribute__((noinline)) tree(uint64_t d, uint64_t v)
{
    __asm__ volatile("" : "+r"(v));
    if (!d) return v + 1;
    return tree(d - 1, v * 3) ^ tree(d - 1, v + 7);
}

/* op = one call + return */
static void __attribute__((noinline)) call_ret(uint64_t n)
{
    uint64_t s = 0;
    for (uint64_t i = 0; i < n / 131071; i++) s += tree(16, i);
    sink = s;
}

/*---------------------------------------------------------------------------
 * memory
 *---------------------------------------------------------------------------*/

#define BIG (16u << 20)
static char *big_a, *big_b;
static char small_a[8192], small_b[8192];
static volatile size_t small_len = 64;

static void memcpy_large(uint64_t n) { for (uint64_t i = 0; i < n; i++) { memcpy(big_b, big_a, BIG); BARRIER(); } sink = big_b[123]; }
static void memset_large(uint64_t n) { for (uint64_t i = 0; i < n; i++) { memset(big_b, (int)i, BIG); BARRIER(); } sink = big_b[321]; }

static void memcpy_small(uint64_t n)
{
    size_t len = small_len;
    for (uint64_t i = 0; i < n; i++) { memcpy(small_b + (i & 63), small_a + ((i >> 6) & 63), len + (i & 7)); BARRIER(); }
    sink = small_b[5];
}

static void memset_small(uint64_t n)
{
    size_t len = small_len * 2;
    for (uint64_t i = 0; i < n; i++) { memset(small_b + (i & 63), (int)i, len); BARRIER(); }
    sink = small_b[9];
}

/* Inline rep movsb as MSVC emits it for memcpy (FEX MemcpySetTSO applies here) */
static void rep_movsb_4k(uint64_t n)
{
    for (uint64_t i = 0; i < n; i++)
    {
        void *d = small_b, *s = small_a;
        size_t c = 4096;
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(c) : : "memory");
    }
    sink = small_b[11];
}

/*---------------------------------------------------------------------------
 * threads: atomics and locks
 *---------------------------------------------------------------------------*/

#define MAXT 8
static volatile LONG64 shared_counter __attribute__((aligned(64)));
static CRITICAL_SECTION cs;
static SRWLOCK srw = SRWLOCK_INIT;
static volatile uint64_t lock_counter;
static uint64_t per_thread_ops;
static HANDLE go_event;

static void atomic_single(uint64_t n) { for (uint64_t i = 0; i < n; i++) InterlockedIncrement64(&shared_counter); }

static DWORD WINAPI atomic_worker(void *arg)
{
    WaitForSingleObject(go_event, INFINITE);
    for (uint64_t i = 0; i < per_thread_ops; i++) InterlockedIncrement64(&shared_counter);
    return 0;
}

static DWORD WINAPI cs_worker(void *arg)
{
    WaitForSingleObject(go_event, INFINITE);
    for (uint64_t i = 0; i < per_thread_ops; i++) { EnterCriticalSection(&cs); lock_counter++; LeaveCriticalSection(&cs); }
    return 0;
}

static DWORD WINAPI srw_worker(void *arg)
{
    WaitForSingleObject(go_event, INFINITE);
    for (uint64_t i = 0; i < per_thread_ops; i++) { AcquireSRWLockExclusive(&srw); lock_counter++; ReleaseSRWLockExclusive(&srw); }
    return 0;
}

/* Starts nthreads workers on a common start signal, reports ns per op over all threads. */
static void run_threads(const char *name, LPTHREAD_START_ROUTINE fn, int nthreads, uint64_t ops)
{
    if (!selected(name)) return;
    HANDLE th[MAXT];
    per_thread_ops = ops / nthreads;
    go_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    for (int i = 0; i < nthreads; i++) th[i] = CreateThread(NULL, 0, fn, NULL, 0, NULL);
    Sleep(20);
    double t0 = now_ns();
    SetEvent(go_event);
    WaitForMultipleObjects(nthreads, th, TRUE, INFINITE);
    double t = now_ns() - t0;
    for (int i = 0; i < nthreads; i++) CloseHandle(th[i]);
    CloseHandle(go_event);
    report(name, t / (double)(per_thread_ops * nthreads));
}

static void cs_single(uint64_t n) { for (uint64_t i = 0; i < n; i++) { EnterCriticalSection(&cs); lock_counter++; LeaveCriticalSection(&cs); } }
static void srw_single(uint64_t n) { for (uint64_t i = 0; i < n; i++) { AcquireSRWLockExclusive(&srw); lock_counter++; ReleaseSRWLockExclusive(&srw); } }

/*---------------------------------------------------------------------------
 * wineserver path: events, semaphores, waits
 *---------------------------------------------------------------------------*/

static HANDLE ping, pong;
static int pp_sem;

static DWORD WINAPI pong_worker(void *arg)
{
    uint64_t n = per_thread_ops;
    for (uint64_t i = 0; i < n; i++)
    {
        WaitForSingleObject(ping, INFINITE);
        if (pp_sem) ReleaseSemaphore(pong, 1, NULL); else SetEvent(pong);
    }
    return 0;
}

/* op = one round trip: signal the other thread and wait for its answer */
static void pingpong(const char *name, int sem, uint64_t n)
{
    if (!selected(name)) return;
    pp_sem = sem;
    ping = sem ? CreateSemaphoreA(NULL, 0, 1, NULL) : CreateEventA(NULL, FALSE, FALSE, NULL);
    pong = sem ? CreateSemaphoreA(NULL, 0, 1, NULL) : CreateEventA(NULL, FALSE, FALSE, NULL);
    per_thread_ops = n;
    HANDLE th = CreateThread(NULL, 0, pong_worker, NULL, 0, NULL);
    Sleep(20);
    double t0 = now_ns();
    for (uint64_t i = 0; i < n; i++)
    {
        if (sem) ReleaseSemaphore(ping, 1, NULL); else SetEvent(ping);
        WaitForSingleObject(pong, INFINITE);
    }
    double t = now_ns() - t0;
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th); CloseHandle(ping); CloseHandle(pong);
    report(name, t / (double)n);
}

static HANDLE signaled;
static void wait_signaled(uint64_t n) { for (uint64_t i = 0; i < n; i++) WaitForSingleObject(signaled, 0); }
static void set_event(uint64_t n) { for (uint64_t i = 0; i < n; i++) SetEvent(signaled); }

/* op = one Sleep(1), reports the real duration (frame pacing in games) */
static void sleep_1ms(uint64_t n) { for (uint64_t i = 0; i < n; i++) Sleep(1); }
static void yield(uint64_t n) { for (uint64_t i = 0; i < n; i++) SwitchToThread(); }

/*---------------------------------------------------------------------------
 * clocks and thunks (calls from FEX code into ARM64EC system DLLs)
 *---------------------------------------------------------------------------*/

static void qpc(uint64_t n) { LARGE_INTEGER t; uint64_t s = 0; for (uint64_t i = 0; i < n; i++) { QueryPerformanceCounter(&t); s += t.QuadPart; } sink = s; }
static void tickcount(uint64_t n) { uint64_t s = 0; for (uint64_t i = 0; i < n; i++) s += GetTickCount(); sink = s; }
static void thread_id(uint64_t n) { uint64_t s = 0; for (uint64_t i = 0; i < n; i++) s += GetCurrentThreadId(); sink = s; }

static DWORD tls;
static void tls_get(uint64_t n) { uint64_t s = 0; for (uint64_t i = 0; i < n; i++) s += (uintptr_t)TlsGetValue(tls); sink = s; }

/*---------------------------------------------------------------------------
 * virtual memory, heap, threads
 *---------------------------------------------------------------------------*/

/* op = VirtualAlloc 64 KB commit, touch, protect read-only and back, free */
static void virtual_cycle(uint64_t n)
{
    DWORD old;
    for (uint64_t i = 0; i < n; i++)
    {
        char *p = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        p[0] = 1; p[40000] = 2;
        VirtualProtect(p, 65536, PAGE_READONLY, &old);
        VirtualProtect(p, 65536, PAGE_READWRITE, &old);
        VirtualFree(p, 0, MEM_RELEASE);
    }
}

/* op = one HeapAlloc + HeapFree, 16..256 bytes, 256 live blocks */
static void heap_small(uint64_t n)
{
    HANDLE h = GetProcessHeap();
    void *live[256] = { 0 };
    uint64_t x = 7;
    for (uint64_t i = 0; i < n; i++)
    {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        int slot = x & 255;
        if (live[slot]) HeapFree(h, 0, live[slot]);
        live[slot] = HeapAlloc(h, 0, 16 + ((x >> 8) & 240));
    }
    for (int i = 0; i < 256; i++) if (live[i]) HeapFree(h, 0, live[i]);
}

/* same through the C runtime (ucrtbase malloc/free) */
static void malloc_small(uint64_t n)
{
    void *live[256] = { 0 };
    uint64_t x = 7;
    for (uint64_t i = 0; i < n; i++)
    {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        int slot = x & 255;
        free(live[slot]);
        live[slot] = malloc(16 + ((x >> 8) & 240));
    }
    for (int i = 0; i < 256; i++) free(live[i]);
}

static DWORD WINAPI empty_thread(void *arg) { return 0; }
static void thread_create(uint64_t n)
{
    for (uint64_t i = 0; i < n; i++)
    {
        HANDLE t = CreateThread(NULL, 0, empty_thread, NULL, 0, NULL);
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
    }
}

int main(int argc, char **argv)
{
    QueryPerformanceFrequency(&qpf);
    if (argc > 1 && strcmp(argv[1], "--nop") == 0) { printf("{\"bench\":\"nop\"}\n"); return 0; }
    filter = argc > 1 ? argv[1] : NULL;

    for (int i = 0; i < 4096; i++) { vbuf_a[i] = (float)(i % 97) - 48.f; vbuf_b[i] = (float)(i % 13); }
    for (int i = 0; i < 1024; i++) { objs[i].vx = i % 7 - 3; objs[i].vy = i % 5 - 2; objs[i].hp = 100; }
    big_a = VirtualAlloc(NULL, BIG, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    big_b = VirtualAlloc(NULL, BIG, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    memset(big_a, 1, BIG); memset(big_b, 2, BIG);
    InitializeCriticalSection(&cs);
    signaled = CreateEventA(NULL, TRUE, TRUE, NULL);
    tls = TlsAlloc();
    TlsSetValue(tls, (void *)42);

    printf("{\"bench\":\"cpu\",\"ns\":{");
    BENCH("int_branchy", 50000000, int_branchy);
    BENCH("float_sse_16k", 20000, float_sse);
    BENCH("scalar_loadstore_1k", 50000, scalar_loadstore);
    if (selected("pointer_chase")) pointer_chase(5000000);
    BENCH("indirect_call", 30000000, indirect_call);
    BENCH("call_ret", 131071 * 200, call_ret);
    BENCH("memcpy_16m", 40, memcpy_large);
    BENCH("memset_16m", 60, memset_large);
    BENCH("memcpy_64", 10000000, memcpy_small);
    BENCH("memset_128", 10000000, memset_small);
    BENCH("rep_movsb_4k", 1000000, rep_movsb_4k);
    BENCH("atomic_inc_1t", 50000000, atomic_single);
    run_threads("atomic_inc_4t", atomic_worker, 4, 20000000);
    BENCH("cs_1t", 20000000, cs_single);
    run_threads("cs_2t", cs_worker, 2, 4000000);
    BENCH("srw_1t", 20000000, srw_single);
    run_threads("srw_2t", srw_worker, 2, 4000000);
    pingpong("event_pingpong", 0, 20000);
    pingpong("semaphore_pingpong", 1, 20000);
    BENCH("wait_signaled", 200000, wait_signaled);
    BENCH("set_event", 200000, set_event);
    BENCH("sleep_1ms", 100, sleep_1ms);
    BENCH("switch_to_thread", 200000, yield);
    BENCH("qpc", 5000000, qpc);
    BENCH("gettickcount", 20000000, tickcount);
    BENCH("getcurrentthreadid", 20000000, thread_id);
    BENCH("tlsgetvalue", 20000000, tls_get);
    BENCH("virtual_cycle_64k", 20000, virtual_cycle);
    BENCH("heap_small", 5000000, heap_small);
    BENCH("malloc_small", 5000000, malloc_small);
    BENCH("thread_create", 1000, thread_create);
    printf("}}\n");
    return 0;
}
