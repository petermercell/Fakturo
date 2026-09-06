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

#include "InvoiceActions.h"

#include "../pdf/PdfRenderer.h"
#include "../sk/Slovak.h"
#include "../ubl/UblWriter.h"
#include "../ubl/Validator.h"

#include <QByteArray>
#include <QString>

namespace fk {

Company sellerForRender(Database& db, const Invoice& inv) {
    Company seller = inv.seller;
    if (inv.companyId == 0) return seller;

    Company current;
    if (!db.loadCompany(inv.companyId, current)) return seller;

    // Only the logo. Everything else on the page comes from the snapshot, so
    // that a document still prints the address and the account it was written
    // with rather than the ones the company has today.
    seller.logo     = current.logo;
    seller.logoPath = current.logoPath;
    return seller;
}

IssuePlan planIssue(Database& db, const Invoice& inv) {
    IssuePlan plan;
    plan.kind = inv.type == DocType::Proforma ? Database::SeriesKind::Proforma
                                              : Database::SeriesKind::Invoice;

    if (inv.state != InvoiceState::Draft) {
        plan.refusal = inv.state == InvoiceState::Cancelled
                           ? "Doklad je stornovaný."
                           : "Doklad už je vystavený " + inv.issuedAt + ".";
        return plan;
    }

    // The number is settled before the document is validated, so that what
    // gets validated is the document as it would actually go out — with its
    // number and the variable symbol derived from it. Validating first would
    // reject every document that has not been numbered yet, which is exactly
    // the case this is here to handle.
    //
    // peekNextNumber, not series.format(series.next): it skips numbers already
    // taken. Recomputing the plain next number instead turns a number that is
    // merely in use into a refusal the user cannot get past.
    const NumberSeries series = db.series(plan.kind);
    const std::string  next   = db.peekNextNumber(plan.kind);
    plan.number = inv.number.empty() ? next : inv.number;

    if (plan.number.empty()) {
        plan.refusal = "Doklad nemá číslo.";
        return plan;
    }
    if (db.invoiceNumberExists(plan.number, inv.id)) {
        plan.refusal = "Doklad s číslom " + plan.number + " už existuje.";
        return plan;
    }

    Invoice asIssued = inv;
    asIssued.number  = plan.number;
    if (asIssued.variableSymbol.empty())
        asIssued.variableSymbol = sk::variableSymbolFrom(plan.number);

    const ValidationResult result = validate(asIssued, asIssued.seller);
    if (!result.ok()) {
        for (const Issue& i : result.issues)
            if (i.severity == Severity::Error) plan.errors.push_back(i.message);
        plan.refusal = "Doklad nie je kompletný.";
        return plan;
    }

    plan.variableSymbol = asIssued.variableSymbol;
    // Only the plain next number moves the series on. A number reached by
    // skipping past one already in use leaves the counter where it was, so the
    // gap closes by itself when that document is deleted.
    plan.advancesSeries = plan.number == series.format(series.next);
    plan.ok = true;
    return plan;
}

IssueOutcome issueDocument(Database& db, const Invoice& inv, const IssuePlan& plan) {
    IssueOutcome out;
    if (!plan.ok) {
        out.error = plan.refusal.empty() ? "Doklad nemožno vystaviť." : plan.refusal;
        return out;
    }
    out.number = plan.number;

    Invoice issued = inv;
    issued.number  = plan.number;
    issued.state   = InvoiceState::Issued;
    // A draft issued without a number of its own would otherwise carry an
    // empty variable symbol into the archived PDF and its payment QR.
    if (issued.variableSymbol.empty() && !plan.variableSymbol.empty())
        issued.variableSymbol = plan.variableSymbol;

    // …and it has to reach the database too, not just the rendering. It did
    // not: issueInvoiceWithDocument writes only state, issued_at and number,
    // so a symbol derived here stayed on this local copy. The printed invoice
    // asked the customer to pay under a symbol the bank-statement matcher
    // would then never find. Persisted while the document is still a draft,
    // which is the only time it can be written.
    if (inv.variableSymbol != issued.variableSymbol) {
        Invoice draft = inv;
        draft.variableSymbol = issued.variableSymbol;
        draft.number         = plan.number;
        if (!db.saveInvoice(draft)) {
            out.error = "Variabilný symbol sa nepodarilo uložiť: " + db.lastError();
            return out;
        }
    }

    // Render before committing. The archived bytes *are* the document; if they
    // cannot be produced there is nothing to issue.
    QString renderError;
    const QByteArray pdf = renderInvoicePdf(issued, sellerForRender(db, issued), &renderError);
    if (pdf.isEmpty()) {
        out.error = "Doklad sa nepodarilo vykresliť, preto nebol vystavený. " +
                    renderError.toStdString();
        return out;
    }
    const std::string ubl =
        canExportToPeppol(issued.type) ? writeUbl(issued, issued.seller) : std::string();

    // Issue and archive together: a document issued with no archived form is
    // precisely what the archive exists to prevent.
    if (!db.issueInvoiceWithDocument(
            inv.id, plan.number,
            std::string(pdf.constData(), static_cast<size_t>(pdf.size())), ubl)) {
        out.error = db.lastError();
        if (out.error.empty()) out.error = "Doklad nebol vystavený.";
        return out;
    }

    // Only now, when the number is really in use.
    if (plan.advancesSeries) {
        NumberSeries series = db.series(plan.kind);
        if (plan.number == series.format(series.next)) {
            series.next += 1;
            db.setSeries(series, plan.kind);
        }
    }

    out.ok = true;
    return out;
}

} // namespace fk
