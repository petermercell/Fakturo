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

#include "Database.h"

#include "../core/Sha256.h"
#include "../sk/Slovak.h"
#include "../ubl/UblReader.h"

#include <algorithm>
#include <set>
#include <filesystem>

#include <sqlite3.h>

#include <utility>

namespace fk {
namespace {

/// Thin RAII wrapper so the call sites below read like plain SQL.
class Stmt {
public:
    Stmt(sqlite3* db, const std::string& sql) : db_(db) {
        ok_ = sqlite3_prepare_v2(db, sql.c_str(), -1, &st_, nullptr) == SQLITE_OK;
    }
    ~Stmt() { if (st_) sqlite3_finalize(st_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    bool ok() const { return ok_; }
    explicit operator bool() const { return ok_; }

    // Binding out of range is SQLITE_RANGE, which sqlite otherwise shrugs off:
    // the parameter simply stays NULL and the statement runs. An "UPDATE …
    // WHERE id=?55" whose ?55 was never bound updates nothing and reports
    // success. Record it instead, and refuse to run.
    Stmt& checkBind(int rc) { if (rc != SQLITE_OK) bindOk_ = false; return *this; }

    Stmt& bind(int i, const std::string& v) {
        return checkBind(sqlite3_bind_text(st_, i, v.c_str(), -1, SQLITE_TRANSIENT));
    }
    Stmt& bind(int i, int64_t v)  { return checkBind(sqlite3_bind_int64(st_, i, v)); }
    Stmt& bind(int i, int v)      { return checkBind(sqlite3_bind_int(st_, i, v)); }
    Stmt& bind(int i, Dec v)      { return checkBind(sqlite3_bind_int64(st_, i, v.raw())); }
    Stmt& bindBlob(int i, const std::string& v) {
        if (v.empty()) return checkBind(sqlite3_bind_null(st_, i));
        return checkBind(
            sqlite3_bind_blob64(st_, i, v.data(), v.size(), SQLITE_TRANSIENT));
    }

    bool step()  { return bindOk_ && sqlite3_step(st_) == SQLITE_ROW; }
    bool done()  { return bindOk_ && sqlite3_step(st_) == SQLITE_DONE; }

    std::string text(int c) const {
        const unsigned char* p = sqlite3_column_text(st_, c);
        return p ? reinterpret_cast<const char*>(p) : std::string();
    }
    int64_t i64(int c) const { return sqlite3_column_int64(st_, c); }
    std::string blob(int c) const {
        const void* data = sqlite3_column_blob(st_, c);
        const int size = sqlite3_column_bytes(st_, c);
        return data ? std::string(static_cast<const char*>(data), static_cast<size_t>(size))
                    : std::string();
    }
    int     i32(int c) const { return sqlite3_column_int(st_, c); }
    Dec     dec(int c) const { return Dec::fromRaw(sqlite3_column_int64(st_, c)); }

    std::string error() const {
        if (!bindOk_) return "parameter index out of range";
        return sqlite3_errmsg(db_);
    }

private:
    sqlite3*      db_ = nullptr;
    sqlite3_stmt* st_ = nullptr;
    bool          ok_ = false;
    bool          bindOk_ = true;
};

void readParty(const Stmt& s, int base, Party& p) {
    p.name                = s.text(base + 0);
    p.ico                 = s.text(base + 1);
    p.dic                 = s.text(base + 2);
    p.icDph               = s.text(base + 3);
    p.address.street      = s.text(base + 4);
    p.address.street2     = s.text(base + 5);
    p.address.city        = s.text(base + 6);
    p.address.postalCode  = s.text(base + 7);
    p.address.countryCode = s.text(base + 8);
    p.email               = s.text(base + 9);
    p.phone               = s.text(base + 10);
    p.contactName         = s.text(base + 11);
    p.endpointScheme      = s.text(base + 12);
    p.endpointId          = s.text(base + 13);
}

void bindParty(Stmt& s, int base, const Party& p) {
    s.bind(base + 0,  p.name);
    s.bind(base + 1,  p.ico);
    s.bind(base + 2,  p.dic);
    s.bind(base + 3,  p.icDph);
    s.bind(base + 4,  p.address.street);
    s.bind(base + 5,  p.address.street2);
    s.bind(base + 6,  p.address.city);
    s.bind(base + 7,  p.address.postalCode);
    s.bind(base + 8,  p.address.countryCode);
    s.bind(base + 9,  p.email);
    s.bind(base + 10, p.phone);
    s.bind(base + 11, p.contactName);
    s.bind(base + 12, p.endpointScheme);
    s.bind(base + 13, p.endpointId);
}

const char* PARTY_COLS =
    "name, ico, dic, ic_dph, street, street2, city, postal_code, country_code, "
    "email, phone, contact_name, endpoint_scheme, endpoint_id";

} // namespace

Database::~Database() { close(); }

bool Database::open(const std::string& path) {
    close();
    path_ = path;
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        err_ = db_ ? sqlite3_errmsg(db_) : "cannot open database";
        close();
        return false;
    }
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA foreign_keys=ON;");
    return migrate();
}

void Database::close() {
    if (bulkInsert_) { sqlite3_finalize(bulkInsert_); bulkInsert_ = nullptr; }
    // close_v2 always releases the handle, deferring cleanup if something is
    // still outstanding. Plain close() can return BUSY and leave the file open,
    // which is exactly the wrong moment when a restore is about to replace it.
    if (db_) { sqlite3_close_v2(db_); db_ = nullptr; }
}

bool Database::exec(const std::string& sql) {
    if (!db_) { err_ = "databáza nie je otvorená"; return false; }
    char* msg = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &msg) != SQLITE_OK) {
        err_ = msg ? msg : "sql error";
        if (msg) sqlite3_free(msg);
        return false;
    }
    return true;
}

namespace {

/// The shared party column definition, so the base schema and the v5 table
/// rebuild cannot drift apart.
std::string partyColumns() {
    return "name TEXT NOT NULL DEFAULT '', ico TEXT NOT NULL DEFAULT '', "
           "dic TEXT NOT NULL DEFAULT '', ic_dph TEXT NOT NULL DEFAULT '', "
           "street TEXT NOT NULL DEFAULT '', street2 TEXT NOT NULL DEFAULT '', "
           "city TEXT NOT NULL DEFAULT '', postal_code TEXT NOT NULL DEFAULT '', "
           "country_code TEXT NOT NULL DEFAULT 'SK', email TEXT NOT NULL DEFAULT '', "
           "phone TEXT NOT NULL DEFAULT '', contact_name TEXT NOT NULL DEFAULT '', "
           "endpoint_scheme TEXT NOT NULL DEFAULT '', endpoint_id TEXT NOT NULL DEFAULT ''";
}

/// Seller snapshot columns on `invoices`, in the order they are read and bound.
const std::vector<std::string>& sellerSnapshotColumns() {
    static const std::vector<std::string> columns = {
        "s_name", "s_ico", "s_dic", "s_ic_dph", "s_street", "s_street2", "s_city",
        "s_postal_code", "s_country_code", "s_email", "s_phone", "s_contact_name",
        "s_endpoint_scheme", "s_endpoint_id", "s_iban", "s_bic", "s_bank_name",
        "s_registry_note", "s_vat_mode", "s_bank_local", "s_qr_format"};
    return columns;
}

int64_t settingAsInt64(const std::string& value) {
    if (value.empty()) return 0;
    try { return std::stoll(value); } catch (...) { return 0; }
}

/// The invoices table, in one place, so the base schema and the rebuild that
/// changes its constraints cannot drift apart. Column order matters: the
/// rebuild copies with SELECT *.
std::string invoicesTableDdl(const std::string& tableName, const std::string& partyCols) {
    return "CREATE TABLE " + tableName + " ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  number TEXT NOT NULL, doc_type INTEGER NOT NULL DEFAULT 0,"
        "  issue_date TEXT NOT NULL, tax_point_date TEXT NOT NULL DEFAULT '',"
        "  due_date TEXT NOT NULL DEFAULT '', currency TEXT NOT NULL DEFAULT 'EUR',"
        "  " + partyCols + ", "
        "  buyer_reference TEXT NOT NULL DEFAULT '', order_reference TEXT NOT NULL DEFAULT '',"
        "  note TEXT NOT NULL DEFAULT '', variable_symbol TEXT NOT NULL DEFAULT '',"
        "  constant_symbol TEXT NOT NULL DEFAULT '', specific_symbol TEXT NOT NULL DEFAULT '',"
        "  payment_means_code TEXT NOT NULL DEFAULT '31', payment_terms TEXT NOT NULL DEFAULT '',"
        "  preceding_number TEXT NOT NULL DEFAULT '', preceding_date TEXT NOT NULL DEFAULT '',"
        "  vat_exemption_reason TEXT NOT NULL DEFAULT '', prepaid_amount INTEGER NOT NULL DEFAULT 0,"
        "  state TEXT NOT NULL DEFAULT 'draft', issued_at TEXT NOT NULL DEFAULT '',"
        "  company_id INTEGER NOT NULL DEFAULT 1,"
        "  s_name TEXT NOT NULL DEFAULT '', s_ico TEXT NOT NULL DEFAULT '',"
        "  s_dic TEXT NOT NULL DEFAULT '', s_ic_dph TEXT NOT NULL DEFAULT '',"
        "  s_street TEXT NOT NULL DEFAULT '', s_street2 TEXT NOT NULL DEFAULT '',"
        "  s_city TEXT NOT NULL DEFAULT '', s_postal_code TEXT NOT NULL DEFAULT '',"
        "  s_country_code TEXT NOT NULL DEFAULT '', s_email TEXT NOT NULL DEFAULT '',"
        "  s_phone TEXT NOT NULL DEFAULT '', s_contact_name TEXT NOT NULL DEFAULT '',"
        "  s_endpoint_scheme TEXT NOT NULL DEFAULT '', s_endpoint_id TEXT NOT NULL DEFAULT '',"
        "  s_iban TEXT NOT NULL DEFAULT '', s_bic TEXT NOT NULL DEFAULT '',"
        "  s_bank_name TEXT NOT NULL DEFAULT '', s_registry_note TEXT NOT NULL DEFAULT '',"
        "  s_vat_mode TEXT NOT NULL DEFAULT '',"
        // Per company, not global: company A's 0001 and company B's 0001 are
        // two different documents.
        "  UNIQUE (company_id, number));";
}

std::string invoicesTableDdlIfNotExists(const std::string& partyCols) {
    std::string ddl = invoicesTableDdl("invoices", partyCols);
    ddl.replace(0, std::string("CREATE TABLE").size(), "CREATE TABLE IF NOT EXISTS");
    return ddl;
}

std::string sellerSnapshotColumnList() {
    std::string out;
    for (const std::string& c : sellerSnapshotColumns()) {
        if (!out.empty()) out += ", ";
        out += c;
    }
    return out;
}

} // namespace

/// The column names in a CREATE TABLE statement, read out of the statement
/// itself. A table rebuild must name its columns — SELECT * copies by position,
/// and a database that gained columns in a different order, which any
/// ALTER-based migration can produce, is then silently scrambled — and the only
/// list that cannot drift from the table being built is the one the table is
/// built from.
///
/// A hand-written list was drifting: it named columns added by later migrations
/// (rounding_amount, s_bank_local, s_qr_format) that the rebuild's target table
/// does not have, so an old database that somehow carried one of them would
/// have failed the INSERT.
std::vector<std::string> columnsInDdl(const std::string& ddl) {
    std::vector<std::string> out;
    const size_t open = ddl.find('(');
    if (open == std::string::npos) return out;

    int depth = 0;
    std::string field;
    auto take = [&out](const std::string& text) {
        // The first word of the definition, skipping UNIQUE(...) and the like.
        size_t start = text.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return;
        const size_t end = text.find_first_of(" \t\r\n", start);
        std::string name = text.substr(start, end == std::string::npos ? end : end - start);
        static const std::vector<std::string> notColumns = {"UNIQUE", "PRIMARY", "FOREIGN",
                                                            "CHECK", "CONSTRAINT"};
        if (std::find(notColumns.begin(), notColumns.end(), name) != notColumns.end()) return;
        if (!name.empty()) out.push_back(name);
    };

    for (size_t i = open + 1; i < ddl.size(); ++i) {
        const char c = ddl[i];
        if (c == '(') { ++depth; field += c; continue; }
        if (c == ')' && depth > 0) { --depth; field += c; continue; }
        if (c == ')' && depth == 0) { take(field); break; }
        if (c == ',' && depth == 0) { take(field); field.clear(); continue; }
        field += c;
    }
    return out;
}

bool Database::createBaseSchema() {
    const std::string partyCols =
        "name TEXT NOT NULL DEFAULT '', ico TEXT NOT NULL DEFAULT '', "
        "dic TEXT NOT NULL DEFAULT '', ic_dph TEXT NOT NULL DEFAULT '', "
        "street TEXT NOT NULL DEFAULT '', street2 TEXT NOT NULL DEFAULT '', "
        "city TEXT NOT NULL DEFAULT '', postal_code TEXT NOT NULL DEFAULT '', "
        "country_code TEXT NOT NULL DEFAULT 'SK', email TEXT NOT NULL DEFAULT '', "
        "phone TEXT NOT NULL DEFAULT '', contact_name TEXT NOT NULL DEFAULT '', "
        "endpoint_scheme TEXT NOT NULL DEFAULT '', endpoint_id TEXT NOT NULL DEFAULT ''";

    return exec(
        "CREATE TABLE IF NOT EXISTS settings ("
        "  key TEXT PRIMARY KEY, value TEXT NOT NULL DEFAULT '');"

        "CREATE TABLE IF NOT EXISTS company ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT, " + partyCols + ", "
        "  iban TEXT NOT NULL DEFAULT '', bic TEXT NOT NULL DEFAULT '', "
        "  bank_name TEXT NOT NULL DEFAULT '', registry_note TEXT NOT NULL DEFAULT '', "
        "  logo_path TEXT NOT NULL DEFAULT '', vat_mode TEXT NOT NULL DEFAULT '');"

        "CREATE TABLE IF NOT EXISTS customers ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT, " + partyCols + ", "
        "  note TEXT NOT NULL DEFAULT '', archived INTEGER NOT NULL DEFAULT 0);"
        + invoicesTableDdlIfNotExists(partyCols) +
        "CREATE TABLE IF NOT EXISTS invoice_lines ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  invoice_id INTEGER NOT NULL REFERENCES invoices(id) ON DELETE CASCADE,"
        "  line_no INTEGER NOT NULL, description TEXT NOT NULL DEFAULT '',"
        "  unit TEXT NOT NULL DEFAULT 'ks', unit_code TEXT NOT NULL DEFAULT 'H87',"
        "  quantity INTEGER NOT NULL DEFAULT 0, unit_price INTEGER NOT NULL DEFAULT 0,"
        "  vat_rate INTEGER NOT NULL DEFAULT 0, vat_category TEXT NOT NULL DEFAULT 'S');"

        "CREATE TABLE IF NOT EXISTS vat_register ("
        "  ic_dph TEXT PRIMARY KEY, ico TEXT NOT NULL DEFAULT '',"
        "  name TEXT NOT NULL DEFAULT '', street TEXT NOT NULL DEFAULT '',"
        "  city TEXT NOT NULL DEFAULT '', postal_code TEXT NOT NULL DEFAULT '',"
        "  country TEXT NOT NULL DEFAULT '', reg_type TEXT NOT NULL DEFAULT '',"
        "  reg_date TEXT NOT NULL DEFAULT '');"

        "CREATE TABLE IF NOT EXISTS dic_register ("
        "  ico TEXT PRIMARY KEY, dic TEXT NOT NULL DEFAULT '',"
        "  name TEXT NOT NULL DEFAULT '', street TEXT NOT NULL DEFAULT '',"
        "  city TEXT NOT NULL DEFAULT '', postal_code TEXT NOT NULL DEFAULT '',"
        "  country TEXT NOT NULL DEFAULT '');"

        "CREATE INDEX IF NOT EXISTS idx_vat_ico ON vat_register(ico);"
        "CREATE INDEX IF NOT EXISTS idx_lines_invoice ON invoice_lines(invoice_id);"
        // Seed one company only when there is none at all. INSERT OR IGNORE on
        // id 1 would resurrect a company the user deleted.
        "INSERT INTO company (id) SELECT 1 WHERE NOT EXISTS (SELECT 1 FROM company);");
}


// ---------------------------------------------------------------- migrations
// Schema changes must reach databases that already hold real invoices, so the
// version is tracked in PRAGMA user_version and every step is idempotent.
// Adding a column with CREATE TABLE IF NOT EXISTS silently does nothing on an
// existing database — that was a live bug until this existed.

int Database::userVersion() {
    Stmt s(db_, "PRAGMA user_version");
    if (!s || !s.step()) return 0;
    return s.i32(0);
}

bool Database::setUserVersion(int version) {
    return exec("PRAGMA user_version = " + std::to_string(version));
}

std::string Database::companyCarryOver() {
    // The target of the version 5 rebuild is the version 5 shape, so anything
    // added later (logo) must be left out — and anything the source table
    // happens to lack must be left out too.
    static const std::vector<std::string> atVersionFive = {
        "id", "name", "ico", "dic", "ic_dph", "street", "street2", "city",
        "postal_code", "country_code", "email", "phone", "contact_name",
        "endpoint_scheme", "endpoint_id", "iban", "bic", "bank_name",
        "registry_note", "logo_path", "vat_mode"};

    const std::vector<std::string> present = tableColumns("company");
    std::string out;
    for (const std::string& column : atVersionFive) {
        if (std::find(present.begin(), present.end(), column) == present.end()) continue;
        if (!out.empty()) out += ", ";
        out += column;
    }
    return out;
}

std::vector<std::string> Database::tableColumns(const std::string& table) {
    std::vector<std::string> out;
    Stmt s(db_, "PRAGMA table_info(" + table + ")");
    if (!s) return out;
    while (s.step()) out.push_back(s.text(1));
    return out;
}

bool Database::hasColumn(const std::string& table, const std::string& column) {
    Stmt s(db_, "PRAGMA table_info(" + table + ")");
    if (!s) return false;
    while (s.step())
        if (s.text(1) == column) return true;
    return false;
}

bool Database::ensureColumn(const std::string& table, const std::string& column,
                            const std::string& declaration) {
    if (hasColumn(table, column)) return true;
    return exec("ALTER TABLE " + table + " ADD COLUMN " + column + " " + declaration);
}

/// True when `invoices` already carries the per-company uniqueness, so the
/// version 6 rebuild has nothing to do — which is the case for any database
/// created by the current base schema.
bool Database::tableHasCompositeNumberKey() {
    Stmt s(db_, "SELECT sql FROM sqlite_master WHERE type='table' AND name='invoices'");
    if (!s || !s.step()) return false;
    const std::string ddl = s.text(0);
    return ddl.find("UNIQUE (company_id, number)") != std::string::npos;
}

bool Database::migrate() {
    if (!createBaseSchema()) return false;

    int version = userVersion();

    // Version 1 is the shape that shipped before versioning existed. Databases
    // from that era report 0 but already have the base tables.
    if (version < 1) {
        if (!setUserVersion(1)) return false;
        version = 1;
    }

    // Version 2: document lifecycle, payments, audit trail.
    if (version < 2) {
        if (!exec("BEGIN")) return false;

        const bool ok =
            ensureColumn("invoices", "state", "TEXT NOT NULL DEFAULT 'draft'") &&
            ensureColumn("invoices", "issued_at", "TEXT NOT NULL DEFAULT ''") &&
            exec("CREATE TABLE IF NOT EXISTS payments ("
                 "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "  invoice_id INTEGER NOT NULL REFERENCES invoices(id) ON DELETE CASCADE,"
                 "  paid_on TEXT NOT NULL, amount INTEGER NOT NULL DEFAULT 0,"
                 "  note TEXT NOT NULL DEFAULT '');"
                 "CREATE INDEX IF NOT EXISTS idx_payments_invoice ON payments(invoice_id);"
                 "CREATE TABLE IF NOT EXISTS invoice_audit ("
                 "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "  invoice_id INTEGER NOT NULL,"
                 "  at TEXT NOT NULL, action TEXT NOT NULL,"
                 "  detail TEXT NOT NULL DEFAULT '');"
                 "CREATE INDEX IF NOT EXISTS idx_audit_invoice ON invoice_audit(invoice_id);") &&
            // Anything that already existed was written before drafts were a
            // concept. Treating it as a draft would quietly make historical
            // documents editable, so it counts as issued.
            exec("UPDATE invoices SET state = 'issued', issued_at = issue_date "
                 "WHERE state = 'draft' AND issued_at = ''");

        if (!ok) { exec("ROLLBACK"); return false; }
        if (!setUserVersion(2)) { exec("ROLLBACK"); return false; }
        if (!exec("COMMIT")) { exec("ROLLBACK"); return false; }
    }

    // Version 3: the VAT regime becomes explicit instead of being guessed from
    // the presence of an IČ DPH, which mislabels every §7 and §7a subject.
    if (version < 3) {
        if (!exec("BEGIN")) return false;
        const bool ok =
            ensureColumn("company", "vat_mode", "TEXT NOT NULL DEFAULT ''") &&
            // Best guess for existing data, matching the old behaviour. A §7a
            // company will read "platiteľ" until corrected, which the lookup
            // does automatically and the user can override.
            exec("UPDATE company SET vat_mode = CASE WHEN ic_dph <> '' THEN 'payer' "
                 "ELSE 'none' END WHERE vat_mode = ''");
        if (!ok) { exec("ROLLBACK"); return false; }
        if (!setUserVersion(3)) { exec("ROLLBACK"); return false; }
        if (!exec("COMMIT")) { exec("ROLLBACK"); return false; }
    }

    // Version 4: proformas. Their own numbering, and links from the invoice
    // that settles them back to each one.
    if (version < 4) {
        if (!exec("BEGIN")) return false;
        const bool ok = exec(
            "CREATE TABLE IF NOT EXISTS document_links ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  invoice_id INTEGER NOT NULL REFERENCES invoices(id) ON DELETE CASCADE,"
            "  related_id INTEGER NOT NULL,"
            "  relation TEXT NOT NULL DEFAULT 'proforma',"
            "  UNIQUE (invoice_id, related_id, relation));"
            "CREATE INDEX IF NOT EXISTS idx_links_related ON document_links(related_id);");
        if (!ok) { exec("ROLLBACK"); return false; }
        if (!setUserVersion(4)) { exec("ROLLBACK"); return false; }
        if (!exec("COMMIT")) { exec("ROLLBACK"); return false; }
    }

    // Version 5: more than one company. Three separate problems in one step —
    // the single-row CHECK has to go, every document must remember which
    // company issued it *and* what that company looked like at the time, and
    // each company needs its own numbering.
    if (version < 5) {
        if (!exec("BEGIN")) return false;

        // Only the columns that exist in both shapes travel across: the target
        // table is the version 5 shape, and the source is whatever this
        // database actually has.
        const std::string carry = companyCarryOver();

        bool ok = true;
        if (hasColumn("company", "id")) {
            // SQLite cannot drop a CHECK constraint, so the table is rebuilt.
            ok = ok && exec(
                "CREATE TABLE company_new ("
                "  id INTEGER PRIMARY KEY AUTOINCREMENT, " + partyColumns() + ", "
                "  iban TEXT NOT NULL DEFAULT '', bic TEXT NOT NULL DEFAULT '', "
                "  bank_name TEXT NOT NULL DEFAULT '', registry_note TEXT NOT NULL DEFAULT '', "
                "  logo_path TEXT NOT NULL DEFAULT '', vat_mode TEXT NOT NULL DEFAULT '');") &&
                exec("INSERT INTO company_new (" + carry + ") "
                     "SELECT " + carry + " FROM company;") &&
                exec("DROP TABLE company;") &&
                exec("ALTER TABLE company_new RENAME TO company;");
        }

        // Which company, and what it looked like.
        ok = ok && ensureColumn("invoices", "company_id", "INTEGER NOT NULL DEFAULT 1");
        for (const std::string& column : sellerSnapshotColumns())
            ok = ok && ensureColumn("invoices", column, "TEXT NOT NULL DEFAULT ''");

        // Existing documents belong to the only company there was, and take
        // its current details as their snapshot. That is the best available
        // answer: nothing older was ever recorded.
        ok = ok && exec(
            "UPDATE invoices SET "
            "  s_name = (SELECT name FROM company WHERE id = 1),"
            "  s_ico = (SELECT ico FROM company WHERE id = 1),"
            "  s_dic = (SELECT dic FROM company WHERE id = 1),"
            "  s_ic_dph = (SELECT ic_dph FROM company WHERE id = 1),"
            "  s_street = (SELECT street FROM company WHERE id = 1),"
            "  s_street2 = (SELECT street2 FROM company WHERE id = 1),"
            "  s_city = (SELECT city FROM company WHERE id = 1),"
            "  s_postal_code = (SELECT postal_code FROM company WHERE id = 1),"
            "  s_country_code = (SELECT country_code FROM company WHERE id = 1),"
            "  s_email = (SELECT email FROM company WHERE id = 1),"
            "  s_phone = (SELECT phone FROM company WHERE id = 1),"
            "  s_contact_name = (SELECT contact_name FROM company WHERE id = 1),"
            "  s_endpoint_scheme = (SELECT endpoint_scheme FROM company WHERE id = 1),"
            "  s_endpoint_id = (SELECT endpoint_id FROM company WHERE id = 1),"
            "  s_iban = (SELECT iban FROM company WHERE id = 1),"
            "  s_bic = (SELECT bic FROM company WHERE id = 1),"
            "  s_bank_name = (SELECT bank_name FROM company WHERE id = 1),"
            "  s_registry_note = (SELECT registry_note FROM company WHERE id = 1),"
            "  s_vat_mode = (SELECT vat_mode FROM company WHERE id = 1) "
            "WHERE s_name = ''");

        // Numbering moves under the company it belongs to.
        // "series.next" -> "series.1.next", "series.proforma.next" ->
        // "series.proforma.1.next". Done explicitly rather than with one
        // REPLACE, which would mangle the proforma keys.
        ok = ok && exec(
            "INSERT OR REPLACE INTO settings (key, value) "
            // "series.proforma." is 16 characters, so the payload starts at 17.
            "SELECT 'series.proforma.1.' || SUBSTR(key, 17), value FROM settings "
            "WHERE key LIKE 'series.proforma.%';") &&
            exec("INSERT OR REPLACE INTO settings (key, value) "
                 "SELECT 'series.1.' || SUBSTR(key, 8), value FROM settings "
                 "WHERE key LIKE 'series.%' AND key NOT LIKE 'series.proforma.%' "
                 "AND key NOT LIKE 'series.1.%';") &&
            exec("DELETE FROM settings WHERE key LIKE 'series.%' "
                 "AND key NOT LIKE 'series.1.%' AND key NOT LIKE 'series.proforma.1.%';") &&
            exec("INSERT OR REPLACE INTO settings (key, value) VALUES ('company.active', '1')");

        if (!ok) { exec("ROLLBACK"); return false; }
        if (!setUserVersion(5)) { exec("ROLLBACK"); return false; }
        if (!exec("COMMIT")) { exec("ROLLBACK"); return false; }
    }

    // Version 6: invoice numbers are unique per company, not globally. Two
    // companies both starting at 0001 are not a duplicate — before this, the
    // second one silently skipped every number the first had used.
    //
    // A table rebuild must copy the columns the table *has at the time*, not
    // the ones it has today: hard-coding today's list breaks this step the
    // next time a column is added, which is exactly what happened once.
    if (version < 6 && !tableHasCompositeNumberKey()) {
        // A table rebuild with children pointing at it: the documented recipe
        // is to disable foreign keys around it, and it cannot be done inside a
        // transaction.
        // Only the columns present in both shapes travel across. Matched whole,
        // not as substrings: "iban" is inside "s_iban" and "name" is inside
        // both "s_name" and "bank_name", so a substring test carries columns
        // the new table does not have.
        const std::vector<std::string> existing = tableColumns("invoices");
        const std::vector<std::string> wanted =
            columnsInDdl(invoicesTableDdl("invoices_new", partyColumns()));
        std::string carryOver;
        for (const std::string& column : existing) {
            if (std::find(wanted.begin(), wanted.end(), column) == wanted.end()) continue;
            if (!carryOver.empty()) carryOver += ", ";
            carryOver += column;
        }

        exec("PRAGMA foreign_keys=OFF");
        const bool ok =
            exec("BEGIN") &&
            exec(invoicesTableDdl("invoices_new", partyColumns())) &&
            exec("INSERT INTO invoices_new (" + carryOver + ") "
                 "SELECT " + carryOver + " FROM invoices;") &&
            exec("DROP TABLE invoices;") &&
            exec("ALTER TABLE invoices_new RENAME TO invoices;") &&
            exec("CREATE INDEX IF NOT EXISTS idx_invoices_company ON invoices(company_id);") &&
            setUserVersion(6) &&
            exec("COMMIT");
        if (!ok) {
            exec("ROLLBACK");
            exec("PRAGMA foreign_keys=ON");
            return false;
        }
        exec("PRAGMA foreign_keys=ON");
    }

    if (version < 6) setUserVersion(6);       // already in the right shape

    // Version 7: keep the rendered document, not just the data behind it.
    if (version < 7) {
        if (!exec("BEGIN")) return false;
        const bool ok = exec(
            "CREATE TABLE IF NOT EXISTS issued_documents ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  invoice_id INTEGER NOT NULL REFERENCES invoices(id) ON DELETE CASCADE,"
            "  version INTEGER NOT NULL DEFAULT 1,"
            "  issued_at TEXT NOT NULL,"
            "  pdf BLOB, ubl TEXT NOT NULL DEFAULT '',"
            "  pdf_sha256 TEXT NOT NULL DEFAULT '', ubl_sha256 TEXT NOT NULL DEFAULT '',"
            "  UNIQUE (invoice_id, version));"
            "CREATE INDEX IF NOT EXISTS idx_issued_invoice ON issued_documents(invoice_id);");
        if (!ok) { exec("ROLLBACK"); return false; }
        if (!setUserVersion(7)) { exec("ROLLBACK"); return false; }
        if (!exec("COMMIT")) { exec("ROLLBACK"); return false; }
    }

    // Version 8: more than one bank account per company, and the domestic
    // account number alongside the IBAN.
    if (version < 8) {
        if (!exec("BEGIN")) return false;
        bool ok = exec(
            "CREATE TABLE IF NOT EXISTS bank_accounts ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  company_id INTEGER NOT NULL REFERENCES company(id) ON DELETE CASCADE,"
            "  label TEXT NOT NULL DEFAULT '', iban TEXT NOT NULL DEFAULT '',"
            "  bic TEXT NOT NULL DEFAULT '', bank_name TEXT NOT NULL DEFAULT '',"
            "  local_number TEXT NOT NULL DEFAULT '', currency TEXT NOT NULL DEFAULT '',"
            "  is_default INTEGER NOT NULL DEFAULT 0, position INTEGER NOT NULL DEFAULT 0,"
            "  qr_format TEXT NOT NULL DEFAULT 'auto');"
            "CREATE INDEX IF NOT EXISTS idx_accounts_company ON bank_accounts(company_id);");

        // The account each company already had becomes its first one.
        ok = ok && exec(
            "INSERT INTO bank_accounts (company_id, label, iban, bic, bank_name, currency, "
            "is_default, position) "
            "SELECT id, '', iban, bic, bank_name, '', 1, 0 FROM company WHERE iban <> ''");

        ok = ok && ensureColumn("invoices", "s_bank_local", "TEXT NOT NULL DEFAULT ''");

        if (!ok) { exec("ROLLBACK"); return false; }
        if (!setUserVersion(8)) { exec("ROLLBACK"); return false; }
        if (!exec("COMMIT")) { exec("ROLLBACK"); return false; }
    }

    // Version 9: which payment code an account prints. Not derivable — Slovak
    // and Czech banking apps read different standards and disagree about
    // foreign ones — so it becomes a setting with a sensible default.
    if (version < 9) {
        if (!exec("BEGIN")) return false;
        const bool ok =
            ensureColumn("bank_accounts", "qr_format", "TEXT NOT NULL DEFAULT 'auto'") &&
            ensureColumn("invoices", "s_qr_format", "TEXT NOT NULL DEFAULT 'auto'");
        if (!ok) { exec("ROLLBACK"); return false; }
        if (!setUserVersion(9)) { exec("ROLLBACK"); return false; }
        if (!exec("COMMIT")) { exec("ROLLBACK"); return false; }
    }

    // Version 10: bank statement import. Keyed on the bank's own movement id
    // so re-importing an overlapping period cannot pay an invoice twice — the
    // one failure mode that would quietly corrupt the books.
    if (version < 10) {
        if (!exec("BEGIN")) return false;
        const bool ok = exec(
            "CREATE TABLE IF NOT EXISTS imported_transactions ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  company_id INTEGER NOT NULL,"
            "  bank_id TEXT NOT NULL,"
            "  invoice_id INTEGER,"
            "  payment_id INTEGER,"
            "  imported_at TEXT NOT NULL DEFAULT '',"
            "  source_file TEXT NOT NULL DEFAULT '',"
            "  source_sha TEXT NOT NULL DEFAULT '',"
            "  UNIQUE (company_id, bank_id));");
        if (!ok) { exec("ROLLBACK"); return false; }
        if (!setUserVersion(10)) { exec("ROLLBACK"); return false; }
        if (!exec("COMMIT")) { exec("ROLLBACK"); return false; }
    }

    // Version 11: the logo as bytes. A stored path breaks when the file moves.
    if (version < 11) {
        if (!exec("BEGIN")) return false;
        const bool ok = ensureColumn("company", "logo", "BLOB");
        if (!ok) { exec("ROLLBACK"); return false; }
        if (!setUserVersion(11)) { exec("ROLLBACK"); return false; }
        if (!exec("COMMIT")) { exec("ROLLBACK"); return false; }
    }

    // Version 12: document-level discounts and surcharges (BG-20 / BG-21) and
    // the rounding amount (BT-114). Until now a discount had to be faked as a
    // negative line, which put it in the wrong total and overstated the
    // taxable base of its VAT category.
    if (version < 12) {
        if (!exec("BEGIN")) return false;
        const bool ok =
            exec("CREATE TABLE IF NOT EXISTS document_allowances ("
                 "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "  invoice_id INTEGER NOT NULL REFERENCES invoices(id) ON DELETE CASCADE,"
                 "  position INTEGER NOT NULL DEFAULT 0,"
                 "  is_charge INTEGER NOT NULL DEFAULT 0,"
                 "  reason TEXT NOT NULL DEFAULT '',"
                 "  percentage INTEGER NOT NULL DEFAULT 0,"
                 "  base_amount INTEGER NOT NULL DEFAULT 0,"
                 "  amount INTEGER NOT NULL DEFAULT 0,"
                 "  vat_category TEXT NOT NULL DEFAULT 'S',"
                 "  vat_rate INTEGER NOT NULL DEFAULT 0);") &&
            exec("CREATE INDEX IF NOT EXISTS idx_allowances_invoice "
                 "ON document_allowances(invoice_id);") &&
            ensureColumn("invoices", "rounding_amount", "INTEGER NOT NULL DEFAULT 0");
        if (!ok) { exec("ROLLBACK"); return false; }
        if (!setUserVersion(12)) { exec("ROLLBACK"); return false; }
        if (!exec("COMMIT")) { exec("ROLLBACK"); return false; }
    }

    // Version 13: an allowance can belong to one line (BG-27 / BG-28) rather
    // than to the document. Zero keeps the meaning it already had.
    if (version < 13) {
        if (!exec("BEGIN")) return false;
        const bool ok =
            ensureColumn("document_allowances", "line_no", "INTEGER NOT NULL DEFAULT 0");
        if (!ok) { exec("ROLLBACK"); return false; }
        if (!setUserVersion(13)) { exec("ROLLBACK"); return false; }
        if (!exec("COMMIT")) { exec("ROLLBACK"); return false; }
    }

    // Version 14: the VAT restated in the seller's own currency (BT-6 / BT-111)
    // and the rate it was arrived at. On the invoice rather than in settings:
    // § 25 has a correction reuse the rate of the original tax point, so the
    // rate has to be remembered per document, not looked up again later.
    if (version < 14) {
        if (!exec("BEGIN")) return false;
        const bool ok =
            ensureColumn("invoices", "vat_accounting_currency", "TEXT NOT NULL DEFAULT ''") &&
            ensureColumn("invoices", "exchange_rate", "INTEGER NOT NULL DEFAULT 0") &&
            ensureColumn("invoices", "exchange_rate_date", "TEXT NOT NULL DEFAULT ''");
        if (!ok) { exec("ROLLBACK"); return false; }
        if (!setUserVersion(14)) { exec("ROLLBACK"); return false; }
        if (!exec("COMMIT")) { exec("ROLLBACK"); return false; }
    }

    // Version 15: invoices somebody sent *to* you. The original bytes are the
    // legal document and are kept verbatim; everything else in the row is a
    // copy of what the reader made of them, so the list can be drawn without
    // parsing every document again.
    if (version < 15) {
        if (!exec("BEGIN")) return false;
        const bool ok = exec(
            "CREATE TABLE IF NOT EXISTS received_invoices ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  company_id INTEGER NOT NULL DEFAULT 1,"
            "  number TEXT NOT NULL DEFAULT '',"
            "  issue_date TEXT NOT NULL DEFAULT '',"
            "  due_date TEXT NOT NULL DEFAULT '',"
            "  tax_point_date TEXT NOT NULL DEFAULT '',"
            "  supplier_name TEXT NOT NULL DEFAULT '',"
            "  supplier_ico TEXT NOT NULL DEFAULT '',"
            "  supplier_ic_dph TEXT NOT NULL DEFAULT '',"
            "  supplier_country TEXT NOT NULL DEFAULT '',"
            "  currency TEXT NOT NULL DEFAULT 'EUR',"
            "  payable INTEGER NOT NULL DEFAULT 0,"
            "  tax_amount INTEGER NOT NULL DEFAULT 0,"
            "  variable_symbol TEXT NOT NULL DEFAULT '',"
            "  iban TEXT NOT NULL DEFAULT '',"
            "  received_at TEXT NOT NULL DEFAULT '',"
            "  source TEXT NOT NULL DEFAULT '',"
            // A BLOB, not TEXT: the promise is that the received file is kept
            // byte for byte, and a TEXT column is read back with strlen — one
            // NUL byte and the document silently ends there while its digest
            // still describes the whole of it.
            "  original BLOB,"
            // The digest is what makes a second import of the same file a
            // no-op. Unique **per company**, not across the table: importing a
            // file while the wrong company is active, then importing it again
            // under the right one, must add it rather than silently succeed
            // and show nothing.
            "  sha256 TEXT NOT NULL DEFAULT '',"
            "  warnings TEXT NOT NULL DEFAULT '',"
            "  note TEXT NOT NULL DEFAULT '',"
            "  paid_on TEXT NOT NULL DEFAULT '',"
            "  paid_amount INTEGER NOT NULL DEFAULT 0,"
            "  UNIQUE (company_id, sha256));"
            "CREATE INDEX IF NOT EXISTS idx_received_company "
            "ON received_invoices(company_id);");
        if (!ok) { exec("ROLLBACK"); return false; }
        if (!setUserVersion(15)) { exec("ROLLBACK"); return false; }
        if (!exec("COMMIT")) { exec("ROLLBACK"); return false; }
    }

    // Version 16: a movement on a statement may pay an invoice you *received*
    // rather than one you issued, so the guard row has to say which.
    if (version < 16) {
        if (!exec("BEGIN")) return false;
        const bool ok = ensureColumn("imported_transactions", "received_id",
                                     "INTEGER NOT NULL DEFAULT 0");
        if (!ok) { exec("ROLLBACK"); return false; }
        if (!setUserVersion(16)) { exec("ROLLBACK"); return false; }
        if (!exec("COMMIT")) { exec("ROLLBACK"); return false; }
    }

    // Version 17: the Slovak Peppol address is the DIČ under scheme 0245, not
    // the IČO under 0158. Records saved before this carry the old identifier,
    // and because an identifier entered by hand deliberately wins over the
    // rule, they would keep it — and keep being unroutable. The old *default*
    // is cleared so the rule applies again; anything else the user chose is
    // left exactly as they left it.
    //
    // Only the live records. Issued invoices keep the seller and buyer they
    // were issued with, which is the whole point of the snapshot.
    if (version < 17) {
        if (!exec("BEGIN")) return false;
        const bool ok =
            exec("UPDATE company SET endpoint_scheme = '', endpoint_id = '' "
                 "WHERE endpoint_scheme = '0158' AND country_code = 'SK';") &&
            exec("UPDATE customers SET endpoint_scheme = '', endpoint_id = '' "
                 "WHERE endpoint_scheme = '0158' AND country_code = 'SK';");
        if (!ok) { exec("ROLLBACK"); return false; }
        if (!setUserVersion(17)) { exec("ROLLBACK"); return false; }
        if (!exec("COMMIT")) { exec("ROLLBACK"); return false; }
    }

    // Version 18: the item catalogue. Scoped per company, like the numbering
    // and the documents: two companies rarely sell the same thing, and a
    // shared list would be one more thing to keep tidy.
    if (version < 18) {
        if (!exec("BEGIN")) return false;
        const bool ok = exec(
            "CREATE TABLE IF NOT EXISTS catalog_items ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  company_id INTEGER NOT NULL DEFAULT 1,"
            "  description TEXT NOT NULL DEFAULT '',"
            "  unit TEXT NOT NULL DEFAULT 'ks',"
            "  unit_code TEXT NOT NULL DEFAULT 'H87',"
            "  unit_price INTEGER NOT NULL DEFAULT 0,"
            "  vat_rate INTEGER NOT NULL DEFAULT 0,"
            "  vat_category TEXT NOT NULL DEFAULT 'S',"
            "  uses INTEGER NOT NULL DEFAULT 0,"
            "  last_used TEXT NOT NULL DEFAULT '',"
            // One line per description per company. Saving the same thing
            // twice updates it rather than filling the list with copies.
            "  UNIQUE (company_id, description));");
        if (!ok) { exec("ROLLBACK"); return false; }
        if (!setUserVersion(18)) { exec("ROLLBACK"); return false; }
        if (!exec("COMMIT")) { exec("ROLLBACK"); return false; }
    }

    return true;
}

// ---------------------------------------------------------------- catalogue
namespace {

const char* CATALOG_COLUMNS =
    "id, company_id, description, unit, unit_code, unit_price, vat_rate, "
    "vat_category, uses, last_used";

void readCatalogItem(Stmt& s, Database::CatalogItem& out) {
    out.id          = s.i64(0);
    out.companyId   = s.i64(1);
    out.description = s.text(2);
    out.unit        = s.text(3);
    out.unitCodeUn  = s.text(4);
    out.unitPrice   = s.dec(5);
    out.vatRate     = s.dec(6);
    out.vatCategory = s.text(7);
    out.uses        = s.i32(8);
    out.lastUsed    = s.text(9);
}

} // namespace

std::vector<Database::CatalogItem> Database::catalogItems() {
    std::vector<CatalogItem> out;
    Stmt s(db_, std::string("SELECT ") + CATALOG_COLUMNS +
                " FROM catalog_items WHERE company_id = ?1 "
                "ORDER BY uses DESC, last_used DESC, description COLLATE NOCASE");
    if (!s) { err_ = s.error(); return out; }
    s.bind(1, activeCompanyId());
    while (s.step()) {
        CatalogItem item;
        readCatalogItem(s, item);
        out.push_back(std::move(item));
    }
    return out;
}

bool Database::saveCatalogItem(CatalogItem& item) {
    if (!db_) { err_ = "databáza nie je otvorená"; return false; }
    if (item.description.empty()) { err_ = "Položka bez popisu."; return false; }
    if (item.companyId == 0) item.companyId = activeCompanyId();

    // Upsert on the description, so saving the same line again corrects the
    // price rather than adding a second entry that will drift from the first.
    // The use count is deliberately left alone: it belongs to the line, not to
    // this edit of it.
    Stmt s(db_,
        "INSERT INTO catalog_items (company_id, description, unit, unit_code, unit_price, "
        "vat_rate, vat_category) VALUES (?1,?2,?3,?4,?5,?6,?7) "
        "ON CONFLICT(company_id, description) DO UPDATE SET "
        "unit = excluded.unit, unit_code = excluded.unit_code, "
        "unit_price = excluded.unit_price, vat_rate = excluded.vat_rate, "
        "vat_category = excluded.vat_category");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, item.companyId).bind(2, item.description).bind(3, item.unit)
     .bind(4, item.unitCodeUn).bind(5, item.unitPrice).bind(6, item.vatRate)
     .bind(7, item.vatCategory);
    if (!s.done()) { err_ = s.error(); return false; }

    // The rowid of an upsert that updated is not last_insert_rowid, so it is
    // read back rather than assumed.
    Stmt find(db_, "SELECT id FROM catalog_items WHERE company_id = ?1 AND description = ?2");
    if (find) {
        find.bind(1, item.companyId).bind(2, item.description);
        if (find.step()) item.id = find.i64(0);
    }
    return true;
}

bool Database::deleteCatalogItem(int64_t id) {
    Stmt s(db_, "DELETE FROM catalog_items WHERE id = ?1");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, id);
    return s.done();
}

bool Database::noteCatalogUse(int64_t id, const std::string& todayIso) {
    Stmt s(db_, "UPDATE catalog_items SET uses = uses + 1, last_used = ?1 WHERE id = ?2");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, todayIso).bind(2, id);
    return s.done();
}

// ------------------------------------------------------------------ company
namespace {

const char* COMPANY_EXTRA = "iban, bic, bank_name, registry_note, logo_path, vat_mode, logo";

void readCompany(Stmt& s, Company& c) {
    c.id = s.i64(0);
    readParty(s, 1, c);
    c.iban         = s.text(15);
    c.bic          = s.text(16);
    c.bankName     = s.text(17);
    c.registryNote = s.text(18);
    c.logoPath     = s.text(19);
    c.logo         = s.blob(21);
    c.vatMode      = vatModeFromCode(s.text(20));
}

} // namespace

std::vector<Company> Database::companies() {
    std::vector<Company> out;
    Stmt s(db_, std::string("SELECT id, ") + PARTY_COLS + ", " + COMPANY_EXTRA +
                " FROM company ORDER BY id");
    if (!s) { err_ = s.error(); return out; }
    while (s.step()) {
        Company c;
        readCompany(s, c);
        out.push_back(std::move(c));
    }
    for (Company& c : out) c.accounts = bankAccounts(c.id);
    return out;
}

bool Database::loadCompany(int64_t id, Company& out) {
    Stmt s(db_, std::string("SELECT id, ") + PARTY_COLS + ", " + COMPANY_EXTRA +
                " FROM company WHERE id = ?1");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, id);
    if (!s.step()) return false;
    readCompany(s, out);
    out.accounts = bankAccounts(out.id);
    return true;
}

int64_t Database::activeCompanyId() {
    const int64_t stored = settingAsInt64(setting("company.active", "0"));
    if (stored != 0) {
        Stmt s(db_, "SELECT 1 FROM company WHERE id = ?1");
        if (s) { s.bind(1, stored); if (s.step()) return stored; }
    }
    // Fall back to the lowest existing company rather than to nothing: an
    // invalid setting must not leave the app with no company at all.
    Stmt first(db_, "SELECT id FROM company ORDER BY id LIMIT 1");
    if (first && first.step()) return first.i64(0);
    return 0;
}

Company Database::activeCompany() {
    Company c;
    const int64_t id = activeCompanyId();
    if (id != 0) loadCompany(id, c);
    return c;
}

bool Database::setActiveCompany(int64_t id) {
    return setSetting("company.active", std::to_string(id));
}

bool Database::saveCompany(Company& c) {
    if (c.id == 0) {
        Stmt s(db_, std::string("INSERT INTO company (") + PARTY_COLS + ", " + COMPANY_EXTRA +
                    ") VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21)");
        if (!s) { err_ = s.error(); return false; }
        bindParty(s, 1, c);
        s.bind(15, c.iban).bind(16, c.bic).bind(17, c.bankName)
         .bind(18, c.registryNote).bind(19, c.logoPath)
         .bind(20, std::string(vatModeCode(c.vatMode)));
        s.bindBlob(21, c.logo);
        if (!s.done()) { err_ = s.error(); return false; }
        c.id = sqlite3_last_insert_rowid(db_);
        return saveBankAccounts(c.id, c.accounts);
    }

    Stmt s(db_,
        "UPDATE company SET name=?1, ico=?2, dic=?3, ic_dph=?4, street=?5, street2=?6, "
        "city=?7, postal_code=?8, country_code=?9, email=?10, phone=?11, contact_name=?12, "
        "endpoint_scheme=?13, endpoint_id=?14, iban=?15, bic=?16, bank_name=?17, "
        "registry_note=?18, logo_path=?19, vat_mode=?20, logo=?21 WHERE id=?22");
    if (!s) { err_ = s.error(); return false; }
    bindParty(s, 1, c);
    s.bind(15, c.iban).bind(16, c.bic).bind(17, c.bankName)
     .bind(18, c.registryNote).bind(19, c.logoPath)
     .bind(20, std::string(vatModeCode(c.vatMode)));
    s.bindBlob(21, c.logo);
    s.bind(22, c.id);
    if (!s.done()) { err_ = s.error(); return false; }
    return saveBankAccounts(c.id, c.accounts);
}

int Database::invoiceCountFor(int64_t companyId) {
    Stmt s(db_, "SELECT COUNT(*) FROM invoices WHERE company_id = ?1");
    if (!s) return 0;
    s.bind(1, companyId);
    return s.step() ? s.i32(0) : 0;
}

std::vector<BankAccount> Database::bankAccounts(int64_t companyId) {
    std::vector<BankAccount> out;
    Stmt s(db_, "SELECT id, label, iban, bic, bank_name, local_number, currency, is_default, "
                "qr_format FROM bank_accounts WHERE company_id = ?1 ORDER BY position, id");
    if (!s) { err_ = s.error(); return out; }
    s.bind(1, companyId);
    while (s.step()) {
        BankAccount a;
        a.id          = s.i64(0);
        a.label       = s.text(1);
        a.iban        = s.text(2);
        a.bic         = s.text(3);
        a.bankName    = s.text(4);
        a.localNumber = s.text(5);
        a.currency    = s.text(6);
        a.isDefault   = s.i32(7) != 0;
        a.qrFormat    = qrFormatFromCode(s.text(8));
        out.push_back(std::move(a));
    }
    return out;
}

bool Database::saveBankAccounts(int64_t companyId, std::vector<BankAccount>& accounts) {
    const bool owns = sqlite3_get_autocommit(db_) != 0;
    if (owns && !exec("BEGIN")) return false;

    Stmt clear(db_, "DELETE FROM bank_accounts WHERE company_id = ?1");
    if (!clear) { err_ = clear.error(); if (owns) exec("ROLLBACK"); return false; }
    clear.bind(1, companyId);
    if (!clear.done()) { err_ = clear.error(); if (owns) exec("ROLLBACK"); return false; }

    int position = 0;
    for (BankAccount& a : accounts) {
        Stmt s(db_, "INSERT INTO bank_accounts (company_id, label, iban, bic, bank_name, "
                    "local_number, currency, is_default, position, qr_format) "
                    "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)");
        if (!s) { err_ = s.error(); if (owns) exec("ROLLBACK"); return false; }
        s.bind(1, companyId).bind(2, a.label).bind(3, a.iban).bind(4, a.bic)
         .bind(5, a.bankName).bind(6, a.localNumber).bind(7, a.currency)
         .bind(8, a.isDefault ? 1 : 0).bind(9, position++)
         .bind(10, std::string(qrFormatCode(a.qrFormat)));
        if (!s.done()) { err_ = s.error(); if (owns) exec("ROLLBACK"); return false; }
        a.id = sqlite3_last_insert_rowid(db_);
    }

    if (owns && !exec("COMMIT")) { exec("ROLLBACK"); return false; }
    return true;
}

bool Database::deleteCompany(int64_t id) {
    if (invoiceCountFor(id) > 0) {
        err_ = "firma má doklady a nedá sa odstrániť";
        return false;
    }
    // Ask before deleting: afterwards activeCompanyId() falls back to another
    // company, so the comparison could never be true.
    const bool wasActive = (activeCompanyId() == id);

    Stmt s(db_, "DELETE FROM company WHERE id = ?1");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, id);
    if (!s.done()) { err_ = s.error(); return false; }

    if (wasActive) setActiveCompany(activeCompanyId());   // the fallback picks a survivor
    return true;
}

// ---------------------------------------------------------------- customers
std::vector<Customer> Database::customers(bool includeArchived) {
    std::vector<Customer> out;
    std::string sql = std::string("SELECT id, ") + PARTY_COLS + ", note, archived FROM customers";
    if (!includeArchived) sql += " WHERE archived = 0";
    sql += " ORDER BY name COLLATE NOCASE";

    Stmt s(db_, sql);
    if (!s) { err_ = s.error(); return out; }
    while (s.step()) {
        Customer c;
        c.id = s.i64(0);
        readParty(s, 1, c);
        c.note     = s.text(15);
        c.archived = s.i32(16) != 0;
        out.push_back(std::move(c));
    }
    return out;
}

bool Database::loadCustomer(int64_t id, Customer& out) {
    Stmt s(db_, std::string("SELECT id, ") + PARTY_COLS +
                ", note, archived FROM customers WHERE id = ?1");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, id);
    if (!s.step()) return false;
    out.id = s.i64(0);
    readParty(s, 1, out);
    out.note     = s.text(15);
    out.archived = s.i32(16) != 0;
    return true;
}

bool Database::saveCustomer(Customer& c) {
    if (c.id == 0) {
        Stmt s(db_, std::string("INSERT INTO customers (") + PARTY_COLS +
                    ", note, archived) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16)");
        if (!s) { err_ = s.error(); return false; }
        bindParty(s, 1, c);
        s.bind(15, c.note).bind(16, c.archived ? 1 : 0);
        if (!s.done()) { err_ = s.error(); return false; }
        c.id = sqlite3_last_insert_rowid(db_);
        return true;
    }
    Stmt s(db_,
        "UPDATE customers SET name=?1, ico=?2, dic=?3, ic_dph=?4, street=?5, street2=?6, "
        "city=?7, postal_code=?8, country_code=?9, email=?10, phone=?11, contact_name=?12, "
        "endpoint_scheme=?13, endpoint_id=?14, note=?15, archived=?16 WHERE id=?17");
    if (!s) { err_ = s.error(); return false; }
    bindParty(s, 1, c);
    s.bind(15, c.note).bind(16, c.archived ? 1 : 0).bind(17, c.id);
    if (!s.done()) { err_ = s.error(); return false; }
    return true;
}

bool Database::deleteCustomer(int64_t id) {
    Stmt s(db_, "UPDATE customers SET archived = 1 WHERE id = ?1");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, id);
    return s.done();
}

// ----------------------------------------------------------------- invoices
std::vector<Database::InvoiceSummary> Database::invoiceList() {
    std::vector<InvoiceSummary> out;

    // Totals are not stored, so they can never drift from the lines. They are
    // also not computed in SQL: rounding VAT per category is subtle enough that
    // a second implementation would eventually disagree with the invoice
    // itself. Two queries and the real Invoice::totals() instead.
    std::vector<Invoice> shells;
    Stmt s(db_,
        "SELECT i.id, i.number, i.issue_date, i.due_date, i.name, i.currency, i.doc_type, "
        "       i.state, i.prepaid_amount, i.vat_exemption_reason, i.variable_symbol, "
        "       i.rounding_amount, "
        "       (SELECT COALESCE(SUM(p.amount), 0) FROM payments p WHERE p.invoice_id = i.id) "
        "FROM invoices i WHERE i.company_id = ?1 "
        "ORDER BY i.issue_date DESC, i.id DESC");
    if (!s) { err_ = s.error(); return out; }
    s.bind(1, activeCompanyId());

    while (s.step()) {
        InvoiceSummary v;
        v.id        = s.i64(0);
        v.number    = s.text(1);
        v.issueDate = s.text(2);
        v.dueDate   = s.text(3);
        v.buyerName = s.text(4);
        v.currency  = s.text(5);
        v.type      = static_cast<DocType>(s.i32(6));
        v.state     = stateFromCode(s.text(7));
        v.variableSymbol = s.text(10);
        v.paid           = s.dec(12);      // the payments subquery moved along one
        out.push_back(std::move(v));

        Invoice shell;
        shell.id                 = out.back().id;
        shell.prepaidAmount      = s.dec(8);
        shell.vatExemptionReason = s.text(9);
        shell.roundingAmount     = s.dec(11);
        shells.push_back(std::move(shell));
    }

    Stmt lines(db_, "SELECT invoice_id, quantity, unit_price, vat_rate, vat_category, line_no "
                    "FROM invoice_lines ORDER BY invoice_id, line_no");
    if (lines) {
        size_t at = 0;
        while (lines.step()) {
            const int64_t invoiceId = lines.i64(0);
            // The two result sets are in different orders, so find the shell.
            if (at >= shells.size() || shells[at].id != invoiceId) {
                at = shells.size();
                for (size_t i = 0; i < shells.size(); ++i)
                    if (shells[i].id == invoiceId) { at = i; break; }
                if (at == shells.size()) continue;
            }
            InvoiceLine l;
            l.lineNo      = lines.i32(5);
            l.quantity    = lines.dec(1);
            l.unitPrice   = lines.dec(2);
            l.vatRate     = lines.dec(3);
            l.vatCategory = lines.text(4);
            shells[at].lines.push_back(std::move(l));
        }
    }

    // Document-level discounts and surcharges, and the rounding. Without
    // these the list shows the total *before* a discount while the PDF shows
    // the amount after it — and the dashboard sums the wrong one.
    Stmt adj(db_,
        "SELECT invoice_id, is_charge, amount, vat_category, vat_rate, line_no "
        "FROM document_allowances ORDER BY invoice_id, position, id");
    if (adj) {
        while (adj.step()) {
            const int64_t invoiceId = adj.i64(0);
            size_t at = shells.size();
            for (size_t i = 0; i < shells.size(); ++i)
                if (shells[i].id == invoiceId) { at = i; break; }
            if (at == shells.size()) continue;

            Allowance a;
            a.isCharge    = adj.i32(1) != 0;
            a.amount      = adj.dec(2);
            a.vatCategory = adj.text(3);
            a.vatRate     = adj.dec(4);

            // On the right level, or a line surcharge would be counted as a
            // document one and the listed total would drift from the PDF.
            const int lineNo = adj.i32(5);
            InvoiceLine* owner = nullptr;
            if (lineNo > 0)
                for (InvoiceLine& l : shells[at].lines)
                    if (l.lineNo == lineNo) { owner = &l; break; }
            if (owner) owner->allowances.push_back(std::move(a));
            else       shells[at].allowances.push_back(std::move(a));
        }
    }

    for (size_t i = 0; i < out.size() && i < shells.size(); ++i)
        out[i].total = shells[i].totals().payable;

    return out;
}

bool Database::loadInvoice(int64_t id, Invoice& out) {
    Stmt s(db_,
        std::string("SELECT id, number, doc_type, issue_date, tax_point_date, due_date, currency, ") +
        PARTY_COLS + ", buyer_reference, order_reference, note, variable_symbol, constant_symbol, "
        "specific_symbol, payment_means_code, payment_terms, preceding_number, preceding_date, "
        "vat_exemption_reason, prepaid_amount, state, issued_at, company_id, " +
        sellerSnapshotColumnList() + ", rounding_amount, vat_accounting_currency, "
        "exchange_rate, exchange_rate_date FROM invoices WHERE id = ?1");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, id);
    if (!s.step()) return false;

    out = Invoice{};
    out.id           = s.i64(0);
    out.number       = s.text(1);
    out.type         = static_cast<DocType>(s.i32(2));
    out.issueDate    = s.text(3);
    out.taxPointDate = s.text(4);
    out.dueDate      = s.text(5);
    out.currency     = s.text(6);
    readParty(s, 7, out.buyer);
    out.buyerReference     = s.text(21);
    out.orderReference     = s.text(22);
    out.note               = s.text(23);
    out.variableSymbol     = s.text(24);
    out.constantSymbol     = s.text(25);
    out.specificSymbol     = s.text(26);
    out.paymentMeansCode   = s.text(27);
    out.paymentTerms       = s.text(28);
    out.precedingNumber    = s.text(29);
    out.precedingDate      = s.text(30);
    out.vatExemptionReason = s.text(31);
    out.prepaidAmount      = s.dec(32);
    out.state              = stateFromCode(s.text(33));
    out.issuedAt           = s.text(34);
    out.companyId          = s.i64(35);

    // The seller as it was, not as it is now.
    Company& seller             = out.seller;
    seller.id                   = out.companyId;
    seller.name                 = s.text(36);
    seller.ico                  = s.text(37);
    seller.dic                  = s.text(38);
    seller.icDph                = s.text(39);
    seller.address.street       = s.text(40);
    seller.address.street2      = s.text(41);
    seller.address.city         = s.text(42);
    seller.address.postalCode   = s.text(43);
    seller.address.countryCode  = s.text(44);
    seller.email                = s.text(45);
    seller.phone                = s.text(46);
    seller.contactName          = s.text(47);
    seller.endpointScheme       = s.text(48);
    seller.endpointId           = s.text(49);
    seller.iban                 = s.text(50);
    seller.bic                  = s.text(51);
    seller.bankName             = s.text(52);
    seller.registryNote         = s.text(53);
    seller.vatMode              = vatModeFromCode(s.text(54));
    seller.bankLocalNumber      = s.text(55);
    seller.qrFormat             = qrFormatFromCode(s.text(56));
    out.roundingAmount          = s.dec(57);
    out.vatAccountingCurrency   = s.text(58);
    out.exchangeRate            = s.dec(59);
    out.exchangeRateDate        = s.text(60);

    Stmt ls(db_,
        "SELECT id, line_no, description, unit, unit_code, quantity, unit_price, vat_rate, "
        "vat_category FROM invoice_lines WHERE invoice_id = ?1 ORDER BY line_no");
    if (!ls) { err_ = ls.error(); return false; }
    ls.bind(1, id);
    while (ls.step()) {
        InvoiceLine l;
        l.id          = ls.i64(0);
        l.lineNo      = ls.i32(1);
        l.description = ls.text(2);
        l.unit        = ls.text(3);
        l.unitCodeUn  = ls.text(4);
        l.quantity    = ls.dec(5);
        l.unitPrice   = ls.dec(6);
        l.vatRate     = ls.dec(7);
        l.vatCategory = ls.text(8);
        out.lines.push_back(std::move(l));
    }
    Stmt as(db_,
        "SELECT id, is_charge, reason, percentage, base_amount, amount, vat_category, vat_rate, "
        "line_no FROM document_allowances WHERE invoice_id = ?1 ORDER BY position, id");
    if (!as) { err_ = as.error(); return false; }
    as.bind(1, id);
    while (as.step()) {
        Allowance a;
        a.id          = as.i64(0);
        a.isCharge    = as.i32(1) != 0;
        a.reason      = as.text(2);
        a.percentage  = as.dec(3);
        a.baseAmount  = as.dec(4);
        a.amount      = as.dec(5);
        a.vatCategory = as.text(6);
        a.vatRate     = as.dec(7);

        // Zero is the document; anything else names a line. A row pointing at
        // a line that is no longer there would otherwise vanish from the
        // arithmetic while still sitting in the table, so it falls back to the
        // document rather than being silently dropped.
        const int lineNo = as.i32(8);
        InvoiceLine* owner = nullptr;
        if (lineNo > 0)
            for (InvoiceLine& l : out.lines)
                if (l.lineNo == lineNo) { owner = &l; break; }
        if (owner) owner->allowances.push_back(std::move(a));
        else       out.allowances.push_back(std::move(a));
    }

    out.payments = paymentsFor(id);

    auto numberOf = [this](int64_t docId) {
        Stmt n(db_, "SELECT number FROM invoices WHERE id = ?1");
        if (!n) return std::string();
        n.bind(1, docId);
        return n.step() ? n.text(0) : std::string();
    };
    for (int64_t proformaId : proformasSettledBy(id)) {
        const std::string number = numberOf(proformaId);
        if (!number.empty()) out.relatedProformaNumbers.push_back(number);
    }
    if (const int64_t settledBy = invoiceSettling(id))
        out.settledByNumber = numberOf(settledBy);

    return true;
}

namespace {

/// Binds the nineteen seller-snapshot values starting at `first`.
void bindSeller(Stmt& s, int first, const Company& c) {
    s.bind(first + 0,  c.name);
    s.bind(first + 1,  c.ico);
    s.bind(first + 2,  c.dic);
    s.bind(first + 3,  c.icDph);
    s.bind(first + 4,  c.address.street);
    s.bind(first + 5,  c.address.street2);
    s.bind(first + 6,  c.address.city);
    s.bind(first + 7,  c.address.postalCode);
    s.bind(first + 8,  c.address.countryCode);
    s.bind(first + 9,  c.email);
    s.bind(first + 10, c.phone);
    s.bind(first + 11, c.contactName);
    s.bind(first + 12, c.endpointScheme);
    s.bind(first + 13, c.endpointId);
    s.bind(first + 14, c.iban);
    s.bind(first + 15, c.bic);
    s.bind(first + 16, c.bankName);
    s.bind(first + 17, c.registryNote);
    s.bind(first + 18, std::string(vatModeCode(c.vatMode)));
    s.bind(first + 19, c.bankLocalNumber);
    s.bind(first + 20, std::string(qrFormatCode(c.qrFormat)));
}

} // namespace

bool Database::saveInvoice(Invoice& inv) {
    // A locked document is a legal record. Changing it silently is exactly what
    // this whole feature exists to prevent, so the guard lives here in the
    // storage layer rather than only in the GUI.
    if (inv.id != 0) {
        Stmt current(db_, "SELECT state FROM invoices WHERE id = ?1");
        if (current) {
            current.bind(1, inv.id);
            if (current.step() && stateFromCode(current.text(0)) != InvoiceState::Draft) {
                err_ = "doklad je vystavený a nedá sa upraviť";
                return false;
            }
        }
    }

    // A draft always carries the current details of its company, so a
    // duplicate of a two-year-old invoice does not go out with the address of
    // two years ago. Issuing freezes them, because an issued document can
    // never be saved again.
    if (inv.companyId == 0) inv.companyId = activeCompanyId();
    if (inv.state == InvoiceState::Draft || inv.seller.name.empty()) {
        loadCompany(inv.companyId, inv.seller);
        // Which account this invoice is payable to is part of the snapshot: a
        // CZK invoice must name the CZK account, not whichever one happens to
        // be the company default.
        inv.seller.useAccountFor(inv.currency);
    }

    // Nesting-safe: only own the transaction if we actually started it.
    const bool ownsTransaction = db_ && sqlite3_get_autocommit(db_) != 0;
    if (ownsTransaction && !exec("BEGIN")) return false;
    auto rollback = [&] { if (ownsTransaction) exec("ROLLBACK"); };

    if (inv.id == 0) {
        Stmt s(db_,
            std::string("INSERT INTO invoices (number, doc_type, issue_date, tax_point_date, "
            "due_date, currency, ") + PARTY_COLS + ", buyer_reference, order_reference, note, "
            "variable_symbol, constant_symbol, specific_symbol, payment_means_code, payment_terms, "
            "preceding_number, preceding_date, vat_exemption_reason, prepaid_amount, "
            "company_id, " + sellerSnapshotColumnList() + ", rounding_amount, "
            "vat_accounting_currency, exchange_rate, exchange_rate_date) VALUES "
            "(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,"
            "?21,?22,?23,?24,?25,?26,?27,?28,?29,?30,?31,?32,?33,"
            "?34,?35,?36,?37,?38,?39,?40,?41,?42,?43,?44,?45,?46,?47,?48,?49,?50,?51,?52,?53,?54,"
            "?55,?56,?57,?58)");
        if (!s) { err_ = s.error(); rollback(); return false; }
        s.bind(1, inv.number).bind(2, static_cast<int>(inv.type))
         .bind(3, inv.issueDate).bind(4, inv.taxPointDate).bind(5, inv.dueDate)
         .bind(6, inv.currency);
        bindParty(s, 7, inv.buyer);
        s.bind(21, inv.buyerReference).bind(22, inv.orderReference).bind(23, inv.note)
         .bind(24, inv.variableSymbol).bind(25, inv.constantSymbol).bind(26, inv.specificSymbol)
         .bind(27, inv.paymentMeansCode).bind(28, inv.paymentTerms)
         .bind(29, inv.precedingNumber).bind(30, inv.precedingDate)
         .bind(31, inv.vatExemptionReason).bind(32, inv.prepaidAmount)
         .bind(33, inv.companyId);
        bindSeller(s, 34, inv.seller);
        s.bind(55, inv.roundingAmount)
         .bind(56, inv.vatAccountingCurrency).bind(57, inv.exchangeRate)
         .bind(58, inv.exchangeRateDate);
        if (!s.done()) { err_ = s.error(); rollback(); return false; }
        inv.id = sqlite3_last_insert_rowid(db_);
        // A new row is always a draft, whatever the caller had in memory: a
        // duplicate or a credit note copied from an issued document starts
        // over.
        inv.state = InvoiceState::Draft;
        inv.issuedAt.clear();
    } else {
        Stmt s(db_,
            "UPDATE invoices SET number=?1, doc_type=?2, issue_date=?3, tax_point_date=?4, "
            "due_date=?5, currency=?6, name=?7, ico=?8, dic=?9, ic_dph=?10, street=?11, "
            "street2=?12, city=?13, postal_code=?14, country_code=?15, email=?16, phone=?17, "
            "contact_name=?18, endpoint_scheme=?19, endpoint_id=?20, buyer_reference=?21, "
            "order_reference=?22, note=?23, variable_symbol=?24, constant_symbol=?25, "
            "specific_symbol=?26, payment_means_code=?27, payment_terms=?28, preceding_number=?29, "
            "preceding_date=?30, vat_exemption_reason=?31, prepaid_amount=?32, company_id=?33, "
        "s_name=?34, s_ico=?35, s_dic=?36, s_ic_dph=?37, s_street=?38, s_street2=?39, "
        "s_city=?40, s_postal_code=?41, s_country_code=?42, s_email=?43, s_phone=?44, "
        "s_contact_name=?45, s_endpoint_scheme=?46, s_endpoint_id=?47, s_iban=?48, "
        "s_bic=?49, s_bank_name=?50, s_registry_note=?51, s_vat_mode=?52, "
        "s_bank_local=?53, s_qr_format=?54, rounding_amount=?55, "
        "vat_accounting_currency=?56, exchange_rate=?57, exchange_rate_date=?58 "
        "WHERE id=?59");
        if (!s) { err_ = s.error(); rollback(); return false; }
        s.bind(1, inv.number).bind(2, static_cast<int>(inv.type))
         .bind(3, inv.issueDate).bind(4, inv.taxPointDate).bind(5, inv.dueDate)
         .bind(6, inv.currency);
        bindParty(s, 7, inv.buyer);
        s.bind(21, inv.buyerReference).bind(22, inv.orderReference).bind(23, inv.note)
         .bind(24, inv.variableSymbol).bind(25, inv.constantSymbol).bind(26, inv.specificSymbol)
         .bind(27, inv.paymentMeansCode).bind(28, inv.paymentTerms)
         .bind(29, inv.precedingNumber).bind(30, inv.precedingDate)
         .bind(31, inv.vatExemptionReason).bind(32, inv.prepaidAmount)
         .bind(33, inv.companyId);
        bindSeller(s, 34, inv.seller);
        s.bind(55, inv.roundingAmount)
         .bind(56, inv.vatAccountingCurrency).bind(57, inv.exchangeRate)
         .bind(58, inv.exchangeRateDate);
        s.bind(59, inv.id);
        if (!s.done()) { err_ = s.error(); rollback(); return false; }

        Stmt d(db_, "DELETE FROM invoice_lines WHERE invoice_id = ?1");
        d.bind(1, inv.id);
        d.done();
    }

    int no = 0;
    for (InvoiceLine& l : inv.lines) {
        l.lineNo = ++no;
        Stmt s(db_,
            "INSERT INTO invoice_lines (invoice_id, line_no, description, unit, unit_code, "
            "quantity, unit_price, vat_rate, vat_category) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9)");
        if (!s) { err_ = s.error(); rollback(); return false; }
        s.bind(1, inv.id).bind(2, l.lineNo).bind(3, l.description).bind(4, l.unit)
         .bind(5, l.unitCodeUn).bind(6, l.quantity).bind(7, l.unitPrice)
         .bind(8, l.vatRate).bind(9, l.vatCategory);
        if (!s.done()) { err_ = s.error(); rollback(); return false; }
    }

    // Replaced wholesale, like the lines: the editor hands over the document
    // as it should now be, not a list of edits.
    {
        Stmt d(db_, "DELETE FROM document_allowances WHERE invoice_id = ?1");
        if (!d) { err_ = d.error(); rollback(); return false; }
        d.bind(1, inv.id);
        if (!d.done()) { err_ = d.error(); rollback(); return false; }
    }
    int position = 0;
    auto writeAllowance = [&](const Allowance& a, int lineNo) {
        Stmt s(db_,
            "INSERT INTO document_allowances (invoice_id, position, is_charge, reason, "
            "percentage, base_amount, amount, vat_category, vat_rate, line_no) "
            "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)");
        if (!s) { err_ = s.error(); return false; }
        s.bind(1, inv.id).bind(2, position++).bind(3, a.isCharge ? 1 : 0)
         .bind(4, a.reason).bind(5, a.percentage).bind(6, a.baseAmount)
         .bind(7, a.amount).bind(8, a.vatCategory).bind(9, a.vatRate)
         .bind(10, lineNo);
        if (!s.done()) { err_ = s.error(); return false; }
        return true;
    };
    for (const InvoiceLine& l : inv.lines)
        for (const Allowance& a : l.allowances)
            if (!writeAllowance(a, l.lineNo)) { rollback(); return false; }
    for (const Allowance& a : inv.allowances)
        if (!writeAllowance(a, 0)) { rollback(); return false; }

    if (ownsTransaction && !exec("COMMIT")) { rollback(); return false; }
    return true;
}

bool Database::deleteInvoice(int64_t id) {
    // Lines and payments cascade; the audit trail has no foreign key on
    // purpose (it must survive its subject), so it is cleared here.
    Stmt audit(db_, "DELETE FROM invoice_audit WHERE invoice_id = ?1");
    if (audit) { audit.bind(1, id); audit.done(); }
    Stmt allowances(db_, "DELETE FROM document_allowances WHERE invoice_id = ?1");
    if (allowances) { allowances.bind(1, id); allowances.done(); }
    Stmt archive(db_, "DELETE FROM issued_documents WHERE invoice_id = ?1");
    if (archive) { archive.bind(1, id); archive.done(); }

    Stmt s(db_, "DELETE FROM invoices WHERE id = ?1");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, id);
    return s.done();
}

// ------------------------------------------------- bank statement import

std::vector<PayableInvoice> Database::payableInvoices() {
    std::vector<PayableInvoice> out;
    // Drafts have no final number and cannot have been paid; proformas are not
    // tax documents but they are certainly paid, so they stay.
    Stmt s(db_,
        "SELECT i.id, i.number, i.variable_symbol, i.currency, i.state, "
        "  COALESCE((SELECT SUM(p.amount) FROM payments p WHERE p.invoice_id = i.id), 0) "
        "FROM invoices i WHERE i.company_id = ?1 AND i.state <> 'draft' "
        "ORDER BY i.id");
    if (!s) { err_ = s.error(); return out; }
    s.bind(1, activeCompanyId());

    while (s.step()) {
        PayableInvoice inv;
        inv.id             = s.i64(0);
        inv.number         = s.text(1);
        inv.variableSymbol = s.text(2);
        inv.currency       = s.text(3);
        inv.cancelled      = s.text(4) == "cancelled";
        inv.paid           = s.dec(5);
        out.push_back(std::move(inv));
    }

    // The total is the sum of the lines and their VAT, which lives in the
    // model rather than in SQL. Loading each document is slower than a clever
    // query and cannot drift from what the invoice itself says it is owed.
    for (PayableInvoice& inv : out) {
        Invoice full;
        if (loadInvoice(inv.id, full)) inv.payable = full.totals().payable;
    }
    return out;
}

std::vector<PayableInvoice> Database::payableReceivedInvoices() {
    std::vector<PayableInvoice> out;
    for (const ReceivedInvoice& row : receivedInvoices()) {
        // Settled ones are left out rather than flagged: unlike a cancelled
        // invoice of your own, there is nothing to report about a supplier's
        // invoice you have already paid, and offering it invites paying twice.
        if (row.isSettled()) continue;

        PayableInvoice inv;
        inv.id             = row.id;
        inv.number         = row.number;
        inv.variableSymbol = row.variableSymbol;
        inv.currency       = row.currency;
        inv.payable        = row.payable;
        inv.paid           = row.paidAmount;
        inv.counterAccount = row.iban;
        out.push_back(std::move(inv));
    }
    return out;
}

std::vector<std::string> Database::importedTransactionIds() {
    std::vector<std::string> out;
    Stmt s(db_, "SELECT bank_id FROM imported_transactions WHERE company_id = ?1");
    if (!s) { err_ = s.error(); return out; }
    s.bind(1, activeCompanyId());
    while (s.step()) out.push_back(s.text(0));
    return out;
}

bool Database::recordStatementImport(const std::vector<ImportedPayment>& payments,
                                     const std::string& sourceFile,
                                     const std::string& sourceSha) {
    if (payments.empty()) return true;

    const bool ownsTransaction = db_ && sqlite3_get_autocommit(db_) != 0;
    if (ownsTransaction && !exec("BEGIN")) return false;
    auto rollback = [&] { if (ownsTransaction) exec("ROLLBACK"); };

    const int64_t company = activeCompanyId();
    const std::string now = sk::nowTimestamp();

    // Two movements in one file that identify themselves the same way. It
    // happens: a statement without the bank's own movement id falls back to a
    // digest of date, amount, counterparty and symbol, and two identical
    // transfers to one supplier on one day collapse to the same key. The
    // INSERT below would refuse the second with a constraint error nobody can
    // act on, so the collision is named here instead.
    {
        std::set<std::string> keys;
        for (const ImportedPayment& item : payments)
            if (!keys.insert(item.bankId).second) {
                err_ = "Vo výpise sú dva pohyby s rovnakým identifikátorom (" + item.bankId +
                       "). Zaúčtujte ich po jednom.";
                rollback();
                return false;
            }
    }

    for (const ImportedPayment& item : payments) {
        if (item.invoiceId != 0 && item.receivedId != 0) {
            err_ = "Pohyb nemôže platiť vydanú aj prijatú faktúru naraz.";
            rollback();
            return false;
        }

        int64_t paymentId = 0;
        if (item.invoiceId != 0) {
            Payment p;
            p.invoiceId = item.invoiceId;
            p.paidOn    = item.paidOn;
            p.amount    = item.amount;
            p.note      = item.note;
            if (!addPayment(p)) { rollback(); return false; }
            paymentId = p.id;
        }
        if (item.receivedId != 0) {
            // A received invoice carries one figure rather than a list of
            // payments, so a second movement against the same one adds to it.
            // The date is the latest, which is the day it was actually settled.
            ReceivedInvoice row;
            if (!loadReceivedInvoice(item.receivedId, row)) {
                err_ = "Prijatá faktúra sa nenašla.";
                rollback();
                return false;
            }
            const Dec total = row.paidAmount + item.amount;
            const std::string when =
                row.paidOn.empty() || item.paidOn > row.paidOn ? item.paidOn : row.paidOn;
            if (!setReceivedPaid(item.receivedId, when, total)) { rollback(); return false; }
        }

        // Every movement gets a guard row. BankTransaction::dedupKey() always
        // produces something, so there is no such thing as an unguarded
        // payment that a second import could duplicate.
        if (item.bankId.empty()) { err_ = "Pohyb bez identifikátora."; rollback(); return false; }

        // INSERT, not INSERT OR IGNORE: reaching here with a movement already
        // recorded means the caller skipped its own guard, and paying twice is
        // worse than refusing the import.
        Stmt s(db_,
            "INSERT INTO imported_transactions "
            "(company_id, bank_id, invoice_id, payment_id, imported_at, source_file, "
            "source_sha, received_id) VALUES (?1,?2,?3,?4,?5,?6,?7,?8)");
        if (!s) { err_ = s.error(); rollback(); return false; }
        s.bind(1, company).bind(2, item.bankId).bind(3, item.invoiceId)
         .bind(4, paymentId).bind(5, now).bind(6, sourceFile).bind(7, sourceSha)
         .bind(8, item.receivedId);
        if (!s.done()) { err_ = s.error(); rollback(); return false; }
    }

    if (ownsTransaction && !exec("COMMIT")) { rollback(); return false; }
    return true;
}

// --------------------------------------------------------------- testing only
bool Database::forceDeleteInvoice(int64_t id) {
    return deleteInvoice(id);
}

int Database::forceDeleteAllInvoices() {
    // Scoped to the active company, matching the count the confirmation shows.
    // A wipe that silently reached into another company's records would be a
    // nasty surprise from a button labelled with one company's document count.
    const int64_t company = activeCompanyId();
    const int removed = invoiceCountFor(company);
    const std::string mine =
        "SELECT id FROM invoices WHERE company_id = " + std::to_string(company);

    if (!exec("BEGIN")) return 0;
    const bool ok = exec("DELETE FROM invoice_audit WHERE invoice_id IN (" + mine + ")") &&
                    exec("DELETE FROM payments WHERE invoice_id IN (" + mine + ")") &&
                    exec("DELETE FROM document_allowances WHERE invoice_id IN (" + mine + ")") &&
                    exec("DELETE FROM invoice_lines WHERE invoice_id IN (" + mine + ")") &&
                    exec("DELETE FROM document_links WHERE invoice_id IN (" + mine + ")") &&
                    exec("DELETE FROM issued_documents WHERE invoice_id IN (" + mine + ")") &&
                    exec("DELETE FROM invoices WHERE company_id = " + std::to_string(company));
    if (!ok) { exec("ROLLBACK"); return 0; }
    if (!exec("COMMIT")) { exec("ROLLBACK"); return 0; }
    return removed;
}

bool Database::InvoiceSummary::isOverdue(const std::string& todayIso) const {
    if (state != InvoiceState::Issued) return false;
    if (dueDate.empty() || isFullyPaid()) return false;
    return dueDate < todayIso;                   // ISO dates compare lexically
}

// ------------------------------------------------------------- lifecycle
bool Database::issueInvoice(int64_t id, const std::string& number) {
    Stmt s(db_, "UPDATE invoices SET state = 'issued', issued_at = ?1, number = ?2 "
                "WHERE id = ?3 AND state = 'draft'");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, sk::nowTimestamp()).bind(2, number).bind(3, id);
    if (!s.done()) { err_ = s.error(); return false; }
    if (sqlite3_changes(db_) == 0) { err_ = "doklad už nie je rozpracovaný"; return false; }
    return recordAudit(id, "vystavenie", "číslo " + number);
}

bool Database::issueInvoiceWithDocument(int64_t id, const std::string& number,
                                        const std::string& pdf, const std::string& ubl) {
    if (!db_) { err_ = "databáza nie je otvorená"; return false; }

    const bool owns = sqlite3_get_autocommit(db_) != 0;
    if (owns && !exec("BEGIN")) return false;
    auto fail = [&](const std::string& why) {
        err_ = why.empty() ? err_ : why;
        if (owns) exec("ROLLBACK");
        return false;
    };

    // One timestamp for the whole event, so the invoice and its archived form
    // cannot disagree about when it was issued.
    const std::string at = sk::nowTimestamp();

    Stmt s(db_, "UPDATE invoices SET state = 'issued', issued_at = ?1, number = ?2 "
                "WHERE id = ?3 AND state = 'draft'");
    if (!s) return fail(s.error());
    s.bind(1, at).bind(2, number).bind(3, id);
    if (!s.done()) return fail(s.error());
    if (sqlite3_changes(db_) == 0) return fail("doklad už nie je rozpracovaný");

    if (!recordAudit(id, "vystavenie", "číslo " + number)) return fail({});
    if (!storeIssuedDocument(id, pdf, ubl, at)) return fail({});

    if (owns && !exec("COMMIT")) return fail({});
    return true;
}

bool Database::unlockInvoice(int64_t id, const std::string& reason) {
    Stmt s(db_, "UPDATE invoices SET state = 'draft' WHERE id = ?1 AND state = 'issued'");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, id);
    if (!s.done()) { err_ = s.error(); return false; }
    if (sqlite3_changes(db_) == 0) { err_ = "doklad nie je vystavený"; return false; }
    return recordAudit(id, "odomknutie", reason);
}

bool Database::cancelInvoice(int64_t id, const std::string& detail) {
    Stmt s(db_, "UPDATE invoices SET state = 'cancelled' WHERE id = ?1 AND state <> 'cancelled'");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, id);
    if (!s.done()) { err_ = s.error(); return false; }
    if (sqlite3_changes(db_) == 0) { err_ = "doklad je už stornovaný"; return false; }
    return recordAudit(id, "storno", detail);
}

bool Database::recordAudit(int64_t invoiceId, const std::string& action,
                           const std::string& detail) {
    Stmt s(db_, "INSERT INTO invoice_audit (invoice_id, at, action, detail) "
                "VALUES (?1, ?2, ?3, ?4)");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, invoiceId).bind(2, sk::nowTimestamp()).bind(3, action).bind(4, detail);
    if (!s.done()) { err_ = s.error(); return false; }
    return true;
}

std::vector<Database::AuditEntry> Database::auditFor(int64_t invoiceId) {
    std::vector<AuditEntry> out;
    Stmt s(db_, "SELECT at, action, detail FROM invoice_audit WHERE invoice_id = ?1 "
                "ORDER BY id");
    if (!s) { err_ = s.error(); return out; }
    s.bind(1, invoiceId);
    while (s.step()) out.push_back({s.text(0), s.text(1), s.text(2)});
    return out;
}

std::string Database::highestIssuedNumber() {
    Stmt s(db_, "SELECT number FROM invoices WHERE state <> 'draft' "
                "ORDER BY LENGTH(number) DESC, number DESC LIMIT 1");
    if (!s || !s.step()) return {};
    return s.text(0);
}

// -------------------------------------------------------------- payments
std::vector<Payment> Database::paymentsFor(int64_t invoiceId) {
    std::vector<Payment> out;
    Stmt s(db_, "SELECT id, invoice_id, paid_on, amount, note FROM payments "
                "WHERE invoice_id = ?1 ORDER BY paid_on, id");
    if (!s) { err_ = s.error(); return out; }
    s.bind(1, invoiceId);
    while (s.step()) {
        Payment p;
        p.id        = s.i64(0);
        p.invoiceId = s.i64(1);
        p.paidOn    = s.text(2);
        p.amount    = s.dec(3);
        p.note      = s.text(4);
        out.push_back(std::move(p));
    }
    return out;
}

bool Database::addPayment(Payment& p) {
    Stmt s(db_, "INSERT INTO payments (invoice_id, paid_on, amount, note) VALUES (?1,?2,?3,?4)");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, p.invoiceId).bind(2, p.paidOn).bind(3, p.amount).bind(4, p.note);
    if (!s.done()) { err_ = s.error(); return false; }
    p.id = sqlite3_last_insert_rowid(db_);
    return recordAudit(p.invoiceId, "platba", p.amount.toString(2) + " dňa " + p.paidOn);
}

bool Database::deletePayment(int64_t paymentId) {
    Stmt find(db_, "SELECT invoice_id, amount FROM payments WHERE id = ?1");
    int64_t invoiceId = 0;
    std::string amount;
    if (find) {
        find.bind(1, paymentId);
        if (find.step()) { invoiceId = find.i64(0); amount = find.dec(1).toString(2); }
    }
    Stmt s(db_, "DELETE FROM payments WHERE id = ?1");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, paymentId);
    if (!s.done()) { err_ = s.error(); return false; }
    if (invoiceId != 0) recordAudit(invoiceId, "zrušenie platby", amount);
    return true;
}

bool Database::invoiceNumberExists(const std::string& number, int64_t exceptId,
                                   int64_t companyId) {
    Stmt s(db_, "SELECT 1 FROM invoices WHERE number = ?1 AND id <> ?2 AND company_id = ?3");
    if (!s) return false;
    s.bind(1, number).bind(2, exceptId)
     .bind(3, companyId == 0 ? activeCompanyId() : companyId);
    return s.step();
}

// ------------------------------------------------------------------ settings
// ------------------------------------------------------------------ archive
bool Database::storeIssuedDocument(int64_t invoiceId, const std::string& pdf,
                                   const std::string& ubl, const std::string& at) {
    if (!db_) { err_ = "databáza nie je otvorená"; return false; }

    // MAX + 1, not COUNT + 1: if a row is ever removed, COUNT would hand back
    // a version number that is already in use.
    int version = 1;
    {
        Stmt next(db_, "SELECT COALESCE(MAX(version), 0) + 1 FROM issued_documents "
                       "WHERE invoice_id = ?1");
        if (next) { next.bind(1, invoiceId); if (next.step()) version = next.i32(0); }
    }

    Stmt s(db_, "INSERT INTO issued_documents "
                "(invoice_id, version, issued_at, pdf, ubl, pdf_sha256, ubl_sha256) "
                "VALUES (?1,?2,?3,?4,?5,?6,?7)");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, invoiceId).bind(2, version).bind(3, at.empty() ? sk::nowTimestamp() : at);
    s.bindBlob(4, pdf);
    s.bind(5, ubl)
     .bind(6, pdf.empty() ? std::string() : Sha256::hexOf(pdf))
     .bind(7, ubl.empty() ? std::string() : Sha256::hexOf(ubl));
    if (!s.done()) { err_ = s.error(); return false; }
    return true;
}

bool Database::loadIssuedDocument(int64_t invoiceId, IssuedDocument& out) {
    Stmt s(db_, "SELECT version, issued_at, pdf, ubl, pdf_sha256, ubl_sha256 "
                "FROM issued_documents WHERE invoice_id = ?1 "
                "ORDER BY version DESC, id DESC LIMIT 1");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, invoiceId);
    if (!s.step()) return false;

    out.version  = s.i32(0);
    out.issuedAt = s.text(1);
    out.pdf      = s.blob(2);
    out.ubl      = s.text(3);
    out.pdfHash  = s.text(4);
    out.ublHash  = s.text(5);
    return true;
}

int Database::issuedVersionCount(int64_t invoiceId) {
    Stmt s(db_, "SELECT COUNT(*) FROM issued_documents WHERE invoice_id = ?1");
    if (!s) return 0;
    s.bind(1, invoiceId);
    return s.step() ? s.i32(0) : 0;
}

// ----------------------------------------------------------------- received
namespace {

/// The warnings, one per line. Stored as text rather than as a table: they
/// are read together, written once and never queried, and a second table
/// would be machinery for nothing.
std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        if (!out.empty()) out += "\n";
        out += line;
    }
    return out;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        const std::string line =
            text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty()) out.push_back(line);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

const char* RECEIVED_COLUMNS =
    "id, company_id, number, issue_date, due_date, tax_point_date, supplier_name, "
    "supplier_ico, supplier_ic_dph, supplier_country, currency, payable, tax_amount, "
    "variable_symbol, iban, received_at, source, sha256, warnings, note, paid_on, "
    "paid_amount";

void readReceived(Stmt& s, Database::ReceivedInvoice& out) {
    out.id                       = s.i64(0);
    out.companyId                = s.i64(1);
    out.number                   = s.text(2);
    out.issueDate                = s.text(3);
    out.dueDate                  = s.text(4);
    out.taxPointDate             = s.text(5);
    out.supplier.name            = s.text(6);
    out.supplier.ico             = s.text(7);
    out.supplier.icDph           = s.text(8);
    out.supplier.address.countryCode = s.text(9);
    out.currency                 = s.text(10);
    out.payable                  = s.dec(11);
    out.taxAmount                = s.dec(12);
    out.variableSymbol           = s.text(13);
    out.iban                     = s.text(14);
    out.receivedAt               = s.text(15);
    out.source                   = s.text(16);
    out.digest                   = s.text(17);
    out.warnings                 = splitLines(s.text(18));
    out.note                     = s.text(19);
    out.paidOn                   = s.text(20);
    out.paidAmount               = s.dec(21);
}

} // namespace

bool Database::importReceivedInvoice(const std::string& xml, const std::string& source,
                                     ReceivedInvoice& out, bool* alreadyHere) {
    if (!db_) { err_ = "databáza nie je otvorená"; return false; }
    if (alreadyHere) *alreadyHere = false;

    const std::string digest = Sha256::hexOf(xml);
    const int64_t company = activeCompanyId();

    // Already here? Then this import has nothing to do, and says so by
    // succeeding. Checked before parsing: the answer does not depend on
    // whether the document reads, and re-reading it would only risk a
    // different answer from a later version of the reader.
    auto findExisting = [&](ReceivedInvoice& row) {
        Stmt existing(db_, std::string("SELECT ") + RECEIVED_COLUMNS +
                           " FROM received_invoices WHERE company_id = ?1 AND sha256 = ?2");
        if (!existing) return false;
        existing.bind(1, company).bind(2, digest);
        if (!existing.step()) return false;
        readReceived(existing, row);
        return true;
    };
    if (findExisting(out)) {
        if (alreadyHere) *alreadyHere = true;
        return true;
    }

    const ReadResult read = readUbl(xml);
    if (!read.ok) { err_ = read.error; return false; }

    out = ReceivedInvoice{};
    out.companyId      = company;
    out.number         = read.invoice.number;
    out.issueDate      = read.invoice.issueDate;
    out.dueDate        = read.invoice.dueDate;
    out.taxPointDate   = read.invoice.taxPointDate;
    out.supplier.name  = read.invoice.seller.name;
    out.supplier.ico   = read.invoice.seller.ico;
    out.supplier.icDph = read.invoice.seller.icDph;
    out.supplier.address.countryCode = read.invoice.seller.address.countryCode;
    out.currency       = read.invoice.currency;
    // What the *sender* asks for, not what this application recomputes. A
    // document that disagrees with itself is stored with the figure it is
    // demanding and a warning saying so; showing our own arithmetic instead
    // would quietly answer a question nobody asked.
    out.payable        = read.stated.payable;
    out.taxAmount      = read.stated.taxAmount;
    out.variableSymbol = read.invoice.variableSymbol;
    out.iban           = read.invoice.seller.iban;
    out.receivedAt     = sk::nowTimestamp();
    out.source         = source;
    out.digest         = digest;
    out.warnings       = read.discrepancies;

    Stmt s(db_,
        "INSERT INTO received_invoices (company_id, number, issue_date, due_date, "
        "tax_point_date, supplier_name, supplier_ico, supplier_ic_dph, supplier_country, "
        "currency, payable, tax_amount, variable_symbol, iban, received_at, source, "
        "original, sha256, warnings) VALUES "
        "(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19)");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, out.companyId).bind(2, out.number).bind(3, out.issueDate)
     .bind(4, out.dueDate).bind(5, out.taxPointDate)
     .bind(6, out.supplier.name).bind(7, out.supplier.ico).bind(8, out.supplier.icDph)
     .bind(9, out.supplier.address.countryCode)
     .bind(10, out.currency).bind(11, out.payable).bind(12, out.taxAmount)
     .bind(13, out.variableSymbol).bind(14, out.iban)
     .bind(15, out.receivedAt).bind(16, out.source);
    s.bindBlob(17, xml);
    s.bind(18, out.digest).bind(19, joinLines(out.warnings));
    if (!s.done()) {
        // The only way to lose that race is for the same file to have been
        // inserted between the check above and here — another connection, or
        // the same one from two places. Report what is there rather than a
        // constraint error the user cannot act on.
        if (findExisting(out)) {
            if (alreadyHere) *alreadyHere = true;
            return true;
        }
        err_ = s.error();
        return false;
    }
    out.id = sqlite3_last_insert_rowid(db_);
    return true;
}

std::vector<Database::ReceivedInvoice> Database::receivedInvoices() {
    std::vector<ReceivedInvoice> out;
    Stmt s(db_, std::string("SELECT ") + RECEIVED_COLUMNS +
                " FROM received_invoices WHERE company_id = ?1 "
                "ORDER BY issue_date DESC, id DESC");
    if (!s) { err_ = s.error(); return out; }
    s.bind(1, activeCompanyId());
    while (s.step()) {
        ReceivedInvoice row;
        readReceived(s, row);
        out.push_back(std::move(row));
    }
    return out;
}

bool Database::loadReceivedInvoice(int64_t id, ReceivedInvoice& out) {
    Stmt s(db_, std::string("SELECT ") + RECEIVED_COLUMNS +
                " FROM received_invoices WHERE id = ?1");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, id);
    if (!s.step()) return false;
    readReceived(s, out);
    return true;
}

std::string Database::receivedOriginal(int64_t id) {
    Stmt s(db_, "SELECT original FROM received_invoices WHERE id = ?1");
    if (!s) { err_ = s.error(); return {}; }
    s.bind(1, id);
    // Read as a blob, with its length, rather than as text: this is the file
    // that arrived, and it is the legal document.
    return s.step() ? s.blob(0) : std::string();
}

bool Database::setReceivedNote(int64_t id, const std::string& note) {
    Stmt s(db_, "UPDATE received_invoices SET note = ?1 WHERE id = ?2");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, note).bind(2, id);
    return s.done();
}

bool Database::setReceivedPaid(int64_t id, const std::string& paidOn, const Dec& amount) {
    Stmt s(db_, "UPDATE received_invoices SET paid_on = ?1, paid_amount = ?2 WHERE id = ?3");
    if (!s) { err_ = s.error(); return false; }
    // Marking it unpaid clears the amount too, so a row cannot claim to have
    // been paid nothing on no date.
    s.bind(1, paidOn).bind(2, paidOn.empty() ? Dec() : amount).bind(3, id);
    if (!s.done()) { err_ = s.error(); return false; }

    // Un-marking it also releases the movements that paid it. Otherwise the
    // guard rows outlive the payment they recorded: the statement can never be
    // imported again, and there is no payments list on this side to undo it
    // from — the money would simply be unrecoverable.
    if (paidOn.empty()) {
        Stmt d(db_, "DELETE FROM imported_transactions WHERE received_id = ?1");
        if (!d) { err_ = d.error(); return false; }
        d.bind(1, id);
        if (!d.done()) { err_ = d.error(); return false; }
    }
    return true;
}

bool Database::deleteReceivedInvoice(int64_t id) {
    Stmt s(db_, "DELETE FROM received_invoices WHERE id = ?1");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, id);
    return s.done();
}

bool Database::verifyIssuedDocument(int64_t invoiceId, std::string* detail) {
    IssuedDocument doc;
    if (!loadIssuedDocument(invoiceId, doc)) {
        if (detail) *detail = "doklad nemá archivovanú podobu";
        return false;
    }

    // Compare against the stored hash whenever there is one. Skipping the
    // check for empty content would let "DELETE the bytes" pass as unchanged —
    // the most obvious tampering there is.
    const bool pdfOk = doc.pdfHash.empty() ? doc.pdf.empty()
                                           : Sha256::hexOf(doc.pdf) == doc.pdfHash;
    const bool ublOk = doc.ublHash.empty() ? doc.ubl.empty()
                                           : Sha256::hexOf(doc.ubl) == doc.ublHash;

    if (detail) {
        if (!pdfOk)      *detail = "kontrolný súčet PDF nesedí";
        else if (!ublOk) *detail = "kontrolný súčet XML nesedí";
        else             *detail = "archivovaná podoba je nezmenená (" + doc.pdfHash.substr(0, 16) + "…)";
    }
    return pdfOk && ublOk;
}

// ------------------------------------------------------------------- backup
namespace fs = std::filesystem;

namespace {

/// "fakturo-2026-07-29-0743.db" — sorts chronologically as text, which is what
/// the rolling cleanup relies on.
std::string backupFileName(const std::string& stamp) {
    std::string clean;
    for (char c : stamp) {
        if (c == ' ' || c == ':') { clean.push_back('-'); continue; }
        clean.push_back(c);
    }
    // "2026-07-29-07-43-11" -> "2026-07-29-074311". Seconds matter: two
    // restores in the same minute would otherwise collide and the second
    // would fail before touching anything.
    if (clean.size() >= 19)
        clean = clean.substr(0, 13) + clean.substr(14, 2) + clean.substr(17, 2);
    return clean + ".db";
}

/// True for the names autoBackup() itself writes: "fakturo-2026-07-29.db".
/// Deliberately exact, so a backup the user saved into the same folder is not
/// swept away by the rolling cleanup.
bool isAutomaticBackupName(const std::string& name) {
    // "fakturo-" (8) + "2026-07-29" (10) + ".db" (3) = 21 characters exactly.
    return name.size() == 21 && name.compare(0, 8, "fakturo-") == 0 &&
           name.compare(18, 3, ".db") == 0 && sk::validIsoDate(name.substr(8, 10));
}

bool isSafetyCopyName(const std::string& name) {
    return name.rfind("pred-obnovou-", 0) == 0;
}

/// Keeps the newest `keep` files matching `matches`, deleting the rest.
void pruneBackups(const fs::path& dir, int keep, bool (*matches)(const std::string&)) {
    if (keep < 1) keep = 1;
    std::error_code ec;
    std::vector<fs::path> found;
    for (const auto& entry : fs::directory_iterator(dir, ec))
        if (matches(entry.path().filename().string())) found.push_back(entry.path());

    std::sort(found.begin(), found.end());          // names sort chronologically
    if (found.size() <= static_cast<size_t>(keep)) return;
    for (size_t i = 0; i < found.size() - static_cast<size_t>(keep); ++i)
        fs::remove(found[i], ec);
}

} // namespace

std::string Database::backupDirectory() const {
    if (path_.empty()) return {};
    std::error_code ec;
    fs::path dir = fs::path(path_).parent_path() / "zalohy";
    fs::create_directories(dir, ec);
    return dir.string();
}

bool Database::backupTo(const std::string& path, bool includeRegisters) {
    if (!db_) { err_ = "databáza nie je otvorená"; return false; }

    std::error_code ec;
    if (fs::exists(path, ec)) {
        err_ = "súbor už existuje: " + path;
        return false;
    }

    // VACUUM INTO, not a file copy: with WAL enabled the .db file on disk is
    // not the whole database, and copying it can produce a backup that is
    // missing the most recent work or is outright inconsistent.
    {
        Stmt s(db_, "VACUUM INTO ?1");
        if (!s) { err_ = s.error(); return false; }
        s.bind(1, path);
        if (!s.done()) { err_ = s.error(); return false; }
    }
    if (includeRegisters) return true;

    // Drop the downloaded registers from the copy. They are hundreds of
    // megabytes of public data that a single click re-fetches, and keeping ten
    // daily copies of them would fill the disk to protect nothing.
    sqlite3* copy = nullptr;
    if (sqlite3_open(path.c_str(), &copy) != SQLITE_OK) {
        if (copy) sqlite3_close_v2(copy);
        return true;                     // the backup itself is fine, just large
    }
    sqlite3_exec(copy, "DELETE FROM vat_register; DELETE FROM dic_register; "
                       "DELETE FROM settings WHERE key IN ('vat.updated','dic.updated'); "
                       "VACUUM;", nullptr, nullptr, nullptr);
    sqlite3_close_v2(copy);
    return true;
}

Database::BackupInfo Database::inspectBackup(const std::string& path) {
    BackupInfo info;

    std::error_code ec;
    if (!fs::exists(path, ec)) { info.error = "súbor neexistuje"; return info; }

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        info.error = db ? sqlite3_errmsg(db) : "súbor sa nedá otvoriť";
        if (db) sqlite3_close(db);
        return info;
    }

    auto scalar = [db](const char* sql, int fallback) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return fallback;
        const int value = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : fallback;
        sqlite3_finalize(st);
        return value;
    };

    // A Fakturo database, not just any SQLite file someone picked.
    const int tables = scalar(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN "
        "('invoices','invoice_lines','company','customers','settings')", 0);
    if (tables < 5) {
        info.error = "toto nie je databáza Fakturo";
        sqlite3_close(db);
        return info;
    }

    info.schemaVersion = scalar("PRAGMA user_version", 0);
    info.invoices      = scalar("SELECT COUNT(*) FROM invoices", 0);
    info.companies     = scalar("SELECT COUNT(*) FROM company", 0);
    info.valid         = true;
    sqlite3_close(db);
    return info;
}

bool Database::restoreFrom(const std::string& path, std::string* safetyCopyPath) {
    const BackupInfo info = inspectBackup(path);
    if (!info.valid) { err_ = info.error; return false; }

    const std::string livePath = path_;
    if (livePath.empty()) { err_ = "databáza nie je otvorená"; return false; }

    // Restoring the wrong file must not be the end of the story.
    const std::string safety =
        (fs::path(backupDirectory()) / ("pred-obnovou-" + backupFileName(sk::nowTimestamp())))
            .string();
    if (!backupTo(safety)) return false;
    if (safetyCopyPath) *safetyCopyPath = safety;

    close();

    // Copy beside the live file and rename into place. overwrite_existing
    // truncates first, so a disk-full halfway through would leave a live
    // database that is neither the old data nor the new.
    std::error_code ec;
    const std::string staging = livePath + ".restoring";
    fs::remove(staging, ec);
    fs::copy_file(path, staging, ec);
    if (ec) {
        err_ = "obnovu sa nepodarilo dokončiť: " + ec.message();
        fs::remove(staging, ec);
        open(livePath);            // the old data is untouched
        return false;
    }

    fs::remove(livePath + "-wal", ec);
    fs::remove(livePath + "-shm", ec);
    fs::rename(staging, livePath, ec);
    if (ec) {
        err_ = "obnovu sa nepodarilo dokončiť: " + ec.message();
        fs::remove(staging, ec);
        open(livePath);
        return false;
    }

    if (!open(livePath)) {
        err_ = "obnovená databáza sa nedá otvoriť: " + err_;
        return false;
    }
    return true;
}

bool Database::autoBackup(int keep) {
    const std::string dir = backupDirectory();
    if (dir.empty()) return false;

    std::error_code ec;
    const std::string today = sk::todayIso();
    const std::string name  = "fakturo-" + today + ".db";
    const std::string target = (fs::path(dir) / name).string();

    // One per day is enough: this protects against mistakes and corruption,
    // not against the disk dying, and a copy per launch would just churn.
    if (!fs::exists(target, ec) && !backupTo(target)) return false;

    // Prune only the files this function writes. A backup the user saved into
    // the same folder must survive, and the pre-restore safety copies get their
    // own smaller cap rather than accumulating forever.
    pruneBackups(dir, keep, &isAutomaticBackupName);
    pruneBackups(dir, 3,    &isSafetyCopyName);

    return true;
}

std::string Database::setting(const std::string& key, const std::string& def) {
    Stmt s(db_, "SELECT value FROM settings WHERE key = ?1");
    if (!s) return def;
    s.bind(1, key);
    return s.step() ? s.text(0) : def;
}

int  Database::schemaVersion()    { return userVersion(); }

bool Database::beginTransaction() { return exec("BEGIN"); }
bool Database::commit()           { return exec("COMMIT"); }
bool Database::rollback()         { return exec("ROLLBACK"); }

bool Database::setSetting(const std::string& key, const std::string& value) {
    Stmt s(db_, "INSERT INTO settings (key, value) VALUES (?1, ?2) "
                "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, key).bind(2, value);
    return s.done();
}

// ------------------------------------------------ Finančná správa registers
namespace {

struct RegisterSpec {
    const char* table;
    const char* insertSql;
    const char* updatedKey;
};

RegisterSpec specFor(RegisterKind kind) {
    if (kind == RegisterKind::Vat)
        return {"vat_register",
                "INSERT OR REPLACE INTO vat_register "
                "(ic_dph, ico, name, street, city, postal_code, country, reg_type, reg_date) "
                "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9)",
                "vat.updated"};
    return {"dic_register",
            "INSERT OR REPLACE INTO dic_register "
            "(ico, dic, name, street, city, postal_code, country) VALUES (?1,?2,?3,?4,?5,?6,?7)",
            "dic.updated"};
}

} // namespace

bool Database::registerImportBegin(RegisterKind kind) {
    if (!db_) { err_ = "databáza nie je otvorená"; return false; }
    registerImportRollback();

    const RegisterSpec spec = specFor(kind);
    if (!exec("BEGIN IMMEDIATE")) return false;
    if (!exec(std::string("DELETE FROM ") + spec.table)) { exec("ROLLBACK"); return false; }

    if (sqlite3_prepare_v2(db_, spec.insertSql, -1, &bulkInsert_, nullptr) != SQLITE_OK) {
        err_ = sqlite3_errmsg(db_);
        bulkInsert_ = nullptr;
        exec("ROLLBACK");
        return false;
    }
    return true;
}

namespace {

void bindText(sqlite3_stmt* st, int i, const std::string& v) {
    sqlite3_bind_text(st, i, v.c_str(), -1, SQLITE_TRANSIENT);
}

} // namespace

bool Database::registerImportAdd(const VatSubject& s) {
    if (!bulkInsert_) { err_ = "import nebol začatý"; return false; }
    sqlite3_reset(bulkInsert_);
    sqlite3_clear_bindings(bulkInsert_);
    bindText(bulkInsert_, 1, s.icDph);
    bindText(bulkInsert_, 2, s.ico);
    bindText(bulkInsert_, 3, s.name);
    bindText(bulkInsert_, 4, s.street);
    bindText(bulkInsert_, 5, s.city);
    bindText(bulkInsert_, 6, s.postalCode);
    bindText(bulkInsert_, 7, s.country);
    bindText(bulkInsert_, 8, s.regType);
    bindText(bulkInsert_, 9, s.regDate);
    if (sqlite3_step(bulkInsert_) != SQLITE_DONE) { err_ = sqlite3_errmsg(db_); return false; }
    return true;
}

bool Database::registerImportAdd(const TaxSubject& s) {
    if (!bulkInsert_) { err_ = "import nebol začatý"; return false; }
    sqlite3_reset(bulkInsert_);
    sqlite3_clear_bindings(bulkInsert_);
    bindText(bulkInsert_, 1, s.ico);
    bindText(bulkInsert_, 2, s.dic);
    bindText(bulkInsert_, 3, s.name);
    bindText(bulkInsert_, 4, s.street);
    bindText(bulkInsert_, 5, s.city);
    bindText(bulkInsert_, 6, s.postalCode);
    bindText(bulkInsert_, 7, s.country);
    if (sqlite3_step(bulkInsert_) != SQLITE_DONE) { err_ = sqlite3_errmsg(db_); return false; }
    return true;
}

bool Database::registerImportCommit(RegisterKind kind, const std::string& datasetDate) {
    if (bulkInsert_) { sqlite3_finalize(bulkInsert_); bulkInsert_ = nullptr; }
    // A failed COMMIT would otherwise leave the connection inside a transaction
    // forever, silently swallowing every later write.
    if (!exec("COMMIT")) { registerImportRollback(); return false; }
    const RegisterSpec spec = specFor(kind);
    return setSetting(spec.updatedKey, datasetDate.empty() ? sk::todayIso() : datasetDate);
}

void Database::registerImportRollback() {
    if (bulkInsert_) { sqlite3_finalize(bulkInsert_); bulkInsert_ = nullptr; }
    if (db_ && sqlite3_get_autocommit(db_) == 0) exec("ROLLBACK");
}

namespace {

const char* VAT_COLS =
    "ic_dph, ico, name, street, city, postal_code, country, reg_type, reg_date";
const char* DIC_COLS = "ico, dic, name, street, city, postal_code, country";

void readVatRow(Stmt& s, VatSubject& out) {
    out.icDph      = s.text(0);
    out.ico        = s.text(1);
    out.name       = s.text(2);
    out.street     = s.text(3);
    out.city       = s.text(4);
    out.postalCode = s.text(5);
    out.country    = s.text(6);
    out.regType    = s.text(7);
    out.regDate    = s.text(8);
}

void readTaxRow(Stmt& s, TaxSubject& out) {
    out.ico        = s.text(0);
    out.dic        = s.text(1);
    out.name       = s.text(2);
    out.street     = s.text(3);
    out.city       = s.text(4);
    out.postalCode = s.text(5);
    out.country    = s.text(6);
}

} // namespace

bool Database::lookupVatByIco(const std::string& ico, VatSubject& out) {
    if (ico.empty()) return false;
    // A subject can appear more than once after re-registration: prefer a full
    // payer (§4) over §7/§7a, then the most recent. reg_date is stored ISO, so
    // a plain DESC is chronological.
    Stmt s(db_, std::string("SELECT ") + VAT_COLS +
                " FROM vat_register WHERE ico = ?1 "
                "ORDER BY (reg_type LIKE '%7%'), reg_date DESC LIMIT 1");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, ico);
    if (!s.step()) return false;
    readVatRow(s, out);
    return true;
}

bool Database::lookupVatByIcDph(const std::string& icDph, VatSubject& out) {
    if (icDph.empty()) return false;
    Stmt s(db_, std::string("SELECT ") + VAT_COLS + " FROM vat_register WHERE ic_dph = ?1");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, icDph);
    if (!s.step()) return false;
    readVatRow(s, out);
    return true;
}

bool Database::lookupTaxByIco(const std::string& ico, TaxSubject& out) {
    if (ico.empty()) return false;
    Stmt s(db_, std::string("SELECT ") + DIC_COLS + " FROM dic_register WHERE ico = ?1");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, ico);
    if (!s.step()) return false;
    readTaxRow(s, out);
    return true;
}

int Database::vatRegisterCount() {
    Stmt s(db_, "SELECT COUNT(*) FROM vat_register");
    if (!s || !s.step()) return 0;
    return s.i32(0);
}

int Database::taxRegisterCount() {
    Stmt s(db_, "SELECT COUNT(*) FROM dic_register");
    if (!s || !s.step()) return 0;
    return s.i32(0);
}

std::string Database::vatRegisterUpdated() { return setting("vat.updated", ""); }
std::string Database::taxRegisterUpdated() { return setting("dic.updated", ""); }

namespace {

/// Each company numbers its own documents, so the settings key carries the
/// company id: "series.7.next", "series.proforma.7.next".
std::string seriesPrefixFor(Database::SeriesKind kind, int64_t companyId) {
    const std::string id = std::to_string(companyId == 0 ? 1 : companyId);
    return (kind == Database::SeriesKind::Proforma ? "series.proforma." : "series.") + id + ".";
}

int settingAsInt(const std::string& value, int fallback) {
    if (value.empty()) return fallback;
    try { return std::stoi(value); } catch (...) { return fallback; }
}

} // namespace

NumberSeries Database::series(SeriesKind kind) {
    const std::string k = seriesPrefixFor(kind, activeCompanyId());
    NumberSeries s;
    // A proforma series defaults to a "ZF" prefix so the two can never be
    // confused on paper.
    s.prefix  = setting(k + "prefix", kind == SeriesKind::Proforma ? "ZF" : "");
    s.year    = settingAsInt(setting(k + "year", "0"), 0);
    s.next    = settingAsInt(setting(k + "next", "1"), 1);
    s.padding = settingAsInt(setting(k + "padding", "4"), 4);
    return s;
}

bool Database::setSeries(const NumberSeries& s, SeriesKind kind) {
    const std::string k = seriesPrefixFor(kind, activeCompanyId());
    return setSetting(k + "prefix", s.prefix) &&
           setSetting(k + "year", std::to_string(s.year)) &&
           setSetting(k + "next", std::to_string(s.next)) &&
           setSetting(k + "padding", std::to_string(s.padding));
}

std::string Database::peekNextNumber(SeriesKind kind) {
    NumberSeries s = series(kind);
    std::string n = s.format(s.next);
    int guard = 0;
    while (invoiceNumberExists(n) && guard++ < 10000)
        n = s.format(s.next + guard);
    return n;
}

std::string Database::takeNextNumber(SeriesKind kind) {
    NumberSeries s = series(kind);
    std::string n = s.format(s.next);
    int guard = 0;
    while (invoiceNumberExists(n) && guard < 10000) { ++guard; n = s.format(s.next + guard); }
    s.next += guard + 1;
    setSeries(s, kind);
    return n;
}

// ---------------------------------------------------------- document links
bool Database::linkProforma(int64_t invoiceId, int64_t proformaId) {
    Stmt s(db_, "INSERT OR IGNORE INTO document_links (invoice_id, related_id, relation) "
                "VALUES (?1, ?2, 'proforma')");
    if (!s) { err_ = s.error(); return false; }
    s.bind(1, invoiceId).bind(2, proformaId);
    return s.done();
}

std::vector<int64_t> Database::proformasSettledBy(int64_t invoiceId) {
    std::vector<int64_t> out;
    Stmt s(db_, "SELECT related_id FROM document_links "
                "WHERE invoice_id = ?1 AND relation = 'proforma' ORDER BY id");
    if (!s) { err_ = s.error(); return out; }
    s.bind(1, invoiceId);
    while (s.step()) out.push_back(s.i64(0));
    return out;
}

int64_t Database::invoiceSettling(int64_t proformaId) {
    Stmt s(db_, "SELECT invoice_id FROM document_links "
                "WHERE related_id = ?1 AND relation = 'proforma' LIMIT 1");
    if (!s) return 0;
    s.bind(1, proformaId);
    return s.step() ? s.i64(0) : 0;
}

std::string Database::peekNextNumber() {
    NumberSeries s = series();
    std::string n = s.format(s.next);
    // Skip numbers already taken (e.g. after a manual override).
    int guard = 0;
    while (invoiceNumberExists(n) && guard++ < 10000)
        n = s.format(s.next + guard);
    return n;
}

std::string Database::takeNextNumber() {
    NumberSeries s = series();
    std::string n = s.format(s.next);
    int guard = 0;
    while (invoiceNumberExists(n) && guard < 10000) { ++guard; n = s.format(s.next + guard); }
    s.next += guard + 1;
    setSeries(s);
    return n;
}

} // namespace fk
