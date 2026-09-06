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

// CompanyPage.h - "Moja firma" tab: seller details, bank account, number series.
#pragma once

#include "../db/Database.h"

#include <QWidget>

#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QTableWidget;
class QSpinBox;

namespace fk {

class PartyForm;

class CompanyPage : public QWidget {
public:
    CompanyPage(Database& db, QWidget* parent = nullptr);

    void reload();
    bool save();     // shows its own error box on failure

    /// Emitted through this callback when the active company changes, so the
    /// window can retitle itself and the invoice list can reload.
    std::function<void()> onActiveCompanyChanged;

private:
    void updatePreview();
    void applyVatModeHint();
    void loadAccounts(const Company& company);
    std::vector<BankAccount> collectAccounts() const;
    void addAccountRow(const BankAccount& account);
    void removeSelectedAccount();
    /// Fills the IBAN from the domestic number, or the other way round.
    void completeAccountRow(int row, int column);
    void chooseLogo();
    void clearLogo();
    void showLogo();
    void flagAccountConflict(int row);
    void reloadCompanyList();
    void switchCompany(int index);
    void addCompany();
    void removeCompany();
    bool hasUnsavedChanges() const;

    Database&       db_;
    PartyForm*      party_    = nullptr;
    QTableWidget*   accounts_ = nullptr;
    QPlainTextEdit* registry_ = nullptr;
    QLabel*         logoView_ = nullptr;
    std::string     logo_;          ///< the bytes, edited here and saved with the rest
    std::string     logoPath_;      ///< where they came from, kept in step with logo_
    QLineEdit*      prefix_   = nullptr;
    QSpinBox*       year_     = nullptr;
    QSpinBox*       next_     = nullptr;
    QSpinBox*       padding_  = nullptr;
    QLabel*         preview_  = nullptr;
    QComboBox*      vatMode_  = nullptr;
    QComboBox*      company_  = nullptr;
    bool            switching_ = false;
    int64_t         loadedCompanyId_ = 0;
};

} // namespace fk
