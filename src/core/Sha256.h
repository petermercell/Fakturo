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

// Sha256.h - FIPS 180-4 SHA-256.
//
// Implemented here rather than taken from Qt so that archive integrity belongs
// to the core and can be unit tested without a GUI. It is checked against the
// published NIST vectors in tests.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace fk {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t length);
    void update(const std::string& text) { update(text.data(), text.size()); }
    /// Lower-case hex. The object must not be updated afterwards.
    std::string hex();

    static std::string hexOf(const void* data, size_t length);
    static std::string hexOf(const std::string& text) { return hexOf(text.data(), text.size()); }

private:
    void transform(const uint8_t block[64]);

    uint32_t state_[8];
    uint64_t bitCount_ = 0;
    uint8_t  buffer_[64];
    size_t   buffered_ = 0;
    bool     finished_ = false;
};

} // namespace fk
