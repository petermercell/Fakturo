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

#include "../core/Xml.h"

#include <algorithm>
#include <cctype>

namespace fk {
namespace {

/// The Czech and Slovak banking associations carry the three domestic symbols
/// inside ISO 20022's reference fields, prefixed with their own names:
///
///     <EndToEndId>VS20260001</EndToEndId>
///     <InstrId>KS0308</InstrId>
///     <PmtInfId>SS19</PmtInfId>
///
/// The prefix is how a reader tells a variable symbol from an end-to-end
/// reference that happens to be numeric — the standard puts a real E2E
/// reference in the same element when the payer supplied one. So a value is
/// only a symbol if it is labelled as one.
std::string symbolFrom(const std::string& value, const char* prefix) {
    if (value.size() <= 2) return {};
    if (std::toupper(static_cast<unsigned char>(value[0])) != prefix[0]) return {};
    if (std::toupper(static_cast<unsigned char>(value[1])) != prefix[1]) return {};

    const std::string rest = value.substr(2);
    const bool numeric = std::all_of(rest.begin(), rest.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
    });
    return numeric ? rest : std::string();
}

/// ISO dates arrive as "2011-03-31" or "2011-03-31T17:30:47.0+01:00".
std::string dateOnly(const std::string& value) {
    if (value.size() < 10) return {};
    const std::string date = value.substr(0, 10);
    if (date[4] != '-' || date[7] != '-') return {};
    return date;
}

/// The date of an entry: booking date, falling back to value date. Both are
/// wrapped in a choice of <Dt> or <DtTm>.
std::string entryDate(const XmlNode& entry) {
    for (const char* which : {"BookgDt", "ValDt"}) {
        const XmlNode* node = entry.find(which);
        if (!node) continue;
        for (const char* form : {"Dt", "DtTm"}) {
            const std::string value = node->textAt(form);
            if (!value.empty()) return dateOnly(value);
        }
    }
    return {};
}

/// An account's number: IBAN if present, otherwise the domestic identifier.
std::string accountNumber(const XmlNode* account) {
    if (!account) return {};
    const std::string iban = account->textAt("Id/IBAN");
    if (!iban.empty()) return iban;
    return account->textAt("Id/Othr/Id");
}

} // namespace

Statement parseCamt053(const std::string& text) {
    Statement out;

    const XmlDocument doc = parseXml(text);
    if (!doc.ok) {
        out.error = "Súbor sa nepodarilo prečítať ako XML. " + doc.error;
        return out;
    }

    // BkToCstmrStmt/Stmt, but banks wrap it in Document or nothing at all, so
    // the statement is found rather than navigated to.
    const XmlNode* stmt =
        doc.root.name == "Stmt" ? &doc.root : doc.root.findDeep("Stmt");
    if (!stmt) {
        out.error = "V súbore nie je výpis (Stmt). Je to camt.053?";
        return out;
    }

    // ---------------------------------------------------------- the account
    if (const XmlNode* account = stmt->find("Acct")) {
        out.iban      = account->textAt("Id/IBAN");
        out.accountId = account->textAt("Id/Othr/Id");
        out.currency  = account->textAt("Ccy");
        out.bic       = account->textAt("Svcr/FinInstnId/BIC");
        out.bankCode  = account->textAt("Svcr/FinInstnId/Othr/Id");
        if (out.accountId.empty() && !out.iban.empty() && out.iban.size() > 8)
            out.accountId = out.iban.substr(8);
    }
    if (const XmlNode* period = stmt->find("FrToDt")) {
        out.dateStart = dateOnly(period->textAt("FrDtTm"));
        out.dateEnd   = dateOnly(period->textAt("ToDtTm"));
    }

    // ----------------------------------------------------------- the entries
    for (const XmlNode* entry : stmt->childrenNamed("Ntry")) {
        // INFO entries are not booked. Treating one as a payment would credit
        // an invoice for money that has not arrived.
        const std::string status = entry->textAt("Sts");
        if (!status.empty() && status != "BOOK") continue;

        BankTransaction t;
        t.date = entryDate(*entry);

        const XmlNode* amount = entry->find("Amt");
        if (!amount) continue;
        const std::optional<Dec> parsed = Dec::parse(amount->text);
        if (!parsed) { out.warnings.push_back("Neplatná suma: " + amount->text); continue; }
        t.amount   = *parsed;
        t.currency = amount->attribute("Ccy");
        if (t.currency.empty()) t.currency = out.currency;

        // ISO keeps the amount unsigned and states the direction separately.
        // A statement read without this pays invoices from money going out.
        if (entry->textAt("CdtDbtInd") == "DBIT") t.amount = -t.amount;

        t.bankId = entry->textAt("NtryRef");
        t.type   = entry->textAt("BkTxCd/Prtry/Cd");
        if (entry->textAt("RvslInd") == "true")
            t.type = t.type.empty() ? std::string("storno") : t.type + " (storno)";

        // A batched entry has several TxDtls. Only the first is read; the rest
        // are reported rather than silently dropped, because a lump sum
        // covering four invoices is not something to guess at.
        const XmlNode* details = entry->find("NtryDtls");
        std::vector<const XmlNode*> transactions =
            details ? details->childrenNamed("TxDtls") : std::vector<const XmlNode*>{};
        if (transactions.size() > 1)
            out.warnings.push_back("Hromadná položka s " + std::to_string(transactions.size()) +
                                   " platbami, načítaná len prvá: " + t.bankId);

        if (!transactions.empty()) {
            const XmlNode& tx = *transactions.front();

            if (const XmlNode* refs = tx.find("Refs")) {
                if (t.bankId.empty()) t.bankId = refs->textAt("AcctSvcrRef");
                t.orderId = refs->textAt("MsgId");

                t.variableSymbol = symbolFrom(refs->textAt("EndToEndId"), "VS");
                t.constantSymbol = symbolFrom(refs->textAt("InstrId"),    "KS");
                t.specificSymbol = symbolFrom(refs->textAt("PmtInfId"),   "SS");
            }

            // For money coming in the counterparty is the debtor; for money
            // going out, the creditor. Getting this backwards would show the
            // customer their own name.
            const bool incoming = t.amount.raw() >= 0;
            if (const XmlNode* parties = tx.find("RltdPties")) {
                const XmlNode* who = parties->find(incoming ? "Dbtr" : "Cdtr");
                const XmlNode* acct = parties->find(incoming ? "DbtrAcct" : "CdtrAcct");
                if (who)  t.counterName = who->textAt("Nm");
                t.counterAccount = accountNumber(acct);
                if (t.counterName.empty() && acct) t.counterName = acct->textAt("Nm");
            }
            if (const XmlNode* agents = tx.find("RltdAgts")) {
                if (const XmlNode* agent = agents->find(incoming ? "DbtrAgt" : "CdtrAgt"))
                    t.counterBankCode = agent->textAt("FinInstnId/Othr/Id");
            }

            if (const XmlNode* remittance = tx.find("RmtInf")) {
                for (const XmlNode* line : remittance->childrenNamed("Ustrd")) {
                    if (line->text.empty()) continue;
                    if (!t.message.empty()) t.message += " ";
                    t.message += line->text;
                }
                // Structured remittance carries the invoice number in
                // CdtrRefInf/Ref. Worth having: a payer using it has named the
                // document exactly.
                const std::string reference = remittance->deepText("Ref");
                if (!reference.empty()) {
                    if (!t.message.empty()) t.message += " ";
                    t.message += reference;
                }
            }
            t.comment = tx.textAt("AddtlTxInf");
        }

        if (t.message.empty()) t.message = entry->textAt("AddtlNtryInf");

        if (t.date.empty()) {
            out.warnings.push_back("Položka bez dátumu: " + t.bankId);
            continue;
        }
        out.transactions.push_back(std::move(t));
    }

    if (out.transactions.empty() && out.error.empty())
        out.error = "Výpis neobsahuje žiadne zaúčtované položky.";
    out.ok = !out.transactions.empty();
    return out;
}

// ------------------------------------------------------------------ routing

StatementFormat detectStatementFormat(const std::string& text) {
    // Look past a byte order mark and any leading whitespace, then decide on
    // what the file actually starts with rather than on its extension — a
    // .csv that is really XML is a thing people do.
    size_t i = 0;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) i = 3;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;

    if (i < text.size() && text[i] == '<') return StatementFormat::Camt053;
    if (text.find("ID pohybu") != std::string::npos ||
        text.find("accountId;") != std::string::npos)
        return StatementFormat::FioCsv;
    return StatementFormat::Unknown;
}

Statement parseStatement(const std::string& text) {
    switch (detectStatementFormat(text)) {
        case StatementFormat::Camt053: return parseCamt053(text);
        case StatementFormat::FioCsv:  return parseFioCsv(text);
        default: break;
    }

    // Neither header matched. Rather than refuse, try both and take whichever
    // finds movements: a trimmed CSV without its header row is still readable.
    Statement csv = parseFioCsv(text);
    if (csv.ok) return csv;

    Statement statement;
    statement.error = "Nerozpoznaný formát. Podporované sú CSV z Fio banky "
                      "a XML výpis camt.053.";
    return statement;
}

} // namespace fk
