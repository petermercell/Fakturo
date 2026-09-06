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

#include "Json.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>

namespace fk {
namespace {

/// The same reasoning as the XML parser: this reads documents from a server
/// somebody else runs, and it recurses once per level of nesting.
constexpr int maxNestingDepth = 64;
constexpr size_t maxDocumentBytes = 8u * 1024u * 1024u;

const JsonValue& nullValue() {
    static const JsonValue empty;
    return empty;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

struct Parser {
    const std::string& in;
    size_t i = 0;
    int depth = 0;
    std::string error;

    explicit Parser(const std::string& text) : in(text) {}

    bool atEnd() const { return i >= in.size(); }
    void skipSpace() {
        while (i < in.size() && std::isspace(static_cast<unsigned char>(in[i]))) ++i;
    }
    bool fail(const std::string& what) {
        if (error.empty()) error = what + " (na pozícii " + std::to_string(i) + ")";
        return false;
    }

    struct Level {
        Parser& p;
        explicit Level(Parser& parser) : p(parser) { ++p.depth; }
        ~Level() { --p.depth; }
    };

    bool readHex4(uint32_t& out) {
        if (i + 4 > in.size()) return fail("skrátená \\u sekvencia");
        out = 0;
        for (int n = 0; n < 4; ++n) {
            const char c = in[i + static_cast<size_t>(n)];
            out <<= 4;
            if (c >= '0' && c <= '9')      out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else return fail("neplatná \\u sekvencia");
        }
        i += 4;
        return true;
    }

    bool readString(std::string& out) {
        if (atEnd() || in[i] != '"') return fail("očakával sa reťazec");
        ++i;
        out.clear();
        while (i < in.size()) {
            const char c = in[i];
            if (c == '"') { ++i; return true; }
            if (c != '\\') { out += c; ++i; continue; }

            ++i;
            if (atEnd()) return fail("neuzavretý reťazec");
            const char esc = in[i++];
            switch (esc) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!readHex4(cp)) return false;
                    // A surrogate pair is two escapes and one character. A lone
                    // half is not a character at all and is refused rather than
                    // encoded, because the bytes it would produce are not UTF-8
                    // and SQLite would reject them later, further from the cause.
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (i + 1 < in.size() && in[i] == '\\' && in[i + 1] == 'u') {
                            i += 2;
                            uint32_t low = 0;
                            if (!readHex4(low)) return false;
                            if (low < 0xDC00 || low > 0xDFFF) return fail("neplatný pár \\u");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            return fail("osamotená \\u polovica páru");
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("osamotená \\u polovica páru");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail("neznáma escape sekvencia");
            }
        }
        return fail("neuzavretý reťazec");
    }

    bool readValue(JsonValue& out) {
        const Level level(*this);
        if (depth > maxNestingDepth) return fail("príliš hlboko vnorený dokument");

        skipSpace();
        if (atEnd()) return fail("prázdny dokument");

        const char c = in[i];
        if (c == '"') {
            out.kind = JsonValue::Kind::String;
            return readString(out.text);
        }
        if (c == '{') {
            ++i;
            out.kind = JsonValue::Kind::Object;
            skipSpace();
            if (!atEnd() && in[i] == '}') { ++i; return true; }
            for (;;) {
                skipSpace();
                std::string key;
                if (!readString(key)) return false;
                skipSpace();
                if (atEnd() || in[i] != ':') return fail("očakávala sa dvojbodka");
                ++i;
                JsonValue member;
                if (!readValue(member)) return false;
                // Last one wins, which is what every mainstream parser does.
                out.members[key] = std::move(member);
                skipSpace();
                if (atEnd()) return fail("neuzavretý objekt");
                if (in[i] == ',') { ++i; continue; }
                if (in[i] == '}') { ++i; return true; }
                return fail("očakávala sa čiarka alebo }");
            }
        }
        if (c == '[') {
            ++i;
            out.kind = JsonValue::Kind::Array;
            skipSpace();
            if (!atEnd() && in[i] == ']') { ++i; return true; }
            for (;;) {
                JsonValue item;
                if (!readValue(item)) return false;
                out.items.push_back(std::move(item));
                skipSpace();
                if (atEnd()) return fail("neuzavreté pole");
                if (in[i] == ',') { ++i; continue; }
                if (in[i] == ']') { ++i; return true; }
                return fail("očakávala sa čiarka alebo ]");
            }
        }
        if (in.compare(i, 4, "true") == 0) {
            i += 4; out.kind = JsonValue::Kind::Bool; out.boolean = true;  return true;
        }
        if (in.compare(i, 5, "false") == 0) {
            i += 5; out.kind = JsonValue::Kind::Bool; out.boolean = false; return true;
        }
        if (in.compare(i, 4, "null") == 0) {
            i += 4; out.kind = JsonValue::Kind::Null; return true;
        }

        // A number, kept as its text.
        const size_t start = i;
        if (!atEnd() && (in[i] == '-' || in[i] == '+')) ++i;
        bool digits = false;
        while (i < in.size() &&
               (std::isdigit(static_cast<unsigned char>(in[i])) || in[i] == '.' ||
                in[i] == 'e' || in[i] == 'E' || in[i] == '-' || in[i] == '+')) {
            if (std::isdigit(static_cast<unsigned char>(in[i]))) digits = true;
            ++i;
        }
        if (!digits) return fail("neznáma hodnota");
        out.kind = JsonValue::Kind::Number;
        out.text = in.substr(start, i - start);
        return true;
    }
};

} // namespace

const JsonValue& JsonValue::at(const std::string& path) const {
    const JsonValue* node = this;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const std::string step =
            path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!step.empty()) {
            if (node->kind != Kind::Object) return nullValue();
            const auto it = node->members.find(step);
            if (it == node->members.end()) return nullValue();
            node = &it->second;
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return *node;
}

std::string JsonValue::stringAt(const std::string& path) const {
    const JsonValue& value = at(path);
    if (value.kind == Kind::String || value.kind == Kind::Number) return value.text;
    return {};
}

long long JsonValue::intAt(const std::string& path, long long fallback) const {
    const std::string text = stringAt(path);
    if (text.empty()) return fallback;
    try { return std::stoll(text); } catch (...) { return fallback; }
}

JsonDocument parseJson(const std::string& text) {
    JsonDocument out;
    if (text.size() > maxDocumentBytes) {
        out.error = "Odpoveď je príliš veľká.";
        return out;
    }
    Parser parser(text);
    if (!parser.readValue(out.root)) {
        out.error = parser.error.empty() ? "Chybný JSON." : parser.error;
        return out;
    }
    parser.skipSpace();
    if (!parser.atEnd()) {
        out.error = "Za koncom JSON dokumentu je ďalší obsah.";
        return out;
    }
    out.ok = true;
    return out;
}

std::string jsonQuote(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += "\"";
    return out;
}

} // namespace fk
