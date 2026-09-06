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

// ImportDialog.h - reviewing a bank statement before anything is recorded.
//
// The rule this screen exists to enforce: nothing is written until Peter has
// looked at it. Confident matches arrive ticked, everything else unticked, and
// any row can be repointed at a different invoice or left alone.
//
// One statement, both directions. Money in is matched against the invoices you
// issued; money out against the ones you received. They are shown together
// because they arrive together — importing the same file twice, once per
// direction, would be a way to pay something twice.
#pragma once

#include "../bank/Matcher.h"
#include "../db/Database.h"

#include <QDialog>
#include <QString>

#include <vector>

class QLabel;
class QTableWidget;
class QPushButton;

namespace fk {
namespace gui {

class ImportDialog : public QDialog {
public:
    ImportDialog(Database& db, const Statement& statement,
                 const QString& sourceFile, const QString& sourceSha,
                 QWidget* parent = nullptr);

    /// How many payments were recorded. Valid after exec() == Accepted.
    int recorded() const { return recorded_; }

private:
    void buildRows();
    void applyImport();
    void updateSummary();

    Database&      db_;
    Statement      statement_;
    QString        sourceFile_;
    QString        sourceSha_;

    /// One row of the review, with the direction it belongs to. The direction
    /// decides which list of invoices its picker offers and which field of
    /// ImportedPayment the answer goes into, so it travels with the match
    /// rather than being worked out again later.
    struct Row {
        TransactionMatch match;
        MatchDirection   direction = MatchDirection::Incoming;
    };

    std::vector<Row>            rows_;
    std::vector<PayableInvoice> invoices_;   ///< issued: what you are owed
    std::vector<PayableInvoice> owed_;       ///< received: what you owe

    QTableWidget*  table_   = nullptr;
    QLabel*        summary_ = nullptr;
    QPushButton*   import_  = nullptr;

    int recorded_ = 0;
};

} // namespace gui
} // namespace fk
