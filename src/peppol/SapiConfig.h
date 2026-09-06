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

// SapiConfig.h - what Fakturo remembers about a SAPI account, and where.
//
// The transport is not written yet. This is the half that can be written
// without a server: which host, which client, which participant, and the rule
// that **the secret is never one of these fields**.
//
// The split is deliberate and is the whole point of the file:
//
//   * everything here goes in the `settings` table, which means it goes into
//     the backup file and into any copy of it;
//   * the client secret goes in the macOS Keychain, which does not.
//
// A backup that restores a working connection is a backup that is a
// credential. Losing a laptop bag should not hand somebody the ability to send
// invoices in studio 202's name, so the secret is stored where the operating
// system will ask before giving it up, and this struct has no field for it.
//
// Per company, not global: the participant identifier is 0245:DIČ, so it names
// one company, and Fakturo already holds several.
#pragma once

#include "Sapi.h"

#include <string>

namespace fk {

class Database;
struct Company;

namespace sapi {

/// The persisted half of a connection.
struct Config {
    /// Off until Peter has typed credentials and a test has passed. Nothing
    /// polls, sends or renews while this is false.
    bool        enabled = false;
    Environment environment = Environment::Sandbox;
    /// Normally empty, meaning "use baseUrl(environment)". SAPI-SK is an open
    /// standard shared by the Slovak operators, so changing provider is this
    /// one field — not a rebuild. Kept for exactly that day.
    std::string host;
    std::string clientId;
    /// "0245:2120345678". Defaults to the company's own participant identifier
    /// but is editable, because a provider may have issued a different one and
    /// overriding what the user was told would be wrong.
    std::string participantId;

    /// True when there is enough here to attempt a connection. Says nothing
    /// about whether the credentials are any good — only the server knows.
    bool complete() const { return !clientId.empty() && !participantId.empty(); }
};

/// The override exactly as it will be stored: trimmed, with no trailing slash.
/// Exposed because a screen comparing "what is typed" against "what is saved"
/// has to compare the same form, or an empty field and a stored default look
/// identical and an edit is silently thrown away.
std::string normalisedHost(const std::string& host);

/// The URL calls should go to: the override when there is one, otherwise the
/// host that belongs to the environment.
std::string effectiveHost(const Config& config);

/// Turns the stored config into what the protocol layer asks for.
Credentials credentialsFor(const Config& config);

/// What `participantId` should default to for `company` — peppolParticipant()
/// in the "0245:DIČ" form, or empty when the company has no DIČ yet.
std::string defaultParticipantId(const Company& company);

/// Rejects what the server would reject anyway, in Slovak, before a round
/// trip. Empty return means it looks usable.
std::string participantProblem(const std::string& participantId);

/// The Keychain entry a secret lives under. Sandbox and live secrets are
/// different secrets and must never overwrite one another, so the environment
/// is part of the name — as is the company, since each has its own account.
///
/// Shape: "sapi:<sandbox|live>:<companyId>:<clientId>".
std::string keychainAccount(int64_t companyId, const Config& config);

/// The service name for every Fakturo Keychain entry.
const char* keychainService();

// --------------------------------------------------------------- persistence
// Stored in `settings` under "sapi.<companyId>.<field>". Free-form keys, so no
// migration and no schema bump: a database written by an older build simply
// has no such rows and reads back the defaults.

/// The settings key for one field, exposed so a test can assert the shape
/// rather than restate it.
std::string configKey(int64_t companyId, const std::string& field);

Config loadConfig(Database& db, int64_t companyId);
bool    saveConfig(Database& db, int64_t companyId, const Config& config);

/// Everything about the connection except the secret, in one line, for the
/// settings screen and for a log that must not leak.
std::string describe(const Config& config);

} // namespace sapi
} // namespace fk
