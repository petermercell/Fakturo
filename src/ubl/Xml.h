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

// Xml.h - a ~100 line XML writer. Deliberately not a DOM: UBL is write-once
// and element order is fixed by the schema, so a streaming writer is enough.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace fk {

using XmlAttrs = std::vector<std::pair<std::string, std::string>>;

class XmlWriter {
public:
    explicit XmlWriter(bool pretty = true) : pretty_(pretty) {
        out_ = R"(<?xml version="1.0" encoding="UTF-8"?>)";
        out_ += pretty_ ? "\n" : "";
    }

    void open(const std::string& name, const XmlAttrs& attrs = {}) {
        indent();
        out_ += "<" + name;
        appendAttrs(attrs);
        out_ += ">";
        newline();
        stack_.push_back(name);
    }

    void close() {
        if (stack_.empty()) return;
        std::string name = stack_.back();
        stack_.pop_back();
        indent();
        out_ += "</" + name + ">";
        newline();
    }

    /// Writes <name attrs>text</name>. Skipped entirely when text is empty.
    void leaf(const std::string& name, const std::string& text, const XmlAttrs& attrs = {}) {
        if (text.empty()) return;
        indent();
        out_ += "<" + name;
        appendAttrs(attrs);
        out_ += ">" + escape(text) + "</" + name + ">";
        newline();
    }

    /// Same as leaf() but writes even when the text is empty.
    void leafAlways(const std::string& name, const std::string& text, const XmlAttrs& attrs = {}) {
        indent();
        out_ += "<" + name;
        appendAttrs(attrs);
        out_ += ">" + escape(text) + "</" + name + ">";
        newline();
    }

    const std::string& str() const { return out_; }

    static std::string escape(const std::string& s) {
        std::string o;
        o.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '&':  o += "&amp;";  break;
                case '<':  o += "&lt;";   break;
                case '>':  o += "&gt;";   break;
                case '"':  o += "&quot;"; break;
                case '\'': o += "&apos;"; break;
                default:   o += c;
            }
        }
        return o;
    }

private:
    void appendAttrs(const XmlAttrs& attrs) {
        for (const auto& [k, v] : attrs)
            if (!v.empty()) out_ += " " + k + "=\"" + escape(v) + "\"";
    }
    void indent()  { if (pretty_) out_.append(stack_.size() * 2, ' '); }
    void newline() { if (pretty_) out_ += "\n"; }

    bool                     pretty_;
    std::string              out_;
    std::vector<std::string> stack_;
};

/// RAII helper: `{ XmlScope s(w, "cac:Party"); ... }` closes on scope exit.
class XmlScope {
public:
    XmlScope(XmlWriter& w, const std::string& name, const XmlAttrs& attrs = {}) : w_(w) {
        w_.open(name, attrs);
    }
    ~XmlScope() { w_.close(); }
    XmlScope(const XmlScope&) = delete;
    XmlScope& operator=(const XmlScope&) = delete;

private:
    XmlWriter& w_;
};

} // namespace fk
