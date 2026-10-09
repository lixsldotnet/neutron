#!/usr/bin/env python3
"""Generates bigcode.c: a large x86_64 program that runs a lot of code once.

Each of N functions has its own mix of integer, float and branch code, so FEX
has to translate every one of them; main calls them all once through a table and
prints the time. Used to measure JIT cost on a first start against a later one
(FEX code cache) and the cost of a game's first frames.

    python3 dev/bench/bigcode.py [functions, default 20000] > bigcode.c
    x86_64-w64-mingw32-clang -O1 -o bigcode.exe bigcode.c
    tool/neutron runinprefix bigcode.exe [passes, default 1]

Output: {"bench":"bigcode","functions":N,"first_pass_ms":..,"second_pass_ms":..,"checksum":..}
The second pass runs the already translated code (in-process cache).
"""
import random
import sys

n = int(sys.argv[1]) if len(sys.argv) > 1 else 20000
rnd = random.Random(1234)

ops_int = ["a = a * {k} + b;", "b ^= a >> {s};", "a += (b << {s}) | {k};", "b = b * {k} - a;",
           "if (a & {m}) a -= b; else b += {k};", "a = (a >> {s}) ^ (b * {k});",
           "for (int i = 0; i < {s}; i++) a += b ^ i;", "switch (a & 7) {{ case 0: b += {k}; break; case 1: b ^= {k}; break; "
           "case 2: a -= {k}; break; case 3: a ^= b; break; default: b = a + {k}; }}",
           "b = (b < a) ? b + {k} : a - {k};"]
ops_flt = ["f = f * {fk} + (double)a;", "g = g / ({fk} + f * f);", "f += g * {fk};", "a += (long long)(f * {fk});",
           "g = (g > f) ? g - {fk} : f + {fk};"]

out = ["#include <windows.h>", "#include <stdio.h>", "#include <stdlib.h>", "typedef long long (*fn_t)(long long, long long);"]
for i in range(n):
    body = []
    for _ in range(rnd.randint(6, 14)):
        if rnd.random() < 0.3:
            body.append(rnd.choice(ops_flt).format(fk=round(rnd.uniform(0.5, 3.0), 3)))
        else:
            body.append(rnd.choice(ops_int).format(k=rnd.randint(3, 99991), s=rnd.randint(1, 13),
                                                   m=rnd.randint(1, 255)))
    out.append("__attribute__((noinline)) long long f%d(long long a, long long b) {\n"
               "    double f = (double)b, g = 1.5;\n    %s\n    return a + b + (long long)(f + g);\n}"
               % (i, "\n    ".join(body)))
out.append("static const fn_t table[] = {" + ",".join("f%d" % i for i in range(n)) + "};")
out.append(r'''
int main(int argc, char **argv)
{
    LARGE_INTEGER f, t0, t1, t2;
    long long sum = 0, i;
    int passes = argc > 1 ? atoi(argv[1]) : 1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (i = 0; i < (long long)(sizeof(table) / sizeof(table[0])); i++) sum += table[i](i, sum);
    QueryPerformanceCounter(&t1);
    for (int p = 1; p < passes; p++)
        for (i = 0; i < (long long)(sizeof(table) / sizeof(table[0])); i++) sum += table[i](i, sum);
    QueryPerformanceCounter(&t2);
    printf("{\"bench\":\"bigcode\",\"functions\":%d,\"first_pass_ms\":%.1f,\"second_pass_ms\":%.1f,\"checksum\":%lld}\n",
           (int)(sizeof(table) / sizeof(table[0])), (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart,
           passes > 1 ? (t2.QuadPart - t1.QuadPart) * 1000.0 / f.QuadPart / (passes - 1) : 0.0, sum);
    return 0;
}''')
print("\n".join(out))
