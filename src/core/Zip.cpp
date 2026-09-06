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

#include "Zip.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace fk {
namespace {

constexpr uint32_t SIG_EOCD    = 0x06054b50;
constexpr uint32_t SIG_CENTRAL = 0x02014b50;
constexpr uint32_t SIG_LOCAL   = 0x04034b50;

uint16_t read16(const std::string& d, size_t at) {
    if (at + 2 > d.size()) return 0;
    return static_cast<uint16_t>(static_cast<unsigned char>(d[at]) |
                                 (static_cast<unsigned char>(d[at + 1]) << 8));
}

uint32_t read32(const std::string& d, size_t at) {
    if (at + 4 > d.size()) return 0;
    return static_cast<uint32_t>(static_cast<unsigned char>(d[at])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(d[at + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(d[at + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(d[at + 3])) << 24);
}

bool endsWithCI(const std::string& s, const std::string& suffix) {
    if (suffix.empty()) return true;
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        const char a = static_cast<char>(std::tolower(
            static_cast<unsigned char>(s[s.size() - suffix.size() + i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
        if (a != b) return false;
    }
    return true;
}

void fail(std::string* error, const std::string& message) {
    if (error) *error = message;
}

} // namespace

std::vector<ZipEntry> zipList(const std::string& archive, std::string* error) {
    std::vector<ZipEntry> out;
    if (archive.size() < 22) {
        fail(error, "Súbor je príliš malý na to, aby bol ZIP.");
        return out;
    }

    // The end-of-central-directory record sits in the last 64 KiB + 22 bytes.
    const size_t window = std::min<size_t>(archive.size(), 0xFFFF + 22);
    size_t eocd = std::string::npos;
    for (size_t back = 22; back <= window; ++back) {
        const size_t at = archive.size() - back;
        if (read32(archive, at) == SIG_EOCD) { eocd = at; break; }
    }
    if (eocd == std::string::npos) {
        fail(error, "Neplatný ZIP: nenašiel sa centrálny adresár.");
        return out;
    }

    const uint16_t count     = read16(archive, eocd + 10);
    const uint32_t dirOffset = read32(archive, eocd + 16);
    if (dirOffset >= archive.size()) {
        fail(error, "Neplatný ZIP: poškodený centrálny adresár.");
        return out;
    }

    size_t at = dirOffset;
    for (uint16_t i = 0; i < count && at + 46 <= archive.size(); ++i) {
        if (read32(archive, at) != SIG_CENTRAL) break;

        ZipEntry e;
        e.method           = read16(archive, at + 10);
        e.compressedSize   = read32(archive, at + 20);
        e.uncompressedSize = read32(archive, at + 24);
        const uint16_t nameLen    = read16(archive, at + 28);
        const uint16_t extraLen   = read16(archive, at + 30);
        const uint16_t commentLen = read16(archive, at + 32);
        e.localOffset      = read32(archive, at + 42);

        if (at + 46 + nameLen > archive.size()) break;
        e.name = archive.substr(at + 46, nameLen);
        out.push_back(std::move(e));

        at += 46u + nameLen + extraLen + commentLen;
    }

    if (out.empty()) fail(error, "ZIP neobsahuje žiadne súbory.");
    return out;
}

bool zipExtract(const std::string& archive, const ZipEntry& entry, std::string& out,
                std::string* error) {
    out.clear();

    if (entry.compressedSize == 0xFFFFFFFFu || entry.uncompressedSize == 0xFFFFFFFFu) {
        fail(error, "ZIP64 archívy nie sú podporované.");
        return false;
    }
    if (entry.localOffset + 30 > archive.size() || read32(archive, entry.localOffset) != SIG_LOCAL) {
        fail(error, "Neplatný ZIP: poškodená hlavička položky.");
        return false;
    }

    const uint16_t nameLen  = read16(archive, entry.localOffset + 26);
    const uint16_t extraLen = read16(archive, entry.localOffset + 28);
    const size_t   dataAt   = entry.localOffset + 30u + nameLen + extraLen;
    if (dataAt + entry.compressedSize > archive.size()) {
        fail(error, "Neplatný ZIP: dáta presahujú koniec súboru.");
        return false;
    }

    if (entry.method == 0) {                       // stored
        out.assign(archive, dataAt, entry.compressedSize);
        return true;
    }
    if (entry.method != 8) {                       // anything but deflate
        fail(error, "Nepodporovaná kompresia v ZIP archíve.");
        return false;
    }

    z_stream stream{};
    // Negative window bits: raw deflate, no zlib header, which is what ZIP uses.
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        fail(error, "Nepodarilo sa inicializovať dekompresiu.");
        return false;
    }

    stream.next_in  = reinterpret_cast<Bytef*>(const_cast<char*>(archive.data() + dataAt));
    stream.avail_in = entry.compressedSize;

    // size_t arithmetic: compressedSize * 4 would overflow 32 bits on a large
    // archive and reserve a nonsense amount.
    const size_t guess = entry.uncompressedSize
                             ? static_cast<size_t>(entry.uncompressedSize)
                             : static_cast<size_t>(entry.compressedSize) * 4;
    out.reserve(guess);
    std::string chunk;
    chunk.resize(256 * 1024);

    int status = Z_OK;
    do {
        stream.next_out  = reinterpret_cast<Bytef*>(&chunk[0]);
        stream.avail_out = static_cast<uInt>(chunk.size());
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) {
            inflateEnd(&stream);
            fail(error, "Chyba pri rozbaľovaní ZIP archívu.");
            return false;
        }
        const size_t produced = chunk.size() - stream.avail_out;
        out.append(chunk.data(), produced);

        // No input left and no output produced means the stream is truncated.
        // Without this the loop would spin forever on a damaged download.
        if (produced == 0 && (stream.avail_in == 0 || status == Z_BUF_ERROR)) break;
    } while (status != Z_STREAM_END);

    if (status != Z_STREAM_END && out.empty()) {
        inflateEnd(&stream);
        fail(error, "Archív je neúplný alebo poškodený.");
        return false;
    }

    inflateEnd(&stream);
    return true;
}

bool zipLargestEntry(const std::string& archive, const std::string& suffix, ZipEntry& out,
                     std::string* error) {
    const std::vector<ZipEntry> entries = zipList(archive, error);
    if (entries.empty()) return false;

    const ZipEntry* best = nullptr;
    for (const ZipEntry& e : entries) {
        if (!e.name.empty() && e.name.back() == '/') continue;      // directory
        if (!endsWithCI(e.name, suffix)) continue;
        if (!best || e.uncompressedSize > best->uncompressedSize) best = &e;
    }
    if (!best) {
        fail(error, "V archíve nie je vhodný súbor" +
                    (suffix.empty() ? std::string() : " s príponou " + suffix) +
                    ". Obsahuje: " + zipContentsDescription(archive));
        return false;
    }
    out = *best;
    return true;
}

bool zipExtractLargest(const std::string& archive, const std::string& suffix, std::string& out,
                       std::string* error) {
    ZipEntry entry;
    if (!zipLargestEntry(archive, suffix, entry, error)) return false;
    return zipExtract(archive, entry, out, error);
}

bool zipExtractStream(const std::string& archive, const ZipEntry& entry, const ZipSink& sink,
                      std::string* error) {
    if (entry.compressedSize == 0xFFFFFFFFu || entry.uncompressedSize == 0xFFFFFFFFu) {
        fail(error, "ZIP64 archívy nie sú podporované.");
        return false;
    }
    if (entry.localOffset + 30 > archive.size() || read32(archive, entry.localOffset) != SIG_LOCAL) {
        fail(error, "Neplatný ZIP: poškodená hlavička položky.");
        return false;
    }

    const uint16_t nameLen  = read16(archive, entry.localOffset + 26);
    const uint16_t extraLen = read16(archive, entry.localOffset + 28);
    const size_t   dataAt   = entry.localOffset + 30u + nameLen + extraLen;
    if (dataAt + entry.compressedSize > archive.size()) {
        fail(error, "Neplatný ZIP: dáta presahujú koniec súboru.");
        return false;
    }

    if (entry.method == 0) {                       // stored
        return sink(archive.data() + dataAt, entry.compressedSize), true;
    }
    if (entry.method != 8) {
        fail(error, "Nepodporovaná kompresia v ZIP archíve.");
        return false;
    }

    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        fail(error, "Nepodarilo sa inicializovať dekompresiu.");
        return false;
    }
    stream.next_in  = reinterpret_cast<Bytef*>(const_cast<char*>(archive.data() + dataAt));
    stream.avail_in = entry.compressedSize;

    std::string chunk;
    chunk.resize(512 * 1024);

    int  status   = Z_OK;
    bool sinkSaidStop = false;
    do {
        stream.next_out  = reinterpret_cast<Bytef*>(&chunk[0]);
        stream.avail_out = static_cast<uInt>(chunk.size());
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) {
            inflateEnd(&stream);
            fail(error, "Chyba pri rozbaľovaní ZIP archívu.");
            return false;
        }
        const size_t produced = chunk.size() - stream.avail_out;
        if (produced > 0 && sink && !sink(chunk.data(), produced)) { sinkSaidStop = true; break; }
        if (produced == 0 && (stream.avail_in == 0 || status == Z_BUF_ERROR)) break;
    } while (status != Z_STREAM_END);

    inflateEnd(&stream);
    if (!sinkSaidStop && status != Z_STREAM_END) {
        fail(error, "Archív je neúplný alebo poškodený.");
        return false;
    }
    return true;
}

std::string zipContentsDescription(const std::string& archive) {
    const std::vector<ZipEntry> entries = zipList(archive);
    if (entries.empty()) return "(nič)";

    std::string out;
    for (size_t i = 0; i < entries.size() && i < 10; ++i) {
        if (!out.empty()) out += ", ";
        out += entries[i].name + " (" + std::to_string(entries[i].uncompressedSize) + " B)";
    }
    if (entries.size() > 10) out += ", …";
    return out;
}

} // namespace fk
