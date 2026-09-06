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

#include "XmlRecords.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace fk {
namespace {

std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

void appendUtf8(std::string& out, unsigned long cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

} // namespace

const std::string& XmlRecord::value(const std::string& name) const {
    static const std::string empty;
    const std::string want = lowered(name);
    for (const auto& [key, text] : fields_)
        if (lowered(key) == want) return text;
    return empty;
}

std::string XmlRecordScanner::decodeEntities(const std::string& text) {
    if (text.find('&') == std::string::npos) return text;

    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') { out.push_back(text[i]); continue; }

        const size_t semi = text.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 10) { out.push_back('&'); continue; }

        const std::string name = text.substr(i + 1, semi - i - 1);
        if      (name == "amp")  out.push_back('&');
        else if (name == "lt")   out.push_back('<');
        else if (name == "gt")   out.push_back('>');
        else if (name == "quot") out.push_back('"');
        else if (name == "apos") out.push_back('\'');
        else if (name.size() > 1 && name[0] == '#') {
            const bool hex = (name[1] == 'x' || name[1] == 'X');
            const unsigned long cp =
                std::strtoul(name.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10);
            if (cp == 0) { out.push_back('&'); continue; }
            appendUtf8(out, cp);
        } else {
            out.push_back('&');
            continue;                     // unknown entity: leave it alone
        }
        i = semi;
    }
    return out;
}

XmlRecordScanner::XmlRecordScanner(std::string recordTag, Handler handler)
    : open_("<" + recordTag + ">"),
      openSelf_("<" + recordTag + " "),
      close_("</" + recordTag + ">"),
      handler_(std::move(handler)) {}

void XmlRecordScanner::parseRecord(const char* begin, const char* end) {
    record_.clear();

    const char* p = begin;
    while (p < end) {
        // Find the next child element start.
        while (p < end && *p != '<') ++p;
        if (p >= end) break;
        ++p;
        if (p < end && (*p == '/' || *p == '?' || *p == '!')) {    // close / PI / comment
            while (p < end && *p != '>') ++p;
            continue;
        }

        const char* nameStart = p;
        while (p < end && *p != '>' && *p != ' ' && *p != '/' && *p != '\t' && *p != '\n') ++p;
        const std::string name(nameStart, static_cast<size_t>(p - nameStart));

        // Skip any attributes.
        bool selfClosing = false;
        while (p < end && *p != '>') {
            if (*p == '/') selfClosing = true;
            ++p;
        }
        if (p >= end) break;
        ++p;                                                        // past '>'
        if (selfClosing || name.empty()) { record_.add(name, {}); continue; }

        const std::string closeTag = "</" + name + ">";
        const char* valueEnd = std::search(p, end, closeTag.begin(), closeTag.end());
        if (valueEnd == end) break;                                 // malformed; give up

        record_.add(name, decodeEntities(trimmed(std::string(p, static_cast<size_t>(valueEnd - p)))));
        p = valueEnd + closeTag.size();
    }

    if (record_.empty()) return;
    ++count_;
    if (handler_ && !handler_(record_)) stopped_ = true;
}

bool XmlRecordScanner::feed(const char* data, size_t length) {
    if (stopped_) return false;
    buffer_.append(data, length);

    size_t searchFrom = 0;
    while (!stopped_) {
        size_t start = buffer_.find(open_, searchFrom);
        if (start == std::string::npos) {
            const size_t alt = buffer_.find(openSelf_, searchFrom);
            if (alt == std::string::npos) break;
            start = alt;
        }
        const size_t bodyStart = buffer_.find('>', start);
        if (bodyStart == std::string::npos) break;

        const size_t stop = buffer_.find(close_, bodyStart);
        if (stop == std::string::npos) {
            // Record is split across chunks: keep it, drop everything before it.
            if (start > 0) buffer_.erase(0, start);
            return !stopped_;
        }

        parseRecord(buffer_.data() + bodyStart + 1, buffer_.data() + stop);
        searchFrom = stop + close_.size();
    }

    // Keep only a tail long enough to hold a split opening tag.
    if (searchFrom > 0) {
        buffer_.erase(0, searchFrom);
    } else if (buffer_.size() > open_.size()) {
        buffer_.erase(0, buffer_.size() - open_.size());
    }
    return !stopped_;
}

} // namespace fk
