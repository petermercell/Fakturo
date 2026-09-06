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

#include "RegistersPage.h"

#include "../registry/RegistryClient.h"
#include "../registry/VatRegister.h"
#include "../sk/Slovak.h"
#include "GuiUtil.h"

#include <QFormLayout>
#include <QFuture>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkReply>
#include <QProgressDialog>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace fk {

using namespace fk::gui;

RegistersPage::RegistersPage(Database& db, QWidget* parent) : QWidget(parent), db_(db) {
    client_ = std::make_unique<RegistryClient>(this);

    vatInfo_   = new QLabel(this);
    taxInfo_   = new QLabel(this);
    vatUpdate_ = new QPushButton("Stiahnuť register DPH", this);
    taxUpdate_ = new QPushButton("Stiahnuť register daňových subjektov", this);

    for (QLabel* l : {vatInfo_, taxInfo_}) {
        l->setWordWrap(true);
        // A wrapping label inside a QFormLayout computes its height before it
        // knows its width, and ends up clipped without a floor.
        l->setMinimumHeight(l->fontMetrics().height() * 2);
    }
    vatUpdate_->setToolTip("Denný zoznam platiteľov DPH, približne 120 MB");
    taxUpdate_->setToolTip("Denný zoznam subjektov registrovaných na daň z príjmov, "
                           "približne 360 MB. Potrebný na doplnenie DIČ neplatiteľov DPH.");

    auto* skBox = new QGroupBox("Slovensko – Finančná správa", this);
    auto* fs = new QFormLayout(skBox);
    auto* skNote = new QLabel(
        "Slovenské registre sa sťahujú ako celok. Bez nich sa IČ DPH a DIČ "
        "zadávajú ručne; názov a adresa sa načítajú z RPO aj bez nich.", this);
    skNote->setWordWrap(true);
    fs->addRow(skNote);
    fs->addRow("Platitelia DPH", vatInfo_);
    fs->addRow("", vatUpdate_);
    fs->addRow("Daňové subjekty (DIČ)", taxInfo_);
    fs->addRow("", taxUpdate_);

    auto* czBox = new QGroupBox("Česko – ARES", this);
    auto* fc = new QVBoxLayout(czBox);
    auto* czNote = new QLabel(
        "Nič sa nesťahuje. ARES odpovedá naživo a na jedno volanie vráti názov, "
        "adresu, DIČ, či je subjekt plátcom DPH aj zápis v obchodnom registri. "
        "Stačí zadať IČO a stlačiť Načítať.", this);
    czNote->setWordWrap(true);
    fc->addWidget(czNote);

    auto* root = new QVBoxLayout(this);
    root->addWidget(skBox);
    root->addWidget(czBox);
    root->addStretch();

    connect(vatUpdate_, &QPushButton::clicked, this,
            [this] { downloadRegister(RegisterKind::Vat); });
    connect(taxUpdate_, &QPushButton::clicked, this,
            [this] { downloadRegister(RegisterKind::IncomeTax); });

    refresh();
}

RegistersPage::~RegistersPage() {
    // Ask a running import to stop. It cannot be waited for here without
    // freezing the shutdown, which is why the shared state is a shared_ptr:
    // the worker finishes its rollback safely on its own.
    cancel_->store(true);
}

void RegistersPage::refresh() {
    if (busy_) return;      // mid-import counts are uncommitted and misleading

    auto describe = [](QLabel* label, int count, const std::string& updated,
                       const QString& emptyText) {
        if (count == 0) { label->setText(emptyText); return; }
        const QString when = updated.empty() ? QString("neznámy dátum")
                                             : qstr(sk::formatDateSk(updated));
        label->setText(QString("%1 subjektov, k %2.").arg(QString::number(count), when));
    };

    describe(vatInfo_, db_.vatRegisterCount(), db_.vatRegisterUpdated(),
             "Nie je stiahnutý.");
    describe(taxInfo_, db_.taxRegisterCount(), db_.taxRegisterUpdated(),
             "Nie je stiahnutý. Potrebný pre DIČ subjektov, ktoré nie sú platiteľmi DPH.");
}

void RegistersPage::downloadRegister(RegisterKind kind) {
    if (busy_) return;
    busy_ = true;
    kind_ = kind;
    cancel_->store(false);
    rows_->store(0);
    vatUpdate_->setEnabled(false);
    taxUpdate_->setEnabled(false);

    const QString what = (kind == RegisterKind::Vat) ? "register DPH"
                                                     : "register daňových subjektov";

    dialog_ = new QProgressDialog("Sťahujem " + what + "…", "Zrušiť", 0, 100, this);
    dialog_->setWindowTitle("Registre finančnej správy");
    dialog_->setWindowModality(Qt::WindowModal);
    dialog_->setMinimumDuration(0);
    dialog_->setAutoClose(false);
    dialog_->setAutoReset(false);
    dialog_->setValue(0);

    QPointer<RegistersPage> self(this);
    QPointer<QNetworkReply> reply = client_->downloadRegister(kind, this,
        [self, what](qint64 received, qint64 total) {
            if (!self || !self->dialog_ || self->dialog_->wasCanceled()) return;
            self->dialog_->setLabelText(QString("Sťahujem %1… %2 MB")
                .arg(what, QString::number(received / 1048576.0, 'f', 1)));
            if (total > 0) self->dialog_->setValue(static_cast<int>(received * 100 / total));
        },
        [self, kind](const QByteArray& zip, const QString& error) {
            if (!self) return;
            const bool canceled = self->dialog_ && self->dialog_->wasCanceled();
            if (canceled || !error.isEmpty()) {
                self->finishRun();
                if (!canceled) QMessageBox::critical(self, "Registre finančnej správy", error);
                return;
            }
            if (self->dialog_) {
                self->dialog_->setLabelText("Importujem…");
                self->dialog_->setRange(0, 0);        // indeterminate while parsing
            }
            self->runImport(kind, zip);               // the worker thread takes over
        });

    // Cancel must stop the work, not merely hide the dialog. The reply is
    // deleted as soon as the download finishes while the dialog lives on
    // through the import, so this has to be a QPointer.
    connect(dialog_.data(), &QProgressDialog::canceled, this, [this, reply] {
        *cancel_ = true;
        if (!reply.isNull()) reply->abort();
        // QProgressDialog hides itself on cancel, but rolling back a 360 MB
        // import takes seconds. Keep it on screen so the app does not look hung.
        if (dialog_) {
            dialog_->setLabelText("Ruším import…");
            dialog_->show();
        }
    });
}

void RegistersPage::finishRun() {
    if (dialog_) {
        // QProgressDialog::closeEvent() emits canceled(). Closing after a
        // *successful* run would fire the cancel handler, which is both wrong
        // and — the reply being long gone by then — a crash.
        dialog_->disconnect();
        dialog_->close();
        dialog_->deleteLater();
        dialog_.clear();
    }
    busy_ = false;
    vatUpdate_->setEnabled(true);
    taxUpdate_->setEnabled(true);
    refresh();
}

void RegistersPage::runImport(RegisterKind kind, const QByteArray& zip) {
    if (zip.isEmpty()) {
        finishRun();
        QMessageBox::critical(this, "Registre finančnej správy", "Stiahnutý súbor je prázdny.");
        return;
    }

    const std::string bytes(zip.constData(), static_cast<size_t>(zip.size()));
    const std::string dbPath = db_.path();

    // Hundreds of thousands of rows in one write transaction. On the GUI thread
    // with processEvents() the user could edit and save inside that
    // transaction, so it gets its own thread and its own connection.
    //
    // The counters are captured by value as shared_ptr: if the window is closed
    // while the import is still running, the task keeps them alive instead of
    // writing into freed memory.
    auto rows   = rows_;
    auto cancel = cancel_;

    QFuture<ImportResult> future = QtConcurrent::run([bytes, dbPath, kind, rows, cancel] {
        ImportResult result;
        Database worker;
        if (!worker.open(dbPath)) {
            result.error = worker.lastError();
            return result;
        }
        auto onProgress = [rows, cancel](int n) {
            rows->store(n);
            return !cancel->load();
        };
        result = (kind == RegisterKind::Vat)
                     ? importVatRegisterZip(bytes, worker, onProgress)
                     : importTaxRegisterZip(bytes, worker, onProgress);
        worker.close();
        return result;
    });

    // Poll the shared counter rather than touching widgets from the worker.
    QPointer<RegistersPage> self(this);
    auto* watcher = new QFutureWatcher<ImportResult>(this);
    auto* ticker  = new QTimer(this);
    ticker->setInterval(200);

    connect(ticker, &QTimer::timeout, this, [self] {
        if (!self || !self->dialog_) return;
        self->dialog_->setLabelText(QString("Importujem… %1 subjektov")
                                        .arg(QString::number(self->rows_->load())));
    });
    connect(watcher, &QFutureWatcher<ImportResult>::finished, this, [this, watcher, ticker] {
        ticker->stop();
        ticker->deleteLater();

        const ImportResult result = watcher->result();
        watcher->deleteLater();
        finishRun();

        if (result.canceled) return;
        if (!result.ok) {
            QMessageBox::critical(this, "Registre finančnej správy",
                "Import zlyhal, pôvodné údaje zostali nezmenené.\n\n" + qstr(result.error));
            return;
        }
        QMessageBox::information(this, "Registre finančnej správy",
            QString("Načítaných %1 subjektov.").arg(QString::number(result.imported)));
    });

    ticker->start();
    watcher->setFuture(future);
}

} // namespace fk
