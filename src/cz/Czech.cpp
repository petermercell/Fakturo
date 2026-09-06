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

#include "Czech.h"

#include <algorithm>
#include <cctype>

namespace fk::cz {
namespace {

std::string digitsOf(const std::string& s) {
    std::string out;
    for (char c : s)
        if (c >= '0' && c <= '9') out.push_back(c);
    return out;
}

std::string upperNoSpace(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == '-' || c == '.') continue;
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

} // namespace

bool validIco(const std::string& ico) {
    const std::string d = digitsOf(ico);
    if (d.size() != 8) return false;

    int sum = 0;
    for (int i = 0; i < 7; ++i) sum += (d[static_cast<size_t>(i)] - '0') * (8 - i);
    const int remainder = sum % 11;

    int check;
    if (remainder == 0)      check = 1;
    else if (remainder == 1) check = 0;
    else                     check = 11 - remainder;

    return check == (d[7] - '0');
}

bool validDic(const std::string& dic) {
    const std::string s = upperNoSpace(dic);
    if (s.size() < 10 || s.size() > 12) return false;
    if (s.compare(0, 2, "CZ") != 0) return false;

    const std::string body = s.substr(2);
    if (!std::all_of(body.begin(), body.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return false;

    // Legal entities: CZ + IČO, so the IČO checksum must hold. Natural persons
    // use a birth number (9 or 10 digits), which we accept on shape alone.
    if (body.size() == 8) return validIco(body);
    return body.size() == 9 || body.size() == 10;
}

std::string dicFromIco(const std::string& ico) {
    const std::string d = digitsOf(ico);
    return d.size() == 8 ? "CZ" + d : std::string();
}

std::string normalizePsc(const std::string& psc) { return digitsOf(psc); }

std::string formatPsc(const std::string& psc) {
    const std::string d = normalizePsc(psc);
    if (d.size() != 5) return d;
    return d.substr(0, 3) + " " + d.substr(3);
}

std::string courtName(const std::string& code) {
    const std::string c = upperNoSpace(code);
    if (c == "MSPH") return "Městským soudem v Praze";
    if (c == "KSBR") return "Krajským soudem v Brně";
    if (c == "KSOS") return "Krajským soudem v Ostravě";
    if (c == "KSCB") return "Krajským soudem v Českých Budějovicích";
    if (c == "KSPL") return "Krajským soudem v Plzni";
    if (c == "KSUL") return "Krajským soudem v Ústí nad Labem";
    if (c == "KSHK") return "Krajským soudem v Hradci Králové";
    return code;
}

std::string registryNoteFromSpisovaZnacka(const std::string& znacka) {
    // ARES publishes it as "B 1581/MSPH": section, insert number, court code.
    if (znacka.empty()) return {};

    const size_t space = znacka.find(' ');
    const size_t slash = znacka.rfind('/');
    if (space == std::string::npos || slash == std::string::npos || slash < space)
        return "Zapsáno v obchodním rejstříku, sp. zn. " + znacka;

    const std::string section = znacka.substr(0, space);
    const std::string insert  = znacka.substr(space + 1, slash - space - 1);
    const std::string court   = courtName(znacka.substr(slash + 1));

    return "Zapsáno v OR vedeném " + court + ", oddíl " + section + ", vložka " + insert;
}

} // namespace fk::cz
