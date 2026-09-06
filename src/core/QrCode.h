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

// QrCode.h - QR Code encoder (ISO/IEC 18004), byte mode.
//
// Written here rather than pulled in as a dependency: the project has no
// third-party runtime dependencies beyond Qt, SQLite and zlib, and a QR
// encoder is small and completely specified. Correctness is checked against a
// reference implementation in the tests.
//
// Byte mode only, which is all the payment formats need — PAY by square is
// base32 text and SPAYD is ASCII.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fk {

class QrCode {
public:
    /// Error correction level. Payment QR codes conventionally use M.
    enum class Ecc { Low, Medium, Quartile, High };

    /// Smallest version that fits. Returns a QrCode with size() == 0 when the
    /// text does not fit in any version at this level.
    static QrCode encode(const std::string& text, Ecc ecc = Ecc::Medium);

    /// Same, with the mask fixed instead of chosen by the penalty rules. Only
    /// for tests: it lets the encoding be compared against a reference without
    /// the mask choice getting in the way.
    static QrCode encodeWithMask(const std::string& text, Ecc ecc, int mask);
    int mask() const { return mask_; }

    /// The final interleaved codeword stream. Exposed so tests can compare the
    /// encoding itself against a reference, separately from the placement.
    static std::vector<uint8_t> codewordsFor(const std::string& text, Ecc ecc);

    int  size() const { return size_; }
    int  version() const { return version_; }
    bool moduleAt(int x, int y) const;

    /// Rows of '1' and '0', one line per row. Handy for tests and debugging.
    std::vector<std::string> rows() const;

    /// Shared by both entry points; forcedMask < 0 means "choose".
    static QrCode build(const std::string& text, Ecc ecc, int forcedMask);

private:
    QrCode() = default;

    void drawFunctionPatterns();
    void drawFormatBits(int mask);
    void drawVersion();
    void drawFinder(int x, int y);
    void drawAlignment(int x, int y);
    void drawCodewords(const std::vector<uint8_t>& data);
    void applyMask(int mask);
    long penaltyScore() const;
    int  finderPenaltyCountPatterns(const int history[7]) const;
    int  finderPenaltyTerminate(bool runColor, int runLength, int history[7]) const;
    void finderPenaltyAddHistory(int runLength, int history[7]) const;
    void setModule(int x, int y, bool dark, bool function);

    std::vector<uint8_t> addEccAndInterleave(const std::vector<uint8_t>& data) const;
    std::vector<int>     alignmentPositions() const;

    int  version_ = 1;
    int  mask_    = 0;
    int  size_    = 0;
    Ecc  ecc_     = Ecc::Medium;
    std::vector<uint8_t> modules_;    // 1 = dark
    std::vector<uint8_t> isFunction_;
    std::vector<uint8_t> lastCodewords_;
};

} // namespace fk
