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

#include "Sapi.h"

#include "../core/Json.h"
#include "../core/Sha256.h"
#include "../country/Country.h"
#include "../ubl/UblWriter.h"

#include <algorithm>

namespace fk {
namespace sapi {
namespace {

std::string join(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out = "{";
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) out += ",";
        out += jsonQuote(fields[i].first) + ":" + jsonQuote(fields[i].second);
    }
    return out + "}";
}

std::vector<std::pair<std::string, std::string>> authHeaders(const Credentials& credentials,
                                                             const Token& token) {
    return {{"Authorization", "Bearer " + token.accessToken},
            {"X-Peppol-Participant-Id", credentials.participantId},
            {"Content-Type", "application/json"}};
}

/// Percent-encodes a path segment. A document id is a UUID today, but it is
/// somebody else's identifier and putting it into a URL unescaped is how a
/// path traversal starts.
std::string urlSegment(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        const bool safe = std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
        if (safe) out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        }
    }
    return out;
}

} // namespace

std::string baseUrl(Environment environment) {
    // Their sandbox credentials fail against the live host on purpose, and the
    // live ones against the sandbox. That is a good design, and it is why the
    // two hosts are named here rather than assembled from a flag somewhere.
    return environment == Environment::Live ? "https://epostak.sk/sapi/v1"
                                            : "https://dev.epostak.sk/sapi/v1";
}

// ------------------------------------------------------------------- tokens

Request tokenRequest(const Credentials& credentials, const std::string& secret,
                     const std::string& scope) {
    std::vector<std::pair<std::string, std::string>> fields = {
        {"grant_type", "client_credentials"},
        {"client_id", credentials.clientId},
        {"client_secret", secret}};
    if (!scope.empty()) fields.push_back({"scope", scope});

    Request r;
    r.method  = "POST";
    r.url     = baseUrl(credentials.environment) + "/auth/token";
    r.headers = {{"Content-Type", "application/json"}};
    r.body    = join(fields);
    return r;
}

Request renewRequest(const Credentials& credentials, const std::string& refreshToken) {
    Request r;
    r.method  = "POST";
    r.url     = baseUrl(credentials.environment) + "/auth/renew";
    r.headers = {{"Content-Type", "application/json"}};
    r.body    = join({{"grant_type", "refresh_token"}, {"refresh_token", refreshToken}});
    return r;
}

Request revokeRequest(const Credentials& credentials, const std::string& token) {
    Request r;
    r.method  = "POST";
    r.url     = baseUrl(credentials.environment) + "/auth/revoke";
    r.headers = {{"Content-Type", "application/json"}};
    r.body    = join({{"token", token}});
    return r;
}

Request tokenStatusRequest(const Credentials& credentials, const Token& token) {
    Request r;
    r.method  = "GET";
    r.url     = baseUrl(credentials.environment) + "/auth/token/status";
    r.headers = {{"Authorization", "Bearer " + token.accessToken}};
    return r;
}

bool readToken(const std::string& json, Token& out, std::string* error) {
    const JsonDocument doc = parseJson(json);
    if (!doc.ok) { if (error) *error = doc.error; return false; }

    out = Token{};
    out.accessToken  = doc.root.stringAt("access_token");
    out.refreshToken = doc.root.stringAt("refresh_token");
    out.expiresIn    = static_cast<int>(doc.root.intAt("expires_in", 0));
    out.scope        = doc.root.stringAt("scope");

    if (out.accessToken.empty()) {
        if (error) *error = "Odpoveď neobsahuje access_token.";
        return false;
    }
    return true;
}

bool shouldRenew(const Token& token, int ageSeconds) {
    if (!token.valid()) return true;
    // A minute of margin. Their own status endpoint recommends renewing about
    // three minutes out; a minute is the least that is safe, and the cost of
    // renewing early is one cheap call against the cost of a send that fails
    // half way through and leaves you not knowing whether it went.
    const int life = token.expiresIn > 0 ? token.expiresIn : 900;
    return ageSeconds >= life - 60;
}

// ---------------------------------------------------------------- documents

std::string documentTypeId(DocType type) {
    if (!canExportToPeppol(type)) return {};
    const bool creditNote = type == DocType::CreditNote;
    const std::string root = creditNote ? "CreditNote" : "Invoice";
    return "urn:oasis:names:specification:ubl:schema:xsd:" + root + "-2::" + root +
           "##" + std::string(fk::peppol::CUSTOMIZATION_ID) + "::2.1";
}

std::string processId() { return fk::peppol::PROFILE_ID; }

SendEnvelope envelopeFor(const Invoice& invoice, const Company& seller,
                         const std::string& ubl, const std::string& nowIso) {
    SendEnvelope e;
    e.documentId            = invoice.number;
    e.documentTypeId        = documentTypeId(invoice.type);
    e.processId             = processId();
    e.senderParticipantId   = peppolParticipant(seller).full();
    e.receiverParticipantId = peppolParticipant(invoice.buyer).full();
    e.creationDateTime      = nowIso;
    e.payload               = ubl;
    // Optional in the standard, sent anyway: it costs one hash and it is the
    // only way either side can tell a truncated payload from a short one.
    e.checksum              = ubl.empty() ? std::string() : Sha256::hexOf(ubl);
    return e;
}

std::string sendBody(const SendEnvelope& envelope) {
    std::string out = "{\"metadata\":";
    out += join({{"documentId", envelope.documentId},
                 {"documentTypeId", envelope.documentTypeId},
                 {"processId", envelope.processId},
                 {"senderParticipantId", envelope.senderParticipantId},
                 {"receiverParticipantId", envelope.receiverParticipantId},
                 {"creationDateTime", envelope.creationDateTime}});
    out += ",\"payload\":" + jsonQuote(envelope.payload);
    out += ",\"payloadFormat\":\"XML\",\"payloadEncoding\":\"UTF-8\"";
    if (!envelope.checksum.empty()) out += ",\"checksum\":" + jsonQuote(envelope.checksum);
    out += "}";
    return out;
}

Request sendRequest(const Credentials& credentials, const Token& token,
                    const SendEnvelope& envelope, const std::string& idempotencyKey) {
    Request r;
    r.method  = "POST";
    r.url     = baseUrl(credentials.environment) + "/document/send";
    r.headers = authHeaders(credentials, token);
    // The one header that makes a retry safe. Same document, same key, or the
    // retry is a second invoice.
    r.headers.push_back({"Idempotency-Key", idempotencyKey});
    r.body    = sendBody(envelope);
    return r;
}

Request receiveListRequest(const Credentials& credentials, const Token& token,
                           int limit, const std::string& pageToken) {
    Request r;
    r.method = "GET";
    r.url    = baseUrl(credentials.environment) + "/document/receive?status=RECEIVED&limit=" +
               std::to_string(std::min(std::max(limit, 1), 100));
    if (!pageToken.empty()) r.url += "&pageToken=" + urlSegment(pageToken);
    r.headers = authHeaders(credentials, token);
    return r;
}

Request receiveDetailRequest(const Credentials& credentials, const Token& token,
                             const std::string& documentId) {
    Request r;
    r.method  = "GET";
    r.url     = baseUrl(credentials.environment) + "/document/receive/" + urlSegment(documentId);
    r.headers = authHeaders(credentials, token);
    return r;
}

Request acknowledgeRequest(const Credentials& credentials, const Token& token,
                           const std::string& documentId) {
    Request r;
    r.method  = "POST";
    r.url     = baseUrl(credentials.environment) + "/document/receive/" +
                urlSegment(documentId) + "/acknowledge";
    r.headers = authHeaders(credentials, token);
    return r;
}

bool readInboxPage(const std::string& json, InboxPage& out, std::string* error) {
    const JsonDocument doc = parseJson(json);
    if (!doc.ok) { if (error) *error = doc.error; return false; }

    out = InboxPage{};
    const JsonValue& documents = doc.root.at("documents");
    if (!documents.isArray()) {
        if (error) *error = "Odpoveď neobsahuje zoznam dokumentov.";
        return false;
    }
    for (const JsonValue& item : documents.items) {
        InboxEntry entry;
        entry.documentId            = item.stringAt("documentId");
        entry.documentTypeId        = item.stringAt("documentTypeId");
        entry.senderParticipantId   = item.stringAt("senderParticipantId");
        entry.receiverParticipantId = item.stringAt("receiverParticipantId");
        entry.creationDateTime      = item.stringAt("creationDateTime");
        // An entry with no id cannot be fetched or acknowledged, and keeping
        // it would mean a list that never empties.
        if (entry.documentId.empty()) continue;
        out.documents.push_back(std::move(entry));
    }
    out.nextPageToken = doc.root.stringAt("nextPageToken");
    return true;
}

bool readDocumentPayload(const std::string& json, std::string& ubl, std::string* error) {
    const JsonDocument doc = parseJson(json);
    if (!doc.ok) { if (error) *error = doc.error; return false; }

    const std::string format = doc.root.stringAt("payloadFormat");
    if (!format.empty() && format != "XML") {
        if (error) *error = "Neočakávaný formát dokumentu: " + format;
        return false;
    }
    ubl = doc.root.stringAt("payload");
    if (ubl.empty()) {
        if (error) *error = "Odpoveď neobsahuje dokument.";
        return false;
    }
    return true;
}

// ------------------------------------------------------------------- errors

Failure classify(const Response& response) {
    Failure f;

    // Never arrived. Not knowing is not the same as being refused: the request
    // may have been received and answered into a socket that had gone.
    if (response.status == 0) {
        f.outcome = Outcome::Retry;
        f.message = response.transportError.empty()
                        ? "Spojenie so službou zlyhalo."
                        : "Spojenie so službou zlyhalo: " + response.transportError;
        return f;
    }

    const JsonDocument doc = parseJson(response.body);
    if (doc.ok) {
        f.code      = doc.root.stringAt("error/code");
        f.message   = doc.root.stringAt("error/message");
        f.requestId = doc.root.stringAt("error/requestId");
    }

    if (response.status >= 200 && response.status < 300) { f.outcome = Outcome::Ok; return f; }

    switch (response.status) {
        case 401: f.outcome = Outcome::Reauthenticate; break;
        case 409: f.outcome = Outcome::Duplicate;      break;
        case 429:
        case 503: f.outcome = Outcome::Retry;          break;
        default:
            f.outcome = response.status >= 500 ? Outcome::Retry : Outcome::Refused;
            break;
    }

    if (f.message.empty()) {
        switch (response.status) {
            case 400: f.message = "Služba odmietla požiadavku ako neplatnú."; break;
            case 401: f.message = "Prihlásenie vypršalo alebo je neplatné.";  break;
            case 403: f.message = "Token nepatrí k tomuto Peppol ID, alebo chýba oprávnenie.";
                      break;
            case 404: f.message = "Dokument sa nenašiel.";                     break;
            case 409: f.message = "Rovnaký dokument sa práve odosiela alebo už bol odoslaný.";
                      break;
            case 422: f.message = "Príjemca nie je v sieti Peppol, alebo doklad neprešiel "
                                  "kontrolou.";                                break;
            case 423: f.message = "Účet je dočasne zablokovaný.";              break;
            case 429: f.message = "Príliš veľa požiadaviek, skúste o chvíľu."; break;
            case 503: f.message = "Služba je dočasne nedostupná.";             break;
            default:  f.message = "Služba vrátila stav " + std::to_string(response.status) + ".";
        }
    }
    return f;
}

int backoffSeconds(int attempt) {
    if (attempt < 1) attempt = 1;
    if (attempt > 6) attempt = 6;             // 2, 4, 8, 16, 32, 64
    int seconds = 1;
    for (int i = 0; i < attempt; ++i) seconds *= 2;
    return seconds;
}

} // namespace sapi
} // namespace fk
