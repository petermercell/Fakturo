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

// DashboardPage.h - the first thing you see.
//
// Answers the two questions the app gets opened for: what am I owed, and how
// did this month go. Every figure comes from `summarise()` in the core, so
// this file only arranges labels — the arithmetic is tested elsewhere.
#pragma once

#include "../db/Database.h"

#include <QWidget>

#include <functional>

class QLabel;
class QVBoxLayout;

namespace fk {

class DashboardPage : public QWidget {
public:
    explicit DashboardPage(Database& db, QWidget* parent = nullptr);

    /// Re-reads the invoice list. Called whenever documents may have changed.
    void reload();

    /// Asked to show the invoice list, filtered — "all", "unpaid" or "overdue".
    /// The window sets this; the page does not know what a tab is.
    std::function<void(const QString& filterMode)> onShowInvoices;
    /// The same for the other direction: opens Prijaté with a filter chosen.
    std::function<void(const QString& filterMode)> onShowPayables;

private:
    void rebuild();

    Database& db_;
    QWidget*  body_   = nullptr;
    QVBoxLayout* bodyLayout_ = nullptr;
};

} // namespace fk
