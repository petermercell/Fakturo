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

#include "Units.h"

#include <cctype>

namespace fk {
namespace {

/// Lower-cases and folds the diacritics, so "Deň", "den" and "DEŇ" are one
/// thing. Small and local: the search folder in InvoiceView is about a
/// different job and should be free to diverge.
std::string fold(const std::string& text) {
    static const struct { const char* from; char to; } map[] = {
        {"á", 'a'}, {"ä", 'a'}, {"č", 'c'}, {"ď", 'd'}, {"é", 'e'}, {"ě", 'e'},
        {"í", 'i'}, {"ĺ", 'l'}, {"ľ", 'l'}, {"ň", 'n'}, {"ó", 'o'}, {"ô", 'o'},
        {"ř", 'r'}, {"š", 's'}, {"ť", 't'}, {"ú", 'u'}, {"ů", 'u'}, {"ý", 'y'},
        {"ž", 'z'},
        {"Á", 'a'}, {"Ä", 'a'}, {"Č", 'c'}, {"Ď", 'd'}, {"É", 'e'}, {"Ě", 'e'},
        {"Í", 'i'}, {"Ĺ", 'l'}, {"Ľ", 'l'}, {"Ň", 'n'}, {"Ó", 'o'}, {"Ô", 'o'},
        {"Ř", 'r'}, {"Š", 's'}, {"Ť", 't'}, {"Ú", 'u'}, {"Ů", 'u'}, {"Ý", 'y'},
        {"Ž", 'z'}};

    std::string out;
    for (size_t i = 0; i < text.size();) {
        const auto b = static_cast<unsigned char>(text[i]);
        if (b < 0x80) {
            if (!std::isspace(b)) out += static_cast<char>(std::tolower(b));
            ++i;
            continue;
        }
        const size_t width = (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
        const std::string glyph = text.substr(i, width);
        bool replaced = false;
        for (const auto& entry : map)
            if (glyph == entry.from) { out += entry.to; replaced = true; break; }
        if (!replaced) out += glyph;      // m², m³ and the like keep their shape
        i += width;
    }
    return out;
}

} // namespace

const std::vector<UnitOfMeasure>& unitsOfMeasure() {
    // Verified against Peppol's own published code list — the one BR-CL-23
    // validates against — and cross-checked against a second published table.
    // Codes I could not confirm from a source (SET, XPK, NAR) are deliberately
    // absent: a plausible guess here is an invoice an Access Point rejects.
    static const std::vector<UnitOfMeasure> units = {
        {"H87", "ks",     "ks"},        // piece
        {"E48", "služba", "služba"},    // service unit
        {"HUR", "hod",    "hod"},       // hour
        {"DAY", "deň",    "den"},       // day
        {"WEE", "týždeň", "týden"},     // week
        {"MON", "mesiac", "měsíc"},     // month
        {"ANN", "rok",    "rok"},       // year
        {"MTR", "m",      "m"},         // metre
        {"MTK", "m²",     "m²"},        // square metre
        {"MTQ", "m³",     "m³"},        // cubic metre
        {"CMT", "cm",     "cm"},        // centimetre
        {"MMT", "mm",     "mm"},        // millimetre
        {"KMT", "km",     "km"},        // kilometre
        {"GRM", "g",      "g"},         // gram
        {"KGM", "kg",     "kg"},        // kilogram
        {"TNE", "t",      "t"},         // tonne
        {"LTR", "l",      "l"},         // litre
    };
    return units;
}

std::string unitCodeForLabel(const std::string& label) {
    const std::string wanted = fold(label);
    if (wanted.empty()) return {};
    for (const UnitOfMeasure& u : unitsOfMeasure()) {
        if (fold(u.labelSk) == wanted || fold(u.labelCz) == wanted) return u.code;
        // Somebody may type the code itself.
        if (fold(u.code) == wanted) return u.code;
    }
    return {};
}

std::string unitLabel(const std::string& code, Country country) {
    for (const UnitOfMeasure& u : unitsOfMeasure())
        if (code == u.code) return country == Country::CZ ? u.labelCz : u.labelSk;
    return code;
}

bool isKnownUnitCode(const std::string& code) {
    for (const UnitOfMeasure& u : unitsOfMeasure())
        if (code == u.code) return true;
    return false;
}

} // namespace fk
