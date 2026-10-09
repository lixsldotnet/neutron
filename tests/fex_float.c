/* neutron headless test: x86 SSE float results under FEX around calls into
 * ARM64EC code.
 *
 * FEX sets FPCR.NEP/AH (x86 NaN rules, x86 min/max, no input flushing under FTZ)
 * only in blocks that need them, and runs small add/sub/mul/div blocks that end
 * in a call into ARM64EC code without them, fixing NaN lanes afterwards (fex 0004).
 * This test checks that x86 results stay exact: default NaN sign, QNaN/SNaN
 * propagation, min/max with NaN and +-0, denormals with and without DAZ/FTZ,
 * compares, float to int conversions, rcp/rsqrt and the upper lanes of scalar ops.
 * Every check runs in four places:
 *   plain   the instruction alone
 *   after   right after a call into ARM64EC code (GetTickCount, FPCR AFP bits off)
 *   before  in a small block between two such calls (FEX runs it without AH)
 *   warm    like before, after a block that set AH
 * Expected values are the x86 results (Intel SDM), not FEX's.
 *
 * Build: x86_64-w64-mingw32-clang -O2 -o fex_float.exe fex_float.c
 * Run:   tool/neutron runinprefix fex_float.exe [-v] [bench]
 *        -v prints every check, bench also prints the cost of a call into ARM64EC
 *        code after different kinds of x86 code (ns per call, best of 40 runs)
 * Exit code: 0 = all checks passed. */
#include <emmintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <xmmintrin.h>

#define NATIVE() ((void)GetTickCount())

static int verbose, fails, checks;
static const char *const mode_name[] = {"plain", "after", "before", "warm"};

static void __attribute__((noinline)) chk(const char *name, int mode, uint64_t got, uint64_t want)
{
    checks++;
    if (got != want) fails++;
    if (verbose || got != want)
        printf("%-4s %-44s %-6s got %016llx want %016llx\n", got == want ? "ok" : "FAIL", name, mode_name[mode],
               (unsigned long long)got, (unsigned long long)want);
}

static float f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t u32(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static double f64(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static uint64_t u64(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

/* Each check is expanded four times with M as a compile time constant, so the
 * instruction and the call sit in one block without a branch between them.
 * The "memory" clobber keeps the asm in order with the call. */
#define BEFORE() do { if (M == 1 || M == 2) NATIVE(); if (M == 3) __asm__ volatile("minss %%xmm15, %%xmm15\n jmp 1f\n 1:" ::: "xmm15", "memory"); } while (0)
#define AFTER()  do { if (M >= 2) NATIVE(); } while (0)
#define RUN4(...) do { { enum { M = 0 }; __VA_ARGS__ } { enum { M = 1 }; __VA_ARGS__ } \
                       { enum { M = 2 }; __VA_ARGS__ } { enum { M = 3 }; __VA_ARGS__ } } while (0)

/* scalar float, insn %1, %0 (x86: %0 = %0 op %1) */
#define SS(name, insn, a, b, want) RUN4({ float r_ = f32(a), y_ = f32(b); BEFORE(); \
    __asm__ volatile(insn " %1, %0" : "+x"(r_) : "x"(y_) : "memory"); AFTER(); chk(name, M, u32(r_), want); })
/* scalar double */
#define SD(name, insn, a, b, want) RUN4({ double r_ = f64(a), y_ = f64(b); BEFORE(); \
    __asm__ volatile(insn " %1, %0" : "+x"(r_) : "x"(y_) : "memory"); AFTER(); chk(name, M, u64(r_), want); })
/* four float lanes (packed or scalar insn, all lanes checked) */
#define V4(name, insn, A, B, W) RUN4({ __m128 r_ = _mm_loadu_ps((const float *)(A)), y_ = _mm_loadu_ps((const float *)(B)); \
    BEFORE(); __asm__ volatile(insn " %1, %0" : "+x"(r_) : "x"(y_) : "memory"); AFTER(); \
    uint32_t o_[4]; _mm_storeu_ps((float *)o_, r_); \
    for (int l_ = 0; l_ < 4; l_++) { char n_[64]; snprintf(n_, sizeof(n_), "%s [%d]", name, l_); chk(n_, M, o_[l_], (W)[l_]); } })
/* two double lanes */
#define V2(name, insn, A, B, W) RUN4({ __m128d r_ = _mm_loadu_pd((const double *)(A)), y_ = _mm_loadu_pd((const double *)(B)); \
    BEFORE(); __asm__ volatile(insn " %1, %0" : "+x"(r_) : "x"(y_) : "memory"); AFTER(); \
    uint64_t o_[2]; _mm_storeu_pd((double *)o_, r_); \
    for (int l_ = 0; l_ < 2; l_++) { char n_[64]; snprintf(n_, sizeof(n_), "%s [%d]", name, l_); chk(n_, M, o_[l_], (W)[l_]); } })
/* comiss/ucomiss a, b (Intel order) and a setcc */
#define CMP(name, insn, setcc, a, b, want) RUN4({ float x_ = f32(a), y_ = f32(b); int c_; BEFORE(); \
    __asm__ volatile("xor %k0, %k0\n " insn " %2, %1\n " setcc " %b0" : "=&r"(c_) : "x"(x_), "x"(y_) : "cc", "memory"); \
    AFTER(); chk(name, M, (uint32_t)c_, want); })
/* float to int32: insn xmm, r32 */
#define CVTI(name, insn, a, want) RUN4({ float x_ = f32(a); int i_; BEFORE(); \
    __asm__ volatile(insn " %1, %0" : "=r"(i_) : "x"(x_) : "memory"); AFTER(); chk(name, M, (uint32_t)i_, want); })
/* double to int64 */
#define CVTQ(name, insn, a, want) RUN4({ double x_ = f64(a); long long i_; BEFORE(); \
    __asm__ volatile(insn " %1, %0" : "=r"(i_) : "x"(x_) : "memory"); AFTER(); chk(name, M, (uint64_t)i_, want); })
/* int to float into the low lane of four float lanes */
#define ITOF4(name, insn, A, i, W) RUN4({ __m128 r_ = _mm_loadu_ps((const float *)(A)); long long i_ = (i); BEFORE(); \
    __asm__ volatile(insn " %1, %0" : "+x"(r_) : "r"(i_) : "memory"); AFTER(); \
    uint32_t o_[4]; _mm_storeu_ps((float *)o_, r_); \
    for (int l_ = 0; l_ < 4; l_++) { char n_[64]; snprintf(n_, sizeof(n_), "%s [%d]", name, l_); chk(n_, M, o_[l_], (W)[l_]); } })
#define ITOF2(name, insn, A, i, W) RUN4({ __m128d r_ = _mm_loadu_pd((const double *)(A)); long long i_ = (i); BEFORE(); \
    __asm__ volatile(insn " %1, %0" : "+x"(r_) : "r"(i_) : "memory"); AFTER(); \
    uint64_t o_[2]; _mm_storeu_pd((double *)o_, r_); \
    for (int l_ = 0; l_ < 2; l_++) { char n_[64]; snprintf(n_, sizeof(n_), "%s [%d]", name, l_); chk(n_, M, o_[l_], (W)[l_]); } })
#define L4(...) ((const uint32_t[4]){__VA_ARGS__})
#define L2(...) ((const uint64_t[2]){__VA_ARGS__})

/* float bit patterns */
#define ONE   0x3f800000u
#define TWO   0x40000000u
#define HALF  0x3f000000u
#define PZERO 0x00000000u
#define NZERO 0x80000000u
#define PINF  0x7f800000u
#define NINF  0xff800000u
#define DNAN  0xffc00000u /* x86 default NaN (real indefinite) */
#define ANAN  0x7fc00000u /* ARM default NaN, a normal QNaN for x86 */
#define QN1   0x7fc00001u
#define QN2   0xffc00002u
#define SN1   0x7f800003u /* signaling, quiet form 0x7fc00003 */
#define SN2   0xff800004u /* signaling, quiet form 0xffc00004 */
#define DEN   0x00000010u /* denormal */
#define SMALL 0x00800000u /* FLT_MIN */
/* double bit patterns */
#define D1    0x3ff0000000000000ull
#define D2    0x4000000000000000ull
#define DHALF 0x3fe0000000000000ull
#define DZERO 0x0000000000000000ull
#define DINF  0x7ff0000000000000ull
#define DDNAN 0xfff8000000000000ull
#define DQN1  0x7ff8000000000001ull
#define DSN1  0x7ff0000000000003ull
#define DSMALL 0x0010000000000000ull

static void tests_default(void)
{
    /* default NaN: sign bit set on x86 */
    SS("divss 0/0", "divss", PZERO, PZERO, DNAN);
    SS("subss inf-inf", "subss", PINF, PINF, DNAN);
    SS("mulss inf*0", "mulss", PINF, PZERO, DNAN);
    SS("addss inf+-inf", "addss", PINF, NINF, DNAN);
    SD("divsd 0/0", "divsd", DZERO, DZERO, DDNAN);
    SD("mulsd inf*0", "mulsd", DINF, DZERO, DDNAN);
    V4("divps 0/0 lanes", "divps", L4(PZERO, ONE, PINF, PZERO), L4(PZERO, TWO, PINF, ONE), L4(DNAN, HALF, DNAN, PZERO));
    V4("subps inf-inf", "subps", L4(PINF, PINF, NINF, ONE), L4(PINF, ONE, NINF, ONE), L4(DNAN, PINF, DNAN, PZERO));
    V2("divpd 0/0", "divpd", L2(DZERO, D1), L2(DZERO, D2), L2(DDNAN, DHALF));
    V2("mulpd inf*0", "mulpd", L2(D2, DINF), L2(D1, DZERO), L2(D2, DDNAN));
    /* junk lanes like clang's cvtdq2ps + divps with lanes 2-3 = 0/0 (d3d11_headless) */
    V4("divps x/y 0/0 0/0", "divps", L4(0x40400000, ONE, PZERO, PZERO), L4(TWO, TWO, PZERO, PZERO), L4(0x3fc00000, HALF, DNAN, DNAN));

    /* NaN propagation: first source if NaN, else second, quieted */
    SS("addss qnan1+qnan2", "addss", QN1, QN2, QN1);
    SS("addss qnan2+qnan1", "addss", QN2, QN1, QN2);
    SS("addss 1+snan", "addss", ONE, SN1, 0x7fc00003);
    SS("addss snan+1", "addss", SN1, ONE, 0x7fc00003);
    SS("mulss qnan*snan", "mulss", QN1, SN1, QN1);
    SS("mulss snan*qnan", "mulss", SN1, QN1, 0x7fc00003);
    SS("subss -snan-snan", "subss", SN2, SN1, 0xffc00004);
    SS("divss 1/-qnan", "divss", ONE, QN2, QN2);
    SS("addss armnan+1", "addss", ANAN, ONE, ANAN);
    SS("subss 1-armnan", "subss", ONE, ANAN, ANAN);
    SS("mulss armnan*snan", "mulss", ANAN, SN2, ANAN);
    SD("addsd qnan+snan", "addsd", DQN1, DSN1, DQN1);
    SD("mulsd 1*snan", "mulsd", D1, DSN1, 0x7ff8000000000003ull);
    SD("subsd snan-qnan", "subsd", DSN1, DQN1, 0x7ff8000000000003ull);
    V4("mulps mixed NaNs", "mulps", L4(PINF, ONE, QN1, QN1), L4(PZERO, TWO, QN2, SN1), L4(DNAN, TWO, QN1, QN1));
    V4("addps snan, armnan, -0", "addps", L4(SN1, ONE, ANAN, NZERO), L4(QN1, SN2, SN1, NZERO), L4(0x7fc00003, 0xffc00004, ANAN, NZERO));
    V4("divps qnan/snan inf/inf", "divps", L4(PZERO, ONE, PINF, QN1), L4(PZERO, 0x40800000, PINF, SN1), L4(DNAN, 0x3e800000, DNAN, QN1));
    V2("addpd snan, qnan", "addpd", L2(DSN1, D1), L2(DQN1, DQN1), L2(0x7ff8000000000003ull, DQN1));
    /* NaN lanes next to -0, denormal and normal results */
    V4("mulps nan lane + -0, denormal", "mulps", L4(SMALL, PINF, NZERO, DEN), L4(HALF, PZERO, ONE, ONE), L4(0x00400000, DNAN, NZERO, DEN));
    V2("mulpd nan lane + denormal", "mulpd", L2(DSMALL, DINF), L2(DHALF, DZERO), L2(0x0008000000000000ull, DDNAN));

    /* min/max: second operand if either is NaN or both are zero */
    SS("minss 1,qnan", "minss", ONE, QN1, QN1);
    SS("minss qnan,1", "minss", QN1, ONE, ONE);
    SS("minss 1,snan", "minss", ONE, SN1, SN1);
    SS("maxss -0,+0", "maxss", NZERO, PZERO, PZERO);
    SS("maxss +0,-0", "maxss", PZERO, NZERO, NZERO);
    SS("minss +0,-0", "minss", PZERO, NZERO, NZERO);
    SS("minss -0,+0", "minss", NZERO, PZERO, PZERO);
    V4("maxps nan/zero lanes", "maxps", L4(QN1, ONE, NZERO, PZERO), L4(ONE, QN2, PZERO, NZERO), L4(ONE, QN2, PZERO, NZERO));
    SD("minsd 1,qnan", "minsd", D1, DQN1, DQN1);

    /* denormals without DAZ/FTZ */
    SS("addss den+den", "addss", DEN, DEN, 0x00000020);
    SS("mulss small*0.5", "mulss", SMALL, HALF, 0x00400000);
    SS("subss small-den", "subss", SMALL, DEN, 0x007ffff0);
    SD("mulsd small*0.5", "mulsd", DSMALL, DHALF, 0x0008000000000000ull);

    /* compares */
    CMP("comiss den > 0", "comiss", "seta", DEN, PZERO, 1);
    CMP("comiss den == 0", "comiss", "sete", DEN, PZERO, 0);
    CMP("ucomiss nan unordered", "ucomiss", "setp", QN1, ONE, 1);
    CMP("comiss -0 == +0", "comiss", "sete", NZERO, PZERO, 1);
    V4("cmpltps", "cmpltps", L4(ONE, QN1, DEN, NZERO), L4(TWO, ONE, PZERO, PZERO), L4(0xffffffff, 0, 0, 0));
    V4("cmpunordps", "cmpunordps", L4(QN1, ONE, PZERO, NINF), L4(ONE, SN1, PZERO, ONE), L4(0xffffffff, 0xffffffff, 0, 0));
    V4("cmpgtps den > 0", "cmpnleps", L4(DEN, DEN, DEN, DEN), L4(0, 0, 0, 0), L4(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff));

    /* float to int: NaN and out of range give 0x80000000 */
    CVTI("cvttss2si nan", "cvttss2si", QN1, 0x80000000);
    CVTI("cvtss2si nan", "cvtss2si", QN2, 0x80000000);
    CVTI("cvttss2si 3e9", "cvttss2si", 0x4f32d05e, 0x80000000);
    CVTI("cvtss2si 1.5", "cvtss2si", 0x3fc00000, 2);
    CVTI("cvtss2si 2.5", "cvtss2si", 0x40200000, 2);
    CVTQ("cvttsd2si nan", "cvttsd2si", DQN1, 0x8000000000000000ull);
    V4("cvttps2dq", "cvttps2dq", L4(0, 0, 0, 0), L4(QN1, 0x3fc00000, 0xc0200000, 0x4f32d05e), L4(0x80000000, 1, (uint32_t)-2, 0x80000000));
    V4("cvtps2dq", "cvtps2dq", L4(0, 0, 0, 0), L4(QN1, 0x3fc00000, 0xc0200000, 0x4f32d05e), L4(0x80000000, 2, (uint32_t)-2, 0x80000000));

    /* rcp/rsqrt: exact special values */
    SS("rcpss +0", "rcpss", ONE, PZERO, PINF);
    SS("rcpss -0", "rcpss", ONE, NZERO, NINF);
    SS("rcpss inf", "rcpss", ONE, PINF, PZERO);
    SS("rsqrtss -1", "rsqrtss", ONE, 0xbf800000, DNAN);
    SS("rsqrtss +0", "rsqrtss", ONE, PZERO, PINF);
    SS("rsqrtss qnan", "rsqrtss", ONE, QN2, QN2);
    SS("sqrtss -1", "sqrtss", ONE, 0xbf800000, DNAN);
    SD("sqrtsd -1", "sqrtsd", D1, 0xbff0000000000000ull, DDNAN);

    /* scalar ops keep the upper lanes of the destination */
    V4("addss upper lanes", "addss", L4(ONE, TWO, QN1, PZERO), L4(ONE, PZERO, PZERO, QN2), L4(TWO, TWO, QN1, PZERO));
    V4("divss 0/0 upper lanes", "divss", L4(PZERO, TWO, 0x40400000, DEN), L4(PZERO, PZERO, PZERO, PZERO), L4(DNAN, TWO, 0x40400000, DEN));
    V4("mulss qnan*snan upper lanes", "mulss", L4(QN1, ONE, NZERO, SN1), L4(SN1, PZERO, PZERO, PZERO), L4(QN1, ONE, NZERO, SN1));
    V4("sqrtss upper lanes", "sqrtss", L4(ONE, TWO, 0x40400000, 0x40800000), L4(0x41800000, 0, 0, 0), L4(0x40800000, TWO, 0x40400000, 0x40800000));
    V4("minss upper lanes", "minss", L4(ONE, TWO, QN1, NZERO), L4(QN2, 0, 0, 0), L4(QN2, TWO, QN1, NZERO));
    V2("subsd upper lane", "subsd", L2(D1, DQN1), L2(DINF, DZERO), L2(0xfff0000000000000ull, DQN1));
    V2("divsd 0/0 upper lane", "divsd", L2(DZERO, D2), L2(DZERO, DZERO), L2(DDNAN, D2));
    ITOF4("cvtsi2ss upper lanes", "cvtsi2ssq", L4(ONE, TWO, 0x40400000, QN1), 7, L4(0x40e00000, TWO, 0x40400000, QN1));
    ITOF2("cvtsi2sd upper lane", "cvtsi2sdq", L2(D1, DQN1), -7, L2(0xc01c000000000000ull, DQN1));
}

/* the MXCSR modes; ldmxcsr in its own block before every group */
static void tests_daz_ftz(void)
{
    const unsigned old = _mm_getcsr();

    _mm_setcsr(old | 0x8040); /* DAZ + FTZ */
    SS("DAZ+FTZ: den+den", "addss", DEN, DEN, PZERO);
    SS("DAZ+FTZ: 1+den", "addss", ONE, DEN, ONE);
    SS("DAZ+FTZ: small*0.5", "mulss", SMALL, HALF, PZERO);
    SS("DAZ+FTZ: 0/0", "divss", PZERO, PZERO, DNAN);
    SS("DAZ+FTZ: qnan*snan", "mulss", QN1, SN1, QN1);
    V4("DAZ+FTZ: mulps", "mulps", L4(SMALL, PINF, DEN, QN1), L4(HALF, PZERO, TWO, SN1), L4(PZERO, DNAN, PZERO, QN1));
    SD("DAZ+FTZ: small*0.5 double", "mulsd", DSMALL, DHALF, DZERO);
    CMP("DAZ+FTZ: comiss den == 0", "comiss", "sete", DEN, PZERO, 1);
    V4("DAZ+FTZ: cmpgtps den > 0", "cmpnleps", L4(DEN, DEN, DEN, DEN), L4(0, 0, 0, 0), L4(0, 0, 0, 0));
    SS("DAZ+FTZ: minss den,1", "minss", DEN, ONE, PZERO);

    _mm_setcsr(old | 0x0040); /* DAZ only */
    SS("DAZ: den+den", "addss", DEN, DEN, PZERO);
    SS("DAZ: small*0.5 not flushed", "mulss", SMALL, HALF, 0x00400000);
    SS("DAZ: 0/0", "divss", PZERO, PZERO, DNAN);
    SS("DAZ: qnan*snan", "mulss", QN1, SN1, QN1);
    V4("DAZ: mulps nan lane + denormal result", "mulps", L4(SMALL, PINF, DEN, QN1), L4(HALF, PZERO, TWO, SN1), L4(0x00400000, DNAN, PZERO, QN1));
    CMP("DAZ: comiss den == 0", "comiss", "sete", DEN, PZERO, 1);

    _mm_setcsr(old | 0x8000); /* FTZ only */
    SS("FTZ: den+small (input kept)", "addss", DEN, SMALL, 0x00800010);
    SS("FTZ: small*0.5", "mulss", SMALL, HALF, PZERO);
    SS("FTZ: 0/0", "divss", PZERO, PZERO, DNAN);
    SS("FTZ: qnan*snan", "mulss", QN1, SN1, QN1);
    V4("FTZ: mulps", "mulps", L4(SMALL, PINF, DEN, QN1), L4(HALF, PZERO, 0x4b000000, SN1), L4(PZERO, DNAN, 0x02800000, QN1));
    CMP("FTZ: comiss den > 0", "comiss", "seta", DEN, PZERO, 1);
    V4("FTZ: cmpgtps den > 0", "cmpnleps", L4(DEN, DEN, DEN, DEN), L4(0, 0, 0, 0), L4(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff));
    CVTI("FTZ: cvtss2si den", "cvtss2si", DEN, 0);

    _mm_setcsr(old);

    /* ldmxcsr and the float op in one block */
    RUN4({ int c_; float a_ = f32(DEN), z_ = f32(PZERO); unsigned ftz_ = old | 0x8000, old_ = old; BEFORE();
           __asm__ volatile("ldmxcsr %3\n xor %k0, %k0\n comiss %2, %1\n seta %b0\n ldmxcsr %4"
                            : "=&r"(c_) : "x"(a_), "x"(z_), "m"(ftz_), "m"(old_) : "cc", "memory");
           AFTER(); chk("ldmxcsr FTZ + comiss den > 0", M, (uint32_t)c_, 1); });
    RUN4({ float r_ = f32(DEN), y_ = f32(DEN); unsigned daz_ = old | 0x0040, old_ = old; BEFORE();
           __asm__ volatile("ldmxcsr %2\n addss %1, %0\n ldmxcsr %3" : "+x"(r_) : "x"(y_), "m"(daz_), "m"(old_) : "memory");
           AFTER(); chk("ldmxcsr DAZ + addss den+den", M, u32(r_), PZERO); });
    RUN4({ float r_ = f32(SMALL), y_ = f32(HALF); unsigned ftz_ = old | 0x8000, old_ = old; BEFORE();
           __asm__ volatile("ldmxcsr %2\n mulss %1, %0\n ldmxcsr %3" : "+x"(r_) : "x"(y_), "m"(ftz_), "m"(old_) : "memory");
           AFTER(); chk("ldmxcsr FTZ + mulss small*0.5", M, u32(r_), PZERO); });
    RUN4({ float r_ = f32(DEN), y_ = f32(SMALL); unsigned ftz_ = old | 0x8000, old_ = old; BEFORE();
           __asm__ volatile("ldmxcsr %2\n addss %1, %0\n ldmxcsr %3" : "+x"(r_) : "x"(y_), "m"(ftz_), "m"(old_) : "memory");
           AFTER(); chk("ldmxcsr FTZ + addss den+small", M, u32(r_), 0x00800010); });

    /* back to the default mode */
    SS("restored: den+den", "addss", DEN, DEN, 0x00000020);
    CMP("restored: comiss den > 0", "comiss", "seta", DEN, PZERO, 1);
}

/* rcpss/rsqrtss are approximations (x86: relative error <= 1.5 * 2^-12) */
static void tests_approx(void)
{
    RUN4({ float x_ = f32(0x40400000); BEFORE(); __asm__ volatile("rcpss %0, %0" : "+x"(x_) :: "memory"); AFTER();
           double e_ = (double)x_ * 3.0 - 1.0; chk("rcpss 3 within 1.5*2^-12", M, (e_ < 0 ? -e_ : e_) <= 1.5 / 4096, 1); });
    RUN4({ float x_ = f32(0x40400000); BEFORE(); __asm__ volatile("rsqrtss %0, %0" : "+x"(x_) :: "memory"); AFTER();
           double e_ = (double)x_ * (double)x_ * 3.0 - 1.0; chk("rsqrtss 3 within 3*2^-12", M, (e_ < 0 ? -e_ : e_) <= 3.0 / 4096, 1); });
}

/* ---- bench: ns per call into ARM64EC code after different x86 code ---- */

static LARGE_INTEGER qpf;
static double now_ns(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart * 1e9 / qpf.QuadPart; }
static volatile uint64_t sink;
static float buf[16] __attribute__((aligned(16)));
#define N 50000
#define BEST(fn) ({ double m_ = 1e9; for (int r_ = 0; r_ < 40; r_++) { double t_ = fn(); if (t_ < m_) m_ = t_; } m_; })

static double b_int(void) { double t = now_ns(); uint64_t s = 1; for (int i = 0; i < N; i++) { s = s * 3 + i; NATIVE(); } sink = s; return (now_ns() - t) / N; }
static double b_scalar(void)
{
    double t = now_ns(); float x = 1.0001f, y = 0.5f;
    for (int i = 0; i < N; i++) { __asm__ volatile("mulss %1, %0\n addss %1, %0" : "+x"(x) : "x"(y)); NATIVE(); }
    sink = (uint64_t)x; return (now_ns() - t) / N;
}
static double b_vector(void)
{
    double t = now_ns(); __m128 x = _mm_set1_ps(1.0001f), y = _mm_set1_ps(0.5f);
    for (int i = 0; i < N; i++) { __asm__ volatile("mulps %1, %0\n addps %1, %0" : "+x"(x) : "x"(y)); NATIVE(); }
    sink = (uint64_t)_mm_cvtss_f32(x); return (now_ns() - t) / N;
}
/* divps with two 0/0 lanes before every call (clang's code in d3d11_headless) */
static double b_nanlanes(void)
{
    double t = now_ns();
    for (int i = 0; i < N; i++)
    {
        __m128 a = _mm_setr_ps(1.f, 2.f, 0.f, 0.f), b = _mm_setr_ps(3.f, 3.f, 0.f, 0.f);
        __asm__ volatile("divps %1, %0\n movaps %0, (%2)" : "+x"(a) : "x"(b), "r"(buf) : "memory");
        NATIVE();
    }
    return (now_ns() - t) / N;
}
static double b_double(void)
{
    double t = now_ns(); double x = 1.0001, y = 0.5;
    for (int i = 0; i < N; i++) { __asm__ volatile("mulsd %1, %0\n addsd %1, %0" : "+x"(x) : "x"(y)); NATIVE(); }
    sink = (uint64_t)x; return (now_ns() - t) / N;
}
static double b_compare(void)
{
    double t = now_ns(); float x = 1.0001f, y = 0.5f; int c = 0;
    for (int i = 0; i < N; i++) { int r; __asm__ volatile("xor %0, %0\n comiss %2, %1\n seta %b0" : "=&r"(r) : "x"(x), "x"(y) : "cc"); c += r; NATIVE(); }
    sink = c; return (now_ns() - t) / N;
}
static double b_move(void)
{
    double t = now_ns();
    for (int i = 0; i < N; i++)
    {
        __asm__ volatile("movaps (%0), %%xmm0\n xorps %%xmm1, %%xmm1\n movaps %%xmm0, 16(%0)\n movaps %%xmm1, 32(%0)" :: "r"(buf) : "xmm0", "xmm1", "memory");
        NATIVE();
    }
    return (now_ns() - t) / N;
}
static double b_sqrt(void)
{
    double t = now_ns(); float x = 2.f;
    for (int i = 0; i < N; i++) { __asm__ volatile("sqrtss %0, %0" : "+x"(x)); NATIVE(); }
    sink = (uint64_t)x; return (now_ns() - t) / N;
}

int main(int argc, char **argv)
{
    int bench = 0;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        if (!strcmp(argv[i], "bench")) bench = 1;
    }
    QueryPerformanceFrequency(&qpf);
    if (bench)
        printf("{\"call_int_ns\":%.2f,\"call_scalar_ns\":%.2f,\"call_vector_ns\":%.2f,\"call_nanlanes_ns\":%.2f,"
               "\"call_double_ns\":%.2f,\"call_compare_ns\":%.2f,\"call_move_ns\":%.2f,\"call_sqrt_ns\":%.2f}\n",
               BEST(b_int), BEST(b_scalar), BEST(b_vector), BEST(b_nanlanes), BEST(b_double), BEST(b_compare), BEST(b_move), BEST(b_sqrt));
    tests_default();
    tests_daz_ftz();
    tests_approx();
    printf("fex_float: %d/%d passed\n", checks - fails, checks);
    return fails != 0;
}
