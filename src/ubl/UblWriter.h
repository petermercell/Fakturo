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

// UblWriter.h - Invoice -> Peppol BIS Billing 3.0 UBL 2.1 XML.
#pragma once

#include "../model/Model.h"

#include <string>

namespace fk {

namespace peppol {
constexpr const char* CUSTOMIZATION_ID =
    "urn:cen.eu:en16931:2017#compliant#urn:fdc:peppol.eu:2017:poacc:billing:3.0";
constexpr const char* PROFILE_ID = "urn:fdc:peppol.eu:2017:poacc:billing:01:1.0";
} // namespace peppol

/// Serialises the invoice. The result is a complete standalone XML document,
/// ready to hand to an Access Point or to save next to the PDF.
std::string writeUbl(const Invoice& inv, const Company& seller);

} // namespace fk
