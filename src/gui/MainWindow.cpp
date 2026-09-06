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

#include "MainWindow.h"

#include "../bank/Statement.h"
#include "../core/Sha256.h"
#include "../pdf/PdfRenderer.h"
#include "../sk/Slovak.h"
#include "../ubl/UblWriter.h"
#include "../ubl/Validator.h"
#include "ApiPage.h"
#include "CompanyPage.h"
#include "DashboardPage.h"
#include "InboxPage.h"
#include "GuiUtil.h"
#include "ImportDialog.h"
#include "InvoiceActions.h"
#include "InvoiceEditor.h"
#include "PartyForm.h"
#include "RegistersPage.h"

#include <QAction>
#include <QBrush>
#include <QColor>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateEdit>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QPoint>
#include <QMenuBar>
#include <QApplication>
#include <QDate>
#include <QDesktopServices>
#include <QUrl>
#include <QSignalBlocker>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QStringConverter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextStream>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

namespace fk {

using namespace fk::gui;

namespace {

QString lastDir() {
    QSettings s;
    return s.value("lastExportDir",
                   QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString();
}

void rememberDir(const QString& file) {
    QSettings s;
    s.setValue("lastExportDir", QFileInfo(file).absolutePath());
}

QTableWidgetItem* cell(const QString& text, bool rightAlign = false) {
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    if (rightAlign) item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return item;
}

} // namespace

MainWindow::MainWindow(Database& db, QWidget* parent) : QMainWindow(parent), db_(db) {
    resize(1080, 680);
    buildUi();
    applyWindowTitle();
    refreshInvoices();
    refreshCustomers();
}

void MainWindow::buildUi() {
    tabs_      = new QTabWidget(this);
    invoices_  = new QTableWidget(this);
    invoices_->setObjectName("invoices");     // reached by name from the GUI tests
    customers_ = new QTableWidget(this);
    customers_->setObjectName("customers");
    dashboard_ = new DashboardPage(db_, this);
    inbox_     = new InboxPage(db_, this);
    company_   = new CompanyPage(db_, this);
    registers_ = new RegistersPage(db_, this);
    api_       = new ApiPage(db_, this);

    // ---------------------------------------------------------- invoices tab
    filter_ = new QComboBox(this);
    filter_->setObjectName("filter");
    filter_->addItem("Všetky doklady", "all");
    filter_->addItem("Rozpracované", "draft");
    filter_->addItem("Neuhradené", "unpaid");
    filter_->addItem("Po splatnosti", "overdue");
    connect(filter_, &QComboBox::currentIndexChanged, this, [this](int) { refreshInvoices(); });

    // One box, not four. Someone looking for "Novák" and someone looking for
    // "20260042" both just type it; the search covers number, customer,
    // variable symbol and amount, and composes with the status filter rather
    // than replacing it.
    search_ = new QLineEdit(this);
    search_->setObjectName("search");
    search_->setPlaceholderText("Hľadať – číslo, odberateľ, VS, suma");
    search_->setClearButtonEnabled(true);
    search_->setMinimumWidth(260);
    connect(search_, &QLineEdit::textChanged, this, [this](const QString&) { refreshInvoices(); });

    invoices_->setColumnCount(8);
    invoices_->setHorizontalHeaderLabels(
        {"Číslo", "Typ", "Stav", "Vystavená", "Splatnosť", "Odberateľ", "Suma", "Zostáva"});
    invoices_->setSelectionBehavior(QAbstractItemView::SelectRows);
    // Extended, because settling several proformas with one invoice is the
    // whole point of the proforma workflow.
    invoices_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    invoices_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    invoices_->verticalHeader()->setVisible(false);
    invoices_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    connect(invoices_, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem*) { editSelectedInvoice(); });

    // Sorting is done on the data before the table is filled, not by
    // QTableWidget, which would compare the displayed text — and "1000.00"
    // sorts below "9.00" as text. The indicator is set by hand to match.
    invoices_->horizontalHeader()->setSectionsClickable(true);
    invoices_->horizontalHeader()->setSortIndicatorShown(true);
    connect(invoices_->horizontalHeader(), &QHeaderView::sectionClicked, this,
            [this](int column) { sortInvoicesBy(column); });

    auto* invBar = new QToolBar(this);
    auto addTool = [this](QToolBar* bar, const QString& text, auto&& handler) {
        auto* action = new QAction(text, this);
        connect(action, &QAction::triggered, this, handler);
        bar->addAction(action);
        return action;
    };

    // Each action is created once and then placed wherever it belongs — the
    // toolbar, the Ďalšie menu, the right-click menu. Sharing the objects
    // rather than making a second set means the three can never drift apart:
    // rename one and it is renamed everywhere, connect one and it works
    // everywhere.
    auto make = [this](const QString& text, auto&& handler) {
        auto* action = new QAction(text, this);
        connect(action, &QAction::triggered, this, handler);
        return action;
    };

    auto* actNewInvoice  = make("Nová faktúra",  [this] { newInvoice(DocType::Invoice); });
    auto* actNewProforma = make("Nová proforma", [this] { newInvoice(DocType::Proforma); });
    auto* actOpen        = make("Otvoriť",       [this] { editSelectedInvoice(); });
    auto* actIssue       = make("Vystaviť",      [this] { issueSelectedInvoice(); });
    auto* actPdf         = make("PDF",           [this] { exportPdf(); });
    auto* actUbl         = make("Peppol XML",    [this] { exportUbl(); });
    auto* actFromProfs   = make("Faktúra z proforiem", [this] { invoiceFromProformas(); });
    auto* actPayment     = make("Platba",        [this] { recordPayment(); });
    auto* actDuplicate   = make("Duplikovať",    [this] { duplicateSelectedInvoice(); });
    auto* actCreditNote  = make("Dobropis",      [this] { creditNoteFromSelected(); });
    auto* actAdvanceDoc  = make("Doklad k platbe", [this] { advanceTaxDocumentFrom(); });
    auto* actCancel      = make("Stornovať",     [this] { cancelSelectedInvoice(); });
    auto* actHistory     = make("História",      [this] { showAuditTrail(); });
    auto* actUnlock      = make("Odomknúť",      [this] { unlockSelectedInvoice(); });
    auto* actDelete      = make("Zmazať",        [this] { deleteSelectedInvoice(); });

    // ------------------------------------------------------------- the bar
    // What gets done to a document every day. The rest lives behind one
    // button, because sixteen of these pushed the search box off a laptop
    // screen — and a search you cannot see is a search you do not use.
    for (QAction* a : {actNewInvoice, actNewProforma, actOpen, actIssue}) invBar->addAction(a);
    invBar->addSeparator();
    for (QAction* a : {actPdf, actUbl, actFromProfs}) invBar->addAction(a);
    invBar->addSeparator();

    // ---------------------------------------------------------- Ďalšie
    auto* moreMenu = new QMenu(this);
    moreMenu->setObjectName("moreMenu");
    moreMenu->addAction(actPayment);
    moreMenu->addSeparator();
    for (QAction* a : {actDuplicate, actCreditNote, actAdvanceDoc}) moreMenu->addAction(a);
    moreMenu->addSeparator();
    for (QAction* a : {actCancel, actHistory, actUnlock, actDelete}) moreMenu->addAction(a);

    auto* moreButton = new QToolButton(this);
    moreButton->setObjectName("moreButton");
    moreButton->setText("Ďalšie");
    moreButton->setMenu(moreMenu);
    // InstantPopup, not MenuButtonPopup: there is no default action to fire,
    // so a split button would have a dead half.
    moreButton->setPopupMode(QToolButton::InstantPopup);
    moreButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    invBar->addWidget(moreButton);

    // ------------------------------------------------------- right-click
    // Everything, not just the overflow: a right-click on a row is where the
    // hand already is, and having to remember which half of the actions live
    // where is exactly the friction this is meant to remove. Row actions
    // first, since that is what was clicked on; making a new document last.
    auto* contextMenu = new QMenu(this);
    contextMenu->setObjectName("contextMenu");
    for (QAction* a : {actOpen, actIssue}) contextMenu->addAction(a);
    contextMenu->addSeparator();
    contextMenu->addAction(actPayment);
    contextMenu->addSeparator();
    for (QAction* a : {actPdf, actUbl}) contextMenu->addAction(a);
    contextMenu->addSeparator();
    for (QAction* a : {actDuplicate, actCreditNote, actFromProfs, actAdvanceDoc})
        contextMenu->addAction(a);
    contextMenu->addSeparator();
    for (QAction* a : {actCancel, actHistory, actUnlock, actDelete}) contextMenu->addAction(a);
    contextMenu->addSeparator();
    for (QAction* a : {actNewInvoice, actNewProforma}) contextMenu->addAction(a);

    invoices_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(invoices_, &QTableWidget::customContextMenuRequested, this,
            [this, contextMenu](const QPoint& at) {
                contextMenu->popup(invoices_->viewport()->mapToGlobal(at));
            });

    invBar->addSeparator();
    invBar->addWidget(filter_);
    invBar->addWidget(search_);

    auto* invPage = new QWidget(this);
    auto* invLayout = new QVBoxLayout(invPage);
    invLayout->setContentsMargins(0, 0, 0, 0);
    invLayout->addWidget(invBar);
    invLayout->addWidget(invoices_);

    // --------------------------------------------------------- customers tab
    customers_->setColumnCount(5);
    customers_->setHorizontalHeaderLabels({"Názov", "IČO", "IČ DPH", "Mesto", "E-mail"});
    customers_->setSelectionBehavior(QAbstractItemView::SelectRows);
    customers_->setSelectionMode(QAbstractItemView::SingleSelection);
    customers_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    customers_->verticalHeader()->setVisible(false);
    customers_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    connect(customers_, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem*) { editSelectedCustomer(); });

    auto* custBar = new QToolBar(this);
    addTool(custBar, "Nový odberateľ", [this] { newCustomer(); });
    addTool(custBar, "Upraviť",        [this] { editSelectedCustomer(); });
    addTool(custBar, "Odstrániť",      [this] { deleteSelectedCustomer(); });

    auto* custPage = new QWidget(this);
    auto* custLayout = new QVBoxLayout(custPage);
    custLayout->setContentsMargins(0, 0, 0, 0);
    custLayout->addWidget(custBar);
    custLayout->addWidget(customers_);

    // Prijaté sits next to Faktúry rather than inside it. Money out is not
    // money in, and the two lists answer different questions.
    tabs_->addTab(dashboard_, "Prehľad");
    tabs_->addTab(invPage, "Faktúry");
    tabs_->addTab(inbox_, "Prijaté");
    tabs_->addTab(custPage, "Odberatelia");
    tabs_->addTab(company_, "Moja firma");
    tabs_->addTab(registers_, "Registre");
    // Last, because it is the tab you visit once and then forget: the account
    // the documents will travel through, not the documents.
    tabs_->addTab(api_, "e-Faktúra");
    // By widget, not by index: inserting a tab used to silently renumber every
    // branch below it, and the wrong page would refresh.
    connect(tabs_, &QTabWidget::currentChanged, this, [this, invPage, custPage](int i) {
        QWidget* page = tabs_->widget(i);
        if      (page == dashboard_) dashboard_->reload();
        else if (page == invPage)    refreshInvoices();
        else if (page == inbox_)     inbox_->reload();
        else if (page == custPage)   refreshCustomers();
        else if (page == company_)   company_->reload();
        else if (page == registers_) registers_->refresh();
        else if (page == api_)       api_->reload();
    });

    // The dashboard's figures are links: clicking one shows the documents
    // behind it rather than leaving you to find them.
    dashboard_->onShowInvoices = [this](const QString& mode) { showInvoices(mode); };
    dashboard_->onShowPayables = [this, inboxPage = inbox_](const QString& mode) {
        if (tabs_) tabs_->setCurrentWidget(inboxPage);
        inboxPage->showFiltered(mode);
    };

    setCentralWidget(tabs_);
    buildToolsMenu();

    // Switching company changes which documents exist, how they are numbered
    // and who they are issued by, so the whole window follows it.
    company_->onActiveCompanyChanged = [this] {
        refreshInvoices();
        applyWindowTitle();
        // The SAPI account belongs to the company, not to the app: leaving the
        // previous company's client id on screen would let it be saved under
        // the new one.
        if (api_) api_->reload();
    };
    statusBar()->showMessage("Pripravené");
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // Quitting during the VAT import would tear the page down while a worker
    // thread is still writing to the database.
    if (registers_ && registers_->isBusy()) {
        QMessageBox::information(this, "Prebieha import",
            "Prebieha aktualizácia registrov finančnej správy. Počkajte, kým sa dokončí.");
        event->ignore();
        return;
    }
    event->accept();
}

// --------------------------------------------------------------------- backup
void MainWindow::backupNow() {
    if (registers_ && registers_->isBusy()) {
        QMessageBox::information(this, "Záloha",
            "Prebieha aktualizácia registrov. Počkajte, kým sa dokončí.");
        return;
    }

    const QString suggested =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/" +
        "zaloha-fakturo-" + QDate::currentDate().toString("yyyy-MM-dd") + ".db";

    QString path = QFileDialog::getSaveFileName(this, "Zálohovať databázu", suggested,
                                                "Databáza Fakturo (*.db)");
    if (path.isEmpty()) return;
    if (!path.endsWith(".db")) path += ".db";

    // Write beside the target and rename: deleting first would leave the user
    // with neither the old backup nor a new one if the write fails.
    const QString staging = path + ".new";
    QFile::remove(staging);
    if (!db_.backupTo(sstr(staging))) {
        QMessageBox::critical(this, "Záloha", qstr(db_.lastError()));
        QFile::remove(staging);
        return;
    }
    QFile::remove(path);
    if (!QFile::rename(staging, path)) {
        QMessageBox::critical(this, "Záloha", "Zálohu sa nepodarilo uložiť na: " + path);
        return;
    }
    const double megabytes = QFileInfo(path).size() / 1048576.0;
    statusBar()->showMessage(
        QString("Záloha uložená: %1  (%2 MB, bez stiahnutých registrov)")
            .arg(path, QString::number(megabytes, 'f', 1)), 8000);
}

void MainWindow::restoreFromBackup() {
    if (registers_ && registers_->isBusy()) {
        QMessageBox::information(this, "Obnova",
            "Prebieha aktualizácia registrov. Počkajte, kým sa dokončí.");
        return;
    }

    const QString path = QFileDialog::getOpenFileName(this, "Obnoviť zo zálohy",
        qstr(db_.backupDirectory()), "Databáza Fakturo (*.db)");
    if (path.isEmpty()) return;

    const Database::BackupInfo info = Database::inspectBackup(sstr(path));
    if (!info.valid) {
        QMessageBox::critical(this, "Obnova",
            "Tento súbor sa nedá použiť:\n\n" + qstr(info.error));
        return;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle("Obnoviť zo zálohy");
    box.setText(QString("Nahradiť súčasné údaje zálohou?"));
    box.setInformativeText(
        QString("Záloha obsahuje %1 dokladov a %2 firiem.\n\n"
                "Súčasné údaje sa pred obnovou uložia do priečinka so zálohami, "
                "takže sa dá vrátiť späť.")
            .arg(QString::number(info.invoices), QString::number(info.companies)));
    box.setStandardButtons(QMessageBox::Cancel);
    auto* restore = box.addButton("Obnoviť", QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != restore) return;

    std::string safety;
    if (!db_.restoreFrom(sstr(path), &safety)) {
        const QString detail = qstr(db_.lastError());
        if (!db_.isOpen()) {
            // Showing an empty window here would read as "all my data is gone".
            QMessageBox::critical(this, "Obnova zlyhala",
                "Obnovu sa nepodarilo dokončiť a databáza nie je otvorená.\n\n" + detail +
                (safety.empty() ? QString()
                                : "\n\nPôvodné údaje sú uložené ako:\n" + qstr(safety)) +
                "\n\nAplikácia sa ukončí. Súbor zálohy skopírujte na miesto databázy ručne.");
            qApp->quit();
            return;
        }
        QMessageBox::critical(this, "Obnova",
            detail + (safety.empty() ? QString()
                                     : "\n\nPôvodné údaje zostali uložené ako:\n" + qstr(safety)));
        reloadEverything();
        return;
    }

    reloadEverything();
    QMessageBox::information(this, "Obnova",
        "Údaje boli obnovené.\n\nPredchádzajúci stav je uložený ako:\n" + qstr(safety));
}

void MainWindow::openBackupFolder() {
    const QString dir = qstr(db_.backupDirectory());
    if (dir.isEmpty()) return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

/// After a restore every page is showing data from a database that no longer
/// exists, so all of them reload — including the company selector and the title.
void MainWindow::reloadEverything() {
    // Test mode is stored in the database, so a restore can change it. Leaving
    // the permanent-delete actions armed from the previous file would be a
    // nasty way to find that out.
    if (testModeAction_) {
        const bool remembered = db_.setting("dev.testMode", "0") == "1";
        const QSignalBlocker blocker(testModeAction_);
        testModeAction_->setChecked(remembered);
        setTestMode(remembered);
    }
    if (company_)   company_->reload();
    if (api_)       api_->reload();
    if (registers_) registers_->refresh();
    if (inbox_)     inbox_->reload();
    refreshCustomers();
    refreshInvoices();
    if (dashboard_) dashboard_->reload();
    applyWindowTitle();
}

// ------------------------------------------------------------------ test mode
// Hard deletion exists only so the app can be exercised without accumulating
// junk documents. It is off by default, has to be switched on deliberately,
// and while it is on the window title says so — because the whole point of the
// draft/issued/cancelled model is that an issued number stays in the series.

/// Reads a statement — Fio's CSV or a camt.053 XML — and hands it to the
/// review screen. Nothing is recorded here: this only gets the file onto the
/// screen. The format is sniffed from the contents, not the extension.
void MainWindow::importStatement() {
    const QString path = QFileDialog::getOpenFileName(
        this, "Vyberte výpis z banky", QString(),
        "Bankový výpis (*.csv *.CSV *.xml *.XML);;CSV z Fio banky (*.csv *.CSV);;"
        "camt.053 (*.xml *.XML);;Všetky súbory (*)");
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this, "Import", "Súbor sa nepodarilo otvoriť:\n" + path);
        return;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    const std::string text(bytes.constData(), static_cast<size_t>(bytes.size()));
    const Statement statement = parseStatement(text);
    if (!statement.ok) {
        QMessageBox::warning(this, "Import", qstr(statement.error));
        return;
    }

    // The account on the statement should be one of this company's. Saying so
    // beats importing someone else's payments into these books.
    const Company me = db_.activeCompany();
    bool knownAccount = statement.iban.empty();
    for (const BankAccount& a : me.accounts)
        if (!a.iban.empty() && sk::normalizeIban(a.iban) == sk::normalizeIban(statement.iban))
            knownAccount = true;
    if (!knownAccount &&
        QMessageBox::question(
            this, "Iný účet",
            "Výpis je z účtu " + qstr(statement.iban) +
                ",\nktorý nie je medzi účtami firmy " + qstr(me.name) + ".\n\nPokračovať?",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    ImportDialog dialog(db_, statement, QFileInfo(path).fileName(),
                        qstr(Sha256::hexOf(text)), this);
    if (dialog.exec() != QDialog::Accepted) return;

    reloadEverything();
    QMessageBox::information(
        this, "Import",
        QString("Zaúčtovaných úhrad: %1.").arg(dialog.recorded()) +
            (statement.warnings.empty()
                 ? QString()
                 : QString("\nPreskočených riadkov: %1.").arg(statement.warnings.size())));
}

void MainWindow::showAbout() {
    // The version comes from CMake so it cannot drift from the one in the
    // bundle. Schema and Qt are here because they are the two things worth
    // knowing when something has gone wrong and the answer has to come back
    // from Peter rather than from a log.
    QMessageBox box(this);
    box.setWindowTitle("O programe Fakturo");
    box.setTextFormat(Qt::PlainText);
    box.setText("Fakturo " FAKTURO_VERSION);
    box.setInformativeText(
        "Made by Peter Mercell and Claude 2026\n\n"
        "Fakturačný program pre Slovensko a Česko.\n"
        "Schéma databázy " + QString::number(db_.schemaVersion()) +
        " · Qt " + QString(qVersion()) +
        // GPLv3 §5(d): a program with an interactive interface has to show
        // the Appropriate Legal Notices somewhere, and the About box is that
        // somewhere. The warranty disclaimer is part of what it must say.
        "\n\nCopyright © 2026 Peter Mercell\n"
        "Licencia GNU GPL verzia 3 alebo novšia\n"
        "https://www.gnu.org/licenses/gpl-3.0.html\n"
        "Program sa poskytuje BEZ AKEJKOĽVEK ZÁRUKY.");
    box.setIcon(QMessageBox::NoIcon);
    box.exec();
}

void MainWindow::buildToolsMenu() {
    auto* tools = menuBar()->addMenu("Nástroje");

    // AboutRole, so macOS lifts it out of this menu and into the application
    // menu next to Quit, where anyone looking for it will look. On Windows and
    // Linux it stays here, which is where anyone there would look.
    auto* about = tools->addAction("O programe Fakturo…", this, [this] { showAbout(); });
    about->setMenuRole(QAction::AboutRole);
    tools->addSeparator();

    tools->addAction("Zálohovať databázu…", this, [this] { backupNow(); });
    tools->addAction("Obnoviť zo zálohy…",  this, [this] { restoreFromBackup(); });
    tools->addAction("Otvoriť priečinok so zálohami", this, [this] { openBackupFolder(); });
    tools->addSeparator();

    tools->addAction("Importovať bankový výpis…", this, [this] { importStatement(); });
    tools->addSeparator();

    testModeAction_ = tools->addAction("Testovací režim");
    testModeAction_->setCheckable(true);
    connect(testModeAction_, &QAction::toggled, this, [this](bool on) { setTestMode(on); });

    tools->addSeparator();

    forceDelete_ = tools->addAction("Zmazať vybraný doklad natrvalo");
    connect(forceDelete_, &QAction::triggered, this, [this] { forceDeleteSelected(); });

    wipeAll_ = tools->addAction("Zmazať všetky doklady…");
    connect(wipeAll_, &QAction::triggered, this, [this] { wipeAllInvoices(); });

    // Survives restarts, so a forgotten test mode is still visible in the title
    // rather than silently re-arming itself.
    const bool remembered = db_.setting("dev.testMode", "0") == "1";
    const QSignalBlocker blocker(testModeAction_);
    testModeAction_->setChecked(remembered);
    setTestMode(remembered);
}

void MainWindow::applyWindowTitle() {
    // The active company belongs in the title: issuing from the wrong entity
    // is the one mistake multi-company makes easy, and it is not undoable.
    const Company active = db_.activeCompany();
    QString title = "Fakturo";
    if (!active.name.empty()) title += " — " + qstr(active.name);
    if (testMode_)            title += "  ·  TESTOVACÍ REŽIM";
    setWindowTitle(title);
}

void MainWindow::setTestMode(bool on) {
    testMode_ = on;
    if (forceDelete_) forceDelete_->setEnabled(on);
    if (wipeAll_)     wipeAll_->setEnabled(on);
    db_.setSetting("dev.testMode", on ? "1" : "0");

    applyWindowTitle();
    if (on)
        statusBar()->showMessage(
            "Testovací režim: doklady sa dajú mazať natrvalo, aj vystavené.", 6000);
}

void MainWindow::forceDeleteSelected() {
    if (!testMode_) return;
    const int64_t id = selectedInvoiceId();
    if (id == 0) return;
    Invoice inv;
    if (!db_.loadInvoice(id, inv)) return;

    const auto answer = QMessageBox::warning(this, "Zmazať natrvalo",
        "Doklad " + qstr(inv.number) + " sa zmaže aj s platbami a históriou. "
        "V číselnom rade zostane medzera.\n\n"
        "Toto je určené len na testovanie. Pokračovať?",
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;

    if (!db_.forceDeleteInvoice(id)) {
        QMessageBox::critical(this, "Chyba", qstr(db_.lastError()));
        return;
    }
    refreshInvoices();
    statusBar()->showMessage("Zmazané natrvalo: " + qstr(inv.number), 4000);
}

void MainWindow::wipeAllInvoices() {
    if (!testMode_) return;

    const int count = static_cast<int>(db_.invoiceList().size());
    if (count == 0) {
        QMessageBox::information(this, "Zmazať všetky doklady", "Žiadne doklady nie sú.");
        return;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle("Zmazať všetky doklady");
    box.setText(QString("Zmazať všetkých %1 dokladov?").arg(count));
    box.setInformativeText(
        "Zmažú sa aj všetky platby a história. Odberatelia, údaje firmy, "
        "prijaté faktúry a stiahnuté registre zostanú.\n\n"
        "Toto sa nedá vrátiť späť.");
    box.setStandardButtons(QMessageBox::Cancel);
    auto* wipe = box.addButton("Zmazať všetko", QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != wipe) return;

    const int removed = db_.forceDeleteAllInvoices();

    // Testing usually wants the numbering back at the start too.
    if (QMessageBox::question(this, "Číselný rad",
            "Vynulovať aj číselný rad na 1?") == QMessageBox::Yes) {
        NumberSeries s = db_.series();
        s.next = 1;
        db_.setSeries(s);
    }

    refreshInvoices();
    statusBar()->showMessage(QString("Zmazaných dokladov: %1").arg(removed), 5000);
}

// ---------------------------------------------------------------- refreshing
/// Shows the invoice tab with `mode` chosen in the status filter. Called from
/// the dashboard, whose figures are meant to be clickable.
void MainWindow::showInvoices(const QString& mode) {
    if (filter_) {
        const int index = filter_->findData(mode);
        if (index >= 0) filter_->setCurrentIndex(index);
    }
    if (search_) search_->clear();
    if (tabs_) tabs_->setCurrentIndex(1);       // the invoice list
    refreshInvoices();
}

/// A click on a column heading. The same column again reverses; a different
/// one starts from whichever direction is most useful for it — newest and
/// largest first for dates and amounts, A to Z for names.
void MainWindow::sortInvoicesBy(int column) {
    static const SortBy columns[] = {
        SortBy::Number, SortBy::Type, SortBy::Status, SortBy::IssueDate,
        SortBy::DueDate, SortBy::Customer, SortBy::Total, SortBy::Outstanding};
    if (column < 0 || column >= static_cast<int>(std::size(columns))) return;

    const SortBy chosen = columns[column];
    if (chosen == sortBy_) sortAscending_ = !sortAscending_;
    else { sortBy_ = chosen; sortAscending_ = defaultAscending(chosen); }
    refreshInvoices();
}

void MainWindow::refreshInvoices() {
    auto rows = db_.invoiceList();
    const std::string today = sk::todayIso();
    const QString     mode  = filter_ ? filter_->currentData().toString() : "all";
    const std::string query = search_ ? sstr(search_->text()) : std::string();

    // Sorted here rather than by the table, which compares displayed text.
    sortInvoices(rows, sortBy_, sortAscending_);
    if (auto* header = invoices_->horizontalHeader()) {
        static const SortBy columns[] = {
            SortBy::Number, SortBy::Type, SortBy::Status, SortBy::IssueDate,
            SortBy::DueDate, SortBy::Customer, SortBy::Total, SortBy::Outstanding};
        for (int c = 0; c < static_cast<int>(std::size(columns)); ++c)
            if (columns[c] == sortBy_)
                header->setSortIndicator(c, sortAscending_ ? Qt::AscendingOrder
                                                           : Qt::DescendingOrder);
    }

    int hidden = 0;
    invoices_->setRowCount(0);
    for (const auto& v : rows) {
        const bool overdue = v.isOverdue(today);
        if (mode == "draft"   && v.state != InvoiceState::Draft) continue;
        if (mode == "unpaid"  && (v.state != InvoiceState::Issued || v.isFullyPaid())) continue;
        if (mode == "overdue" && !overdue) continue;

        // Counted after the status filter, so "3 of 7" means three of the
        // seven this filter would otherwise show — not three of everything.
        if (!matchesSearch(v, query)) { ++hidden; continue; }

        const int r = invoices_->rowCount();
        invoices_->insertRow(r);

        auto* first = cell(qstr(v.number));
        first->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(v.id));
        invoices_->setItem(r, 0, first);
        invoices_->setItem(r, 1, cell(docTypeLabel(v.type)));

        QString status = stateLabel(v.state);
        if (v.state == InvoiceState::Issued) {
            if (v.isFullyPaid())  status = "Uhradená";
            else if (overdue)     status = QString("Po splatnosti");
        }
        invoices_->setItem(r, 2, cell(status));
        invoices_->setItem(r, 3, cell(qstr(sk::formatDateSk(v.issueDate))));
        invoices_->setItem(r, 4, cell(qstr(sk::formatDateSk(v.dueDate))));
        invoices_->setItem(r, 5, cell(qstr(v.buyerName)));
        invoices_->setItem(r, 6, cell(decToUi(v.total) + " " + qstr(v.currency), true));
        invoices_->setItem(r, 7,
            cell(v.isFullyPaid() ? QString("—") : decToUi(v.outstanding()), true));

        // Colour carries the same information as the text, never instead of it.
        QBrush brush;
        if (overdue)                              brush = QBrush(QColor(0xB0, 0x00, 0x20));
        else if (v.state == InvoiceState::Draft)  brush = QBrush(QColor(0x88, 0x88, 0x88));
        else if (v.state == InvoiceState::Cancelled) brush = QBrush(QColor(0x88, 0x88, 0x88));
        if (brush.style() != Qt::NoBrush)
            for (int c = 0; c < invoices_->columnCount(); ++c)
                if (auto* item = invoices_->item(r, c)) item->setForeground(brush);
    }
    invoices_->resizeColumnsToContents();
    invoices_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);

    // Say what is not being shown, so an empty list is never a mystery.
    if (!query.empty())
        statusBar()->showMessage(
            invoices_->rowCount() == 0
                ? QString("Hľadaniu nezodpovedá žiadny doklad.")
                : QString("Zobrazených %1 z %2 dokladov.")
                      .arg(invoices_->rowCount())
                      .arg(invoices_->rowCount() + hidden));
    else
        statusBar()->clearMessage();
}

void MainWindow::refreshCustomers() {
    auto rows = db_.customers();
    customers_->setRowCount(static_cast<int>(rows.size()));
    for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
        const Customer& c = rows[static_cast<size_t>(r)];
        auto* first = cell(qstr(c.name));
        first->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(c.id));
        customers_->setItem(r, 0, first);
        customers_->setItem(r, 1, cell(qstr(c.ico)));
        customers_->setItem(r, 2, cell(qstr(c.icDph)));
        customers_->setItem(r, 3, cell(qstr(c.address.city)));
        customers_->setItem(r, 4, cell(qstr(c.email)));
    }
}

std::vector<int64_t> MainWindow::selectedInvoiceIds() const {
    std::vector<int64_t> ids;
    for (const QModelIndex& index : invoices_->selectionModel()->selectedRows()) {
        if (auto* item = invoices_->item(index.row(), 0))
            ids.push_back(item->data(Qt::UserRole).toLongLong());
    }
    return ids;
}

int64_t MainWindow::selectedInvoiceId() const {
    const int row = invoices_->currentRow();
    if (row < 0 || !invoices_->item(row, 0)) return 0;
    return invoices_->item(row, 0)->data(Qt::UserRole).toLongLong();
}

int64_t MainWindow::selectedCustomerId() const {
    const int row = customers_->currentRow();
    if (row < 0 || !customers_->item(row, 0)) return 0;
    return customers_->item(row, 0)->data(Qt::UserRole).toLongLong();
}

// ----------------------------------------------------------------- invoices
void MainWindow::newInvoice(DocType type) {
    Invoice inv = InvoiceEditor::blank(db_, type);
    InvoiceEditor editor(db_, inv, this);
    if (editor.exec() != QDialog::Accepted) return;

    Invoice saved = editor.invoice();
    if (!db_.saveInvoice(saved)) {
        QMessageBox::critical(this, "Chyba", qstr(db_.lastError()));
        return;
    }
    // The series is advanced by "Vystaviť", not here: a draft that is thrown
    // away must not leave a hole in the numbering.
    refreshInvoices();
    statusBar()->showMessage("Uložený návrh: " + qstr(saved.number), 4000);
}

void MainWindow::editSelectedInvoice() {
    const int64_t id = selectedInvoiceId();
    if (id == 0) return;
    Invoice inv;
    if (!db_.loadInvoice(id, inv)) return;

    // A locked document opens read-only; the editor shows it and offers Close.
    InvoiceEditor editor(db_, inv, this);
    if (editor.exec() != QDialog::Accepted) return;
    Invoice saved = editor.invoice();
    if (!db_.saveInvoice(saved))
        QMessageBox::critical(this, "Chyba", qstr(db_.lastError()));
    refreshInvoices();
}

void MainWindow::issueSelectedInvoice() {
    const int64_t id = selectedInvoiceId();
    if (id == 0) return;
    Invoice inv;
    if (!db_.loadInvoice(id, inv)) return;

    // The decisions and the work live in InvoiceActions, without dialogs, so
    // they can be tested. What is left here is the asking.
    const IssuePlan plan = planIssue(db_, inv);
    if (!plan.ok) {
        QString text = qstr(plan.refusal);
        for (const std::string& e : plan.errors) text += "\n• " + qstr(e);
        if (plan.errors.empty())
            QMessageBox::information(this, "Vystavenie", text);
        else
            QMessageBox::critical(this, "Doklad nie je kompletný",
                                  "Pred vystavením treba opraviť:\n" + text);
        return;
    }

    const auto answer = QMessageBox::question(this, "Vystaviť doklad",
        "Vystaviť doklad č. " + qstr(plan.number) + "?\n\n"
        "Po vystavení sa obsah uzamkne. Opraviť sa dá len dobropisom, "
        "prípadne odomknutím, ktoré sa zaznamená.",
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
    if (answer != QMessageBox::Yes) return;

    const IssueOutcome outcome = issueDocument(db_, inv, plan);
    if (!outcome.ok) {
        QMessageBox::critical(this, "Vystavenie", qstr(outcome.error));
        return;
    }

    refreshInvoices();
    statusBar()->showMessage("Vystavené: " + qstr(outcome.number), 4000);
}

void MainWindow::recordPayment() {
    const int64_t id = selectedInvoiceId();
    if (id == 0) return;
    Invoice inv;
    if (!db_.loadInvoice(id, inv)) return;

    if (inv.state != InvoiceState::Issued) {
        // Naming the way out, because on a proforma this is the message the
        // user meets after the customer has already paid.
        QMessageBox::information(this, "Platba",
            inv.state == InvoiceState::Draft
                ? QString("Platbu možno zaznamenať až po vystavení dokladu.\n\n"
                          "Vystavte ho tlačidlom Vystaviť — pri exporte PDF sa "
                          "proforma ponúkne vystaviť sama.")
                : QString("Platbu možno zaznamenať až po vystavení dokladu."));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle("Zaznamenať platbu");

    auto* when   = new QDateEdit(QDate::currentDate(), &dlg);
    auto* amount = new QLineEdit(decToUi(inv.outstanding()), &dlg);
    auto* note   = new QLineEdit(&dlg);
    when->setCalendarPopup(true);
    when->setDisplayFormat("dd.MM.yyyy");
    note->setPlaceholderText("napr. výpis č. 8");

    auto* form = new QFormLayout;
    form->addRow("Doklad", new QLabel(qstr(inv.number) + " · " +
                                      decToUi(inv.totals().payable) + " " + qstr(inv.currency),
                                      &dlg));
    form->addRow("Zostáva uhradiť", new QLabel(decToUi(inv.outstanding()), &dlg));
    form->addRow("Dátum platby", when);
    form->addRow("Suma", amount);
    form->addRow("Poznámka", note);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    box->button(QDialogButtonBox::Save)->setText("Uložiť");
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto* layout = new QVBoxLayout(&dlg);
    layout->addLayout(form);
    layout->addWidget(box);

    if (dlg.exec() != QDialog::Accepted) return;

    Payment p;
    p.invoiceId = id;
    p.paidOn    = sstr(when->date().toString(Qt::ISODate));
    p.amount    = decFromUi(amount->text());
    p.note      = lineText(note);
    if (p.amount.isZero()) {
        QMessageBox::warning(this, "Platba", "Zadajte sumu.");
        return;
    }
    if (!db_.addPayment(p))
        QMessageBox::critical(this, "Chyba", qstr(db_.lastError()));
    refreshInvoices();
}

void MainWindow::cancelSelectedInvoice() {
    const int64_t id = selectedInvoiceId();
    if (id == 0) return;
    Invoice inv;
    if (!db_.loadInvoice(id, inv)) return;

    if (inv.state == InvoiceState::Draft) {
        QMessageBox::information(this, "Storno",
            "Rozpracovaný doklad netreba stornovať – jednoducho ho zmažte.");
        return;
    }
    if (inv.state == InvoiceState::Cancelled) {
        QMessageBox::information(this, "Storno", "Doklad je už stornovaný.");
        return;
    }

    QString warning;
    if (!inv.payments.empty())
        warning = "\n\nPozor: k dokladu je zaznamenaná platba "
                  + decToUi(inv.paidAmount()) + ". Storno ju nezruší.";

    bool ok = false;
    const QString reason = QInputDialog::getText(this, "Stornovať doklad",
        "Doklad " + qstr(inv.number) + " zostane v evidencii ako stornovaný, "
        "aby v číselnom rade nevznikla nevysvetlená medzera.\n\n"
        "Ak už doklad odberateľ dostal alebo bol vykázaný v DPH, "
        "správnou opravou je dobropis, nie storno." + warning + "\n\n"
        "Dôvod storna:", QLineEdit::Normal, QString(), &ok);
    if (!ok || reason.trimmed().isEmpty()) return;

    if (!db_.cancelInvoice(id, sstr(reason))) {
        QMessageBox::critical(this, "Chyba", qstr(db_.lastError()));
        return;
    }
    refreshInvoices();
    statusBar()->showMessage("Stornované: " + qstr(inv.number), 4000);
}

void MainWindow::unlockSelectedInvoice() {
    const int64_t id = selectedInvoiceId();
    if (id == 0) return;
    Invoice inv;
    if (!db_.loadInvoice(id, inv)) return;

    if (inv.state != InvoiceState::Issued) {
        QMessageBox::information(this, "Odomknutie", "Doklad nie je vystavený.");
        return;
    }

    bool ok = false;
    const QString reason = QInputDialog::getText(this, "Odomknúť doklad",
        "Doklad " + qstr(inv.number) + " je vystavený a odberateľ ho už môže mať.\n"
        "Správna oprava je dobropis. Odomknutie sa zapíše do histórie dokladu.\n\n"
        "Dôvod odomknutia:", QLineEdit::Normal, QString(), &ok);
    if (!ok || reason.trimmed().isEmpty()) return;

    if (!db_.unlockInvoice(id, sstr(reason)))
        QMessageBox::critical(this, "Chyba", qstr(db_.lastError()));
    refreshInvoices();
}

void MainWindow::showAuditTrail() {
    const int64_t id = selectedInvoiceId();
    if (id == 0) return;
    Invoice inv;
    if (!db_.loadInvoice(id, inv)) return;

    QString text;
    // addPayment() already writes an audit row, so the trail is the whole
    // story; listing inv.payments as well would print each one twice.
    for (const auto& e : db_.auditFor(id))
        text += qstr(e.at) + "  " + qstr(e.action) +
                (e.detail.empty() ? QString() : "  (" + qstr(e.detail) + ")") + "\n";
    if (text.isEmpty()) text = "Zatiaľ žiadne záznamy.";

    Database::IssuedDocument archived;
    if (db_.loadIssuedDocument(id, archived)) {
        std::string detail;
        const bool intact = db_.verifyIssuedDocument(id, &detail);
        text += "\n";
        text += QString("Archív: verzia %1 z %2, PDF %3 kB%4\n")
                    .arg(archived.version)
                    .arg(qstr(archived.issuedAt))
                    .arg(archived.pdf.size() / 1024)
                    .arg(archived.ubl.empty() ? QString() : ", XML priložené");
        text += (intact ? "Kontrola: " : "POZOR: ") + qstr(detail) + "\n";
        if (db_.issuedVersionCount(id) > 1)
            text += QString("Doklad bol vystavený %1-krát (po odomknutí).\n")
                        .arg(db_.issuedVersionCount(id));
    }

    QDialog dlg(this);
    dlg.setWindowTitle("História dokladu " + qstr(inv.number));
    dlg.resize(560, 340);
    auto* view = new QPlainTextEdit(text, &dlg);
    view->setReadOnly(true);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    box->button(QDialogButtonBox::Close)->setText("Zavrieť");
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dlg);
    layout->addWidget(view);
    layout->addWidget(box);
    dlg.exec();
}

void MainWindow::duplicateSelectedInvoice() {
    const int64_t id = selectedInvoiceId();
    if (id == 0) return;
    Invoice inv;
    if (!db_.loadInvoice(id, inv)) return;

    inv.id        = 0;
    inv.number    = db_.peekNextNumber();
    inv.issueDate = sk::todayIso();
    inv.taxPointDate = inv.issueDate;
    inv.dueDate   = sk::addDays(inv.issueDate, 14);
    inv.variableSymbol = sk::variableSymbolFrom(inv.number);
    // A copy of an issued document is a new draft. Without this the editor sees
    // the state it was copied from, opens read-only, and accept() rejects — so
    // duplicating an issued invoice quietly did nothing at all.
    inv.state = InvoiceState::Draft;
    inv.issuedAt.clear();
    // None of these belong to the copy: they describe what happened to the
    // original.
    inv.payments.clear();
    inv.precedingNumber.clear();
    inv.precedingDate.clear();
    inv.relatedProformaNumbers.clear();
    inv.settledByNumber.clear();
    // A new tax point needs its own exchange rate. Only a correction reuses the
    // original one (§ 25), and a duplicate is not a correction — carrying last
    // month's rate onto today's invoice would be wrong in a way nobody looks at.
    inv.vatAccountingCurrency.clear();
    inv.exchangeRate = Dec();
    inv.exchangeRateDate.clear();

    InvoiceEditor editor(db_, inv, this);
    if (editor.exec() != QDialog::Accepted) return;
    Invoice saved = editor.invoice();
    if (!db_.saveInvoice(saved))
        QMessageBox::critical(this, "Chyba", qstr(db_.lastError()));
    refreshInvoices();
}

void MainWindow::creditNoteFromSelected() {
    const int64_t id = selectedInvoiceId();
    if (id == 0) {
        QMessageBox::information(this, "Dobropis", "Najprv vyberte faktúru, ku ktorej patrí dobropis.");
        return;
    }
    Invoice src;
    if (!db_.loadInvoice(id, src)) return;

    // The exchange rate is deliberately *not* reset here: § 25 has a correction
    // converted at the rate used when the original tax liability arose, so the
    // dobropis keeps the rate of the invoice it corrects.
    Invoice cn = src;
    cn.id   = 0;
    cn.type = DocType::CreditNote;
    cn.precedingNumber = src.number;
    cn.precedingDate   = src.issueDate;
    cn.number    = db_.peekNextNumber();
    cn.issueDate = sk::todayIso();
    cn.taxPointDate = cn.issueDate;
    cn.dueDate   = sk::addDays(cn.issueDate, 14);
    cn.variableSymbol = sk::variableSymbolFrom(cn.number);
    // The dobropis is a new draft, whatever state the invoice it corrects is
    // in — and it is corrected precisely when that state is "issued".
    cn.state = InvoiceState::Draft;
    cn.issuedAt.clear();
    cn.payments.clear();
    cn.relatedProformaNumbers.clear();
    cn.settledByNumber.clear();

    InvoiceEditor editor(db_, cn, this);
    if (editor.exec() != QDialog::Accepted) return;
    Invoice saved = editor.invoice();
    if (!db_.saveInvoice(saved))
        QMessageBox::critical(this, "Chyba", qstr(db_.lastError()));
    refreshInvoices();
}

void MainWindow::invoiceFromProformas() {
    const std::vector<int64_t> ids = selectedInvoiceIds();
    if (ids.empty()) {
        QMessageBox::information(this, "Faktúra z proforiem",
            "Označte jednu alebo viac proforiem (Cmd-klik pre viac).");
        return;
    }

    std::vector<Invoice> proformas;
    for (int64_t id : ids) {
        Invoice p;
        if (!db_.loadInvoice(id, p)) continue;
        if (p.type != DocType::Proforma) {
            QMessageBox::warning(this, "Faktúra z proforiem",
                "Doklad " + qstr(p.number) + " nie je proforma.");
            return;
        }
        if (!p.settledByNumber.empty()) {
            QMessageBox::warning(this, "Faktúra z proforiem",
                "Proforma " + qstr(p.number) + " je už vyúčtovaná faktúrou " +
                qstr(p.settledByNumber) + ".");
            return;
        }
        proformas.push_back(std::move(p));
    }
    if (proformas.empty()) return;

    // One customer per invoice: silently merging two customers' proformas would
    // produce a document nobody can pay.
    for (const Invoice& p : proformas)
        if (p.buyer.name != proformas.front().buyer.name) {
            QMessageBox::warning(this, "Faktúra z proforiem",
                "Označené proformy patria rôznym odberateľom.");
            return;
        }

    Invoice inv = InvoiceEditor::blank(db_, DocType::Invoice);
    inv.buyer          = proformas.front().buyer;
    inv.currency       = proformas.front().currency;
    inv.buyerReference = proformas.front().buyerReference;
    inv.orderReference = proformas.front().orderReference;
    inv.lines.clear();

    // Whatever the customer has already handed over is deducted here as a
    // prepayment (BT-113), so the final invoice asks only for the remainder.
    //
    // Two sources, depending on the regime. A §4 payer declares the advance on
    // a daňový doklad k prijatej platbe, and that document is what gets
    // deducted. Everyone else has no such document, so the deduction is simply
    // the money recorded against the proformas.
    Dec alreadyDeclared;
    Dec receivedOnProformas;
    for (const Invoice& p : proformas) {
        // Discounts and the rounding travel with the lines. Dropping them
        // turns a discounted proforma into a full-price invoice, silently.
        for (const Allowance& a : p.allowances) inv.allowances.push_back(a);
        inv.roundingAmount += p.roundingAmount;

        for (const InvoiceLine& l : p.lines) inv.lines.push_back(l);
        receivedOnProformas += p.paidAmount();

        for (int64_t advanceId : db_.proformasSettledBy(p.id)) {
            Invoice advance;
            if (db_.loadInvoice(advanceId, advance) &&
                advance.type == DocType::AdvanceTaxDocument)
                alreadyDeclared += advance.totals().payable;
        }
    }
    int lineNo = 0;
    for (InvoiceLine& l : inv.lines) l.lineNo = ++lineNo;
    inv.prepaidAmount =
        alreadyDeclared.isZero() ? receivedOnProformas.roundTo(2) : alreadyDeclared.roundTo(2);

    InvoiceEditor editor(db_, inv, this);
    if (editor.exec() != QDialog::Accepted) return;

    Invoice saved = editor.invoice();
    if (!db_.saveInvoice(saved)) {
        QMessageBox::critical(this, "Chyba", qstr(db_.lastError()));
        return;
    }
    for (const Invoice& p : proformas) db_.linkProforma(saved.id, p.id);

    refreshInvoices();
    QString message = QString("Vytvorený návrh faktúry z %1 proforiem").arg(proformas.size());
    if (!saved.prepaidAmount.isZero())
        message += QString(", odpočítaná záloha %1").arg(decToUi(saved.prepaidAmount));
    statusBar()->showMessage(message, 6000);
}

void MainWindow::advanceTaxDocumentFrom() {
    const int64_t id = selectedInvoiceId();
    if (id == 0) return;
    Invoice proforma;
    if (!db_.loadInvoice(id, proforma)) return;

    if (proforma.type != DocType::Proforma) {
        QMessageBox::information(this, "Doklad k prijatej platbe",
            "Označte proformu, na ktorú prišla platba.");
        return;
    }

    // The advance document follows the regime of the proforma it settles, not
    // whichever company happens to be selected now.
    const Company& seller = proforma.seller;
    if (!seller.chargesVat()) {
        QMessageBox::information(this, "Doklad k prijatej platbe",
            "Daňový doklad k prijatej platbe vystavuje len platiteľ DPH (§4). "
            "Vo vašom režime stačí proforma a potom faktúra.");
        return;
    }

    const Dec received = proforma.paidAmount();
    if (received.isZero()) {
        QMessageBox::information(this, "Doklad k prijatej platbe",
            "Na proforme " + qstr(proforma.number) + " nie je zaznamenaná žiadna platba.");
        return;
    }

    // The advance is a gross amount already received; the VAT inside it is
    // extracted at the rate of the proforma's lines rather than added on top.
    Invoice doc = InvoiceEditor::blank(db_, DocType::AdvanceTaxDocument);
    doc.buyer    = proforma.buyer;
    doc.currency = proforma.currency;
    doc.lines.clear();

    const Dec rate = proforma.lines.empty() ? Dec() : proforma.lines.front().vatRate;
    const std::string category =
        proforma.lines.empty() ? std::string(VatCat::Standard) : proforma.lines.front().vatCategory;

    InvoiceLine l;
    l.lineNo      = 1;
    l.description = "Prijatá platba k zálohovej faktúre " + proforma.number;
    l.unit        = "ks";
    l.quantity    = Dec::fromInt(1);
    l.vatRate     = rate;
    l.vatCategory = category;
    // net = gross / (1 + rate/100)
    l.unitPrice   = (received / (Dec::fromInt(1) + rate / Dec::fromInt(100))).roundTo(2);
    doc.lines.push_back(l);

    doc.precedingNumber = proforma.number;
    doc.precedingDate   = proforma.issueDate;
    doc.taxPointDate    = proforma.payments.empty() ? sk::todayIso()
                                                    : proforma.payments.back().paidOn;
    doc.note = "Daňový doklad k platbe prijatej dňa " + qstr(doc.taxPointDate).toStdString() +
               " k zálohovej faktúre " + proforma.number + ".";

    InvoiceEditor editor(db_, doc, this);
    if (editor.exec() != QDialog::Accepted) return;

    Invoice saved = editor.invoice();
    if (!db_.saveInvoice(saved)) {
        QMessageBox::critical(this, "Chyba", qstr(db_.lastError()));
        return;
    }
    db_.linkProforma(saved.id, proforma.id);
    refreshInvoices();
    statusBar()->showMessage("Vytvorený daňový doklad k prijatej platbe", 5000);
}

void MainWindow::deleteSelectedInvoice() {
    const int64_t id = selectedInvoiceId();
    if (id == 0) return;
    Invoice inv;
    if (!db_.loadInvoice(id, inv)) return;

    // issuedAt survives an unlock, so a document that was once issued can
    // never be deleted — otherwise unlocking would be a way around the rule,
    // taking its payments with it and leaving a hole in the numbering.
    if (inv.state != InvoiceState::Draft || !inv.issuedAt.empty()) {
        QMessageBox::warning(this, "Zmazať doklad",
            "Doklad už bol vystavený a nedá sa zmazať – jeho číslo musí zostať "
            "v číselnom rade.\n\n"
            "Ak ste ho vystavili omylom a neodoslali, použite Stornovať. "
            "Ak ho odberateľ už dostal, vystavte dobropis.");
        return;
    }
    if (QMessageBox::question(this, "Zmazať doklad",
            "Naozaj zmazať rozpracovaný doklad " + qstr(inv.number) + "?") != QMessageBox::Yes)
        return;
    db_.deleteInvoice(id);
    refreshInvoices();
}

void MainWindow::exportPdf() {
    const int64_t id = selectedInvoiceId();
    if (id == 0) return;
    Invoice inv;
    if (!db_.loadInvoice(id, inv)) return;

    // A proforma is exported in order to be sent, and a proforma that has been
    // sent should be a real document: only an issued one can take a payment, be
    // matched from a bank statement, count towards what you are owed, or settle
    // into an invoice. Offering it here keeps one rule everywhere else in the
    // application — issued means real — instead of teaching five places that
    // handle money about a document that is a draft and payable at the same
    // time.
    //
    // Offered, not done. Exporting a proforma to look at it is a reasonable
    // thing to want, and issuing locks the content.
    if (inv.type == DocType::Proforma && inv.state == InvoiceState::Draft) {
        const IssuePlan plan = planIssue(db_, inv);
        if (plan.ok) {
            const auto answer = QMessageBox::question(this, "Vystaviť proformu",
                "Proforma č. " + qstr(plan.number) + " je zatiaľ rozpracovaná.\n\n"
                "Vystaviť ju teraz? Až vystavenú proformu možno uhradiť, "
                "spárovať s výpisom a vyúčtovať faktúrou.",
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
                QMessageBox::Yes);
            if (answer == QMessageBox::Cancel) return;
            if (answer == QMessageBox::Yes) {
                const IssueOutcome outcome = issueDocument(db_, inv, plan);
                if (!outcome.ok) {
                    QMessageBox::critical(this, "Vystavenie", qstr(outcome.error));
                    return;
                }
                // Reloaded, or the export below would render the draft that no
                // longer exists — and miss the archived bytes just written.
                if (!db_.loadInvoice(id, inv)) return;
                refreshInvoices();
                statusBar()->showMessage("Vystavené: " + qstr(outcome.number), 4000);
            }
        }
        // When it cannot be issued, saying so here would interrupt an export
        // the user may only have wanted in order to read it. Vystaviť says why.
    }

    QString suggested = lastDir() + "/" + qstr(inv.number) + ".pdf";
    QString path = QFileDialog::getSaveFileName(this, "Uložiť PDF", suggested, "PDF (*.pdf)");
    if (path.isEmpty()) return;
    rememberDir(path);

    // Only while the document is still issued. A cancelled one must print its
    // STORNOVANÉ banner, and one unlocked back to draft must print NÁVRH —
    // serving the old bytes would hide both.
    Database::IssuedDocument archived;
    if (inv.state == InvoiceState::Issued &&
        db_.loadIssuedDocument(id, archived) && !archived.pdf.empty()) {
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly)) {
            QMessageBox::critical(this, "Chyba", "Súbor sa nedá zapísať: " + path);
            return;
        }
        out.write(archived.pdf.data(), static_cast<qint64>(archived.pdf.size()));
        out.close();
        statusBar()->showMessage(
            QString("PDF z archívu (verzia %1): %2").arg(archived.version).arg(path), 6000);
        return;
    }

    QString error;
    // The document prints as it was written, not as the company reads today —
    // except for the logo, which the snapshot has never carried. See
    // sellerForRender().
    if (!writeInvoicePdf(inv, sellerForRender(db_, inv), path, &error)) {
        QMessageBox::critical(this, "Chyba", error);
        return;
    }
    statusBar()->showMessage("PDF uložené: " + path, 5000);
}

void MainWindow::exportUbl() {
    const int64_t id = selectedInvoiceId();
    if (id == 0) return;
    Invoice inv;
    if (!db_.loadInvoice(id, inv)) return;

    if (!canExportToPeppol(inv.type)) {
        QMessageBox::information(this, "Peppol XML",
            "Proforma nie je daňový doklad a Peppol pre ňu nemá kód. "
            "Odošlite až vyúčtovaciu faktúru.");
        return;
    }
    if (inv.state == InvoiceState::Cancelled) {
        QMessageBox::information(this, "Peppol XML", "Stornovaný doklad sa neodosiela.");
        return;
    }
    if (inv.state != InvoiceState::Issued) {
        QMessageBox::information(this, "Peppol XML",
            "Peppol sa odosiela až po vystavení dokladu. Návrh nemá platné číslo.");
        return;
    }

    const Company& seller = inv.seller;
    ValidationResult r = validate(inv, seller);
    if (!r.ok()) {
        QString text;
        for (const Issue& i : r.issues)
            if (i.severity == Severity::Error) text += "• " + qstr(i.message) + "\n";
        QMessageBox::critical(this, "Faktúru nie je možné exportovať",
            "Peppol by ju odmietol:\n\n" + text);
        return;
    }

    QString suggested = lastDir() + "/" + qstr(inv.number) + ".xml";
    QString path = QFileDialog::getSaveFileName(this, "Uložiť Peppol UBL", suggested, "XML (*.xml)");
    if (path.isEmpty()) return;
    rememberDir(path);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {   // no Text flag: XML must keep LF endings
        QMessageBox::critical(this, "Chyba", "Súbor sa nedá zapísať: " + path);
        return;
    }
    Database::IssuedDocument archived;
    const bool fromArchive = db_.loadIssuedDocument(id, archived) && !archived.ubl.empty();

    if (fromArchive) {
        // Byte for byte. Round-tripping through QString and back to UTF-8 would
        // defeat the point: the file must still hash to the stored digest.
        f.write(archived.ubl.data(), static_cast<qint64>(archived.ubl.size()));
    } else {
        QTextStream out(&f);
        out.setEncoding(QStringConverter::Utf8);
        out << qstr(writeUbl(inv, seller));
        out.flush();
    }
    f.close();

    QString msg = (fromArchive ? "UBL z archívu: " : "UBL uložené: ") + path;
    if (r.warningCount() > 0)
        msg += QString("  (%1 upozornení)").arg(r.warningCount());
    statusBar()->showMessage(msg, 6000);
}

// ---------------------------------------------------------------- customers
bool MainWindow::editCustomerDialog(Customer& c) {
    QDialog dlg(this);
    dlg.setWindowTitle(c.id == 0 ? "Nový odberateľ" : "Odberateľ");
    dlg.resize(480, 640);

    auto* form = new PartyForm(&dlg);
    form->setDatabase(&db_);
    form->load(c);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    box->button(QDialogButtonBox::Save)->setText("Uložiť");
    box->button(QDialogButtonBox::Cancel)->setText("Zrušiť");
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto* layout = new QVBoxLayout(&dlg);
    layout->addWidget(form);
    layout->addWidget(box);

    if (dlg.exec() != QDialog::Accepted) return false;
    form->applyTo(c);
    if (c.name.empty()) {
        QMessageBox::warning(this, "Chýba názov", "Zadajte obchodné meno odberateľa.");
        return false;
    }
    return true;
}

void MainWindow::newCustomer() {
    Customer c;
    if (!editCustomerDialog(c)) return;
    if (!db_.saveCustomer(c))
        QMessageBox::critical(this, "Chyba", qstr(db_.lastError()));
    refreshCustomers();
}

void MainWindow::editSelectedCustomer() {
    const int64_t id = selectedCustomerId();
    if (id == 0) return;
    Customer c;
    if (!db_.loadCustomer(id, c)) return;
    if (!editCustomerDialog(c)) return;
    if (!db_.saveCustomer(c))
        QMessageBox::critical(this, "Chyba", qstr(db_.lastError()));
    refreshCustomers();
}

void MainWindow::deleteSelectedCustomer() {
    const int64_t id = selectedCustomerId();
    if (id == 0) return;
    if (QMessageBox::question(this, "Odstrániť odberateľa",
            "Odberateľ sa skryje zo zoznamu. Existujúce faktúry zostanú nezmenené. Pokračovať?")
        != QMessageBox::Yes)
        return;
    db_.deleteCustomer(id);
    refreshCustomers();
}

} // namespace fk
