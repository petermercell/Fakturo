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

// UblReader.h - reading an EN 16931 / Peppol BIS Billing 3.0 invoice.
//
// The mirror of UblWriter, and the half this application needs by 1 January
// 2027: from that date every taxable person must be able to *receive* an
// eFaktúra, whether or not they issue one.
//
// Two things make this different from the writer, and both are about trust.
//
// The writer produces documents from a model it controls. The reader is handed
// bytes by somebody else, who may have written them with a different tool, a
// different reading of the standard, or a mistake. So it does not merely
// populate an Invoice: it also reports **what the document says its own totals
// are**, and where those disagree with what this application computes from the
// same parts. A silent disagreement would be the worst outcome — a payable
// figure on screen that is not the figure the sender is asking for.
//
// The other difference is that a received document is evidence. The caller is
// expected to keep the original bytes; nothing here is lossy on purpose, but
// EN 16931 carries fields this model has no place for, and those are dropped.
#pragma once

#include "../model/Model.h"

#include <string>
#include <vector>

namespace fk {

/// The totals as the *document* states them (BT-106 … BT-115), before this
/// application has recomputed anything. Read verbatim, so a document that does
/// not add up still reports what it claimed.
struct StatedTotals {
    Dec lineExtension;    // BT-106
    Dec allowanceTotal;   // BT-107
    Dec chargeTotal;      // BT-108
    Dec taxExclusive;     // BT-109
    Dec taxAmount;        // BT-110
    Dec taxInclusive;     // BT-112
    Dec prepaidAmount;    // BT-113
    Dec rounding;         // BT-114
    Dec payable;          // BT-115

    /// BT-111, when the document restates its VAT in another currency.
    bool restated = false;
    Dec  taxAmountAccounting;

    /// The VAT breakdown as stated (BG-23), rather than as recomputed.
    std::vector<VatBreakdownRow> vat;
};

struct ReadResult {
    bool        ok = false;
    std::string error;          ///< why it could not be read at all

    /// The document as this application models it. `seller` and `buyer` are
    /// both filled from the document, not from your own company list.
    Invoice invoice;

    StatedTotals stated;

    /// Where the document's own figures and this application's recomputation
    /// of them disagree, in words, one per line. Empty is the good case.
    ///
    /// Not an error: plenty of real invoices are a cent out somewhere, and
    /// refusing to show one because of that would be worse than showing it
    /// with a note. But never silent.
    std::vector<std::string> discrepancies;

    bool agrees() const { return discrepancies.empty(); }
};

/// Reads a UBL 2.1 `Invoice` or `CreditNote`. Namespace prefixes are ignored,
/// so a sender's choice of `ubl:` or no prefix at all makes no difference.
ReadResult readUbl(const std::string& xml);

} // namespace fk
