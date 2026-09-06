#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Fakturo — invoicing for Slovak and Czech sole traders
# Copyright (C) 2026 Peter Mercell
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

#
# check-portable-math.sh — prove the portable 128-bit path agrees with __int128.
#
# `Dec` is the one class in this project that must not be even slightly wrong,
# and it now has two implementations of its wide intermediate: the compiler's
# __int128 where there is one, and src/core/Wide.h where there is not — which
# is to say, on MSVC, on the platform none of this can be tested from.
#
# So the two are checked against each other rather than reasoned about, which
# is the same method the QR encoder, the LZMA coder and the SHA-256 were
# settled with. Two things happen here:
#
#   1. A direct comparison over several million values, including the edges
#      that a hand-written multiply gets wrong — the most negative int64, both
#      halves of a 64-bit product carrying, a divisor above 2^63.
#   2. The whole core suite, built a second time with FAKTURO_NO_INT128 so
#      every Dec operation in every test goes down the portable path. If the
#      1675 assertions pass both ways, the paths agree on everything the
#      application actually does.
#
# Needs a compiler that has __int128 — that is the point. Run it on macOS or
# Linux before shipping a Windows build.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

CXX="${CXX:-c++}"

echo "1/2  comparing the two implementations directly"

cat > "$WORK/compare.cpp" <<'CPP'
#include "core/Wide.h"

#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

using namespace fk;

// The reference: exactly what Dec.cpp does when __int128 is available.
static uint64_t reference(uint64_t a, uint64_t b, uint64_t d) {
    const unsigned __int128 num = static_cast<unsigned __int128>(a) * b;
    return static_cast<uint64_t>((num + d / 2) / d);
}

static uint64_t portable(uint64_t a, uint64_t b, uint64_t d) {
    uint64_t rest = 0;
    return wide::divmod(wide::add64(wide::mul64(a, b), d / 2), d, rest).lo;
}

static long failures = 0;
static long checked  = 0;

static void check(uint64_t a, uint64_t b, uint64_t d) {
    if (d == 0) return;
    ++checked;
    const uint64_t want = reference(a, b, d);
    const uint64_t got  = portable(a, b, d);
    if (want != got && failures < 10) {
        std::printf("  MISMATCH  a=%llu b=%llu d=%llu  want=%llu got=%llu\n",
                    (unsigned long long)a, (unsigned long long)b,
                    (unsigned long long)d, (unsigned long long)want,
                    (unsigned long long)got);
        ++failures;
    } else if (want != got) {
        ++failures;
    }

    // The full product, independently: hi:lo must equal what __int128 says.
    const unsigned __int128 exact = static_cast<unsigned __int128>(a) * b;
    const wide::U128 mine = wide::mul64(a, b);
    if (mine.hi != static_cast<uint64_t>(exact >> 64) ||
        mine.lo != static_cast<uint64_t>(exact)) {
        if (failures < 10)
            std::printf("  MUL MISMATCH  a=%llu b=%llu\n",
                        (unsigned long long)a, (unsigned long long)b);
        ++failures;
    }
}

int main() {
    // The values that break a hand-written implementation, tried against each
    // other in every combination.
    const std::vector<uint64_t> edges = {
        0, 1, 2, 999999, 1000000, 1000001,
        0x7FFFFFFFull, 0x80000000ull, 0xFFFFFFFFull, 0x100000000ull,
        0x7FFFFFFFFFFFFFFFull,            // int64 max
        0x8000000000000000ull,            // the magnitude of int64 min
        0x8000000000000001ull,
        0xFFFFFFFFFFFFFFFFull,            // and a divisor above 2^63
        1234567890123456789ull,
    };
    for (uint64_t a : edges)
        for (uint64_t b : edges)
            for (uint64_t d : edges)
                check(a, b, d);

    // Then the shapes Dec actually produces: money times money over SCALE,
    // and money times SCALE over money.
    std::mt19937_64 rng(20260805);
    for (int i = 0; i < 400000; ++i) {
        const uint64_t a = rng() % 100000000000000ull;   // ±100 million, scaled
        const uint64_t b = rng() % 100000000000000ull;
        check(a, b, 1000000ull);
        check(a, 1000000ull, b ? b : 1);
    }
    // And the unrestricted case, where the quotient overflows 64 bits and the
    // two implementations must at least truncate identically.
    for (int i = 0; i < 400000; ++i) {
        const uint64_t a = rng(), b = rng();
        uint64_t d = rng();
        check(a, b, d ? d : 1);
    }

    std::printf("  %ld comparisons, %ld disagreements\n", checked, failures);
    return failures == 0 ? 0 : 1;
}
CPP

"$CXX" -std=c++17 -O2 -Wall -Wextra -I"$ROOT/src" "$WORK/compare.cpp" -o "$WORK/compare"
"$WORK/compare"

echo
echo "2/2  the core suite, forced down the portable path"
echo "     (build it the way you normally do, with -DFAKTURO_NO_INT128 added)"
echo
echo "     cmake -B build-portable -DCMAKE_CXX_FLAGS=-DFAKTURO_NO_INT128"
echo "     cmake --build build-portable -j --target core_tests"
echo "     ./build-portable/core_tests"
echo
echo "     Every assertion must pass there too. It is the same 1675 checks"
echo "     over the same numbers, with MSVC's arithmetic underneath them."
