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

// Dec.h - fixed-point decimal (6 fractional digits) for money and quantities.
// Never use double for money. All arithmetic is exact until an explicit round.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace fk {

class Dec {
public:
    static constexpr int64_t SCALE = 1000000;   // 6 decimal places
    static constexpr int      DP    = 6;

    constexpr Dec() = default;

    static constexpr Dec fromRaw(int64_t r) { Dec d; d.v_ = r; return d; }
    static constexpr Dec fromInt(int64_t n) { return fromRaw(n * SCALE); }
    static Dec fromDouble(double x);
    /// Accepts "1234.56", "1 234,56", "-12", "". Returns nullopt on garbage.
    static std::optional<Dec> parse(const std::string& s);

    int64_t raw() const { return v_; }
    double  toDouble() const { return static_cast<double>(v_) / SCALE; }
    /// Plain decimal string with exactly `dp` fractional digits, '.' separator.
    std::string toString(int dp = 2) const;

    Dec operator+(Dec o) const { return fromRaw(v_ + o.v_); }
    Dec operator-(Dec o) const { return fromRaw(v_ - o.v_); }
    Dec operator-()      const { return fromRaw(-v_); }
    Dec operator*(Dec o) const;
    Dec operator/(Dec o) const;
    Dec& operator+=(Dec o) { v_ += o.v_; return *this; }
    Dec& operator-=(Dec o) { v_ -= o.v_; return *this; }

    bool operator==(Dec o) const { return v_ == o.v_; }
    bool operator!=(Dec o) const { return v_ != o.v_; }
    bool operator< (Dec o) const { return v_ <  o.v_; }
    bool operator<=(Dec o) const { return v_ <= o.v_; }
    bool operator> (Dec o) const { return v_ >  o.v_; }
    bool operator>=(Dec o) const { return v_ >= o.v_; }

    bool isZero()     const { return v_ == 0; }
    bool isNegative() const { return v_ < 0; }
    Dec  abs()        const { return fromRaw(v_ < 0 ? -v_ : v_); }

    /// Half-away-from-zero rounding to `dp` decimal places.
    Dec roundTo(int dp) const;
    /// this * (pct / 100), rounded to 2 dp. Used for VAT.
    Dec percentOf(Dec pct) const;

private:
    int64_t v_ = 0;
};

inline Dec operator""_d(unsigned long long n) { return Dec::fromInt(static_cast<int64_t>(n)); }

} // namespace fk
