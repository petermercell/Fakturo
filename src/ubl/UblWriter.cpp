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

#include "UblWriter.h"

#include "Xml.h"

#include "../country/Country.h"

#include <string>

namespace fk {
namespace {

std::string amt(Dec v)  { return v.roundTo(2).toString(2); }
std::string pct(Dec v)  { return v.roundTo(2).toString(2); }
std::string qty(Dec v)  { return v.roundTo(4).toString(4); }

/// EN 16931 BR-DEC-23 caps the item net price at 2 decimals. When the real
/// price needs more precision we scale it and declare a BaseQuantity, which is
/// the standard UBL escape hatch (e.g. 0.0125 -> price 1.25 per 100 units).
struct ScaledPrice {
    std::string amount;
    std::string baseQuantity;
};

ScaledPrice scalePrice(Dec unitPrice) {
    for (int64_t base : {1, 10, 100, 1000, 10000}) {
        Dec scaled = unitPrice * Dec::fromInt(base);
        if (scaled.roundTo(2) == scaled)
            return { scaled.toString(2), Dec::fromInt(base).toString(0) };
    }
    Dec scaled = unitPrice * Dec::fromInt(10000);
    return { scaled.roundTo(2).toString(2), "10000" };
}

void writeAddress(XmlWriter& w, const Address& a) {
    XmlScope s(w, "cac:PostalAddress");
    w.leaf("cbc:StreetName", a.street);
    w.leaf("cbc:AdditionalStreetName", a.street2);
    w.leaf("cbc:CityName", a.city);
    w.leaf("cbc:PostalZone", a.postalCode);
    XmlScope c(w, "cac:Country");
    w.leafAlways("cbc:IdentificationCode", a.countryCode.empty() ? "SK" : a.countryCode);
}

/// EN 16931 BR-O-11: an invoice whose only VAT category is "O" (not subject to
/// VAT) must not contain a seller, tax representative or buyer VAT identifier.
/// A §7a supplier does hold an IČ DPH, so it has to be left out deliberately.
bool documentIsOutOfScope(const Invoice& inv) {
    const Totals t = inv.totals();
    if (t.vat.empty()) return false;
    for (const VatBreakdownRow& r : t.vat)
        if (r.category != VatCat::OutOfScope) return false;
    return true;
}

void writeParty(XmlWriter& w, const Party& p, bool omitVatId) {
    XmlScope party(w, "cac:Party");

    const CountryProfile& profile = profileFor(p.address.countryCode);
    // BT-34 / BT-49, the address the network routes on — not the same as the
    // registration number below. Slovakia mandates 0245:DIČ for this and
    // resolves nothing else.
    const PeppolParticipant endpoint = peppolParticipant(p);
    if (endpoint.valid())
        w.leafAlways("cbc:EndpointID", endpoint.id, {{"schemeID", endpoint.scheme}});

    {
        XmlScope n(w, "cac:PartyName");
        w.leafAlways("cbc:Name", p.name);
    }

    writeAddress(w, p.address);

    // BT-31 seller VAT identifier. Only emitted for VAT-registered parties,
    // and never on an out-of-scope document (BR-O-11).
    if (p.isVatRegistered() && !omitVatId) {
        XmlScope ts(w, "cac:PartyTaxScheme");
        w.leafAlways("cbc:CompanyID", p.icDph);
        XmlScope sch(w, "cac:TaxScheme");
        w.leafAlways("cbc:ID", "VAT");
    }
    // The Slovak DIC (BT-32) is intentionally NOT written here. Its UBL binding
    // requires a TaxScheme/ID other than "VAT" and the accepted value varies by
    // receiver, so emitting it is a common source of rejections. It still
    // appears on the PDF, which is what Slovak law actually requires.

    {
        XmlScope le(w, "cac:PartyLegalEntity");
        w.leafAlways("cbc:RegistrationName", p.name);
        // 0158 = Slovak ICO, 0154 = Czech ICO. Getting this wrong routes the
        // document into another country's participant space.
        w.leaf("cbc:CompanyID", p.ico, {{"schemeID", profile.schemes.companyId}});
    }

    if (!p.contactName.empty() || !p.phone.empty() || !p.email.empty()) {
        XmlScope ct(w, "cac:Contact");
        w.leaf("cbc:Name", p.contactName);
        w.leaf("cbc:Telephone", p.phone);
        w.leaf("cbc:ElectronicMail", p.email);
    }
}

void writeTaxTotal(XmlWriter& w, const Totals& t, const std::string& cur) {
    XmlScope tt(w, "cac:TaxTotal");
    w.leafAlways("cbc:TaxAmount", amt(t.taxAmount), {{"currencyID", cur}});

    for (const VatBreakdownRow& r : t.vat) {
        XmlScope st(w, "cac:TaxSubtotal");
        w.leafAlways("cbc:TaxableAmount", amt(r.taxableAmount), {{"currencyID", cur}});
        w.leafAlways("cbc:TaxAmount", amt(r.taxAmount), {{"currencyID", cur}});

        XmlScope tc(w, "cac:TaxCategory");
        w.leafAlways("cbc:ID", r.category);
        w.leafAlways("cbc:Percent", pct(r.rate));
        if (r.category != VatCat::Standard && !r.exemptionReason.empty())
            w.leaf("cbc:TaxExemptionReason", r.exemptionReason);
        XmlScope sch(w, "cac:TaxScheme");
        w.leafAlways("cbc:ID", "VAT");
    }
}

/// BG-20 and BG-21. The element order is the UBL schema's, not a preference:
/// ChargeIndicator, reason, factor, amount, base, category.
void writeAllowance(XmlWriter& w, const Allowance& a, const std::string& cur) {
    XmlScope ac(w, "cac:AllowanceCharge");
    w.leafAlways("cbc:ChargeIndicator", a.isCharge ? "true" : "false");
    w.leaf("cbc:AllowanceChargeReason", a.reason);
    // BT-94 / BT-101. Present only when the amount really was worked out from
    // a percentage — BR-CO-05 wants the factor and the base together or not at
    // all, and inventing a base for a flat discount would be a lie.
    if (!a.percentage.isZero()) {
        w.leafAlways("cbc:MultiplierFactorNumeric", pct(a.percentage));
    }
    w.leafAlways("cbc:Amount", amt(a.amount), {{"currencyID", cur}});
    if (!a.percentage.isZero())
        w.leafAlways("cbc:BaseAmount", amt(a.baseAmount), {{"currencyID", cur}});

    // A discount belongs to a VAT category: it lowers that category's taxable
    // base, and the breakdown has to reconcile (BR-32, BR-45).
    XmlScope tc(w, "cac:TaxCategory");
    w.leafAlways("cbc:ID", a.vatCategory);
    w.leafAlways("cbc:Percent", pct(a.vatRate));
    XmlScope sch(w, "cac:TaxScheme");
    w.leafAlways("cbc:ID", "VAT");
}

void writeLine(XmlWriter& w, const InvoiceLine& l, const Invoice& inv) {
    const std::string tag = inv.isCreditNote() ? "cac:CreditNoteLine" : "cac:InvoiceLine";
    const std::string qtyTag = inv.isCreditNote() ? "cbc:CreditedQuantity" : "cbc:InvoicedQuantity";

    XmlScope line(w, tag);
    w.leafAlways("cbc:ID", std::to_string(l.lineNo));
    w.leafAlways(qtyTag, qty(l.quantity), {{"unitCode", l.unitCodeUn}});
    w.leafAlways("cbc:LineExtensionAmount", amt(l.netAmount()), {{"currencyID", inv.currency}});

    // BG-27 / BG-28. Inside the line, and already reflected in the amount
    // above — that is what makes them line allowances rather than document
    // ones. No TaxCategory here: a line allowance takes its line's.
    for (const Allowance& a : l.allowances) {
        XmlScope ac(w, "cac:AllowanceCharge");
        w.leafAlways("cbc:ChargeIndicator", a.isCharge ? "true" : "false");
        w.leaf("cbc:AllowanceChargeReason", a.reason);
        if (!a.percentage.isZero())
            w.leafAlways("cbc:MultiplierFactorNumeric", pct(a.percentage));
        w.leafAlways("cbc:Amount", amt(a.amount), {{"currencyID", inv.currency}});
        if (!a.percentage.isZero())
            w.leafAlways("cbc:BaseAmount", amt(a.baseAmount), {{"currencyID", inv.currency}});
    }

    {
        XmlScope item(w, "cac:Item");
        w.leafAlways("cbc:Name", l.description);
        XmlScope tc(w, "cac:ClassifiedTaxCategory");
        w.leafAlways("cbc:ID", l.vatCategory);
        w.leafAlways("cbc:Percent", pct(l.vatRate));
        XmlScope sch(w, "cac:TaxScheme");
        w.leafAlways("cbc:ID", "VAT");
    }
    {
        ScaledPrice sp = scalePrice(l.unitPrice);
        XmlScope price(w, "cac:Price");
        w.leafAlways("cbc:PriceAmount", sp.amount, {{"currencyID", inv.currency}});
        if (sp.baseQuantity != "1")
            w.leafAlways("cbc:BaseQuantity", sp.baseQuantity, {{"unitCode", l.unitCodeUn}});
    }
}

} // namespace

std::string writeUbl(const Invoice& inv, const Company& seller) {
    // A proforma is not a tax document; Peppol BIS Billing has no code for it
    // (UNCL1001 325 is not in the allowed list). Refusing here means the GUI
    // cannot accidentally put one on the network.
    if (!canExportToPeppol(inv.type)) return {};

    const bool  isCn = inv.isCreditNote();
    const auto  root = isCn ? std::string("CreditNote") : std::string("Invoice");
    const Totals t   = inv.totals();

    XmlWriter w;
    w.open(root, {
        {"xmlns", "urn:oasis:names:specification:ubl:schema:xsd:" + root + "-2"},
        {"xmlns:cac", "urn:oasis:names:specification:ubl:schema:xsd:CommonAggregateComponents-2"},
        {"xmlns:cbc", "urn:oasis:names:specification:ubl:schema:xsd:CommonBasicComponents-2"},
    });

    w.leafAlways("cbc:CustomizationID", peppol::CUSTOMIZATION_ID);
    w.leafAlways("cbc:ProfileID", peppol::PROFILE_ID);
    w.leafAlways("cbc:ID", inv.number);
    w.leafAlways("cbc:IssueDate", inv.issueDate);

    // The element order below is fixed by the UBL 2.1 schema and differs
    // between Invoice and CreditNote. Do not reorder.
    if (isCn) {
        w.leaf("cbc:TaxPointDate", inv.taxPointDate);
        w.leafAlways("cbc:CreditNoteTypeCode", typeCode(inv.type));
        w.leaf("cbc:Note", inv.note);
    } else {
        w.leaf("cbc:DueDate", inv.dueDate);
        w.leafAlways("cbc:InvoiceTypeCode", typeCode(inv.type));
        w.leaf("cbc:Note", inv.note);
        w.leaf("cbc:TaxPointDate", inv.taxPointDate);
    }

    w.leafAlways("cbc:DocumentCurrencyCode", inv.currency);
    // BT-6, immediately after the document currency and before the buyer
    // reference — the schema's order, not a preference. Written only when the
    // totals really carry a restatement: PEPPOL-EN16931-R005 rejects a tax
    // currency equal to the document currency, and BR-53 rejects the code
    // without the amount below.
    if (t.restated) w.leafAlways("cbc:TaxCurrencyCode", inv.vatAccountingCurrency);
    // BR-CO-25: an invoice needs a buyer reference or an order reference.
    w.leaf("cbc:BuyerReference", inv.buyerReference);

    if (!inv.orderReference.empty()) {
        XmlScope s(w, "cac:OrderReference");
        w.leafAlways("cbc:ID", inv.orderReference);
    }
    if (!inv.precedingNumber.empty()) {
        XmlScope s(w, "cac:BillingReference");
        XmlScope r(w, "cac:InvoiceDocumentReference");
        w.leafAlways("cbc:ID", inv.precedingNumber);
        w.leaf("cbc:IssueDate", inv.precedingDate);
    }

    const bool outOfScope = documentIsOutOfScope(inv);
    { XmlScope s(w, "cac:AccountingSupplierParty"); writeParty(w, seller, outOfScope); }
    { XmlScope s(w, "cac:AccountingCustomerParty"); writeParty(w, inv.buyer, outOfScope); }

    if (!seller.iban.empty()) {
        XmlScope pm(w, "cac:PaymentMeans");
        w.leafAlways("cbc:PaymentMeansCode", inv.paymentMeansCode);
        // Slovak variable symbol travels as the remittance information.
        w.leaf("cbc:PaymentID", inv.variableSymbol);
        XmlScope fa(w, "cac:PayeeFinancialAccount");
        w.leafAlways("cbc:ID", seller.iban);
        w.leaf("cbc:Name", seller.name);
        if (!seller.bic.empty()) {
            XmlScope br(w, "cac:FinancialInstitutionBranch");
            w.leafAlways("cbc:ID", seller.bic);
        }
    }

    if (!inv.paymentTerms.empty()) {
        XmlScope s(w, "cac:PaymentTerms");
        w.leafAlways("cbc:Note", inv.paymentTerms);
    }

    // Before TaxTotal, as the schema requires.
    for (const Allowance& a : inv.allowances) writeAllowance(w, a, inv.currency);

    writeTaxTotal(w, t, inv.currency);

    // BT-111, as a second cac:TaxTotal carrying only the amount.
    // PEPPOL-EN16931-R054 counts the subtotal-less TaxTotals and demands
    // exactly one whenever the tax currency code is present — so this element
    // must have no cac:TaxSubtotal, and must not appear without BT-6.
    // R051 makes it the one place in the document where a currencyID other
    // than BT-5 is allowed.
    if (t.restated) {
        XmlScope tt(w, "cac:TaxTotal");
        w.leafAlways("cbc:TaxAmount", amt(t.taxAmountAccounting),
                     {{"currencyID", inv.vatAccountingCurrency}});
    }

    {
        XmlScope s(w, "cac:LegalMonetaryTotal");
        w.leafAlways("cbc:LineExtensionAmount", amt(t.lineExtension), {{"currencyID", inv.currency}});
        w.leafAlways("cbc:TaxExclusiveAmount", amt(t.taxExclusive), {{"currencyID", inv.currency}});
        w.leafAlways("cbc:TaxInclusiveAmount", amt(t.taxInclusive), {{"currencyID", inv.currency}});
        // Order matters: allowance and charge totals come after the inclusive
        // amount and before the prepayment.
        if (!t.allowanceTotal.isZero())
            w.leafAlways("cbc:AllowanceTotalAmount", amt(t.allowanceTotal),
                         {{"currencyID", inv.currency}});
        if (!t.chargeTotal.isZero())
            w.leafAlways("cbc:ChargeTotalAmount", amt(t.chargeTotal),
                         {{"currencyID", inv.currency}});
        if (!t.prepaidAmount.isZero())
            w.leafAlways("cbc:PrepaidAmount", amt(t.prepaidAmount), {{"currencyID", inv.currency}});
        if (!t.rounding.isZero())
            w.leafAlways("cbc:PayableRoundingAmount", amt(t.rounding),
                         {{"currencyID", inv.currency}});
        w.leafAlways("cbc:PayableAmount", amt(t.payable), {{"currencyID", inv.currency}});
    }

    for (const InvoiceLine& l : inv.lines) writeLine(w, l, inv);

    w.close();
    return w.str();
}

} // namespace fk
