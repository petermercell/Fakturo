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

// Zip.h - just enough ZIP to read a government data export.
//
// Reads the central directory of an in-memory archive and inflates one entry.
// Store (method 0) and deflate (method 8) only; no encryption, no ZIP64,
// no multi-disk. Backed by zlib, which ships with macOS and every Linux.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fk {

struct ZipEntry {
    std::string name;
    uint16_t    method           = 0;
    uint32_t    compressedSize   = 0;
    uint32_t    uncompressedSize = 0;
    uint32_t    localOffset      = 0;
};

/// Lists the archive contents. Empty result + `error` set means it is not a
/// ZIP we can read.
std::vector<ZipEntry> zipList(const std::string& archive, std::string* error = nullptr);

/// Inflates one entry into `out`. Only for entries small enough to hold whole.
bool zipExtract(const std::string& archive, const ZipEntry& entry, std::string& out,
                std::string* error = nullptr);

/// Inflates one entry in chunks, handing each to `sink`. Nothing is buffered,
/// which is what makes a 361 MB dataset possible on a laptop. Returning false
/// from the sink stops the extraction and reports success.
using ZipSink = std::function<bool(const char* data, size_t length)>;
bool zipExtractStream(const std::string& archive, const ZipEntry& entry, const ZipSink& sink,
                      std::string* error = nullptr);

/// The largest entry, optionally filtered by suffix. Returns false if none.
bool zipLargestEntry(const std::string& archive, const std::string& suffix, ZipEntry& out,
                     std::string* error = nullptr);

/// Extracts the largest entry whose name ends with `suffix` (case-insensitive).
/// Pass an empty suffix to take the largest entry of any kind.
bool zipExtractLargest(const std::string& archive, const std::string& suffix, std::string& out,
                       std::string* error = nullptr);

/// Comma-separated entry names, for error messages when the expected file is
/// not where it was assumed to be.
std::string zipContentsDescription(const std::string& archive);

} // namespace fk
