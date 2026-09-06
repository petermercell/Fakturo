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

// RegistryData.h - what a public-registry lookup returns, and the pure string
// logic for turning it into invoice fields.
//
// Deliberately Qt-free so the tricky parts (register note wording, address
// splitting, postal codes) are unit tested without a network or a GUI.
// The HTTP + JSON layer lives in RegistryClient.
#pragma once

#include "../model/Model.h"

#include <string>
#include <vector>

namespace fk {

/// One subject as returned by RPO (Register právnických osôb) or VIES.
struct RegistryRecord {
    bool        found = false;
    std::string source;            // "RPO" or "VIES"

    std::string name;
    std::string ico;
    std::string icDph;
    std::string dic;

    std::string street;            // already joined with the building number
    std::string city;
    std::string postalCode;        // digits only, no space
    std::string countryCode = "SK";

    /// True when the source says the subject is an active VAT payer.
    /// ARES reports this directly; RPO does not know it.
    bool vatActive = false;
    /// Filled by sources that already phrase the register line themselves
    /// (ARES). When set, registryNote() returns it verbatim.
    std::string registryNoteText;

    std::string registerName;      // "Obchodný register"
    std::string court;             // "Okresný súd Banská Bystrica"
    std::string registrationNumber;// "Sro/37737/S"
    std::string established;       // ISO date

    /// Human-readable note for the invoice footer, e.g.
    /// "Zapísaná v obchodnom registri OS Banská Bystrica, oddiel: Sro, vložka č. 37737/S"
    std::string registryNote() const;
};

/// "Strážska cesta" + "8467/17" -> "Strážska cesta 8467/17".
std::string joinStreet(const std::string& street, const std::string& buildingNumber);

/// "960 01" -> "96001". Leaves anything unexpected untouched.
std::string normalizePostalCode(const std::string& postalCode);

/// Slovak courts are long; invoices use the short form.
std::string shortenCourt(const std::string& court);

/// RPO returns a numeric ISO 3166-1 country code ("703"); we need alpha-2.
std::string countryAlpha2(const std::string& numericOrName);

/// Uppercase, no spaces or dashes. Used before comparing VAT numbers, so that
/// "sk 2020 317068" and "SK2020317068" are recognised as the same value.
std::string normalizeVat(const std::string& vat);

/// "SK2020317068" -> "2020317068". For Slovak subjects the DIČ and the IČ DPH
/// are the same number, so each can be derived from the other.
std::string dicFromIcDph(const std::string& icDph);

/// "2020317068" + "SK" -> "SK2020317068". Returns empty for a DIČ that is not
/// 10 digits, so a half-typed number never produces a bogus VAT id.
std::string icDphFromDic(const std::string& dic, const std::string& countryCode = "SK");

/// VIES hands back one address blob, typically "Ulica 1\n81101 Mesto".
struct SplitAddress {
    std::string street;
    std::string postalCode;
    std::string city;
};
SplitAddress splitViesAddress(const std::string& blob);

/// Which fields of `target` would be overwritten by non-matching registry data.
/// Empty result means the merge is safe to apply without asking.
std::vector<std::string> conflictingFields(const RegistryRecord& r, const Party& target);

/// Copies the record onto the party. `overwrite = false` fills only blanks.
void applyRecord(const RegistryRecord& r, Party& target, bool overwrite);

} // namespace fk
