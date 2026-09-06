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

#include "SapiConfig.h"

#include "../country/Country.h"
#include "../db/Database.h"
#include "../model/Model.h"

#include <cctype>

namespace fk {
namespace sapi {
namespace {

std::string trimmed(const std::string& text) {
    size_t a = 0, b = text.size();
    while (a < b && std::isspace(static_cast<unsigned char>(text[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(text[b - 1]))) --b;
    return text.substr(a, b - a);
}

/// A trailing slash on the host and a leading one on the path would give
/// "…/v1//auth/token", which some gateways answer with a 404 and others with a
/// redirect that drops the Authorization header. Neither failure names itself.
std::string withoutTrailingSlash(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

const char* environmentWord(Environment environment) {
    return environment == Environment::Live ? "live" : "sandbox";
}

} // namespace

std::string normalisedHost(const std::string& host) {
    return withoutTrailingSlash(trimmed(host));
}

std::string effectiveHost(const Config& config) {
    const std::string host = normalisedHost(config.host);
    return host.empty() ? baseUrl(config.environment) : host;
}

Credentials credentialsFor(const Config& config) {
    Credentials c;
    c.environment   = config.environment;
    c.clientId      = trimmed(config.clientId);
    c.participantId = trimmed(config.participantId);
    return c;
}

std::string defaultParticipantId(const Company& company) {
    return peppolParticipant(company).full();
}

std::string participantProblem(const std::string& participantId) {
    const std::string id = trimmed(participantId);
    if (id.empty()) return "Zadajte identifikátor účastníka.";

    const size_t colon = id.find(':');
    if (colon == std::string::npos)
        return "Identifikátor má tvar 0245:DIČ, napríklad 0245:2120345678.";

    const std::string scheme = id.substr(0, colon);
    const std::string value  = id.substr(colon + 1);
    if (value.empty()) return "Za dvojbodkou chýba číslo.";

    // 0158:IČO and 9950:SK… do not resolve on the Slovak network. Saying so
    // here is cheaper than a document handed to a network that cannot find the
    // recipient, which is what happens instead — and it looks like success.
    if (scheme == "0158")
        return "0158:IČO sa v slovenskom Peppole nesmeruje. Použite 0245:DIČ.";
    if (scheme == "9950")
        return "9950:SK… sa v slovenskom Peppole nesmeruje. Použite 0245:DIČ.";
    // 0060 (DUNS) and 9908 (Norwegian organisation number) are real Peppol
    // schemes and a provider may have issued one, so they are let through
    // unchecked rather than refused — the rule this function exists to enforce
    // is the Slovak one, and second-guessing an identifier the user was given
    // would be worse than trusting it. Anything else is a typo.
    if (scheme != "0245" && scheme != "0060" && scheme != "9908")
        return "Neznáma schéma „" + scheme + "“. Slovenská je 0245.";

    if (scheme == "0245") {
        for (char c : value)
            if (!std::isdigit(static_cast<unsigned char>(c)))
                return "DIČ v 0245 má obsahovať iba číslice.";
        if (value.size() != 10) return "Slovenské DIČ má 10 číslic.";
    }
    return {};
}

const char* keychainService() { return "sk.studio202.fakturo.sapi"; }

std::string keychainAccount(int64_t companyId, const Config& config) {
    return std::string("sapi:") + environmentWord(config.environment) + ":" +
           std::to_string(companyId) + ":" + trimmed(config.clientId);
}

// --------------------------------------------------------------- persistence

std::string configKey(int64_t companyId, const std::string& field) {
    return "sapi." + std::to_string(companyId) + "." + field;
}

Config loadConfig(Database& db, int64_t companyId) {
    Config c;
    c.enabled       = db.setting(configKey(companyId, "enabled"), "0") == "1";
    c.environment   = db.setting(configKey(companyId, "environment"), "sandbox") == "live"
                          ? Environment::Live
                          : Environment::Sandbox;
    c.host          = db.setting(configKey(companyId, "host"));
    c.clientId      = db.setting(configKey(companyId, "clientId"));
    c.participantId = db.setting(configKey(companyId, "participantId"));
    return c;
}

bool saveConfig(Database& db, int64_t companyId, const Config& config) {
    // A half-written config is worse than none: it would be enabled against
    // one environment with the other one's client id, and the screen would say
    // the save failed while the database held the mixture. Five separate
    // UPDATEs cannot promise that on their own, so they run in a transaction.
    if (!db.beginTransaction()) return false;

    bool ok = true;
    ok = db.setSetting(configKey(companyId, "enabled"), config.enabled ? "1" : "0") && ok;
    ok = db.setSetting(configKey(companyId, "environment"),
                       environmentWord(config.environment)) && ok;
    ok = db.setSetting(configKey(companyId, "host"), normalisedHost(config.host)) && ok;
    ok = db.setSetting(configKey(companyId, "clientId"), trimmed(config.clientId)) && ok;
    ok = db.setSetting(configKey(companyId, "participantId"),
                       trimmed(config.participantId)) && ok;

    if (!ok) { db.rollback(); return false; }
    // A COMMIT that fails (SQLITE_BUSY) leaves the transaction open, and every
    // later BEGIN on this connection would then fail forever. Close it.
    if (db.commit()) return true;
    db.rollback();
    return false;
}

std::string describe(const Config& config) {
    std::string out = std::string(config.enabled ? "zapnuté" : "vypnuté") + " · " +
                      environmentWord(config.environment) + " · " + effectiveHost(config);
    if (!config.clientId.empty())   out += " · " + trimmed(config.clientId);
    if (!config.participantId.empty()) out += " · " + trimmed(config.participantId);
    return out;
}

} // namespace sapi
} // namespace fk
