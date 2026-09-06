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

#include "Validator.h"

#include "../country/Country.h"
#include "../sk/Slovak.h"

#include <algorithm>

namespace fk {
namespace {

struct Collector {
    ValidationResult r;
    void err(const char* code, std::string msg)  { r.issues.push_back({Severity::Error, code, std::move(msg)}); }
    void warn(const char* code, std::string msg) { r.issues.push_back({Severity::Warning, code, std::move(msg)}); }
};

bool isKnownCategory(const std::string& c) {
    return c == VatCat::Standard || c == VatCat::ZeroRated || c == VatCat::Exempt ||
           c == VatCat::ReverseCharge || c == VatCat::IntraCommunity ||
           c == VatCat::Export || c == VatCat::OutOfScope;
}

} // namespace

int ValidationResult::errorCount() const {
    return static_cast<int>(std::count_if(issues.begin(), issues.end(),
        [](const Issue& i) { return i.severity == Severity::Error; }));
}

int ValidationResult::warningCount() const {
    return static_cast<int>(std::count_if(issues.begin(), issues.end(),
        [](const Issue& i) { return i.severity == Severity::Warning; }));
}

ValidationResult validate(const Invoice& inv, const Company& seller) {
    Collector c;
    const Totals t = inv.totals();

    // ---------------------------------------------------------- document head
    if (inv.number.empty())
        c.err("BR-02", "Faktura musi mat cislo.");
    if (!sk::validIsoDate(inv.issueDate))
        c.err("BR-03", "Chyba alebo je neplatny datum vystavenia.");
    if (!inv.taxPointDate.empty() && !sk::validIsoDate(inv.taxPointDate))
        c.err("BR-CO-03", "Datum dodania nie je platny datum.");
    // Three upper-case letters, not merely three characters: "eur" is not an
    // ISO 4217 code, and code that compares currencies would not treat it as
    // one either (BR-CL-06).
    if (inv.currency.size() != 3 ||
        !std::all_of(inv.currency.begin(), inv.currency.end(),
                     [](char ch) { return ch >= 'A' && ch <= 'Z'; }))
        c.err("BR-05", "Mena musi byt trojpismenovy ISO 4217 kod velkymi pismenami (napr. EUR).");
    if (inv.lines.empty())
        c.err("BR-16", "Faktura musi mat aspon jeden riadok.");

    // BR-CO-25: with a positive amount due, either a due date or payment terms.
    if (t.payable > Dec() && inv.dueDate.empty() && inv.paymentTerms.empty())
        c.err("BR-CO-25", "Uvedte datum splatnosti alebo platobne podmienky.");
    if (!inv.dueDate.empty()) {
        if (!sk::validIsoDate(inv.dueDate))
            c.err("BR-CO-25", "Datum splatnosti nie je platny datum.");
        else if (sk::validIsoDate(inv.issueDate) && inv.dueDate < inv.issueDate)
            c.warn("SK-DUE", "Datum splatnosti je skorsi ako datum vystavenia.");
    }
    if (inv.isCreditNote() && inv.precedingNumber.empty())
        c.err("BR-55", "Dobropis musi odkazovat na cislo povodnej faktury.");

    // --------------------------------------------------------------- supplier
    if (seller.name.empty())
        c.err("BR-06", "Chyba obchodne meno dodavatela.");
    if (seller.address.city.empty() || seller.address.countryCode.empty())
        c.err("BR-08", "Adresa dodavatela musi obsahovat mesto a krajinu.");
    if (seller.ico.empty() && seller.icDph.empty())
        c.err("BR-CO-26", "Dodavatel musi mat uvedene ICO alebo IC DPH.");
    const Country sellerCountry = countryFromCode(seller.address.countryCode);
    if (!seller.ico.empty() && !validCompanyId(sellerCountry, seller.ico))
        c.warn("SK-ICO", "ICO dodavatela nepreslo kontrolou kontrolnej cislice.");
    if (!seller.icDph.empty() && !validVatId(sellerCountry, seller.icDph))
        c.warn("SK-ICDPH", "IC DPH / DIC dodavatela nema platny format.");
    if (!seller.dic.empty() && !validTaxId(sellerCountry, seller.dic))
        c.warn("SK-DIC", "DIC dodavatela nema platny format.");
    // The routing address, not the registration number. A Slovak party without
    // a DIČ has no address on the network at all, however complete the rest of
    // its details are.
    if (!peppolParticipant(seller).valid())
        c.err("PEPPOL-01", sellerCountry == Country::SK
                               ? "Chyba Peppol identifikator dodavatela: pre SR je to DIC."
                               : "Chyba Peppol identifikator dodavatela.");

    // ----------------------------------------------------------------- buyer
    if (inv.buyer.name.empty())
        c.err("BR-07", "Chyba obchodne meno odberatela.");
    if (inv.buyer.address.city.empty() || inv.buyer.address.countryCode.empty())
        c.err("BR-10", "Adresa odberatela musi obsahovat mesto a krajinu.");
    const Country buyerCountry = countryFromCode(inv.buyer.address.countryCode);
    if (!peppolParticipant(inv.buyer).valid())
        c.err("PEPPOL-02", buyerCountry == Country::SK
                               ? "Chyba Peppol identifikator odberatela: pre SR je to DIC."
                               : "Chyba Peppol identifikator odberatela.");
    if (!inv.buyer.ico.empty() && !validCompanyId(buyerCountry, inv.buyer.ico))
        c.warn("SK-ICO-B", "ICO odberatela nepreslo kontrolou kontrolnej cislice.");
    if (!inv.buyer.icDph.empty() && !validVatId(buyerCountry, inv.buyer.icDph))
        c.warn("SK-ICDPH-B", "IC DPH / DIC odberatela nema platny format.");

    // BR-CO-25 companion: Peppol wants a buyer reference or an order reference.
    if (inv.buyerReference.empty() && inv.orderReference.empty())
        c.warn("PEPPOL-EN16931-R003",
               "Odporuca sa vyplnit referenciu odberatela alebo cislo objednavky.");

    // ----------------------------------------------------------------- lines
    int n = 0;
    for (const InvoiceLine& l : inv.lines) {
        ++n;
        const std::string at = " (riadok " + std::to_string(n) + ")";
        if (l.description.empty())
            c.err("BR-25", "Chyba nazov polozky" + at + ".");
        if (l.quantity.isZero())
            c.warn("BR-22", "Mnozstvo je nulove" + at + ".");
        if (!isKnownCategory(l.vatCategory))
            c.err("BR-CO-04", "Neznamy kod kategorie DPH '" + l.vatCategory + "'" + at + ".");
        if (l.vatRate.isNegative())
            c.err("BR-CO-17", "Zaporna sadzba DPH" + at + ".");
        if (l.vatCategory == VatCat::Standard && l.vatRate.isZero())
            c.err("BR-S-05", "Zakladna sadzba DPH nemoze byt 0 %" + at + ".");
        if (l.vatCategory != VatCat::Standard && !l.vatRate.isZero())
            c.err("BR-Z-05", "Pri kategorii '" + l.vatCategory + "' musi byt sadzba 0 %" + at + ".");
    }

    // --------------------------------------------------------- VAT and totals
    bool needsReason = false;
    for (const VatBreakdownRow& r : t.vat)
        if (r.category != VatCat::Standard) needsReason = true;

    if (needsReason && inv.vatExemptionReason.empty())
        c.err("BR-E-10", "Pri oslobodeni od DPH alebo preneseni danovej povinnosti "
                         "musi byt uvedeny dovod (napr. 'Prenesenie danovej povinnosti').");

    // §7 and §7a subjects hold an IC DPH but are not platitelia: the regime
    // decides, not the presence of a number.
    if (!seller.chargesVat()) {
        const bool chargesVat = std::any_of(t.vat.begin(), t.vat.end(),
            [](const VatBreakdownRow& r) { return !r.taxAmount.isZero(); });
        if (chargesVat)
            c.err("SK-NONVAT",
                  seller.vatMode == VatMode::RegisteredNotPayer
                      ? "Dodavatel je registrovany podla §7/§7a, nie je platcom DPH "
                        "a nesmie uctovat slovensku DPH."
                      : "Dodavatel nie je platca DPH, ale faktura uctuje DPH.");
    }

    // BR-AE-2/3: reverse charge is only valid when both parties are identified
    // for VAT. This is the rule a §7a supplier depends on, and the one that
    // silently invalidates the invoice when the customer's VAT id is missing.
    const bool hasReverseCharge =
        std::any_of(t.vat.begin(), t.vat.end(),
            [](const VatBreakdownRow& r) { return r.category == VatCat::ReverseCharge; });
    if (hasReverseCharge) {
        if (seller.icDph.empty())
            c.err("BR-AE-2", "Pri preneseni danovej povinnosti musi mat dodavatel IC DPH.");
        if (inv.buyer.icDph.empty())
            c.err("BR-AE-3", "Pri preneseni danovej povinnosti musi mat odberatel IC DPH. "
                             "Overte ho vo VIES.");
        if (inv.vatExemptionReason.empty())
            c.err("BR-AE-10", "Doklad musi obsahovat text o preneseni danovej povinnosti "
                              "(reverse charge).");
    }

    // BR-O-11: a document using category O must carry no VAT identifiers at all.
    const bool onlyOutOfScope =
        !t.vat.empty() && std::all_of(t.vat.begin(), t.vat.end(),
            [](const VatBreakdownRow& r) { return r.category == VatCat::OutOfScope; });
    if (onlyOutOfScope && (!seller.icDph.empty() || !inv.buyer.icDph.empty()))
        c.warn("BR-O-11",
               "Pri kategorii 'O' sa IC DPH neuvadza; z exportu bude vynechane.");

    // BR-CO-13 / BR-CO-15: the arithmetic must close.
    Dec sumLines;
    for (const InvoiceLine& l : inv.lines) sumLines += l.netAmount();
    if (sumLines.roundTo(2) != t.lineExtension.roundTo(2))
        c.err("BR-CO-10", "Sucet riadkov nesedi so zakladom dane.");
    if ((t.taxExclusive + t.taxAmount).roundTo(2) != t.taxInclusive.roundTo(2))
        c.err("BR-CO-15", "Suma s DPH sa nerovna zakladu dane plus DPH.");
    // A document-level discount reduces the taxable base of its VAT category.
    // Take more off than that category holds — or name a category no line
    // uses — and the base goes negative, which is arithmetically consistent
    // and rejected by most receivers.
    for (const VatBreakdownRow& r : t.vat)
        if (r.taxableAmount.isNegative())
            c.err("BR-45", "Zľava je vyššia než základ pre sadzbu " +
                           r.rate.toString(0) + " % (" + r.category + ").");

    // BT-115 = BT-112 − BT-113 + BT-114. The rounding term is not optional:
    // leaving it out made every rounded invoice fail its own validation, so
    // the button that offers rounding produced documents the app refused.
    if (t.payable.roundTo(2) !=
        (t.taxInclusive - t.prepaidAmount + t.rounding).roundTo(2))
        c.err("BR-CO-16", "Suma na uhradu nesedi.");

    // ------------------------------------------------ foreign currency (BT-6)
    // A Slovak payer invoicing in crowns still owes euro VAT, and § 74 ods. 1
    // písm. i) makes the euro figure a required particular of the invoice. The
    // app cannot supply it without a rate, so this is an error rather than a
    // warning — and it only fires when there is VAT to restate, which keeps it
    // out of the way of a non-payer invoicing abroad.
    //
    // Only for a seller in a country whose rules this application actually
    // knows. profileFor() falls back to the Slovak profile for anything else,
    // so without this guard a Hungarian seller invoicing in forints would be
    // told to restate their VAT into euro.
    const std::string accounting =
        sellerCountry == Country::Other ? std::string() : profileFor(sellerCountry).currency;
    const bool foreign = !accounting.empty() &&
                         normalisedCurrency(inv.currency) != normalisedCurrency(accounting);
    // A proforma is a request for payment, not a tax document: it declares no
    // VAT, has no tax point, and there is nothing for § 26 to hang a rate on.
    if (foreign && isTaxDocument(inv.type) && !t.taxAmount.isZero()) {
        if (!t.restated) {
            c.err("SK-KURZ", "Faktura je v cudzej mene: uvedte kurz, aby sa DPH dala "
                             "uviest aj v " + accounting + ".");
        } else if (!sk::validIsoDate(inv.exchangeRateDate)) {
            c.warn("SK-KURZ2", "Ku kurzu chyba datum. Podla § 26 sa pouzije kurz ECB "
                               "zo dna predchadzajuceho dnu dodania.");
        } else {
            // The rate belongs to the tax point. A rate published *after* it
            // cannot be the right one, and one from a week earlier is a
            // forgotten field rather than a weekend. The day of the tax point
            // itself is allowed: that is the customs rate some payers elect.
            const std::string taxPoint =
                inv.taxPointDate.empty() ? inv.issueDate : inv.taxPointDate;
            if (sk::validIsoDate(taxPoint) &&
                (inv.exchangeRateDate > taxPoint ||
                 inv.exchangeRateDate < sk::addDays(taxPoint, -7)))
                c.warn("SK-KURZ3", "Datum kurzu nesedi s datumom dodania. Podla § 26 ide "
                                   "o kurz zo dna predchadzajuceho dnu dodania.");
        }
    }
    // BT-6 must differ from BT-5 (PEPPOL-EN16931-R005). It never reaches the
    // XML when they are equal, but a document carrying a rate that will be
    // silently ignored is worth saying out loud.
    if (!inv.vatAccountingCurrency.empty() &&
        normalisedCurrency(inv.vatAccountingCurrency) == normalisedCurrency(inv.currency))
        c.warn("R005", "Mena pre DPH je rovnaka ako mena faktury; prepocet sa neuvedie.");

    // ------------------------------------------------------------- SK payment
    if (!seller.iban.empty() && !sk::validIban(seller.iban))
        c.err("SK-IBAN", "IBAN dodavatela nepreslo kontrolou (mod-97).");
    if (seller.iban.empty() && t.payable > Dec())
        c.warn("SK-NOIBAN", "Nie je uvedeny IBAN, odberatel nema kam zaplatit.");
    if (inv.variableSymbol.size() > 10)
        c.err("SK-VS", "Variabilny symbol moze mat najviac 10 cislic.");
    if (!inv.variableSymbol.empty() &&
        !std::all_of(inv.variableSymbol.begin(), inv.variableSymbol.end(),
                     [](char ch) { return ch >= '0' && ch <= '9'; }))
        c.err("SK-VS2", "Variabilny symbol smie obsahovat iba cislice.");

    return c.r;
}

} // namespace fk
