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

#include "Statement.h"

#include "../core/Sha256.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>

namespace fk {
namespace {

std::string trim(const std::string& text) {
    size_t b = 0, e = text.size();
    while (b < e && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1]))) --e;
    return text.substr(b, e - b);
}

/// Fio's documentation shows typographic quotes around quoted fields. That is
/// the PDF's rendering of an ASCII quote, but a file really containing them
/// would otherwise keep them as part of the value, so they come off here.
std::string stripStrayQuotes(const std::string& text) {
    static const std::vector<std::string> marks = {"\"", "\xE2\x80\x9C", "\xE2\x80\x9D",
                                                   "\xE2\x80\x9E"};
    std::string out = trim(text);
    bool changed = true;
    while (changed && !out.empty()) {
        changed = false;
        for (const std::string& m : marks) {
            if (out.size() > m.size() && out.compare(0, m.size(), m) == 0) {
                out.erase(0, m.size());
                changed = true;
                break;
            }
        }
        for (const std::string& m : marks) {
            if (out.size() >= m.size() &&
                out.compare(out.size() - m.size(), m.size(), m) == 0) {
                out.erase(out.size() - m.size());
                changed = true;
                break;
            }
        }
    }
    return trim(out);
}

// Which of our fields a column holds. Unknown columns are simply ignored.
enum class Col {
    Unknown, Id, OrderId, Date, Amount, Currency, CounterAccount, CounterName,
    CounterBank, Ks, Vs, Ss, UserIdent, Message, Type, Comment
};

/// Matched on the folded name so the Czech and Slovak exports both land.
Col columnFor(const std::string& folded) {
    if (folded == "id pohybu")                       return Col::Id;
    if (folded == "id pokynu")                       return Col::OrderId;
    if (folded == "datum")                           return Col::Date;
    if (folded == "objem" || folded == "castka" || folded == "suma")
        return Col::Amount;
    if (folded == "mena")                            return Col::Currency;
    if (folded == "protiucet")                       return Col::CounterAccount;
    if (folded == "kod banky")                       return Col::CounterBank;
    if (folded == "ks")                              return Col::Ks;
    if (folded == "vs")                              return Col::Vs;
    if (folded == "ss")                              return Col::Ss;
    if (folded == "typ")                             return Col::Type;
    if (folded.find("komentar") != std::string::npos) return Col::Comment;

    // "Název protiúčtu" / "Názov protiúčtu" — but not "Název banky".
    if (folded.find("protiuct") != std::string::npos) return Col::CounterName;
    if (folded.find("banky") != std::string::npos)    return Col::Unknown;

    // "Zpráva pro příjemce" / "Správa pre príjemcu".
    if (folded.find("prijemc") != std::string::npos)  return Col::Message;
    if (folded.find("identifik") != std::string::npos) return Col::UserIdent;
    return Col::Unknown;
}

/// The documented column order, used when a file arrives without its header
/// row. Same list as the specification, in the same sequence.
const std::vector<Col>& positionalColumns() {
    static const std::vector<Col> order = {
        Col::Id, Col::Date, Col::Amount, Col::Currency, Col::CounterAccount,
        Col::CounterName, Col::CounterBank, Col::Unknown /*Název banky*/,
        Col::Ks, Col::Vs, Col::Ss, Col::UserIdent, Col::Message, Col::Type,
        Col::Unknown /*Provedl*/, Col::Unknown /*Upřesnění*/, Col::Comment,
        Col::Unknown /*BIC*/, Col::OrderId};
    return order;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (char c : text) {
        if (c == '\n') { out.push_back(current); current.clear(); }
        else if (c != '\r') current += c;
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

bool digitsOnly(const std::string& text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

} // namespace

// ------------------------------------------------------------------ encoding

bool isValidUtf8(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        const auto b = static_cast<unsigned char>(text[i]);
        int extra = 0;
        if (b < 0x80)                       extra = 0;
        else if ((b & 0xE0) == 0xC0)        extra = 1;
        else if ((b & 0xF0) == 0xE0)        extra = 2;
        else if ((b & 0xF8) == 0xF0)        extra = 3;
        else return false;
        if (i + static_cast<size_t>(extra) >= text.size() && extra > 0) return false;
        for (int k = 1; k <= extra; ++k)
            if ((static_cast<unsigned char>(text[i + static_cast<size_t>(k)]) & 0xC0) != 0x80)
                return false;
        i += static_cast<size_t>(extra) + 1;
    }
    return true;
}

std::string cp1250ToUtf8(const std::string& text) {
    // Only the high half differs; 0x00-0x7F is ASCII. Unicode code points for
    // Windows-1250, in order from 0x80.
    static const uint16_t high[128] = {
        0x20AC, 0x0081, 0x201A, 0x0083, 0x201E, 0x2026, 0x2020, 0x2021,
        0x0088, 0x2030, 0x0160, 0x2039, 0x015A, 0x0164, 0x017D, 0x0179,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x0098, 0x2122, 0x0161, 0x203A, 0x015B, 0x0165, 0x017E, 0x017A,
        0x00A0, 0x02C7, 0x02D8, 0x0141, 0x00A4, 0x0104, 0x00A6, 0x00A7,
        0x00A8, 0x00A9, 0x015E, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x017B,
        0x00B0, 0x00B1, 0x02DB, 0x0142, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
        0x00B8, 0x0105, 0x015F, 0x00BB, 0x013D, 0x02DD, 0x013E, 0x017C,
        0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7,
        0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
        0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7,
        0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
        0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7,
        0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F,
        0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7,
        0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9};

    std::string out;
    out.reserve(text.size() + text.size() / 4);
    for (char raw : text) {
        const auto b = static_cast<unsigned char>(raw);
        if (b < 0x80) { out += static_cast<char>(b); continue; }
        const uint16_t cp = high[b - 0x80];
        if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// --------------------------------------------------------------------- pieces

std::vector<std::string> splitCsvLine(const std::string& line) {
    std::vector<std::string> out;
    std::string field;
    bool quoted = false;

    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { field += '"'; ++i; }
                else quoted = false;
            } else field += c;
        } else if (c == '"' && field.empty()) {
            quoted = true;
        } else if (c == ';') {
            out.push_back(field);
            field.clear();
        } else {
            field += c;
        }
    }
    out.push_back(field);
    return out;
}

std::string dateFromFio(const std::string& text) {
    const std::string t = trim(text);
    if (t.size() != 10 || t[2] != '.' || t[5] != '.') return {};
    const std::string d = t.substr(0, 2), m = t.substr(3, 2), y = t.substr(6, 4);
    if (!digitsOnly(d) || !digitsOnly(m) || !digitsOnly(y)) return {};
    const int day = std::stoi(d), month = std::stoi(m);
    if (day < 1 || day > 31 || month < 1 || month > 12) return {};
    return y + "-" + m + "-" + d;
}

std::string foldHeader(const std::string& text) {
    // The same folding as deburr() in the payment layer, kept separate because
    // that one exists to satisfy a banking standard and this one to compare
    // two spellings of a column name. They should be free to diverge.
    static const std::map<std::string, char> map = {
        {"á", 'a'}, {"ä", 'a'}, {"č", 'c'}, {"ď", 'd'}, {"é", 'e'}, {"ě", 'e'},
        {"í", 'i'}, {"ĺ", 'l'}, {"ľ", 'l'}, {"ň", 'n'}, {"ó", 'o'}, {"ô", 'o'},
        {"ö", 'o'}, {"ŕ", 'r'}, {"ř", 'r'}, {"š", 's'}, {"ť", 't'}, {"ú", 'u'},
        {"ů", 'u'}, {"ü", 'u'}, {"ý", 'y'}, {"ž", 'z'},
        {"Á", 'a'}, {"Ä", 'a'}, {"Č", 'c'}, {"Ď", 'd'}, {"É", 'e'}, {"Ě", 'e'},
        {"Í", 'i'}, {"Ĺ", 'l'}, {"Ľ", 'l'}, {"Ň", 'n'}, {"Ó", 'o'}, {"Ô", 'o'},
        {"Ö", 'o'}, {"Ŕ", 'r'}, {"Ř", 'r'}, {"Š", 's'}, {"Ť", 't'}, {"Ú", 'u'},
        {"Ů", 'u'}, {"Ü", 'u'}, {"Ý", 'y'}, {"Ž", 'z'}};

    std::string out;
    for (size_t i = 0; i < text.size();) {
        const auto b = static_cast<unsigned char>(text[i]);
        if (b < 0x80) {
            const char c = static_cast<char>(std::tolower(b));
            // Collapse runs of whitespace, so a name wrapped across two lines
            // in someone's editor still matches.
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!out.empty() && out.back() != ' ') out += ' ';
            } else out += c;
            ++i;
            continue;
        }
        const size_t len = (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
        const std::string ch = text.substr(i, len);
        const auto it = map.find(ch);
        if (it != map.end()) out += it->second;
        i += len;
    }
    return trim(out);
}

std::string BankTransaction::dedupKey() const {
    if (!bankId.empty()) return bankId;
    if (!orderId.empty()) return "pokyn:" + orderId;

    const std::string identity = date + "|" + amount.toString(2) + "|" + currency + "|" +
                                 counterAccount + "|" + counterBankCode + "|" +
                                 variableSymbol + "|" + specificSymbol + "|" + message;
    return "sha:" + Sha256::hexOf(identity).substr(0, 32);
}

std::string BankTransaction::freeText() const {
    std::string out = message;
    for (const std::string* part : {&userIdentification, &comment, &counterName}) {
        if (part->empty()) continue;
        if (!out.empty()) out += " ";
        out += *part;
    }
    return out;
}

// ---------------------------------------------------------------------- parse

Statement parseFioCsv(const std::string& text) {
    Statement out;

    std::string body = text;
    if (body.compare(0, 3, "\xEF\xBB\xBF") == 0) body.erase(0, 3);
    if (!isValidUtf8(body)) body = cp1250ToUtf8(body);

    const std::vector<std::string> lines = splitLines(body);
    if (lines.empty()) {
        out.error = "Súbor je prázdny.";
        return out;
    }

    // --------------------------------------------------------- header block
    // key;value pairs until the column header row. Anything unrecognised is
    // skipped rather than treated as an error: Fio has added fields before.
    size_t i = 0;
    std::vector<Col> layout;
    for (; i < lines.size(); ++i) {
        const std::string line = trim(lines[i]);
        if (line.empty()) continue;

        const std::vector<std::string> cells = splitCsvLine(line);
        const std::string first = foldHeader(cells.front());

        if (first == "id pohybu") {                    // the column header row
            for (const std::string& cell : cells) layout.push_back(columnFor(foldHeader(cell)));
            ++i;
            break;
        }
        if (cells.size() < 2) continue;

        const std::string key   = trim(cells[0]);
        const std::string value = stripStrayQuotes(cells[1]);
        if      (key == "accountId")  out.accountId = value;
        else if (key == "bankId")     out.bankCode  = value;
        else if (key == "currency")   out.currency  = value;
        else if (key == "iban")       out.iban      = value;
        else if (key == "bic")        out.bic       = value;
        else if (key == "dateStart")  out.dateStart = dateFromFio(value);
        else if (key == "dateEnd")    out.dateEnd   = dateFromFio(value);
    }

    if (layout.empty()) {
        // No header row. Fall back to the documented order, but only from the
        // first line that actually looks like a movement, so a header block
        // without a column row does not become a transaction.
        layout = positionalColumns();
        i = 0;
        for (; i < lines.size(); ++i) {
            const std::vector<std::string> cells = splitCsvLine(trim(lines[i]));
            if (cells.size() >= 4 && digitsOnly(trim(cells[0])) &&
                !dateFromFio(cells[1]).empty())
                break;
        }
        if (i >= lines.size()) {
            out.error = "V súbore sa nenašli žiadne pohyby. Je to výpis z Fio banky vo formáte CSV?";
            return out;
        }
    }

    // ----------------------------------------------------------- the rows
    for (; i < lines.size(); ++i) {
        const std::string line = trim(lines[i]);
        if (line.empty()) continue;

        const std::vector<std::string> cells = splitCsvLine(lines[i]);

        BankTransaction t;
        bool sawAmount = false;
        for (size_t c = 0; c < cells.size() && c < layout.size(); ++c) {
            const std::string value = stripStrayQuotes(cells[c]);
            if (value.empty()) continue;
            switch (layout[c]) {
                case Col::Id:             t.bankId = value; break;
                case Col::OrderId:        t.orderId = value; break;
                case Col::Date:           t.date = dateFromFio(value); break;
                case Col::Currency:       t.currency = value; break;
                case Col::CounterAccount: t.counterAccount = value; break;
                case Col::CounterName:    t.counterName = value; break;
                case Col::CounterBank:    t.counterBankCode = value; break;
                case Col::Ks:             t.constantSymbol = value; break;
                case Col::Vs:             t.variableSymbol = value; break;
                case Col::Ss:             t.specificSymbol = value; break;
                case Col::UserIdent:      t.userIdentification = value; break;
                case Col::Message:        t.message = value; break;
                case Col::Type:           t.type = value; break;
                case Col::Comment:        t.comment = value; break;
                case Col::Amount: {
                    const std::optional<Dec> parsed = Dec::parse(value);
                    if (parsed) { t.amount = *parsed; sawAmount = true; }
                    break;
                }
                case Col::Unknown: break;
            }
        }

        // A movement needs a date and an amount to be worth anything. A row
        // without them is a footer, a blank or a summary line.
        if (t.date.empty() || !sawAmount) {
            if (cells.size() > 3) out.warnings.push_back(trim(lines[i]));
            continue;
        }
        if (t.currency.empty()) t.currency = out.currency;
        out.transactions.push_back(std::move(t));
    }

    if (out.transactions.empty() && out.error.empty())
        out.error = "Výpis neobsahuje žiadne pohyby.";
    out.ok = !out.transactions.empty();
    return out;
}

} // namespace fk
