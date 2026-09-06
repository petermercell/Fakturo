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

// Validator.h - a hand-written subset of the EN 16931 / Peppol business rules,
// plus the Slovak checks. Runs in microseconds, needs no Schematron engine and
// catches the mistakes people actually make when typing an invoice.
//
// It is NOT a substitute for full Schematron validation before you go live.
// See README "Validation" for how to run the official rules.
#pragma once

#include "../model/Model.h"

#include <string>
#include <vector>

namespace fk {

enum class Severity { Error, Warning };

struct Issue {
    Severity    severity;
    std::string code;      // e.g. "BR-06" or "SK-IBAN"
    std::string message;   // shown to the user, in Slovak
};

struct ValidationResult {
    std::vector<Issue> issues;

    bool ok() const {
        for (const Issue& i : issues)
            if (i.severity == Severity::Error) return false;
        return true;
    }
    bool empty() const { return issues.empty(); }
    int  errorCount() const;
    int  warningCount() const;
};

ValidationResult validate(const Invoice& inv, const Company& seller);

} // namespace fk
