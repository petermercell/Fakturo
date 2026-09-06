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

#include "Matcher.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace fk {
namespace {

/// Leading zeros are not significant in a variable symbol: a customer typing
/// 0020260001 has paid invoice 20260001.
std::string normaliseSymbol(const std::string& text) {
    std::string digits;
    for (char c : text)
        if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
    size_t start = 0;
    while (start + 1 < digits.size() && digits[start] == '0') ++start;
    return digits.substr(start);
}

/// Two account numbers, compared as the digits that identify them. A statement
/// writes "SK31 1200 0000 1987 4263 7541", an invoice writes it without the
/// spaces, and Fio writes a domestic number with a prefix and a bank code.
/// Comparing the digits gets all three to agree; comparing the strings gets
/// none of them to.
bool sameAccount(const std::string& a, const std::string& b) {
    auto digits = [](const std::string& text) {
        std::string out;
        for (char c : text)
            if (std::isdigit(static_cast<unsigned char>(c))) out += c;
        // Leading zeros inside an account number are not significant either.
        size_t start = 0;
        while (start + 1 < out.size() && out[start] == '0') ++start;
        return out.substr(start);
    };
    const std::string left = digits(a), right = digits(b);
    if (left.empty() || right.empty()) return false;
    if (left == right) return true;
    if (left.size() < 6 || right.size() < 6) return false;

    // One may be an IBAN and the other the domestic number inside it. In a
    // Slovak or Czech BBAN the base number is the *end* of the string, so the
    // shorter is compared against the tail of the longer rather than searched
    // for anywhere in it. An unanchored search matched "123456" inside an
    // unrelated IBAN, and — worse, because it is systematic rather than a
    // coincidence — matched an account against the same account with a
    // different prefix, which is a different account.
    const std::string& shorter = left.size() < right.size() ? left : right;
    const std::string& longer  = left.size() < right.size() ? right : left;

    // And the longer has to be long enough to actually *be* an IBAN wrapped
    // around the shorter — country, check digits, bank code and prefix come to
    // a dozen digits. Without this, "2000145399" and "19-2000145399" match on
    // the suffix, and those are two different accounts: the prefix is part of
    // the identity, not decoration.
    if (longer.size() < shorter.size() + 8) return false;
    return longer.compare(longer.size() - shorter.size(), shorter.size(), shorter) == 0;
}

} // namespace

bool mentionsNumber(const std::string& text, const std::string& number) {
    const std::string needle = normaliseSymbol(number);
    if (needle.empty()) return false;

    // Walk every run of digits in the text and compare it whole. Substring
    // search would find 202601 inside 2026010, which is a different invoice.
    for (size_t i = 0; i < text.size();) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) { ++i; continue; }
        size_t j = i;
        while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) ++j;
        if (normaliseSymbol(text.substr(i, j - i)) == needle) return true;
        i = j;
    }
    return false;
}

std::vector<TransactionMatch> matchTransactions(
    const Statement& statement,
    const std::vector<PayableInvoice>& invoices,
    const std::vector<std::string>& alreadyImported,
    MatchDirection direction) {

    const bool outgoing = direction == MatchDirection::Outgoing;
    const std::set<std::string> seen(alreadyImported.begin(), alreadyImported.end());
    std::vector<TransactionMatch> out;

    for (size_t i = 0; i < statement.transactions.size(); ++i) {
        const BankTransaction& t = statement.transactions[i];

        if (t.isCredit() == outgoing)                 continue;   // the other direction
        if (t.amount.isZero())                        continue;   // neither one
        if (seen.count(t.dedupKey()))                 continue;   // imported before

        // A debit is stored negative. Everything below compares magnitudes, so
        // the sign is taken off once, here, rather than at six comparisons.
        const Dec paid = t.amount.abs();

        TransactionMatch m;
        m.transaction = i;

        // ------------------------------------------------- by variable symbol
        const std::string vs = normaliseSymbol(t.variableSymbol);
        std::vector<const PayableInvoice*> bySymbol;
        if (!vs.empty())
            for (const PayableInvoice& inv : invoices) {
                if (inv.cancelled) continue;
                if (normaliseSymbol(inv.variableSymbol) == vs ||
                    normaliseSymbol(inv.number) == vs)
                    bySymbol.push_back(&inv);
            }

        // ----------------------------------------------------- by the message
        // Only when the symbol found nothing. A customer who fills in the
        // symbol has told us more than one who mentions a number in passing.
        std::vector<const PayableInvoice*> byText;
        if (bySymbol.empty()) {
            const std::string text = t.freeText();
            for (const PayableInvoice& inv : invoices) {
                if (inv.cancelled) continue;
                if (mentionsNumber(text, inv.number)) byText.push_back(&inv);
            }
        }

        // --------------------------------------------------- by the account
        // Outgoing only, and only as a last resort. Paying a supplier, the
        // account you sent the money to is something the invoice told you in
        // advance — which is never true of a customer paying you. Narrowed by
        // amount as well, because one supplier sends more than one invoice.
        std::vector<const PayableInvoice*> byAccount;
        if (outgoing && bySymbol.empty() && byText.empty() && !t.counterAccount.empty()) {
            for (const PayableInvoice& inv : invoices) {
                if (inv.cancelled || inv.counterAccount.empty()) continue;
                if (sameAccount(t.counterAccount, inv.counterAccount) &&
                    inv.outstanding() == paid)
                    byAccount.push_back(&inv);
            }
        }

        const std::vector<const PayableInvoice*>& found =
            !bySymbol.empty() ? bySymbol : (!byText.empty() ? byText : byAccount);
        const bool viaSymbol  = !bySymbol.empty();
        const bool viaAccount = bySymbol.empty() && byText.empty() && !byAccount.empty();

        if (found.empty()) {
            m.reason = vs.empty() ? "Bez variabilného symbolu a bez zhody v texte."
                                  : "Žiadna faktúra s variabilným symbolom " + vs + ".";
            out.push_back(std::move(m));
            continue;
        }

        if (found.size() > 1) {
            // Two invoices answering to one symbol is a numbering mistake
            // worth seeing, not something to resolve by picking the first.
            m.reason = "Zodpovedá viacerým faktúram (" + found.front()->number + ", " +
                       found.at(1)->number + "…). Vyberte ručne.";
            out.push_back(std::move(m));
            continue;
        }

        const PayableInvoice& inv = *found.front();
        m.invoiceId     = inv.id;
        m.invoiceNumber = inv.number;

        const Dec outstanding = inv.outstanding();
        const bool sameCurrency = t.currency.empty() || inv.currency.empty() ||
                                  t.currency == inv.currency;

        if (!sameCurrency) {
            m.quality = MatchQuality::Uncertain;
            m.reason  = "Platba je v " + t.currency + ", faktúra v " + inv.currency + ".";
        } else if (outstanding.isZero() && !inv.payable.isZero()) {
            m.quality = MatchQuality::Uncertain;
            m.reason  = "Faktúra je už uhradená.";
        } else if (paid == outstanding) {
            m.quality = viaSymbol ? MatchQuality::Confident : MatchQuality::Uncertain;
            m.reason  = viaSymbol  ? "Variabilný symbol a suma sedia."
                      : viaAccount ? "Účet dodávateľa a suma sedia, variabilný symbol chýba."
                                   : "Číslo faktúry v texte platby, suma sedí.";
        } else if (paid < outstanding) {
            m.quality = MatchQuality::Uncertain;
            m.reason  = "Čiastočná úhrada, zostáva " +
                        (outstanding - paid).toString(2) + " " + inv.currency + ".";
        } else {
            m.quality = MatchQuality::Uncertain;
            m.reason  = "Preplatok o " + (paid - outstanding).toString(2) + " " +
                        inv.currency + ".";
        }

        out.push_back(std::move(m));
    }

    return out;
}

} // namespace fk
