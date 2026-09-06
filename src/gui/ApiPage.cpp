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

#include "ApiPage.h"

#include "../platform/Keychain.h"
#include "GuiUtil.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QFont>
#include <QSignalBlocker>
#include <QStringList>
#include <QVariant>
#include <QVBoxLayout>

namespace fk {

using namespace fk::gui;

namespace {

/// Grey, small, wrapped: the explanatory text that is on this page rather than
/// in a manual nobody has.
QLabel* hint(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    QFont f = label->font();
    f.setPointSizeF(f.pointSizeF() - 1.0);
    label->setFont(f);
    return label;
}

} // namespace

ApiPage::ApiPage(Database& db, QWidget* parent) : QWidget(parent), db_(db) {
    // Without this, findChild<ApiPage*>() from a test would match the first
    // plain QWidget it met and cast it to the wrong type. There is no
    // Q_OBJECT in this project, so the name is the only handle there is.
    setObjectName("apiPage");

    enabled_     = new QCheckBox("Používať elektronické doručovanie cez SAPI", this);
    environment_ = new QComboBox(this);
    host_        = new QLineEdit(this);
    clientId_    = new QLineEdit(this);
    participant_ = new QLineEdit(this);
    secret_      = new QLineEdit(this);
    secretStatus_ = new QLabel(this);
    summary_     = new QLabel(this);
    participantHint_ = new QLabel(this);

    enabled_->setObjectName("apiEnabled");
    environment_->setObjectName("apiEnvironment");
    host_->setObjectName("apiHost");
    clientId_->setObjectName("apiClientId");
    participant_->setObjectName("apiParticipant");
    secret_->setObjectName("apiSecret");
    secretStatus_->setObjectName("apiSecretStatus");
    summary_->setObjectName("apiSummary");
    participantHint_->setObjectName("apiParticipantHint");

    // Sandbox first, and the default, because sandbox credentials fail against
    // the live host on purpose. Starting on "live" would make the first thing
    // a new user does the thing that costs €0,10 a document.
    environment_->addItem("Testovacia (sandbox)", "sandbox");
    environment_->addItem("Ostrá prevádzka", "live");

    host_->setPlaceholderText(qstr(sapi::baseUrl(sapi::Environment::Sandbox)));
    clientId_->setPlaceholderText("client id od poskytovateľa");
    participant_->setPlaceholderText("0245:2120345678");
    secret_->setEchoMode(QLineEdit::Password);
    secret_->setPlaceholderText("client secret — uloží sa do Kľúčenky, nie do databázy");
    participantHint_->setWordWrap(true);
    summary_->setWordWrap(true);
    summary_->setTextFormat(Qt::PlainText);
    secretStatus_->setTextFormat(Qt::PlainText);

    // ------------------------------------------------------------ connection
    auto* connection = new QGroupBox("Pripojenie", this);
    auto* cf = new QFormLayout(connection);
    auto* fillBtn = new QPushButton("Doplniť z firmy", this);
    fillBtn->setObjectName("apiFillParticipant");
    auto* participantRow = new QHBoxLayout;
    participantRow->addWidget(participant_, 1);
    participantRow->addWidget(fillBtn);

    cf->addRow(enabled_);
    cf->addRow("Prostredie", environment_);
    cf->addRow("Adresa servera", host_);
    cf->addRow(hint("Prázdne znamená štandardnú adresu poskytovateľa. SAPI-SK 1.0 je "
                    "otvorený štandard, ktorý používajú všetci slovenskí operátori — "
                    "zmena poskytovateľa je preto zmena tohto riadku, nie prestavba.",
                    this));
    cf->addRow("Client ID", clientId_);
    cf->addRow("Identifikátor účastníka", participantRow);
    cf->addRow(participantHint_);

    // ---------------------------------------------------------------- secret
    auto* secretBox = new QGroupBox("Heslo (client secret)", this);
    auto* sf = new QVBoxLayout(secretBox);
    saveSecret_  = new QPushButton("Uložiť heslo do Kľúčenky", this);
    clearSecret_ = new QPushButton("Odstrániť uložené heslo", this);
    saveSecret_->setObjectName("apiSaveSecret");
    clearSecret_->setObjectName("apiClearSecret");

    auto* secretRow = new QHBoxLayout;
    secretRow->addWidget(saveSecret_);
    secretRow->addWidget(clearSecret_);
    secretRow->addStretch();

    sf->addWidget(hint("Heslo sa ukladá do systémovej Kľúčenky, nie do databázy — "
                       "záloha Fakturo ho teda neobsahuje a nikto ho z nej nezíska. "
                       "Každé prostredie a každá firma má vlastný záznam.", this));
    sf->addWidget(secret_);
    sf->addLayout(secretRow);
    sf->addWidget(secretStatus_);

    // ----------------------------------------------------------------- state
    auto* stateBox = new QGroupBox("Stav", this);
    auto* stf = new QVBoxLayout(stateBox);
    test_ = new QPushButton("Test pripojenia", this);
    test_->setObjectName("apiTest");
    auto* saveBtn = new QPushButton("Uložiť nastavenia", this);
    saveBtn->setObjectName("apiSave");

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(saveBtn);
    buttons->addWidget(test_);
    buttons->addStretch();

    stf->addWidget(summary_);
    stf->addLayout(buttons);
    stf->addWidget(hint(
        "Doručovanie cez sieť ešte nie je hotové — zatiaľ sa dá pripraviť účet. "
        "Test pripojenia preto kontroluje iba nastavenie, na server sa ešte "
        "nevolá.\n\n"
        "Od 1. januára 2027 musí každá zdaniteľná osoba vedieť elektronickú "
        "faktúru prijať; ako neplatiteľ podľa §7a ju nemusíte vystavovať. "
        "Doklady chodia výhradne cez certifikovaného doručovateľa (Digitálny "
        "poštár) zo zoznamu na vpds.financnasprava.sk — Fakturo nie je "
        "prístupový bod a nič samo neohlasuje.", this));

    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->addWidget(connection);
    layout->addWidget(secretBox);
    layout->addWidget(stateBox);
    layout->addStretch();

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setWidget(body);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    // Functor connects with `this` as context, as everywhere in this project:
    // no Q_OBJECT, no moc, and nothing fires into a deleted page.
    connect(saveBtn, &QPushButton::clicked, this, [this] { save(); });
    connect(test_, &QPushButton::clicked, this, [this] { testConnection(); });
    connect(saveSecret_, &QPushButton::clicked, this, [this] { saveSecret(); });
    connect(clearSecret_, &QPushButton::clicked, this, [this] { clearSecret(); });
    connect(fillBtn, &QPushButton::clicked, this, [this] { fillParticipantFromCompany(); });

    // The Keychain entry is named after the environment and the client id, so
    // either one changing points the status line at a different secret.
    connect(environment_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (loading_) return;
        host_->setPlaceholderText(qstr(sapi::baseUrl(
            environment_->currentData().toString() == "live" ? sapi::Environment::Live
                                                             : sapi::Environment::Sandbox)));
        refreshSecretStatus();
        refreshSummary();
    });
    connect(clientId_, &QLineEdit::textChanged, this, [this](const QString&) {
        if (loading_) return;
        refreshSecretStatus();
        refreshSummary();
    });
    connect(participant_, &QLineEdit::textChanged, this, [this](const QString&) {
        if (loading_) return;
        refreshSummary();
    });
    connect(host_, &QLineEdit::textChanged, this, [this](const QString&) {
        if (loading_) return;
        refreshSummary();
    });
    // The one setting this whole screen exists to control. Without this the
    // tick box was the only widget on the page with no visible effect.
    connect(enabled_, &QCheckBox::toggled, this, [this](bool) {
        if (loading_) return;
        refreshSummary();
    });

    if (!keychain::available()) {
        secret_->setEnabled(false);
        saveSecret_->setEnabled(false);
        clearSecret_->setEnabled(false);
    }

    reload();
}

sapi::Config ApiPage::currentConfig() const {
    sapi::Config c;
    c.enabled       = enabled_->isChecked();
    c.environment   = environment_->currentData().toString() == "live"
                          ? sapi::Environment::Live
                          : sapi::Environment::Sandbox;
    c.host          = lineText(host_);
    c.clientId      = lineText(clientId_);
    c.participantId = lineText(participant_);
    return c;
}

std::string ApiPage::account() const {
    return sapi::keychainAccount(companyId_, currentConfig());
}

bool ApiPage::hasUnsavedChanges() {
    if (companyId_ == 0) return false;
    const sapi::Config stored  = sapi::loadConfig(db_, companyId_);
    const sapi::Config current = currentConfig();
    return current.enabled     != stored.enabled ||
           current.environment != stored.environment ||
           current.clientId    != stored.clientId ||
           current.participantId != stored.participantId ||
           // What would be written, not what was typed: saveConfig() trims and
           // strips a trailing slash, so a host entered with one would
           // otherwise read as changed forever. Comparing effectiveHost()
           // instead would hide the opposite case — clearing a field that
           // holds the default URL looks like no change and is then typed back
           // in by the reload.
           sapi::normalisedHost(current.host) != sapi::normalisedHost(stored.host);
}

void ApiPage::reload() {
    // Switching away used to discard whatever had just been typed, and the
    // secret is stored under the client id on screen — so losing the client id
    // to a tab switch would orphan a Keychain entry the page could never find
    // again.
    if (!loading_ && hasUnsavedChanges()) {
        const auto answer = QMessageBox::question(
            this, "Neuložené zmeny",
            "Nastavenia e-Faktúry sa zmenili. Uložiť ich?",
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);
        if (answer == QMessageBox::Cancel) return;
        // A save the user asked for that did not happen must not be followed by
        // the reload that discards what they typed. save() has already said why.
        if (answer == QMessageBox::Save && !save()) return;
    }

    // Saved and restored, not set and cleared: reload() runs from the
    // constructor and from a company switch, and clearing a flag somebody
    // else set is how the widgets end up written back half-filled.
    const bool wasLoading = loading_;
    loading_ = true;

    companyId_ = db_.activeCompanyId();
    const sapi::Config c = sapi::loadConfig(db_, companyId_);

    {
        const QSignalBlocker b1(environment_), b2(clientId_), b3(participant_),
                             b4(host_), b5(enabled_);
        enabled_->setChecked(c.enabled);
        environment_->setCurrentIndex(c.environment == sapi::Environment::Live ? 1 : 0);
        setLine(host_, c.host);
        setLine(clientId_, c.clientId);
        setLine(participant_, c.participantId);
        secret_->clear();
    }
    host_->setPlaceholderText(qstr(sapi::baseUrl(c.environment)));

    loading_ = wasLoading;
    refreshSecretStatus();
    refreshSummary();
}

bool ApiPage::save() {
    // Only reachable if the database has no company at all, but writing
    // "sapi.0.clientId" would be a row nothing ever reads again.
    if (companyId_ == 0) return false;

    const sapi::Config c = currentConfig();

    // Turning it on with nothing to turn on would be a setting that lies. The
    // check is here rather than at send time because at send time it is late.
    if (c.enabled) {
        if (!c.complete()) {
            QMessageBox::warning(this, "Nastavenia",
                                 "Doplňte client ID a identifikátor účastníka, "
                                 "alebo doručovanie zatiaľ nezapínajte.");
            return false;
        }
        const std::string problem = sapi::participantProblem(c.participantId);
        if (!problem.empty()) {
            QMessageBox::warning(this, "Identifikátor účastníka", qstr(problem));
            return false;
        }
        if (keychain::available()) {
            std::string keychainError;
            if (!keychain::has(sapi::keychainService(), account(), &keychainError)) {
                QMessageBox::warning(this, "Heslo",
                    keychainError.empty()
                        ? QString("Pre toto client ID a prostredie nie je uložené heslo. "
                                  "Uložte ho, alebo doručovanie zatiaľ nezapínajte.")
                        : qstr("Kľúčenku sa nepodarilo prečítať: " + keychainError));
                return false;
            }
        }
    }

    if (!sapi::saveConfig(db_, companyId_, c)) {
        QMessageBox::critical(this, "Nastavenia",
                              qstr("Nastavenia sa nepodarilo uložiť: " + db_.lastError()));
        return false;
    }
    refreshSummary();
    return true;
}

void ApiPage::saveSecret() {
    const std::string clientId = lineText(clientId_);
    if (clientId.empty()) {
        QMessageBox::warning(this, "Heslo",
                             "Najprv zadajte client ID — heslo sa ukladá pod ním.");
        return;
    }
    const QString typed = secret_->text();     // not trimmed: a secret may end in a space
    if (typed.isEmpty()) {
        QMessageBox::warning(this, "Heslo", "Zadajte heslo.");
        return;
    }

    std::string error;
    if (!keychain::store(sapi::keychainService(), account(), typed.toStdString(), &error)) {
        QMessageBox::critical(this, "Kľúčenka", qstr(error));
        return;
    }
    // Out of the widget as soon as it is in the Keychain. It is still in the
    // process, but not sitting on screen behind dots waiting to be revealed.
    secret_->clear();
    refreshSecretStatus();
    refreshSummary();
}

void ApiPage::clearSecret() {
    if (QMessageBox::question(this, "Heslo",
                              "Odstrániť uložené heslo z Kľúčenky?") != QMessageBox::Yes)
        return;

    std::string error;
    if (!keychain::remove(sapi::keychainService(), account(), &error)) {
        QMessageBox::critical(this, "Kľúčenka", qstr(error));
        return;
    }
    secret_->clear();
    refreshSecretStatus();
    refreshSummary();
}

void ApiPage::fillParticipantFromCompany() {
    Company company;
    // A failure to read is not the same thing as a company with no DIČ, and
    // sending someone to fix a field that is already right is worse than
    // saying nothing.
    if (!db_.loadCompany(companyId_, company)) {
        QMessageBox::critical(this, "Identifikátor účastníka",
                              qstr("Firmu sa nepodarilo načítať: " + db_.lastError()));
        return;
    }
    const std::string id = sapi::defaultParticipantId(company);
    if (id.empty()) {
        QMessageBox::information(this, "Identifikátor účastníka",
                                 "Firma nemá vyplnené DIČ, z ktorého sa identifikátor "
                                 "skladá. Doplňte ho v záložke Moja firma.");
        return;
    }
    setLine(participant_, id);
    refreshSummary();
}

void ApiPage::refreshSecretStatus() {
    // The button state is set on every path. It used to be set only at the
    // end, so on a fresh install it stayed enabled, asked for confirmation and
    // then deleted nothing.
    if (!keychain::available()) {
        secretStatus_->setText("Bezpečné úložisko hesiel je zatiaľ len na macOS.");
        clearSecret_->setEnabled(false);
        return;
    }
    if (lineText(clientId_).empty()) {
        secretStatus_->setText("Heslo sa uloží pod client ID — zatiaľ žiadne nie je zadané.");
        clearSecret_->setEnabled(false);
        return;
    }

    std::string error;
    const bool stored = keychain::has(sapi::keychainService(), account(), &error);
    if (!stored && !error.empty()) {
        // A locked keychain is not an empty one. Saying "no password stored"
        // here would be stating as fact something this cannot know.
        secretStatus_->setText(qstr("Kľúčenku sa nepodarilo prečítať: " + error));
        clearSecret_->setEnabled(false);
        return;
    }
    secretStatus_->setText(stored
        ? "Heslo je uložené v Kľúčenke pre toto client ID a prostredie."
        : "Pre toto client ID a prostredie nie je uložené žiadne heslo.");
    clearSecret_->setEnabled(stored);
}

void ApiPage::refreshSummary() {
    const sapi::Config c = currentConfig();
    QString text = qstr(sapi::describe(c));

    const std::string problem = c.participantId.empty()
                                    ? std::string()
                                    : sapi::participantProblem(c.participantId);
    participantHint_->setText(problem.empty()
        ? QString("Slovensko smeruje na 0245:DIČ. IČO (0158) ani 9950:SK… sa nesmerujú.")
        : qstr(problem));
    // Loud, not decorative: a wrong identifier is invisible on the printed
    // page and fatal on the network.
    participantHint_->setStyleSheet(problem.empty() ? QString()
                                                    : "color: #b00020;");

    if (!c.complete()) text += "\nNastavenie nie je úplné.";
    if (hasUnsavedChanges()) text += "\nNeuložené zmeny — kliknite na „Uložiť nastavenia“.";
    summary_->setText(text);
}

void ApiPage::testConnection() {
    const sapi::Config c = currentConfig();
    QStringList problems;

    if (c.clientId.empty()) problems << "Chýba client ID.";
    const std::string participant = sapi::participantProblem(c.participantId);
    if (!participant.empty()) problems << qstr(participant);
    if (!keychain::available()) {
        problems << "Heslo sa na tejto platforme nedá bezpečne uložiť.";
    } else if (!c.clientId.empty()) {
        std::string keychainError;
        if (!keychain::has(sapi::keychainService(), account(), &keychainError))
            problems << (keychainError.empty()
                             ? QString("V Kľúčenke nie je heslo pre toto client ID a prostredie.")
                             : qstr("Kľúčenku sa nepodarilo prečítať: " + keychainError));
    }

    if (!problems.isEmpty()) {
        QMessageBox::warning(this, "Test pripojenia",
                             "Nastavenie zatiaľ nie je úplné:\n\n• " +
                                 problems.join("\n• "));
        return;
    }
    // Says exactly what it did and exactly what it did not. A green tick that
    // meant "the fields are filled in" would be read as "the account works".
    QMessageBox::information(
        this, "Test pripojenia",
        "Nastavenie je úplné a identifikátor má správny tvar.\n\n"
        "Server sa zatiaľ nekontaktuje — sieťová časť ešte nie je hotová. "
        "Adresa, na ktorú sa bude volať:\n" + qstr(sapi::effectiveHost(c)));
}

} // namespace fk
