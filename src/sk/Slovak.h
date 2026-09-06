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

// Slovak.h - SK-specific formats, checksums and defaults.
#pragma once

#include "../core/Dec.h"

#include <string>
#include <vector>

namespace fk::sk {

/// VAT rates in force from 1 Jan 2025: 23 % standard, 19 % and 5 % reduced.
inline std::vector<Dec> vatRates() {
    return { Dec::fromInt(23), Dec::fromInt(19), Dec::fromInt(5), Dec::fromInt(0) };
}
inline Dec standardVatRate() { return Dec::fromInt(23); }

/// Constant symbol for payment of an invoice for goods/services.
constexpr const char* KS_INVOICE = "0308";

bool validIco(const std::string& ico);          // 8 digits + mod-11 check
bool validIcDph(const std::string& icDph);      // SK + 10 digits, mod-11
bool validIban(const std::string& iban);        // ISO 13616 mod-97
bool validDic(const std::string& dic);          // 10 digits

std::string normalizeIban(const std::string& iban);   // strip spaces, uppercase
std::string formatIban(const std::string& iban);      // groups of 4 for display

/// Digits of the invoice number, right-trimmed to 10 chars. Empty if none.
std::string variableSymbolFrom(const std::string& invoiceNumber);

std::string todayIso();
/// "2026-07-28 19:04:11" — local time, for audit entries.
std::string nowTimestamp();
std::string addDays(const std::string& iso, int days);
bool        validIsoDate(const std::string& iso);
std::string formatDateSk(const std::string& iso);     // 28.07.2026

} // namespace fk::sk
