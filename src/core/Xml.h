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

// Xml.h - a small read-only XML tree.
//
// Enough to read a bank statement and, later, an incoming UBL invoice:
// elements, attributes, text, entities, CDATA, comments, processing
// instructions and self-closing tags. Not a validating parser and not a
// streaming one — statements and invoices are small enough to hold in memory.
//
// Namespace prefixes are stripped. `<ns2:Ntry>` and `<Ntry>` are the same
// element here, because every bank declares the ISO 20022 namespace under a
// different prefix and none of that changes what the document means.
//
// No Qt, no dependencies.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace fk {

struct XmlNode {
    std::string name;                              // local name, prefix removed
    std::map<std::string, std::string> attributes; // also with prefixes removed
    std::string text;                              // this element's own text
    std::vector<XmlNode> children;

    /// A child by path, "Amt" or "Acct/Id/IBAN". Null when any step is missing.
    const XmlNode* find(const std::string& path) const;

    /// The text at `path`, or empty. The workhorse.
    std::string textAt(const std::string& path) const;

    /// An attribute, or empty.
    std::string attribute(const std::string& name) const;

    /// Every direct child called `name`, in document order.
    std::vector<const XmlNode*> childrenNamed(const std::string& name) const;

    /// The first descendant called `name`, at any depth, breadth first. Banks
    /// disagree about how deeply they nest the same field, and a search that
    /// stops at the shallowest match gets the one the standard intends.
    const XmlNode* findDeep(const std::string& name) const;

    /// The text of the first descendant called `name`, or empty.
    std::string deepText(const std::string& name) const;
};

struct XmlDocument {
    XmlNode     root;
    bool        ok = false;
    std::string error;      // with a byte offset when the document is malformed
};

XmlDocument parseXml(const std::string& text);

/// Expands &amp; &lt; &gt; &quot; &apos; and numeric references. Exposed for
/// testing; parseXml applies it to text and attribute values already.
std::string xmlUnescape(const std::string& text);

} // namespace fk
