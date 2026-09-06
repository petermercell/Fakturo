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

#include "Slovak.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace fk::sk {
namespace {

std::string stripped(const std::string& s) {
    std::string out;
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c)) && c != '-' && c != '/')
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return out;
}

bool allDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(),
                                     [](char c) { return c >= '0' && c <= '9'; });
}

} // namespace

bool validIco(const std::string& ico) {
    std::string s = stripped(ico);
    if (s.size() != 8 || !allDigits(s)) return false;
    int sum = 0;
    for (int i = 0; i < 7; ++i) sum += (s[i] - '0') * (8 - i);
    int check = (11 - sum % 11) % 10;
    return check == (s[7] - '0');
}

bool validIcDph(const std::string& icDph) {
    std::string s = stripped(icDph);
    if (s.size() != 12 || s.compare(0, 2, "SK") != 0) return false;
    std::string d = s.substr(2);
    if (!allDigits(d)) return false;
    if (d[0] == '0') return false;
    if (d[2] != '2' && d[2] != '3' && d[2] != '4' && d[2] != '7' &&
        d[2] != '8' && d[2] != '9') return false;
    long long n = 0;
    for (char c : d) n = n * 10 + (c - '0');
    return n % 11 == 0;
}

bool validDic(const std::string& dic) {
    std::string s = stripped(dic);
    return s.size() == 10 && allDigits(s);
}

std::string normalizeIban(const std::string& iban) { return stripped(iban); }

bool validIban(const std::string& iban) {
    std::string s = normalizeIban(iban);
    if (s.size() < 15 || s.size() > 34) return false;
    if (!std::isalpha(static_cast<unsigned char>(s[0])) ||
        !std::isalpha(static_cast<unsigned char>(s[1]))) return false;

    std::string rearranged = s.substr(4) + s.substr(0, 4);
    long long remainder = 0;
    for (char c : rearranged) {
        int value;
        if (c >= '0' && c <= '9')      value = c - '0';
        else if (c >= 'A' && c <= 'Z') value = c - 'A' + 10;
        else return false;
        remainder = (value > 9) ? (remainder * 100 + value) : (remainder * 10 + value);
        remainder %= 97;
    }
    return remainder == 1;
}

std::string formatIban(const std::string& iban) {
    std::string s = normalizeIban(iban), out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i && i % 4 == 0) out.push_back(' ');
        out.push_back(s[i]);
    }
    return out;
}

std::string variableSymbolFrom(const std::string& invoiceNumber) {
    std::string d;
    for (char c : invoiceNumber)
        if (c >= '0' && c <= '9') d.push_back(c);
    if (d.size() > 10) d = d.substr(d.size() - 10);
    return d;
}

bool validIsoDate(const std::string& iso) {
    if (iso.size() != 10 || iso[4] != '-' || iso[7] != '-') return false;
    for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u})
        if (iso[i] < '0' || iso[i] > '9') return false;
    int m = (iso[5] - '0') * 10 + (iso[6] - '0');
    int d = (iso[8] - '0') * 10 + (iso[9] - '0');
    return m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

std::string todayIso() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

std::string nowTimestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    // Wide enough for the compiler to prove no truncation: six %d fields can
    // in principle print more digits than a date ever will.
    char buf[80];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string addDays(const std::string& iso, int days) {
    if (!validIsoDate(iso)) return iso;
    std::tm tm{};
    tm.tm_year = (iso[0]-'0')*1000 + (iso[1]-'0')*100 + (iso[2]-'0')*10 + (iso[3]-'0') - 1900;
    tm.tm_mon  = (iso[5]-'0')*10 + (iso[6]-'0') - 1;
    tm.tm_mday = (iso[8]-'0')*10 + (iso[9]-'0') + days;
    tm.tm_hour = 12;
    std::mktime(&tm);   // normalises the overflowed day-of-month
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

std::string formatDateSk(const std::string& iso) {
    if (!validIsoDate(iso)) return iso;
    return iso.substr(8, 2) + "." + iso.substr(5, 2) + "." + iso.substr(0, 4);
}

} // namespace fk::sk
