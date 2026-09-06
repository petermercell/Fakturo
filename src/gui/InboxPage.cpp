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

#include "InboxPage.h"

#include "../db/InvoiceView.h"
#include "../pdf/PdfRenderer.h"
#include "../sk/Slovak.h"
#include "../ubl/UblReader.h"
#include "GuiUtil.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QDate>
#include <QDateEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTextBrowser>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <map>

namespace fk {

using namespace fk::gui;

namespace {

enum Column { ColNumber, ColSupplier, ColIssued, ColDue, ColAmount, ColState, ColCount };

QString stateLabelFor(const Database::ReceivedInvoice& row, const std::string& today) {
    if (row.isSettled())      return "Uhradená";
    // Part-paid is its own answer. Marking a 1 200 € invoice paid with 5 € and
    // reading "Uhradená" would be the application agreeing with a mistake.
    if (row.isPaid())         return "Uhradená čiastočne";
    if (row.isOverdue(today)) return "Po splatnosti";
    return "Neuhradená";
}

/// "3 doklady" — Slovak counts differently at one, at two to four, and above.
QString documents(int n) {
    if (n == 1) return "1 doklad";
    if (n >= 2 && n <= 4) return QString::number(n) + " doklady";
    return QString::number(n) + " dokladov";
}

} // namespace

InboxPage::InboxPage(Database& db, QWidget* parent) : QWidget(parent), db_(db) {
    // Named, because there is no Q_OBJECT anywhere in this project and so
    // findChild<InboxPage*>() would match the first QWidget it met.
    setObjectName("inboxPage");

    table_ = new QTableWidget(this);
    table_->setObjectName("inbox");
    table_->setColumnCount(ColCount);
    table_->setHorizontalHeaderLabels(
        {"Číslo", "Dodávateľ", "Vystavená", "Splatnosť", "Suma", "Stav"});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    // Read-only in the strongest sense the widget offers: a received document
    // is not yours to change, and a table you can type into invites the belief
    // that it is.
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setColumnWidth(ColSupplier, 240);

    auto* importBtn = new QPushButton("Importovať XML…", this);
    importBtn->setObjectName("importReceived");
    // Always available, whatever a delivery service does later. The law routes
    // e-invoices through a certified poštár, but a file that arrived by any
    // other route is still an invoice you have to account for.
    importBtn->setToolTip("Načítať prijatú e-faktúru zo súboru (XML)");

    view_ = new QPushButton("Zobraziť", this);
    view_->setObjectName("viewReceived");
    pay_ = new QPushButton("Označiť ako uhradenú", this);
    pay_->setObjectName("markReceivedPaid");
    note_ = new QPushButton("Poznámka…", this);
    note_->setObjectName("receivedNote");
    saveXml_ = new QPushButton("Uložiť XML…", this);
    saveXml_->setObjectName("saveReceivedXml");
    remove_ = new QPushButton("Odstrániť", this);
    remove_->setObjectName("deleteReceived");

    summary_ = new QLabel(this);
    summary_->setObjectName("inboxSummary");
    summary_->setTextFormat(Qt::RichText);

    // The filter carries its mode as data rather than depending on the order
    // of the items, so the dashboard can ask for one by name.
    filter_ = new QComboBox(this);
    filter_->setObjectName("inboxFilter");
    filter_->addItem("Všetky", "all");
    filter_->addItem("Neuhradené", "unpaid");
    filter_->addItem("Splatné do 7 dní", "dueSoon");
    filter_->addItem("Po splatnosti", "overdue");
    filter_->addItem("S upozornením", "warnings");

    search_ = new QLineEdit(this);
    search_->setObjectName("inboxSearch");
    search_->setPlaceholderText("Hľadať dodávateľa, číslo, sumu…");
    search_->setClearButtonEnabled(true);
    search_->setMaximumWidth(280);

    auto* bar = new QHBoxLayout;
    bar->addWidget(importBtn);
    bar->addWidget(view_);
    bar->addWidget(pay_);
    bar->addWidget(note_);
    bar->addWidget(saveXml_);
    bar->addWidget(remove_);
    bar->addStretch();
    bar->addWidget(filter_);
    bar->addWidget(search_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(bar);
    layout->addWidget(table_);
    layout->addWidget(summary_);

    connect(importBtn, &QPushButton::clicked, this, [this] { importChosenFiles(); });
    connect(view_,     &QPushButton::clicked, this, [this] { showSelected(); });
    connect(pay_,      &QPushButton::clicked, this, [this] { markSelectedPaid(); });
    connect(note_,     &QPushButton::clicked, this, [this] { editSelectedNote(); });
    connect(saveXml_,  &QPushButton::clicked, this, [this] { saveSelectedXml(); });
    connect(remove_,   &QPushButton::clicked, this, [this] { deleteSelected(); });
    connect(filter_, &QComboBox::currentIndexChanged, this, [this](int) { reload(); });
    connect(search_, &QLineEdit::textChanged, this, [this](const QString&) { reload(); });
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this] { updateButtons(); });
    connect(table_, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem*) { showSelected(); });

    reload();
}

void InboxPage::showFiltered(const QString& mode) {
    // Both widgets blocked, then one reload. Letting each change fire its own
    // rebuilt the whole list three times for one click, and an unknown mode
    // would have cleared the search and reloaded while quietly leaving the
    // filter on whatever it was — which looks like it worked.
    if (filter_) {
        const QSignalBlocker blocked(filter_);
        const int index = filter_->findData(mode);
        filter_->setCurrentIndex(index >= 0 ? index : 0);   // 0 is "Všetky"
    }
    if (search_) {
        const QSignalBlocker blocked(search_);
        search_->clear();
    }
    reload();
}

void InboxPage::reload() {
    const std::string today = sk::todayIso();
    const std::vector<Database::ReceivedInvoice> all = db_.receivedInvoices();

    static const std::map<QString, PayableFilter> modes = {
        {"all",      PayableFilter::All},
        {"unpaid",   PayableFilter::Unpaid},
        {"dueSoon",  PayableFilter::DueSoon},
        {"overdue",  PayableFilter::Overdue},
        {"warnings", PayableFilter::Warnings}};
    const auto chosen = modes.find(filter_ ? filter_->currentData().toString() : QString("all"));
    const PayableFilter filter = chosen == modes.end() ? PayableFilter::All : chosen->second;
    const std::string query = search_ ? sstr(search_->text()) : std::string();

    // The summary counts what is *shown*, not everything: a filtered list whose
    // total describes rows you cannot see is a total you cannot check.
    std::vector<Database::ReceivedInvoice> rows;
    for (const Database::ReceivedInvoice& row : all)
        if (passesFilter(row, filter, today) && matchesSearch(row, query))
            rows.push_back(row);

    // Grouped over *everything*, not over the filtered rows: "how much do I owe
    // this supplier" is not a question the filter should be able to change the
    // answer to.
    const std::vector<SupplierGroup> groups = groupBySupplier(all);

    // Marking one paid rebuilds the list, and losing the selection there means
    // the row you were working on stops being the row the buttons act on.
    const int64_t wasSelected = selectedId();

    table_->setRowCount(0);
    int unpaid = 0;
    int withWarnings = 0;
    std::map<std::string, Dec> owed;

    for (const Database::ReceivedInvoice& row : rows) {
        const int r = table_->rowCount();
        table_->insertRow(r);

        auto* number = new QTableWidgetItem(qstr(row.number));
        // Not editable as an item either, and not only because the view says
        // so. Somebody changing the edit triggers later must not accidentally
        // make a received document typeable.
        number->setFlags(number->flags() & ~Qt::ItemIsEditable);
        // The warning travels with the row rather than hiding in the detail
        // view: a document that does not add up should be visible as such in
        // the list, not only once you open it.
        if (!row.warnings.empty()) {
            number->setText("⚠ " + qstr(row.number));
            QString tip;
            for (const std::string& w : row.warnings) tip += qstr(w) + "\n";
            number->setToolTip(tip.trimmed());
            ++withWarnings;
        }
        number->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(row.id));
        table_->setItem(r, ColNumber, number);

        auto cell = [this, r](int column, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            table_->setItem(r, column, item);
            return item;
        };

        // Everything from this supplier, in a tooltip. Grouped on the IČO the
        // document itself carries, so there is no supplier list to keep in
        // step with anything.
        QTableWidgetItem* supplier = cell(ColSupplier, qstr(row.supplier.name));
        const std::string key =
            row.supplier.ico.empty() ? foldForSearch(row.supplier.name) : row.supplier.ico;
        const auto group = std::find_if(groups.begin(), groups.end(),
                                        [&key](const SupplierGroup& g) { return g.key == key; });
        if (group != groups.end()) {
            QString tip = documents(group->count);
            if (!group->ico.empty()) tip += ", IČO " + qstr(group->ico);
            for (const CurrencyTotal& owedTo : group->outstanding)
                tip += "\nneuhradené " + decToUi(owedTo.amount) + " " + qstr(owedTo.currency);
            supplier->setToolTip(tip);
        }
        cell(ColIssued, qstr(sk::formatDateSk(row.issueDate)));
        cell(ColDue, qstr(sk::formatDateSk(row.dueDate)));

        cell(ColAmount, decToUi(row.payable) + " " + qstr(row.currency))
            ->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

        QTableWidgetItem* state = cell(ColState, stateLabelFor(row, today));
        if (row.isOverdue(today)) state->setForeground(QColor(0xB0, 0x00, 0x20));
        // A part payment says how much, or the label is a claim with no figure
        // behind it.
        if (row.isPaid() && !row.isSettled())
            state->setToolTip("Uhradené " + decToUi(row.paidAmount) + " " + qstr(row.currency) +
                              ", zostáva " + decToUi(row.outstanding()) + " " +
                              qstr(row.currency));

        if (!row.isSettled()) {
            ++unpaid;
            // What is left, not the whole invoice: a part-paid one owes the
            // remainder.
            owed[row.currency] += row.outstanding();
        }
    }

    // Says when it is describing part of the list rather than all of it. A
    // total that answers a narrower question than the one you asked is worse
    // than no total.
    const bool narrowed = rows.size() != all.size();
    QString text;
    if (unpaid == 0) {
        text = rows.empty() ? (all.empty() ? "Zatiaľ nič neprišlo."
                                           : "Ničomu nezodpovedá.")
                            : "Všetko uhradené.";
    } else {
        text = narrowed ? "Neuhradené (zobrazené): " : "Neuhradené: ";
        bool first = true;
        for (const auto& entry : owed) {
            if (!first) text += " + ";
            // Escaped: the currency code comes out of a stranger's document,
            // and this label is rich text on the main window.
            text += "<b>" + decToUi(entry.second) + " " +
                    qstr(entry.first).toHtmlEscaped() + "</b>";
            first = false;
        }
    }
    if (withWarnings > 0)
        text += QString("<span style='color:#b00020'> &nbsp; ⚠ %1 s upozornením</span>")
                    .arg(documents(withWarnings));
    summary_->setText(text);

    if (wasSelected != 0)
        for (int r = 0; r < table_->rowCount(); ++r)
            if (table_->item(r, ColNumber) &&
                table_->item(r, ColNumber)->data(Qt::UserRole).toLongLong() == wasSelected) {
                table_->setCurrentCell(r, ColNumber);
                break;
            }

    updateButtons();
}

int64_t InboxPage::selectedId() const {
    const int row = table_->currentRow();
    if (row < 0) return 0;
    QTableWidgetItem* item = table_->item(row, ColNumber);
    return item ? item->data(Qt::UserRole).toLongLong() : 0;
}

void InboxPage::updateButtons() {
    const bool have = selectedId() != 0;
    for (QPushButton* b : {view_, pay_, note_, saveXml_, remove_})
        if (b) b->setEnabled(have);
}

bool InboxPage::importFile(const QString& path, bool quiet) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (!quiet)
            QMessageBox::warning(this, "Import", "Súbor sa nedá otvoriť:\n" + path);
        return false;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    Database::ReceivedInvoice row;
    bool alreadyHere = false;
    if (!db_.importReceivedInvoice(bytes.toStdString(),
                                   QFileInfo(path).fileName().toStdString(), row,
                                   &alreadyHere)) {
        if (!quiet)
            QMessageBox::warning(this, "Import",
                                 "Súbor sa nepodarilo prečítať ako e-faktúru:\n" +
                                     qstr(db_.lastError()));
        return false;
    }
    lastImportWasDuplicate_ = alreadyHere;
    reload();
    return true;
}

void InboxPage::importChosenFiles() {
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, "Prijaté e-faktúry", QString(), "E-faktúry (*.xml);;Všetky súbory (*)");
    if (paths.isEmpty()) return;

    int added = 0;
    int duplicates = 0;
    QStringList failed;
    for (const QString& path : paths) {
        if (!importFile(path, true)) { failed << QFileInfo(path).fileName(); continue; }
        // "Imported" and "you already had this" look identical from the
        // outside, and one of them means the list did not change. Saying
        // "Načítané: 1 z 1" for a file that added nothing is how somebody
        // concludes the import is broken.
        if (lastImportWasDuplicate_) ++duplicates; else ++added;
    }

    QString text = QString("Načítané: %1 z %2.").arg(added).arg(paths.size());
    if (duplicates > 0)
        text += QString("\n%1 už bolo v zozname.").arg(documents(duplicates));
    if (!failed.isEmpty()) text += "\n\nNepodarilo sa:\n" + failed.join("\n");
    QMessageBox::information(this, "Import", text);
}

void InboxPage::showSelected() {
    const int64_t id = selectedId();
    if (id == 0) return;

    Database::ReceivedInvoice row;
    if (!db_.loadReceivedInvoice(id, row)) return;
    const std::string original = db_.receivedOriginal(id);

    QDialog dialog(this);
    dialog.setObjectName("receivedDialog");
    dialog.setWindowTitle("Prijatá faktúra " + qstr(row.number));
    dialog.resize(820, 700);

    auto* layout = new QVBoxLayout(&dialog);

    // The banner first, because it changes how everything below it should be
    // read. A document that does not reconcile is still imported — you owe the
    // money either way — but it must never be shown as if it were sound.
    if (!row.warnings.empty()) {
        QString text = "<b>Doklad nesedí sám so sebou:</b><ul>";
        for (const std::string& w : row.warnings) text += "<li>" + qstr(w).toHtmlEscaped() + "</li>";
        text += "</ul>Zobrazené sumy sú prepočítané z položiek. "
                "Dodávateľ žiada <b>" + decToUi(row.payable) + " " + qstr(row.currency) +
                "</b>.";
        auto* banner = new QLabel(text, &dialog);
        banner->setObjectName("receivedWarning");
        banner->setWordWrap(true);
        banner->setTextFormat(Qt::RichText);
        banner->setStyleSheet("background:#fff4f4; color:#b00020; padding:8px;"
                              "border:1px solid #b00020;");
        layout->addWidget(banner);
    }

    auto* body = new QTextBrowser(&dialog);
    body->setObjectName("receivedBody");
    body->setOpenExternalLinks(false);
    // Never fetch anything a received document points at. A remote image in
    // somebody else's invoice would tell them when you opened it, and is one
    // step from worse.
    body->setOpenLinks(false);

    const ReadResult read = readUbl(original);
    if (read.ok) body->setHtml(invoiceHtml(read.invoice, read.invoice.seller));
    else         body->setPlainText(qstr(original));
    layout->addWidget(body, 1);

    if (!row.note.empty()) {
        auto* note = new QLabel("<b>Poznámka:</b> " + qstr(row.note).toHtmlEscaped(), &dialog);
        note->setWordWrap(true);
        layout->addWidget(note);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    if (auto* close = buttons->button(QDialogButtonBox::Close)) close->setText("Zavrieť");
    // Close emits rejected(), not accepted(). Connecting both would be one
    // dead connection pretending to be a safety net.
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    dialog.exec();
}

void InboxPage::markSelectedPaid() {
    const int64_t id = selectedId();
    if (id == 0) return;
    Database::ReceivedInvoice row;
    if (!db_.loadReceivedInvoice(id, row)) return;

    // Already paid: the button undoes it rather than asking again.
    if (row.isPaid()) {
        const auto answer = QMessageBox::question(
            this, "Úhrada",
            "Faktúra je označená ako uhradená " + qstr(sk::formatDateSk(row.paidOn)) +
                ".\nZrušiť označenie?",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes) {
            db_.setReceivedPaid(id, "", Dec());
            reload();
        }
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle("Označiť ako uhradenú");
    auto* form = new QFormLayout(&dialog);

    auto* when = new QDateEdit(&dialog);
    when->setObjectName("paidOn");
    when->setCalendarPopup(true);
    when->setDisplayFormat("dd.MM.yyyy");
    when->setDate(QDate::currentDate());

    auto* amount = new QLineEdit(decToUi(row.payable), &dialog);
    amount->setObjectName("paidAmount");

    form->addRow("Dátum úhrady", when);
    form->addRow("Suma (" + qstr(row.currency) + ")", amount);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel,
                                         &dialog);
    if (auto* save = buttons->button(QDialogButtonBox::Save)) save->setText("Uložiť");
    if (auto* cancel = buttons->button(QDialogButtonBox::Cancel)) cancel->setText("Zrušiť");
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    if (dialog.exec() != QDialog::Accepted) return;
    db_.setReceivedPaid(id, sstr(when->date().toString(Qt::ISODate)),
                        decFromUi(amount->text(), row.payable));
    reload();
}

void InboxPage::editSelectedNote() {
    const int64_t id = selectedId();
    if (id == 0) return;
    Database::ReceivedInvoice row;
    if (!db_.loadReceivedInvoice(id, row)) return;

    QDialog dialog(this);
    dialog.setWindowTitle("Poznámka");
    dialog.resize(460, 240);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("Vaša poznámka k prijatej faktúre. Samotný doklad "
                                 "sa nemení.", &dialog));

    auto* edit = new QPlainTextEdit(qstr(row.note), &dialog);
    edit->setObjectName("noteText");
    layout->addWidget(edit, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel,
                                         &dialog);
    if (auto* save = buttons->button(QDialogButtonBox::Save)) save->setText("Uložiť");
    if (auto* cancel = buttons->button(QDialogButtonBox::Cancel)) cancel->setText("Zrušiť");
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) return;
    db_.setReceivedNote(id, sstr(edit->toPlainText()));
    reload();
}

void InboxPage::saveSelectedXml() {
    const int64_t id = selectedId();
    if (id == 0) return;
    Database::ReceivedInvoice row;
    if (!db_.loadReceivedInvoice(id, row)) return;

    QString suggested = qstr(row.number);
    suggested.replace(QRegularExpression("[^A-Za-z0-9_.-]"), "-");
    const QString path = QFileDialog::getSaveFileName(
        this, "Uložiť e-faktúru", suggested + ".xml", "E-faktúry (*.xml)");
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, "Uložiť", "Súbor sa nedá zapísať.");
        return;
    }
    // The stored bytes, not a re-rendering of them. What the accountant gets
    // has to be what arrived.
    const std::string original = db_.receivedOriginal(id);
    const qint64 written = file.write(original.data(), static_cast<qint64>(original.size()));
    const bool flushed = file.flush();
    file.close();
    // A half-written document is worse than none: it looks like a file.
    if (written != static_cast<qint64>(original.size()) || !flushed) {
        QFile::remove(path);
        QMessageBox::warning(this, "Uložiť", "Súbor sa nepodarilo zapísať celý.");
    }
}

void InboxPage::deleteSelected() {
    const int64_t id = selectedId();
    if (id == 0) return;
    Database::ReceivedInvoice row;
    if (!db_.loadReceivedInvoice(id, row)) return;

    const auto answer = QMessageBox::question(
        this, "Odstrániť",
        "Odstrániť prijatú faktúru " + qstr(row.number) + " od " +
            qstr(row.supplier.name) + "?\n\n"
            "Doklad sa vymaže aj s pôvodným súborom. Ak vám ho dodávateľ "
            "poslal, máte povinnosť ho uchovávať.",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    db_.deleteReceivedInvoice(id);
    reload();
}

} // namespace fk
