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

// VatRegister.h - the two Finančná správa registers, kept locally.
//
//   ds_dphs  "Zoznam daňových subjektov registrovaných pre DPH"   ~120 MB XML
//   ds_dsrdp "Zoznam daňových subjektov registrovaných na daň
//             z príjmov"                                          ~361 MB XML
//
// Both are daily, public, CC-BY, and need no API key. The first gives IČ DPH
// (and therefore DIČ) for VAT payers; the second gives DIČ for everyone else,
// which is the only way to fill that field for a customer who is not a
// platiteľ DPH.
//
// Everything here streams. Nothing ever holds a whole dataset in memory.
#pragma once

#include <functional>
#include <string>

namespace fk {

class Database;
class XmlRecord;

constexpr const char* VAT_REGISTER_URL = "https://report.financnasprava.sk/ds_dphs.zip";
constexpr const char* TAX_REGISTER_URL = "https://report.financnasprava.sk/ds_dsrdp.zip";

/// One <ITEM> of ds_dphs.
struct VatSubject {
    std::string icDph;      // IC_DPH
    std::string ico;        // ICO
    std::string name;       // NAZOV_DS
    std::string street;     // ULICA_CISLO
    std::string city;       // OBEC
    std::string postalCode; // PSC
    std::string country;    // STAT
    std::string regType;    // DRUH_REG_DPH — "§4", "§7", "§7a"
    std::string regDate;    // DATUM_REG, stored ISO

    bool valid() const { return !icDph.empty(); }
    /// DIČ is the VAT number without its country prefix.
    std::string dic() const;
    /// True only for §4 — a full VAT payer who charges Slovak VAT.
    bool isFullVatPayer() const;
    std::string registrationNote() const;
};

/// One <ITEM> of ds_dsrdp. Many are individuals with no IČO at all.
struct TaxSubject {
    std::string dic;        // DIC
    std::string ico;        // ICO — often empty
    std::string name;       // NAZOV_DS
    std::string street;     // ULICA_CISLO
    std::string city;       // OBEC
    std::string postalCode; // PSC
    std::string country;    // NAZOV_STATU

    bool valid() const { return !dic.empty() && !ico.empty(); }
};

struct ImportResult {
    bool        ok       = false;
    bool        canceled = false;
    int         imported = 0;
    int         skipped  = 0;
    std::string datasetDate;   // ISO, taken from <DatumAktualizacieZoznamu>
    std::string error;
};

VatSubject vatSubjectFromRecord(const XmlRecord& record);
TaxSubject taxSubjectFromRecord(const XmlRecord& record);

/// "11.10.2000" -> "2000-10-11", and "28072026" -> "2026-07-28".
/// Returns the input unchanged if it is neither shape.
std::string isoFromSkDate(const std::string& published);

/// `progress` gets the running record count; returning false cancels and rolls
/// the whole import back.
using ImportProgress = std::function<bool(int recordsSoFar)>;

ImportResult importVatRegisterZip(const std::string& zipBytes, Database& db,
                                  const ImportProgress& progress = {});
ImportResult importTaxRegisterZip(const std::string& zipBytes, Database& db,
                                  const ImportProgress& progress = {});

} // namespace fk
