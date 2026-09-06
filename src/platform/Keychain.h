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

// Keychain.h - the one place a secret is kept, and the only place.
//
// Fakturo's SAPI client secret is the credential that can send an invoice in
// studio 202's name. It must therefore not be in the database (which is
// backed up, copied to a second machine, and inspectable with any SQLite
// browser), not in a settings file, and not in a crash dump. On macOS the
// answer is the Keychain: the operating system holds it, ties it to this
// application, and asks the user before handing it to anything else.
//
// Deliberately not Qt. QSettings would put it in a plist in plain text, and
// there is no Qt keychain in a build with no third-party dependencies. This is
// forty lines of Security.framework instead.
//
// On anything that is not Apple, every call fails and says why. That is the
// honest behaviour: silently keeping the secret in memory, or worse in the
// database, would be a security hole that looks like a working feature. macOS
// is the only platform Fakturo ships on today; the day Windows matters, the
// Credential Manager goes in the same #ifdef.
#pragma once

#include <string>

namespace fk {
namespace keychain {

/// Whether this build can store secrets at all. False on non-Apple platforms,
/// where the settings screen says so rather than pretending.
bool available();

/// Writes (or replaces) the secret for `service`/`account`.
/// `error` is filled in Slovak on failure.
bool store(const std::string& service, const std::string& account,
           const std::string& secret, std::string* error = nullptr);

/// Reads the secret back. Returns false and leaves `out` untouched when there
/// is none — a missing entry is *not* an error worth showing, so `error` stays
/// empty in that case and `found` distinguishes the two.
bool load(const std::string& service, const std::string& account,
          std::string& out, bool* found = nullptr, std::string* error = nullptr);

/// True when an entry exists, without reading it — attributes only, so this
/// does not pull the secret into the process and does not prompt.
///
/// False has two meanings and they are not the same: "there is nothing there"
/// leaves `error` empty, while a locked keychain or a refused prompt fills it.
/// A screen that reported the second as the first would state as fact that no
/// password is stored while one is sitting there unreadable.
bool has(const std::string& service, const std::string& account,
         std::string* error = nullptr);

/// Removes it. Removing something that is not there succeeds.
bool remove(const std::string& service, const std::string& account,
            std::string* error = nullptr);

} // namespace keychain
} // namespace fk
