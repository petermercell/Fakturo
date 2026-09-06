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

#include "PartyForm.h"

#include "../country/Country.h"
#include "../cz/Czech.h"
#include "../db/Database.h"
#include "../registry/RegistryClient.h"
#include "../registry/VatRegister.h"
#include "../sk/Slovak.h"
#include "GuiUtil.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSizePolicy>
#include <QPushButton>
#include <QStringList>
#include <string>
#include <QVBoxLayout>

namespace fk {

using namespace fk::gui;

namespace {

/// A line edit with a small action button glued to its right-hand side.
QWidget* withButton(QLineEdit* edit, QPushButton* button, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->addWidget(edit, 1);
    layout->addWidget(button);
    return row;
}

} // namespace

PartyForm::PartyForm(QWidget* parent) : QWidget(parent) {
    registry_ = std::make_unique<RegistryClient>(this);

    name_       = new QLineEdit(this);
    ico_        = new QLineEdit(this);
    dic_        = new QLineEdit(this);
    icDph_      = new QLineEdit(this);
    street_     = new QLineEdit(this);
    street2_    = new QLineEdit(this);
    city_       = new QLineEdit(this);
    zip_        = new QLineEdit(this);
    email_      = new QLineEdit(this);
    phone_      = new QLineEdit(this);
    contact_    = new QLineEdit(this);
    endpointId_ = new QLineEdit(this);
    scheme_     = new QComboBox(this);
    country_    = new QComboBox(this);
    icoLookup_  = new QPushButton("Načítať", this);
    vatLookup_  = new QPushButton("Overiť", this);
    status_     = new QLabel(this);

    // Editable, so a country outside the two supported profiles can still be
    // typed in; the lookups simply stay disabled for it.
    country_->setEditable(true);
    country_->addItem("SK – Slovensko", "SK");
    country_->addItem("CZ – Česko", "CZ");
    for (const char* other : {"AT", "DE", "HU", "PL"}) country_->addItem(other, other);
    country_->setCurrentIndex(0);

    ico_->setPlaceholderText("8 číslic");
    vatLookup_->setToolTip("Overiť IČ DPH / DIČ v európskom systéme VIES");
    endpointId_->setPlaceholderText("prázdne = použije sa IČO");
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::RichText);
    // A wrapping label in a QFormLayout is given one line's height and clips
    // the rest. Every registry message this shows is a full sentence, and a
    // truncated error is worse than none — the real cause hides in the part
    // that was cut off.
    status_->setMinimumHeight(status_->fontMetrics().height() * 3);
    status_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    status_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);

    auto* identity = new QGroupBox("Identifikácia", this);
    auto* fi = new QFormLayout(identity);
    fi->addRow("Obchodné meno *", name_);
    fi->addRow("IČO", withButton(ico_, icoLookup_, this));
    fi->addRow("DIČ", dic_);
    fi->addRow("IČ DPH", withButton(icDph_, vatLookup_, this));
    fi->addRow("", status_);

    auto* address = new QGroupBox("Adresa", this);
    auto* fa = new QFormLayout(address);
    fa->addRow("Ulica a číslo *", street_);
    fa->addRow("Doplnok adresy", street2_);
    fa->addRow("Mesto *", city_);
    fa->addRow("PSČ", zip_);
    fa->addRow("Krajina (ISO) *", country_);

    auto* contact = new QGroupBox("Kontakt a Peppol", this);
    auto* fc = new QFormLayout(contact);
    fc->addRow("Kontaktná osoba", contact_);
    fc->addRow("E-mail", email_);
    fc->addRow("Telefón", phone_);
    fc->addRow("Peppol schéma", scheme_);
    fc->addRow("Peppol identifikátor", endpointId_);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(identity);
    root->addWidget(address);
    root->addWidget(contact);
    root->addStretch();

    // DIČ and IČ DPH are the same number for Slovak subjects, so filling one
    // fills the other. Only ever writes into a field the user left blank.
    connect(dic_,   &QLineEdit::editingFinished, this, [this] { syncDicAndVat(); });
    connect(icDph_, &QLineEdit::editingFinished, this, [this] { syncDicAndVat(); });

    connect(icoLookup_, &QPushButton::clicked, this, [this] { lookupByIco(); });
    connect(vatLookup_, &QPushButton::clicked, this, [this] { verifyVat(); });
    // Enter must not bypass a button that applyCountry() disabled, or an
    // Austrian IČO would be sent to the Slovak register.
    connect(ico_, &QLineEdit::returnPressed, this, [this] {
        if (icoLookup_->isEnabled()) lookupByIco();
    });
    connect(country_, &QComboBox::currentTextChanged, this, [this] { applyCountry(); });

    applyCountry();
}

Country PartyForm::selectedCountry() const {
    // An editable QComboBox does not move currentIndex when the text is set or
    // typed, so currentData() would keep returning the previously selected
    // country. Resolve from the visible text instead.
    const QString text  = country_->currentText().trimmed();
    const int     index = country_->findText(text);
    const QString code  = index >= 0 ? country_->itemData(index).toString() : text.left(2);
    return countryFromCode(sstr(code.toUpper()));
}

/// Keeps everything that depends on the country in step: Peppol schemes, the
/// IČ DPH placeholder, and which registry the buttons will talk to.
void PartyForm::applyCountry() {
    const Country        country = selectedCountry();
    const CountryProfile& p      = profileFor(country);
    const bool supported = (country == Country::SK || country == Country::CZ);

    const QString previousScheme = scheme_->count() > 0 ? scheme_->currentData().toString()
                                                        : QString();
    scheme_->clear();
    // The scheme the country's Peppol Authority routes on comes first and is
    // the default. For Slovakia that is 0245 on the DIČ; 0158 on the IČO
    // identifies the company on the document and addresses nothing, and an
    // earlier version of this form offered only the latter — so opening this
    // page and saving it used to stamp an unroutable address onto the record.
    scheme_->addItem(p.schemes.participantLabel, p.schemes.participant);
    if (std::string(p.schemes.companyId) != p.schemes.participant)
        scheme_->addItem(p.schemes.companyLabel, p.schemes.companyId);
    scheme_->addItem(p.schemes.vatLabel, p.schemes.vatId);
    const int keep = scheme_->findData(previousScheme);
    if (keep >= 0) scheme_->setCurrentIndex(keep);

    endpointId_->setPlaceholderText(
        country == Country::SK ? "prázdne = použije sa DIČ" : "prázdne = použije sa IČO");

    icDph_->setPlaceholderText(country == Country::CZ ? "CZ + 8 až 10 číslic"
                                                      : "SK + 10 číslic");
    icoLookup_->setEnabled(supported);
    icoLookup_->setToolTip(country == Country::CZ
        ? "Načítať názov, adresu a DIČ z registra ARES (MF ČR)"
        : "Načítať názov a adresu z registra právnických osôb (ŠÚ SR)");
    if (!supported)
        icoLookup_->setToolTip("Automatické načítanie je dostupné pre SK a CZ");
}

PartyForm::~PartyForm() = default;

void PartyForm::load(const Party& p) {
    setLine(name_,    p.name);
    setLine(ico_,     p.ico);
    setLine(dic_,     p.dic);
    setLine(icDph_,   p.icDph);
    setLine(street_,  p.address.street);
    setLine(street2_, p.address.street2);
    setLine(city_,    p.address.city);
    setLine(zip_,     p.address.postalCode);
    {
        const std::string code = p.address.countryCode.empty() ? std::string("SK")
                                                               : p.address.countryCode;
        const int index = country_->findData(qstr(code));
        if (index >= 0) country_->setCurrentIndex(index);
        else            country_->setCurrentText(qstr(code));
        applyCountry();
    }
    setLine(email_,   p.email);
    setLine(phone_,   p.phone);
    setLine(contact_, p.contactName);
    setLine(endpointId_, p.endpointId);
    status_->clear();

    int idx = scheme_->findData(qstr(p.endpointScheme));
    scheme_->setCurrentIndex(idx >= 0 ? idx : 0);
}

void PartyForm::applyTo(Party& p) const {
    p.name                = lineText(name_);
    p.ico                 = lineText(ico_);
    p.dic                 = lineText(dic_);
    p.icDph               = lineText(icDph_);
    p.address.street      = lineText(street_);
    p.address.street2     = lineText(street2_);
    p.address.city        = lineText(city_);
    p.address.postalCode  = lineText(zip_);
    p.address.countryCode = codeFor(selectedCountry());
    if (p.address.countryCode.empty())          // a country we have no profile for
        p.address.countryCode = sstr(country_->currentText().trimmed().left(2).toUpper());
    p.email               = lineText(email_);
    p.phone               = lineText(phone_);
    p.contactName         = lineText(contact_);
    // The scheme is only meaningful with an identifier beside it. Writing one
    // on its own is what made this form undo the correction it was supposed to
    // carry: an empty box saved "0158" and nothing else, and 0158 with nothing
    // is still enough to look like a choice the user made and be honoured as
    // one. Empty here means "work it out from the country", which is what the
    // placeholder promises.
    p.endpointId = lineText(endpointId_);
    p.endpointScheme = p.endpointId.empty() ? std::string()
                                            : sstr(scheme_->currentData().toString());
}

Party PartyForm::value() const {
    Party p;
    applyTo(p);
    return p;
}

bool PartyForm::checkIdentifiers(QString* message) const {
    const Party   p       = value();
    const Country country = countryFromCode(p.address.countryCode);
    QStringList problems;

    // Only the two countries we have real rules for get checksum warnings;
    // anything else would be a guess dressed up as a check.
    if (country == Country::SK || country == Country::CZ) {
        if (!p.ico.empty() && !validCompanyId(country, p.ico))
            problems << "IČO neprešlo kontrolou kontrolnej číslice.";
        if (!p.icDph.empty() && !validVatId(country, p.icDph))
            problems << (country == Country::CZ
                             ? "DIČ nemá platný formát (CZ + 8 až 10 číslic)."
                             : "IČ DPH nemá platný formát (SK + 10 číslic).");
        if (!p.dic.empty() && !validTaxId(country, p.dic))
            problems << (country == Country::CZ ? "DIČ nemá platný formát."
                                                : "DIČ má mať 10 číslic.");
    }
    if (message) *message = problems.join("\n");
    return problems.isEmpty();
}

void PartyForm::syncDicAndVat() {
    const Country     country = selectedCountry();
    const std::string dic     = lineText(dic_);
    const std::string vat     = lineText(icDph_);

    // In Czechia the DIČ *is* the VAT number, prefix included, so the two
    // fields hold the same string rather than one being derived from the other.
    if (country == Country::CZ) {
        if (!vat.empty() && dic.empty())      setLine(dic_, vat);
        else if (!dic.empty() && vat.empty() && cz::validDic(dic)) setLine(icDph_, dic);
        return;
    }

    if (!vat.empty() && dic.empty()) {
        setLine(dic_, dicFromIcDph(vat));
    } else if (!dic.empty() && vat.empty()) {
        const std::string derived = icDphFromDic(dic, codeFor(country));
        // Only a complete 10-digit DIČ produces a VAT number, and only for a
        // party that is actually VAT registered — so this is a suggestion the
        // user confirms with "Overiť", not a fact.
        if (!derived.empty()) setLine(icDph_, derived);
    }
}

// ------------------------------------------------------------------- lookups
void PartyForm::setStatus(const QString& text, bool isError) {
    status_->setText(text.isEmpty()
        ? QString()
        : QString("<span style='color:%1'>%2</span>")
              .arg(isError ? "#b00020" : "#2e7d32", text.toHtmlEscaped()));
}

PartyForm::VatNote PartyForm::fillFromVatRegister(const QString& ico) {
    if (!db_ || ico.isEmpty()) return {};

    // A VAT payer gives us both numbers at once, since IČ DPH is SK + DIČ.
    VatSubject subject;
    if (db_->lookupVatByIco(sstr(ico), subject)) {
        if (lineText(icDph_).empty()) setLine(icDph_, subject.icDph);
        if (lineText(dic_).empty())   setLine(dic_, subject.dic());
        if (onVatModeDetected)
            onVatModeDetected(subject.isFullVatPayer() ? VatMode::Payer
                                                       : VatMode::RegisteredNotPayer);
        return {qstr(subject.registrationNote()), false};
    }

    // Not a VAT payer, or not in that register: the income-tax register still
    // knows the DIČ, which every company has and which belongs on the invoice.
    TaxSubject taxpayer;
    if (db_->lookupTaxByIco(sstr(ico), taxpayer)) {
        if (lineText(dic_).empty()) setLine(dic_, taxpayer.dic);
        if (onVatModeDetected) onVatModeDetected(VatMode::NotRegistered);
        return {"nie je platiteľom DPH, doplnené DIČ", false};
    }

    const bool haveVat = db_->vatRegisterCount() > 0;
    const bool haveTax = db_->taxRegisterCount() > 0;
    if (!haveVat && !haveTax)
        return {"registre FS nie sú stiahnuté, IČ DPH a DIČ doplňte ručne "
                "(Moja firma → Registre finančnej správy)", true};
    if (!haveTax)
        return {"nie je platiteľom DPH; pre DIČ stiahnite register daňových subjektov", true};
    return {"v registroch finančnej správy sa nenašiel", false};
}

void PartyForm::lookupByIco() {
    const QString ico = ico_->text().trimmed();
    if (ico.isEmpty()) {
        setStatus("Najprv zadajte IČO.", true);
        return;
    }
    icoLookup_->setEnabled(false);
    setStatus("Hľadám v registri…", false);

    registry_->lookupCompany(selectedCountry(), ico, this,
                             [this](const RegistryRecord& r, const QString& error) {
        applyCountry();          // restores the button's country-dependent state
        if (!error.isEmpty() || !r.found) {
            setStatus(error.isEmpty() ? "Nenašlo sa." : error, true);
            return;
        }
        applyRecordToForm(r);

        const QString source = qstr(r.source.empty() ? "registra" : r.source);
        if (r.source == "ARES" && onVatModeDetected)
            onVatModeDetected(r.vatActive ? VatMode::Payer : VatMode::NotRegistered);

        if (r.source == "ARES") {
            // ARES already carries the DIČ and the VAT status, so there is
            // nothing local to consult.
            const QString note = r.vatActive ? "plátce DPH" : "není plátcem DPH";
            setStatus(QString("Načítané z ARES: %1 · %2").arg(qstr(r.name), note), false);
            return;
        }

        // RPO carries no tax numbers; the locally imported registers do.
        // This runs after applyRecordToForm, which rewrites every field.
        const VatNote note = fillFromVatRegister(qstr(r.ico));
        if (!note.text.isEmpty())
            setStatus(QString("Načítané z %1: %2 · %3").arg(source, qstr(r.name), note.text),
                      note.isProblem);
    });
}

void PartyForm::verifyVat() {
    const QString vat = icDph_->text().trimmed();
    if (vat.isEmpty()) {
        setStatus("Najprv zadajte IČ DPH.", true);
        return;
    }
    vatLookup_->setEnabled(false);
    setStatus("Overujem vo VIES…", false);

    registry_->lookupVat(vat, this, [this](const RegistryRecord& r, const QString& error) {
        vatLookup_->setEnabled(true);
        if (!error.isEmpty() || !r.found) {
            setStatus(error.isEmpty() ? "IČ DPH nie je platné." : error, true);
            return;
        }
        applyRecordToForm(r);
    });
}

void PartyForm::applyRecordToForm(const RegistryRecord& record) {
    Party current = value();

    // Only ask when the registry genuinely disagrees with something already typed.
    const std::vector<std::string> clashes = conflictingFields(record, current);
    bool overwrite = true;
    if (!clashes.empty()) {
        QStringList list;
        for (const std::string& f : clashes) list << qstr(f);
        const auto answer = QMessageBox::question(
            this, "Prepísať údaje?",
            "Register uvádza iné hodnoty pre:\n\n  " + list.join("\n  ") +
            "\n\nPrepísať údaje z registra?",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        overwrite = (answer == QMessageBox::Yes);
    }

    applyRecord(record, current, overwrite);
    load(current);

    if (onRecordApplied) onRecordApplied(record);

    QString source = qstr(record.source);
    if (record.source == "RPO")  source = "RPO (ŠÚ SR)";
    if (record.source == "ARES") source = "ARES (MF ČR)";
    if (source.isEmpty())        source = "registra";
    setStatus("Načítané z " + source + ": " + qstr(record.name), false);
}

} // namespace fk
