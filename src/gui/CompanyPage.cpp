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

#include "CompanyPage.h"

#include "../country/Country.h"
#include "../sk/Slovak.h"
#include "GuiUtil.h"
#include "PartyForm.h"

#include <QBrush>
#include <QVariant>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QPixmap>
#include <QComboBox>
#include <QHeaderView>
#include <QTableWidget>
#include <QFormLayout>
#include <QSignalBlocker>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStringList>

#include <algorithm>
#include <cctype>
#include <QVBoxLayout>

namespace fk {

using namespace fk::gui;

CompanyPage::CompanyPage(Database& db, QWidget* parent) : QWidget(parent), db_(db) {
    party_    = new PartyForm(this);
    accounts_ = new QTableWidget(this);
    accounts_->setObjectName("accounts");   // reached by name from the GUI tests
    registry_ = new QPlainTextEdit(this);
    prefix_   = new QLineEdit(this);
    year_     = new QSpinBox(this);
    next_     = new QSpinBox(this);
    padding_  = new QSpinBox(this);
    preview_  = new QLabel(this);
    vatMode_  = new QComboBox(this);
    company_  = new QComboBox(this);

    // The single most consequential setting in the app: it decides whether
    // invoices carry VAT at all. Holding an IČ DPH is not the same thing —
    // a §7 or §7a subject has one and still must not charge Slovak VAT.
    vatMode_->addItem(vatModeLabel(VatMode::Payer),              vatModeCode(VatMode::Payer));
    vatMode_->addItem(vatModeLabel(VatMode::RegisteredNotPayer),
                      vatModeCode(VatMode::RegisteredNotPayer));
    vatMode_->addItem(vatModeLabel(VatMode::NotRegistered),      vatModeCode(VatMode::NotRegistered));

    // One account per row. A company invoicing in both countries needs two,
    // and a customer paying a CZK invoice into a EUR account pays for the
    // privilege.
    // "QR kód" is normally left on Automaticky, which means the currency picks
    // the standard: EUR prints PAY by square, CZK prints QR Platba. It is here
    // because banking apps disagree about the other one and no rule the app
    // could infer is going to be right for every customer's bank.
    accounts_->setColumnCount(7);
    accounts_->setHorizontalHeaderLabels(
        {"Popis", "IBAN", "Číslo účtu", "BIC/SWIFT", "Banka", "Mena", "QR kód"});
    accounts_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    accounts_->verticalHeader()->setVisible(false);
    accounts_->setSelectionBehavior(QAbstractItemView::SelectRows);
    accounts_->setMinimumHeight(120);
    registry_->setPlaceholderText("Zapísaný v OR OS Bratislava I, oddiel: Sro, vložka č. …");
    registry_->setMaximumHeight(60);
    year_->setRange(0, 2999);
    next_->setRange(1, 999999);
    padding_->setRange(1, 8);

    // Which company you are working in. Everything else on this page — and the
    // invoice list, and the numbering — follows this.
    auto* pick = new QGroupBox("Firma", this);
    auto* fp = new QFormLayout(pick);
    auto* addBtn    = new QPushButton("Nová firma", this);
    auto* removeBtn = new QPushButton("Odstrániť firmu", this);
    auto* row = new QHBoxLayout;
    row->addWidget(addBtn);
    row->addWidget(removeBtn);
    row->addStretch();
    fp->addRow("Aktívna firma", company_);
    fp->addRow("", row);

    auto* vatBox = new QGroupBox("Režim DPH", this);
    auto* fv = new QFormLayout(vatBox);
    auto* vatHint = new QLabel(this);
    vatHint->setWordWrap(true);
    vatHint->setObjectName("vatHint");
    fv->addRow("Režim", vatMode_);
    fv->addRow(vatHint);

    auto* bank = new QGroupBox("Bankové spojenie", this);
    auto* fb = new QVBoxLayout(bank);
    auto* bankHint = new QLabel(
        "Na faktúru sa použije účet podľa jej meny — pre faktúru v CZK účet s menou "
        "CZK, inak prvý účet. IBAN a číslo účtu sa dopĺňajú navzájom.", this);
    bankHint->setWordWrap(true);
    fb->addWidget(bankHint);
    fb->addWidget(accounts_);

    auto* addAccount    = new QPushButton("Pridať účet", this);
    auto* removeAccount = new QPushButton("Odstrániť účet", this);
    auto* accountRow = new QHBoxLayout;
    accountRow->addWidget(addAccount);
    accountRow->addWidget(removeAccount);
    accountRow->addStretch();
    fb->addLayout(accountRow);

    auto* fbForm = new QFormLayout;
    fbForm->addRow("Zápis v registri", registry_);

    // The logo, kept in the database rather than as a path: a path stops
    // working the day the file moves, and an invoice that quietly lost its
    // logo is only noticed after it has been sent.
    logoView_ = new QLabel(this);
    logoView_->setObjectName("logoPreview");
    logoView_->setMinimumHeight(52);
    logoView_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    auto* chooseLogoBtn = new QPushButton("Vybrať logo…", this);
    auto* clearLogoBtn  = new QPushButton("Odstrániť", this);
    chooseLogoBtn->setObjectName("chooseLogo");
    clearLogoBtn->setObjectName("clearLogo");
    connect(chooseLogoBtn, &QPushButton::clicked, this, [this] { chooseLogo(); });
    connect(clearLogoBtn,  &QPushButton::clicked, this, [this] { clearLogo(); });

    auto* logoRow = new QHBoxLayout;
    logoRow->addWidget(logoView_, 1);
    logoRow->addWidget(chooseLogoBtn);
    logoRow->addWidget(clearLogoBtn);
    fbForm->addRow("Logo na faktúre", logoRow);

    fb->addLayout(fbForm);

    auto* series = new QGroupBox("Číselný rad faktúr", this);
    auto* fs = new QFormLayout(series);
    fs->addRow("Predpona", prefix_);
    fs->addRow("Rok (0 = bez roku)", year_);
    fs->addRow("Nasledujúce číslo", next_);
    fs->addRow("Počet číslic", padding_);
    fs->addRow("Ukážka", preview_);

    auto* saveBtn = new QPushButton("Uložiť", this);
    saveBtn->setDefault(true);

    auto* inner = new QWidget;
    auto* col = new QVBoxLayout(inner);
    col->addWidget(pick);
    col->addWidget(party_);
    col->addWidget(vatBox);
    col->addWidget(bank);
    col->addWidget(series);
    col->addStretch();

    auto* scroll = new QScrollArea(this);
    scroll->setWidget(inner);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(saveBtn);

    auto* root = new QVBoxLayout(this);
    root->addWidget(scroll);
    root->addLayout(buttons);

    connect(vatMode_, &QComboBox::currentIndexChanged, this, [this](int) { applyVatModeHint(); });

    // A registry lookup knows the answer: §4 means payer, §7 / §7a does not.
    party_->onVatModeDetected = [this](VatMode mode) {
        const int index = vatMode_->findData(vatModeCode(mode));
        if (index >= 0) vatMode_->setCurrentIndex(index);
    };

    party_->setDatabase(&db_);
    // A registry lookup also knows the court and the register entry, which is
    // exactly what the "zapísaná v OR" line on the invoice needs.
    party_->onRecordApplied = [this](const RegistryRecord& r) {
        const std::string note = r.registryNote();
        if (!note.empty() && registry_->toPlainText().trimmed().isEmpty())
            registry_->setPlainText(qstr(note));
    };

    connect(saveBtn, &QPushButton::clicked, this, [this] { save(); });
    connect(addBtn, &QPushButton::clicked, this, [this] { addCompany(); });
    connect(addAccount, &QPushButton::clicked, this, [this] { addAccountRow(BankAccount()); });
    connect(removeAccount, &QPushButton::clicked, this, [this] { removeSelectedAccount(); });
    connect(accounts_, &QTableWidget::cellChanged, this,
            [this](int row, int column) { completeAccountRow(row, column); });
    connect(removeBtn, &QPushButton::clicked, this, [this] { removeCompany(); });
    connect(company_, &QComboBox::currentIndexChanged, this,
            [this](int i) { switchCompany(i); });

    auto refresh = [this] { updatePreview(); };
    connect(prefix_,  &QLineEdit::textChanged, this, refresh);
    connect(year_,    &QSpinBox::valueChanged, this, refresh);
    connect(next_,    &QSpinBox::valueChanged, this, refresh);
    connect(padding_, &QSpinBox::valueChanged, this, refresh);

    reload();
}

void CompanyPage::reloadCompanyList() {
    const QSignalBlocker blocker(company_);
    company_->clear();
    const int64_t active = db_.activeCompanyId();
    for (const Company& c : db_.companies()) {
        const QString label = c.name.empty() ? QString("(bez názvu)") : qstr(c.name);
        company_->addItem(label, QVariant::fromValue<qlonglong>(c.id));
        if (c.id == active) company_->setCurrentIndex(company_->count() - 1);
    }
}

/// True when the form no longer matches what is stored for the company being
/// shown. Cheap and exact: compare the whole record.
bool CompanyPage::hasUnsavedChanges() const {
    if (loadedCompanyId_ == 0) return false;

    Company stored;
    if (!db_.loadCompany(loadedCompanyId_, stored)) return false;

    Company current = stored;
    party_->applyTo(current);
    current.accounts     = collectAccounts();
    current.registryNote = sstr(registry_->toPlainText());
    current.vatMode      = vatModeFromCode(sstr(vatMode_->currentData().toString()));
    current.logo         = logo_;
    current.logoPath     = logoPath_;

    return current.name != stored.name || current.ico != stored.ico ||
           current.dic != stored.dic || current.icDph != stored.icDph ||
           current.address.street != stored.address.street ||
           current.address.street2 != stored.address.street2 ||
           current.address.city != stored.address.city ||
           current.address.postalCode != stored.address.postalCode ||
           current.address.countryCode != stored.address.countryCode ||
           current.email != stored.email || current.phone != stored.phone ||
           current.contactName != stored.contactName ||
           current.endpointScheme != stored.endpointScheme ||
           current.endpointId != stored.endpointId ||
           current.accounts.size() != stored.accounts.size() ||
           !std::equal(current.accounts.begin(), current.accounts.end(),
                       stored.accounts.begin(),
                       [](const BankAccount& a, const BankAccount& b) {
                           return a.label == b.label && a.iban == b.iban &&
                                  a.localNumber == b.localNumber && a.bic == b.bic &&
                                  a.bankName == b.bankName && a.currency == b.currency &&
                                  a.qrFormat == b.qrFormat;
                       }) ||
           current.registryNote != stored.registryNote ||
           current.logo != stored.logo ||
           current.logoPath != stored.logoPath ||
           current.vatMode != stored.vatMode;
}

void CompanyPage::switchCompany(int index) {
    if (switching_ || index < 0) return;
    const qlonglong id = company_->itemData(index).toLongLong();
    if (id == 0 || id == db_.activeCompanyId()) return;

    // Switching away used to discard whatever had just been typed. Ask, while
    // the company being left is still the active one — save() writes to it.
    if (hasUnsavedChanges()) {
        const auto answer = QMessageBox::question(this, "Neuložené zmeny",
            "Údaje firmy sa zmenili. Uložiť ich pred prepnutím?",
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);
        if (answer == QMessageBox::Cancel) {
            switching_ = true;
            reloadCompanyList();     // put the selection back
            switching_ = false;
            return;
        }
        if (answer == QMessageBox::Save && !save()) {
            switching_ = true;
            reloadCompanyList();
            switching_ = false;
            return;
        }
    }

    switching_ = true;
    db_.setActiveCompany(id);
    reload();
    switching_ = false;
    if (onActiveCompanyChanged) onActiveCompanyChanged();
}

void CompanyPage::addCompany() {
    Company fresh;
    fresh.name = "Nová firma";
    fresh.address.countryCode = "SK";
    if (!db_.saveCompany(fresh)) {
        QMessageBox::critical(this, "Chyba", qstr(db_.lastError()));
        return;
    }
    db_.setActiveCompany(fresh.id);
    reload();
    if (onActiveCompanyChanged) onActiveCompanyChanged();
    QMessageBox::information(this, "Nová firma",
        "Firma bola vytvorená a je aktívna. Vyplňte údaje a uložte ich.\n\n"
        "Číselný rad má vlastný – nastavte si ho nižšie.");
}

void CompanyPage::removeCompany() {
    const int64_t id = db_.activeCompanyId();
    if (id == 0) return;

    if (db_.companies().size() <= 1) {
        QMessageBox::information(this, "Odstrániť firmu", "Poslednú firmu nemožno odstrániť.");
        return;
    }
    const int documents = db_.invoiceCountFor(id);
    if (documents > 0) {
        QMessageBox::warning(this, "Odstrániť firmu",
            QString("Firma má %1 dokladov a nedá sa odstrániť. "
                    "Doklady musia zostať v evidencii.").arg(documents));
        return;
    }
    if (QMessageBox::question(this, "Odstrániť firmu",
            "Naozaj odstrániť aktívnu firmu?") != QMessageBox::Yes)
        return;

    if (!db_.deleteCompany(id)) {
        QMessageBox::critical(this, "Chyba", qstr(db_.lastError()));
        return;
    }
    reload();
    if (onActiveCompanyChanged) onActiveCompanyChanged();
}

void CompanyPage::reload() {
    reloadCompanyList();
    Company c = db_.activeCompany();
    loadedCompanyId_ = c.id;
    party_->load(c);
    loadAccounts(c);
    registry_->setPlainText(qstr(c.registryNote));
    logo_     = c.logo;
    logoPath_ = c.logoPath;
    showLogo();
    const int vatIndex = vatMode_->findData(vatModeCode(c.vatMode));
    vatMode_->setCurrentIndex(vatIndex >= 0 ? vatIndex : 0);
    applyVatModeHint();

    NumberSeries s = db_.series();
    prefix_->setText(qstr(s.prefix));
    year_->setValue(s.year);
    next_->setValue(s.next);
    padding_->setValue(s.padding);
    updatePreview();
}

/// A picture on the invoice, stored with the company rather than referenced by
/// path. Read once, here, and carried in memory until the page is saved.
void CompanyPage::chooseLogo() {
    const QString file = QFileDialog::getOpenFileName(
        this, "Logo na faktúru", QString(),
        "Obrázky (*.png *.jpg *.jpeg *.PNG *.JPG *.JPEG);;Všetky súbory (*)");
    if (file.isEmpty()) return;

    // Asked before reading, not after: no reason to pull a 40 MB photograph
    // into memory only to refuse it.
    if (QFileInfo(file).size() > 2 * 1024 * 1024) {
        QMessageBox::warning(this, "Logo",
            "Obrázok je väčší než 2 MB. Použite menší — na faktúre má šírku "
            "asi 4 cm.");
        return;
    }

    QFile source(file);
    if (!source.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Logo", "Súbor sa nepodarilo otvoriť:\n" + file);
        return;
    }
    const QByteArray bytes = source.readAll();
    source.close();

    QImage probe;
    if (!probe.loadFromData(bytes) || probe.isNull()) {
        QMessageBox::warning(this, "Logo",
            "Súbor sa nepodarilo prečítať ako obrázok. Skúste PNG alebo JPEG.");
        return;
    }

    logo_     = std::string(bytes.constData(), static_cast<size_t>(bytes.size()));
    logoPath_ = sstr(file);      // where it came from, for the settings screen
    showLogo();
}

void CompanyPage::clearLogo() {
    logo_.clear();
    logoPath_.clear();
    showLogo();
}

void CompanyPage::showLogo() {
    if (!logoView_) return;
    if (logo_.empty()) {
        logoView_->setPixmap(QPixmap());
        logoView_->setText("(žiadne logo)");
        return;
    }
    QImage image;
    if (!image.loadFromData(reinterpret_cast<const uchar*>(logo_.data()),
                            static_cast<int>(logo_.size())) || image.isNull()) {
        logoView_->setPixmap(QPixmap());
        logoView_->setText("(obrázok sa nepodarilo prečítať)");
        return;
    }
    logoView_->setText(QString());
    logoView_->setPixmap(QPixmap::fromImage(
        image.scaled(220, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}

void CompanyPage::loadAccounts(const Company& company) {
    const QSignalBlocker blocker(accounts_);
    accounts_->setRowCount(0);
    for (const BankAccount& a : company.accounts) addAccountRow(a);
    if (company.accounts.empty()) addAccountRow(BankAccount());
    for (int row = 0; row < accounts_->rowCount(); ++row) flagAccountConflict(row);
    accounts_->resizeColumnsToContents();
    accounts_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
}

void CompanyPage::addAccountRow(const BankAccount& account) {
    const QSignalBlocker blocker(accounts_);
    const int row = accounts_->rowCount();
    accounts_->insertRow(row);
    accounts_->setItem(row, 0, new QTableWidgetItem(qstr(account.label)));
    accounts_->setItem(row, 1, new QTableWidgetItem(qstr(sk::formatIban(account.iban))));
    accounts_->setItem(row, 2, new QTableWidgetItem(qstr(account.localNumber)));
    accounts_->setItem(row, 3, new QTableWidgetItem(qstr(account.bic)));
    accounts_->setItem(row, 4, new QTableWidgetItem(qstr(account.bankName)));
    accounts_->setItem(row, 5, new QTableWidgetItem(qstr(account.currency)));

    QComboBox* qr = new QComboBox(accounts_);
    for (const QrFormat f : {QrFormat::Automatic, QrFormat::PayBySquare, QrFormat::Spayd,
                             QrFormat::Both, QrFormat::None})
        qr->addItem(QString::fromUtf8(qrFormatLabel(f)), static_cast<int>(f));
    qr->setCurrentIndex(qr->findData(static_cast<int>(account.qrFormat)));
    accounts_->setCellWidget(row, 6, qr);
}

void CompanyPage::removeSelectedAccount() {
    const int row = accounts_->currentRow();
    if (row < 0) return;
    accounts_->removeRow(row);
    if (accounts_->rowCount() == 0) addAccountRow(BankAccount());
}

/// Slovak and Czech accounts have the same structure, so one form of the number
/// determines the other exactly. Filling it in beats asking twice.
void CompanyPage::completeAccountRow(int row, int column) {
    if (row < 0) return;

    auto text = [this, row](int c) {
        return accounts_->item(row, c) ? accounts_->item(row, c)->text().trimmed() : QString();
    };
    const QSignalBlocker blocker(accounts_);

    if (column == 2 && !text(2).isEmpty() && text(1).isEmpty()) {
        // Try both countries rather than assuming the company's: a Slovak
        // company may perfectly well hold a Czech account, which is the whole
        // reason for having two.
        const LocalAccount local = parseLocalAccount(sstr(text(2)));
        for (Country candidate : {Country::SK, Country::CZ}) {
            const std::string iban = ibanFromLocalAccount(candidate, local);
            if (iban.empty()) continue;
            if (!accounts_->item(row, 1)) accounts_->setItem(row, 1, new QTableWidgetItem());
            accounts_->item(row, 1)->setText(qstr(sk::formatIban(iban)));
            break;
        }
    } else if (column == 1 && !text(1).isEmpty() && text(2).isEmpty()) {
        const std::string local = localAccountFromIban(sstr(text(1)));
        if (!local.empty()) {
            if (!accounts_->item(row, 2)) accounts_->setItem(row, 2, new QTableWidgetItem());
            accounts_->item(row, 2)->setText(qstr(local));
        }
    }

    flagAccountConflict(row);
}

/// Two numbers in one row that name two different accounts is a mistake worth
/// seeing before an invoice goes out with both on it. A differing *bank code*
/// is not that mistake — see accountConflict().
void CompanyPage::flagAccountConflict(int row) {
    const auto cell = [this, row](int c) {
        return accounts_->item(row, c) ? sstr(accounts_->item(row, c)->text()) : std::string();
    };
    const std::string problem = accountConflict(cell(1), cell(2), cell(3));

    for (int c = 1; c <= 3; ++c) {
        QTableWidgetItem* item = accounts_->item(row, c);
        if (!item) continue;
        // Clearing the role, not setting a default-constructed QBrush: that
        // stores a valid black brush and would paint these cells black on
        // black in dark mode.
        if (problem.empty()) {
            item->setData(Qt::ForegroundRole, QVariant());
            item->setData(Qt::ToolTipRole,    QVariant());
        } else {
            item->setForeground(QBrush(Qt::red));
            item->setToolTip(qstr(problem));
        }
    }
}

std::vector<BankAccount> CompanyPage::collectAccounts() const {
    std::vector<BankAccount> out;
    for (int row = 0; row < accounts_->rowCount(); ++row) {
        auto text = [this, row](int c) {
            return accounts_->item(row, c) ? sstr(accounts_->item(row, c)->text()) : std::string();
        };
        BankAccount a;
        a.label       = text(0);
        a.iban        = sk::normalizeIban(text(1));
        a.localNumber = text(2);
        a.bic         = text(3);
        a.bankName    = text(4);
        a.currency    = text(5);
        if (const QComboBox* qr = qobject_cast<QComboBox*>(accounts_->cellWidget(row, 6)))
            a.qrFormat = static_cast<QrFormat>(qr->currentData().toInt());
        for (char& c : a.currency) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (a.iban.empty() && a.localNumber.empty()) continue;   // a blank row is not an account
        a.isDefault = out.empty();                               // the first one is the fallback
        out.push_back(std::move(a));
    }
    return out;
}

void CompanyPage::applyVatModeHint() {
    auto* hint = findChild<QLabel*>("vatHint");
    if (!hint) return;

    switch (vatModeFromCode(sstr(vatMode_->currentData().toString()))) {
        case VatMode::Payer:
            hint->setText("Na faktúrach sa účtuje DPH a v editore sa dajú nastaviť sadzby.");
            break;
        case VatMode::RegisteredNotPayer:
            hint->setText("IČ DPH sa používa len pri obchode v rámci EÚ. Na tuzemských "
                          "faktúrach sa DPH neúčtuje – stĺpce DPH sú v editore skryté.");
            break;
        default:
            hint->setText("Na faktúrach sa DPH neúčtuje – stĺpce DPH sú v editore skryté.");
            break;
    }
}

void CompanyPage::updatePreview() {
    NumberSeries s;
    s.prefix  = lineText(prefix_);
    s.year    = year_->value();
    s.next    = next_->value();
    s.padding = padding_->value();
    preview_->setText(qstr(s.format(s.next)));
}

bool CompanyPage::save() {
    Company c = db_.activeCompany();
    party_->applyTo(c);
    c.accounts     = collectAccounts();
    c.registryNote = sstr(registry_->toPlainText());
    // Keep the company-level fields pointing at a sensible account, so
    // anything reading them without a currency still gets something real.
    c.iban.clear(); c.bic.clear(); c.bankName.clear(); c.bankLocalNumber.clear();
    c.useAccountFor(profileFor(c.address.countryCode).currency);
    c.vatMode      = vatModeFromCode(sstr(vatMode_->currentData().toString()));
    c.logo         = logo_;
    c.logoPath     = logoPath_;

    QStringList warnings;
    QString idProblems;
    if (!party_->checkIdentifiers(&idProblems)) warnings << idProblems;
    for (const BankAccount& a : c.accounts) {
        if (a.iban.empty() && a.localNumber.empty()) continue;
        if (!a.iban.empty() && !sk::validIban(a.iban))
            warnings << QString("IBAN %1 neprešiel kontrolou (mod-97).").arg(qstr(a.iban));
    }
    if (c.accounts.empty())
        warnings << "Nie je zadaný žiadny bankový účet – na faktúre nebude kam zaplatiť.";
    if (c.vatMode == VatMode::Payer && c.icDph.empty())
        warnings << "Režim je nastavený na platiteľa DPH, ale chýba IČ DPH.";

    if (!warnings.isEmpty()) {
        const auto answer = QMessageBox::warning(
            this, "Skontrolujte údaje",
            warnings.join("\n") + "\n\nUložiť aj tak?",
            QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Save) return false;
    }

    if (!db_.saveCompany(c)) {
        QMessageBox::critical(this, "Chyba", qstr(db_.lastError()));
        return false;
    }

    NumberSeries s;
    s.prefix  = lineText(prefix_);
    s.year    = year_->value();
    s.next    = next_->value();
    s.padding = padding_->value();
    db_.setSeries(s);

    // A rename has to reach the selector and the window title, or the old name
    // sits there until the tab is left and re-entered.
    loadedCompanyId_ = c.id;
    reloadCompanyList();
    if (onActiveCompanyChanged) onActiveCompanyChanged();
    return true;
}

} // namespace fk
