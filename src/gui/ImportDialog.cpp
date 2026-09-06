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

#include "ImportDialog.h"

#include "GuiUtil.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

#include <algorithm>

namespace fk {
namespace gui {
namespace {

enum Column {
    ColTick, ColDirection, ColDate, ColAmount, ColCounterparty, ColVs, ColInvoice, ColNote,
    ColCount
};

QString money(const Dec& amount, const std::string& currency) {
    return qstr(amount.toString(2)) + " " + qstr(currency);
}

} // namespace

ImportDialog::ImportDialog(Database& db, const Statement& statement,
                           const QString& sourceFile, const QString& sourceSha,
                           QWidget* parent)
    : QDialog(parent), db_(db), statement_(statement),
      sourceFile_(sourceFile), sourceSha_(sourceSha) {

    setWindowTitle("Import bankového výpisu");
    resize(1000, 560);

    invoices_ = db_.payableInvoices();
    owed_     = db_.payableReceivedInvoices();

    // Both directions off one guard list. A movement is either a credit or a
    // debit, so the two matchers never see the same one — but they must share
    // what has already been recorded, or a statement imported twice would pay
    // the other direction's invoices on the second pass.
    const std::vector<std::string> seen = db_.importedTransactionIds();
    for (const TransactionMatch& m :
         matchTransactions(statement_, invoices_, seen, MatchDirection::Incoming))
        rows_.push_back({m, MatchDirection::Incoming});
    for (const TransactionMatch& m :
         matchTransactions(statement_, owed_, seen, MatchDirection::Outgoing))
        rows_.push_back({m, MatchDirection::Outgoing});

    // Back into the order the bank wrote them in. Grouping by direction would
    // put a day's movements in two places on a screen whose whole job is to be
    // checked against the statement it came from.
    std::sort(rows_.begin(), rows_.end(), [](const Row& a, const Row& b) {
        return a.match.transaction < b.match.transaction;
    });

    auto* layout = new QVBoxLayout(this);

    auto* heading = new QLabel(this);
    QString period;
    if (!statement_.dateStart.empty())
        period = " za obdobie " + qstr(statement_.dateStart) + " – " + qstr(statement_.dateEnd);
    heading->setText("Výpis z účtu " + qstr(statement_.iban) + period +
                     ". Zaškrtnuté sa zaúčtujú — prijaté platby k vydaným faktúram, "
                     "odoslané k prijatým.");
    heading->setWordWrap(true);
    layout->addWidget(heading);

    table_ = new QTableWidget(this);
    table_->setObjectName("importTable");   // reached by name from the GUI tests
    table_->setColumnCount(ColCount);
    table_->setHorizontalHeaderLabels(
        {"", "Smer", "Dátum", "Suma", "Protistrana", "VS", "Faktúra", "Poznámka"});
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(table_, 1);

    summary_ = new QLabel(this);
    layout->addWidget(summary_);

    auto* buttons = new QDialogButtonBox(this);
    import_ = buttons->addButton("Zaúčtovať", QDialogButtonBox::AcceptRole);
    import_->setObjectName("importButton");
    buttons->addButton("Zrušiť", QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { applyImport(); });

    buildRows();
    updateSummary();
}

void ImportDialog::buildRows() {
    table_->setRowCount(static_cast<int>(rows_.size()));

    for (int row = 0; row < static_cast<int>(rows_.size()); ++row) {
        const Row&              r = rows_[static_cast<size_t>(row)];
        const TransactionMatch& m = r.match;
        const BankTransaction&  t = statement_.transactions.at(m.transaction);
        const bool outgoing = r.direction == MatchDirection::Outgoing;

        // A checkbox inside a centred wrapper: a checkable item would be
        // indistinguishable from a selected row at a glance.
        auto* holder = new QWidget(table_);
        auto* box    = new QCheckBox(holder);
        auto* hl     = new QHBoxLayout(holder);
        hl->addWidget(box);
        hl->setAlignment(Qt::AlignCenter);
        hl->setContentsMargins(0, 0, 0, 0);
        box->setChecked(m.quality == MatchQuality::Confident);
        table_->setCellWidget(row, ColTick, holder);
        connect(box, &QCheckBox::toggled, this, [this] { updateSummary(); });

        table_->setItem(row, ColDirection,
                        new QTableWidgetItem(outgoing ? "Odoslaná" : "Prijatá"));
        table_->setItem(row, ColDate,   new QTableWidgetItem(qstr(t.date)));
        table_->setItem(row, ColAmount, new QTableWidgetItem(money(t.amount, t.currency)));
        table_->setItem(row, ColCounterparty,
                        new QTableWidgetItem(qstr(t.counterName.empty() ? t.counterAccount
                                                                        : t.counterName)));
        table_->setItem(row, ColVs, new QTableWidgetItem(qstr(t.variableSymbol)));

        // Every open document in *this row's* direction is selectable, so a
        // wrong guess can be corrected here rather than by editing the payment
        // afterwards. Offering the other direction's invoices would let a
        // payment to a supplier be recorded against a customer.
        auto* pick = new QComboBox(table_);
        pick->addItem("— nezaúčtovať —", QVariant::fromValue<qlonglong>(0));
        for (const PayableInvoice& inv : (outgoing ? owed_ : invoices_)) {
            if (inv.cancelled) continue;
            pick->addItem(qstr(inv.number) + "  (" + money(inv.outstanding(), inv.currency) + ")",
                          QVariant::fromValue<qlonglong>(inv.id));
        }
        const int found = pick->findData(QVariant::fromValue<qlonglong>(m.invoiceId));
        pick->setCurrentIndex(found >= 0 ? found : 0);
        table_->setCellWidget(row, ColInvoice, pick);
        connect(pick, &QComboBox::currentIndexChanged, this, [this] { updateSummary(); });

        table_->setItem(row, ColNote, new QTableWidgetItem(qstr(m.reason)));
    }

    table_->resizeColumnsToContents();
    table_->setColumnWidth(ColTick, 44);   // sized from items, which a cell widget is not
    table_->horizontalHeader()->setSectionResizeMode(ColNote, QHeaderView::Stretch);
}

void ImportDialog::updateSummary() {
    int incoming = 0, outgoing = 0;
    Dec in, out;
    bool mixedCurrency = false;
    std::string currency;

    for (int row = 0; row < table_->rowCount(); ++row) {
        auto* holder = table_->cellWidget(row, ColTick);
        auto* box    = holder ? holder->findChild<QCheckBox*>() : nullptr;
        auto* pick   = qobject_cast<QComboBox*>(table_->cellWidget(row, ColInvoice));
        if (!box || !pick || !box->isChecked()) continue;
        if (pick->currentData().toLongLong() == 0) continue;

        const Row& r = rows_.at(static_cast<size_t>(row));
        const BankTransaction& t = statement_.transactions.at(r.match.transaction);
        // Magnitudes, kept apart. Netting money in against money out would
        // produce one number that answers neither question.
        if (r.direction == MatchDirection::Outgoing) { ++outgoing; out += t.amount.abs(); }
        else                                          { ++incoming; in  += t.amount.abs(); }

        if (currency.empty()) currency = t.currency;
        else if (currency != t.currency) mixedCurrency = true;
    }

    const int count = incoming + outgoing;
    if (count == 0) {
        summary_->setText("Nič na zaúčtovanie.");
    } else if (mixedCurrency) {
        summary_->setText(QString("Zaúčtuje sa %1 úhrad vo viacerých menách.").arg(count));
    } else {
        QString text;
        if (incoming > 0)
            text += QString("Prijaté: %1 (%2)").arg(incoming).arg(money(in, currency));
        if (outgoing > 0) {
            if (!text.isEmpty()) text += "   ";
            text += QString("Odoslané: %1 (%2)").arg(outgoing).arg(money(out, currency));
        }
        summary_->setText(text);
    }
    if (import_) import_->setEnabled(count > 0);
}

void ImportDialog::applyImport() {
    std::vector<Database::ImportedPayment> payments;

    for (int row = 0; row < table_->rowCount(); ++row) {
        auto* holder = table_->cellWidget(row, ColTick);
        auto* box    = holder ? holder->findChild<QCheckBox*>() : nullptr;
        auto* pick   = qobject_cast<QComboBox*>(table_->cellWidget(row, ColInvoice));
        if (!box || !pick || !box->isChecked()) continue;

        const int64_t chosen = pick->currentData().toLongLong();
        if (chosen == 0) continue;

        const Row& r = rows_.at(static_cast<size_t>(row));
        const BankTransaction& t = statement_.transactions.at(r.match.transaction);

        Database::ImportedPayment item;
        item.bankId = t.dedupKey();
        // Which field the id goes into is the whole difference between paying
        // a supplier and being paid by a customer.
        if (r.direction == MatchDirection::Outgoing) item.receivedId = chosen;
        else                                         item.invoiceId  = chosen;
        item.paidOn = t.date;
        // A debit is negative on the statement and positive as a payment: the
        // amount recorded is what was settled, and the direction is already
        // carried by which invoice it settles.
        item.amount = t.amount.abs();
        item.note   = "Bankový výpis" + (t.bankId.empty() ? std::string()
                                                          : ", pohyb " + t.bankId);

        payments.push_back(std::move(item));
    }

    if (payments.empty()) { reject(); return; }

    if (!db_.recordStatementImport(payments, sstr(sourceFile_), sstr(sourceSha_))) {
        QMessageBox::critical(this, "Import zlyhal",
                              "Nič sa nezaúčtovalo.\n\n" + qstr(db_.lastError()));
        return;
    }

    recorded_ = static_cast<int>(payments.size());
    accept();
}

} // namespace gui
} // namespace fk
