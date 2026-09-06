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

// MainWindow.h - seven tabs: overview, invoices you issued, invoices you
// received, customers, my company, registers and the e-invoicing account.
// That is the app.
#pragma once

#include "../db/Database.h"
#include "../db/InvoiceView.h"

#include <QMainWindow>

#include <vector>

class QAction;
class QComboBox;
class QLineEdit;
class QTableWidget;
class QTabWidget;

namespace fk {

class ApiPage;
class CompanyPage;
class DashboardPage;
class InboxPage;
class RegistersPage;

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(Database& db, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildUi();
    void refreshInvoices();
    /// Shows the invoice tab with `mode` selected in the status filter.
    void showInvoices(const QString& mode);
    void sortInvoicesBy(int column);
    void refreshCustomers();

    void newInvoice(DocType type);
    void editSelectedInvoice();
    void duplicateSelectedInvoice();
    void creditNoteFromSelected();
    void invoiceFromProformas();
    void advanceTaxDocumentFrom();
    std::vector<int64_t> selectedInvoiceIds() const;
    void deleteSelectedInvoice();
    void exportPdf();
    void exportUbl();
    void issueSelectedInvoice();
    void recordPayment();
    void cancelSelectedInvoice();
    void buildToolsMenu();
    void showAbout();
    void importStatement();
    void backupNow();
    void restoreFromBackup();
    void openBackupFolder();
    void reloadEverything();
    void setTestMode(bool on);
    void applyWindowTitle();
    void forceDeleteSelected();
    void wipeAllInvoices();
    void unlockSelectedInvoice();
    void showAuditTrail();

    void newCustomer();
    void editSelectedCustomer();
    void deleteSelectedCustomer();
    bool editCustomerDialog(Customer& c);

    int64_t selectedInvoiceId() const;
    int64_t selectedCustomerId() const;

    Database&     db_;
    QTabWidget*   tabs_      = nullptr;
    QTableWidget* invoices_  = nullptr;
    QComboBox*    filter_    = nullptr;
    QLineEdit*    search_    = nullptr;
    SortBy        sortBy_    = SortBy::IssueDate;
    bool          sortAscending_ = false;
    QAction*      testModeAction_ = nullptr;
    QAction*      forceDelete_    = nullptr;
    QAction*      wipeAll_        = nullptr;
    bool          testMode_       = false;
    QTableWidget* customers_ = nullptr;
    DashboardPage* dashboard_ = nullptr;
    InboxPage*     inbox_     = nullptr;
    CompanyPage*   company_   = nullptr;
    RegistersPage* registers_ = nullptr;
    ApiPage*       api_       = nullptr;
};

} // namespace fk
