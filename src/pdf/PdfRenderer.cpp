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

#include "PdfRenderer.h"

#include "../core/QrCode.h"
#include "../country/Country.h"
#include "../pay/PaymentQr.h"
#include "../cz/Czech.h"
#include "../sk/Slovak.h"

#include <QBuffer>

#include <algorithm>
#include <QImage>
#include <QPainter>
#include <QStringList>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSizeF>
#include <QMarginsF>
#include <QPageLayout>
#include <QPageSize>
#include <QPdfWriter>
#include <QTextDocument>
#include <QUrl>
#include <QVariant>

namespace fk {
namespace {

/// Slovak number formatting: comma as the decimal separator.
QString num(Dec v, int dp = 2) {
    return QString::fromStdString(v.toString(dp)).replace('.', ',');
}

QString esc(const std::string& s) { return QString::fromStdString(s).toHtmlEscaped(); }

/// Every currency code on the page goes through esc(), because a currency code
/// is not necessarily "EUR". This template renders documents *received* from
/// strangers as well as ones written here, and `DocumentCurrencyCode` is
/// whatever the sender put in it — the reader only folds its case. An
/// unescaped field is a field somebody else can put markup in, and the markup
/// worth putting there is a second payment block.
///
/// Escaped for the same reason, and it is the likelier of the two: an invalid
/// date is returned unchanged by formatDateSk, and the reader takes the three
/// date fields verbatim.
QString date(const std::string& iso) {
    return esc(sk::formatDateSk(iso));
}

/// Postal codes are grouped differently: "960 01" in Slovakia, "140 00" in
/// Czechia, both from five digits.
///
/// Escaped like everything else that came from a stranger. formatPsc groups
/// five digits and hands anything else back unchanged, so on a received
/// document this is the sender's string, not a number.
QString postalCode(const Party& p) {
    return esc(cz::formatPsc(p.address.postalCode));
}

/// Thousands grouped with a non-breaking space: "325 270,10". The old layout
/// printed "325270,10", which is readable on a line total and not on the one
/// figure the customer is actually going to pay. Non-breaking, because a
/// number broken across a line is worse than a number that overflows.
QString grouped(Dec v, int dp = 2) {
    QString text = num(v, dp);
    bool negative = text.startsWith('-');
    if (negative) text.remove(0, 1);

    int cut = text.indexOf(',');
    if (cut < 0) cut = text.size();
    for (int at = cut - 3; at > 0; at -= 3)
        text.insert(at, QChar(0x00A0));
    return (negative ? QString("-") : QString()) + text;
}

/// One label/value line: the label grey on the left, the value hard against
/// the right edge of its column. The whole layout is built from these — it is
/// what makes the supplier's block and the customer's block line up without a
/// grid drawn round them. Returns a row, so a caller can put several in one
/// table and have their two columns agree.
QString kv(const QString& label, const QString& value) {
    if (value.isEmpty()) return {};
    return "<tr><td class=\"k\">" + label + "</td><td class=\"v\">" + value + "</td></tr>";
}

/// A statement rather than a pair — "Neplatca DPH" has no value to put on the
/// right, and printing it as a label with an empty cell beside it would read
/// like something had gone missing.
QString kvNote(const QString& text) {
    return "<tr><td class=\"k\" colspan=\"2\">" + text + "</td></tr>";
}

QString kvTable(const QString& rows) {
    if (rows.isEmpty()) return {};
    return "<table class=\"kv full\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\">" +
           rows + "</table>";
}

/// The supplier's or the customer's half of the header.
///
/// `showCountry` is false on a domestic invoice: printing "SK" under a Slovak
/// address on a document going to Bratislava is noise, and this layout is
/// mostly whitespace, so noise costs more than it used to.
QString partyColumn(const QString& title, const Party& p, const InvoiceLabels& L,
                    bool showCountry) {
    QString h;
    h += "<p class=\"label\">" + title + "</p>";
    h += "<p class=\"name\"><b>" + esc(p.name) + "</b></p>";

    QString address;
    if (!p.address.street.empty())  address += esc(p.address.street) + "<br/>";
    if (!p.address.street2.empty()) address += esc(p.address.street2) + "<br/>";
    address += postalCode(p) + " " + esc(p.address.city);
    if (showCountry && !p.address.countryCode.empty())
        address += "<br/>" + esc(p.address.countryCode);
    h += "<p class=\"party\">" + address + "</p>";
    h += "<p class=\"gap\">&nbsp;</p>";

    QString ids;
    if (!p.ico.empty()) ids += kv(QString(L.companyId), esc(p.ico));

    // In Czechia the DIC and the VAT number are the same string, so printing
    // both would just repeat it.
    const bool sameNumber = !p.dic.empty() && p.dic == p.icDph;
    if (!p.dic.empty()) ids += kv(QString(L.taxId), esc(p.dic));
    if (!p.icDph.empty() && !sameNumber) ids += kv(QString(L.vatId), esc(p.icDph));
    if (p.icDph.empty()) ids += kvNote(QString(L.notVatRegistered));
    h += kvTable(ids);
    return h;
}

QString categoryLabel(const std::string& cat, Country country) {
    const bool cz = (country == Country::CZ);
    if (cat == VatCat::ReverseCharge)
        return cz ? "přenesení daňové povinnosti" : "prenesenie daňovej povinnosti";
    if (cat == VatCat::IntraCommunity) return cz ? "dodání do EU" : "dodanie do EÚ";
    if (cat == VatCat::Export)         return cz ? "vývoz mimo EU" : "vývoz mimo EÚ";
    if (cat == VatCat::Exempt)         return cz ? "osvobozeno od DPH" : "oslobodené od DPH";
    if (cat == VatCat::ZeroRated)      return cz ? "nulová sazba" : "nulová sadzba";
    if (cat == VatCat::OutOfScope)     return cz ? "mimo předmět DPH" : "mimo predmetu DPH";
    return {};
}

} // namespace

namespace {

/// Everything the payment code needs, taken from the document rather than from
/// the current state of anything else.
PaymentRequest paymentRequestFor(const Invoice& inv, const Company& seller) {
    PaymentRequest request;
    request.iban              = seller.iban;
    request.bic               = seller.bic;
    request.localNumber       = seller.bankLocalNumber;
    request.amount            = inv.totals().payable;
    request.currency          = inv.currency;
    request.variableSymbol    = inv.variableSymbol;
    request.constantSymbol    = inv.constantSymbol;
    request.specificSymbol    = inv.specificSymbol;
    request.dueDate           = inv.dueDate;
    request.beneficiaryName   = seller.name;
    request.beneficiaryStreet = seller.address.street;
    request.beneficiaryCity   = seller.address.city;
    request.invoiceNumber     = inv.number;
    request.note              = inv.number.empty() ? std::string() : "Faktura " + inv.number;
    return request;
}


/// Draws one encoded code as a square image with the quiet zone the standard
/// requires. Whole-pixel modules, or scanners struggle.
QImage renderQr(const std::string& payload, int pixels) {
    const QrCode code = QrCode::encode(payload, QrCode::Ecc::Medium);
    if (code.size() == 0) return {};

    const int quiet   = 4;
    const int modules = code.size() + 2 * quiet;
    const int scale   = std::max(1, pixels / modules);

    QImage image(modules * scale, modules * scale, QImage::Format_RGB32);
    image.fill(Qt::white);

    QPainter painter(&image);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    for (int y = 0; y < code.size(); ++y)
        for (int x = 0; x < code.size(); ++x)
            if (code.moduleAt(x, y))
                painter.fillRect((x + quiet) * scale, (y + quiet) * scale, scale, scale,
                                 Qt::black);
    painter.end();
    return image;
}

} // namespace

std::vector<PaymentQrCode> paymentQrCodes(const Invoice& inv, const Company& seller,
                                          int pixels) {
    // A credit note has nothing to collect.
    if (inv.isCreditNote()) return {};

    // A proforma exists to be paid — that is the whole of its job — and it is
    // normally sent while it is still a draft. So it gets its code as soon as
    // it has a number and a symbol to put in it, rather than waiting to be
    // issued.
    //
    // Safe because the number on a draft is not provisional in the way it
    // looks: planIssue() keeps a number the document already has, and if that
    // number has since been taken it **refuses to issue** rather than quietly
    // moving to the next one. So the symbol encoded here is the symbol the
    // document will carry, or nothing is issued at all. A code the customer
    // pays under a symbol that later changed is the failure this was guarding
    // against, and that cannot happen.
    //
    // Everything else still waits: an invoice draft is not a request for
    // payment, and a cancelled document is neither.
    const bool payableProformaDraft =
        inv.type == DocType::Proforma && inv.state == InvoiceState::Draft &&
        !inv.number.empty() && !inv.variableSymbol.empty();
    if (inv.state != InvoiceState::Issued && !payableProformaDraft) return {};
    if (pixels <= 0) return {};

    const PaymentRequest request = paymentRequestFor(inv, seller);
    if (!request.usable()) return {};

    const std::vector<Country> standards = paymentStandardsFor(
        request, countryFromCode(seller.address.countryCode), seller.qrFormat);

    std::vector<PaymentQrCode> out;
    for (const Country standard : standards) {
        const std::string payload = paymentPayload(standard, request);
        if (payload.empty()) continue;
        QImage image = renderQr(payload, pixels);
        if (image.isNull()) continue;
        PaymentQrCode entry;
        entry.image    = std::move(image);
        entry.label    = QString::fromUtf8(paymentQrLabel(standard));
        entry.resource = "payment-qr-" + QString::number(out.size());
        out.push_back(std::move(entry));
    }
    return out;
}

QImage companyLogo(const Company& seller, int maxWidth, int maxHeight) {
    if (seller.logo.empty() || maxWidth <= 0 || maxHeight <= 0) return {};

    QImage image;
    // Format sniffed from the bytes, not from a file extension there no longer
    // is. Whatever Qt was built with is what works; a logo it cannot read is
    // simply not drawn.
    if (!image.loadFromData(reinterpret_cast<const uchar*>(seller.logo.data()),
                            static_cast<int>(seller.logo.size())))
        return {};
    if (image.isNull()) return {};

    // Only ever scaled down. Blowing a small logo up to fill the box would
    // make a tidy 200-pixel image look like a mistake.
    if (image.width() <= maxWidth && image.height() <= maxHeight) return image;
    return image.scaled(maxWidth, maxHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QString invoiceHtml(const Invoice& inv, const Company& seller, int qrPixels,
                    const std::vector<PaymentQrCode>& qrCodes,
                    int logoWidth, int logoHeight, double unitsPerPoint) {
    // The design is written in points because that is what a printed page is
    // measured in, and emitted in px because a CSS pixel is a layout unit and
    // therefore cannot be reinterpreted by whatever Qt thinks the resolution
    // is. See the header for the defect this exists to prevent.
    const auto px = [unitsPerPoint](double points) {
        int units = static_cast<int>(points * unitsPerPoint + 0.5);
        if (points > 0.0 && units < 1) units = 1;   // a hairline must not vanish
        return QString::number(units) + "px";
    };

    // Taken off the reference invoice rather than chosen: 9,75 pt for
    // everything that is read, 11,25 pt for the party's name, 18 pt for the
    // title, 15 pt for the figure that is paid. The label and its value are the
    // *same size* and differ only in colour — shrinking labels, which the first
    // attempt did, is what made the page look like a form rather than a letter.
    // Read off the reference invoice and then taken down about 8 %, because
    // at the reference's own sizes "Množstvo" would not fit its column and
    // broke across two lines. Everything moves together: shrinking one heading
    // to make a word fit is how a page stops looking like it was set.
    const double base = 9.0, nameSize = 10.5, captionSize = 9.0,
                 headingSize = 8.0, titleSize = 16.5, totalSize = 14.0,
                 footSize = 7.5;
    // And the rhythm: one line of leading is 14,6 pt, a gap between groups is
    // two of them. Air is most of what this layout is.
    // A group break on the reference is 29,2 pt between baselines — exactly two
    // lines. One of them is the normal advance, so the spacer supplies one more.
    const double lead = 13.5, groupGap = lead;

    const Totals  t       = inv.totals();
    const bool    cn      = inv.isCreditNote();
    // The document follows the seller: a Czech customer receiving a Slovak
    // invoice from a Czech supplier would look careless.
    const Country country = countryFromCode(seller.address.countryCode);
    const InvoiceLabels& L = labelsFor(country);
    QString title = L.invoice;
    if (cn)                                          title = L.creditNote;
    else if (inv.type == DocType::Proforma)          title = L.proforma;
    else if (inv.type == DocType::AdvanceTaxDocument) title = L.advanceTaxDocument;
    // Nothing is charged, so the VAT column and the recapitulation would be a
    // row of zeroes. The statement replaces them.
    const bool noVat        = !seller.chargesVat();
    const bool hasPaymentQr = qrPixels > 0 && !qrCodes.empty();
    // Quantity, unit and unit price when they say something; description and
    // price alone when every line is one piece. Decided in the core, because
    // it decides what appears on a legal document — see Model.h.
    const bool slim = quantityColumnsAreRedundant(inv.lines);
    // Two addresses in the same country do not need the country printed under
    // each of them.
    const bool crossBorder =
        !inv.buyer.address.countryCode.empty() &&
        !seller.address.countryCode.empty() &&
        countryFromCode(inv.buyer.address.countryCode) != country;

    QString h;
    h += "<html><head><meta charset=\"utf-8\"><style>"
         "body   { font-family: 'Helvetica Neue', Helvetica, Arial, sans-serif;"
                  " font-size: " + px(base) + "; color: #1a1a1a;"
                  " line-height: " + px(lead) + "; }"
         ".title { font-size: " + px(titleSize) + "; margin: 0; }"
         ".docno { color: #8a8a8a; }"
         ".label { color: #8a8a8a; font-size: " + px(captionSize) + ";"
                  " letter-spacing: " + px(0.7) + "; margin: 0 0 " + px(lead) + " 0; }"
         ".name  { font-size: " + px(nameSize) + "; margin: 0 0 " + px(4) + " 0; }"
         ".party { margin: 0 0 " + px(lead) + " 0; }"
         // Every rule on this page is a border on a table cell. Qt draws those
         // reliably in a collapsed table and not always otherwise, and with the
         // grid gone there is nothing to fall back on if one fails to appear.
         "table  { border-collapse: collapse; }"
         "table.full, table.kv, table.lines { width: 100%; }"
         ".head  { border-top: " + px(1.6) + " solid #1a1a1a; padding-top: " + px(9) + "; }"
         ".rule  { border-top: " + px(1.6) + " solid #1a1a1a; padding-top: " + px(7) + "; }"
         // Every value column gets a left gutter. Without one a long
         // description runs straight into the price beside it — there is no
         // vertical rule between them any more to stop it.
         "table.kv td.k { color: #8a8a8a; padding: " + px(1.6) + " 0; }"
         "table.kv td.v { text-align: right; padding: " + px(1.6) + " 0 "
                        + px(1.6) + " " + px(14) + "; }"
         "table.lines th { border-bottom: " + px(0.6) + " solid #b0b0b0;"
                        " padding: 0 0 " + px(5) + " 0; color: #8a8a8a;"
                        " font-size: " + px(headingSize) + "; letter-spacing: " + px(0.7) + ";"
                        " font-weight: normal; text-align: left; }"
         "table.lines td { border-bottom: " + px(0.6) + " solid #e0e0e0;"
                        " padding: " + px(7) + " 0; }"
         "table.lines th.r, table.lines td.r { padding-left: " + px(12) + "; }"
         "table.lines th.u, table.lines td.u { padding-left: " + px(8) + "; }"
         "table.recap th { border-bottom: " + px(0.6) + " solid #b0b0b0;"
                        " padding: 0 " + px(6) + " " + px(4) + " 0; color: #8a8a8a;"
                        " font-size: " + px(headingSize) + "; font-weight: normal;"
                        " text-align: left; }"
         "table.recap td { border-bottom: " + px(0.6) + " solid #e0e0e0;"
                        " padding: " + px(4) + " " + px(6) + " " + px(4) + " 0; }"
         // A thin box round the payment code, as on the reference. It tells the
         // customer where the code ends, which matters when a phone is being
         // held up to it.
         ".qrbox { border: " + px(0.6) + " solid #d8d8d8; padding: " + px(9) + "; }"
         ".r     { text-align: right; }"
         ".total { font-size: " + px(totalSize) + "; }"
         ".muted { color: #8a8a8a; font-size: " + px(footSize) + "; }"
         ".contact { color: #8a8a8a; font-size: " + px(base) + "; margin: 0; }"
         ".foot  { color: #8a8a8a; font-size: " + px(footSize) + "; margin: 0 0 "
                  + px(6) + " 0; }"
         // The gaps between groups. A table takes no margin of its own in this
         // HTML subset, so the air has to come from a paragraph. It carries a
         // non-breaking space and a matching line-height because an *empty*
         // paragraph has no line box at all and is therefore worth nothing
         // however large its font is set — which is how the first attempt at
         // this ran every group straight into the next.
         ".gap   { margin: 0; font-size: " + px(groupGap) + ";"
                  " line-height: " + px(groupGap) + "; }"
         ".gapbig { margin: 0; font-size: " + px(groupGap * 1.5) + ";"
                  " line-height: " + px(groupGap * 1.5) + "; }"
         // Under the letterhead. Wider than the rest, because the logo is a
         // block of solid colour and the caption sat straight underneath it —
         // the one place on the page where two lines of air read as one.
         ".gaphead { margin: 0; font-size: " + px(groupGap * 2.0) + ";"
                  " line-height: " + px(groupGap * 2.0) + "; }"
         // Belt and braces with the valign attribute. Where vertical-align is
         // honoured and valign is not, a column with fewer rows than the one
         // beside it gets centred against it, and the two halves of the header
         // stop lining up — which in a layout with no grid reads as a mistake.
         "td.col { vertical-align: top; }"
         ".draft { color: #b00020; border: " + px(1.6) + " solid #b00020;"
                  " padding: " + px(4) + " " + px(8) + ";"
                  " font-size: " + px(12) + "; letter-spacing: " + px(1.5) + "; }"
         "</style></head><body>";

    // A draft must never be mistaken for the real document. There is no
    // diagonal watermark in QTextDocument's HTML subset, so it says so in
    // words, at the top, in red.
    if (inv.state == InvoiceState::Draft && inv.type != DocType::Proforma)
        h += "<p class=\"draft\"><b>" +
             QString(country == Country::CZ ? "NÁVRH – NENÍ DAŇOVÝM DOKLADEM"
                                            : "NÁVRH – NIE JE DAŇOVÝM DOKLADOM") +
             "</b></p>";
    else if (inv.state == InvoiceState::Cancelled)
        h += "<p class=\"draft\"><b>" +
             QString(country == Country::CZ ? "STORNOVÁNO" : "STORNOVANÉ") +
             "</b></p>";

    // ------------------------------------------------------------- the head
    // Logo on the left where a letterhead would be, the document's name on the
    // right under a heavy rule. The logo is sized by the caller in the
    // document's own units, and drawn only when the caller has actually
    // registered the image — recomputing that condition here is how a picture
    // ends up as a broken-image box.
    h += "<table class=\"full\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\"><tr>"
         "<td width=\"48%\" valign=\"bottom\">";
    if (logoWidth > 0 && logoHeight > 0)
        h += "<img src=\"company-logo\" width=\"" + QString::number(logoWidth) +
             "\" height=\"" + QString::number(logoHeight) + "\"/>";
    h += "</td><td width=\"8%\"></td>"
         "<td width=\"44%\" valign=\"top\" class=\"col head\">"
         "<p class=\"title\"><b>" + title + "</b> "
         "<span class=\"docno\">" + esc(inv.number) + "</span></p>"
         "</td></tr></table>";

    h += "<p class=\"gaphead\">&nbsp;</p>";

    // ----------------------------------------------- the parties, and under
    // each of them the things that belong to that side: how to pay on the
    // left, when it is due on the right.
    QString payment;
    if (!seller.bankLocalNumber.empty())
        payment += kv(QString(L.bankAccount), esc(seller.bankLocalNumber));
    if (!seller.iban.empty())
        payment += kv(QString(L.iban), esc(sk::formatIban(seller.iban)));
    if (!seller.bic.empty())
        payment += kv(QString(L.swift), esc(seller.bic));
    if (!inv.variableSymbol.empty())
        payment += kv(QString(L.variableSymbol), esc(inv.variableSymbol));
    if (!inv.constantSymbol.empty())
        payment += kv(QString(L.constantSymbol), esc(inv.constantSymbol));
    if (!payment.isEmpty())
        payment += kv(QString(L.paymentMethod), QString(L.byTransfer));

    QString dates;
    dates += kv(QString(L.issueDate), date(inv.issueDate));
    if (!inv.taxPointDate.empty()) dates += kv(QString(L.taxPointDate), date(inv.taxPointDate));
    if (!inv.dueDate.empty())      dates += kv(QString(L.dueDate), date(inv.dueDate));

    h += "<table class=\"full\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\"><tr>"
         "<td width=\"48%\" valign=\"top\" class=\"col\">"
       + partyColumn(QString(L.supplier), seller, L, crossBorder)
       + (payment.isEmpty() ? QString() : "<p class=\"gap\">&nbsp;</p>" + kvTable(payment))
       + "</td><td width=\"8%\"></td>"
         "<td width=\"44%\" valign=\"top\" class=\"col\">"
       + partyColumn(QString(L.customer), inv.buyer, L, crossBorder)
       + "<p class=\"gap\">&nbsp;</p>" + kvTable(dates)
       + "</td></tr></table>";

    // How to reach the seller, in one line. Nowhere else on the page is there
    // room for it without making a column of its own.
    QStringList contact;
    if (!seller.email.empty()) contact << esc(seller.email);
    if (!seller.phone.empty()) contact << esc(seller.phone);
    if (!contact.isEmpty())
        h += "<p class=\"gap\">&nbsp;</p><p class=\"contact\"><i>" +
             contact.join(" | ") + "</i></p>";
    // A table takes no margin of its own in this HTML subset, so the air above
    // the line table has to come from a paragraph. Without it the contact line
    // and the column heading sit on top of one another.
    h += "<p class=\"gap\">&nbsp;</p>";

    // ----------------------------------------------------------------- lines
    // The widths add up to 100 in each combination, or Qt squeezes the row and
    // a column silently loses its share.
    // The currency travels with the figure, on every row of the money column
    // and on the total — as it does on the reference. The unit-price column is
    // left bare: repeating it twice on the same row is where it stops helping.
    const QString cur = " " + esc(inv.currency);

    h += "<table class=\"lines full\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\"><tr>";
    if (slim) {
        h += "<th width=\"66%\">&nbsp;</th>"
             "<th class=\"r\" width=\"34%\">" + QString(L.priceColumn) +
             "</th>";
    } else {
        h += "<th width=\"" + QString::number(noVat ? 38 : 32) + "%\">" +
             QString(L.description) + "</th>"
             "<th class=\"r\" width=\"14%\">" + QString(L.quantity) + "</th>"
             "<th class=\"u\" width=\"8%\">" + QString(L.unit) + "</th>"
             "<th class=\"r\" width=\"18%\">" + QString(L.unitPrice) + "</th>"
           + (noVat ? QString() : "<th class=\"r\" width=\"6%\">" + QString(L.vat) + "</th>")
           + "<th class=\"r\" width=\"22%\">" + QString(L.lineTotal) + "</th>";
    }
    h += "</tr>";

    // How many cells an indented note under a line has to skip to reach the
    // money column. One shape for both layouts, so a discount cannot land in
    // the wrong column when the table collapses.
    const int emptyCells = slim ? 0 : (noVat ? 3 : 4);

    for (const InvoiceLine& l : inv.lines) {
        h += "<tr><td>" + esc(l.description) + "</td>";
        if (!slim) {
            h += "<td class=\"r\">" + num(l.quantity, 2) + "</td>"
                 "<td class=\"u\">" + esc(l.unit) + "</td>"
                 "<td class=\"r\">" + grouped(l.unitPrice) + "</td>"
               + (noVat ? QString() : "<td class=\"r\">" + num(l.vatRate, 0) + " %</td>");
        }
        h += "<td class=\"r\">" + grouped(l.netAmount()) + cur + "</td></tr>";

        // A discount on this line, spelled out underneath it. The line total
        // above already includes it, so without this the customer sees a
        // figure that does not follow from the quantity and the price.
        for (const Allowance& a : l.allowances) {
            QString label = esc(a.reason);
            if (label.isEmpty())
                label = a.isCharge ? QString(L.surcharge) : QString(L.discount);
            if (!a.percentage.isZero()) label += " (" + num(a.percentage, 0) + " %)";
            h += "<tr><td class=\"muted\">&nbsp;&nbsp;" + label + "</td>";
            for (int i = 0; i < emptyCells; ++i) h += "<td></td>";
            h += "<td class=\"r muted\">" + (a.isCharge ? QString("+") : QString("-")) +
                 grouped(a.amount) + "</td></tr>";
        }
    }
    h += "</table>";

    // ------------------------------------------------- the recap, the code
    // and the figure. The payment code sits bottom left, away from the numbers
    // it encodes, because that is where there is room for it and because the
    // total is the thing the eye should land on first.
    QString left;
    if (!noVat &&
        (t.restated || t.vat.size() > 1 ||
         (!t.vat.empty() && t.vat.front().category != VatCat::Standard))) {
        // In a foreign currency the recap carries both: the crowns the customer
        // pays and the euro the VAT is owed in, per rate. That is the table an
        // auditor reconciles the kontrolný výkaz against.
        const QString suffix = t.restated ? " " + esc(inv.vatAccountingCurrency) : QString();
        left += "<p class=\"label\">" + QString(L.vatRecap) + "</p>"
                "<table class=\"recap full\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\">"
                "<tr><th>" + QString(L.rate) + "</th><th class=\"r\">" +
                QString(L.taxableAmount) + "</th><th class=\"r\">" + QString(L.vatAmount) +
                "</th>";
        if (t.restated)
            left += "<th class=\"r\">" + QString(L.taxableAmount) + suffix +
                    "</th><th class=\"r\">" + QString(L.vatAmount) + suffix + "</th>";
        left += "</tr>";
        for (const VatBreakdownRow& r : t.vat) {
            QString lbl = num(r.rate, 0) + " %";
            const QString extra = categoryLabel(r.category, country);
            if (!extra.isEmpty()) lbl += " <span class=\"muted\">(" + extra + ")</span>";
            left += "<tr><td>" + lbl + "</td><td class=\"r\">" + grouped(r.taxableAmount) +
                    "</td><td class=\"r\">" + grouped(r.taxAmount) + "</td>";
            if (t.restated)
                left += "<td class=\"r\">" + grouped(r.taxableAmountAccounting) +
                        "</td><td class=\"r\">" + grouped(r.taxAmountAccounting) + "</td>";
            left += "</tr>";
        }
        left += "</table>";
    }
    if (hasPaymentQr) {
        left += "<p class=\"gap\">&nbsp;</p>";
        left += "<table cellspacing=\"0\" cellpadding=\"0\"><tr><td class=\"qrbox\">";
        for (const PaymentQrCode& qr : qrCodes)
            left += "<img src=\"" + qr.resource + "\" width=\"" + QString::number(qrPixels) +
                    "\" height=\"" + QString::number(qrPixels) + "\"/>";
        left += "</td></tr></table>";
        // Named, because two unlabelled codes side by side are a puzzle.
        QStringList names;
        for (const PaymentQrCode& qr : qrCodes) names << qr.label;
        left += "<p class=\"foot\">" + names.join(" · ") + " – " + QString(L.scanToPay) + "</p>";
    }

    // The steps to the total, small, above it. With a discount or a prepayment
    // the figure has to be followable: showing only the amount payable would
    // leave the customer unable to check the arithmetic.
    QString steps;
    const bool adjusted = !t.allowanceTotal.isZero() || !t.chargeTotal.isZero();
    if (adjusted) {
        steps += kv(QString(L.lineSubtotal), grouped(t.lineExtension));
        for (const Allowance& a : inv.allowances) {
            QString label = esc(a.reason);
            if (label.isEmpty())
                label = a.isCharge ? QString(L.surcharge) : QString(L.discount);
            if (!a.percentage.isZero())
                label += " (" + num(a.percentage, 0) + " %)";
            steps += kv(label, (a.isCharge ? QString() : QString("-")) + grouped(a.amount));
        }
    }
    if (!noVat) {
        steps += kv(QString(L.taxExclusive), grouped(t.taxExclusive));
        steps += kv(QString(L.vat), grouped(t.taxAmount));
    } else if (adjusted) {
        // No VAT to show, so the net after adjustment would otherwise never
        // appear and the discount would look like it went nowhere.
        steps += kv(QString(L.taxExclusive), grouped(t.taxExclusive));
    }
    if (!t.prepaidAmount.isZero())
        steps += kv(QString(L.prepaid), "-" + grouped(t.prepaidAmount));
    if (!t.rounding.isZero())
        steps += kv(QString(L.rounding),
                    (t.rounding.isNegative() ? QString() : QString("+")) + grouped(t.rounding));

    QString right = kvTable(steps);
    right += "<table class=\"full\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\">"
             "<tr><td class=\"rule r\"><span class=\"label\">" + QString(L.payable).toUpper() +
             "</span></td></tr>"
             "<tr><td class=\"r total\"><b>" + grouped(t.payable) + cur +
             "</b></td></tr></table>";

    // The VAT in the seller's own currency. Not decoration: § 74 ods. 1 písm. i)
    // makes the euro figure a required particular of the invoice itself, so it
    // has to be on the page and not only in the XML.
    if (t.restated) {
        QString restated;
        restated += kv(QString(L.taxExclusive) + " " + esc(inv.vatAccountingCurrency),
                       grouped(t.taxExclusiveAccounting));
        restated += kv(QString(L.vatInCurrency) + " " + esc(inv.vatAccountingCurrency),
                       grouped(t.taxAmountAccounting));
        right += kvTable(restated);
        // The rate underneath, small. No Member State may *require* it (Article
        // 230 allows nothing beyond Article 226), but it is what lets the
        // customer and the auditor check the line above.
        right += "<p class=\"foot r\">" + QString(L.exchangeRate) + " 1 EUR = " +
                 num(inv.exchangeRate, 3) + " " +
                 esc(inv.currency == "EUR" ? inv.vatAccountingCurrency : inv.currency) +
                 (inv.exchangeRateDate.empty() ? QString()
                                               : " (" + date(inv.exchangeRateDate) + ")") +
                 "</p>";
    }

    h += "<table class=\"full\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\"><tr>"
         "<td width=\"48%\" valign=\"top\" class=\"col\">" + left + "</td>"
         "<td width=\"8%\"></td>"
         "<td width=\"44%\" valign=\"top\" class=\"col\">" + right + "</td>"
         "</tr></table>";

    // ---------------------------------------------------------------- notes
    if (!inv.vatExemptionReason.empty())
        h += "<p><b>" + esc(inv.vatExemptionReason) + "</b></p>";
    if (!inv.note.empty())
        h += "<p>" + esc(inv.note).replace("\n", "<br/>") + "</p>";

    // Everything that is true of the document rather than said by it, small
    // and grey at the foot of the page.
    h += "<p class=\"gapbig\">&nbsp;</p>";
    if (!inv.precedingNumber.empty())
        h += "<p class=\"foot\">" + QString(L.creditNoteFor) + " " + esc(inv.precedingNumber) +
             (inv.precedingDate.empty() ? QString()
                                        : " " + QString(L.issuedOn) + " " + date(inv.precedingDate)) +
             "</p>";
    if (!inv.relatedProformaNumbers.empty()) {
        QString list;
        for (const std::string& n : inv.relatedProformaNumbers) {
            if (!list.isEmpty()) list += ", ";
            list += esc(n);
        }
        h += "<p class=\"foot\">" + QString(L.settlesProformas) + " " + list + "</p>";
    }
    if (!inv.settledByNumber.empty())
        h += "<p class=\"foot\">" + QString(country == Country::CZ ? "Vyúčtováno fakturou č. "
                                                                   : "Vyúčtované faktúrou č. ") +
             esc(inv.settledByNumber) + "</p>";
    if (!inv.orderReference.empty())
        h += "<p class=\"foot\">" + QString(L.orderReference) + ": " +
             esc(inv.orderReference) + "</p>";
    if (!seller.registryNote.empty())
        h += "<p class=\"foot\">" + esc(seller.registryNote) + "</p>";
    h += "<p class=\"foot\">" + QString(L.electronicNote) + "</p>";
    // Last, and in bold: saying so on the document itself is what keeps a
    // proforma out of anyone's accounting by mistake.
    if (inv.type == DocType::Proforma)
        h += "<p class=\"foot\"><b>" + QString(L.notATaxDocument) + "</b></p>";
    h += "</body></html>";
    return h;
}

namespace {

/// Shared by both renderers, so the archived bytes and a freshly written file
/// can never differ in layout.
void paintInvoice(QPdfWriter& writer, const Invoice& inv, const Company& seller) {
    writer.setPageSize(QPageSize(QPageSize::A4));
    // 10 mm, near enough the reference's 9,2 mm and still inside what every
    // desktop printer can reach. The old 15 mm threw away 20 mm of a page this
    // layout wants to spread across.
    writer.setPageMargins(QMarginsF(10, 10, 10, 10), QPageLayout::Millimeter);
    writer.setResolution(300);
    writer.setTitle(QString::fromStdString(inv.number));

    QTextDocument doc;
    doc.setDefaultStyleSheet("");

    doc.setPageSize(QSizeF(writer.width(), writer.height()));

    // The page is laid out in the writer's device pixels — 2126 across at
    // 300 dpi — so a length written in points would be converted against
    // whatever Qt takes the resolution to be, and it does not take it to be
    // the writer's. That is how the first version of this layout printed at a
    // third of its size in the corner of an empty sheet. Everything is given
    // in these units instead, and this is the factor that gets it there.
    const double unitsPerPoint = writer.resolution() / 72.0;

    // Size the code as a fraction of the printable width rather than from the
    // device resolution. Everything else on the page is laid out in these same
    // units, so this is the one measure that cannot end up out of scale with
    // the text — about 36 mm on A4, as on the reference.
    int qrPixels = static_cast<int>(doc.pageSize().width() * 0.19);
    std::vector<PaymentQrCode> qrCodes = paymentQrCodes(inv, seller, qrPixels);
    // Two codes have to share the space one would have taken, or the payment
    // block pushes the table off the page.
    if (qrCodes.size() > 1) {
        qrPixels = static_cast<int>(qrPixels * 0.62);
        qrCodes  = paymentQrCodes(inv, seller, qrPixels);
    }
    for (const PaymentQrCode& qr : qrCodes)
        doc.addResource(QTextDocument::ImageResource, QUrl(qr.resource), QVariant(qr.image));

    // About 55 mm by 19 mm on A4, which is the letterhead on the reference.
    // Measured in the document's own units like everything else, so it scales
    // with the page rather than with the device resolution.
    const int logoBoxWidth  = static_cast<int>(doc.pageSize().width() * 0.29);
    const int logoBoxHeight = static_cast<int>(doc.pageSize().width() * 0.10);
    const QImage logo = companyLogo(seller, logoBoxWidth, logoBoxHeight);
    if (!logo.isNull())
        doc.addResource(QTextDocument::ImageResource, QUrl("company-logo"), QVariant(logo));

    doc.setHtml(invoiceHtml(inv, seller, qrCodes.empty() ? 0 : qrPixels, qrCodes,
                            logo.isNull() ? 0 : logo.width(),
                            logo.isNull() ? 0 : logo.height(),
                            unitsPerPoint));
    doc.print(&writer);
}

} // namespace

QByteArray renderInvoicePdf(const Invoice& inv, const Company& seller, QString* error) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly)) {
        if (error) *error = "PDF sa nepodarilo vytvoriť v pamäti.";
        return {};
    }
    {
        QPdfWriter writer(&buffer);
        paintInvoice(writer, inv, seller);
    }                                   // the writer must finish before closing
    buffer.close();

    if (bytes.isEmpty() && error) *error = "PDF sa nepodarilo vygenerovať.";
    return bytes;
}

bool writeInvoicePdf(const Invoice& inv, const Company& seller,
                     const QString& path, QString* error) {
    QFileInfo info(path);
    if (!info.absoluteDir().exists()) {
        if (error) *error = "Priečinok neexistuje: " + info.absolutePath();
        return false;
    }

    {
        QPdfWriter writer(path);
        paintInvoice(writer, inv, seller);
    }

    if (!QFile::exists(path)) {
        if (error) *error = "PDF sa nepodarilo zapísať: " + path;
        return false;
    }
    return true;
}

} // namespace fk
