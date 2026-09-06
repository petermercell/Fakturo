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

#include "db/Database.h"
#include "gui/MainWindow.h"

#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("Fakturo");
    QCoreApplication::setOrganizationDomain("fakturo.eu");
    QCoreApplication::setApplicationName("Fakturo");

    // ~/Library/Application Support/Fakturo on macOS.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    const QString dbPath = dir + "/fakturo.db";

    fk::Database db;
    if (!db.open(dbPath.toStdString())) {
        QMessageBox::critical(nullptr, "Fakturo",
            "Databázu sa nepodarilo otvoriť:\n" + dbPath + "\n\n" +
            QString::fromStdString(db.lastError()));
        return 1;
    }

    fk::MainWindow window(db);
    window.show();

    // The backups people actually have are the ones they never had to remember.
    // One per day, ten kept, next to the database, without the downloaded
    // registers. After show(), so a slow disk never delays the first window,
    // and not fatal: a full disk must not stop the app from opening.
    db.autoBackup(10);
    return app.exec();
}
