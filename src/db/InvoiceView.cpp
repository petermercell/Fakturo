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

#include "InvoiceView.h"

#include "../sk/Slovak.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>

namespace fk {
namespace {

/// Adds `amount` to the entry for `currency`, creating it if need be. Kept
/// separate per currency throughout: adding EUR to CZK is not a number.
void addTo(std::vector<CurrencyTotal>& totals, const std::string& currency, const Dec& amount) {
    for (CurrencyTotal& t : totals)
        if (t.currency == currency) { t.amount += amount; ++t.count; return; }
    CurrencyTotal fresh;
    fresh.currency = currency;
    fresh.amount   = amount;
    fresh.count    = 1;
    totals.push_back(std::move(fresh));
}

/// Largest first, so the currency you mostly work in leads.
void orderTotals(std::vector<CurrencyTotal>& totals) {
    std::sort(totals.begin(), totals.end(), [](const CurrencyTotal& a, const CurrencyTotal& b) {
        if (a.amount != b.amount) return b.amount < a.amount;
        return a.currency < b.currency;
    });
}

bool isRealDocument(const Database::InvoiceSummary& row) {
    return row.state == InvoiceState::Issued;
}

/// Days from the civil epoch. Howard Hinnant's days_from_civil, which is exact
/// for any proleptic Gregorian date and needs no library.
long long daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

bool splitIso(const std::string& iso, int& y, int& m, int& d) {
    if (iso.size() < 10 || iso[4] != '-' || iso[7] != '-') return false;
    for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u})
        if (!std::isdigit(static_cast<unsigned char>(iso[i]))) return false;
    y = std::stoi(iso.substr(0, 4));
    m = std::stoi(iso.substr(5, 2));
    d = std::stoi(iso.substr(8, 2));
    return m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

std::string isoOf(int y, int m, int d) {
    // Built rather than printed: snprintf into a fixed buffer is what
    // -Wformat-truncation objects to, and it is right to — a four-digit year
    // is an assumption, not a guarantee.
    auto pad = [](int value, size_t width) {
        std::string text = std::to_string(value);
        while (text.size() < width) text.insert(text.begin(), '0');
        return text;
    };
    return pad(y, 4) + "-" + pad(m, 2) + "-" + pad(d, 2);
}

int daysInMonth(int y, int m) {
    static const std::array<int, 12> lengths = {31, 28, 31, 30, 31, 30,
                                                31, 31, 30, 31, 30, 31};
    if (m == 2) {
        const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        return leap ? 29 : 28;
    }
    return lengths[static_cast<size_t>(m - 1)];
}

} // namespace

// --------------------------------------------------------------------- search

std::string foldForSearch(const std::string& text) {
    // Both alphabets, both cases. A search box that only matches when the
    // accents are right is a search box people stop using.
    static const std::map<std::string, char> folded = {
        {"á", 'a'}, {"ä", 'a'}, {"č", 'c'}, {"ď", 'd'}, {"é", 'e'}, {"ě", 'e'},
        {"í", 'i'}, {"ĺ", 'l'}, {"ľ", 'l'}, {"ň", 'n'}, {"ó", 'o'}, {"ô", 'o'},
        {"ö", 'o'}, {"ŕ", 'r'}, {"ř", 'r'}, {"š", 's'}, {"ť", 't'}, {"ú", 'u'},
        {"ů", 'u'}, {"ü", 'u'}, {"ý", 'y'}, {"ž", 'z'}, {"ĝ", 'g'},
        {"Á", 'a'}, {"Ä", 'a'}, {"Č", 'c'}, {"Ď", 'd'}, {"É", 'e'}, {"Ě", 'e'},
        {"Í", 'i'}, {"Ĺ", 'l'}, {"Ľ", 'l'}, {"Ň", 'n'}, {"Ó", 'o'}, {"Ô", 'o'},
        {"Ö", 'o'}, {"Ŕ", 'r'}, {"Ř", 'r'}, {"Š", 's'}, {"Ť", 't'}, {"Ú", 'u'},
        {"Ů", 'u'}, {"Ü", 'u'}, {"Ý", 'y'}, {"Ž", 'z'}, {"Ĝ", 'g'}};

    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const auto b = static_cast<unsigned char>(text[i]);
        if (b < 0x80) {
            out += static_cast<char>(std::tolower(b));
            ++i;
            continue;
        }
        const size_t width = (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
        const std::string glyph = text.substr(i, width);
        const auto it = folded.find(glyph);
        if (it != folded.end()) out += it->second;
        else                    out += glyph;      // keep it; it may still match
        i += width;
    }
    return out;
}

namespace {

/// An amount both ways round. The list shows Slovak commas — 1 291,46 — so
/// somebody searching for what is on their screen types a comma, while the
/// stored form uses a dot. Both go into the haystack rather than making the
/// person guess which one this box wants.
std::string amountForms(const Dec& value) {
    const std::string dotted = value.toString(2);
    std::string comma = dotted;
    for (char& c : comma)
        if (c == '.') c = ',';
    return dotted + " " + comma;
}

} // namespace

bool matchesSearch(const Database::InvoiceSummary& row, const std::string& query) {
    const std::string needle = foldForSearch(query);
    if (needle.find_first_not_of(" \t") == std::string::npos) return true;

    // One folded haystack per row. The amounts go in as plain text, so typing
    // "1291" finds an invoice for 1291.46 without a separate amount box.
    const std::string hay =
        foldForSearch(row.number) + " " + foldForSearch(row.buyerName) + " " +
        foldForSearch(row.variableSymbol) + " " + amountForms(row.total) + " " +
        amountForms(row.outstanding()) + " " + foldForSearch(row.currency) + " " +
        row.issueDate + " " + row.dueDate;

    // Every word has to appear somewhere. Two words narrow rather than widen,
    // which is what a person typing a second word means by it.
    size_t start = 0;
    while (start < needle.size()) {
        const size_t end = needle.find(' ', start);
        const std::string word =
            needle.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!word.empty() && hay.find(word) == std::string::npos) return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

// ----------------------------------------------------------------------- sort

bool defaultAscending(SortBy by) {
    switch (by) {
        case SortBy::IssueDate:
        case SortBy::DueDate:
        case SortBy::Total:
        case SortBy::Outstanding:
            return false;      // newest and largest first
        default:
            return true;
    }
}

void sortInvoices(std::vector<Database::InvoiceSummary>& rows, SortBy by, bool ascending) {
    using Row = Database::InvoiceSummary;

    // Returns <0, 0, >0. Amounts and dates are compared by value: sorting
    // "1000.00" before "9.00" because '1' < '9' is the classic bug here, and
    // it is invisible until the day it matters.
    auto compare = [by](const Row& a, const Row& b) -> int {
        auto text = [](const std::string& x, const std::string& y) {
            const std::string fx = foldForSearch(x), fy = foldForSearch(y);
            return fx < fy ? -1 : fx > fy ? 1 : 0;
        };
        auto money = [](const Dec& x, const Dec& y) { return x < y ? -1 : y < x ? 1 : 0; };

        switch (by) {
            case SortBy::Number:      return text(a.number, b.number);
            case SortBy::Type:        return text(docTypeLabel(a.type), docTypeLabel(b.type));
            case SortBy::Status:      return static_cast<int>(a.state) - static_cast<int>(b.state);
            case SortBy::IssueDate:   return text(a.issueDate, b.issueDate);   // ISO sorts
            case SortBy::DueDate:     return text(a.dueDate, b.dueDate);
            case SortBy::Customer:    return text(a.buyerName, b.buyerName);
            case SortBy::Total:       return money(a.total, b.total);
            case SortBy::Outstanding: return money(a.outstanding(), b.outstanding());
        }
        return 0;
    };

    std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
        const int c = compare(a, b);
        // A total order, so two sorts of equal keys cannot disagree and rows
        // do not shuffle under the cursor when the list refreshes.
        if (c != 0) return ascending ? c < 0 : c > 0;
        if (a.issueDate != b.issueDate) return a.issueDate > b.issueDate;
        return a.id > b.id;
    });
}

// ------------------------------------------------------------------- payables

Payables summarisePayables(const std::vector<Database::ReceivedInvoice>& rows,
                           const std::string& todayIso) {
    Payables out;
    const std::string weekEnd = sk::addDays(todayIso, 7);

    for (const Database::ReceivedInvoice& row : rows) {
        if (!row.warnings.empty()) ++out.withWarnings;
        if (row.isSettled()) continue;

        // What is left, not the whole invoice: a part-paid one owes the rest.
        const Dec left = row.outstanding();
        if (left <= Dec()) continue;

        if (row.dueDate.empty() || row.dueDate >= weekEnd) addTo(out.later, row.currency, left);
        else if (row.dueDate < todayIso)                   addTo(out.overdue, row.currency, left);
        else                                               addTo(out.dueSoon, row.currency, left);

        // The oldest by due date, which is the one a supplier will ring about.
        // An undated invoice never claims the place: `hasOldest` can only be
        // set from inside this guard, so `oldestDueDate` is never empty when
        // it is true.
        const bool older = !out.hasOldest || row.dueDate < out.oldestDueDate;
        if (!row.dueDate.empty() && older) {
            out.hasOldest      = true;
            out.oldestNumber   = row.number;
            out.oldestSupplier = row.supplier.name;
            out.oldestDueDate  = row.dueDate;
            out.oldestCurrency = row.currency;
            out.oldestAmount   = left;
            out.oldestDaysLate = daysBetween(row.dueDate, todayIso);
        }
    }

    orderTotals(out.overdue);
    orderTotals(out.dueSoon);
    orderTotals(out.later);
    return out;
}

std::vector<SupplierGroup> groupBySupplier(
    const std::vector<Database::ReceivedInvoice>& rows) {
    std::vector<SupplierGroup> out;

    for (const Database::ReceivedInvoice& row : rows) {
        // The IČO is the identity. Two documents from one company differ in
        // how the name is spelled far more often than in the number behind it.
        std::string key =
            row.supplier.ico.empty() ? foldForSearch(row.supplier.name) : row.supplier.ico;
        // A sender with neither an IČO nor a name gets a bucket of its own
        // rather than being dropped. Silently losing an invoice — and the
        // money owed on it — out of a summary is the worse answer.
        if (key.empty()) key = "?";

        auto it = std::find_if(out.begin(), out.end(),
                               [&key](const SupplierGroup& g) { return g.key == key; });
        if (it == out.end()) {
            SupplierGroup fresh;
            fresh.key  = key;
            fresh.name = row.supplier.name;
            fresh.ico  = row.supplier.ico;
            out.push_back(std::move(fresh));
            it = out.end() - 1;
        }
        ++it->count;
        // The most recently *issued* document decides how the name is spelled:
        // a company that renames itself should appear under the new name.
        if (it->lastIssueDate.empty() || row.issueDate > it->lastIssueDate) {
            it->lastIssueDate = row.issueDate;
            if (!row.supplier.name.empty()) it->name = row.supplier.name;
        }
        if (!row.isSettled() && row.outstanding() > Dec())
            addTo(it->outstanding, row.currency, row.outstanding());
    }

    for (SupplierGroup& g : out) orderTotals(g.outstanding);
    // Owing something first, then by name. Ordering by *amount* would compare
    // across currencies — 1 000 CZK outranking 500 EUR — and everything else
    // in this file is scrupulous about not doing that. The key breaks the last
    // tie so the order is total and two runs cannot disagree.
    std::sort(out.begin(), out.end(), [](const SupplierGroup& a, const SupplierGroup& b) {
        const bool leftOwes = !a.outstanding.empty(), rightOwes = !b.outstanding.empty();
        if (leftOwes != rightOwes) return leftOwes;
        const std::string an = foldForSearch(a.name), bn = foldForSearch(b.name);
        if (an != bn) return an < bn;
        return a.key < b.key;
    });
    return out;
}

bool matchesSearch(const Database::ReceivedInvoice& row, const std::string& query) {
    const std::string needle = foldForSearch(query);
    if (needle.find_first_not_of(" \t") == std::string::npos) return true;

    const std::string hay =
        foldForSearch(row.number) + " " + foldForSearch(row.supplier.name) + " " +
        foldForSearch(row.supplier.ico) + " " + foldForSearch(row.variableSymbol) + " " +
        amountForms(row.payable) + " " + amountForms(row.outstanding()) + " " +
        foldForSearch(row.currency) + " " + row.issueDate + " " + row.dueDate + " " +
        foldForSearch(row.note);

    size_t start = 0;
    while (start < needle.size()) {
        const size_t end = needle.find(' ', start);
        const std::string word =
            needle.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!word.empty() && hay.find(word) == std::string::npos) return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

bool passesFilter(const Database::ReceivedInvoice& row, PayableFilter filter,
                  const std::string& todayIso) {
    switch (filter) {
        case PayableFilter::All:      return true;
        case PayableFilter::Unpaid:   return !row.isSettled();
        case PayableFilter::Overdue:  return row.isOverdue(todayIso);
        case PayableFilter::Warnings: return !row.warnings.empty();
        case PayableFilter::DueSoon:
            // Due within a week, including what is already late: "what do I
            // have to pay this week" means both.
            return !row.isSettled() && !row.dueDate.empty() &&
                   row.dueDate < sk::addDays(todayIso, 7);
    }
    return true;
}

// ---------------------------------------------------------------------- dates

std::string startOfMonth(const std::string& iso) {
    int y, m, d;
    if (!splitIso(iso, y, m, d)) return {};
    return isoOf(y, m, 1);
}

std::string endOfMonth(const std::string& iso) {
    int y, m, d;
    if (!splitIso(iso, y, m, d)) return {};
    return isoOf(y, m, daysInMonth(y, m));
}

std::string startOfYear(const std::string& iso) {
    int y, m, d;
    if (!splitIso(iso, y, m, d)) return {};
    return isoOf(y, 1, 1);
}

std::string startOfPreviousMonth(const std::string& iso) {
    int y, m, d;
    if (!splitIso(iso, y, m, d)) return {};
    if (--m == 0) { m = 12; --y; }
    return isoOf(y, m, 1);
}

std::string endOfPreviousMonth(const std::string& iso) {
    int y, m, d;
    if (!splitIso(iso, y, m, d)) return {};
    if (--m == 0) { m = 12; --y; }
    return isoOf(y, m, daysInMonth(y, m));
}

int daysBetween(const std::string& fromIso, const std::string& toIso) {
    int ay, am, ad, by, bm, bd;
    if (!splitIso(fromIso, ay, am, ad) || !splitIso(toIso, by, bm, bd)) return 0;
    return static_cast<int>(daysFromCivil(by, static_cast<unsigned>(bm), static_cast<unsigned>(bd)) -
                            daysFromCivil(ay, static_cast<unsigned>(am), static_cast<unsigned>(ad)));
}

// ------------------------------------------------------------------ dashboard

Overview summarise(const std::vector<Database::InvoiceSummary>& rows,
                   const std::string& todayIso) {
    Overview out;

    const std::string monthStart = startOfMonth(todayIso);
    const std::string monthEnd   = endOfMonth(todayIso);
    const std::string prevStart  = startOfPreviousMonth(todayIso);
    const std::string prevEnd    = endOfPreviousMonth(todayIso);
    const std::string yearStart  = startOfYear(todayIso);

    std::string oldestDue;

    for (const Database::InvoiceSummary& row : rows) {
        if (row.state == InvoiceState::Draft) { ++out.draftCount; continue; }
        if (!isRealDocument(row)) continue;          // cancelled: nobody owes it

        // A credit note reduces what is owed rather than adding to it, and a
        // proforma is not a receivable at all — it is a request to pay before
        // there is a document.
        const bool receivable = row.type == DocType::Invoice ||
                                row.type == DocType::CreditNote;

        if (receivable && !row.isFullyPaid()) {
            const Dec left = row.outstanding();
            if (!left.isZero()) {
                const bool late = row.isOverdue(todayIso);
                addTo(late ? out.overdue : out.notYetDue, row.currency, left);

                // The oldest one still owed, by due date. It is nearly always
                // the one worth a phone call.
                if (late && (oldestDue.empty() || row.dueDate < oldestDue)) {
                    oldestDue           = row.dueDate;
                    out.hasOldest       = true;
                    out.oldestNumber    = row.number;
                    out.oldestCustomer  = row.buyerName;
                    out.oldestDueDate   = row.dueDate;
                    out.oldestCurrency  = row.currency;
                    out.oldestAmount    = left;
                    out.oldestDaysLate  = daysBetween(row.dueDate, todayIso);
                }
            }
        }

        // Turnover is by issue date, so it lines up with a VAT period. A
        // proforma is excluded: it is not a tax document and counting it would
        // book the same money twice when the invoice follows.
        if (isTaxDocument(row.type)) {
            if (!monthStart.empty() && row.issueDate >= monthStart && row.issueDate <= monthEnd)
                addTo(out.issuedThisMonth, row.currency, row.total);
            if (!prevStart.empty() && row.issueDate >= prevStart && row.issueDate <= prevEnd)
                addTo(out.issuedLastMonth, row.currency, row.total);
            if (!yearStart.empty() && row.issueDate >= yearStart && row.issueDate <= todayIso)
                addTo(out.issuedThisYear, row.currency, row.total);
        }
    }

    for (std::vector<CurrencyTotal>* totals :
         {&out.notYetDue, &out.overdue, &out.issuedThisMonth, &out.issuedLastMonth,
          &out.issuedThisYear})
        orderTotals(*totals);

    return out;
}

} // namespace fk
