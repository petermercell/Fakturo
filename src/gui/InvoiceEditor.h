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

// InvoiceEditor.h - the one screen where an invoice is actually written.
#pragma once

#include "../db/Database.h"
#include "../model/Model.h"

#include <QDialog>

class QComboBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QTableWidget;

namespace fk {

class InvoiceEditor : public QDialog {
public:
    InvoiceEditor(Database& db, const Invoice& invoice, QWidget* parent = nullptr);

    /// Valid after exec() == QDialog::Accepted.
    const Invoice& invoice() const { return inv_; }

    /// Convenience: a blank invoice with the next number, today's date and +14 days.
    static Invoice blank(Database& db, DocType type = DocType::Invoice);

private:
    void buildUi();
    void loadFromModel();
    void collectToModel();
    void recalcTotals();
    /// Inserts at `atRow`, or appends when `atRow` is negative.
    void addLine(const InvoiceLine& line, int atRow = -1);
    /// A fresh line with the right VAT for this invoice, at `atRow` or at the
    /// end. Returns the row it went to.
    int  insertEmptyLine(int atRow = -1);
    void appendEmptyLine();
    void removeSelectedLines();

    /// `lineNo` of 0 means the whole document.
    void addAllowanceRow(const Allowance& a, int lineNo);
    void appendAllowance(bool isCharge);
    /// Refills every Položka picker after the lines change, keeping each row
    /// pointed at the line it was pointed at.
    void refreshAllowanceTargets();
    void removeSelectedAllowances();
    /// One row of the discount table, read and priced. `lineNo` comes back as
    /// 0 for the whole document. Shared by collecting and by the live redisplay
    /// so the Suma shown can never disagree with the Suma used.
    Allowance allowanceAt(int row, int* lineNo) const;
    /// Each row with the line it belongs to; 0 means the whole document.
    std::vector<std::pair<int, Allowance>> collectAllowances() const;
    /// Puts each collected row on its line or on the document.
    void placeAllowances();
    /// Fills in BT-114 so the amount due lands on a whole unit.
    void roundToWholeUnit();
    /// Picks a saved line and puts it on the invoice, below the selected row.
    void insertFromCatalogue();
    /// Saves the selected row for next month. Everything about it stays
    /// editable afterwards: a catalogue entry is a starting point, not a rule.
    void saveLineToCatalogue();
    /// The currency this seller accounts for VAT in: euro in Slovakia, crowns
    /// in Czechia. Follows the seller's country, not the active company's.
    std::string accountingCurrency() const;
    /// Shows, hides and relabels the exchange-rate row. It appears only for a
    /// VAT payer writing a document in a currency other than the one their VAT
    /// is owed in; for everybody else the row is simply not there. It does not
    /// look at whether *this* document happens to carry VAT — a row that came
    /// and went as lines were typed would be worse than one that is sometimes
    /// left blank, and leaving it blank costs nothing.
    void updateExchangeRow();
    void pickCustomer(int index);
    void updateBuyerInfo();
    void editBuyerDetails();
    void runValidation();
    void applyReadOnly();
    /// Re-derives the VAT treatment from the two parties and shows what it chose.
    void applySupplyRegime();
    void accept() override;

    Database& db_;
    Invoice   inv_;
    bool      updating_ = false;
    /// Cached from the company: when false, the VAT columns are hidden and
    /// every line is forced to "out of scope".
    bool      sellerChargesVat_ = true;

    QLineEdit*      number_    = nullptr;
    QComboBox*      docType_   = nullptr;
    QDateEdit*      issue_     = nullptr;
    QDateEdit*      taxPoint_  = nullptr;
    QDateEdit*      due_       = nullptr;
    QLineEdit*      currency_  = nullptr;
    QLineEdit*      vs_        = nullptr;
    QLineEdit*      ks_        = nullptr;
    QLineEdit*      ss_        = nullptr;
    QComboBox*      customer_  = nullptr;
    QLabel*         buyerInfo_ = nullptr;
    QLineEdit*      buyerRef_  = nullptr;
    QLineEdit*      orderRef_  = nullptr;
    QLineEdit*      preceding_ = nullptr;
    QPlainTextEdit* exemption_ = nullptr;
    QPlainTextEdit* note_      = nullptr;
    QTableWidget*   lines_     = nullptr;
    QTableWidget*   allowances_ = nullptr;
    QLineEdit*      rounding_   = nullptr;

    /// Whether the exchange-rate row applies, kept as a value rather than read
    /// back from isVisible(): a widget in a dialog that has not been shown is
    /// not visible, and the tests run without a screen.
    bool            fxApplies_ = false;
    /// The currency the number in the rate box was typed for, so that changing
    /// the invoice currency cannot leave a rate belonging to the old one.
    std::string     fxRateFor_;
    /// True once the person has picked a rate date themselves; until then it
    /// follows the tax point, which is what § 26 makes it.
    bool            fxDateChosen_ = false;
    QWidget*        fxRow_     = nullptr;
    QLabel*         fxSuffix_  = nullptr;
    QLineEdit*      fxRate_    = nullptr;
    QDateEdit*      fxDate_    = nullptr;

    QLabel*         totals_    = nullptr;
    QLabel*         regime_    = nullptr;
};

} // namespace fk
