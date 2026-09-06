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

#include "QrCode.h"

#include <algorithm>
#include <cstdlib>

namespace fk {
namespace {

// Table 13 of ISO/IEC 18004: error-correction codewords per block, and the
// number of blocks, indexed by [ecc][version].
const int8_t ECC_CODEWORDS_PER_BLOCK[4][41] = {
    // 0 is unused; versions run 1..40.
    {-1,  7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28,
      28, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},  // L
    {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26,
      26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28},  // M
    {-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26,
      30, 28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},  // Q
    {-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26,
      28, 30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},  // H
};

const int8_t NUM_ERROR_CORRECTION_BLOCKS[4][41] = {
    {-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4,  4,  4,  4,  4,  6,  6,  6,  6,  7,
      8, 8, 9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25},   // L
    {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5,  5,  8,  9,  9, 10, 10, 11, 13, 14,
     16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},  // M
    {-1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8,  8, 10, 12, 16, 12, 17, 16, 18, 21,
     20, 23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68},  // Q
    {-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25,
     25, 25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81},  // H
};

int eccIndex(QrCode::Ecc ecc) { return static_cast<int>(ecc); }

/// Format-information bit pattern order: L=01, M=00, Q=11, H=10.
int eccFormatBits(QrCode::Ecc ecc) {
    switch (ecc) {
        case QrCode::Ecc::Low:      return 1;
        case QrCode::Ecc::Medium:   return 0;
        case QrCode::Ecc::Quartile: return 3;
        case QrCode::Ecc::High:     return 2;
    }
    return 0;
}

int numRawDataModules(int version) {
    int result = (16 * version + 128) * version + 64;
    if (version >= 2) {
        const int numAlign = version / 7 + 2;
        result -= (25 * numAlign - 10) * numAlign - 55;
        if (version >= 7) result -= 36;
    }
    return result;
}

int numDataCodewords(int version, QrCode::Ecc ecc) {
    const int e = eccIndex(ecc);
    return numRawDataModules(version) / 8
         - ECC_CODEWORDS_PER_BLOCK[e][version] * NUM_ERROR_CORRECTION_BLOCKS[e][version];
}

// ------------------------------------------------------- GF(256) arithmetic
uint8_t gfMultiply(uint8_t x, uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return static_cast<uint8_t>(z);
}

std::vector<uint8_t> reedSolomonDivisor(int degree) {
    std::vector<uint8_t> result(static_cast<size_t>(degree), 0);
    result.back() = 1;

    uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (size_t j = 0; j < result.size(); ++j) {
            result[j] = gfMultiply(result[j], root);
            if (j + 1 < result.size()) result[j] ^= result[j + 1];
        }
        root = gfMultiply(root, 0x02);
    }
    return result;
}

std::vector<uint8_t> reedSolomonRemainder(const std::vector<uint8_t>& data,
                                          const std::vector<uint8_t>& divisor) {
    std::vector<uint8_t> result(divisor.size(), 0);
    for (uint8_t b : data) {
        const uint8_t factor = b ^ result.front();
        result.erase(result.begin());
        result.push_back(0);
        for (size_t i = 0; i < divisor.size(); ++i)
            result[i] ^= gfMultiply(divisor[i], factor);
    }
    return result;
}

} // namespace

// --------------------------------------------------------------- encoding
QrCode QrCode::encode(const std::string& text, Ecc ecc) { return build(text, ecc, -1); }

QrCode QrCode::encodeWithMask(const std::string& text, Ecc ecc, int mask) {
    return build(text, ecc, mask);
}

std::vector<uint8_t> QrCode::codewordsFor(const std::string& text, Ecc ecc) {
    const QrCode qr = build(text, ecc, 0);
    return qr.lastCodewords_;
}

QrCode QrCode::build(const std::string& text, Ecc ecc, int forcedMask) {
    QrCode qr;
    qr.ecc_ = ecc;

    // Byte mode: 4 bits mode indicator, then the length, then the data.
    // The character-count field is 8 bits up to version 9 and 16 beyond.
    int version = 0;
    for (int v = 1; v <= 40; ++v) {
        const int capacityBits = numDataCodewords(v, ecc) * 8;
        const int lengthBits   = (v <= 9) ? 8 : 16;
        if (4 + lengthBits + static_cast<int>(text.size()) * 8 <= capacityBits) {
            version = v;
            break;
        }
    }
    if (version == 0) return qr;              // does not fit at any version

    qr.version_ = version;
    qr.size_    = version * 4 + 17;
    qr.modules_.assign(static_cast<size_t>(qr.size_ * qr.size_), 0);
    qr.isFunction_.assign(static_cast<size_t>(qr.size_ * qr.size_), 0);

    std::vector<bool> bits;
    auto appendBits = [&bits](uint32_t value, int length) {
        for (int i = length - 1; i >= 0; --i) bits.push_back(((value >> i) & 1) != 0);
    };

    appendBits(0x4, 4);                                        // byte mode
    appendBits(static_cast<uint32_t>(text.size()), version <= 9 ? 8 : 16);
    for (unsigned char c : text) appendBits(c, 8);

    const int capacityBits = numDataCodewords(version, ecc) * 8;
    appendBits(0, std::min(4, capacityBits - static_cast<int>(bits.size())));   // terminator
    appendBits(0, (8 - static_cast<int>(bits.size() % 8)) % 8);                 // byte align

    // Pad alternately with 0xEC and 0x11 until full.
    for (uint8_t pad = 0xEC; static_cast<int>(bits.size()) < capacityBits; pad ^= 0xEC ^ 0x11)
        appendBits(pad, 8);

    std::vector<uint8_t> dataCodewords(bits.size() / 8, 0);
    for (size_t i = 0; i < bits.size(); ++i)
        if (bits[i]) dataCodewords[i >> 3] |= static_cast<uint8_t>(1 << (7 - (i & 7)));

    qr.drawFunctionPatterns();
    qr.lastCodewords_ = qr.addEccAndInterleave(dataCodewords);
    qr.drawCodewords(qr.lastCodewords_);

    // Try every mask, keep the one the standard's penalty rules like best.
    int bestMask = forcedMask;
    if (bestMask < 0) {
        long bestScore = -1;
        for (int mask = 0; mask < 8; ++mask) {
            qr.applyMask(mask);
            qr.drawFormatBits(mask);
            const long score = qr.penaltyScore();
            if (bestScore < 0 || score < bestScore) { bestScore = score; bestMask = mask; }
            qr.applyMask(mask);                    // XOR again to undo
        }
    }
    qr.applyMask(bestMask);
    qr.drawFormatBits(bestMask);
    qr.mask_ = bestMask;
    return qr;
}

std::vector<uint8_t> QrCode::addEccAndInterleave(const std::vector<uint8_t>& data) const {
    const int e         = eccIndex(ecc_);
    const int numBlocks = NUM_ERROR_CORRECTION_BLOCKS[e][version_];
    const int blockEcc  = ECC_CODEWORDS_PER_BLOCK[e][version_];
    const int rawCodewords   = numRawDataModules(version_) / 8;
    const int numShortBlocks = numBlocks - rawCodewords % numBlocks;
    const int shortBlockLen  = rawCodewords / numBlocks;

    const std::vector<uint8_t> divisor = reedSolomonDivisor(blockEcc);

    std::vector<std::vector<uint8_t>> blocks;
    for (int i = 0, k = 0; i < numBlocks; ++i) {
        const int len = shortBlockLen - blockEcc + (i < numShortBlocks ? 0 : 1);
        std::vector<uint8_t> block(data.begin() + k, data.begin() + k + len);
        k += len;
        const std::vector<uint8_t> ecc = reedSolomonRemainder(block, divisor);
        if (i < numShortBlocks) block.push_back(0);            // placeholder, skipped below
        block.insert(block.end(), ecc.begin(), ecc.end());
        blocks.push_back(std::move(block));
    }

    // Interleave: take one codeword from each block in turn.
    std::vector<uint8_t> result;
    for (size_t i = 0; i < blocks.front().size(); ++i)
        for (size_t j = 0; j < blocks.size(); ++j)
            if (i != static_cast<size_t>(shortBlockLen - blockEcc) ||
                j >= static_cast<size_t>(numShortBlocks))
                result.push_back(blocks[j][i]);
    return result;
}

// ---------------------------------------------------------------- drawing
void QrCode::setModule(int x, int y, bool dark, bool function) {
    modules_[static_cast<size_t>(y * size_ + x)]    = dark ? 1 : 0;
    if (function) isFunction_[static_cast<size_t>(y * size_ + x)] = 1;
}

bool QrCode::moduleAt(int x, int y) const {
    if (x < 0 || y < 0 || x >= size_ || y >= size_) return false;
    return modules_[static_cast<size_t>(y * size_ + x)] != 0;
}

void QrCode::drawFinder(int x, int y) {
    for (int dy = -4; dy <= 4; ++dy)
        for (int dx = -4; dx <= 4; ++dx) {
            const int dist = std::max(std::abs(dx), std::abs(dy));
            const int px = x + dx, py = y + dy;
            if (px < 0 || py < 0 || px >= size_ || py >= size_) continue;
            setModule(px, py, dist != 2 && dist != 4, true);
        }
}

void QrCode::drawAlignment(int x, int y) {
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
            setModule(x + dx, y + dy, std::max(std::abs(dx), std::abs(dy)) != 1, true);
}

std::vector<int> QrCode::alignmentPositions() const {
    if (version_ == 1) return {};
    const int numAlign = version_ / 7 + 2;
    const int step = (version_ * 8 + numAlign * 3 + 5) / (numAlign * 4 - 4) * 2;

    std::vector<int> result{6};
    for (int pos = size_ - 7; result.size() < static_cast<size_t>(numAlign); pos -= step)
        result.insert(result.begin() + 1, pos);
    return result;
}

void QrCode::drawFunctionPatterns() {
    for (int i = 0; i < size_; ++i) {
        setModule(6, i, i % 2 == 0, true);          // timing
        setModule(i, 6, i % 2 == 0, true);
    }

    drawFinder(3, 3);
    drawFinder(size_ - 4, 3);
    drawFinder(3, size_ - 4);

    const std::vector<int> align = alignmentPositions();
    for (size_t i = 0; i < align.size(); ++i)
        for (size_t j = 0; j < align.size(); ++j) {
            const bool corner = (i == 0 && j == 0) ||
                                (i == 0 && j == align.size() - 1) ||
                                (i == align.size() - 1 && j == 0);
            if (!corner) drawAlignment(align[i], align[j]);
        }

    drawFormatBits(0);
    drawVersion();
}

void QrCode::drawFormatBits(int mask) {
    const int data = eccFormatBits(ecc_) << 3 | mask;
    int rem = data;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    const int bits = (data << 10 | rem) ^ 0x5412;

    auto bit = [bits](int i) { return ((bits >> i) & 1) != 0; };

    for (int i = 0; i <= 5; ++i) setModule(8, i, bit(i), true);
    setModule(8, 7, bit(6), true);
    setModule(8, 8, bit(7), true);
    setModule(7, 8, bit(8), true);
    for (int i = 9; i < 15; ++i) setModule(14 - i, 8, bit(i), true);

    for (int i = 0; i < 8; ++i) setModule(size_ - 1 - i, 8, bit(i), true);
    for (int i = 8; i < 15; ++i) setModule(8, size_ - 15 + i, bit(i), true);
    setModule(8, size_ - 8, true, true);            // always dark
}

void QrCode::drawVersion() {
    if (version_ < 7) return;

    int rem = version_;
    for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    const long bits = static_cast<long>(version_) << 12 | rem;

    for (int i = 0; i < 18; ++i) {
        const bool dark = ((bits >> i) & 1) != 0;
        const int a = size_ - 11 + i % 3;
        const int b = i / 3;
        setModule(a, b, dark, true);
        setModule(b, a, dark, true);
    }
}

void QrCode::drawCodewords(const std::vector<uint8_t>& data) {
    size_t i = 0;                                   // bit index
    for (int right = size_ - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;                  // the vertical timing pattern
        for (int vert = 0; vert < size_; ++vert) {
            for (int j = 0; j < 2; ++j) {
                const int x = right - j;
                const bool upward = ((right + 1) & 2) == 0;
                const int y = upward ? size_ - 1 - vert : vert;
                if (isFunction_[static_cast<size_t>(y * size_ + x)]) continue;
                if (i < data.size() * 8) {
                    const bool dark = ((data[i >> 3] >> (7 - (i & 7))) & 1) != 0;
                    setModule(x, y, dark, false);
                    ++i;
                }
                // Remaining modules stay light, as the standard requires.
            }
        }
    }
}

void QrCode::applyMask(int mask) {
    for (int y = 0; y < size_; ++y)
        for (int x = 0; x < size_; ++x) {
            if (isFunction_[static_cast<size_t>(y * size_ + x)]) continue;
            bool invert = false;
            switch (mask) {
                case 0: invert = (x + y) % 2 == 0; break;
                case 1: invert = y % 2 == 0; break;
                case 2: invert = x % 3 == 0; break;
                case 3: invert = (x + y) % 3 == 0; break;
                case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
                case 5: invert = x * y % 2 + x * y % 3 == 0; break;
                case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0; break;
                case 7: invert = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                default: break;
            }
            if (invert)
                modules_[static_cast<size_t>(y * size_ + x)] ^= 1;
        }
}

// The four penalty rules of the standard. These decide which mask is chosen,
// so getting them wrong produces a valid but different QR code — which is
// exactly how the first version of this failed against a reference encoder.
int QrCode::finderPenaltyCountPatterns(const int history[7]) const {
    const int n = history[1];
    const bool core = n > 0 && history[2] == n && history[3] == n * 3 &&
                      history[4] == n && history[5] == n;
    return (core && history[0] >= n * 4 && history[6] >= n ? 1 : 0)
         + (core && history[6] >= n * 4 && history[0] >= n ? 1 : 0);
}

void QrCode::finderPenaltyAddHistory(int runLength, int history[7]) const {
    if (history[0] == 0) runLength += size_;      // the light border counts
    for (int i = 6; i > 0; --i) history[i] = history[i - 1];
    history[0] = runLength;
}

int QrCode::finderPenaltyTerminate(bool runColor, int runLength, int history[7]) const {
    if (runColor) {                                // ends with a dark run
        finderPenaltyAddHistory(runLength, history);
        runLength = 0;
    }
    runLength += size_;                            // and the light border
    finderPenaltyAddHistory(runLength, history);
    return finderPenaltyCountPatterns(history);
}

long QrCode::penaltyScore() const {
    long result = 0;
    constexpr int N1 = 3, N2 = 3, N3 = 40, N4 = 10;

    // Rule 1 and 3, by row.
    for (int y = 0; y < size_; ++y) {
        bool runColor = false;
        int  runLen   = 0;
        int  history[7] = {0};
        for (int x = 0; x < size_; ++x) {
            if (moduleAt(x, y) == runColor) {
                ++runLen;
                if (runLen == 5)      result += N1;
                else if (runLen > 5)  ++result;
            } else {
                finderPenaltyAddHistory(runLen, history);
                if (!runColor) result += finderPenaltyCountPatterns(history) * N3;
                runColor = moduleAt(x, y);
                runLen   = 1;
            }
        }
        result += finderPenaltyTerminate(runColor, runLen, history) * N3;
    }

    // Rule 1 and 3, by column.
    for (int x = 0; x < size_; ++x) {
        bool runColor = false;
        int  runLen   = 0;
        int  history[7] = {0};
        for (int y = 0; y < size_; ++y) {
            if (moduleAt(x, y) == runColor) {
                ++runLen;
                if (runLen == 5)      result += N1;
                else if (runLen > 5)  ++result;
            } else {
                finderPenaltyAddHistory(runLen, history);
                if (!runColor) result += finderPenaltyCountPatterns(history) * N3;
                runColor = moduleAt(x, y);
                runLen   = 1;
            }
        }
        result += finderPenaltyTerminate(runColor, runLen, history) * N3;
    }

    // Rule 2: 2x2 blocks of one colour.
    for (int y = 0; y < size_ - 1; ++y)
        for (int x = 0; x < size_ - 1; ++x) {
            const bool c = moduleAt(x, y);
            if (c == moduleAt(x + 1, y) && c == moduleAt(x, y + 1) &&
                c == moduleAt(x + 1, y + 1))
                result += N2;
        }

    // Rule 4: imbalance between dark and light.
    int dark = 0;
    for (uint8_t m : modules_) dark += m;
    const int total = size_ * size_;
    const int k = static_cast<int>((std::abs(dark * 20L - total * 10L) + total - 1) / total) - 1;
    result += static_cast<long>(k) * N4;
    return result;
}

std::vector<std::string> QrCode::rows() const {
    std::vector<std::string> out;
    for (int y = 0; y < size_; ++y) {
        std::string row;
        for (int x = 0; x < size_; ++x) row.push_back(moduleAt(x, y) ? '1' : '0');
        out.push_back(row);
    }
    return out;
}

} // namespace fk
