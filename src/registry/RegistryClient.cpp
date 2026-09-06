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

#include "RegistryClient.h"

#include "VatRegister.h"

#include "../cz/Czech.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

namespace fk {
namespace {

constexpr const char* RPO_BASE  = "https://api.statistics.sk/rpo/v1/search";
constexpr const char* ARES_BASE =
    "https://ares.gov.cz/ekonomicke-subjekty-v-be/rest/ekonomicke-subjekty/";
constexpr const char* VIES_BASE = "https://ec.europa.eu/taxation_customs/vies/rest-api/ms/";

std::string s(const QString& v) { return v.trimmed().toStdString(); }

/// Several member states return "---" instead of a name or address. Treat that
/// as "not disclosed" so it never gets written onto an invoice.
std::string sOrEmpty(const QString& v) {
    const std::string out = s(v);
    return (out == "---" || out == "—") ? std::string() : out;
}

/// RPO returns history: several entries each with validFrom / validTo.
/// The current value is the last one that has not been closed.
QJsonObject currentEntry(const QJsonArray& entries) {
    QJsonObject best;
    for (const QJsonValue& v : entries) {
        const QJsonObject o = v.toObject();
        if (o.contains("validTo") && !o.value("validTo").toString().isEmpty()) continue;
        best = o;
    }
    if (best.isEmpty() && !entries.isEmpty()) best = entries.last().toObject();
    return best;
}

QString digitsOnly(const QString& v) {
    QString out;
    for (QChar c : v)
        if (c.isDigit()) out += c;
    return out;
}

} // namespace

RegistryClient::RegistryClient(QObject* parent)
    : nam_(new QNetworkAccessManager(parent)) {}

RegistryClient::~RegistryClient() {
    // The manager is owned by `parent` when there is one; otherwise by us.
    if (nam_ && !nam_->parent()) delete nam_;
}

// ------------------------------------------------------------------ parsing
RegistryRecord RegistryClient::parseRpo(const QByteArray& json) {
    RegistryRecord r;
    r.source = "RPO";

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return r;

    const QJsonArray results = doc.object().value("results").toArray();
    if (results.isEmpty()) return r;

    const QJsonObject subject = results.first().toObject();
    r.found = true;

    r.ico  = s(currentEntry(subject.value("identifiers").toArray()).value("value").toString());
    r.name = s(currentEntry(subject.value("fullNames").toArray()).value("value").toString());
    r.established = s(subject.value("establishment").toString());

    const QJsonObject addr = currentEntry(subject.value("addresses").toArray());
    if (!addr.isEmpty()) {
        r.street = joinStreet(s(addr.value("street").toString()),
                              s(addr.value("buildingNumber").toString()));
        r.city   = s(addr.value("municipality").toObject().value("value").toString());

        const QJsonArray codes = addr.value("postalCodes").toArray();
        if (!codes.isEmpty()) r.postalCode = normalizePostalCode(s(codes.first().toString()));

        const QJsonObject country = addr.value("country").toObject();
        const QString code = country.value("code").toString();
        r.countryCode = countryAlpha2(s(code.isEmpty() ? country.value("value").toString() : code));
    }

    const QJsonObject src = subject.value("sourceRegister").toObject();
    r.registerName = s(src.value("value").toObject().value("value").toString());
    r.court        = s(currentEntry(src.value("registrationOffices").toArray()).value("value").toString());
    r.registrationNumber =
        s(currentEntry(src.value("registrationNumbers").toArray()).value("value").toString());

    return r;
}

RegistryRecord RegistryClient::parseAres(const QByteArray& json) {
    RegistryRecord r;
    r.source = "ARES";

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return r;

    const QJsonObject o = doc.object();
    if (!o.contains("ico")) return r;                    // ARES 404 body

    r.found       = true;
    r.ico         = s(o.value("ico").toString());
    r.name        = s(o.value("obchodniJmeno").toString());
    r.dic         = s(o.value("dic").toString());
    r.countryCode = "CZ";
    r.established = s(o.value("datumVzniku").toString());

    // A Czech DIČ *is* the VAT number, but only counts if the VAT registration
    // is live — ARES says so explicitly, which RPO never does.
    const QJsonObject registrations = o.value("seznamRegistraci").toObject();
    r.vatActive = registrations.value("stavZdrojeDph").toString() == "AKTIVNI";
    if (r.vatActive) r.icDph = r.dic;

    const QJsonObject seat = o.value("sidlo").toObject();
    if (!seat.isEmpty()) {
        QString street = seat.value("nazevUlice").toString().trimmed();
        const QJsonValue houseNumber = seat.value("cisloDomovni");
        const QJsonValue orientation = seat.value("cisloOrientacni");
        QString number;
        if (houseNumber.isDouble()) number = QString::number(houseNumber.toInt());
        if (orientation.isDouble()) number += "/" + QString::number(orientation.toInt());
        // Villages often have no street name, only a house number.
        if (street.isEmpty()) street = seat.value("nazevCastiObce").toString().trimmed();
        r.street = s(number.isEmpty() ? street : (street.isEmpty() ? number : street + " " + number));

        r.city = s(seat.value("nazevObce").toString());
        const QJsonValue psc = seat.value("psc");
        if (psc.isDouble())
            r.postalCode = cz::normalizePsc(std::to_string(psc.toInt()));
        else
            r.postalCode = cz::normalizePsc(s(psc.toString()));
    }

    // The commercial-register entry lives in the "vr" (veřejný rejstřík) block.
    for (const QJsonValue& extra : o.value("dalsiUdaje").toArray()) {
        const QJsonObject block = extra.toObject();
        if (block.value("datovyZdroj").toString() != "vr") continue;
        const std::string znacka = s(block.value("spisovaZnacka").toString());
        if (!znacka.empty()) {
            r.registrationNumber = znacka;
            r.registryNoteText   = cz::registryNoteFromSpisovaZnacka(znacka);
        }
        break;
    }
    return r;
}

QString RegistryClient::viesErrorText(const QByteArray& json) {
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return {};

    const QJsonObject o = doc.object();
    if (o.value("isValid").toBool()) return {};

    // The distinction that matters: "this number is wrong" versus "the country
    // that could tell us is not answering right now".
    // Match by family: VIES has several MS_* and GLOBAL_* variants, including
    // ones ending in _TIME, and every one of them means "ask again later"
    // rather than "this number is wrong".
    const QString code = o.value("userError").toString();
    if (code.startsWith("MS_") || code.startsWith("GLOBAL_") ||
        code == "SERVICE_UNAVAILABLE" || code == "TIMEOUT" ||
        code == "IP_BLOCKED" || code == "VAT_BLOCKED")
        return "Register členského štátu momentálne neodpovedá (" + code +
               "). IČ DPH tým nie je vyvrátené – skúste to neskôr.";
    if (code == "INVALID_INPUT")
        return "VIES: formát IČ DPH nezodpovedá danej krajine.";
    if (code == "INVALID")
        return "VIES: toto IČ DPH nie je platné ani registrované.";
    return {};
}

RegistryRecord RegistryClient::parseVies(const QByteArray& json, const QString& countryCode) {
    RegistryRecord r;
    r.source = "VIES";

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return r;

    const QJsonObject o = doc.object();
    if (!o.value("isValid").toBool()) return r;

    r.found       = true;
    r.countryCode = s(countryCode.toUpper());
    r.icDph       = r.countryCode + s(o.value("vatNumber").toString());
    r.dic         = dicFromIcDph(r.icDph);
    r.name        = sOrEmpty(o.value("name").toString());

    const SplitAddress a = splitViesAddress(sOrEmpty(o.value("address").toString()));
    r.street     = a.street;
    r.city       = a.city;
    r.postalCode = normalizePostalCode(a.postalCode);
    return r;
}

// ------------------------------------------------------------------ requests
void RegistryClient::lookupCompany(Country country, const QString& ico, QObject* context,
                                   Callback done) {
    const QString clean = digitsOnly(ico);
    if (clean.size() != 8) {
        done({}, "IČO musí mať 8 číslic.");
        return;
    }
    const bool czech = (country == Country::CZ);

    QUrl url;
    if (czech) {
        url = QUrl(QString::fromLatin1(ARES_BASE) + clean);
    } else {
        url = QUrl(RPO_BASE);
        QUrlQuery query;
        query.addQueryItem("identifier", clean);
        url.setQuery(query);
    }

    QNetworkRequest request(url);
    request.setTransferTimeout(timeoutMs_);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Fakturo/0.1");
    request.setRawHeader("Accept", "application/json");

    QNetworkReply* reply = nam_->get(request);
    // Independent of the callback: the reply must be freed even when `context`
    // dies mid-flight and Qt drops the connection below.
    QObject::connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    QObject::connect(reply, &QNetworkReply::finished, context ? context : static_cast<QObject*>(nam_),
                     [reply, czech, done] {
        const QString source = czech ? "ARES" : "RPO";
        if (reply->error() != QNetworkReply::NoError) {
            // ARES answers 404 with a JSON body for an unknown IČO.
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (czech && status == 404) {
                done({}, "Subjekt s týmto IČO sa v ARES nenašiel.");
                return;
            }
            done({}, source + " nie je dostupný: " + reply->errorString());
            return;
        }
        RegistryRecord r = czech ? parseAres(reply->readAll()) : parseRpo(reply->readAll());
        done(r, r.found ? QString()
                        : QString("Subjekt s týmto IČO sa v %1 nenašiel.").arg(source));
    });
}

void RegistryClient::lookupVat(const QString& vatNumber, QObject* context, Callback done) {
    QString raw = vatNumber.toUpper().remove(' ').remove('-');
    QString country, number;
    if (raw.size() > 2 && raw.at(0).isLetter() && raw.at(1).isLetter()) {
        country = raw.left(2);
        number  = raw.mid(2);
    } else {
        country = "SK";
        number  = raw;
    }
    if (number.isEmpty()) {
        done({}, "Zadajte IČ DPH.");
        return;
    }

    QUrl url(QString(VIES_BASE) + country + "/vat/" + number);
    QNetworkRequest request(url);
    // VIES is a proxy: it forwards the query to that member state's own tax
    // administration, and they answer at wildly different speeds. The Czech
    // service routinely takes longer than a lookup against a local register,
    // so this gets its own, much larger budget.
    request.setTransferTimeout(30000);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Fakturo/0.1");
    request.setRawHeader("Accept", "application/json");

    QNetworkReply* reply = nam_->get(request);
    QObject::connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    QObject::connect(reply, &QNetworkReply::finished, context ? context : static_cast<QObject*>(nam_),
                     [reply, country, done] {
        // VIES answers some failures with an HTTP error *and* a JSON body that
        // explains it, so the body is worth reading either way.
        const QByteArray body = reply->readAll();
        const QString detail  = viesErrorText(body);

        if (reply->error() != QNetworkReply::NoError && detail.isEmpty()) {
            const bool timedOut = reply->error() == QNetworkReply::OperationCanceledError;
            done({}, timedOut
                         ? QString("VIES neodpovedal včas. Register členského štátu býva "
                                   "pomalý – skúste to o chvíľu znova.")
                         : QString("VIES nie je dostupný: ") + reply->errorString());
            return;
        }
        if (!detail.isEmpty()) { done({}, detail); return; }

        RegistryRecord r = parseVies(body, country);
        done(r, r.found ? QString()
                        : QString("VIES: toto IČ DPH nie je platné alebo nie je registrované."));
    });
}

QNetworkReply* RegistryClient::downloadRegister(RegisterKind kind, QObject* context,
                                                DownloadProgress progress, DownloadDone done) {
    const char* url = (kind == RegisterKind::Vat) ? VAT_REGISTER_URL : TAX_REGISTER_URL;
    QNetworkRequest request{QUrl(QString::fromLatin1(url))};
    // Tens of megabytes over a slow line: allow far longer than a field lookup.
    request.setTransferTimeout(600000);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Fakturo/0.1");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = nam_->get(request);
    QObject::connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);

    QObject* ctx = context ? context : static_cast<QObject*>(nam_);
    if (progress) {
        QObject::connect(reply, &QNetworkReply::downloadProgress, ctx,
                         [progress](qint64 received, qint64 total) { progress(received, total); });
    }
    QObject::connect(reply, &QNetworkReply::finished, ctx, [reply, done] {
        if (reply->error() != QNetworkReply::NoError) {
            done({}, "Register sa nepodarilo stiahnuť: " + reply->errorString());
            return;
        }
        const QByteArray payload = reply->readAll();
        if (payload.size() < 1000) {
            done({}, "Stiahnutý súbor je podozrivo malý – skúste to znova neskôr.");
            return;
        }
        done(payload, QString());
    });
    return reply;
}

} // namespace fk
