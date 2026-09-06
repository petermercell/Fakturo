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

// Lzma1.h - a minimal LZMA1 raw encoder.
//
// PAY by square carries its payload as a raw LZMA1 stream. Rather than take on
// liblzma for one QR code, this emits a *literals-only* stream: every byte is
// coded as a literal, with no match finding at all.
//
// The result is a completely valid LZMA1 stream — any decoder reads it — it is
// simply larger than a real compressor would produce. For a payment payload of
// a few hundred bytes that costs nothing that matters, and it keeps the project
// free of another native dependency. Verified by round-tripping through both
// liblzma and the Slovak Banking Association's own reference decoder.
#pragma once

#include <string>

namespace fk {

/// Raw LZMA1, lc=3 lp=0 pb=2, no 13-byte header and no end marker: the caller
/// stores the uncompressed length separately, as the format requires.
std::string lzma1CompressLiterals(const std::string& data);

} // namespace fk
