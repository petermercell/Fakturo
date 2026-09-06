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

// Matcher.h - deciding which invoice a payment belongs to.
//
// Deliberately conservative. A wrong match marks an unpaid invoice paid, and
// the person who finds out is the customer receiving a reminder they do not
// deserve. So a match is only "confident" when the variable symbol identifies
// exactly one invoice, the currency agrees and the amount settles it exactly.
// Everything else is offered as a suggestion for a human to confirm.
//
// No Qt, no database: invoices in, decisions out.
#pragma once

#include "Statement.h"
#include "../core/Dec.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fk {

/// What the matcher needs to know about an invoice. A narrow view of the real
/// thing, so the matching can be tested without a database behind it. Used for
/// both directions: an invoice you issued and are owed, or one you received
/// and owe.
struct PayableInvoice {
    int64_t     id = 0;
    std::string number;
    std::string variableSymbol;
    std::string currency;
    Dec         payable;          // the total owed, either way round
    Dec         paid;             // what has already been recorded against it
    bool        cancelled = false;

    /// Only for the outgoing direction: the account the money should go to.
    /// A payment leaving your account for exactly that IBAN is strong evidence
    /// even when the variable symbol is missing, which on a supplier's invoice
    /// it often is.
    std::string counterAccount;

    Dec outstanding() const { return payable - paid; }
};

/// Which way the money is going.
///
/// The two are not symmetrical. A customer paying you fills in *your* variable
/// symbol, so the symbol is the strong signal. Paying a supplier, *you* fill
/// in theirs — and the field a Slovak bank sends it in is the same one, but
/// the account number is now something you know in advance, so it carries
/// weight it never had incoming.
enum class MatchDirection {
    Incoming,   ///< credits, against invoices you issued
    Outgoing    ///< debits, against invoices you received
};

enum class MatchQuality {
    Confident,   // tick it by default
    Uncertain,   // show it, but the human decides
    None         // nothing plausible
};

struct TransactionMatch {
    size_t       transaction = 0;   // index into Statement::transactions
    int64_t      invoiceId   = 0;
    std::string  invoiceNumber;
    MatchQuality quality = MatchQuality::None;
    std::string  reason;            // shown in the review screen, in Slovak

    bool matched() const { return invoiceId != 0; }
};

/// One decision per movement in the chosen direction. The other direction is
/// skipped entirely — money leaving the account is not a customer paying you,
/// and money arriving is not you paying a supplier.
///
/// `alreadyImported` holds the dedup keys of movements recorded on an earlier
/// import; those are skipped too, so re-importing an overlapping period cannot
/// pay the same invoice twice. **One list serves both directions**: a movement
/// is either a credit or a debit, so the two passes never see the same one,
/// and giving them separate lists would let a statement imported twice pay the
/// other direction on the second pass.
std::vector<TransactionMatch> matchTransactions(
    const Statement& statement,
    const std::vector<PayableInvoice>& invoices,
    const std::vector<std::string>& alreadyImported = {},
    MatchDirection direction = MatchDirection::Incoming);

/// True when `number` appears in `text` as a whole run of digits rather than
/// inside a longer one — so invoice 202601 is not found inside 2026010.
bool mentionsNumber(const std::string& text, const std::string& number);

} // namespace fk
