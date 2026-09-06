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

// PaymentQr.h - the payment string behind the QR code on an invoice.
//
//   Slovakia: PAY by square (Slovak Banking Association)
//   Czechia:  SPAYD / QR Platba (Czech Banking Association)
//
// The two could hardly be less alike. SPAYD is a readable ASCII string. PAY by
// square is a tab-separated payload, CRC32'd, LZMA-compressed and base32hex
// encoded. Both are produced here; the QR drawing itself is in the PDF layer.
#pragma once

#include "../core/Dec.h"
#include "../country/Country.h"
#include "../model/Model.h"

#include <string>
#include <vector>

namespace fk {

struct PaymentRequest {
    std::string iban;
    std::string bic;
    std::string localNumber;        // domestic form, when the account has one
    Dec         amount;
    std::string currency = "EUR";
    std::string variableSymbol;
    std::string constantSymbol;
    std::string specificSymbol;
    std::string dueDate;            // ISO yyyy-mm-dd
    std::string beneficiaryName;
    std::string beneficiaryStreet;
    std::string beneficiaryCity;
    std::string note;
    std::string invoiceNumber;

    bool usable() const { return !iban.empty() && !amount.isZero(); }
};

/// Replaces Slovak and Czech diacritics with their ASCII base letters. Banking
/// apps are unreliable with anything else, and the standards recommend it.
std::string deburr(const std::string& text);

/// base32hex, alphabet "0123456789ABCDEFGHIJKLMNOPQRSTUV", no padding.
std::string base32hexEncode(const std::string& data);

/// The Czech string, human-readable: "SPD*1.0*ACC:CZ…*AM:450.00*CC:CZK*X-VS:…"
std::string spaydPayload(const PaymentRequest& request);

/// The Slovak string: header, length, LZMA body, base32hex. Empty when the
/// request has nothing to pay.
std::string payBySquarePayload(const PaymentRequest& request);

/// The same account, addressed the way `standard`'s own country addresses it.
///
/// Fio banka operates on both sides of the border and one account is reachable
/// as 2201730826/8330 in Slovakia and 2201730826/2010 in Czechia. Stored as a
/// Slovak IBAN, it is correct — but a Czech app handed that IBAN cannot route a
/// domestic transfer, which is why paying it in Czechia always means the 2010
/// code. When the domestic number names a different bank code for the same
/// account number, that is the user telling us the second address, and a code
/// meant for that country is built from it.
///
/// The BIC is dropped along the way: it belongs to the IBAN being replaced.
/// It is optional in both standards, and a wrong one is worse than none.
PaymentRequest addressedFor(const PaymentRequest& request, Country standard);

/// Whichever the country uses. Empty when there is nothing to encode.
/// Addresses the account for `country` first.
std::string paymentPayload(Country country, const PaymentRequest& request);

/// Which standard(s) to print, in order. Usually one.
///
/// The currency decides: **EUR gets PAY by square, CZK gets QR Platba**. That
/// is the rule tested against real banking apps — Fio reads the Slovak code for
/// a EUR payment, ČSOB reads the Czech one for a CZK payment — and it is the
/// rule a user can hold in their head.
///
/// It is checked against the account's own country, because the one
/// combination known to fail is a Slovak IBAN inside a Czech code: Fio accepted
/// the payload and left the account number blank. When currency and account
/// disagree — a CZK invoice payable to a Slovak account, which means no CZK
/// account is set up — both codes are printed rather than guessing wrong.
///
/// `format` overrides all of it; `fallback` covers an account and a currency
/// that are both foreign.
std::vector<Country> paymentStandardsFor(const PaymentRequest& request,
                                         Country fallback,
                                         QrFormat format = QrFormat::Automatic);

/// Short label for the PDF, e.g. "PAY by square" or "QR Platba".
const char* paymentQrLabel(Country country);

} // namespace fk
