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

#include "DashboardPage.h"

#include "../db/InvoiceView.h"
#include "../sk/Slovak.h"
#include "GuiUtil.h"

#include <QColor>
#include <QFont>
#include <QFrame>
#include <QLayoutItem>
#include <QPalette>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace fk {

using namespace fk::gui;   // qstr, sstr, decToUi — as in every other screen

namespace {

QString money(const CurrencyTotal& total) {
    return decToUi(total.amount) + " " + qstr(total.currency);
}

/// "3 doklady" — Slovak counts differently at one, at two to four, and above.
/// Getting this wrong is the kind of small wrongness that makes software feel
/// foreign.
QString documents(int n) {
    if (n == 1) return "1 doklad";
    if (n >= 2 && n <= 4) return QString::number(n) + " doklady";
    return QString::number(n) + " dokladov";
}

QString totalsText(const std::vector<CurrencyTotal>& totals, const QString& whenEmpty) {
    if (totals.empty()) return whenEmpty;
    QString out;
    for (const CurrencyTotal& t : totals) {
        if (!out.isEmpty()) out += "   ";
        out += money(t);
    }
    return out;
}

int countIn(const std::vector<CurrencyTotal>& totals) {
    int n = 0;
    for (const CurrencyTotal& t : totals) n += t.count;
    return n;
}

QLabel* heading(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    QFont font = label->font();
    font.setPointSizeF(font.pointSizeF() + 1.0);
    font.setBold(true);
    label->setFont(font);
    return label;
}

QLabel* figure(const QString& text, QWidget* parent, const QColor& colour = QColor()) {
    auto* label = new QLabel(text, parent);
    QFont font = label->font();
    font.setPointSizeF(font.pointSizeF() + 6.0);
    label->setFont(font);
    if (colour.isValid()) {
        QPalette p = label->palette();
        p.setColor(QPalette::WindowText, colour);
        label->setPalette(p);
    }
    return label;
}

QLabel* quiet(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    QPalette p = label->palette();
    p.setColor(QPalette::WindowText, p.color(QPalette::WindowText).lighter(160));
    label->setPalette(p);
    label->setWordWrap(true);
    return label;
}

QFrame* card(QWidget* parent) {
    auto* frame = new QFrame(parent);
    frame->setFrameShape(QFrame::StyledPanel);
    return frame;
}

} // namespace

DashboardPage::DashboardPage(Database& db, QWidget* parent) : QWidget(parent), db_(db) {
    // Named, because there is no Q_OBJECT anywhere in this project and so
    // findChild<DashboardPage*>() would quietly match the first QWidget it
    // met and cast it to the wrong type.
    setObjectName("dashboard");

    auto* outer = new QVBoxLayout(this);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    body_ = new QWidget(scroll);
    body_->setObjectName("dashboardBody");
    bodyLayout_ = new QVBoxLayout(body_);
    bodyLayout_->setAlignment(Qt::AlignTop);
    scroll->setWidget(body_);
    outer->addWidget(scroll);

    rebuild();
}

void DashboardPage::reload() { rebuild(); }

void DashboardPage::rebuild() {
    // Thrown away and rebuilt rather than updated in place. There are a dozen
    // labels; the simplest thing that cannot go stale is the right one until
    // it is measurably too slow.
    // Deleted outright, not deleteLater(). A deferred delete needs an event
    // loop at the level it was posted from, and reload() is called from paths
    // that go straight into a modal dialog — leaving the previous figures
    // alive, parented and painted behind the new ones. It would also make
    // findChild() return the stale label, which is how a test passes while
    // showing the wrong number. Nothing here re-enters rebuild(): the buttons
    // only ask the window to show a list.
    while (QLayoutItem* item = bodyLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    const std::string today = sk::todayIso();
    const Overview view = summarise(db_.invoiceList(), today);

    // ------------------------------------------------------------ what I am owed
    auto* owed = card(body_);
    auto* owedGrid = new QGridLayout(owed);

    owedGrid->addWidget(heading("Čaká sa na úhradu", owed), 0, 0, 1, 2);

    const int overdueCount = countIn(view.overdue);
    auto* overdueValue =
        figure(totalsText(view.overdue, "—"), owed,
               overdueCount > 0 ? QColor(0xB0, 0x00, 0x20) : QColor());
    overdueValue->setObjectName("overdueTotal");
    owedGrid->addWidget(quiet("Po splatnosti", owed), 1, 0);
    owedGrid->addWidget(overdueValue, 2, 0);
    owedGrid->addWidget(quiet(overdueCount ? documents(overdueCount) : QString("nič"), owed), 3, 0);

    const int dueCount = countIn(view.notYetDue);
    auto* dueValue = figure(totalsText(view.notYetDue, "—"), owed);
    dueValue->setObjectName("notYetDueTotal");
    owedGrid->addWidget(quiet("Ešte nie je splatné", owed), 1, 1);
    owedGrid->addWidget(dueValue, 2, 1);
    owedGrid->addWidget(quiet(dueCount ? documents(dueCount) : QString("nič"), owed), 3, 1);

    auto* owedButtons = new QHBoxLayout();
    auto* showOverdue = new QPushButton("Zobraziť po splatnosti", owed);
    showOverdue->setObjectName("showOverdue");
    showOverdue->setEnabled(overdueCount > 0);
    connect(showOverdue, &QPushButton::clicked, this,
            [this] { if (onShowInvoices) onShowInvoices("overdue"); });
    auto* showUnpaid = new QPushButton("Zobraziť neuhradené", owed);
    showUnpaid->setObjectName("showUnpaid");
    connect(showUnpaid, &QPushButton::clicked, this,
            [this] { if (onShowInvoices) onShowInvoices("unpaid"); });
    owedButtons->addWidget(showOverdue);
    owedButtons->addWidget(showUnpaid);
    owedButtons->addStretch();
    owedGrid->addLayout(owedButtons, 4, 0, 1, 2);

    bodyLayout_->addWidget(owed);

    // ------------------------------------------------------------- what I owe
    // Beside what you are owed, because those are the two halves of the same
    // question and looking at one without the other is how a month goes wrong.
    const Payables payables = summarisePayables(db_.receivedInvoices(), today);
    const int owedOverdue = countIn(payables.overdue);
    const int owedSoon    = countIn(payables.dueSoon);
    if (owedOverdue > 0 || owedSoon > 0 || !payables.later.empty()) {
        auto* payablesCard = card(body_);
        auto* grid = new QGridLayout(payablesCard);
        grid->addWidget(heading("Na úhradu", payablesCard), 0, 0, 1, 2);

        auto* lateValue =
            figure(totalsText(payables.overdue, "—"), payablesCard,
                   owedOverdue > 0 ? QColor(0xB0, 0x00, 0x20) : QColor());
        lateValue->setObjectName("owedOverdueTotal");
        grid->addWidget(quiet("Po splatnosti", payablesCard), 1, 0);
        grid->addWidget(lateValue, 2, 0);
        grid->addWidget(quiet(owedOverdue ? documents(owedOverdue) : QString("nič"), payablesCard),
                        3, 0);

        auto* soonValue = figure(totalsText(payables.dueSoon, "—"), payablesCard);
        soonValue->setObjectName("owedDueSoonTotal");
        grid->addWidget(quiet("Splatné do 7 dní", payablesCard), 1, 1);
        grid->addWidget(soonValue, 2, 1);
        grid->addWidget(quiet(owedSoon ? documents(owedSoon) : QString("nič"), payablesCard), 3, 1);

        // The third column exists because the card is shown when there is
        // anything at all outstanding. Without it, an inbox holding only
        // invoices due next month drew a card headed "Na úhradu" whose every
        // figure was "nič".
        const int owedLater = countIn(payables.later);
        auto* laterValue = figure(totalsText(payables.later, "—"), payablesCard);
        laterValue->setObjectName("owedLaterTotal");
        grid->addWidget(quiet("Neskôr", payablesCard), 1, 2);
        grid->addWidget(laterValue, 2, 2);
        grid->addWidget(quiet(owedLater ? documents(owedLater) : QString("nič"), payablesCard),
                        3, 2);

        if (payables.hasOldest) {
            auto* oldest = quiet(qstr(payables.oldestNumber) + " — " + qstr(payables.oldestSupplier) +
                                 ", " + decToUi(payables.oldestAmount) + " " +
                                 qstr(payables.oldestCurrency) + ", splatná " +
                                 qstr(sk::formatDateSk(payables.oldestDueDate)), payablesCard);
            oldest->setObjectName("oldestPayable");
            grid->addWidget(oldest, 4, 0, 1, 3);
        }

        auto* buttons = new QHBoxLayout();
        // "Unpaid", not "due soon". The figure above it excludes what is
        // already overdue and the filter would have included it, so clicking
        // a number would have opened a list that does not add up to it.
        auto* showDue = new QPushButton("Zobraziť neuhradené", payablesCard);
        showDue->setObjectName("showPayables");
        connect(showDue, &QPushButton::clicked, this,
                [this] { if (onShowPayables) onShowPayables("unpaid"); });
        buttons->addWidget(showDue);
        if (owedOverdue > 0) {
            auto* late = new QPushButton("Po splatnosti", payablesCard);
            late->setObjectName("showPayablesOverdue");
            connect(late, &QPushButton::clicked, this,
                    [this] { if (onShowPayables) onShowPayables("overdue"); });
            buttons->addWidget(late);
        }
        if (payables.withWarnings > 0) {
            auto* warned = new QPushButton(
                "⚠ " + documents(payables.withWarnings) + " s upozornením", payablesCard);
            warned->setObjectName("showPayableWarnings");
            connect(warned, &QPushButton::clicked, this,
                    [this] { if (onShowPayables) onShowPayables("warnings"); });
            buttons->addWidget(warned);
        }
        buttons->addStretch();
        grid->addLayout(buttons, 5, 0, 1, 3);

        bodyLayout_->addWidget(payablesCard);
    }

    // ------------------------------------------------- the one worth chasing
    if (view.hasOldest) {
        auto* oldest = card(body_);
        auto* line = new QVBoxLayout(oldest);
        line->addWidget(heading("Najdlhšie nezaplatená", oldest));
        auto* text = new QLabel(oldest);
        text->setObjectName("oldestUnpaid");
        text->setWordWrap(true);
        text->setText(
            qstr(view.oldestNumber) + " — " + qstr(view.oldestCustomer) + ", " +
            decToUi(view.oldestAmount) + " " + qstr(view.oldestCurrency) +
            ", splatná " + qstr(sk::formatDateSk(view.oldestDueDate)) + " (" +
            QString::number(view.oldestDaysLate) + " dní po splatnosti)");
        line->addWidget(text);
        bodyLayout_->addWidget(oldest);
    }

    // ------------------------------------------------------------- turnover
    auto* turnover = card(body_);
    auto* grid = new QGridLayout(turnover);
    grid->addWidget(heading("Vystavené", turnover), 0, 0, 1, 3);

    auto column = [&](int col, const QString& title,
                      const std::vector<CurrencyTotal>& totals, const char* name) {
        grid->addWidget(quiet(title, turnover), 1, col);
        auto* value = figure(totalsText(totals, "—"), turnover);
        value->setObjectName(name);
        grid->addWidget(value, 2, col);
        const int n = countIn(totals);
        grid->addWidget(quiet(n ? documents(n) : QString("nič"), turnover), 3, col);
    };
    column(0, "Tento mesiac", view.issuedThisMonth, "issuedThisMonth");
    column(1, "Minulý mesiac", view.issuedLastMonth, "issuedLastMonth");
    column(2, "Tento rok",     view.issuedThisYear,  "issuedThisYear");

    bodyLayout_->addWidget(turnover);

    // -------------------------------------------------------------- drafts
    if (view.draftCount > 0) {
        auto* drafts = card(body_);
        auto* line = new QHBoxLayout(drafts);
        auto* text = new QLabel(
            documents(view.draftCount) + " je rozpracovaných — nikto ich zatiaľ nedostal.",
            drafts);
        text->setObjectName("draftNotice");
        line->addWidget(text);
        auto* show = new QPushButton("Zobraziť", drafts);
        connect(show, &QPushButton::clicked, this,
                [this] { if (onShowInvoices) onShowInvoices("draft"); });
        line->addWidget(show);
        line->addStretch();
        bodyLayout_->addWidget(drafts);
    }

    // A note on the arithmetic, because a figure you cannot account for is
    // worse than no figure.
    bodyLayout_->addWidget(quiet(
        "Sumy sú vrátane DPH a podľa meny sa nesčítavajú. Rozpracované a stornované "
        "doklady sa nezapočítavajú, proforma faktúry tiež nie — nie sú daňovým dokladom.",
        body_));
}

} // namespace fk
