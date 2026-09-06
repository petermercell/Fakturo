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

// RegistryClient.h - asynchronous lookups against two free, keyless registries:
//
//   RPO  (Štatistický úrad SR)  IČO   -> name, address, court, register entry
//   VIES (European Commission)  IČ DPH -> validity + registered name and address
//
// Both are optional. The application works completely offline; nothing here is
// ever on the path of saving an invoice.
//
// No Q_OBJECT: results come back through std::function callbacks. Each call
// takes a context QObject; if it dies first, Qt drops the connection and the
// callback never fires, so capturing `this` in the caller is safe.
#pragma once

#include "RegistryData.h"
#include "../country/Country.h"
#include "../db/Database.h"   // RegisterKind

#include <QByteArray>
#include <QObject>
#include <QtGlobal>
#include <QString>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace fk {

class RegistryClient {
public:
    /// error is empty on success. record.found == false means "no such subject".
    using Callback = std::function<void(const RegistryRecord& record, const QString& error)>;

    /// `parent` owns the internal QNetworkAccessManager. Passing the widget
    /// that uses this client is the intended usage.
    explicit RegistryClient(QObject* parent);
    ~RegistryClient();
    RegistryClient(const RegistryClient&) = delete;
    RegistryClient& operator=(const RegistryClient&) = delete;

    /// Look up a company by its registration number. Slovak IČO goes to RPO,
    /// Czech IČO to ARES; both are public and keyless.
    void lookupCompany(Country country, const QString& ico, QObject* context, Callback done);

    /// Validate an EU VAT number in VIES and fetch the registered name/address.
    /// Accepts "SK2120345678" or a bare number plus a country code.
    void lookupVat(const QString& vatNumber, QObject* context, Callback done);

    /// Downloads one of the daily Finančná správa register ZIPs. No API key.
    /// `progress` gets (received, total); total is -1 while unknown.
    using DownloadProgress = std::function<void(qint64 received, qint64 total)>;
    using DownloadDone     = std::function<void(const QByteArray& zip, const QString& error)>;
    /// Returns the reply so the caller can abort() it on cancel. Ownership
    /// stays with the client; it deletes itself when finished.
    QNetworkReply* downloadRegister(RegisterKind kind, QObject* context,
                                    DownloadProgress progress, DownloadDone done);

    /// Exposed for testing: parse a raw RPO / ARES / VIES response body.
    static RegistryRecord parseRpo(const QByteArray& json);
    static RegistryRecord parseAres(const QByteArray& json);
    static RegistryRecord parseVies(const QByteArray& json, const QString& countryCode);
    /// Turns the VIES `userError` field into something a person can act on.
    /// Empty when the answer was a plain, valid "no".
    static QString viesErrorText(const QByteArray& json);

    void setTimeoutMs(int ms) { timeoutMs_ = ms; }

private:
    QNetworkAccessManager* nam_ = nullptr;
    int                    timeoutMs_ = 8000;
};

} // namespace fk
