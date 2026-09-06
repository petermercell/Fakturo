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

#include "Country.h"

#include "../cz/Czech.h"
#include "../sk/Slovak.h"

#include <algorithm>
#include <cctype>

namespace fk {
namespace {

// EAS codes taken from the Peppol "Participant identifier schemes" code list.
//   0158  IČO, Slovakia (Act on State Statistics of 29 November 2001, § 27)
//   0245  DIČ, Slovakia — the tax identification number
//   9950  Slovakia VAT number
//   0154  IČO, Czech Republic
//   9929  Czech Republic VAT number
//
// These are the schemes for *identifying* a party on the document. Which one
// addresses it on the network is a separate question — see peppolParticipant()
// below, and the comment on it, because for Slovakia the two differ.
constexpr PeppolSchemes SK_SCHEMES{"0158", "9950", "0158 – IČO", "9950 – IČ DPH",
                                   "0245", "0245 – DIČ (adresa v Peppole)"};
constexpr PeppolSchemes CZ_SCHEMES{"0154", "9929", "0154 – IČO", "9929 – DIČ",
                                   "0154", "0154 – IČO"};

/// Slovakia's Peppol participant scheme: the DIČ, ten digits, no country
/// prefix. Kept separate from the identifying schemes because it answers a
/// different question and changing one must not silently change the other.
constexpr const char* SK_PARTICIPANT_SCHEME = "0245";

CountryProfile makeSk() {
    CountryProfile p;
    p.country         = Country::SK;
    p.code            = "SK";
    p.currency        = "EUR";
    p.vatPrefix       = "SK";
    p.paymentSymbol   = "0308";
    p.standardVatRate = Dec::fromInt(23);
    p.vatRates        = {Dec::fromInt(23), Dec::fromInt(19), Dec::fromInt(5), Dec()};
    p.schemes         = SK_SCHEMES;
    return p;
}

CountryProfile makeCz() {
    CountryProfile p;
    p.country         = Country::CZ;
    p.code            = "CZ";
    p.currency        = "CZK";
    p.vatPrefix       = "CZ";
    p.paymentSymbol   = "0308";
    p.standardVatRate = Dec::fromInt(21);
    // Unified reduced rate of 12 % since 1 January 2024.
    p.vatRates        = {Dec::fromInt(21), Dec::fromInt(12), Dec()};
    p.schemes         = CZ_SCHEMES;
    return p;
}

const InvoiceLabels SK_LABELS{
    "FAKTÚRA", "DOBROPIS", "PROFORMA FAKTÚRA",
    "FAKTÚRA – DAŇOVÝ DOKLAD K PRIJATEJ PLATBE",
    "Toto nie je daňový doklad. Slúži ako podklad na úhradu.",
    "Vyúčtovanie zálohových faktúr:", "č.",
    "Dátum vystavenia", "Dátum dodania", "Dátum splatnosti",
    "DODÁVATEĽ", "ODBERATEĽ",
    "IČO", "DIČ", "IČ DPH", "Neplatca DPH",
    "PLATBA", "IBAN", "BIC/SWIFT", "Variabilný symbol", "Konštantný symbol",
    "Popis", "Množstvo", "MJ", "Cena/MJ", "DPH", "Spolu bez DPH",
    "REKAPITULÁCIA DPH", "Sadzba", "Základ", "DPH",
    "Základ dane",
    "Súčet položiek", "Zľava", "Príplatok", "Zaokrúhlenie",
    "DPH v", "Kurz",
    "Uhradená záloha", "Na úhradu",
    "Dobropis k faktúre č.", "zo dňa", "Objednávka",
    "Faktúra je vyhotovená v elektronickej podobe a je platná bez podpisu a pečiatky.",
    "naskenujte v mobilnej aplikácii banky",
    "Bankový účet", "Spôsob platby", "Prevodom", "CENA"};

const InvoiceLabels CZ_LABELS{
    "FAKTURA", "DOBROPIS", "ZÁLOHOVÁ FAKTURA",
    "DAŇOVÝ DOKLAD K PŘIJATÉ PLATBĚ",
    "Toto není daňový doklad. Slouží jako podklad k úhradě.",
    "Vyúčtování zálohových faktur:", "č.",
    "Datum vystavení", "Datum zdanitelného plnění", "Datum splatnosti",
    "DODAVATEL", "ODBĚRATEL",
    "IČO", "DIČ", "DIČ", "Neplátce DPH",
    "PLATBA", "IBAN", "BIC/SWIFT", "Variabilní symbol", "Konstantní symbol",
    "Popis", "Množství", "MJ", "Cena/MJ", "DPH", "Celkem bez DPH",
    "REKAPITULACE DPH", "Sazba", "Základ", "DPH",
    "Základ daně",
    "Součet položek", "Sleva", "Příplatek", "Zaokrouhlení",
    "DPH v", "Kurz",
    "Uhrazená záloha", "K úhradě",
    "Dobropis k faktuře č.", "ze dne", "Objednávka",
    "Faktura je vystavena elektronicky a je platná bez podpisu a razítka.",
    "naskenujte v mobilní aplikaci banky",
    "Bankovní účet", "Způsob platby", "Převodem", "CENA"};

} // namespace

namespace {

std::string onlyDigits(const std::string& text) {
    std::string out;
    for (char c : text)
        if (c >= '0' && c <= '9') out.push_back(c);
    return out;
}

std::string padLeft(const std::string& text, size_t width) {
    if (text.size() >= width) return text.substr(text.size() - width);
    return std::string(width - text.size(), '0') + text;
}

std::string stripLeadingZeros(const std::string& text) {
    size_t i = 0;
    while (i + 1 < text.size() && text[i] == '0') ++i;
    return text.substr(i);
}

} // namespace

std::string ibanCheckDigits(const std::string& countryCode, const std::string& bban) {
    // ISO 13616: move the country code and "00" to the end, letters become
    // numbers, then 98 minus the whole thing mod 97.
    std::string rearranged = bban;
    for (char c : countryCode + "00") {
        if (std::isdigit(static_cast<unsigned char>(c)))
            rearranged.push_back(c);
        else
            rearranged += std::to_string(std::toupper(static_cast<unsigned char>(c)) - 'A' + 10);
    }

    int remainder = 0;
    for (char c : rearranged) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return {};
        remainder = (remainder * 10 + (c - '0')) % 97;
    }
    const int check = 98 - remainder;
    return (check < 10 ? "0" : "") + std::to_string(check);
}

LocalAccount parseLocalAccount(const std::string& text) {
    LocalAccount account;
    const size_t slash = text.rfind('/');
    if (slash == std::string::npos) return account;

    account.bankCode = onlyDigits(text.substr(slash + 1));
    if (account.bankCode.size() > 4) account.bankCode = account.bankCode.substr(0, 4);

    const std::string body = text.substr(0, slash);
    const size_t dash = body.find('-');
    if (dash == std::string::npos) {
        account.number = onlyDigits(body);
    } else {
        account.prefix = onlyDigits(body.substr(0, dash));
        account.number = onlyDigits(body.substr(dash + 1));
    }
    if (account.number.size() > 10) account.number = account.number.substr(0, 10);
    if (account.prefix.size() > 6)  account.prefix = account.prefix.substr(0, 6);
    return account;
}

std::string formatLocalAccount(const LocalAccount& account) {
    if (!account.valid()) return {};
    std::string out;
    const std::string prefix = stripLeadingZeros(account.prefix.empty() ? "0" : account.prefix);
    if (!prefix.empty() && prefix != "0") out += prefix + "-";
    out += stripLeadingZeros(account.number) + "/" + account.bankCode;
    return out;
}

std::string ibanFromLocalAccount(Country country, const LocalAccount& account) {
    if (!account.valid()) return {};
    const std::string code = codeFor(country);
    if (code != "SK" && code != "CZ") return {};

    const std::string bban = padLeft(account.bankCode, 4) + padLeft(account.prefix, 6) +
                             padLeft(account.number, 10);
    const std::string check = ibanCheckDigits(code, bban);
    if (check.empty()) return {};
    return code + check + bban;
}

std::string localAccountFromIban(const std::string& iban) {
    std::string clean;
    for (char c : iban)
        if (!std::isspace(static_cast<unsigned char>(c)))
            clean.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));

    if (clean.size() != 24) return {};
    const std::string code = clean.substr(0, 2);
    if (code != "SK" && code != "CZ") return {};

    LocalAccount account;
    account.bankCode = clean.substr(4, 4);
    account.prefix   = clean.substr(8, 6);
    account.number   = clean.substr(14, 10);
    for (const std::string& part : {account.bankCode, account.prefix, account.number})
        for (char c : part)
            if (!std::isdigit(static_cast<unsigned char>(c))) return {};
    return formatLocalAccount(account);
}

bool isEuMember(const std::string& isoCode) {
    static const std::vector<std::string> members = {
        "AT","BE","BG","HR","CY","CZ","DK","EE","FI","FR","DE","GR","EL","HU","IE",
        "IT","LV","LT","LU","MT","NL","PL","PT","RO","SK","SI","ES","SE"};
    std::string c;
    for (char ch : isoCode)
        if (!std::isspace(static_cast<unsigned char>(ch)))
            c.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    return std::find(members.begin(), members.end(), c) != members.end();
}

SupplyRegime detectSupplyRegime(const Company& seller, const Party& buyer) {
    const std::string sellerCountry = seller.address.countryCode;
    const std::string buyerCountry  =
        buyer.address.countryCode.empty() ? sellerCountry : buyer.address.countryCode;

    const bool sameCountry = countryFromCode(sellerCountry) == countryFromCode(buyerCountry) &&
                             !sellerCountry.empty();

    if (!sameCountry && !isEuMember(buyerCountry)) return SupplyRegime::ExportOutsideEu;

    // Reverse charge needs a business customer with a VAT number in another
    // member state, and the seller needs one too — which is precisely what a
    // §7a registration provides. Without both numbers the rule cannot apply.
    if (!sameCountry && isEuMember(buyerCountry) && isEuMember(sellerCountry) &&
        !buyer.icDph.empty() && !seller.icDph.empty())
        return SupplyRegime::ReverseChargeEu;

    return seller.chargesVat() ? SupplyRegime::DomesticVat : SupplyRegime::DomesticNoVat;
}

const char* vatCategoryFor(SupplyRegime regime) {
    switch (regime) {
        case SupplyRegime::DomesticVat:     return VatCat::Standard;
        case SupplyRegime::ReverseChargeEu: return VatCat::ReverseCharge;   // AE
        case SupplyRegime::ExportOutsideEu: return VatCat::Export;          // G
        case SupplyRegime::DomesticNoVat:   return VatCat::OutOfScope;      // O
    }
    return VatCat::Standard;
}

std::string supplyNoteFor(SupplyRegime regime, Country sellerCountry) {
    const bool czech = (sellerCountry == Country::CZ);
    switch (regime) {
        case SupplyRegime::ReverseChargeEu:
            // Article 226(11a) of the VAT Directive requires the words "reverse
            // charge" on the document. The customer is foreign, so it is
            // stated in both languages.
            return czech
                ? "Přenesení daňové povinnosti / Reverse charge. "
                  "Subject to the reverse charge in the country of receipt. "
                  "(čl. 196 směrnice 2006/112/ES)"
                : "Prenesenie daňovej povinnosti / Reverse charge. "
                  "Subject to the reverse charge in the country of receipt. "
                  "(čl. 196 smernice 2006/112/ES)";
        case SupplyRegime::ExportOutsideEu:
            return czech ? "Vývoz zboží / služeb mimo EU, osvobozeno od DPH."
                         : "Vývoz tovaru / služieb mimo EÚ, oslobodené od DPH.";
        case SupplyRegime::DomesticNoVat:
            return notVatPayerNote(czech);
        case SupplyRegime::DomesticVat:
            return {};
    }
    return {};
}

Country countryFromCode(const std::string& isoCode) {
    std::string c;
    for (char ch : isoCode)
        if (!std::isspace(static_cast<unsigned char>(ch)))
            c.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    if (c == "SK") return Country::SK;
    if (c == "CZ") return Country::CZ;
    return Country::Other;
}

std::string codeFor(Country country) {
    switch (country) {
        case Country::SK: return "SK";
        case Country::CZ: return "CZ";
        default:          return "";
    }
}

PeppolParticipant peppolParticipant(const Party& party) {
    PeppolParticipant out;

    // Entered by hand wins. Somebody's provider may have told them an
    // identifier, and a rule that overrides what the user was told is a rule
    // that makes the application wrong on purpose.
    if (!party.endpointId.empty()) {
        out.id     = party.endpointId;
        out.scheme = party.endpointScheme;
    }

    const Country country = countryFromCode(party.address.countryCode);
    if (out.id.empty()) {
        // Slovakia routes on the DIČ. The IČO identifies the company on the
        // document (BT-30) and does not address it on the network.
        if (country == Country::SK) out.id = onlyDigits(party.dic);
        else                        out.id = party.ico;
    }
    if (out.scheme.empty())
        out.scheme = country == Country::SK ? SK_PARTICIPANT_SCHEME
                                            : profileFor(country).schemes.companyId;

    if (out.id.empty()) out.scheme.clear();     // no address at all, not a bare scheme
    return out;
}

const CountryProfile& profileFor(Country country) {
    static const CountryProfile sk = makeSk();
    static const CountryProfile cz = makeCz();
    return country == Country::CZ ? cz : sk;      // SK is the fallback profile
}

bool validCompanyId(Country country, const std::string& id) {
    // The two implementations happen to agree on every input; the dispatch is
    // here so that a future divergence is a one-line change.
    return country == Country::CZ ? cz::validIco(id) : sk::validIco(id);
}

bool validVatId(Country country, const std::string& vatId) {
    return country == Country::CZ ? cz::validDic(vatId) : sk::validIcDph(vatId);
}

bool validTaxId(Country country, const std::string& taxId) {
    // A Czech DIČ *is* the VAT number, so the same check applies with or
    // without the prefix.
    if (country == Country::CZ)
        return cz::validDic(taxId.rfind("CZ", 0) == 0 ? taxId : "CZ" + taxId);
    return sk::validDic(taxId);
}

const InvoiceLabels& labelsFor(Country country) {
    return country == Country::CZ ? CZ_LABELS : SK_LABELS;
}

} // namespace fk

namespace fk {

std::string accountConflict(const std::string& iban, const std::string& localNumber,
                            const std::string& bic) {
    std::string normalisedIban;
    for (char c : iban)
        if (!std::isspace(static_cast<unsigned char>(c)))
            normalisedIban += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    // IBAN against the domestic number. Comparing the parsed forms rather than
    // the strings, so "19-2000145399/0800" and "0019-2000145399/0800" agree.
    if (!normalisedIban.empty() && !localNumber.empty()) {
        const LocalAccount typed  = parseLocalAccount(localNumber);
        const LocalAccount fromIban = parseLocalAccount(localAccountFromIban(normalisedIban));
        if (typed.valid() && fromIban.valid()) {
            // A differing bank code is *not* a conflict. Fio operates in both
            // countries and the same account is addressed 2201730826/8330 in
            // Slovakia and 2201730826/2010 in Czechia. That is the whole point
            // of filling in both fields, and it is what the payment code uses
            // to address the account the way the payer's country does.
            //
            // A differing account number is another matter: those are two
            // accounts, and an invoice carrying both would let a customer pay
            // the wrong one.
            const auto digits = [](const std::string& v) {
                std::string out = v;
                while (out.size() > 1 && out.front() == '0') out.erase(out.begin());
                return out;
            };
            if (digits(typed.number) != digits(fromIban.number) ||
                digits(typed.prefix) != digits(fromIban.prefix))
                return "IBAN a číslo účtu sú dva rôzne účty.";
        }
    }

    // BIC against the IBAN. Characters 5 and 6 of a BIC are its country.
    if (normalisedIban.size() >= 2 && bic.size() >= 6) {
        std::string bicUpper;
        for (char c : bic)
            if (!std::isspace(static_cast<unsigned char>(c)))
                bicUpper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (bicUpper.size() >= 6) {
            const std::string bicCountry  = bicUpper.substr(4, 2);
            const std::string ibanCountry = normalisedIban.substr(0, 2);
            if (bicCountry != ibanCountry)
                return "BIC " + bicUpper + " patrí do krajiny " + bicCountry +
                       ", IBAN do krajiny " + ibanCountry + ".";
        }
    }

    return {};
}

} // namespace fk
