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

// ApiPage.h - the "e-Faktúra" tab: the SAPI account this company sends and
// receives through.
//
// The transport is not written yet, and this screen says so rather than
// pretending otherwise. What it does do is everything that can be finished
// without a server: the host and environment, the client id, the participant
// identifier — checked against the rule that actually applies in Slovakia —
// and the client secret, put in the macOS Keychain and never in the database.
//
// Per company. The participant identifier is 0245:DIČ, which names exactly one
// company, so a second company gets its own account and its own Keychain
// entry.
#pragma once

#include "../db/Database.h"
#include "../peppol/SapiConfig.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace fk {

class ApiPage : public QWidget {
public:
    ApiPage(Database& db, QWidget* parent = nullptr);

    /// Re-reads the active company's settings. Called when the company
    /// changes, because these belong to the company and not to the app.
    void reload();

    /// Writes the settings (not the secret — that has its own button).
    /// Shows its own message box on failure.
    bool save();

    /// What is on screen right now, whether or not it has been saved. The
    /// transport will want this.
    sapi::Config currentConfig() const;

private:
    /// Whether the fields differ from what is in the database. Reading the
    /// database, so not const. Switching away used to discard whatever had
    /// just been typed — CompanyPage carries the same guard for the same
    /// reason.
    bool hasUnsavedChanges();
    void refreshSecretStatus();
    void refreshSummary();
    void saveSecret();
    void clearSecret();
    void fillParticipantFromCompany();
    void testConnection();
    /// The Keychain entry the fields on screen point at. Changing the client
    /// id or the environment points at a different one — which is why the
    /// status line has to be recomputed whenever either changes.
    std::string account() const;

    Database&    db_;
    int64_t      companyId_ = 0;
    QCheckBox*   enabled_       = nullptr;
    QComboBox*   environment_   = nullptr;
    QLineEdit*   host_          = nullptr;
    QLineEdit*   clientId_      = nullptr;
    QLineEdit*   participant_   = nullptr;
    QLabel*      participantHint_ = nullptr;
    QLineEdit*   secret_        = nullptr;
    QLabel*      secretStatus_  = nullptr;
    QPushButton* saveSecret_    = nullptr;
    QPushButton* clearSecret_   = nullptr;
    QPushButton* test_          = nullptr;
    QLabel*      summary_       = nullptr;
    bool         loading_       = false;
};

} // namespace fk
