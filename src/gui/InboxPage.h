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

// InboxPage.h - the Prijaté tab: invoices somebody sent to you.
//
// Separate from the invoice list on purpose. Money out is not money in, and
// the two lists answer different questions — "who owes me" and "what do I
// owe". Mixing them behind a direction column would make both worse.
//
// Nothing about a received document can be edited. It is somebody else's
// document and, from 2027, the legal record of a supply you have to account
// for. The two things that are yours are a note and whether you have paid it.
#pragma once

#include "../db/Database.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace fk {

class InboxPage : public QWidget {
public:
    explicit InboxPage(Database& db, QWidget* parent = nullptr);

    /// Re-reads the list from the database.
    void reload();

    /// Imports one file. Exposed so the tests can drive it without a file
    /// dialog, and so a delivery service can be wired into the same path
    /// later — whatever a Digitálny poštár turns out to hand over, a file is
    /// the fallback that always works.
    /// `quiet` suppresses the message boxes, for the same reason.
    bool importFile(const QString& path, bool quiet = false);

    /// True when the last successful importFile() found the document already
    /// there rather than adding it.
    bool lastImportWasDuplicate() const { return lastImportWasDuplicate_; }

    /// Chooses the filter by name — "all", "unpaid", "dueSoon", "overdue",
    /// "warnings" — so the dashboard's figures can click through to the rows
    /// behind them, the way the invoice list already does.
    void showFiltered(const QString& mode);

private:
    void importChosenFiles();
    void showSelected();
    void markSelectedPaid();
    void editSelectedNote();
    void saveSelectedXml();
    void deleteSelected();
    void updateButtons();
    int64_t selectedId() const;

    Database&     db_;
    bool          lastImportWasDuplicate_ = false;
    QTableWidget* table_ = nullptr;
    QComboBox*    filter_ = nullptr;
    QLineEdit*    search_ = nullptr;
    QLabel*       summary_ = nullptr;
    QPushButton*  view_   = nullptr;
    QPushButton*  pay_    = nullptr;
    QPushButton*  note_   = nullptr;
    QPushButton*  saveXml_ = nullptr;
    QPushButton*  remove_ = nullptr;
};

} // namespace fk
