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

#include "Dec.h"

#include "Wide.h"

#include <cmath>
#include <cstdlib>

// gcc and clang have a 128-bit type; MSVC does not and never has. Defining
// FAKTURO_NO_INT128 forces the portable path on a compiler that *does* have
// one, which is how the two are checked against each other — see Wide.h and
// scripts/check-portable-math.sh. They must agree on every input.
#if defined(__SIZEOF_INT128__) && !defined(FAKTURO_NO_INT128)
#define FAKTURO_HAS_INT128 1
#endif

namespace fk {
namespace {

int64_t pow10(int n) {
    int64_t r = 1;
    for (int i = 0; i < n; ++i) r *= 10;
    return r;
}

/// |a × b| ÷ d, rounded half away from zero, signed by the two factors.
///
/// Every 128-bit intermediate in this class is of that one shape — a product
/// of two int64 divided by a positive int64 — which is why it is the only wide
/// operation the portable path has to provide.
int64_t mulDivRound(int64_t a, int64_t b, int64_t d) {
    const bool neg = (a < 0) != (b < 0);

    // Negated as unsigned, so the most negative int64 can reach its own
    // magnitude. `-(-2^63)` is undefined behaviour; this is not.
    const uint64_t ua = a < 0 ? ~static_cast<uint64_t>(a) + 1 : static_cast<uint64_t>(a);
    const uint64_t ub = b < 0 ? ~static_cast<uint64_t>(b) + 1 : static_cast<uint64_t>(b);
    const uint64_t ud = static_cast<uint64_t>(d);

#ifdef FAKTURO_HAS_INT128
    const unsigned __int128 num = static_cast<unsigned __int128>(ua) * ub;
    const uint64_t q = static_cast<uint64_t>((num + ud / 2) / ud);
#else
    uint64_t rest = 0;
    const uint64_t q = wide::divmod(wide::add64(wide::mul64(ua, ub), ud / 2),
                                    ud, rest).lo;
#endif

    return neg ? -static_cast<int64_t>(q) : static_cast<int64_t>(q);
}

/// The same, where the numerator is already only 64 bits wide.
int64_t divRound(int64_t num, int64_t den) {
    return mulDivRound(num, 1, den);
}

} // namespace

Dec Dec::fromDouble(double x) {
    double scaled = x * static_cast<double>(SCALE);
    return fromRaw(static_cast<int64_t>(scaled < 0 ? scaled - 0.5 : scaled + 0.5));
}

Dec Dec::operator*(Dec o) const {
    return fromRaw(mulDivRound(v_, o.v_, SCALE));
}

Dec Dec::operator/(Dec o) const {
    if (o.v_ == 0) return Dec();
    // v_ × SCALE ÷ o.v_, with the divisor's sign folded into the numerator
    // because the wide helper wants a positive divisor.
    const bool negDen = o.v_ < 0;
    return fromRaw(mulDivRound(negDen ? -v_ : v_, SCALE, negDen ? -o.v_ : o.v_));
}

Dec Dec::roundTo(int dp) const {
    if (dp >= DP) return *this;
    if (dp < 0) dp = 0;
    int64_t step = pow10(DP - dp);
    return fromRaw(divRound(v_, step) * step);
}

Dec Dec::percentOf(Dec pct) const {
    return (*this * pct / Dec::fromInt(100)).roundTo(2);
}

std::optional<Dec> Dec::parse(const std::string& s) {
    std::string t;
    t.reserve(s.size());
    for (char c : s) {
        if (c == ' ' || c == '\'' || c == '\xa0') continue;   // thousands separators
        t.push_back(c == ',' ? '.' : c);
    }
    if (t.empty()) return Dec();

    size_t i = 0;
    bool neg = false;
    if (t[i] == '+' || t[i] == '-') { neg = (t[i] == '-'); ++i; }
    if (i >= t.size()) return std::nullopt;

    int64_t intPart = 0;
    bool sawDigit = false;
    for (; i < t.size() && t[i] != '.'; ++i) {
        if (t[i] < '0' || t[i] > '9') return std::nullopt;
        intPart = intPart * 10 + (t[i] - '0');
        sawDigit = true;
        if (intPart > 9000000000LL) return std::nullopt;      // overflow guard
    }
    int64_t frac = 0, fracScale = SCALE;
    if (i < t.size() && t[i] == '.') {
        ++i;
        for (int d = 0; i < t.size(); ++i, ++d) {
            if (t[i] < '0' || t[i] > '9') return std::nullopt;
            sawDigit = true;
            if (d < DP) { fracScale /= 10; frac += (t[i] - '0') * fracScale; }
        }
    }
    if (!sawDigit) return std::nullopt;
    int64_t v = intPart * SCALE + frac;
    return fromRaw(neg ? -v : v);
}

std::string Dec::toString(int dp) const {
    if (dp < 0) dp = 0;
    if (dp > DP) dp = DP;
    int64_t r    = roundTo(dp).v_;
    bool    neg  = r < 0;
    if (neg) r = -r;
    int64_t step = pow10(DP - dp);
    int64_t units = r / SCALE;
    int64_t rest  = (r % SCALE) / step;

    std::string out = neg && (units != 0 || rest != 0) ? "-" : "";
    out += std::to_string(units);
    if (dp > 0) {
        std::string f = std::to_string(rest);
        out += '.';
        out.append(static_cast<size_t>(dp) - f.size(), '0');
        out += f;
    }
    return out;
}

} // namespace fk
