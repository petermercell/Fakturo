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

#include "UblReader.h"

#include "../core/Xml.h"
#include "../country/Country.h"
#include "../model/Units.h"

#include <algorithm>

namespace fk {
namespace {

/// Numbers out of a document somebody else wrote.
///
/// A figure that cannot be read is **not** quietly zero. Zero is a lie that
/// survives the arithmetic: an invoice whose every amount is out of range
/// reads as an invoice for nothing, and nothing for nothing balances
/// perfectly. Each failure is recorded instead, and the reader reports them.
class Numbers {
public:
    explicit Numbers(std::vector<std::string>& problems) : problems_(problems) {}

    Dec at(const XmlNode& node, const std::string& path) {
        const XmlNode* found = node.find(path);
        if (!found) return Dec();               // absent is genuinely zero
        return read(found->text, path);
    }

    Dec read(const std::string& text, const std::string& what) {
        if (text.find_first_not_of(" \t\r\n") == std::string::npos) return Dec();
        const std::optional<Dec> parsed = Dec::parse(text);
        if (parsed) return *parsed;
        problems_.push_back("Údaj " + what + " sa nedá prečítať ako číslo: „" + text + "“.");
        return Dec();
    }

private:
    std::vector<std::string>& problems_;
};

std::string moneyText(const Dec& v) { return v.toString(2); }

/// xsd:boolean is "true", "false", "1" or "0". Reading only the words turns a
/// charge written as 1 into a discount, which is the whole amount wrong twice
/// over.
bool xmlBool(const std::string& text) { return text == "true" || text == "1"; }

void readAddress(const XmlNode& party, Address& out) {
    const XmlNode* a = party.find("PostalAddress");
    if (!a) return;
    out.street      = a->textAt("StreetName");
    out.street2     = a->textAt("AdditionalStreetName");
    out.city        = a->textAt("CityName");
    out.postalCode  = a->textAt("PostalZone");
    const std::string country = a->textAt("Country/IdentificationCode");
    if (!country.empty()) out.countryCode = country;
    // Some senders put the street in AddressLine/Line instead of StreetName.
    if (out.street.empty()) out.street = a->textAt("AddressLine/Line");
}

void readParty(const XmlNode& wrapper, Party& out) {
    const XmlNode* p = wrapper.find("Party");
    if (!p) return;

    // BT-27 is the *legal* name — cac:PartyLegalEntity/cbc:RegistrationName —
    // and it is the mandatory one. cac:PartyName/cbc:Name is BT-28, the
    // trading name, and is optional. Preferring the trading name files
    // "SupplierTradingName Ltd." in the accounting record of a company whose
    // actual name is "SupplierOfficialName Ltd", which is what OpenPeppol's
    // own example document is named to demonstrate.
    out.name = p->textAt("PartyLegalEntity/RegistrationName");
    if (out.name.empty()) out.name = p->textAt("PartyName/Name");

    out.ico = p->textAt("PartyLegalEntity/CompanyID");
    readAddress(*p, out.address);

    // BT-31 / BT-48. The VAT identifier is the PartyTaxScheme whose scheme is
    // VAT; a party can carry others (FC for a Slovak DIČ, for instance), and
    // taking the first one blindly would file a tax number as a VAT number.
    for (const XmlNode* ts : p->childrenNamed("PartyTaxScheme")) {
        const std::string scheme = ts->textAt("TaxScheme/ID");
        if (scheme == "VAT") out.icDph = ts->textAt("CompanyID");
        else if (out.dic.empty())  out.dic = ts->textAt("CompanyID");
    }

    if (const XmlNode* ep = p->find("EndpointID")) {
        out.endpointId     = ep->text;
        out.endpointScheme = ep->attribute("schemeID");
    }

    out.contactName = p->textAt("Contact/Name");
    out.phone       = p->textAt("Contact/Telephone");
    out.email       = p->textAt("Contact/ElectronicMail");
}

/// One allowance or charge. `withCategory` is false for a line-level one:
/// BG-27 and BG-28 carry no VAT category of their own, they take the line's.
Allowance readAllowance(const XmlNode& ac, bool withCategory, Numbers& n) {
    Allowance a;
    a.isCharge   = xmlBool(ac.textAt("ChargeIndicator"));
    a.reason     = ac.textAt("AllowanceChargeReason");
    // BT-98 / BT-105. A sender may give only the coded reason, and an
    // allowance that arrives with no reason at all reads as unexplained money.
    if (a.reason.empty()) a.reason = ac.textAt("AllowanceChargeReasonCode");
    a.percentage = n.at(ac, "MultiplierFactorNumeric");
    a.amount     = n.at(ac, "Amount");
    a.baseAmount = n.at(ac, "BaseAmount");
    if (withCategory) {
        a.vatCategory = ac.textAt("TaxCategory/ID");
        a.vatRate     = n.at(ac, "TaxCategory/Percent");
    }
    return a;
}

void readLine(const XmlNode& node, bool isCreditNote, InvoiceLine& out, Country country,
              Numbers& n, std::vector<std::string>& problems) {
    const std::string qtyTag = isCreditNote ? "CreditedQuantity" : "InvoicedQuantity";

    // A line identifier is free text in practice — "1", "A1", "L-001" — so a
    // number is used when there is one and the position otherwise. Clamped
    // rather than cast: converting an out-of-range double to int is undefined
    // behaviour, and Dec::parse accepts values far past what an int holds.
    const std::optional<Dec> id = Dec::parse(node.textAt("ID"));
    out.lineNo = 0;
    if (id) {
        const Dec whole = id->roundTo(0);
        if (whole > Dec() && whole < Dec::fromInt(1000000))
            out.lineNo = static_cast<int>(whole.raw() / Dec::SCALE);
    }

    // Absent quantity is zero, not the model's default of one. A line missing
    // BT-129 must fail its reconciliation rather than quietly become "one unit
    // at the stated price".
    out.quantity = Dec();
    if (const XmlNode* q = node.find(qtyTag)) {
        out.quantity = n.read(q->text, qtyTag);
        const std::string code = q->attribute("unitCode");
        if (!code.empty()) {
            out.unitCodeUn = code;
            out.unit       = unitLabel(code, country);
        }
    } else {
        problems.push_back("Riadok bez množstva (" + qtyTag + ").");
    }

    out.description = node.textAt("Item/Name");
    if (out.description.empty()) out.description = node.textAt("Item/Description");
    out.vatCategory = node.textAt("Item/ClassifiedTaxCategory/ID");
    out.vatRate     = n.at(node, "Item/ClassifiedTaxCategory/Percent");

    // BT-146 with BT-149: the price may be quoted per N units. Ten items at
    // "200 per 2" is 1000, not 2000 — the classic trap in this element, and
    // one of Peppol's own examples does exactly that.
    Dec price = n.at(node, "Price/PriceAmount");
    if (const XmlNode* base = node.find("Price/BaseQuantity")) {
        const Dec baseQty = n.read(base->text, "Price/BaseQuantity");
        // BR-65: the base quantity's unit must be the line's unit. A sender
        // who disagrees is quoting a price this reader cannot safely divide.
        const std::string baseUnit = base->attribute("unitCode");
        if (!baseUnit.empty() && !out.unitCodeUn.empty() && baseUnit != out.unitCodeUn)
            problems.push_back("Riadok: jednotka základného množstva (" + baseUnit +
                               ") sa líši od jednotky riadku (" + out.unitCodeUn + ").");
        if (!baseQty.isZero()) {
            const Dec each = price / baseQty;
            // Six decimal places is all a price has here. 100 over 3 is not
            // one of them, and the line will not reconcile — better to say so
            // than to let the arithmetic look like the sender's mistake.
            if ((each * baseQty).roundTo(2) != price.roundTo(2))
                problems.push_back("Riadok: cena " + moneyText(price) + " za " +
                                   baseQty.toString(2) + " jednotiek sa nedá presne rozdeliť.");
            price = each;
        }
    }
    out.unitPrice = price;

    // Only the line's *own* allowances. The one that may sit inside cac:Price
    // is a different thing entirely — it is the difference between the gross
    // and net price, already inside PriceAmount — so reading the line's direct
    // children rather than searching the subtree is load-bearing here.
    for (const XmlNode* ac : node.childrenNamed("AllowanceCharge"))
        out.allowances.push_back(readAllowance(*ac, false, n));
}

DocType typeFromCode(const std::string& code, bool isCreditNote) {
    if (isCreditNote) return DocType::CreditNote;
    if (code == "386") return DocType::AdvanceTaxDocument;
    if (code == "381") return DocType::CreditNote;
    return DocType::Invoice;
}

/// Reports a figure the document states that this application does not arrive
/// at from the same parts.
void compare(std::vector<std::string>& out, const std::string& what,
             const Dec& stated, const Dec& computed) {
    if (stated.roundTo(2) == computed.roundTo(2)) return;
    out.push_back(what + ": doklad uvádza " + moneyText(stated) +
                  ", z položiek vychádza " + moneyText(computed));
}

} // namespace

ReadResult readUbl(const std::string& xml) {
    ReadResult r;

    const XmlDocument doc = parseXml(xml);
    if (!doc.ok) {
        r.error = "XML sa nedá prečítať: " + doc.error;
        return r;
    }

    const XmlNode& root = doc.root;
    const bool isCreditNote = root.name == "CreditNote";
    if (root.name != "Invoice" && !isCreditNote) {
        r.error = "Neznámy koreňový prvok: " + root.name;
        return r;
    }

    Invoice& inv = r.invoice;
    Numbers  n(r.discrepancies);

    inv.number       = root.textAt("ID");
    inv.issueDate    = root.textAt("IssueDate");
    inv.dueDate      = root.textAt("DueDate");
    inv.currency     = normalisedCurrency(root.textAt("DocumentCurrencyCode"));
    if (inv.currency.empty()) inv.currency = "EUR";
    inv.type = typeFromCode(root.textAt(isCreditNote ? "CreditNoteTypeCode" : "InvoiceTypeCode"),
                            isCreditNote);

    // BT-22 is repeatable and senders use it that way — terms on one, delivery
    // instructions on the next. Keeping only the first drops the rest of what
    // was said.
    for (const XmlNode* note : root.childrenNamed("Note")) {
        if (note->text.empty()) continue;
        if (!inv.note.empty()) inv.note += "\n";
        inv.note += note->text;
    }

    // The supply date. Peppol BIS discourages cbc:TaxPointDate and steers
    // senders to BT-72 or the invoicing period instead, so the element this
    // application writes is the one a received document is least likely to
    // have — and "dátum dodania" is a required particular of a Slovak invoice.
    inv.taxPointDate = root.textAt("TaxPointDate");
    if (inv.taxPointDate.empty())
        inv.taxPointDate = root.textAt("Delivery/ActualDeliveryDate");
    if (inv.taxPointDate.empty())
        inv.taxPointDate = root.textAt("InvoicePeriod/EndDate");

    inv.buyerReference = root.textAt("BuyerReference");
    inv.orderReference = root.textAt("OrderReference/ID");
    inv.precedingNumber = root.textAt("BillingReference/InvoiceDocumentReference/ID");
    inv.precedingDate   = root.textAt("BillingReference/InvoiceDocumentReference/IssueDate");

    // BT-6. There is no exchange rate anywhere in EN 16931 — the standard
    // carries the restated amount and not how it was arrived at — so a
    // document read here has a tax currency but no rate, and recomputing its
    // BT-111 is not possible. The stated figure is kept as stated.
    inv.vatAccountingCurrency = normalisedCurrency(root.textAt("TaxCurrencyCode"));
    if (!inv.vatAccountingCurrency.empty() && inv.vatAccountingCurrency == inv.currency) {
        // PEPPOL-EN16931-R005 forbids this, and carrying it would make the
        // model claim a restatement into the currency it is already in.
        r.discrepancies.push_back("Mena pre DPH je rovnaká ako mena dokladu; ignoruje sa.");
        inv.vatAccountingCurrency.clear();
    }

    if (const XmlNode* supplier = root.find("AccountingSupplierParty"))
        readParty(*supplier, inv.seller);
    if (const XmlNode* customer = root.find("AccountingCustomerParty"))
        readParty(*customer, inv.buyer);
    // Whether the sender charges VAT, as far as *this document* is concerned.
    // Elsewhere in this application the VAT mode is an explicit setting,
    // precisely because deriving it from "has an IČ DPH" mislabels a § 7a
    // registrant — but a received invoice tells us nothing else, and here it
    // decides only how the document is displayed, not how one is issued.
    inv.seller.vatMode = inv.seller.icDph.empty() ? VatMode::NotRegistered : VatMode::Payer;

    // The first payment means that actually names an account. Senders list
    // several — a card and a transfer, or one account per currency — and the
    // account number is the field somebody pays money into, so picking the
    // first element regardless can lose it entirely.
    {
        const std::vector<const XmlNode*> means = root.childrenNamed("PaymentMeans");
        const XmlNode* chosen = nullptr;
        for (const XmlNode* pm : means)
            if (pm->find("PayeeFinancialAccount/ID")) { chosen = pm; break; }
        if (!chosen && !means.empty()) chosen = means.front();
        if (chosen) {
            inv.paymentMeansCode = chosen->textAt("PaymentMeansCode");
            inv.variableSymbol   = chosen->textAt("PaymentID");
            inv.seller.iban      = chosen->textAt("PayeeFinancialAccount/ID");
            inv.seller.bic       = chosen->textAt(
                "PayeeFinancialAccount/FinancialInstitutionBranch/ID");
            inv.seller.bankName  = chosen->textAt("PayeeFinancialAccount/Name");
        }
        if (means.size() > 1)
            r.discrepancies.push_back("Doklad uvádza viac spôsobov platby; použil sa jeden.");
    }
    inv.paymentTerms = root.textAt("PaymentTerms/Note");

    const Country sellerCountry = countryFromCode(inv.seller.address.countryCode);

    // Document-level allowances and charges, which carry their own VAT
    // category (BG-20 / BG-21).
    for (const XmlNode* ac : root.childrenNamed("AllowanceCharge"))
        inv.allowances.push_back(readAllowance(*ac, true, n));

    const std::string lineTag = isCreditNote ? "CreditNoteLine" : "InvoiceLine";
    for (const XmlNode* line : root.childrenNamed(lineTag)) {
        InvoiceLine l;
        readLine(*line, isCreditNote, l, sellerCountry, n, r.discrepancies);
        inv.lines.push_back(std::move(l));
    }
    if (inv.lines.empty()) r.discrepancies.push_back("Doklad nemá ani jednu položku (BR-16).");
    // Renumbered only when the document did not number its own lines: the
    // sender's numbering is part of what was sent.
    int seen = 0;
    for (InvoiceLine& l : inv.lines) {
        ++seen;
        if (l.lineNo <= 0) l.lineNo = seen;
    }

    // ------------------------------------------------------ the stated totals
    if (const XmlNode* lmt = root.find("LegalMonetaryTotal")) {
        r.stated.lineExtension  = n.at(*lmt, "LineExtensionAmount");
        r.stated.taxExclusive   = n.at(*lmt, "TaxExclusiveAmount");
        r.stated.taxInclusive   = n.at(*lmt, "TaxInclusiveAmount");
        r.stated.allowanceTotal = n.at(*lmt, "AllowanceTotalAmount");
        r.stated.chargeTotal    = n.at(*lmt, "ChargeTotalAmount");
        r.stated.prepaidAmount  = n.at(*lmt, "PrepaidAmount");
        r.stated.rounding       = n.at(*lmt, "PayableRoundingAmount");
        r.stated.payable        = n.at(*lmt, "PayableAmount");
        inv.prepaidAmount  = r.stated.prepaidAmount;
        inv.roundingAmount = r.stated.rounding;
    } else {
        r.discrepancies.push_back("Doklad nemá súhrn súm (LegalMonetaryTotal).");
    }

    // Both tax totals: the one with the breakdown is BT-110, and a second one
    // without a breakdown is BT-111 in the accounting currency. They are told
    // apart by their shape, exactly as PEPPOL-EN16931-R054 defines them.
    bool sawBreakdown = false;
    for (const XmlNode* tt : root.childrenNamed("TaxTotal")) {
        const std::vector<const XmlNode*> subtotals = tt->childrenNamed("TaxSubtotal");
        const XmlNode* amount = tt->find("TaxAmount");
        if (subtotals.empty()) {
            if (!amount) continue;
            const std::string cur = normalisedCurrency(amount->attribute("currencyID"));
            // A subtotal-less tax total in the document's own currency is not
            // BT-111 — it is a document with no VAT breakdown. Treating it as
            // a restatement would invent a tax accounting currency equal to
            // BT-5, which R005 forbids and the model documents as impossible.
            if (cur.empty() || cur == inv.currency) {
                r.discrepancies.push_back(
                    "Doklad má súčet DPH bez rozpisu sadzieb (BG-23 chýba).");
                continue;
            }
            r.stated.restated            = true;
            r.stated.taxAmountAccounting = n.read(amount->text, "TaxAmount (BT-111)");
            if (inv.vatAccountingCurrency.empty()) inv.vatAccountingCurrency = cur;
            continue;
        }
        if (sawBreakdown) {
            r.discrepancies.push_back("Doklad má viac ako jeden rozpis DPH; použil sa prvý.");
            continue;
        }
        sawBreakdown = true;
        if (amount) r.stated.taxAmount = n.read(amount->text, "TaxAmount (BT-110)");
        for (const XmlNode* st : subtotals) {
            VatBreakdownRow row;
            row.taxableAmount   = n.at(*st, "TaxableAmount");
            row.taxAmount       = n.at(*st, "TaxAmount");
            row.category        = st->textAt("TaxCategory/ID");
            row.rate            = n.at(*st, "TaxCategory/Percent");
            row.exemptionReason = st->textAt("TaxCategory/TaxExemptionReason");
            r.stated.vat.push_back(std::move(row));
        }
    }
    if (!sawBreakdown)
        r.discrepancies.push_back("Doklad neuvádza rozpis DPH (cac:TaxTotal).");
    // BT-120 lives on the breakdown; the model carries one per document, which
    // is what the printed page shows.
    for (const VatBreakdownRow& row : r.stated.vat)
        if (!row.exemptionReason.empty()) { inv.vatExemptionReason = row.exemptionReason; break; }

    // ------------------------------------------------------ does it add up?
    const Totals computed = inv.totals();
    compare(r.discrepancies, "Súčet riadkov (BT-106)", r.stated.lineExtension,
            computed.lineExtension);
    compare(r.discrepancies, "Zľavy spolu (BT-107)", r.stated.allowanceTotal,
            computed.allowanceTotal);
    compare(r.discrepancies, "Príplatky spolu (BT-108)", r.stated.chargeTotal,
            computed.chargeTotal);
    compare(r.discrepancies, "Základ dane (BT-109)", r.stated.taxExclusive,
            computed.taxExclusive);
    compare(r.discrepancies, "DPH spolu (BT-110)", r.stated.taxAmount, computed.taxAmount);
    compare(r.discrepancies, "Suma s DPH (BT-112)", r.stated.taxInclusive,
            computed.taxInclusive);
    compare(r.discrepancies, "Na úhradu (BT-115)", r.stated.payable, computed.payable);

    // The VAT breakdown, which is what a tax authority reads and what the
    // totals above can agree on while it is wrong. Three separate ways for it
    // to be untrue, so three checks.
    {
        // BR-CO-14: BT-110 is the sum of the categories' own tax amounts.
        Dec sumOfRows;
        for (const VatBreakdownRow& row : r.stated.vat) sumOfRows += row.taxAmount;
        compare(r.discrepancies, "Súčet sadzieb DPH (BR-CO-14)", r.stated.taxAmount, sumOfRows);

        for (const VatBreakdownRow& row : r.stated.vat) {
            const std::string label = "Sadzba " + row.rate.toString(0) + " % (" + row.category + ")";
            // Each category's tax is its own base at its own rate.
            compare(r.discrepancies, label + ": DPH", row.taxAmount,
                    row.taxableAmount.percentOf(row.rate));

            // And the base is the one the lines and document adjustments
            // actually produce for that category.
            const auto ours = std::find_if(computed.vat.begin(), computed.vat.end(),
                [&row](const VatBreakdownRow& c) {
                    return c.category == row.category && c.rate == row.rate;
                });
            if (ours == computed.vat.end()) {
                r.discrepancies.push_back(label + ": doklad uvádza sadzbu, ktorú žiadny "
                                                  "riadok nepoužíva");
                continue;
            }
            compare(r.discrepancies, label + ": základ", row.taxableAmount, ours->taxableAmount);
        }
        // The other direction: a category on the lines that the breakdown does
        // not mention at all.
        for (const VatBreakdownRow& row : computed.vat) {
            const auto theirs = std::find_if(r.stated.vat.begin(), r.stated.vat.end(),
                [&row](const VatBreakdownRow& s) {
                    return s.category == row.category && s.rate == row.rate;
                });
            if (theirs == r.stated.vat.end())
                r.discrepancies.push_back("Sadzba " + row.rate.toString(0) + " % (" +
                                          row.category + ") je na položkách, ale chýba "
                                          "v rozpise DPH");
        }
    }

    // Each line's own arithmetic, which is where a price quoted per N units
    // goes wrong if it goes wrong at all.
    {
        size_t i = 0;
        for (const XmlNode* line : root.childrenNamed(lineTag)) {
            if (i >= inv.lines.size()) break;
            const Dec statedNet = n.at(*line, "LineExtensionAmount");
            const Dec ourNet    = inv.lines[i].netAmount();
            if (statedNet.roundTo(2) != ourNet.roundTo(2))
                r.discrepancies.push_back(
                    "Riadok " + std::to_string(i + 1) + ": doklad uvádza " +
                    moneyText(statedNet) + ", z množstva a ceny vychádza " + moneyText(ourNet));
            ++i;
        }
    }

    r.ok = true;
    return r;
}

} // namespace fk
