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

// PartyForm.h - reusable editor for a Party (used for both company and customer).
// Includes the optional IČO / IČ DPH registry lookups.
#pragma once

#include "../country/Country.h"
#include "../model/Model.h"
#include "../registry/RegistryData.h"

#include <QWidget>

#include <functional>
#include <memory>

class QLabel;
class QLineEdit;
class QComboBox;
class QPushButton;

namespace fk {

class Database;
class RegistryClient;

class PartyForm : public QWidget {
public:
    explicit PartyForm(QWidget* parent = nullptr);
    ~PartyForm() override;

    void  load(const Party& p);
    void  applyTo(Party& p) const;
    Party value() const;

    /// Live feedback on IČO / IČ DPH checksums. Returns true if nothing is wrong.
    bool  checkIdentifiers(QString* message) const;

    /// Gives the form access to the locally imported VAT register, so an IČO
    /// lookup can also fill IČ DPH and DIČ. Optional: without it the form
    /// simply falls back to RPO and VIES.
    void setDatabase(Database* db) { db_ = db; }

    /// Called after a successful registry lookup, so the owner can pick up
    /// extras that are not part of Party (the company's register note).
    std::function<void(const RegistryRecord&)> onRecordApplied;

    /// Called when a lookup can tell whether the subject actually charges VAT.
    /// Only the company page cares; customers keep their own regime implicitly.
    std::function<void(VatMode)> onVatModeDetected;

private:
    void lookupByIco();
    void verifyVat();
    void applyRecordToForm(const RegistryRecord& record);
    struct VatNote {
        QString text;
        bool    isProblem = false;
    };
    /// Consults the local VAT register and fills IČ DPH / DIČ if it knows them.
    VatNote fillFromVatRegister(const QString& ico);
    void syncDicAndVat();
    void applyCountry();
    Country selectedCountry() const;
    void setStatus(const QString& text, bool isError);

    QLineEdit*   name_        = nullptr;
    QLineEdit*   ico_         = nullptr;
    QLineEdit*   dic_         = nullptr;
    QLineEdit*   icDph_       = nullptr;
    QLineEdit*   street_      = nullptr;
    QLineEdit*   street2_     = nullptr;
    QLineEdit*   city_        = nullptr;
    QLineEdit*   zip_         = nullptr;
    QComboBox*   country_     = nullptr;
    QLineEdit*   email_       = nullptr;
    QLineEdit*   phone_       = nullptr;
    QLineEdit*   contact_     = nullptr;
    QComboBox*   scheme_      = nullptr;
    QLineEdit*   endpointId_  = nullptr;
    QPushButton* icoLookup_   = nullptr;
    QPushButton* vatLookup_   = nullptr;
    QLabel*      status_      = nullptr;

    std::unique_ptr<RegistryClient> registry_;
    Database* db_ = nullptr;
};

} // namespace fk
