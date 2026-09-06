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

#include "Lzma1.h"

#include <cstdint>
#include <vector>

namespace fk {
namespace {

constexpr int      LC = 3;         // literal context bits
constexpr int      PB = 2;         // position bits
constexpr int      MODEL_TOTAL_BITS = 11;
constexpr uint16_t PROB_INIT = (1 << MODEL_TOTAL_BITS) / 2;
constexpr int      MOVE_BITS = 5;

/// The LZMA range encoder, as in the reference implementation. Every bit is
/// coded against an adaptive probability that both sides update identically.
class RangeEncoder {
public:
    void encodeBit(uint16_t& prob, int bit) {
        const uint32_t bound = (range_ >> MODEL_TOTAL_BITS) * prob;
        if (bit == 0) {
            range_ = bound;
            prob = static_cast<uint16_t>(prob + (((1 << MODEL_TOTAL_BITS) - prob) >> MOVE_BITS));
        } else {
            low_ += bound;
            range_ -= bound;
            prob = static_cast<uint16_t>(prob - (prob >> MOVE_BITS));
        }
        while (range_ < (1u << 24)) {
            range_ <<= 8;
            shiftLow();
        }
    }

    void flush() {
        for (int i = 0; i < 5; ++i) shiftLow();
    }

    const std::string& bytes() const { return out_; }

private:
    void shiftLow() {
        if (static_cast<uint32_t>(low_ >> 32) != 0 || low_ < 0xFF000000ULL) {
            uint8_t temp = cache_;
            do {
                out_.push_back(static_cast<char>(
                    static_cast<uint8_t>(temp + static_cast<uint8_t>(low_ >> 32))));
                temp = 0xFF;
            } while (--cacheSize_ != 0);
            cache_ = static_cast<uint8_t>(low_ >> 24);
        }
        ++cacheSize_;
        low_ = (low_ & 0x00FFFFFFULL) << 8;
    }

    std::string out_;
    uint64_t low_       = 0;
    uint32_t range_     = 0xFFFFFFFF;
    uint8_t  cache_     = 0;
    int64_t  cacheSize_ = 1;
};

} // namespace

std::string lzma1CompressLiterals(const std::string& data) {
    RangeEncoder encoder;

    // Two probability models are enough when nothing but literals is emitted:
    // "is this a match?" (always no) and the literal coder itself.
    //
    // The state machine stays at 0 throughout, because a literal following a
    // literal leaves it there — which is why no other context is needed.
    std::vector<uint16_t> isMatch(static_cast<size_t>(16), PROB_INIT);
    std::vector<uint16_t> literals(static_cast<size_t>(0x300) << LC, PROB_INIT);

    uint8_t previous = 0;
    for (size_t position = 0; position < data.size(); ++position) {
        const uint8_t symbol = static_cast<uint8_t>(data[position]);
        const size_t  posState = position & ((1u << PB) - 1);

        encoder.encodeBit(isMatch[posState], 0);            // literal, never a match

        // lp = 0, so the context is the top LC bits of the previous byte.
        uint16_t* prob = literals.data() + 0x300 * static_cast<size_t>(previous >> (8 - LC));
        int context = 1;
        for (int bit = 7; bit >= 0; --bit) {
            const int value = (symbol >> bit) & 1;
            encoder.encodeBit(prob[context], value);
            context = (context << 1) | value;
        }

        previous = symbol;
    }

    encoder.flush();
    return encoder.bytes();
}

} // namespace fk
