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

// Units.h - the units people actually invoice in, with their UN/ECE codes.
//
// Every line carries a unit twice: the text printed on the PDF ("hod") and the
// UN/ECE Recommendation 20 code that goes into the Peppol XML ("HUR"). Until
// now the code was hard-wired to H87 — "piece" — for everything, so an invoice
// for eight hours of work went out as eight pieces. Wrong, invisible on the
// printed page, and rejected by a validating Access Point.
//
// Only codes checked against a published list are here. Peppol validates the
// element against this very list (BR-CL-23), so a plausible-looking invention
// would be a rejected invoice.
#pragma once

#include "../country/Country.h"

#include <string>
#include <vector>

namespace fk {

struct UnitOfMeasure {
    const char* code;      ///< UN/ECE Rec 20, e.g. "HUR"
    const char* labelSk;   ///< what is printed, e.g. "hod"
    const char* labelCz;
};

/// The offered list, in the order a picker should show it: the common ones
/// first, then time, then length, then weight and volume.
const std::vector<UnitOfMeasure>& unitsOfMeasure();

/// The code for a label, matched case- and diacritic-insensitively against
/// both languages. Empty when the label is not one of ours — free text stays
/// allowed, and the caller decides what code to keep.
std::string unitCodeForLabel(const std::string& label);

/// What to print for a code, in `country`'s language. Falls back to the code
/// itself, which is at least honest.
std::string unitLabel(const std::string& code, Country country);

/// True for a code in the list above.
bool isKnownUnitCode(const std::string& code);

} // namespace fk
