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

// Czech.h - CZ-specific formats and checksums.
//
// Deliberately mirrors sk/Slovak.h. Note that the Czech and Slovak IČO check
// digits are the *same* algorithm, inherited from the shared Czechoslovak
// standard: the usual "(11 - r) % 10" and "r == 0 -> 1, r == 1 -> 0" phrasings
// produce identical results for all ten million inputs. They are kept as
// separate entry points anyway, because that is an accident of history rather
// than a guarantee, and a future divergence should touch one country only.
#pragma once

#include <string>

namespace fk::cz {

/// 8 digits with a mod-11 check digit (Act No. 89/1995 Coll.).
/// Identical in practice to the Slovak check — see the note above.
bool validIco(const std::string& ico);

/// "CZ" + 8 to 10 digits. For a company it is CZ + IČO; for a natural person
/// it is CZ + rodné číslo, so the length varies and only the shape is checked.
bool validDic(const std::string& dic);

/// "45274649" -> "CZ45274649". Empty if the IČO is not 8 digits.
std::string dicFromIco(const std::string& ico);

/// 12345 -> "12345", and formats for display as "123 45".
std::string normalizePsc(const std::string& psc);
std::string formatPsc(const std::string& psc);

/// Turns the ARES spisová značka "B 1581/MSPH" into the line that belongs on
/// an invoice: "Zapsáno v OR vedeném Městským soudem v Praze, oddíl B, vložka 1581".
std::string registryNoteFromSpisovaZnacka(const std::string& znacka);

/// "MSPH" -> "Městským soudem v Praze". Returns the code unchanged if unknown.
std::string courtName(const std::string& code);

} // namespace fk::cz
