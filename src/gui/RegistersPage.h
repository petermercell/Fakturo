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

// RegistersPage.h - the "Registre" tab: local copies of the Slovak bulk
// datasets, and an explanation of why Czechia needs none.
#pragma once

#include "../db/Database.h"

#include <QPointer>
#include <QWidget>

#include <atomic>
#include <memory>

class QLabel;
class QProgressDialog;
class QPushButton;

namespace fk {

class RegistryClient;

class RegistersPage : public QWidget {
public:
    RegistersPage(Database& db, QWidget* parent = nullptr);
    ~RegistersPage() override;

    void refresh();

    /// True while an import is running. The window refuses to close then.
    bool isBusy() const { return busy_; }

private:
    void downloadRegister(RegisterKind kind);
    void runImport(RegisterKind kind, const QByteArray& zip);
    void finishRun();

    Database&    db_;
    QLabel*      vatInfo_   = nullptr;
    QPushButton* vatUpdate_ = nullptr;
    QLabel*      taxInfo_   = nullptr;
    QPushButton* taxUpdate_ = nullptr;

    std::unique_ptr<RegistryClient> client_;
    QPointer<QProgressDialog>       dialog_;

    // The import runs on a worker thread with its own database connection.
    // These two are the only things the threads share, and they are shared_ptr
    // rather than members-by-address: QFutureWatcher does not wait for the
    // future in its destructor, so the task can outlive this widget.
    bool         busy_ = false;
    RegisterKind kind_ = RegisterKind::Vat;
    std::shared_ptr<std::atomic<int>>  rows_   = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
};

} // namespace fk
