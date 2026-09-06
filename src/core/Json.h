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

// Json.h - a small read-only JSON tree, and just enough writing.
//
// The same bargain as Xml.h: enough to read what an Access Point replies, and
// no more. It exists so that reading a reply — which decides whether an
// invoice was sent — happens in the layer that has tests, rather than in the
// Qt layer, which is where every data-correctness defect in this project has
// come from.
//
// Not a general JSON library. Numbers are kept as their text, because the only
// numbers in these replies are counts and second-counts, and turning 900 into
// a double and back is a way to introduce a rounding question that does not
// otherwise exist.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace fk {

class JsonValue {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    /// The number exactly as it was written, and the text of a string.
    std::string text;
    std::vector<JsonValue> items;
    std::map<std::string, JsonValue> members;

    bool isNull()   const { return kind == Kind::Null; }
    bool isObject() const { return kind == Kind::Object; }
    bool isArray()  const { return kind == Kind::Array; }

    /// A member by path, "error/code". Returns a Null value when any step is
    /// missing, so a chain of lookups never needs a null check.
    const JsonValue& at(const std::string& path) const;
    /// The string at `path`, or empty. Works for a number too, which is what
    /// the caller wants for an identifier that a server may quote or not.
    std::string stringAt(const std::string& path) const;
    /// The integer at `path`, or `fallback` when it is missing or not one.
    long long intAt(const std::string& path, long long fallback = 0) const;
};

struct JsonDocument {
    JsonValue   root;
    bool        ok = false;
    std::string error;
};

JsonDocument parseJson(const std::string& text);

/// `"a \"quoted\" string"` — the value with its quotes, escaped. The only
/// writing this needs: everything sent is an object of strings.
std::string jsonQuote(const std::string& text);

} // namespace fk
