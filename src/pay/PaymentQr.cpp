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

#include "PaymentQr.h"

#include "../core/Lzma1.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <vector>

namespace fk {
namespace {

/// UTF-8 sequences that Slovak and Czech actually use, and their ASCII base.
struct Fold { const char* from; char to; };
const Fold FOLDS[] = {
    {"á",'a'},{"ä",'a'},{"č",'c'},{"ď",'d'},{"é",'e'},{"ě",'e'},{"í",'i'},{"ĺ",'l'},
    {"ľ",'l'},{"ň",'n'},{"ó",'o'},{"ô",'o'},{"ö",'o'},{"ŕ",'r'},{"ř",'r'},{"š",'s'},
    {"ť",'t'},{"ú",'u'},{"ů",'u'},{"ü",'u'},{"ý",'y'},{"ž",'z'},
    {"Á",'A'},{"Ä",'A'},{"Č",'C'},{"Ď",'D'},{"É",'E'},{"Ě",'E'},{"Í",'I'},{"Ĺ",'L'},
    {"Ľ",'L'},{"Ň",'N'},{"Ó",'O'},{"Ô",'O'},{"Ö",'O'},{"Ŕ",'R'},{"Ř",'R'},{"Š",'S'},
    {"Ť",'T'},{"Ú",'U'},{"Ů",'U'},{"Ü",'U'},{"Ý",'Y'},{"Ž",'Z'},
};

/// Digits only, keeping at most `maxDigits` from the right. Both standards cap
/// the variable and specific symbols at 10 digits and the constant symbol at 4;
/// an over-long value is a rejected payment, not a truncated one.
std::string digitsOnly(const std::string& text, size_t maxDigits = 10) {
    std::string out;
    for (char c : text)
        if (c >= '0' && c <= '9') out.push_back(c);
    if (out.size() > maxDigits) out = out.substr(out.size() - maxDigits);
    return out;
}

std::string upperNoSpace(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

/// "2026-08-11" -> "20260811". Empty for anything else.
std::string compactDate(const std::string& iso) {
    if (iso.size() != 10 || iso[4] != '-' || iso[7] != '-') return {};
    return iso.substr(0, 4) + iso.substr(5, 2) + iso.substr(8, 2);
}

/// Values may not contain the separator, in either format.
std::string clean(const std::string& value, char forbidden) {
    std::string out = deburr(value);
    std::replace(out.begin(), out.end(), forbidden, ' ');
    std::replace(out.begin(), out.end(), '\n', ' ');
    std::replace(out.begin(), out.end(), '\r', ' ');
    return out;
}

} // namespace

std::string deburr(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        bool folded = false;
        for (const Fold& f : FOLDS) {
            const size_t len = std::char_traits<char>::length(f.from);
            if (text.compare(i, len, f.from) == 0) {
                out.push_back(f.to);
                i += len;
                folded = true;
                break;
            }
        }
        if (folded) continue;

        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) { out.push_back(static_cast<char>(c)); ++i; continue; }

        // Any other non-ASCII: drop the whole UTF-8 sequence rather than emit
        // bytes a banking app will render as noise.
        ++i;
        while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) ++i;
    }
    return out;
}

std::string base32hexEncode(const std::string& data) {
    static const char* CHARS = "0123456789ABCDEFGHIJKLMNOPQRSTUV";
    std::string out;
    uint32_t buffer = 0;
    int bits = 0;
    for (unsigned char byte : data) {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out.push_back(CHARS[(buffer >> bits) & 0x1F]);
        }
    }
    if (bits > 0) out.push_back(CHARS[(buffer << (5 - bits)) & 0x1F]);
    return out;
}

// ------------------------------------------------------------------- Czechia
std::string spaydPayload(const PaymentRequest& request) {
    if (!request.usable()) return {};

    std::string out = "SPD*1.0";
    out += "*ACC:" + upperNoSpace(request.iban);
    if (!request.bic.empty()) out += "+" + upperNoSpace(request.bic);
    out += "*AM:" + request.amount.roundTo(2).toString(2);
    out += "*CC:" + upperNoSpace(request.currency);

    const std::string vs = digitsOnly(request.variableSymbol);
    const std::string ks = digitsOnly(request.constantSymbol, 4);
    const std::string ss = digitsOnly(request.specificSymbol);
    if (!vs.empty()) out += "*X-VS:" + vs;
    if (!ks.empty()) out += "*X-KS:" + ks;
    if (!ss.empty()) out += "*X-SS:" + ss;

    const std::string due = compactDate(request.dueDate);
    if (!due.empty()) out += "*DT:" + due;

    // MSG is limited to 60 characters by the specification.
    std::string message = clean(request.note.empty() ? request.invoiceNumber : request.note, '*');
    if (message.size() > 60) message = message.substr(0, 60);
    if (!message.empty()) out += "*MSG:" + message;

    if (!request.beneficiaryName.empty()) {
        std::string name = clean(request.beneficiaryName, '*');
        if (name.size() > 35) name = name.substr(0, 35);
        out += "*RN:" + name;
    }
    return out;
}

// ------------------------------------------------------------------ Slovakia
std::string payBySquarePayload(const PaymentRequest& request) {
    if (!request.usable()) return {};

    // Field order is fixed by the standard; every field is present even when
    // empty, because position is what identifies it.
    std::vector<std::string> fields;
    auto add = [&fields](const std::string& value) { fields.push_back(clean(value, '\t')); };

    add(request.invoiceNumber);                 // invoiceId
    add("1");                                   // one payment
    add("1");                                   // type: simple payment order
    add(request.amount.roundTo(2).toString(2));
    add(upperNoSpace(request.currency));
    add(compactDate(request.dueDate));
    add(digitsOnly(request.variableSymbol));
    add(digitsOnly(request.constantSymbol, 4));
    add(digitsOnly(request.specificSymbol));
    add("");                                    // originator's reference
    add(request.note);
    add("1");                                   // one bank account
    add(upperNoSpace(request.iban));
    add(upperNoSpace(request.bic));
    add("0");                                   // no standing order
    add("0");                                   // no direct debit
    add(request.beneficiaryName);
    add(request.beneficiaryStreet);
    add(request.beneficiaryCity);

    std::string payload;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) payload.push_back('\t');
        payload += fields[i];
    }

    // CRC32 of the payload, little endian, in front of it.
    const uint32_t sum = static_cast<uint32_t>(
        ::crc32(0L, reinterpret_cast<const Bytef*>(payload.data()),
                static_cast<uInt>(payload.size())));
    std::string checked;
    for (int i = 0; i < 4; ++i) checked.push_back(static_cast<char>((sum >> (i * 8)) & 0xFF));
    checked += payload;

    const std::string body = lzma1CompressLiterals(checked);

    // Two header bytes (type, version, document type, reserved as nibbles),
    // then the *uncompressed* length little endian, then the LZMA body.
    //
    // Version 0 (1.0.0), not 2 (1.2.0). The field layout is identical — both
    // decode to exactly the same payment through the reference implementation
    // — but 1.0.0 is what every deployed generator emits, and banking apps
    // have been seen to reject the newer nibble. There is nothing to gain from
    // declaring a version whose only difference is that fewer apps accept it.
    constexpr uint8_t VERSION = 0x00;
    std::string out;
    out.push_back(static_cast<char>((0x00 << 4) | VERSION));
    out.push_back(static_cast<char>((0x00 << 4) | 0x00));
    out.push_back(static_cast<char>(checked.size() & 0xFF));
    out.push_back(static_cast<char>((checked.size() >> 8) & 0xFF));
    out += body;

    return base32hexEncode(out);
}

namespace {

/// The country of the account, when it is one of the two that have a standard.
/// Country::Other means "no opinion", never "somewhere else".
Country accountStandard(const PaymentRequest& request) {
    const std::string iban = upperNoSpace(request.iban);
    if (iban.size() < 2) return Country::Other;
    const Country of = countryFromCode(iban.substr(0, 2));
    return (of == Country::SK || of == Country::CZ) ? of : Country::Other;
}

Country currencyStandard(const PaymentRequest& request) {
    if (request.currency == "EUR") return Country::SK;
    if (request.currency == "CZK") return Country::CZ;
    return Country::Other;
}

} // namespace

std::vector<Country> paymentStandardsFor(const PaymentRequest& request,
                                         Country fallback,
                                         QrFormat format) {
    switch (format) {
        case QrFormat::None:        return {};
        case QrFormat::PayBySquare: return {Country::SK};
        case QrFormat::Spayd:       return {Country::CZ};
        case QrFormat::Both:        return {Country::SK, Country::CZ};
        default: break;
    }

    const Country byCurrency = currencyStandard(request);
    Country       byAccount  = accountStandard(request);

    // An account with a second address in the currency's country is reachable
    // there, so there is nothing to disagree about.
    if (byCurrency != Country::Other &&
        addressedFor(request, byCurrency).iban != upperNoSpace(request.iban))
        byAccount = byCurrency;

    if (byCurrency == Country::Other) return {byAccount == Country::Other ? fallback : byAccount};
    if (byAccount  == Country::Other) return {byCurrency};
    if (byCurrency == byAccount)      return {byCurrency};

    // A CZK invoice payable to a Slovak account, or the reverse. Print both:
    // the currency's code is what the payer's app most likely expects, and the
    // account's code is the one certain to carry the account number correctly.
    return {byCurrency, byAccount};
}

PaymentRequest addressedFor(const PaymentRequest& request, Country standard) {
    if (standard != Country::SK && standard != Country::CZ) return request;
    if (request.localNumber.empty() || request.iban.empty())  return request;

    const std::string iban = upperNoSpace(request.iban);
    if (iban.size() < 2) return request;
    if (countryFromCode(iban.substr(0, 2)) == standard) return request;  // already right

    const LocalAccount typed    = parseLocalAccount(request.localNumber);
    const LocalAccount fromIban = parseLocalAccount(localAccountFromIban(iban));
    if (!typed.valid() || !fromIban.valid())        return request;
    if (typed.bankCode == fromIban.bankCode)        return request;  // no second address
    if (typed.number   != fromIban.number)          return request;  // a different account

    const std::string rebuilt = ibanFromLocalAccount(standard, typed);
    if (rebuilt.empty()) return request;

    PaymentRequest out = request;
    out.iban = rebuilt;
    out.bic.clear();
    return out;
}

std::string paymentPayload(Country country, const PaymentRequest& request) {
    const PaymentRequest addressed = addressedFor(request, country);
    return country == Country::CZ ? spaydPayload(addressed) : payBySquarePayload(addressed);
}

const char* paymentQrLabel(Country country) {
    return country == Country::CZ ? "QR Platba" : "PAY by square";
}

} // namespace fk
