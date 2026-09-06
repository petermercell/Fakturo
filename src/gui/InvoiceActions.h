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

// InvoiceActions.h - what the buttons actually do, separated from the asking.
//
// Issuing a document is the most consequential thing this application does: it
// consumes a number, locks the content and archives the bytes that were sent.
// It also has to refuse in five different ways. All of that used to live inside
// one method interleaved with message boxes, where it could not be tested —
// and this layer is where every defect of consequence in this project has come
// from.
//
// So the decisions and the work live here, with no dialogs in them. MainWindow
// keeps the asking: plan, confirm with the user, perform, report.
#pragma once

#include "../db/Database.h"
#include "../model/Model.h"

#include <string>
#include <vector>

namespace fk {

/// The seller to print, which is the document's own frozen snapshot with the
/// company's logo put back on it.
///
/// The snapshot has every seller field except that one: there is no `s_logo`
/// column, so `inv.seller.logo` is always empty and the logo has never reached
/// the page. It is fetched from the company the document belongs to — by
/// `inv.companyId`, not by whichever company happens to be active — so a
/// document issued under one company cannot print another's mark.
///
/// Not stored on the invoice on purpose. A logo is tens of kilobytes and would
/// sit in every row and in every backup, and the case it would protect against
/// barely exists: an issued document is exported from its **archived bytes**,
/// which already hold the logo as it was when the document went out. Only a
/// draft, or a document unlocked back to one, re-renders — and a draft printing
/// today's logo is right rather than wrong.
Company sellerForRender(Database& db, const Invoice& inv);

/// Whether this document can be issued, and under what number.
struct IssuePlan {
    bool        ok = false;
    std::string number;                    // the number it would take
    std::string refusal;                   // why not, when !ok
    std::vector<std::string> errors;       // validation errors behind the refusal

    /// The symbol the document will carry, derived from the number when it has
    /// none of its own. Part of the plan because it has to be *stored*, not
    /// only printed: issuing writes state, issued_at and number, so a symbol
    /// derived at render time never reached the database and the bank matcher
    /// could not find the payment the invoice asked for.
    std::string variableSymbol;

    /// True when `number` is the next one in the series, and issuing should
    /// therefore advance it. A number reached by skipping past one already in
    /// use consumes nothing, so the gap closes by itself.
    bool advancesSeries = false;
    Database::SeriesKind kind = Database::SeriesKind::Invoice;
};

/// Decides. Reads the database; changes nothing.
IssuePlan planIssue(Database& db, const Invoice& inv);

struct IssueOutcome {
    bool        ok = false;
    std::string number;
    std::string error;
};

/// Renders the PDF and UBL, issues, archives, and advances the series — in
/// that order, because a document that cannot be rendered must not be issued
/// and a number must not be consumed until it really is in use.
IssueOutcome issueDocument(Database& db, const Invoice& inv, const IssuePlan& plan);

} // namespace fk
