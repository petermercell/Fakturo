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

#include "RegistryData.h"

#include "../country/Country.h"

#include <algorithm>
#include <cctype>

namespace fk {
namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

} // namespace

std::string joinStreet(const std::string& street, const std::string& buildingNumber) {
    std::string s = trim(street), n = trim(buildingNumber);
    if (s.empty()) return n;
    if (n.empty() || n == "0") return s;
    return s + " " + n;
}

std::string normalizePostalCode(const std::string& postalCode) {
    std::string out;
    for (char c : postalCode)
        if (!std::isspace(static_cast<unsigned char>(c))) out.push_back(c);
    return out;
}

std::string shortenCourt(const std::string& court) {
    std::string c = trim(court);
    // "Okresný súd Banská Bystrica" -> "OS Banská Bystrica"
    if (startsWith(c, "Okresný súd ")) return "OS " + c.substr(std::string("Okresný súd ").size());
    if (startsWith(c, "Mestský súd "))  return "MS " + c.substr(std::string("Mestský súd ").size());
    if (startsWith(c, "Krajský súd "))  return "KS " + c.substr(std::string("Krajský súd ").size());
    return c;
}

std::string normalizeVat(const std::string& vat) {
    std::string out;
    for (char c : vat) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == '-' || c == '.') continue;
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string dicFromIcDph(const std::string& icDph) {
    const std::string v = normalizeVat(icDph);
    if (v.size() > 2 && std::isalpha(static_cast<unsigned char>(v[0])) &&
        std::isalpha(static_cast<unsigned char>(v[1])))
        return v.substr(2);
    return v;
}

std::string icDphFromDic(const std::string& dic, const std::string& countryCode) {
    std::string digits;
    for (char c : dic)
        if (c >= '0' && c <= '9') digits.push_back(c);
    if (digits.size() != 10) return {};
    std::string cc = countryCode.empty() ? std::string("SK") : countryCode;
    for (char& c : cc) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return cc + digits;
}

std::string countryAlpha2(const std::string& numericOrName) {
    const std::string v = trim(numericOrName);
    if (v.size() == 2) return v;
    if (v == "703") return "SK";
    if (v == "203") return "CZ";
    if (v == "040") return "AT";
    if (v == "348") return "HU";
    if (v == "616") return "PL";
    if (v == "276") return "DE";
    if (v.find("Slovensk") != std::string::npos) return "SK";
    if (v.find("Česk")     != std::string::npos) return "CZ";
    if (v.find("Rakúsk")   != std::string::npos) return "AT";
    if (v.find("Maďarsk")  != std::string::npos) return "HU";
    if (v.find("Poľsk")    != std::string::npos) return "PL";
    return "SK";
}

std::string RegistryRecord::registryNote() const {
    if (!registryNoteText.empty()) return registryNoteText;
    if (court.empty() && registrationNumber.empty()) return {};

    const std::string where = shortenCourt(court);
    const bool isCommercial = registerName.find("bchodn") != std::string::npos;

    if (!isCommercial) {
        std::string note = registerName.empty() ? std::string("Zapísaný v registri")
                                                : "Zapísaný v registri: " + registerName;
        if (!where.empty())              note += ", " + where;
        if (!registrationNumber.empty()) note += ", č. " + registrationNumber;
        return note;
    }

    // Commercial register numbers look like "Sro/37737/S": section / entry.
    std::string section, entry;
    const size_t slash = registrationNumber.find('/');
    if (slash != std::string::npos) {
        section = registrationNumber.substr(0, slash);
        entry   = registrationNumber.substr(slash + 1);
    } else {
        entry = registrationNumber;
    }

    std::string note = "Zapísaná v obchodnom registri";
    if (!where.empty())   note += " " + where;
    if (!section.empty()) note += ", oddiel: " + section;
    if (!entry.empty())   note += ", vložka č. " + entry;
    return note;
}

SplitAddress splitViesAddress(const std::string& blob) {
    SplitAddress out;

    // Normalise the separators VIES uses: newlines, or a single line with commas.
    std::vector<std::string> lines;
    std::string current;
    for (char c : blob) {
        if (c == '\n' || c == '\r') {
            if (!trim(current).empty()) lines.push_back(trim(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!trim(current).empty()) lines.push_back(trim(current));
    if (lines.size() == 1) {
        // Fall back to splitting on commas.
        std::vector<std::string> parts;
        std::string piece;
        for (char c : lines[0]) {
            if (c == ',') { if (!trim(piece).empty()) parts.push_back(trim(piece)); piece.clear(); }
            else piece.push_back(c);
        }
        if (!trim(piece).empty()) parts.push_back(trim(piece));
        if (parts.size() > 1) lines = parts;
    }
    if (lines.empty()) return out;

    out.street = lines.front();

    // The last line that starts with digits is the "PSČ Mesto" line.
    for (size_t i = lines.size(); i-- > 0;) {
        const std::string& l = lines[i];
        size_t p = 0;
        std::string digits;
        while (p < l.size() && (std::isdigit(static_cast<unsigned char>(l[p])) || l[p] == ' ')) {
            if (l[p] != ' ') digits.push_back(l[p]);
            ++p;
        }
        if (digits.size() >= 4 && p < l.size()) {
            out.postalCode = digits;
            out.city       = trim(l.substr(p));
            if (i == 0) out.street.clear();
            break;
        }
    }
    if (out.city.empty() && lines.size() > 1) {
        // The last line of a VIES address is normally the country. Only use it
        // as the city if there is nothing better, i.e. exactly two lines.
        auto hasDigit = [](const std::string& l) {
            return std::any_of(l.begin(), l.end(),
                               [](char c) { return c >= '0' && c <= '9'; });
        };
        size_t idx = lines.size() - 1;
        if (!hasDigit(lines.back()) && lines.size() >= 3) idx = lines.size() - 2;
        out.city = lines[idx];

        // Formats like "Bratislava 851 01" put the postal code at the end.
        if (out.postalCode.empty()) {
            size_t end = out.city.size();
            std::string digits;
            while (end > 0 && (std::isdigit(static_cast<unsigned char>(out.city[end - 1])) ||
                               out.city[end - 1] == ' ')) {
                if (out.city[end - 1] != ' ') digits.insert(digits.begin(), out.city[end - 1]);
                --end;
            }
            if (digits.size() >= 4) {
                out.postalCode = digits;
                out.city       = trim(out.city.substr(0, end));
            }
        }
    }
    return out;
}

std::vector<std::string> conflictingFields(const RegistryRecord& r, const Party& target) {
    std::vector<std::string> out;
    auto check = [&](const char* label, const std::string& incoming, const std::string& existing) {
        if (!incoming.empty() && !existing.empty() && incoming != existing) out.push_back(label);
    };
    check("Obchodné meno", r.name, target.name);
    check("Ulica",         joinStreet(r.street, ""), target.address.street);
    check("Mesto",         r.city, target.address.city);
    check("PSČ",           r.postalCode, target.address.postalCode);
    if (!r.icDph.empty() && !target.icDph.empty() &&
        normalizeVat(r.icDph) != normalizeVat(target.icDph))
        out.push_back("IČ DPH");
    check("DIČ",           r.dic, target.dic);
    return out;
}

void applyRecord(const RegistryRecord& r, Party& target, bool overwrite) {
    auto set = [overwrite](std::string& field, const std::string& value) {
        if (value.empty()) return;
        if (overwrite || field.empty()) field = value;
    };
    set(target.name,                r.name);
    set(target.ico,                 r.ico);
    set(target.icDph,               r.icDph);
    set(target.dic,                 r.dic);
    set(target.address.street,      r.street);
    set(target.address.city,        r.city);
    set(target.address.postalCode,  r.postalCode);
    set(target.address.countryCode, r.countryCode);

    // The Peppol endpoint is deliberately *not* filled from a registry lookup
    // for a Slovak party. Slovakia routes on the DIČ under scheme 0245, which
    // peppolParticipant() works out from the party itself — and a lookup that
    // wrote the IČO here would put an address on the record that the network
    // cannot resolve, and then win over the rule because an identifier that is
    // there is treated as one the user meant.
    //
    // Czechia still routes on the IČO, so there it is worth filling in.
    const std::string code =
        r.countryCode.empty() ? target.address.countryCode : r.countryCode;
    if (target.endpointId.empty() && !r.ico.empty() &&
        countryFromCode(code) != Country::SK) {
        target.endpointId = r.ico;
        if (target.endpointScheme.empty())
            target.endpointScheme = profileFor(code).schemes.companyId;
    }
}

} // namespace fk
