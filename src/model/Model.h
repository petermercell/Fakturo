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

// Model.h - the whole invoice domain. Plain structs, no Qt, no database.
#pragma once

#include "../core/Dec.h"

#include <optional>
#include <string>
#include <vector>

namespace fk {

// ---------------------------------------------------------------- VAT codes
// UBL / EN 16931 VAT category codes (UNCL5305 subset used by Peppol BIS 3.0).
namespace VatCat {
constexpr const char* Standard      = "S";   // standard rate
constexpr const char* ZeroRated     = "Z";   // zero rated
constexpr const char* Exempt        = "E";   // exempt from VAT (oslobodene)
constexpr const char* ReverseCharge = "AE";  // prenesenie danovej povinnosti
constexpr const char* IntraCommunity= "K";   // intra-community supply
constexpr const char* Export        = "G";   // export outside the EU
constexpr const char* OutOfScope    = "O";   // not subject to VAT (neplatca DPH)
} // namespace VatCat

/// A proforma is a request for payment, not a tax document: it declares no VAT
/// and has no place in the tax numbering. An advance tax document (§4 payers
/// only) *is* one — it declares the VAT on money already received, and the
/// final invoice then deducts it as a prepayment.
enum class DocType { Invoice, CreditNote, Proforma, AdvanceTaxDocument };

/// UNCL1001 code for the UBL export. Empty for documents Peppol will not carry.
const char* typeCode(DocType type);
const char* docTypeLabel(DocType type);
/// False for a proforma: it declares no VAT and is not reported.
bool isTaxDocument(DocType type);
/// Peppol BIS Billing 3.0 accepts 380, 381 and 386 — but not a proforma (325).
bool canExportToPeppol(DocType type);

/// A draft is a working document: freely editable, numbered only provisionally,
/// and printed with a watermark. Issuing consumes the number and locks the
/// content, because once a document has left the building the lawful way to
/// change it is a credit note, not an edit.
enum class InvoiceState { Draft, Issued, Cancelled };

const char*  stateCode(InvoiceState state);          // "draft" / "issued" / "cancelled"
InvoiceState stateFromCode(const std::string& code);
/// Human label, in Slovak, for the list and the status bar.
const char*  stateLabel(InvoiceState state);

// ------------------------------------------------------------------ parties
struct Address {
    std::string street;
    std::string street2;
    std::string city;
    std::string postalCode;
    std::string countryCode = "SK";   // ISO 3166-1 alpha-2
};

struct Party {
    std::string name;
    std::string ico;        // IC O   - company registration number  (BT-30, scheme 0158)
    std::string dic;        // DIC    - national tax number          (BT-32)
    std::string icDph;      // IC DPH - VAT number, e.g. SK2020123456 (BT-31)
    Address     address;
    std::string email;
    std::string phone;
    std::string contactName;

    // Peppol routing. Empty means "use the country default" — see
    // country/Country.h. It must not default to a literal here: a hard-coded
    // Slovak 0158 on a Czech party routes the invoice to the wrong country.
    std::string endpointScheme;
    std::string endpointId;

    bool isVatRegistered() const { return !icDph.empty(); }
    /// Falls back to ICO so a minimally-filled party still routes.
    std::string effectiveEndpointId() const { return endpointId.empty() ? ico : endpointId; }
};

/// Whether the seller actually charges VAT. This is *not* the same as holding
/// a VAT number: a Slovak subject registered under §7 or §7a has an IČ DPH for
/// intra-EU purposes but is not a platiteľ and must not put VAT on a domestic
/// invoice. Deriving it from "icDph is not empty" gets that case wrong.
enum class VatMode {
    NotRegistered,       ///< no VAT number at all
    RegisteredNotPayer,  ///< §7 / §7a — has an IČ DPH, does not charge VAT
    Payer                ///< §4 — charges VAT
};

const char* vatModeCode(VatMode mode);
VatMode     vatModeFromCode(const std::string& code);
const char* vatModeLabel(VatMode mode);
/// The sentence that belongs on an invoice when no VAT is charged.
const char* notVatPayerNote(bool czech);

/// One account a company can be paid into. A company doing business in both
/// countries needs at least two — a Slovak customer paying a CZK invoice into
/// an EUR account pays a conversion fee for the privilege.
/// Which payment code to print for an account. The two standards are mutually
/// incompatible — Slovak apps read PAY by square, Czech apps read SPAYD, and
/// ČSOB will not read the Slovak one — so this cannot be derived reliably from
/// anything the app knows. Automatic guesses; the rest are the user's call.
enum class QrFormat { Automatic, PayBySquare, Spayd, Both, None };

const char* qrFormatCode(QrFormat format);
QrFormat    qrFormatFromCode(const std::string& code);
const char* qrFormatLabel(QrFormat format);

struct BankAccount {
    int64_t     id = 0;
    std::string label;          // "SK účet", "CZ účet"
    std::string iban;
    std::string bic;
    std::string bankName;
    std::string localNumber;    // domestic form, "19-2000145399/0800"
    std::string currency;       // empty means "any"
    QrFormat    qrFormat = QrFormat::Automatic;
    bool        isDefault = false;
};

struct Company : Party {
    int64_t     id = 0;
    /// On a company these are the *chosen* account, copied from `accounts`
    /// when a document is written. On an invoice's seller snapshot they are
    /// the account that invoice is payable to, frozen with the rest.
    std::string iban;
    std::string bic;
    std::string bankName;
    std::string bankLocalNumber;
    /// e.g. "Zapisany v OR OS Bratislava I, oddiel: Sro, vlozka c. 12345/B"
    std::string registryNote;
    std::string logoPath;    ///< where it came from, shown in the settings
    /// The image itself. Kept in the database rather than as a path, because a
    /// path stops working the day the file is moved or the folder renamed —
    /// and an invoice that quietly lost its logo is the sort of thing you only
    /// notice after sending it.
    std::string logo;
    QrFormat    qrFormat = QrFormat::Automatic;   // of the account in use
    VatMode     vatMode = VatMode::NotRegistered;

    /// Accounts to choose from. Empty on a snapshot, which carries one account.
    std::vector<BankAccount> accounts;

    /// The only question the rest of the app should ask.
    bool chargesVat() const { return vatMode == VatMode::Payer; }

    /// Best account for an invoice in `currency`: an exact currency match
    /// first, then the default, then the first one there is.
    const BankAccount* accountFor(const std::string& currency) const;
    /// Copies that account into iban/bic/bankName/bankLocalNumber.
    void useAccountFor(const std::string& currency);
};

struct Customer : Party {
    int64_t     id = 0;
    std::string note;
    bool        archived = false;
};

// ------------------------------------------------------------------ invoice
/// A discount or a surcharge — on one line (EN 16931 BG-27 / BG-28) or on the
/// whole document (BG-20 / BG-21). One type, because the two are the same
/// shape; where it lives decides which it is.
///
/// The difference is where the money lands. A **line** allowance is folded
/// into that line's net amount (BT-131), so it changes BT-106. A **document**
/// allowance sits outside the lines in BT-107 / BT-108 and leaves BT-106
/// alone. Getting that backwards is the whole reason a discount cannot just
/// be a negative line.
///
/// Worth its own type rather than a negative line. A negative line is wrong in
/// two ways at once: the XML says you sold something for a negative amount,
/// and the discount lands in the line total (BT-106) instead of the allowance
/// total (BT-107) — so the taxable base of its VAT category is overstated and
/// the breakdown does not reconcile.
struct Allowance {
    int64_t     id = 0;
    /// False for a discount, true for a surcharge. They differ only in sign
    /// and label.
    bool        isCharge = false;

    std::string reason;                          ///< BT-97 / BT-104
    /// BT-94 / BT-101. Zero means the amount was given directly rather than
    /// worked out from a percentage.
    Dec         percentage;
    Dec         baseAmount;                      ///< BT-93 / BT-100
    Dec         amount;                          ///< BT-92 / BT-99, always the money

    /// Which VAT category this belongs to. A discount does not float free of
    /// VAT: it reduces the taxable base of one category, and the standard
    /// insists on being told which (BR-32).
    /// Which VAT category this belongs to — document level only. A line
    /// allowance takes the category of its line, because it is part of that
    /// line's net amount and cannot belong to a different rate.
    std::string vatCategory = VatCat::Standard;  ///< BT-95 / BT-102
    Dec         vatRate;                         ///< BT-96 / BT-103

    /// Signed: negative for a discount. What the totals actually add up.
    Dec signedAmount() const { return isCharge ? amount : -amount; }
};

struct InvoiceLine {
    int64_t     id     = 0;
    int         lineNo = 1;
    std::string description;
    std::string unit = "ks";          // free text shown on the PDF
    std::string unitCodeUn = "H87";   // UN/ECE Rec 20 code; H87 = piece
    Dec         quantity  = Dec::fromInt(1);
    Dec         unitPrice;            // net, per unit
    Dec         vatRate;              // percent, e.g. 23
    std::string vatCategory = VatCat::Standard;

    /// Discounts and surcharges on this line: BG-27 and BG-28. They are part
    /// of the line, not of the document — which is what makes them fold into
    /// BT-131 rather than into BT-107.
    std::vector<Allowance> allowances;

    /// Quantity × price, before any line-level discount. The base a percentage
    /// discount on this line applies to (BT-137 / BT-142).
    Dec grossAmount() const { return (quantity * unitPrice).roundTo(2); }

    /// Net line amount, rounded to 2 dp (EN 16931 BT-131): the gross, less the
    /// line's discounts, plus its surcharges.
    Dec netAmount() const {
        Dec net = grossAmount();
        for (const Allowance& a : allowances)
            net += a.isCharge ? a.amount.roundTo(2) : -a.amount.roundTo(2);
        return net;
    }
};

/// Whether the quantity, unit and unit-price columns would tell the reader
/// anything the description and the line total do not.
///
/// They would not when every line is one piece: the quantity is 1, the unit is
/// the default "piece", nothing is discounted on the line, and the unit price
/// is therefore the line total repeated. Printing three columns of noise for
/// that is what the old layout did.
///
/// The test is deliberately strict, and it is a document-wide answer rather
/// than a per-line one. § 74 ods. 1 makes the extent of the supply and the
/// unit price without tax **required particulars**, so the moment one line
/// carries a real quantity — eight hours, three days, 1,5 kg — every column
/// comes back for the whole table. A table where some rows show a quantity and
/// others do not would be worse than either.
///
/// Lives here rather than in the renderer so it can be tested without Qt: it
/// decides what appears on a legal document.
bool quantityColumnsAreRedundant(const std::vector<InvoiceLine>& lines);

struct VatBreakdownRow {
    std::string category;
    Dec         rate;
    Dec         taxableAmount;
    Dec         taxAmount;
    std::string exemptionReason;

    /// The same two figures in the currency the seller accounts for VAT in,
    /// when that differs from the invoice currency. Zero when it does not.
    /// Not part of EN 16931 — only the document total (BT-111) is — but the
    /// base has to be converted per category to arrive at that total, and the
    /// printed VAT recap shows both columns.
    Dec taxableAmountAccounting;
    Dec taxAmountAccounting;
};

struct Totals {
    Dec lineExtension;   // BT-106 sum of line net amounts
    Dec allowanceTotal;  // BT-107 sum of document-level discounts
    Dec chargeTotal;     // BT-108 sum of document-level surcharges
    Dec taxExclusive;    // BT-109 = BT-106 − BT-107 + BT-108
    Dec taxAmount;       // BT-110
    Dec taxInclusive;    // BT-112 = BT-109 + BT-110
    Dec prepaidAmount;   // BT-113
    Dec rounding;        // BT-114 rounding of the amount due
    Dec payable;         // BT-115 = BT-112 − BT-113 + BT-114
    std::vector<VatBreakdownRow> vat;

    /// BT-111, the VAT restated in the seller's accounting currency. Present
    /// only when the invoice is written in another currency and there is a
    /// rate to convert at; `restated` says which, because a genuine zero (an
    /// invoice with no VAT) and "not converted" are different things.
    bool restated = false;
    Dec  taxAmountAccounting;
    Dec  taxExclusiveAccounting;   ///< the converted base, for the printed recap
};

/// Upper-cased with the spaces taken out. Currency codes are compared, never
/// displayed, through this: "eur" and "EUR" are the same currency and code that
/// believes otherwise divides a euro amount by an exchange rate.
std::string normalisedCurrency(const std::string& code);

/// Converts an amount from the invoice currency to the seller's VAT accounting
/// currency. Empty when the pair cannot be converted at this rate, which the
/// caller must treat as "do not restate" rather than as zero.
///
/// `rate` is the rate exactly as **both** the ECB and the ČNB publish it: how
/// many units of the other currency make **one euro** — 24.167 CZK to the euro,
/// not 0.041 euro to the crown. Quoting it the other way round would cost real
/// accuracy: a reciprocal held to six places is only five significant digits,
/// which is tens of crowns adrift on a six-figure invoice.
///
/// That convention is why one of the two currencies must be EUR. Both of this
/// application's countries account in EUR or in CZK, so every pair it can
/// produce qualifies; a third country accounting in neither would need the
/// rate to carry its own base, and this returns empty rather than guess.
std::optional<Dec> toAccountingCurrency(const Dec& amount,
                                        const std::string& invoiceCurrency,
                                        const std::string& accountingCurrency,
                                        const Dec& rate);

/// What BT-114 should be to bring the amount due to a whole unit — how a
/// Czech invoice rounds to whole crowns. Offered, never applied on its own:
/// rounding is a decision about the document, not a property of the arithmetic.
Dec suggestedRounding(const Dec& amountWithVat);

/// One receipt against an invoice. Partial payments are normal, so this is a
/// list rather than a paid flag.
struct Payment {
    int64_t     id       = 0;
    int64_t     invoiceId = 0;
    std::string paidOn;      // ISO date
    Dec         amount;
    std::string note;
};

/// Bank transfers get rounded in practice, so a document counts as settled
/// when it is within half a cent. Defined once: the invoice and the list view
/// both use it, and a second implementation would eventually disagree.
bool isSettled(Dec payable, Dec paid);

struct Invoice {
    int64_t      id    = 0;
    /// Which of your companies issued this. Decides the numbering series and
    /// which list the document appears in.
    int64_t      companyId = 0;
    /// The seller exactly as it was when the document was written. The buyer
    /// has always been snapshotted this way; the seller was not, so changing
    /// your address silently rewrote every historical invoice. Because an
    /// issued document cannot be saved again, this freezes itself on issue.
    Company      seller;
    InvoiceState state = InvoiceState::Draft;
    std::string  issuedAt;      // ISO timestamp when it was issued, empty for drafts
    DocType      type  = DocType::Invoice;
    std::string number;              // BT-1
    std::string issueDate;           // BT-2  ISO yyyy-mm-dd
    std::string taxPointDate;        // BT-7  datum dodania; empty = same as issue
    std::string dueDate;             // BT-9
    std::string currency = "EUR";    // BT-5

    /// BT-6, and the rate it is arrived at. Filled only when this invoice is
    /// written in a currency other than the one the seller accounts for VAT
    /// in — a Slovak payer invoicing in crowns still owes euro VAT, and
    /// § 74 ods. 1 písm. i) makes the euro figure a required particular of the
    /// invoice itself, not merely of the XML.
    ///
    /// Snapshotted like the seller: § 25 has a correction reuse the rate of the
    /// original tax point, so this must not follow later edits to anything.
    std::string vatAccountingCurrency;   ///< empty when it does not apply
    Dec         exchangeRate;            ///< units of the other currency to one euro
    std::string exchangeRateDate;        ///< ISO date the rate was published for

    Customer    buyer;               // snapshot taken at issue time
    std::string buyerReference;      // BT-10
    std::string orderReference;      // BT-13
    std::string note;                // BT-22

    // Slovak payment symbols. Not part of UBL; carried in PaymentID / PDF.
    std::string variableSymbol;
    std::string constantSymbol = "0308";
    std::string specificSymbol;

    std::string paymentMeansCode = "31";   // 31 = credit transfer
    std::string paymentTerms;              // BT-20 free text

    // Credit notes must point at the invoice they correct (BG-3).
    std::string precedingNumber;
    std::string precedingDate;

    /// Free-text reason shown when no VAT is charged (BT-120).
    std::string vatExemptionReason;

    Dec prepaidAmount;
    /// BT-114. Czech invoices commonly round the amount due to whole crowns
    /// and show the difference; the standard has a field for exactly that.
    Dec roundingAmount;
    std::vector<InvoiceLine> lines;
    /// Document-level discounts and surcharges (BG-20 / BG-21).
    std::vector<Allowance> allowances;

    /// Payments recorded against this document. Not part of the invoice itself,
    /// so they survive independently of the lock.
    std::vector<Payment> payments;

    /// Numbers of the proformas this invoice settles, in order. A company that
    /// issues four advance requests and then one invoice needs all four here.
    std::vector<std::string> relatedProformaNumbers;
    /// For a proforma: the number of the invoice that settled it, if any.
    std::string settledByNumber;

    Totals totals() const;
    bool   isCreditNote() const { return type == DocType::CreditNote; }
    bool   isProforma()   const { return type == DocType::Proforma; }

    /// Only drafts may be edited. Everything else needs a corrective document.
    bool isEditable() const { return state == InvoiceState::Draft; }

    Dec  paidAmount() const;
    /// Payable minus what has been received. Negative means overpaid.
    Dec  outstanding() const;
    bool isFullyPaid() const;
    /// Unpaid and past the due date on `todayIso`. Drafts are never overdue.
    bool isOverdue(const std::string& todayIso) const;
    /// Days past due, negative while still in time. 0 when there is no due date.
    int  daysOverdue(const std::string& todayIso) const;
};

/// Number series, e.g. prefix "2026" + 4 digits -> "20260001".
struct NumberSeries {
    std::string prefix = "";
    int         year   = 0;
    int         next   = 1;
    int         padding = 4;

    std::string format(int n) const;
};

} // namespace fk
