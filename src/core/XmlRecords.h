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

// XmlRecords.h - a streaming scanner for "flat list of records" XML.
//
// The Slovak public datasets are shaped like this, and they are large — the
// income-tax register is 361 MB uncompressed — so nothing here ever holds more
// than one record plus the current chunk:
//
//   <ZoznamSubjektovRegistrovanychkDPH>
//     <DS_DPHS>
//       <ITEM><IC_DPH>SK…</IC_DPH><ICO>…</ICO>…</ITEM>
//       <ITEM>…</ITEM>
//
// Feed it whatever chunks the decompressor produces; record boundaries are
// found across chunk boundaries.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace fk {

/// Field name -> text, in document order, for one record.
class XmlRecord {
public:
    void add(std::string name, std::string value) {
        fields_.emplace_back(std::move(name), std::move(value));
    }
    void clear() { fields_.clear(); }
    bool empty() const { return fields_.empty(); }
    size_t size() const { return fields_.size(); }

    /// Case-insensitive lookup. Empty string when absent.
    const std::string& value(const std::string& name) const;
    const std::vector<std::pair<std::string, std::string>>& fields() const { return fields_; }

private:
    std::vector<std::pair<std::string, std::string>> fields_;
};

class XmlRecordScanner {
public:
    /// Returning false from the handler stops the scan.
    using Handler = std::function<bool(const XmlRecord&)>;

    XmlRecordScanner(std::string recordTag, Handler handler);

    /// Feeds one chunk. Returns false once the handler has asked to stop.
    bool feed(const char* data, size_t length);
    bool feed(const std::string& chunk) { return feed(chunk.data(), chunk.size()); }

    /// Number of records handed to the handler so far.
    int count() const { return count_; }
    bool stopped() const { return stopped_; }

    /// &amp; &lt; &gt; &quot; &apos; and numeric references.
    static std::string decodeEntities(const std::string& text);

private:
    void parseRecord(const char* begin, const char* end);

    std::string open_;        // "<ITEM>"  and  "<ITEM "
    std::string openSelf_;
    std::string close_;       // "</ITEM>"
    Handler     handler_;
    std::string buffer_;
    XmlRecord   record_;
    int         count_   = 0;
    bool        stopped_ = false;
};

} // namespace fk
