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

#include "Model.h"

#include <algorithm>
#include <cctype>
#include <ctime>

namespace fk {

const char* qrFormatCode(QrFormat format) {
    switch (format) {
        case QrFormat::PayBySquare: return "bysquare";
        case QrFormat::Spayd:       return "spayd";
        case QrFormat::Both:        return "both";
        case QrFormat::None:        return "none";
        default:                    return "auto";
    }
}

QrFormat qrFormatFromCode(const std::string& code) {
    if (code == "bysquare") return QrFormat::PayBySquare;
    if (code == "spayd")    return QrFormat::Spayd;
    if (code == "both")     return QrFormat::Both;
    if (code == "none")     return QrFormat::None;
    return QrFormat::Automatic;
}

const char* qrFormatLabel(QrFormat format) {
    switch (format) {
        case QrFormat::PayBySquare: return "PAY by square (SK)";
        case QrFormat::Spayd:       return "QR Platba (CZ)";
        case QrFormat::Both:        return "Oba kódy";
        case QrFormat::None:        return "Bez QR kódu";
        default:                    return "Automaticky";
    }
}

const BankAccount* Company::accountFor(const std::string& currency) const {
    if (accounts.empty()) return nullptr;

    for (const BankAccount& a : accounts)
        if (!a.currency.empty() && a.currency == currency) return &a;
    for (const BankAccount& a : accounts)
        if (a.isDefault) return &a;
    return &accounts.front();
}

void Company::useAccountFor(const std::string& currency) {
    const BankAccount* account = accountFor(currency);
    if (!account) return;
    iban            = account->iban;
    bic             = account->bic;
    bankName        = account->bankName;
    bankLocalNumber = account->localNumber;
    qrFormat        = account->qrFormat;
}

const char* typeCode(DocType type) {
    switch (type) {
        case DocType::Invoice:            return "380";
        case DocType::CreditNote:         return "381";
        case DocType::AdvanceTaxDocument: return "386";   // prepayment invoice
        case DocType::Proforma:           return "";      // 325, not carried by Peppol
    }
    return "380";
}

const char* docTypeLabel(DocType type) {
    switch (type) {
        case DocType::Invoice:            return "Faktúra";
        case DocType::CreditNote:         return "Dobropis";
        case DocType::Proforma:           return "Proforma";
        case DocType::AdvanceTaxDocument: return "Daňový doklad k platbe";
    }
    return "Faktúra";
}

bool isTaxDocument(DocType type) { return type != DocType::Proforma; }

bool canExportToPeppol(DocType type) { return isTaxDocument(type); }

const char* vatModeCode(VatMode mode) {
    switch (mode) {
        case VatMode::Payer:              return "payer";
        case VatMode::RegisteredNotPayer: return "registered";
        default:                          return "none";
    }
}

VatMode vatModeFromCode(const std::string& code) {
    if (code == "payer")      return VatMode::Payer;
    if (code == "registered") return VatMode::RegisteredNotPayer;
    return VatMode::NotRegistered;
}

const char* vatModeLabel(VatMode mode) {
    switch (mode) {
        case VatMode::Payer:              return "Platiteľ DPH (§4)";
        case VatMode::RegisteredNotPayer: return "Registrovaný podľa §7 / §7a – neplatiteľ DPH";
        default:                          return "Neplatiteľ DPH";
    }
}

const char* notVatPayerNote(bool czech) {
    return czech ? "Nejsem plátcem DPH." : "Nie som platiteľom DPH.";
}

const char* stateCode(InvoiceState state) {
    switch (state) {
        case InvoiceState::Issued:    return "issued";
        case InvoiceState::Cancelled: return "cancelled";
        default:                      return "draft";
    }
}

InvoiceState stateFromCode(const std::string& code) {
    if (code == "issued")    return InvoiceState::Issued;
    if (code == "cancelled") return InvoiceState::Cancelled;
    return InvoiceState::Draft;
}

const char* stateLabel(InvoiceState state) {
    switch (state) {
        case InvoiceState::Issued:    return "Vystavená";
        case InvoiceState::Cancelled: return "Stornovaná";
        default:                      return "Rozpracovaná";
    }
}

namespace {

/// Days between two ISO dates. Both must be valid; returns 0 otherwise.
int daysBetween(const std::string& fromIso, const std::string& toIso) {
    auto toTm = [](const std::string& iso, std::tm& tm) {
        if (iso.size() != 10) return false;
        tm = {};
        tm.tm_year = (iso[0]-'0')*1000 + (iso[1]-'0')*100 + (iso[2]-'0')*10 + (iso[3]-'0') - 1900;
        tm.tm_mon  = (iso[5]-'0')*10 + (iso[6]-'0') - 1;
        tm.tm_mday = (iso[8]-'0')*10 + (iso[9]-'0');
        tm.tm_hour = 12;                     // avoid DST edges
        return true;
    };
    std::tm a{}, b{};
    if (!toTm(fromIso, a) || !toTm(toIso, b)) return 0;
    const std::time_t ta = std::mktime(&a);
    const std::time_t tb = std::mktime(&b);
    if (ta == -1 || tb == -1) return 0;
    return static_cast<int>((tb - ta) / (60 * 60 * 24));
}

} // namespace

bool isSettled(Dec payable, Dec paid) {
    return (payable - paid).abs() <= Dec::fromRaw(Dec::SCALE / 200);   // 0.005
}

Dec Invoice::paidAmount() const {
    Dec sum;
    for (const Payment& p : payments) sum += p.amount;
    return sum;      // unrounded: the settled test needs the real figure
}

Dec Invoice::outstanding() const {
    return (totals().payable - paidAmount()).roundTo(2);
}

bool Invoice::isFullyPaid() const {
    return isSettled(totals().payable, paidAmount());
}

bool Invoice::isOverdue(const std::string& todayIso) const {
    if (state != InvoiceState::Issued) return false;
    if (dueDate.empty() || isFullyPaid()) return false;
    return daysBetween(dueDate, todayIso) > 0;
}

int Invoice::daysOverdue(const std::string& todayIso) const {
    if (dueDate.empty()) return 0;
    return daysBetween(dueDate, todayIso);
}

Totals Invoice::totals() const {
    Totals t;

    std::vector<VatBreakdownRow> rows;

    // The breakdown is keyed on category *and* rate: two lines at 23 % and 19 %
    // are two rows even though both are category S.
    auto rowFor = [&rows, this](const std::string& category, const Dec& rate) -> VatBreakdownRow& {
        auto it = std::find_if(rows.begin(), rows.end(), [&](const VatBreakdownRow& r) {
            return r.category == category && r.rate == rate;
        });
        if (it != rows.end()) return *it;
        VatBreakdownRow fresh;
        fresh.category        = category;
        fresh.rate            = rate;
        fresh.exemptionReason = vatExemptionReason;
        rows.push_back(fresh);
        // By index, not rows.back(): the reference is safe today because the
        // caller uses it immediately, but that is one careless edit away from
        // a dangling reference into a reallocated vector.
        return rows[rows.size() - 1];
    };

    // netAmount() already has this line's own discounts and surcharges folded
    // in (BG-27 / BG-28), which is what makes them part of BT-106 while the
    // document-level ones below are not.
    for (const InvoiceLine& l : lines) {
        const Dec net = l.netAmount();
        t.lineExtension += net;
        rowFor(l.vatCategory, l.vatRate).taxableAmount += net;
    }

    // Document-level discounts and surcharges sit *outside* the lines: they
    // reduce or raise the taxable base of the category they name (BR-45) and
    // appear in BT-107 / BT-108, leaving BT-106 alone. This is the whole
    // reason they are not negative lines — a negative line would land in
    // BT-106 and leave the breakdown reconciling to the wrong figure.
    for (const Allowance& a : allowances) {
        const Dec amount = a.amount.roundTo(2);
        if (a.isCharge) t.chargeTotal    += amount;
        else            t.allowanceTotal += amount;
        rowFor(a.vatCategory, a.vatRate).taxableAmount += a.isCharge ? amount : -amount;
    }

    // VAT is computed per category group, never per line (EN 16931 BR-CO-17).
    for (VatBreakdownRow& r : rows) {
        r.taxAmount = r.taxableAmount.percentOf(r.rate);
        t.taxAmount += r.taxAmount;
    }

    // The VAT restated in the seller's own currency (BT-111), when this
    // invoice is not written in it.
    //
    // The order of operations is the tax office's, not mine: § 26 ods. 1 has
    // the *base* converted to euro and the tax computed from the euro base —
    // not the tax computed in crowns and then converted. The two disagree by a
    // cent often enough to matter, and only one of them is the prescribed one.
    // Per category, because that is the level VAT is computed at anyway
    // (BR-CO-17).
    // Asked once, of a zero amount: whether a conversion is possible depends on
    // the two currencies and the rate, never on the figure. Doing it per row
    // would leave a document half restated if that ever stopped being true.
    // `rows` empty means there is no VAT breakdown at all, and a document with
    // BT-6 but no VAT subtotal is rejected twice over — R053 wants a tax total
    // *with* subtotals, R054 wants exactly one without.
    t.restated =
        !rows.empty() &&
        toAccountingCurrency(Dec(), currency, vatAccountingCurrency, exchangeRate).has_value();
    if (t.restated) {
        for (VatBreakdownRow& r : rows) {
            const Dec base = *toAccountingCurrency(r.taxableAmount, currency,
                                                   vatAccountingCurrency, exchangeRate);
            r.taxableAmountAccounting = base.roundTo(2);
            r.taxAmountAccounting     = r.taxableAmountAccounting.percentOf(r.rate);
            t.taxExclusiveAccounting += r.taxableAmountAccounting;
            t.taxAmountAccounting    += r.taxAmountAccounting;
        }
    }

    t.vat            = std::move(rows);
    t.taxExclusive   = t.lineExtension - t.allowanceTotal + t.chargeTotal;   // BR-CO-13
    t.taxInclusive   = t.taxExclusive + t.taxAmount;                         // BR-CO-15
    t.prepaidAmount  = prepaidAmount;
    t.rounding       = roundingAmount.roundTo(2);
    t.payable        = t.taxInclusive - t.prepaidAmount + t.rounding;        // BR-CO-16
    return t;
}

bool quantityColumnsAreRedundant(const std::vector<InvoiceLine>& lines) {
    // An empty table is not a simple one. It has no rows to judge, and letting
    // it collapse would mean the layout of a document depended on an accident.
    if (lines.empty()) return false;

    const Dec one = Dec::fromInt(1);
    for (const InvoiceLine& l : lines) {
        if (!(l.quantity == one)) return false;
        // H87 is "piece". Any other code — hours, kilograms, months — is part
        // of what was supplied and must stay on the page even at a quantity of
        // one: "1 hod" and "1 ks" are different statements.
        if (l.unitCodeUn != "H87") return false;
        // A discount on the line makes the unit price and the line total
        // differ, so the reader can no longer derive one from the other.
        if (!l.allowances.empty()) return false;
        if (!(l.netAmount() == l.unitPrice.roundTo(2))) return false;
    }
    return true;
}

std::string normalisedCurrency(const std::string& code) {
    std::string out;
    for (char c : code) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

std::optional<Dec> toAccountingCurrency(const Dec& amount,
                                        const std::string& invoiceCurrency,
                                        const std::string& accountingCurrency,
                                        const Dec& rate) {
    // Compared folded, never raw. Somebody types "eur" into a three-character
    // box and the raw comparison sees a currency different from EUR: the euro
    // VAT on a euro invoice would then be divided by an exchange rate, and the
    // rule that is supposed to catch it — R005, BT-6 must differ from BT-5 —
    // would agree that they differ.
    const std::string invoice    = normalisedCurrency(invoiceCurrency);
    const std::string accounting = normalisedCurrency(accountingCurrency);

    // Nothing to restate, and Peppol EN16931-R005 forbids declaring BT-6 at all
    // when it equals BT-5.
    if (accounting.empty() || accounting == invoice) return std::nullopt;
    if (rate.isZero() || rate.isNegative()) return std::nullopt;

    // The rate is quoted per euro, so the euro side divides and the other side
    // multiplies. A pair with no euro in it cannot be read at all — better an
    // empty answer than a number arrived at by guessing which way round it is.
    if (accounting == "EUR") return amount / rate;
    if (invoice    == "EUR") return amount * rate;
    return std::nullopt;
}

Dec suggestedRounding(const Dec& amountWithVat) {
    // To whole units, the way a Czech invoice rounds to whole crowns. The
    // difference is what BT-114 carries, so BT-112 − BT-113 + BT-114 lands on
    // a round number.
    const Dec whole = amountWithVat.roundTo(0);
    return whole - amountWithVat;
}

std::string NumberSeries::format(int n) const {
    std::string digits = std::to_string(n);
    if (static_cast<int>(digits.size()) < padding)
        digits.insert(digits.begin(), static_cast<size_t>(padding) - digits.size(), '0');
    std::string out = prefix;
    if (year > 0) out += std::to_string(year);
    return out + digits;
}

} // namespace fk
