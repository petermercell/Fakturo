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

#include "Xml.h"

#include <cctype>
#include <cstdint>
#include <deque>

namespace fk {
namespace {

std::string localName(const std::string& qualified) {
    const size_t colon = qualified.rfind(':');
    return colon == std::string::npos ? qualified : qualified.substr(colon + 1);
}

std::string trimSpace(const std::string& text) {
    size_t b = 0, e = text.size();
    while (b < e && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1]))) --e;
    return text.substr(b, e - b);
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

/// How deep an element may nest. A UBL invoice is about fifteen levels deep
/// and a camt.053 statement about twelve, so a hundred is generous by an order
/// of magnitude — and the parser recurses once per level, so without a limit a
/// 280 KB file of nothing but open tags exhausts the stack and takes the
/// process with it. From January 2027 these documents arrive from strangers,
/// which turns that from a curiosity into the one genuinely exploitable thing
/// in the import path.
/// Named in lower camel case, not SHOUTING_CASE: `MAX_INPUT` is a macro in
/// macOS's <sys/syslimits.h>, and a macro beats a namespace every time. The
/// Linux build never saw it.
constexpr int maxNestingDepth = 100;

/// The largest document this will look at. Statements and invoices are tens of
/// kilobytes; anything past this is either a mistake or an attempt to exhaust
/// memory, and the parser holds the whole tree at once.
constexpr size_t maxDocumentBytes = 32u * 1024u * 1024u;

struct Parser {
    const std::string& in;
    size_t             i = 0;
    int                depth = 0;
    std::string        error;

    explicit Parser(const std::string& text) : in(text) {}

    bool atEnd() const { return i >= in.size(); }

    bool looking(const char* literal) const {
        const size_t n = std::char_traits<char>::length(literal);
        return in.compare(i, n, literal) == 0;
    }

    void skipSpace() {
        while (i < in.size() && std::isspace(static_cast<unsigned char>(in[i]))) ++i;
    }

    void fail(const std::string& what) {
        if (error.empty()) error = what + " (na pozícii " + std::to_string(i) + ")";
    }

    /// Comments, processing instructions, the declaration and DOCTYPE. None of
    /// them carry data we want; all of them appear in real files.
    bool skipProlog() {
        for (;;) {
            skipSpace();
            if (looking("<!--")) {
                const size_t end = in.find("-->", i + 4);
                if (end == std::string::npos) { fail("neuzavretý komentár"); return false; }
                i = end + 3;
            } else if (looking("<?")) {
                const size_t end = in.find("?>", i + 2);
                if (end == std::string::npos) { fail("neuzavretá inštrukcia"); return false; }
                i = end + 2;
            } else if (looking("<!DOCTYPE")) {
                // Refused outright rather than skipped. Skipping it correctly
                // means tracking quotes and comments through an internal
                // subset, and the previous attempt got that wrong in both
                // directions — a legal `<!ENTITY e "x>y">` was rejected, and an
                // unbalanced '<' inside a quoted value swallowed the rest of
                // the file. Nothing this application reads has a DOCTYPE, and
                // declared entities are deliberately not honoured anyway, so
                // the safe reading of one is none.
                fail("dokument obsahuje DOCTYPE, ktorý sa nespracúva");
                return false;
            } else {
                return true;
            }
        }
    }

    std::string readName() {
        const size_t start = i;
        while (i < in.size()) {
            const char c = in[i];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ||
                c == '.' || c == ':')
                ++i;
            else break;
        }
        return in.substr(start, i - start);
    }

    /// Raises and lowers the nesting count around one element, so every exit
    /// from readElement — including the failing ones — puts it back.
    struct Level {
        Parser& p;
        explicit Level(Parser& parser) : p(parser) { ++p.depth; }
        ~Level() { --p.depth; }
    };

    /// Reads one element, assuming `i` points at its '<'.
    bool readElement(XmlNode& out) {
        const Level level(*this);
        if (depth > maxNestingDepth) { fail("príliš hlboko vnorený dokument"); return false; }
        if (in[i] != '<') { fail("očakával sa element"); return false; }
        ++i;
        const std::string qualified = readName();
        if (qualified.empty()) { fail("element bez názvu"); return false; }
        out.name = localName(qualified);

        // ------------------------------------------------------- attributes
        for (;;) {
            skipSpace();
            if (atEnd()) { fail("neuzavretá značka"); return false; }
            if (in[i] == '>' || looking("/>")) break;

            const std::string attrName = readName();
            if (attrName.empty()) { fail("neplatný atribút"); return false; }
            skipSpace();
            if (atEnd() || in[i] != '=') { fail("atribút bez hodnoty"); return false; }
            ++i;
            skipSpace();
            if (atEnd() || (in[i] != '"' && in[i] != '\'')) {
                fail("hodnota atribútu bez úvodzoviek");
                return false;
            }
            const char quote = in[i++];
            const size_t start = i;
            while (i < in.size() && in[i] != quote) ++i;
            if (atEnd()) { fail("neuzavretá hodnota atribútu"); return false; }
            out.attributes[localName(attrName)] = xmlUnescape(in.substr(start, i - start));
            ++i;
        }

        if (looking("/>")) { i += 2; return true; }   // empty element
        ++i;                                          // past '>'

        // ---------------------------------------------------------- content
        std::string text;
        for (;;) {
            if (atEnd()) { fail("neuzavretý element " + out.name); return false; }

            if (looking("</")) {
                i += 2;
                const std::string closing = localName(readName());
                skipSpace();
                if (atEnd() || in[i] != '>') { fail("neuzavretá koncová značka"); return false; }
                ++i;
                if (closing != out.name) {
                    fail("koncová značka " + closing + " nesedí na " + out.name);
                    return false;
                }
                out.text = trimSpace(text);
                return true;
            }
            if (looking("<!--")) {
                const size_t end = in.find("-->", i + 4);
                if (end == std::string::npos) { fail("neuzavretý komentár"); return false; }
                i = end + 3;
                continue;
            }
            if (looking("<![CDATA[")) {
                const size_t end = in.find("]]>", i + 9);
                if (end == std::string::npos) { fail("neuzavretá sekcia CDATA"); return false; }
                text += in.substr(i + 9, end - (i + 9));   // verbatim, no unescaping
                i = end + 3;
                continue;
            }
            if (looking("<?")) {
                const size_t end = in.find("?>", i + 2);
                if (end == std::string::npos) { fail("neuzavretá inštrukcia"); return false; }
                i = end + 2;
                continue;
            }
            if (in[i] == '<') {
                XmlNode child;
                if (!readElement(child)) return false;
                out.children.push_back(std::move(child));
                continue;
            }

            const size_t start = i;
            while (i < in.size() && in[i] != '<') ++i;
            text += xmlUnescape(in.substr(start, i - start));
        }
    }
};

} // namespace

std::string xmlUnescape(const std::string& text) {
    if (text.find('&') == std::string::npos) return text;

    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text[i] != '&') { out += text[i++]; continue; }

        const size_t semi = text.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) { out += text[i++]; continue; }

        const std::string entity = text.substr(i + 1, semi - i - 1);
        if      (entity == "amp")  out += '&';
        else if (entity == "lt")   out += '<';
        else if (entity == "gt")   out += '>';
        else if (entity == "quot") out += '"';
        else if (entity == "apos") out += '\'';
        else if (entity.size() > 1 && entity[0] == '#') {
            try {
                // Range-checked as 64 bits *before* narrowing. Checking after
                // the cast let &#4294967361; wrap to 65 and come out as "A" —
                // a character reference that means nothing turning into one
                // that means something.
                const unsigned long value = entity[1] == 'x' || entity[1] == 'X'
                                                ? std::stoul(entity.substr(2), nullptr, 16)
                                                : std::stoul(entity.substr(1));
                // Surrogate halves are code points that may not stand alone;
                // encoding them produces bytes that are not valid UTF-8, and
                // SQLite and Qt both reject or mangle those downstream.
                const bool surrogate = value >= 0xD800 && value <= 0xDFFF;
                if (value == 0 || value > 0x10FFFF || surrogate)
                    out += text.substr(i, semi - i + 1);
                else
                    appendUtf8(out, static_cast<uint32_t>(value));
            } catch (...) {
                out += text.substr(i, semi - i + 1);
            }
        } else {
            out += text.substr(i, semi - i + 1);   // unknown: leave it alone
        }
        i = semi + 1;
    }
    return out;
}

// ------------------------------------------------------------------- lookups

const XmlNode* XmlNode::find(const std::string& path) const {
    const XmlNode* node = this;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const std::string step =
            path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!step.empty()) {
            const XmlNode* next = nullptr;
            for (const XmlNode& child : node->children)
                if (child.name == step) { next = &child; break; }
            if (!next) return nullptr;
            node = next;
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return node;
}

std::string XmlNode::textAt(const std::string& path) const {
    const XmlNode* node = find(path);
    return node ? node->text : std::string();
}

std::string XmlNode::attribute(const std::string& name) const {
    const auto it = attributes.find(name);
    return it == attributes.end() ? std::string() : it->second;
}

std::vector<const XmlNode*> XmlNode::childrenNamed(const std::string& name) const {
    std::vector<const XmlNode*> out;
    for (const XmlNode& child : children)
        if (child.name == name) out.push_back(&child);
    return out;
}

const XmlNode* XmlNode::findDeep(const std::string& name) const {
    // Breadth first on purpose: the shallowest match is the one the standard
    // means. A depth-first walk into RltdPties would find a nested Nm before
    // the element's own.
    std::deque<const XmlNode*> queue;
    for (const XmlNode& child : children) queue.push_back(&child);
    while (!queue.empty()) {
        const XmlNode* node = queue.front();
        queue.pop_front();
        if (node->name == name) return node;
        for (const XmlNode& child : node->children) queue.push_back(&child);
    }
    return nullptr;
}

std::string XmlNode::deepText(const std::string& name) const {
    const XmlNode* node = findDeep(name);
    return node ? node->text : std::string();
}

// -------------------------------------------------------------------- parse

XmlDocument parseXml(const std::string& text) {
    XmlDocument out;

    if (text.size() > maxDocumentBytes) {
        out.error = "Súbor je príliš veľký na spracovanie.";
        return out;
    }

    // The BOM is skipped by moving the start rather than by copying the whole
    // document to erase three bytes from it. The tree already costs several
    // times the size of the input; a second copy of the input on top of that
    // was measurable.
    const size_t begin = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3u : 0u;
    const std::string& body = text;

    Parser parser(body);
    parser.i = begin;
    if (!parser.skipProlog()) { out.error = parser.error; return out; }
    if (parser.atEnd() || body[parser.i] != '<') {
        out.error = "Súbor nezačína XML elementom.";
        return out;
    }
    if (!parser.readElement(out.root)) {
        out.error = parser.error.empty() ? "Chybné XML." : parser.error;
        return out;
    }
    // Nothing may follow the root but whitespace, comments and processing
    // instructions. Two documents concatenated used to read as the first one
    // and report success, which is how a receiver silently drops an invoice.
    if (!parser.skipProlog()) { out.error = parser.error; return out; }
    if (!parser.atEnd()) {
        out.error = "Za koncom dokumentu je ďalší obsah (na pozícii " +
                    std::to_string(parser.i) + ").";
        return out;
    }
    out.ok = true;
    return out;
}

} // namespace fk
