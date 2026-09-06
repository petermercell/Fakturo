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

// Country.h - everything that differs between Slovakia and Czechia in one place.
//
// Adding a third country should mean adding a profile and a validator here,
// not hunting through the GUI for hard-coded "SK".
#pragma once

#include "../core/Dec.h"
#include "../model/Model.h"

#include <string>
#include <vector>

namespace fk {

enum class Country { SK, CZ, Other };

Country     countryFromCode(const std::string& isoCode);
std::string codeFor(Country country);

/// Peppol electronic address schemes, from the official EAS code list.
/// These matter: a wrong scheme routes the invoice to a different country's
/// participant space. 9931 is Estonia, not Slovakia — an easy and costly slip.
struct PeppolSchemes {
    const char* companyId;   // scheme for the company registration number
    const char* vatId;       // scheme for the VAT number
    const char* companyLabel;
    const char* vatLabel;
    /// The scheme this country's Peppol Authority *routes* on, which is not
    /// necessarily either of the two above — in Slovakia it is neither.
    const char* participant;
    const char* participantLabel;
};

struct CountryProfile {
    Country       country       = Country::Other;
    const char*   code          = "SK";
    const char*   currency      = "EUR";
    const char*   vatPrefix     = "SK";
    const char*   paymentSymbol = "0308";   // constant symbol for invoices
    Dec           standardVatRate;
    std::vector<Dec> vatRates;
    PeppolSchemes schemes;
};

/// Where a document is *routed* to — which is not the same thing as who the
/// party is. BT-30 (the legal registration number) stays the IČO under scheme
/// 0158; the endpoint identifier BT-34 / BT-49 is the address the Peppol
/// network resolves, and Slovakia's Peppol Authority mandates exactly one form
/// for it: **0245, the DIČ**. The 9950:SK… VAT form is not accepted, and
/// 0158:IČO — which this application used to write — does not resolve.
///
/// Getting this wrong is invisible on the printed page and fatal on the
/// network: the document is handed to a network that cannot find the
/// recipient.
struct PeppolParticipant {
    std::string scheme;
    std::string id;

    bool valid() const { return !scheme.empty() && !id.empty(); }
    /// "0245:2120345678", the form the delivery services want.
    std::string full() const { return valid() ? scheme + ":" + id : std::string(); }
};

/// The participant identifier for `party`. An identifier the user has entered
/// by hand wins: they may have been told one by their own provider, and
/// second-guessing it would be worse than trusting it.
PeppolParticipant peppolParticipant(const Party& party);

const CountryProfile& profileFor(Country country);
inline const CountryProfile& profileFor(const std::string& isoCode) {
    return profileFor(countryFromCode(isoCode));
}

/// Company registration number (IČO). Both countries use 8 digits with a
/// mod-11 check digit, but the two algorithms differ.
bool validCompanyId(Country country, const std::string& id);
/// VAT identification number, including the country prefix.
bool validVatId(Country country, const std::string& vatId);
/// National tax number (DIČ). In Czechia it is the VAT number without "CZ".
bool validTaxId(Country country, const std::string& taxId);

/// Text on the printed invoice. The UI stays Slovak; the document follows the
/// seller, because a Czech customer receiving a Slovak invoice looks careless.
struct InvoiceLabels {
    const char* invoice;
    const char* creditNote;
    const char* proforma;
    const char* advanceTaxDocument;
    const char* notATaxDocument;
    const char* settlesProformas;
    const char* number;
    const char* issueDate;
    const char* taxPointDate;
    const char* dueDate;
    const char* supplier;
    const char* customer;
    const char* companyId;      // IČO / IČO
    const char* taxId;          // DIČ
    const char* vatId;          // IČ DPH / DIČ (CZ uses DIČ for both)
    const char* notVatRegistered;
    const char* payment;
    const char* iban;
    const char* swift;
    const char* variableSymbol;
    const char* constantSymbol;
    const char* description;
    const char* quantity;
    const char* unit;
    const char* unitPrice;
    const char* vat;
    const char* lineTotal;
    const char* vatRecap;
    const char* rate;
    const char* taxableAmount;
    const char* vatAmount;
    const char* taxExclusive;
    const char* lineSubtotal; ///< sum of the lines, shown only when adjusted
    const char* discount;     ///< fallback when a discount has no reason
    const char* surcharge;
    const char* rounding;     ///< BT-114
    const char* vatInCurrency; ///< "DPH v" — the code follows: "DPH v EUR"
    const char* exchangeRate;  ///< "Kurz"
    const char* prepaid;
    const char* payable;
    const char* creditNoteFor;
    const char* issuedOn;
    const char* orderReference;
    const char* electronicNote;
    const char* scanToPay;
    // Appended, and appended at the end on purpose: InvoiceLabels is filled by
    // aggregate initialisation in two places, so a field inserted in the middle
    // shifts every string after it by one and the invoice starts calling the
    // IČO a DIČ. New labels go here.
    const char* bankAccount;    ///< "Bankový účet" — the heading over IBAN
    const char* paymentMethod;  ///< "Spôsob platby"
    const char* byTransfer;     ///< "Prevodom"
    const char* priceColumn;    ///< "CENA" — the one column heading the new layout keeps
};

/// Slovak and Czech account numbers share one structure: a 4-digit bank code,
/// an optional 6-digit prefix and a 10-digit account number, written locally as
/// "19-2000145399/0800" and internationally as "CZ6508000000192000145399". The
/// conversion is exact in both directions, which is why the form can fill one
/// from the other.
struct LocalAccount {
    std::string prefix;      // up to 6 digits, often empty
    std::string number;      // up to 10 digits
    std::string bankCode;    // 4 digits
    bool valid() const { return !number.empty() && bankCode.size() == 4; }
};

/// Parses "19-2000145399/0800" or "2000145399/0800".
LocalAccount parseLocalAccount(const std::string& text);
/// "19-2000145399/0800" from the parts, without leading zeros.
std::string formatLocalAccount(const LocalAccount& account);

/// Builds the IBAN. Empty when the account or the country is not one we know.
std::string ibanFromLocalAccount(Country country, const LocalAccount& account);
/// The domestic form of an SK or CZ IBAN. Empty for anything else.
std::string localAccountFromIban(const std::string& iban);

/// The two ISO 13616 check digits for a country and BBAN.
std::string ibanCheckDigits(const std::string& countryCode, const std::string& bban);

/// Empty when an account's fields agree with one another; otherwise a sentence
/// naming what does not.
///
/// This exists because two contradictory numbers sat in one row for weeks
/// without anything noticing: a Slovak IBAN beside a Czech domestic number,
/// beside a Slovak BIC. Each field was individually well-formed. The invoice
/// then carried an IBAN pointing at one bank and a domestic number pointing at
/// another, and a customer could pay either.
///
/// Nothing here invents data. It only reports disagreement between fields the
/// user typed, so it cannot itself be the source of a wrong account.
std::string accountConflict(const std::string& iban, const std::string& localNumber,
                            const std::string& bic);

/// EU VAT territory. Greece appears as both GR (ISO) and EL (VAT prefix).
bool isEuMember(const std::string& isoCode);

/// What kind of supply this is, which decides the VAT category, the rate and
/// the sentence that must appear on the invoice.
enum class SupplyRegime {
    DomesticVat,        ///< seller charges its own VAT
    DomesticNoVat,      ///< seller is not a payer — nothing to charge
    ReverseChargeEu,    ///< B2B across an EU border: the customer accounts for it
    ExportOutsideEu     ///< outside the EU: no EU VAT
};

/// Works it out from the two parties. This is the case §7a exists for: a
/// registered non-payer supplying a business in another member state must not
/// charge VAT, but *must* state that the recipient accounts for it, and must
/// show both VAT numbers.
SupplyRegime detectSupplyRegime(const Company& seller, const Party& buyer);

/// UBL VAT category code for the regime.
const char* vatCategoryFor(SupplyRegime regime);
/// The sentence required on the invoice, empty when none is needed.
std::string supplyNoteFor(SupplyRegime regime, Country sellerCountry);

const InvoiceLabels& labelsFor(Country country);
inline const InvoiceLabels& labelsFor(const std::string& isoCode) {
    return labelsFor(countryFromCode(isoCode));
}

} // namespace fk
