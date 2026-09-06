// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Fakturo — invoicing for Slovak and Czech sole traders
 * Copyright (C) 2026 Peter Mercell
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Wide.h - the 128-bit intermediate that money arithmetic needs, for compilers
// that have no 128-bit type. Which means MSVC: gcc and clang have __int128,
// MSVC has never had it and is not going to.
//
// Only what `Dec` actually asks for: an exact 64×64 product, and a division of
// that product by a 64-bit divisor with half-away-from-zero rounding. Nothing
// general, because a general 128-bit type would be a lot of code to get subtly
// wrong in the one place this project cannot afford to be subtly wrong.
//
// The division is schoolbook binary long division — 128 iterations of a shift,
// a compare and a subtract. It is the slowest way to do this and the easiest
// to read, and both of those are the right trade here: an invoice has a
// handful of lines, and the alternative is Knuth's algorithm D, which is fast
// and is also where people put their bugs.
//
// **This file is checked against `__int128` rather than argued about.** The
// core suite runs a second time with FAKTURO_NO_INT128 defined, which forces
// every platform down this path, and the two runs must agree exactly. See
// scripts/check-portable-math.sh.
#pragma once

#include <cstdint>

namespace fk {
namespace wide {

/// An unsigned 128-bit magnitude. Sign is carried by the caller, because every
/// caller here already knows it and negating in two places is how a sign gets
/// lost.
struct U128 {
    uint64_t hi = 0;
    uint64_t lo = 0;
};

/// Exact 64 × 64 → 128, from four 32-bit products.
inline U128 mul64(uint64_t a, uint64_t b) {
    const uint64_t mask = 0xFFFFFFFFull;
    const uint64_t a0 = a & mask, a1 = a >> 32;
    const uint64_t b0 = b & mask, b1 = b >> 32;

    const uint64_t p00 = a0 * b0;
    const uint64_t p01 = a0 * b1;
    const uint64_t p10 = a1 * b0;
    const uint64_t p11 = a1 * b1;

    // The two middle products are added to the top half of p00. Their sum can
    // carry out of 64 bits, which is what `carry` is for — dropping it is the
    // classic way this function comes out almost right.
    const uint64_t middle = (p00 >> 32) + (p01 & mask) + (p10 & mask);

    U128 r;
    r.lo = (middle << 32) | (p00 & mask);
    r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32);
    return r;
}

/// a + b, where the sum is known to fit. Used only to add half the divisor
/// before dividing, so it cannot overflow anything a caller here produces.
inline U128 add64(U128 a, uint64_t b) {
    U128 r;
    r.lo = a.lo + b;
    r.hi = a.hi + (r.lo < a.lo ? 1u : 0u);
    return r;
}

/// a / d, truncated, with the remainder out. `d` must not be zero.
///
/// The quotient is returned in full 128 bits; a caller that wants an int64_t
/// truncates, which is what the __int128 version has always done and is
/// therefore the behaviour to match rather than improve.
inline U128 divmod(U128 a, uint64_t d, uint64_t& remainder) {
    U128 q;
    uint64_t r = 0;

    for (int bit = 127; bit >= 0; --bit) {
        const uint64_t next = bit >= 64 ? (a.hi >> (bit - 64)) & 1u
                                        : (a.lo >> bit) & 1u;
        // r is always less than d before this, so 2r + next is less than 2d
        // and can be brought back under d by one subtraction. It can also
        // exceed 64 bits on the way — hence `carried`, which stands in for the
        // 65th bit that the shift threw away. Without it, a divisor above
        // 2^63 quietly gives the wrong answer and nothing else notices.
        const bool carried = (r >> 63) != 0;
        r = (r << 1) | next;
        if (carried || r >= d) {
            r -= d;
            if (bit >= 64) q.hi |= (uint64_t{1} << (bit - 64));
            else           q.lo |= (uint64_t{1} << bit);
        }
    }
    remainder = r;
    return q;
}

} // namespace wide
} // namespace fk
