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

#include "InvoiceEditor.h"

#include "../country/Country.h"
#include "../model/Units.h"
#include "../sk/Slovak.h"
#include "../ubl/Validator.h"
#include "GuiUtil.h"
#include "PartyForm.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateEdit>
#include <QFrame>
#include <QGuiApplication>
#include <QInputDialog>
#include <QStringList>
#include <QScreen>
#include <QScrollArea>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QModelIndex>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
#include <functional>

namespace fk {

using namespace fk::gui;

namespace {

enum Col { ColDesc = 0, ColQty, ColUnit, ColPrice, ColVat, ColCategory, ColTotal, ColCount };
enum AllowCol { AllowKind = 0, AllowTarget, AllowReason, AllowPercent, AllowAmount,
                AllowCategory, AllowVat, AllowCount };

const char* const CATEGORIES[] = {
    VatCat::Standard, VatCat::ReverseCharge, VatCat::IntraCommunity,
    VatCat::Export, VatCat::Exempt, VatCat::ZeroRated, VatCat::OutOfScope
};

QString categoryTitle(const std::string& c) {
    if (c == VatCat::Standard)       return "S – bežná sadzba";
    if (c == VatCat::ReverseCharge)  return "AE – prenesenie daňovej povinnosti";
    if (c == VatCat::IntraCommunity) return "K – dodanie do EÚ";
    if (c == VatCat::Export)         return "G – vývoz mimo EÚ";
    if (c == VatCat::Exempt)         return "E – oslobodené";
    if (c == VatCat::ZeroRated)      return "Z – nulová sadzba";
    return "O – mimo predmetu DPH";
}

QDate toQDate(const std::string& iso) {
    QDate d = QDate::fromString(qstr(iso), Qt::ISODate);
    return d.isValid() ? d : QDate::currentDate();
}

} // namespace

// ------------------------------------------------------------------ factory
Invoice InvoiceEditor::blank(Database& db, DocType type) {
    Company c = db.activeCompany();
    const CountryProfile& p = profileFor(c.address.countryCode);

    Invoice inv;
    inv.type         = type;
    inv.number       = db.peekNextNumber(type == DocType::Proforma
                                             ? Database::SeriesKind::Proforma
                                             : Database::SeriesKind::Invoice);
    inv.issueDate    = sk::todayIso();
    inv.taxPointDate = inv.issueDate;
    inv.dueDate      = sk::addDays(inv.issueDate, 14);
    inv.variableSymbol = sk::variableSymbolFrom(inv.number);
    inv.constantSymbol = p.paymentSymbol;
    inv.currency       = p.currency;      // EUR for a SK seller, CZK for a CZ one

    InvoiceLine l;
    l.lineNo      = 1;
    l.vatRate     = c.chargesVat() ? p.standardVatRate : Dec();
    l.vatCategory = c.chargesVat() ? VatCat::Standard : VatCat::OutOfScope;
    if (!c.chargesVat())
        inv.vatExemptionReason =
            notVatPayerNote(countryFromCode(c.address.countryCode) == Country::CZ);
    inv.lines.push_back(l);
    inv.companyId = c.id;
    inv.seller    = c;          // frozen onto the document from the start
    return inv;
}

// --------------------------------------------------------------------- ctor
InvoiceEditor::InvoiceEditor(Database& db, const Invoice& invoice, QWidget* parent)
    : QDialog(parent), db_(db), inv_(invoice) {
    // A draft follows the company as it is now; an issued document keeps what
    // it was written with.
    if (inv_.isEditable() && inv_.seller.name.empty()) {
        // Its own company, not the active one: a draft belonging to company A
        // opened while B is selected must still be A's document.
        if (inv_.companyId == 0) inv_.companyId = db_.activeCompanyId();
        db_.loadCompany(inv_.companyId, inv_.seller);
    }
    sellerChargesVat_ = inv_.seller.chargesVat();
    setWindowTitle(QString(docTypeLabel(inv_.type)) +
                   (inv_.isEditable() ? "" : QString(" – ") + stateLabel(inv_.state)));
    // Never larger than the screen it opens on. A dialog whose Save button is
    // below the bottom edge cannot be finished, and on a laptop 720 points plus
    // a title bar plus the Dock is already too much.
    {
        QSize wanted(940, 720);
        const QScreen* screen = parent && parent->screen() ? parent->screen()
                                                           : QGuiApplication::primaryScreen();
        if (screen) {
            const QRect available = screen->availableGeometry();
            wanted.setWidth(std::min(wanted.width(), available.width() - 60));
            wanted.setHeight(std::min(wanted.height(), available.height() - 80));
        }
        resize(wanted);
    }
    buildUi();
    loadFromModel();
    applyReadOnly();
}

void InvoiceEditor::buildUi() {
    number_   = new QLineEdit(this);
    docType_  = new QComboBox(this);
    // Object names exist so the GUI tests can reach these without the tests
    // needing to be a friend of the class. Every defect of consequence in this
    // project came from this layer, untested; naming a widget is a cheap price.
    docType_->setObjectName("docType");
    issue_    = new QDateEdit(this);
    taxPoint_ = new QDateEdit(this);
    due_      = new QDateEdit(this);
    currency_ = new QLineEdit(this);
    vs_       = new QLineEdit(this);
    ks_       = new QLineEdit(this);
    ss_       = new QLineEdit(this);
    customer_ = new QComboBox(this);
    buyerInfo_= new QLabel(this);
    buyerRef_ = new QLineEdit(this);
    orderRef_ = new QLineEdit(this);
    preceding_= new QLineEdit(this);
    exemption_= new QPlainTextEdit(this);
    note_     = new QPlainTextEdit(this);
    lines_    = new QTableWidget(this);
    lines_->setObjectName("lines");
    totals_   = new QLabel(this);
    regime_   = new QLabel(this);

    // Every document type, carrying the enum value as data. Reading this combo
    // by index was how a proforma silently turned into an invoice on save.
    for (DocType t : {DocType::Invoice, DocType::CreditNote,
                      DocType::Proforma, DocType::AdvanceTaxDocument})
        docType_->addItem(docTypeLabel(t), static_cast<int>(t));
    for (QDateEdit* d : {issue_, taxPoint_, due_}) {
        d->setCalendarPopup(true);
        d->setDisplayFormat("dd.MM.yyyy");
    }
    currency_->setObjectName("currency");
    currency_->setMaxLength(3);
    currency_->setMaximumWidth(70);
    vs_->setPlaceholderText("max 10 číslic");
    // The reverse-charge wording is a two-line sentence in two languages; a
    // single-line field showed only its tail.
    exemption_->setPlaceholderText("Dôvod oslobodenia / prenesenia daňovej povinnosti");
    exemption_->setFixedHeight(exemption_->fontMetrics().height() * 3 + 12);
    exemption_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    note_->setMaximumHeight(70);
    buyerInfo_->setWordWrap(true);
    buyerInfo_->setTextFormat(Qt::RichText);
    totals_->setTextFormat(Qt::RichText);
    totals_->setAlignment(Qt::AlignRight);
    regime_->setWordWrap(true);
    regime_->setStyleSheet("color:#555;");
    regime_->setMinimumHeight(regime_->fontMetrics().height() * 2);
    regime_->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    // ------------------------------------------------------------- head form
    auto* head = new QGroupBox("Doklad", this);
    auto* hf = new QFormLayout(head);
    hf->addRow("Typ", docType_);
    hf->addRow("Číslo *", number_);
    hf->addRow("Dátum vystavenia *", issue_);
    hf->addRow("Dátum dodania", taxPoint_);
    hf->addRow("Dátum splatnosti", due_);
    hf->addRow("Mena", currency_);
    hf->addRow("Variabilný symbol", vs_);
    hf->addRow("Konštantný symbol", ks_);
    hf->addRow("Špecifický symbol", ss_);
    hf->addRow("Dobropis k faktúre", preceding_);

    // ----------------------------------------------------------- buyer panel
    auto* editBuyer = new QPushButton("Upraviť údaje odberateľa…", this);
    auto* buyer = new QGroupBox("Odberateľ", this);
    auto* bf = new QFormLayout(buyer);
    bf->addRow("Z adresára", customer_);
    bf->addRow("", editBuyer);
    bf->addRow(buyerInfo_);
    bf->addRow("Referencia odberateľa", buyerRef_);
    bf->addRow("Číslo objednávky", orderRef_);

    auto* top = new QHBoxLayout;
    top->addWidget(head, 1);
    top->addWidget(buyer, 1);

    // ----------------------------------------------------------- lines table
    lines_->setColumnCount(ColCount);
    lines_->setHorizontalHeaderLabels(
        {"Popis", "Množstvo", "MJ", "Cena/MJ bez DPH", "DPH %", "Kategória DPH", "Spolu bez DPH"});
    lines_->horizontalHeader()->setSectionResizeMode(ColDesc, QHeaderView::Stretch);
    lines_->horizontalHeader()->setSectionResizeMode(ColCategory, QHeaderView::ResizeToContents);
    lines_->verticalHeader()->setVisible(false);
    lines_->setSelectionBehavior(QAbstractItemView::SelectRows);

    // Room for four rows and the heading without scrolling. Measured from the
    // style's own row height rather than guessed in pixels, so it still holds
    // at a larger font or on a different platform.
    {
        const int rowHeight = lines_->verticalHeader()->defaultSectionSize();
        lines_->setMinimumHeight(rowHeight * 5 + 12);
    }

    // "Dole" with nothing selected appends, so this covers the plain "add a
    // row" case too — two buttons that do everything the old three did.
    auto* addAboveBtn = new QPushButton("+ Riadok hore", this);
    auto* addBelowBtn = new QPushButton("+ Riadok dole", this);
    auto* delBtn = new QPushButton("– Odstrániť", this);
    auto* chkBtn = new QPushButton("Skontrolovať", this);
    addAboveBtn->setObjectName("addLineAbove");
    addBelowBtn->setObjectName("addLineBelow");
    delBtn->setObjectName("removeLine");
    auto* fromCatalog = new QPushButton("Z katalógu…", this);
    fromCatalog->setObjectName("fromCatalog");
    fromCatalog->setToolTip("Vložiť uloženú položku");
    auto* toCatalog = new QPushButton("Uložiť do katalógu", this);
    toCatalog->setObjectName("toCatalog");
    toCatalog->setToolTip("Uložiť označený riadok na neskôr");

    auto* lineButtons = new QHBoxLayout;
    lineButtons->addWidget(addAboveBtn);
    lineButtons->addWidget(addBelowBtn);
    lineButtons->addWidget(delBtn);
    lineButtons->addSpacing(12);
    lineButtons->addWidget(fromCatalog);
    lineButtons->addWidget(toCatalog);
    lineButtons->addStretch();
    lineButtons->addWidget(chkBtn);

    auto* bottom = new QFormLayout;
    bottom->addRow("Režim plnenia", regime_);
    bottom->addRow(sellerChargesVat_ ? "Dôvod oslobodenia od DPH" : "Text namiesto DPH",
                   exemption_);
    bottom->addRow("Poznámka na faktúre", note_);

    // A non-payer should never have to think about a VAT rate. The columns are
    // hidden rather than disabled, because an empty column invites the question
    // "what should go here?" all over again.
    if (!sellerChargesVat_) {
        lines_->setColumnHidden(ColVat, true);
        lines_->setColumnHidden(ColCategory, true);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Save)->setText("Uložiť");
    buttons->button(QDialogButtonBox::Cancel)->setText("Zrušiť");

    // Everything except Uložiť/Zrušiť scrolls. Making the lines table taller
    // pushed those two buttons off the bottom of a laptop screen — and a
    // dialog you cannot confirm is broken whatever else it does. Keeping them
    // outside the scroll area means that cannot happen again, at any window
    // size or font.
    auto* content = new QWidget(this);
    content->setObjectName("editorContent");
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->addLayout(top);
    contentLayout->addWidget(lines_, 1);
    contentLayout->addLayout(lineButtons);

    // ------------------------------------------------- discounts and charges
    // A row of its own rather than a negative line. A negative line is wrong
    // in the XML and lands in the wrong total, so the VAT breakdown stops
    // reconciling — which is invisible until an Access Point rejects it.
    allowances_ = new QTableWidget(this);
    allowances_->setObjectName("allowances");
    allowances_->setColumnCount(AllowCount);
    allowances_->setHorizontalHeaderLabels(
        {"Typ", "Položka", "Dôvod", "%", "Suma", "Kategória DPH", "DPH %"});
    allowances_->horizontalHeader()->setSectionResizeMode(AllowReason, QHeaderView::Stretch);
    allowances_->verticalHeader()->setVisible(false);
    allowances_->setSelectionBehavior(QAbstractItemView::SelectRows);
    {
        const int rowHeight = allowances_->verticalHeader()->defaultSectionSize();
        allowances_->setMinimumHeight(rowHeight * 3 + 12);
        allowances_->setMaximumHeight(rowHeight * 5 + 12);
    }
    if (!sellerChargesVat_) {
        allowances_->setColumnHidden(AllowCategory, true);
        allowances_->setColumnHidden(AllowVat, true);
    }

    auto* addDiscount  = new QPushButton("+ Zľava", this);
    auto* addSurcharge = new QPushButton("+ Príplatok", this);
    auto* delAllowance = new QPushButton("– Odstrániť", this);
    addDiscount->setObjectName("addDiscount");
    addSurcharge->setObjectName("addSurcharge");
    delAllowance->setObjectName("removeAllowance");

    rounding_ = new QLineEdit(this);
    rounding_->setObjectName("rounding");
    rounding_->setPlaceholderText("0,00");
    rounding_->setMaximumWidth(110);
    auto* roundBtn = new QPushButton("Zaokrúhliť na celé", this);
    roundBtn->setObjectName("roundWhole");

    auto* allowanceButtons = new QHBoxLayout;
    allowanceButtons->addWidget(addDiscount);
    allowanceButtons->addWidget(addSurcharge);
    allowanceButtons->addWidget(delAllowance);
    allowanceButtons->addStretch();
    allowanceButtons->addWidget(new QLabel("Zaokrúhlenie", this));
    allowanceButtons->addWidget(rounding_);
    allowanceButtons->addWidget(roundBtn);

    // The exchange rate, for an invoice written in a currency other than the
    // one its VAT is owed in. Hidden for everyone else — which is almost
    // everyone — so the screen does not carry a field that never applies.
    fxRate_ = new QLineEdit(this);
    fxRate_->setObjectName("fxRate");
    fxRate_->setPlaceholderText("napr. 24,167");
    fxRate_->setMaximumWidth(110);
    fxDate_ = new QDateEdit(this);
    fxDate_->setObjectName("fxDate");
    fxDate_->setCalendarPopup(true);
    fxDate_->setDisplayFormat("dd.MM.yyyy");
    fxSuffix_ = new QLabel(this);
    fxRow_ = new QWidget(this);
    fxRow_->setObjectName("fxRow");
    auto* fxLayout = new QHBoxLayout(fxRow_);
    fxLayout->setContentsMargins(0, 0, 0, 0);
    fxLayout->addWidget(new QLabel("Kurz pre DPH:  1 EUR =", fxRow_));
    fxLayout->addWidget(fxRate_);
    fxLayout->addWidget(fxSuffix_);
    fxLayout->addWidget(new QLabel("ku dňu", fxRow_));
    fxLayout->addWidget(fxDate_);
    fxLayout->addStretch();
    fxRow_->setVisible(false);

    contentLayout->addWidget(allowances_);
    contentLayout->addLayout(allowanceButtons);
    contentLayout->addWidget(fxRow_);
    contentLayout->addLayout(bottom);
    contentLayout->addWidget(totals_);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName("editorScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);

    auto* root = new QVBoxLayout(this);
    root->addWidget(scroll, 1);
    root->addWidget(buttons);

    // ------------------------------------------------------------ behaviour
    connect(addDiscount,  &QPushButton::clicked, this, [this] { appendAllowance(false); });
    connect(addSurcharge, &QPushButton::clicked, this, [this] { appendAllowance(true); });
    connect(delAllowance, &QPushButton::clicked, this, [this] { removeSelectedAllowances(); });
    connect(roundBtn,     &QPushButton::clicked, this, [this] { roundToWholeUnit(); });
    connect(allowances_,  &QTableWidget::cellChanged, this,
            [this](int, int) { if (!updating_) recalcTotals(); });
    connect(rounding_, &QLineEdit::textChanged, this,
            [this](const QString&) { if (!updating_) recalcTotals(); });
    connect(fxRate_, &QLineEdit::textChanged, this,
            [this](const QString&) { if (!updating_) recalcTotals(); });
    connect(fxDate_, &QDateEdit::dateChanged, this, [this](const QDate&) {
        if (updating_) return;
        fxDateChosen_ = true;          // from here on it stops following the tax point
        recalcTotals();
    });
    // Until the person picks one, the rate date is the day before the tax point
    // — § 26 ods. 1 — so correcting the delivery date has to move it along.
    connect(taxPoint_, &QDateEdit::dateChanged, this, [this](const QDate& d) {
        if (updating_ || fxDateChosen_) return;
        updating_ = true;
        fxDate_->setDate(d.addDays(-1));
        updating_ = false;
        recalcTotals();
    });
    // The rate row appears and disappears with the currency, so it has to be
    // reconsidered as the currency is typed rather than only when the dialog
    // opens.
    connect(currency_, &QLineEdit::textChanged, this, [this](const QString&) {
        if (updating_) return;
        updateExchangeRow();
        recalcTotals();
    });

    connect(addAboveBtn, &QPushButton::clicked, this, [this] {
        const int at = lines_->currentRow();
        insertEmptyLine(at < 0 ? 0 : at);
    });
    connect(addBelowBtn, &QPushButton::clicked, this, [this] {
        const int at = lines_->currentRow();
        insertEmptyLine(at < 0 ? -1 : at + 1);     // nothing selected: append
    });
    connect(delBtn, &QPushButton::clicked, this, [this] { removeSelectedLines(); });
    connect(fromCatalog, &QPushButton::clicked, this, [this] { insertFromCatalogue(); });
    connect(toCatalog,   &QPushButton::clicked, this, [this] { saveLineToCatalogue(); });
    connect(chkBtn, &QPushButton::clicked, this, [this] { runValidation(); });
    connect(editBuyer, &QPushButton::clicked, this, [this] { editBuyerDetails(); });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(lines_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (updating_) return;
        // Renaming a line has to relabel the Položka pickers, or a discount
        // says it belongs to "1. test" long after that line became something
        // else. refreshAllowanceTargets keeps each row on the same line
        // number, so nothing moves.
        if (item && item->column() == ColDesc) refreshAllowanceTargets();
        recalcTotals();
    });
    connect(customer_, &QComboBox::currentIndexChanged, this, [this](int i) { pickCustomer(i); });
    connect(number_, &QLineEdit::textEdited, this, [this](const QString& text) {
        if (vs_->text().isEmpty() || vs_->text() == qstr(sk::variableSymbolFrom(inv_.number)))
            vs_->setText(qstr(sk::variableSymbolFrom(sstr(text))));
        inv_.number = sstr(text);
    });
    connect(issue_, &QDateEdit::dateChanged, this, [this](const QDate& d) {
        if (due_->date() <= d) due_->setDate(d.addDays(14));
    });
    connect(docType_, &QComboBox::currentIndexChanged, this, [this](int) {
        const DocType chosen = static_cast<DocType>(docType_->currentData().toInt());
        preceding_->setEnabled(chosen == DocType::CreditNote ||
                               chosen == DocType::AdvanceTaxDocument);
        if (updating_ || inv_.id != 0) return;

        // A document that has never been saved can be renumbered freely, and a
        // proforma belongs in its own series.
        const auto kind = chosen == DocType::Proforma ? Database::SeriesKind::Proforma
                                                      : Database::SeriesKind::Invoice;
        const std::string number = db_.peekNextNumber(kind);
        number_->setText(qstr(number));
        vs_->setText(qstr(sk::variableSymbolFrom(number)));
    });
}

// -------------------------------------------------------------- model <-> ui
/// A locked document is shown, not edited. Everything is disabled rather than
/// hidden, so the numbers are still there to look at and to copy.
void InvoiceEditor::applyReadOnly() {
    if (inv_.isEditable()) return;

    // Disable the inputs by name rather than sweeping every child widget:
    // findChildren() is recursive and would also disable the buttons inside
    // the button box, leaving no way to close the dialog.
    const QList<QWidget*> inputs = {
        number_, docType_, issue_, taxPoint_, due_, currency_, vs_, ks_, ss_,
        customer_, buyerRef_, orderRef_, preceding_, exemption_, note_};
    for (QWidget* w : inputs)
        if (w) w->setEnabled(false);

    // The lines stay readable and selectable, just not editable.
    lines_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // Cell widgets are not items, so NoEditTriggers does not reach them. Both
    // combos have to be disabled by hand or an issued document stays editable
    // in two of its columns.
    for (int r = 0; r < lines_->rowCount(); ++r)
        for (int c : {ColUnit, ColCategory})
            if (auto* combo = qobject_cast<QComboBox*>(lines_->cellWidget(r, c)))
                combo->setEnabled(false);

    allowances_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    for (int r = 0; r < allowances_->rowCount(); ++r)
        for (int c : {AllowKind, AllowTarget, AllowCategory})
            if (auto* combo = qobject_cast<QComboBox*>(allowances_->cellWidget(r, c)))
                combo->setEnabled(false);
    if (rounding_) rounding_->setEnabled(false);
    if (fxRate_)   fxRate_->setEnabled(false);
    if (fxDate_)   fxDate_->setEnabled(false);

    for (QPushButton* b : findChildren<QPushButton*>()) {
        // Everything except the button box's own buttons.
        if (b->parentWidget() && qobject_cast<QDialogButtonBox*>(b->parentWidget())) continue;
        b->setEnabled(false);
    }

    if (auto* box = findChild<QDialogButtonBox*>()) {
        // setStandardButtons() replaces the buttons but keeps the box and its
        // connections, so rejected() is already wired to reject().
        box->setStandardButtons(QDialogButtonBox::Close);
        if (auto* close = box->button(QDialogButtonBox::Close)) close->setText("Zavrieť");
    }

    auto* banner = new QLabel(
        QString("Doklad je %1 a nedá sa upravovať. Opravu urobte dobropisom.")
            .arg(QString(stateLabel(inv_.state)).toLower()), this);
    banner->setStyleSheet("color:#b00020;");
    if (auto* row = qobject_cast<QVBoxLayout*>(layout())) row->insertWidget(0, banner);
}

void InvoiceEditor::loadFromModel() {
    updating_ = true;

    const int typeIndex = docType_->findData(static_cast<int>(inv_.type));
    docType_->setCurrentIndex(typeIndex >= 0 ? typeIndex : 0);
    number_->setText(qstr(inv_.number));
    issue_->setDate(toQDate(inv_.issueDate));
    taxPoint_->setDate(toQDate(inv_.taxPointDate.empty() ? inv_.issueDate : inv_.taxPointDate));
    due_->setDate(toQDate(inv_.dueDate.empty() ? sk::addDays(inv_.issueDate, 14) : inv_.dueDate));
    currency_->setText(qstr(inv_.currency));
    vs_->setText(qstr(inv_.variableSymbol));
    ks_->setText(qstr(inv_.constantSymbol));
    ss_->setText(qstr(inv_.specificSymbol));
    buyerRef_->setText(qstr(inv_.buyerReference));
    orderRef_->setText(qstr(inv_.orderReference));
    preceding_->setText(qstr(inv_.precedingNumber));
    preceding_->setEnabled(inv_.type == DocType::CreditNote ||
                           inv_.type == DocType::AdvanceTaxDocument);
    exemption_->setPlainText(qstr(inv_.vatExemptionReason));
    note_->setPlainText(qstr(inv_.note));

    customer_->clear();
    customer_->addItem("— vlastné údaje —", QVariant::fromValue<qlonglong>(0));
    for (const Customer& c : db_.customers()) {
        customer_->addItem(qstr(c.name + (c.ico.empty() ? "" : "  (IČO " + c.ico + ")")),
                           QVariant::fromValue<qlonglong>(c.id));
        if (inv_.buyer.id != 0 && c.id == inv_.buyer.id)
            customer_->setCurrentIndex(customer_->count() - 1);
    }

    lines_->setRowCount(0);
    for (const InvoiceLine& l : inv_.lines) addLine(l);
    if (inv_.lines.empty()) appendEmptyLine();

    allowances_->setRowCount(0);
    for (const InvoiceLine& l : inv_.lines)
        for (const Allowance& a : l.allowances) addAllowanceRow(a, l.lineNo);
    for (const Allowance& a : inv_.allowances) addAllowanceRow(a, 0);
    refreshAllowanceTargets();
    if (rounding_)
        rounding_->setText(inv_.roundingAmount.isZero() ? QString()
                                                        : decToUi(inv_.roundingAmount, 2));

    // Three decimals: the ECB publishes the crown to three, and rounding the
    // rate to two would move the euro figure by more than the cent it is
    // supposed to be accurate to.
    fxRate_->setText(inv_.exchangeRate.isZero() ? QString() : decToUi(inv_.exchangeRate, 3));
    // § 26 ods. 1 wants the rate of the day *before* the tax point, so that is
    // where the date starts. It is a starting point, not a rule the app
    // enforces: the customs rate, which some payers elect, is the rate of the
    // day itself.
    fxDateChosen_ = !inv_.exchangeRateDate.empty();
    fxDate_->setDate(fxDateChosen_ ? toQDate(inv_.exchangeRateDate)
                                   : taxPoint_->date().addDays(-1));
    // The rate on the document belongs to the currency the document is in —
    // the non-euro one of the pair, since the rate is quoted per euro.
    fxRateFor_ = inv_.exchangeRate.isZero()
                     ? std::string()
                     : (inv_.currency == "EUR" ? accountingCurrency() : inv_.currency);

    updating_ = false;
    updateBuyerInfo();
    applySupplyRegime();
    updateExchangeRow();
    recalcTotals();
}

/// The VAT treatment follows from who is buying, not from a menu the user has
/// to remember. A §7a supplier billing a business in another member state must
/// not charge VAT and must say the recipient accounts for it; the same
/// supplier billing at home simply charges nothing.
void InvoiceEditor::applySupplyRegime() {
    const Company& seller = inv_.seller;
    const SupplyRegime regime = detectSupplyRegime(seller, inv_.buyer);
    const Country sellerCountry = countryFromCode(seller.address.countryCode);
    const std::string category = vatCategoryFor(regime);
    const std::string note     = supplyNoteFor(regime, sellerCountry);

    QString description;
    switch (regime) {
        case SupplyRegime::DomesticVat:
            description = "Tuzemské plnenie s DPH."; break;
        case SupplyRegime::DomesticNoVat:
            description = "Tuzemské plnenie bez DPH – nie ste platiteľom."; break;
        case SupplyRegime::ReverseChargeEu:
            description = "Dodanie do EÚ – prenesenie daňovej povinnosti na odberateľa. "
                          "Na doklade musia byť obe IČ DPH."; break;
        case SupplyRegime::ExportOutsideEu:
            description = "Vývoz mimo EÚ – bez DPH."; break;
    }
    regime_->setText(description);

    if (!inv_.isEditable()) return;

    // Only fill the note when the user has not written their own.
    const std::string current = sstr(exemption_->toPlainText());
    const bool userWroteNote =
        !current.empty() &&
        current != supplyNoteFor(SupplyRegime::DomesticNoVat, sellerCountry) &&
        current != supplyNoteFor(SupplyRegime::ReverseChargeEu, sellerCountry) &&
        current != supplyNoteFor(SupplyRegime::ExportOutsideEu, sellerCountry);
    if (!userWroteNote) exemption_->setPlainText(qstr(note));

    // The switch has to work in both directions. Choosing an EU customer zeroes
    // every line; choosing a domestic one again must put the rate back, or the
    // invoice ships at 0 % with a reverse-charge category on it.
    auto wasSetByRegime = [](const std::string& c) {
        return c == VatCat::ReverseCharge || c == VatCat::Export ||
               c == VatCat::IntraCommunity || c == VatCat::OutOfScope;
    };
    const Dec standardRate = profileFor(seller.address.countryCode).standardVatRate;

    const bool previous = updating_;
    updating_ = true;
    for (int r = 0; r < lines_->rowCount(); ++r) {
        auto* cat = qobject_cast<QComboBox*>(lines_->cellWidget(r, ColCategory));

        if (regime == SupplyRegime::DomesticVat) {
            // Only undo what a previous regime imposed; a line the user
            // deliberately marked exempt stays exempt.
            if (!cat) continue;
            if (!wasSetByRegime(sstr(cat->currentData().toString()))) continue;
            const int index = cat->findData(QString(VatCat::Standard));
            if (index >= 0) cat->setCurrentIndex(index);
            if (auto* item = lines_->item(r, ColVat)) item->setText(decToUi(standardRate, 0));
            continue;
        }

        if (auto* item = lines_->item(r, ColVat)) item->setText(decToUi(Dec(), 0));
        if (cat) {
            const int index = cat->findData(qstr(category));
            if (index >= 0) cat->setCurrentIndex(index);
        }
    }
    updating_ = previous;
}

void InvoiceEditor::addLine(const InvoiceLine& line, int atRow) {
    const int row = (atRow < 0 || atRow > lines_->rowCount()) ? lines_->rowCount() : atRow;
    lines_->insertRow(row);

    lines_->setItem(row, ColDesc,  new QTableWidgetItem(qstr(line.description)));
    lines_->setItem(row, ColQty,   new QTableWidgetItem(decToUi(line.quantity, 2)));
    // A picker rather than free text, because the unit is written twice: what
    // is printed ("hod") and the UN/ECE code that goes into the Peppol XML
    // ("HUR"). The code used to be H87 — "piece" — for everything, so an
    // invoice for eight hours went out as eight pieces. Editable, so an
    // unusual unit is still possible; the code then stays as it was.
    auto* unit = new QComboBox(lines_);
    unit->setEditable(true);
    const Country sellerCountry = countryFromCode(inv_.seller.address.countryCode);
    for (const UnitOfMeasure& u : unitsOfMeasure())
        unit->addItem(qstr(sellerCountry == Country::CZ ? u.labelCz : u.labelSk),
                      QString(u.code));
    const int known = unit->findText(qstr(line.unit));
    if (known >= 0) unit->setCurrentIndex(known);
    else            unit->setCurrentText(qstr(line.unit));
    unit->setProperty("unitCode", qstr(line.unitCodeUn.empty() ? "H87" : line.unitCodeUn));
    connect(unit, &QComboBox::currentTextChanged, this, [this, unit](const QString& text) {
        // The code follows the label. For something we do not recognise it
        // falls back to H87, "piece" — keeping the previous code would print
        // "kus balenia" while exporting HUR, which is an invoice claiming
        // hours it does not show. H87 is at least the neutral answer.
        const std::string code = unitCodeForLabel(sstr(text));
        unit->setProperty("unitCode", qstr(code.empty() ? std::string("H87") : code));
        if (!updating_) recalcTotals();
    });
    lines_->setCellWidget(row, ColUnit, unit);
    lines_->setItem(row, ColPrice, new QTableWidgetItem(decToUi(line.unitPrice, 2)));
    lines_->setItem(row, ColVat,   new QTableWidgetItem(decToUi(line.vatRate, 0)));

    auto* cat = new QComboBox(lines_);
    for (const char* c : CATEGORIES) cat->addItem(categoryTitle(c), QString(c));
    int idx = cat->findData(qstr(line.vatCategory));
    cat->setCurrentIndex(idx >= 0 ? idx : 0);
    connect(cat, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!updating_) recalcTotals();
    });
    lines_->setCellWidget(row, ColCategory, cat);

    auto* total = new QTableWidgetItem(decToUi(line.netAmount()));
    total->setFlags(total->flags() & ~Qt::ItemIsEditable);
    total->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    lines_->setItem(row, ColTotal, total);
}

int InvoiceEditor::insertEmptyLine(int atRow) {
    const Company& c = inv_.seller;
    const CountryProfile& p = profileFor(c.address.countryCode);

    // Follow the supply regime, not just the seller's country: adding a row to
    // a reverse-charge invoice must not slip a 23 % line into it, which would
    // both charge VAT wrongly and break BR-AE-5.
    const SupplyRegime regime = detectSupplyRegime(c, inv_.buyer);

    InvoiceLine l;
    l.quantity    = Dec::fromInt(1);
    l.vatCategory = vatCategoryFor(regime);
    l.vatRate     = (regime == SupplyRegime::DomesticVat) ? p.standardVatRate : Dec();

    const int row = (atRow < 0 || atRow > lines_->rowCount()) ? lines_->rowCount() : atRow;
    // Numbering is not set here. A line inserted in the middle would otherwise
    // duplicate the number of the row it displaced; collectToModel numbers the
    // rows in the order they appear on screen, which is the order that ends up
    // on the invoice.
    // The guard is saved and restored rather than set and cleared, and the
    // recalculation is skipped while it is up. loadFromModel() adds the first
    // line of an empty document from inside its own guard; clearing the flag
    // from underneath it let the rest of the load run unguarded, with
    // collectToModel() emptying the exchange rate, the rounding and the
    // discounts out of the model moments before the widgets were filled from
    // them. loadFromModel() does both of these itself when it has finished.
    const bool previous = updating_;
    updating_ = true;
    addLine(l, row);
    updating_ = previous;
    if (!previous) {
        refreshAllowanceTargets();
        recalcTotals();
    }

    // Selected and being typed into, so a second click on the same button adds
    // the next row where you would expect rather than back at the top.
    lines_->setCurrentCell(row, ColDesc);
    lines_->editItem(lines_->item(row, ColDesc));
    return row;
}

void InvoiceEditor::appendEmptyLine() { insertEmptyLine(-1); }

// ------------------------------------------- discounts and surcharges

void InvoiceEditor::addAllowanceRow(const Allowance& a, int lineNo) {
    const QSignalBlocker blocker(allowances_);
    const int row = allowances_->rowCount();
    allowances_->insertRow(row);

    // Which line this belongs to, or the whole document. One table for both,
    // because widening the lines table is what makes this screen unusable on a
    // laptop — and because a discount is the same thing at either level.
    auto* target = new QComboBox(allowances_);
    target->setProperty("wanted", lineNo);
    connect(target, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!updating_) recalcTotals();
    });
    allowances_->setCellWidget(row, AllowTarget, target);

    auto* kind = new QComboBox(allowances_);
    kind->addItem("Zľava",     false);
    kind->addItem("Príplatok", true);
    kind->setCurrentIndex(a.isCharge ? 1 : 0);
    connect(kind, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!updating_) recalcTotals();
    });
    allowances_->setCellWidget(row, AllowKind, kind);

    allowances_->setItem(row, AllowReason,  new QTableWidgetItem(qstr(a.reason)));
    allowances_->setItem(row, AllowPercent,
                         new QTableWidgetItem(a.percentage.isZero() ? QString()
                                                                   : decToUi(a.percentage, 2)));
    allowances_->setItem(row, AllowAmount,  new QTableWidgetItem(decToUi(a.amount, 2)));
    allowances_->setItem(row, AllowVat,     new QTableWidgetItem(decToUi(a.vatRate, 0)));

    auto* cat = new QComboBox(allowances_);
    for (const char* c : CATEGORIES) cat->addItem(categoryTitle(c), QString(c));
    const int idx = cat->findData(qstr(a.vatCategory));
    cat->setCurrentIndex(idx >= 0 ? idx : 0);
    connect(cat, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!updating_) recalcTotals();
    });
    allowances_->setCellWidget(row, AllowCategory, cat);
}

void InvoiceEditor::refreshAllowanceTargets() {
    const QSignalBlocker blocker(allowances_);
    for (int r = 0; r < allowances_->rowCount(); ++r) {
        auto* target = qobject_cast<QComboBox*>(allowances_->cellWidget(r, AllowTarget));
        if (!target) continue;

        // What it currently points at, so a rebuild does not silently move a
        // discount from one item to another.
        const int wanted = target->count() > 0 ? target->currentData().toInt()
                                               : target->property("wanted").toInt();
        target->clear();
        target->addItem("celý doklad", 0);
        for (int line = 0; line < lines_->rowCount(); ++line) {
            const QString description =
                lines_->item(line, ColDesc) ? lines_->item(line, ColDesc)->text() : QString();
            target->addItem(QString::number(line + 1) + ". " +
                                (description.isEmpty() ? QString("(bez popisu)") : description),
                            line + 1);
        }
        const int found = target->findData(wanted);
        target->setCurrentIndex(found >= 0 ? found : 0);
        target->setProperty("wanted", target->currentData().toInt());
    }
}

void InvoiceEditor::appendAllowance(bool isCharge) {
    // A new discount inherits the VAT treatment of the document, because that
    // is what it reduces. Guessing "standard" on a reverse-charge invoice
    // would break BR-AE-5 the moment it was saved.
    const SupplyRegime regime = detectSupplyRegime(inv_.seller, inv_.buyer);
    const CountryProfile& p = profileFor(inv_.seller.address.countryCode);

    Allowance a;
    a.isCharge    = isCharge;
    a.vatCategory = vatCategoryFor(regime);
    a.vatRate     = (regime == SupplyRegime::DomesticVat && sellerChargesVat_)
                        ? p.standardVatRate : Dec();
    // A new one starts on the selected line, since that is nearly always what
    // was meant; "celý doklad" is one click away.
    const int selected = lines_->currentRow();
    addAllowanceRow(a, selected >= 0 ? selected + 1 : 0);
    refreshAllowanceTargets();
    recalcTotals();
    allowances_->setCurrentCell(allowances_->rowCount() - 1, AllowReason);
    allowances_->editItem(allowances_->item(allowances_->rowCount() - 1, AllowReason));
}

void InvoiceEditor::removeSelectedAllowances() {
    QList<int> rows;
    for (const QModelIndex& i : allowances_->selectionModel()->selectedRows()) rows << i.row();
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    updating_ = true;
    for (int r : rows) allowances_->removeRow(r);
    updating_ = false;
    recalcTotals();
}

Allowance InvoiceEditor::allowanceAt(int row, int* lineNoOut) const {
    auto text = [this, row](int c) {
        return allowances_->item(row, c) ? allowances_->item(row, c)->text() : QString();
    };

    int lineNo = 0;
    if (auto* target = qobject_cast<QComboBox*>(allowances_->cellWidget(row, AllowTarget)))
        lineNo = target->currentData().toInt();

    Allowance a;
    if (auto* kind = qobject_cast<QComboBox*>(allowances_->cellWidget(row, AllowKind)))
        a.isCharge = kind->currentData().toBool();
    a.reason     = sstr(text(AllowReason));
    a.percentage = decFromUi(text(AllowPercent));
    a.amount     = decFromUi(text(AllowAmount));
    a.vatRate    = decFromUi(text(AllowVat));
    if (auto* cat = qobject_cast<QComboBox*>(allowances_->cellWidget(row, AllowCategory)))
        a.vatCategory = sstr(cat->currentData().toString());

    const bool onLine = lineNo >= 1 && lineNo <= static_cast<int>(inv_.lines.size());

    if (onLine) {
        // A line allowance takes its line's VAT treatment: it is part of that
        // line's net amount and cannot belong to another rate. Its base is the
        // line's own gross — quantity times price, before any discount.
        const InvoiceLine& owner = inv_.lines[static_cast<size_t>(lineNo - 1)];
        a.vatCategory = owner.vatCategory;
        a.vatRate     = owner.vatRate;
        if (!a.percentage.isZero()) {
            a.baseAmount = owner.grossAmount();
            a.amount     = a.baseAmount.percentOf(a.percentage);
        }
    } else {
        if (!sellerChargesVat_) {
            a.vatCategory = vatCategoryFor(detectSupplyRegime(inv_.seller, inv_.buyer));
            a.vatRate     = Dec();
        }
        if (a.vatCategory != VatCat::Standard) a.vatRate = Dec();

        // For the document, the base is the sum of the lines *in the same VAT
        // category and rate*. Using every line would put a figure in BT-93
        // that the receiver cannot reproduce, and on a mixed-rate invoice
        // would take "10 % off the books" out of the 23 % lines as well.
        if (!a.percentage.isZero()) {
            Dec base;
            for (const InvoiceLine& l : inv_.lines)
                if (l.vatCategory == a.vatCategory && l.vatRate == a.vatRate)
                    base += l.netAmount();
            a.baseAmount = base;
            a.amount     = base.percentOf(a.percentage);
        }
    }

    if (lineNoOut) *lineNoOut = onLine ? lineNo : 0;
    return a;
}

std::vector<std::pair<int, Allowance>> InvoiceEditor::collectAllowances() const {
    std::vector<std::pair<int, Allowance>> out;
    for (int r = 0; r < allowances_->rowCount(); ++r) {
        int lineNo = 0;
        Allowance a = allowanceAt(r, &lineNo);
        if (a.amount.isZero()) continue;    // an empty row is not a discount
        out.emplace_back(lineNo, std::move(a));
    }
    return out;
}

/// Splits what the table holds between the lines and the document. Called
/// from collectToModel once the lines themselves are in place.
void InvoiceEditor::placeAllowances() {
    for (InvoiceLine& l : inv_.lines) l.allowances.clear();
    inv_.allowances.clear();

    for (auto& [lineNo, allowance] : collectAllowances()) {
        if (lineNo >= 1 && lineNo <= static_cast<int>(inv_.lines.size()))
            inv_.lines[static_cast<size_t>(lineNo - 1)].allowances.push_back(allowance);
        else
            inv_.allowances.push_back(allowance);
    }
}

void InvoiceEditor::roundToWholeUnit() {
    // Read what is on screen first. Every field that feeds the totals is wired
    // to recalcTotals today, so inv_ happens to be current — but relying on
    // that is one unwired widget away from rounding a stale figure.
    collectToModel();

    // From the document as it stands *without* rounding, or asking twice would
    // compound it. And from the amount actually due: BT-115 is BT-112 − BT-113
    // + BT-114, so rounding BT-112 leaves the payable on an arbitrary figure
    // whenever there is a prepayment — which is exactly what an invoice made
    // from proformas has.
    Invoice probe = inv_;
    probe.roundingAmount = Dec();
    const Totals before = probe.totals();
    const Dec suggested = suggestedRounding(before.taxInclusive - before.prepaidAmount);
    updating_ = true;
    rounding_->setText(decToUi(suggested, 2));
    updating_ = false;
    recalcTotals();
}

void InvoiceEditor::removeSelectedLines() {
    QList<int> rows;
    for (const QModelIndex& i : lines_->selectionModel()->selectedRows()) rows << i.row();
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    updating_ = true;
    for (int r : rows) lines_->removeRow(r);
    updating_ = false;
    refreshAllowanceTargets();
    if (lines_->rowCount() == 0) appendEmptyLine();
    else recalcTotals();
}

void InvoiceEditor::insertFromCatalogue() {
    if (!inv_.isEditable()) return;

    const std::vector<Database::CatalogItem> items = db_.catalogItems();
    if (items.empty()) {
        QMessageBox::information(this, "Katalóg",
            "Katalóg je zatiaľ prázdny.\n\nOznačte riadok a použite "
            "„Uložiť do katalógu“ — nabudúce ho vložíte jedným kliknutím.");
        return;
    }

    QStringList labels;
    for (const Database::CatalogItem& item : items)
        labels << qstr(item.description) + "   —   " + decToUi(item.unitPrice) + " " +
                      qstr(inv_.currency) + " / " + qstr(item.unit);

    bool chosen = false;
    const QString picked = QInputDialog::getItem(this, "Z katalógu", "Položka:", labels, 0,
                                                 false, &chosen);
    if (!chosen) return;
    const int index = labels.indexOf(picked);
    if (index < 0 || index >= static_cast<int>(items.size())) return;
    const Database::CatalogItem& item = items[static_cast<size_t>(index)];

    InvoiceLine line;
    line.description = item.description;
    line.unit        = item.unit;
    line.unitCodeUn  = item.unitCodeUn;
    line.quantity    = Dec::fromInt(1);
    line.unitPrice   = item.unitPrice;
    line.vatCategory = item.vatCategory;
    line.vatRate     = item.vatRate;

    // Below the selected row, or at the end. Then the supply regime has its
    // say: a saved 23 % line dropped onto a reverse-charge invoice must not
    // charge VAT just because that is how it was saved.
    const int at = lines_->currentRow();
    addLine(line, at < 0 ? -1 : at + 1);
    applySupplyRegime();
    refreshAllowanceTargets();
    recalcTotals();

    db_.noteCatalogUse(item.id, sk::todayIso());
}

void InvoiceEditor::saveLineToCatalogue() {
    const int row = lines_->currentRow();
    if (row < 0) {
        QMessageBox::information(this, "Katalóg", "Najprv označte riadok, ktorý sa má uložiť.");
        return;
    }

    // Read from the table rather than from the model: the row the person is
    // looking at is the one they mean, and it may not have been collected yet.
    collectToModel();
    if (row >= static_cast<int>(inv_.lines.size())) return;
    const InvoiceLine& line = inv_.lines[static_cast<size_t>(row)];
    if (line.description.empty()) {
        QMessageBox::information(this, "Katalóg", "Riadok bez popisu sa uložiť nedá.");
        return;
    }

    Database::CatalogItem item;
    item.description = line.description;
    item.unit        = line.unit;
    item.unitCodeUn  = line.unitCodeUn;
    item.unitPrice   = line.unitPrice;
    item.vatRate     = line.vatRate;
    item.vatCategory = line.vatCategory;
    if (!db_.saveCatalogItem(item)) {
        QMessageBox::warning(this, "Katalóg", qstr(db_.lastError()));
        return;
    }
    QMessageBox::information(this, "Katalóg",
        "Uložené: " + qstr(item.description) +
        "\n\nNabudúce ho vložíte cez „Z katalógu…“.");
}

std::string InvoiceEditor::accountingCurrency() const {
    // Empty for a country this application does not have rules for: profileFor
    // falls back to the Slovak profile, and taking that at face value would
    // offer a Hungarian seller a euro restatement they do not owe.
    const Country country = countryFromCode(inv_.seller.address.countryCode);
    if (country == Country::Other) return {};
    return profileFor(country).currency;
}

void InvoiceEditor::updateExchangeRow() {
    const std::string accounting = accountingCurrency();
    const std::string typed = normalisedCurrency(lineText(currency_));
    const std::string cur = typed.empty() ? std::string("EUR") : typed;

    // A non-payer charges no VAT, so there is no euro figure for the law to
    // want. Convertibility is asked of the same function that does the
    // conversion — a row offering a rate the totals would then ignore would be
    // worse than no row. Half-typed codes are ignored: "E" on the way to "EUR"
    // is not a currency, and reacting to it makes the row flicker.
    fxApplies_ = sellerChargesVat_ && (typed.empty() || typed.size() == 3) &&
                 toAccountingCurrency(Dec::fromInt(1), cur, accounting,
                                      Dec::fromInt(1)).has_value();
    fxRow_->setVisible(fxApplies_);
    if (!fxApplies_) return;

    const std::string other = cur == "EUR" ? accounting : cur;
    fxSuffix_->setText(qstr(other));

    // A rate belongs to the pair it was typed for. Change crowns to some third
    // currency and the old number would silently be applied to it — a figure
    // out by a factor of twenty-five, on the one line of the invoice the tax
    // office reads.
    if (!fxRateFor_.empty() && fxRateFor_ != other) {
        const bool previous = updating_;
        updating_ = true;
        fxRate_->clear();
        fxDateChosen_ = false;
        updating_ = previous;
    }
    fxRateFor_ = other;
}

void InvoiceEditor::collectToModel() {
    inv_.type           = static_cast<DocType>(docType_->currentData().toInt());
    inv_.number         = lineText(number_);
    inv_.issueDate      = sstr(issue_->date().toString(Qt::ISODate));
    inv_.taxPointDate   = sstr(taxPoint_->date().toString(Qt::ISODate));
    inv_.dueDate        = sstr(due_->date().toString(Qt::ISODate));
    // Folded on the way in, not on the way out: "eur" typed into a
    // three-character box is the euro, and a document that stores it as typed
    // makes every later currency comparison — including the one that decides
    // whether VAT has to be restated — answer the wrong question.
    inv_.currency       = normalisedCurrency(lineText(currency_));
    if (inv_.currency.empty()) inv_.currency = "EUR";
    inv_.variableSymbol = lineText(vs_);
    inv_.constantSymbol = lineText(ks_);
    inv_.specificSymbol = lineText(ss_);
    inv_.buyerReference = lineText(buyerRef_);
    inv_.orderReference = lineText(orderRef_);
    inv_.precedingNumber = preceding_->isEnabled() ? lineText(preceding_) : inv_.precedingNumber;
    inv_.vatExemptionReason = sstr(exemption_->toPlainText());
    inv_.note           = sstr(note_->toPlainText());

    inv_.lines.clear();
    for (int r = 0; r < lines_->rowCount(); ++r) {
        InvoiceLine l;
        l.lineNo      = r + 1;
        l.description = sstr(lines_->item(r, ColDesc) ? lines_->item(r, ColDesc)->text() : QString());
        if (auto* unit = qobject_cast<QComboBox*>(lines_->cellWidget(r, ColUnit))) {
            l.unit         = sstr(unit->currentText());
            l.unitCodeUn   = sstr(unit->property("unitCode").toString());
        }
        l.quantity    = decFromUi(lines_->item(r, ColQty)   ? lines_->item(r, ColQty)->text()   : QString());
        l.unitPrice   = decFromUi(lines_->item(r, ColPrice) ? lines_->item(r, ColPrice)->text() : QString());
        l.vatRate     = decFromUi(lines_->item(r, ColVat)   ? lines_->item(r, ColVat)->text()   : QString());
        if (l.unit.empty()) l.unit = "ks";
        if (l.unitCodeUn.empty()) l.unitCodeUn = "H87";
        if (auto* cat = qobject_cast<QComboBox*>(lines_->cellWidget(r, ColCategory)))
            l.vatCategory = sstr(cat->currentData().toString());

        // With the category column hidden there is no widget to read, so the
        // regime decides: out of scope at home, reverse charge across an EU
        // border, export beyond it.
        if (!sellerChargesVat_) {
            l.vatCategory = vatCategoryFor(detectSupplyRegime(inv_.seller, inv_.buyer));
            l.vatRate     = Dec();
        }
        // Categories other than "S" must carry a 0 % rate (BR-Z-05 and friends).
        if (l.vatCategory != VatCat::Standard) l.vatRate = Dec();
        if (l.description.empty() && l.quantity.isZero() && l.unitPrice.isZero()) continue;
        inv_.lines.push_back(l);
    }

    // After the lines, because a percentage is worked out from what it applies
    // to — and each row says whether that is one line or the document.
    placeAllowances();
    inv_.roundingAmount = rounding_ ? decFromUi(rounding_->text()) : Dec();

    // Cleared rather than left behind when the row does not apply: a currency
    // changed back to euro must not leave a stale rate on the document that
    // nothing on screen still shows.
    if (fxApplies_) {
        inv_.vatAccountingCurrency = accountingCurrency();
        inv_.exchangeRate          = decFromUi(fxRate_->text());
        inv_.exchangeRateDate      = sstr(fxDate_->date().toString(Qt::ISODate));
    } else {
        inv_.vatAccountingCurrency.clear();
        inv_.exchangeRate = Dec();
        inv_.exchangeRateDate.clear();
    }
}

void InvoiceEditor::recalcTotals() {
    collectToModel();

    // Row totals are computed straight from the row so that a blank row in the
    // middle of the table cannot shift the numbers by one line.
    updating_ = true;
    for (int r = 0; r < lines_->rowCount(); ++r) {
        auto text = [this, r](int col) {
            return lines_->item(r, col) ? lines_->item(r, col)->text() : QString();
        };
        Dec net = (decFromUi(text(ColQty)) * decFromUi(text(ColPrice))).roundTo(2);
        if (auto* item = lines_->item(r, ColTotal)) item->setText(decToUi(net));
    }
    // A percentage is the thing that was meant; the amount follows from it. So
    // the Suma column is filled in as the percentage is typed, rather than
    // quietly becoming right only once the document is saved.
    for (int r = 0; r < allowances_->rowCount(); ++r) {
        const QString typed = allowances_->item(r, AllowPercent)
                                  ? allowances_->item(r, AllowPercent)->text() : QString();
        if (decFromUi(typed).isZero()) continue;      // a flat amount is left alone
        const Allowance a = allowanceAt(r, nullptr);
        if (auto* item = allowances_->item(r, AllowAmount)) item->setText(decToUi(a.amount));
    }
    updating_ = false;

    const Totals t = inv_.totals();
    if (sellerChargesVat_) {
        QString text = QString(
            "Základ dane: <b>%1 %4</b> &nbsp;&nbsp; DPH: <b>%2 %4</b> &nbsp;&nbsp; "
            "<span style='font-size:14pt'>Na úhradu: <b>%3 %4</b></span>")
            .arg(decToUi(t.taxExclusive), decToUi(t.taxAmount), decToUi(t.payable),
                 qstr(inv_.currency));
        // The figure the tax office wants, kept in front of the person while
        // they are typing the rate rather than appearing first on the PDF.
        if (t.restated)
            text += QString("<br/><span style='color:#555'>DPH v %1: <b>%2 %1</b></span>")
                        .arg(qstr(inv_.vatAccountingCurrency),
                             decToUi(t.taxAmountAccounting));
        totals_->setText(text);
    } else {
        totals_->setText(QString("<span style='font-size:14pt'>Na úhradu: <b>%1 %2</b></span>")
                             .arg(decToUi(t.payable), qstr(inv_.currency)));
    }
}

// ------------------------------------------------------------------- buyer
void InvoiceEditor::pickCustomer(int index) {
    if (updating_ || index < 0) return;
    const qlonglong id = customer_->itemData(index).toLongLong();
    if (id == 0) {
        // Otherwise the removed customer stays on the document and the regime
        // keeps pointing at a buyer that is no longer selected.
        inv_.buyer = Customer();
        buyerInfo_->clear();
        applySupplyRegime();
        recalcTotals();
        return;
    }

    Customer c;
    if (db_.loadCustomer(id, c)) {
        inv_.buyer = c;
        updateBuyerInfo();
        applySupplyRegime();
        recalcTotals();
    }
}

void InvoiceEditor::updateBuyerInfo() {
    if (inv_.buyer.name.empty()) { buyerInfo_->clear(); return; }
    QString ids;
    if (!inv_.buyer.ico.empty())   ids += "IČO " + qstr(inv_.buyer.ico).toHtmlEscaped();
    if (!inv_.buyer.icDph.empty()) ids += (ids.isEmpty() ? "" : " · ") +
                                          QString("IČ DPH ") + qstr(inv_.buyer.icDph).toHtmlEscaped();
    buyerInfo_->setText(QString("<b>%1</b><br/>%2<br/>%3 %4<br/><span style='color:#777'>%5</span>")
        .arg(qstr(inv_.buyer.name).toHtmlEscaped(),
             qstr(inv_.buyer.address.street).toHtmlEscaped(),
             qstr(inv_.buyer.address.postalCode).toHtmlEscaped(),
             qstr(inv_.buyer.address.city).toHtmlEscaped(),
             ids));
}

void InvoiceEditor::editBuyerDetails() {
    QDialog dlg(this);
    dlg.setWindowTitle("Údaje odberateľa");
    dlg.resize(480, 640);

    auto* form = new PartyForm(&dlg);
    form->setDatabase(&db_);
    form->load(inv_.buyer);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto* layout = new QVBoxLayout(&dlg);
    layout->addWidget(form);
    layout->addWidget(box);

    if (dlg.exec() != QDialog::Accepted) return;

    const int64_t keepId = inv_.buyer.id;
    form->applyTo(inv_.buyer);
    inv_.buyer.id = keepId;
    updateBuyerInfo();
    applySupplyRegime();
    recalcTotals();
}

// -------------------------------------------------------------- validation
void InvoiceEditor::runValidation() {
    collectToModel();
    ValidationResult r = validate(inv_, inv_.seller);
    if (r.empty()) {
        QMessageBox::information(this, "Kontrola", "Faktúra je v poriadku.");
        return;
    }
    QString text;
    for (const Issue& i : r.issues)
        text += QString("%1 [%2] %3\n")
                    .arg(i.severity == Severity::Error ? "CHYBA " : "Pozor ")
                    .arg(qstr(i.code), qstr(i.message));
    QMessageBox box(this);
    box.setWindowTitle("Kontrola");
    box.setIcon(r.ok() ? QMessageBox::Warning : QMessageBox::Critical);
    box.setText(r.ok() ? "Faktúru je možné odoslať, ale skontrolujte upozornenia:"
                       : "Faktúra obsahuje chyby:");
    box.setDetailedText(text);
    box.exec();
}

void InvoiceEditor::accept() {
    if (!inv_.isEditable()) { QDialog::reject(); return; }
    collectToModel();

    if (inv_.number.empty()) {
        QMessageBox::warning(this, "Chýba číslo", "Zadajte číslo dokladu.");
        return;
    }
    if (db_.invoiceNumberExists(inv_.number, inv_.id)) {
        QMessageBox::warning(this, "Duplicitné číslo",
                             "Doklad s číslom " + qstr(inv_.number) + " už existuje.");
        return;
    }
    if (inv_.lines.empty()) {
        QMessageBox::warning(this, "Prázdny doklad", "Pridajte aspoň jednu položku.");
        return;
    }

    ValidationResult r = validate(inv_, inv_.seller);
    if (!r.ok()) {
        QString text;
        for (const Issue& i : r.issues)
            if (i.severity == Severity::Error)
                text += "• " + qstr(i.message) + "\n";
        auto answer = QMessageBox::warning(
            this, "Faktúra nie je kompletná",
            text + "\nUložiť ako rozpracovanú? Export do Peppolu nebude možný.",
            QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Save) return;
    }
    QDialog::accept();
}

} // namespace fk
