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

#include "VatRegister.h"

#include "../core/XmlRecords.h"
#include "../core/Zip.h"
#include "../db/Database.h"

#include <algorithm>
#include <cctype>

namespace fk {
namespace {

bool isLetter(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }
bool isDigit(char c)  { return c >= '0' && c <= '9'; }

/// Pulls <DatumAktualizacieZoznamu>ddmmyyyy</…> out of the first chunk, so the
/// UI can show when the data was published rather than when it was downloaded.
std::string datasetDateFrom(const std::string& head) {
    const std::string open = "<DatumAktualizacieZoznamu>";
    const size_t at = head.find(open);
    if (at == std::string::npos) return {};
    const size_t end = head.find('<', at + open.size());
    if (end == std::string::npos) return {};
    return isoFromSkDate(head.substr(at + open.size(), end - at - open.size()));
}

} // namespace

// ------------------------------------------------------------------- dates
std::string isoFromSkDate(const std::string& published) {
    // "11.10.2000"
    if (published.size() == 10 && published[2] == '.' && published[5] == '.') {
        for (size_t i : {0u, 1u, 3u, 4u, 6u, 7u, 8u, 9u})
            if (!isDigit(published[i])) return published;
        return published.substr(6, 4) + "-" + published.substr(3, 2) + "-" + published.substr(0, 2);
    }
    // "28072026" — the header uses this form
    if (published.size() == 8) {
        for (char c : published)
            if (!isDigit(c)) return published;
        return published.substr(4, 4) + "-" + published.substr(2, 2) + "-" + published.substr(0, 2);
    }
    return published;
}

// ---------------------------------------------------------------- subjects
std::string VatSubject::dic() const {
    if (icDph.size() > 2 && isLetter(icDph[0]) && isLetter(icDph[1])) return icDph.substr(2);
    return icDph;
}

bool VatSubject::isFullVatPayer() const {
    return !regType.empty() && regType.find('7') == std::string::npos;
}

std::string VatSubject::registrationNote() const {
    if (regType.find("7a") != std::string::npos)
        return "Registrovaný podľa §7a – nie je platiteľom DPH, nefakturuje sa mu slovenská DPH.";
    if (regType.find('7') != std::string::npos)
        return "Registrovaný podľa §7 – nie je platiteľom DPH.";
    if (regType.empty()) return {};
    return "Platiteľ DPH (" + regType + ").";
}

VatSubject vatSubjectFromRecord(const XmlRecord& r) {
    VatSubject s;
    s.icDph      = r.value("IC_DPH");
    s.ico        = r.value("ICO");
    s.name       = r.value("NAZOV_DS");
    s.street     = r.value("ULICA_CISLO");
    s.city       = r.value("OBEC");
    s.postalCode = r.value("PSC");
    s.country    = r.value("STAT");
    s.regType    = r.value("DRUH_REG_DPH");
    s.regDate    = isoFromSkDate(r.value("DATUM_REG"));
    return s;
}

TaxSubject taxSubjectFromRecord(const XmlRecord& r) {
    TaxSubject s;
    s.dic        = r.value("DIC");
    s.ico        = r.value("ICO");
    s.name       = r.value("NAZOV_DS");
    s.street     = r.value("ULICA_CISLO");
    s.city       = r.value("OBEC");
    s.postalCode = r.value("PSC");
    s.country    = r.value("NAZOV_STATU");
    return s;
}

// ----------------------------------------------------------------- import
namespace {

/// Shared skeleton for both registers: open the archive, stream-inflate the
/// largest entry through an XML scanner, insert inside one transaction.
template <typename Subject, typename FromRecord>
ImportResult runImport(const std::string& zipBytes, Database& db, RegisterKind kind,
                       const char* recordTag, FromRecord fromRecord,
                       const ImportProgress& progress, const char* missingFieldHint) {
    ImportResult result;

    ZipEntry entry;
    std::string zipError;
    if (!zipLargestEntry(zipBytes, "", entry, &zipError)) {
        result.error = zipError.empty() ? "Archív sa nepodarilo otvoriť." : zipError;
        return result;
    }

    if (!db.registerImportBegin(kind)) {
        result.error = db.lastError();
        return result;
    }

    int  count       = 0;
    int  accepted    = 0;
    int  skipped     = 0;
    bool writeFailed = false;
    bool canceled    = false;
    std::string head;

    XmlRecordScanner scanner(recordTag, [&](const XmlRecord& record) {
        const Subject subject = fromRecord(record);
        if (!subject.valid()) { ++skipped; return true; }
        if (!db.registerImportAdd(subject)) { writeFailed = true; return false; }
        ++accepted;
        ++count;
        if (progress && (count == 1 || count % 20000 == 0) && !progress(count)) {
            canceled = true;
            return false;
        }
        return true;
    });

    const bool streamed = zipExtractStream(zipBytes, entry,
        [&](const char* data, size_t length) {
            if (head.size() < 4096) head.append(data, std::min<size_t>(length, 4096));
            return scanner.feed(data, length);
        },
        &zipError);

    if (canceled) {
        db.registerImportRollback();
        result.canceled = true;
        result.error    = "Import bol zrušený.";
        return result;
    }
    if (writeFailed) {
        db.registerImportRollback();
        result.error = db.lastError();
        return result;
    }
    if (!streamed) {
        db.registerImportRollback();
        result.error = zipError.empty() ? "Archív sa nepodarilo rozbaliť." : zipError;
        return result;
    }
    if (accepted == 0) {
        db.registerImportRollback();
        result.error = std::string("V exporte sa nenašiel ani jeden použiteľný záznam. ")
                     + missingFieldHint + " Súbor v archíve: " + entry.name;
        return result;
    }

    result.datasetDate = datasetDateFrom(head);
    if (!db.registerImportCommit(kind, result.datasetDate)) {
        result.error = db.lastError();
        return result;
    }

    result.ok       = true;
    result.imported = accepted;
    result.skipped  = skipped;
    if (progress) progress(accepted);
    return result;
}

} // namespace

ImportResult importVatRegisterZip(const std::string& zipBytes, Database& db,
                                  const ImportProgress& progress) {
    return runImport<VatSubject>(zipBytes, db, RegisterKind::Vat, "ITEM", vatSubjectFromRecord,
                                 progress, "Očakával sa prvok <IC_DPH>.");
}

ImportResult importTaxRegisterZip(const std::string& zipBytes, Database& db,
                                  const ImportProgress& progress) {
    return runImport<TaxSubject>(zipBytes, db, RegisterKind::IncomeTax, "ITEM", taxSubjectFromRecord,
                                 progress, "Očakávali sa prvky <DIC> a <ICO>.");
}

} // namespace fk
