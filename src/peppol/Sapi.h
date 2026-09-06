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

// Sapi.h - SAPI-SK 1.0, the Slovak Access Point interface.
//
// SAPI is an open standard the Slovak Peppol Access Point operators agreed on
// so that an integration is not tied to one provider: the same eight calls
// work against any of them. That is the reason this file is written against
// the standard rather than against ePošťák, and the reason the host is a
// setting rather than a constant.
//
// **No Qt and no network here.** This layer decides *what* to send and what a
// reply means; a thin transport above it does the talking. Everything that can
// be got wrong — the envelope, the identifiers, which failures are worth
// retrying, when a token has to be renewed — is therefore testable without a
// server, which matters because the server is somebody else's and the mistakes
// are expensive: a duplicate send is an invoice sent twice.
//
// The eight endpoints:
//   POST /auth/token      POST /auth/renew   POST /auth/revoke  GET /auth/token/status
//   POST /document/send   GET  /document/receive
//   GET  /document/receive/{id}              POST /document/receive/{id}/acknowledge
#pragma once

#include "../model/Model.h"

#include <string>
#include <vector>

namespace fk {
namespace sapi {

/// Which host to talk to. Their sandbox credentials deliberately fail against
/// the live host and the reverse, which is a good design and makes this a
/// setting worth being explicit about rather than a URL somebody edits.
enum class Environment { Sandbox, Live };

/// What a client needs before it can say anything. The secret is *not* here by
/// value on purpose: it lives in the system keychain and is fetched at the
/// moment of use, so it does not sit in the database, in a backup, or in a
/// crash dump.
struct Credentials {
    Environment environment = Environment::Sandbox;
    std::string clientId;
    /// "0245:2120345678" — the company being acted for. Every document call
    /// carries it, and the server checks it against the token.
    std::string participantId;
};

std::string baseUrl(Environment environment);

/// One HTTP call, described. The transport turns this into a request and
/// hands back a Response; nothing here knows how.
struct Request {
    std::string method;      ///< "GET" or "POST"
    std::string url;         ///< absolute
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;        ///< JSON, empty for a GET
};

struct Response {
    int         status = 0;  ///< HTTP status, or 0 when the request never arrived
    std::string body;
    std::string transportError;   ///< set when status is 0
};

// ------------------------------------------------------------------- tokens

struct Token {
    std::string accessToken;
    std::string refreshToken;
    /// Seconds the access token is good for, from when it was issued.
    int         expiresIn = 0;
    std::string scope;

    bool valid() const { return !accessToken.empty(); }
};

Request tokenRequest(const Credentials& credentials, const std::string& secret,
                     const std::string& scope = {});
Request renewRequest(const Credentials& credentials, const std::string& refreshToken);
Request revokeRequest(const Credentials& credentials, const std::string& token);
Request tokenStatusRequest(const Credentials& credentials, const Token& token);

/// Reads a token reply. `ok` false means the body was not a token.
bool readToken(const std::string& json, Token& out, std::string* error = nullptr);

/// Whether a token minted `ageSeconds` ago should be renewed before the next
/// call. Renewed early on purpose: a token that expires *during* a send leaves
/// you not knowing whether the document went.
bool shouldRenew(const Token& token, int ageSeconds);

// ---------------------------------------------------------------- documents

/// What goes with a document on the wire. Built from the invoice rather than
/// typed, because every one of these has to agree with the XML and the server
/// rejects the pair when they do not (422).
struct SendEnvelope {
    std::string documentId;
    std::string documentTypeId;
    std::string processId;
    std::string senderParticipantId;
    std::string receiverParticipantId;
    std::string creationDateTime;   ///< ISO 8601 in UTC, ending in Z
    std::string payload;            ///< the UBL, as XML, not base64
    std::string checksum;           ///< sha256 hex of the payload
};

/// The Peppol document type identifier for what `inv` is. Empty for a document
/// Peppol does not carry, which is how a proforma is kept off the network.
std::string documentTypeId(DocType type);
std::string processId();

/// Builds the envelope for an invoice and the UBL already written for it.
/// `nowIso` is passed in rather than read from the clock so this is testable.
SendEnvelope envelopeFor(const Invoice& invoice, const Company& seller,
                         const std::string& ubl, const std::string& nowIso);

/// The JSON body of a send. Separate from the request so a caller can show it,
/// log it, or diff it without making one.
std::string sendBody(const SendEnvelope& envelope);

/// `idempotencyKey` must be a UUID, and must be **the same** on a retry of the
/// same document: that is what makes a retry after a network error safe rather
/// than a second invoice.
Request sendRequest(const Credentials& credentials, const Token& token,
                    const SendEnvelope& envelope, const std::string& idempotencyKey);

Request receiveListRequest(const Credentials& credentials, const Token& token,
                           int limit = 100, const std::string& pageToken = {});
Request receiveDetailRequest(const Credentials& credentials, const Token& token,
                             const std::string& documentId);
Request acknowledgeRequest(const Credentials& credentials, const Token& token,
                           const std::string& documentId);

/// One entry of the receive list.
struct InboxEntry {
    std::string documentId;
    std::string documentTypeId;
    std::string senderParticipantId;
    std::string receiverParticipantId;
    std::string creationDateTime;
};

struct InboxPage {
    std::vector<InboxEntry> documents;
    std::string             nextPageToken;   ///< empty when there is no more
};

bool readInboxPage(const std::string& json, InboxPage& out, std::string* error = nullptr);

/// The UBL of one received document, from the detail reply.
bool readDocumentPayload(const std::string& json, std::string& ubl,
                         std::string* error = nullptr);

// ------------------------------------------------------------------- errors

/// What to do about a reply. The distinction that matters is between "try
/// again" and "stop": retrying something the server has already accepted is
/// how one invoice becomes two.
enum class Outcome {
    Ok,
    Retry,        ///< transient: 429, 503, or the request never arrived
    Reauthenticate,   ///< 401: mint a new token and try once
    Refused,      ///< 4xx that will not become true by repeating it
    Duplicate     ///< 409: the idempotency key is in flight or already used
};

struct Failure {
    Outcome     outcome = Outcome::Ok;
    std::string code;      ///< the server's machine-readable code, when it gave one
    std::string message;   ///< in Slovak where this layer supplies it
    std::string requestId; ///< quote this at their support desk
};

/// Classifies a reply. `body` is parsed for their {"error":{...}} shape when
/// there is one.
Failure classify(const Response& response);

/// How long to wait before attempt `attempt` (1-based) of something
/// retryable — exponential, capped, and never zero.
int backoffSeconds(int attempt);

} // namespace sapi
} // namespace fk
