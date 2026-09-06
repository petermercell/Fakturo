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

// GuiUtil.h - tiny conversion helpers shared by the Qt widgets.
#pragma once

#include "../core/Dec.h"

#include <QLineEdit>
#include <QString>

namespace fk::gui {

inline QString qstr(const std::string& s) { return QString::fromStdString(s); }
inline std::string sstr(const QString& s) { return s.trimmed().toStdString(); }

/// Slovak display: comma decimal separator.
inline QString decToUi(Dec v, int dp = 2) {
    return QString::fromStdString(v.toString(dp)).replace('.', ',');
}

/// Accepts "1234,56", "1 234.56", "" -> 0. Invalid input yields `fallback`.
inline Dec decFromUi(const QString& s, Dec fallback = Dec()) {
    auto d = Dec::parse(s.trimmed().toStdString());
    return d ? *d : fallback;
}

inline void setLine(QLineEdit* e, const std::string& v) { e->setText(qstr(v)); }
inline std::string lineText(const QLineEdit* e) { return sstr(e->text()); }

} // namespace fk::gui
