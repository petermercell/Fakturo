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

// Statement.h - reading a bank statement.
//
// Fio's CSV, as documented in their API_Bankovnictvi.pdf: semicolon separated,
// UTF-8, a key;value header block, then a column header row, then one line per
// movement. Amounts use a decimal comma, dates are dd.mm.rrrr.
//
// Fio operates in both countries and the Slovak export uses Slovak column
// names, so columns are found by name rather than by position, with the
// diacritics folded and both languages accepted. Position is the fallback.
//
// No Qt and no database here: this is a string in, records out.
#pragma once

#include "../core/Dec.h"

#include <string>
#include <vector>

namespace fk {

/// One movement on the account. Amounts keep the bank's sign: credits are
/// positive, debits negative.
struct BankTransaction {
    std::string bankId;          // "ID pohybu" — the bank's own unique number
    std::string orderId;         // "ID pokynu"
    std::string date;            // ISO yyyy-mm-dd
    Dec         amount;
    std::string currency;

    std::string counterAccount;  // "Protiúčet"
    std::string counterBankCode;
    std::string counterName;

    std::string variableSymbol;
    std::string constantSymbol;
    std::string specificSymbol;

    std::string message;         // "Zpráva pro příjemce"
    std::string userIdentification;
    std::string comment;
    std::string type;            // "Platba převodem uvnitř banky" and friends

    bool isCredit() const { return amount.raw() > 0; }

    /// Everything a customer might have typed the invoice number into. Fio
    /// spreads it across three fields and payers are not consistent.
    std::string freeText() const;

    /// What identifies this movement across two imports.
    ///
    /// Normally the bank's own "ID pohybu", which is exactly what it is for.
    /// A hand-trimmed export can lack it, and a movement with no key would get
    /// a payment on the first import and another on the second — so a digest
    /// of the fields that identify the movement stands in. Two genuinely
    /// identical payments on one day collapse to one key; declining to import
    /// a real duplicate is the safer of the two mistakes.
    std::string dedupKey() const;
};

struct Statement {
    // From the header block. Any of it may be absent in a hand-trimmed file.
    std::string accountId;
    std::string bankCode;
    std::string currency;
    std::string iban;
    std::string bic;
    std::string dateStart;       // ISO
    std::string dateEnd;         // ISO

    std::vector<BankTransaction> transactions;

    /// Lines that could not be read. The import goes ahead without them and
    /// says so, rather than throwing away a statement over one bad row.
    std::vector<std::string> warnings;

    bool        ok = false;
    std::string error;           // set only when nothing could be read at all
};

/// Which reader a file needs.
enum class StatementFormat { Unknown, FioCsv, Camt053 };

/// Decides from the file's own contents, not its extension.
StatementFormat detectStatementFormat(const std::string& text);

/// Reads whichever format the file is in. The one entry point the GUI needs.
Statement parseStatement(const std::string& text);

/// ISO 20022 camt.053, in the ČBA/SBA national flavour: the Czech and Slovak
/// symbols ride inside the reference fields as "VS…", "KS…" and "SS…", the
/// amount is unsigned with the direction in CdtDbtInd, and only BOOK entries
/// are real money.
Statement parseCamt053(const std::string& text);

/// Parses Fio's CSV. `text` is the file exactly as it came off disk, including
/// any byte order mark; Windows-1250 is detected and converted, since not every
/// export is the UTF-8 the specification promises.
Statement parseFioCsv(const std::string& text);

// ------------------------------------------------------------ pieces, exposed
// for testing. Getting these individually right is most of the work.

/// Splits one CSV line on semicolons, honouring double-quoted fields with
/// doubled quotes inside them. Trailing empty field from a trailing separator
/// is kept: the caller decides whether it matters.
std::vector<std::string> splitCsvLine(const std::string& line);

/// "01.03.2012" to "2012-03-01". Empty when it is not a date.
std::string dateFromFio(const std::string& text);

/// Lowercases and strips diacritics, so "Zpráva pro příjemce" and "Správa pre
/// príjemcu" can be compared with something.
std::string foldHeader(const std::string& text);

/// True when `text` is valid UTF-8. Fio's own export is, but files that have
/// been through a spreadsheet often are not.
bool isValidUtf8(const std::string& text);

/// Windows-1250 to UTF-8. Used only when the input is not valid UTF-8.
std::string cp1250ToUtf8(const std::string& text);

} // namespace fk
