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

// InvoiceView.h - searching, sorting and summarising the invoice list.
//
// Everything the two "everyday use" screens need, as plain functions over the
// rows the database already returns. No Qt, no widgets, no database access —
// so the parts that are easy to get subtly wrong (folding diacritics, sorting
// money by value rather than as text, deciding what counts as outstanding) are
// testable without a screen.
#pragma once

#include "Database.h"

#include <string>
#include <vector>

namespace fk {

// --------------------------------------------------------------------- search

/// Lower-cases and strips Slovak and Czech diacritics, so a search for "novak"
/// finds "Novák" — which is how people actually type when looking for
/// something.
std::string foldForSearch(const std::string& text);

/// Does this document match what was typed? One box searches the **number,
/// the customer, the variable symbol and the amount**, because someone looking
/// for "Novák" and someone looking for "20260042" both just type it.
///
/// An empty query matches everything. Several words all have to match, in any
/// order and in any field: "novak 1291" finds Novák's invoice for 1291.46.
bool matchesSearch(const Database::InvoiceSummary& row, const std::string& query);

// ----------------------------------------------------------------------- sort

/// The columns of the invoice list, in the order they are shown.
enum class SortBy { Number, Type, Status, IssueDate, DueDate, Customer, Total, Outstanding };

/// Sorts in place. Amounts and dates are compared by value, never as text —
/// sorting "1000.00" before "9.00" is the classic bug in a list like this.
/// The order is total, so two runs over equal keys cannot disagree.
void sortInvoices(std::vector<Database::InvoiceSummary>& rows, SortBy by, bool ascending);

/// What a fresh click on a column should do. Dates and amounts are most useful
/// largest-first; names and numbers read better the other way.
bool defaultAscending(SortBy by);

// ------------------------------------------------------------------ dashboard

struct CurrencyTotal {
    std::string currency;
    Dec         amount;
    int         count = 0;
};

/// The answers to the two questions the app gets opened for: what am I owed,
/// and how did this month go.
struct Overview {
    // Money owed to you, split at the due date. Only issued documents: a draft
    // has not been sent to anyone, and a cancelled one is not owed.
    std::vector<CurrencyTotal> notYetDue;
    std::vector<CurrencyTotal> overdue;

    // What went out. By issue date, so it lines up with a VAT period.
    std::vector<CurrencyTotal> issuedThisMonth;
    std::vector<CurrencyTotal> issuedLastMonth;
    std::vector<CurrencyTotal> issuedThisYear;

    int draftCount = 0;

    /// The oldest unpaid issued document, if there is one. Usually the one
    /// worth doing something about.
    bool        hasOldest = false;
    std::string oldestNumber;
    std::string oldestCustomer;
    std::string oldestDueDate;
    std::string oldestCurrency;
    Dec         oldestAmount;
    int         oldestDaysLate = 0;   // always positive: only overdue ones qualify
};

/// Computes the lot from the rows the list already has. `todayIso` is passed in
/// rather than read from the clock so this can be tested.
Overview summarise(const std::vector<Database::InvoiceSummary>& rows,
                   const std::string& todayIso);

// ------------------------------------------------------------------- payables
// The mirror of the dashboard, for invoices somebody sent *you*. Same shape,
// opposite direction: not "what am I owed" but "what do I owe, and when".

/// What is outstanding on received invoices, split by when it falls due.
/// Per currency throughout, because adding EUR to CZK is not a number.
struct Payables {
    std::vector<CurrencyTotal> overdue;     ///< due date already passed
    std::vector<CurrencyTotal> dueSoon;     ///< within the next seven days
    std::vector<CurrencyTotal> later;       ///< everything else still unpaid

    /// The one worth doing something about: the oldest thing still owed.
    bool        hasOldest = false;
    std::string oldestNumber;
    std::string oldestSupplier;
    std::string oldestDueDate;
    std::string oldestCurrency;
    Dec         oldestAmount;
    int         oldestDaysLate = 0;   ///< negative while still in time

    /// How many carry a warning from the reader — a document that does not
    /// reconcile with itself is worth counting on the front page.
    int withWarnings = 0;
};

Payables summarisePayables(const std::vector<Database::ReceivedInvoice>& rows,
                           const std::string& todayIso);

/// Everything received from one supplier, keyed on their IČO — which is the
/// grouping the documents themselves already carry, so there is no supplier
/// list to maintain and nothing to keep in step. Falls back to the folded name
/// when a sender gives no IČO at all.
struct SupplierGroup {
    std::string key;            ///< IČO, or the folded name when there is none
    std::string name;           ///< as most recently received
    std::string ico;
    int         count = 0;
    std::vector<CurrencyTotal> outstanding;
    std::string lastIssueDate;
};

/// Largest debt first, so the one to deal with is at the top.
std::vector<SupplierGroup> groupBySupplier(
    const std::vector<Database::ReceivedInvoice>& rows);

/// Does this received invoice match what was typed? Number, supplier, variable
/// symbol and amount, folded the same way the invoice list folds them.
bool matchesSearch(const Database::ReceivedInvoice& row, const std::string& query);

/// The filter above the received list.
enum class PayableFilter { All, Unpaid, DueSoon, Overdue, Warnings };
bool passesFilter(const Database::ReceivedInvoice& row, PayableFilter filter,
                  const std::string& todayIso);

// ---------------------------------------------------------------------- dates
// ISO date arithmetic, kept here because the dashboard is the only thing that
// needs it and it is easier to test than to trust.

std::string startOfMonth(const std::string& iso);   // 2026-07-19 -> 2026-07-01
std::string endOfMonth(const std::string& iso);     //            -> 2026-07-31
std::string startOfYear(const std::string& iso);    //            -> 2026-01-01
std::string startOfPreviousMonth(const std::string& iso);
std::string endOfPreviousMonth(const std::string& iso);

/// Whole days from `from` to `to`, negative when `to` is earlier.
int daysBetween(const std::string& fromIso, const std::string& toIso);

} // namespace fk
