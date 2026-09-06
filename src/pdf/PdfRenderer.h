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

// PdfRenderer.h - invoice -> A4 PDF, via QTextDocument HTML. No extra library.
#pragma once

#include "../model/Model.h"

#include <QByteArray>
#include <QImage>
#include <QString>

#include <vector>

namespace fk {

/// Builds the printable HTML. Exposed separately so it can be shown in a
/// preview widget without writing a file.
/// One payment code, ready to place on the page.
struct PaymentQrCode {
    QImage  image;
    QString label;      // "PAY by square" / "QR Platba"
    QString resource;   // document resource name the HTML refers to
};

/// The payment codes for an invoice, `pixels` on a side. Empty when there is
/// nothing to collect. Normally one: EUR gets PAY by square, CZK gets QR
/// Platba. Two only when the currency and the account disagree.
std::vector<PaymentQrCode> paymentQrCodes(const Invoice& inv, const Company& seller,
                                          int pixels);

/// `qrPixels` is the side of each payment QR in the document's own layout
/// units, and `qrCodes` the ones the caller has registered as image resources.
/// The caller decides, because it is the caller that registers them —
/// recomputing the condition here is how the caption and the image end up
/// disagreeing.
///
/// `unitsPerPoint` converts the design — which is specified in points, because
/// that is what a printed page is measured in — into the layout units the
/// document is actually sized in.
///
/// It exists because of a real defect. The page was laid out in the writer's
/// device pixels (2126 across at 300 dpi) while the font sizes were given in
/// `pt`, and Qt converted those against a default of 72 dpi rather than the
/// writer's — so 9 pt became 9 units on a 2126-unit page and the whole invoice
/// printed at about a third of its intended size, in the top corner of an
/// otherwise empty sheet. Emitting every length in `px` removes the conversion
/// from the picture: a CSS pixel *is* a layout unit, whatever Qt believes the
/// resolution to be.
///
/// The default is the ratio for a 96 dpi screen, which is what the preview in
/// the Prijaté tab wants. The PDF passes its writer's own.
QString invoiceHtml(const Invoice& inv, const Company& seller, int qrPixels = 0,
                    const std::vector<PaymentQrCode>& qrCodes = {},
                    int logoWidth = 0, int logoHeight = 0,
                    double unitsPerPoint = 96.0 / 72.0);

/// The seller's logo, decoded and scaled to fit `maxWidth` × `maxHeight` with
/// its proportions kept. A null image when there is none, or when the bytes
/// are not an image this build can read — a logo that cannot be drawn must not
/// stop an invoice being issued.
QImage companyLogo(const Company& seller, int maxWidth, int maxHeight);

/// Renders the A4 PDF into memory. Empty on failure. Used at issue time, so
/// the exact bytes sent to the customer can be archived rather than
/// re-rendered later from a template that may since have changed.
QByteArray renderInvoicePdf(const Invoice& inv, const Company& seller, QString* error = nullptr);

/// Writes an A4 PDF. Returns false and fills `error` if the file cannot be written.
bool writeInvoicePdf(const Invoice& inv, const Company& seller,
                     const QString& path, QString* error = nullptr);

} // namespace fk
