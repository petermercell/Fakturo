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

// Database.h - SQLite storage. One file, no server, no ORM.
#pragma once

#include "../bank/Matcher.h"
#include "../model/Model.h"
#include "../registry/VatRegister.h"

#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace fk {

enum class RegisterKind { Vat, IncomeTax };

class Database {
public:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    /// Opens (creating if needed) and migrates the schema.
    bool open(const std::string& path);
    void close();
    bool isOpen() const { return db_ != nullptr; }
    /// The file this connection was opened from. A background import opens its
    /// own connection to the same path rather than sharing this one.
    const std::string& path() const { return path_; }
    const std::string& lastError() const { return err_; }

    // ----------------------------------------------------------- companies
    /// Every company you invoice from. Customers are shared between them.
    std::vector<Company> companies();
    bool    loadCompany(int64_t id, Company& out);
    /// The company new documents are issued under.
    Company activeCompany();
    int64_t activeCompanyId();
    bool    setActiveCompany(int64_t id);
    /// Inserts when c.id is 0, updates otherwise; fills c.id.
    bool    saveCompany(Company& c);
    /// Refuses while any document still belongs to it.
    bool    deleteCompany(int64_t id);
    int     invoiceCountFor(int64_t companyId);

    /// Replaces the whole list for a company. Simpler than diffing, and the
    /// list is a handful of rows.
    bool    saveBankAccounts(int64_t companyId, std::vector<BankAccount>& accounts);
    std::vector<BankAccount> bankAccounts(int64_t companyId);

    // ------------------------------------------------------------ customers
    std::vector<Customer> customers(bool includeArchived = false);
    bool loadCustomer(int64_t id, Customer& out);
    bool saveCustomer(Customer& c);        // inserts or updates; fills c.id
    bool deleteCustomer(int64_t id);       // soft delete (archived = 1)

    // ------------------------------------------------------------- invoices
    struct InvoiceSummary {
        int64_t      id = 0;
        std::string  number;
        std::string  issueDate;
        std::string  dueDate;
        std::string  buyerName;
        std::string  variableSymbol;
        std::string  currency;
        Dec          total;         // payable, including VAT
        Dec          paid;
        DocType      type  = DocType::Invoice;
        InvoiceState state = InvoiceState::Draft;

        Dec  outstanding() const { return (total - paid).roundTo(2); }
        bool isFullyPaid() const { return isSettled(total, paid); }
        bool isOverdue(const std::string& todayIso) const;
    };
    /// Documents of the active company only.
    std::vector<InvoiceSummary> invoiceList();
    bool loadInvoice(int64_t id, Invoice& out);
    bool saveInvoice(Invoice& inv);        // inserts or updates; fills inv.id
    bool deleteInvoice(int64_t id);

    // ------------------------------------------------------- testing only
    // Real deletion of a document, audit trail and all. Nothing in normal
    // operation may call these: an issued number must stay in the series.
    // They exist so the app can be exercised without accumulating junk.
    bool forceDeleteInvoice(int64_t id);
    /// Wipes every document, line, payment and audit row. Returns how many
    /// documents were removed.
    int  forceDeleteAllInvoices();
    /// Scoped to one company: two companies number independently, so the same
    /// number in each is correct, not a duplicate. 0 means the active company.
    bool invoiceNumberExists(const std::string& number, int64_t exceptId = 0,
                             int64_t companyId = 0);
    /// Highest number ever assigned to an issued document, for sequence checks.
    std::string highestIssuedNumber();

    // -------------------------------------------------- document lifecycle
    /// Consumes `number`, locks the content and stamps the issue time.
    bool issueInvoice(int64_t id, const std::string& number);
    /// Issue and archive as one transaction. A document that is issued but has
    /// no archived form is exactly what the archive exists to prevent, so the
    /// two must succeed or fail together.
    bool issueInvoiceWithDocument(int64_t id, const std::string& number,
                                  const std::string& pdf, const std::string& ubl);
    /// Returns a locked document to draft. Always recorded, because the app
    /// cannot know whether the customer already has a copy.
    bool unlockInvoice(int64_t id, const std::string& reason);
    /// Marks a document as superseded by a credit note.
    bool cancelInvoice(int64_t id, const std::string& detail);

    struct AuditEntry {
        std::string at;
        std::string action;
        std::string detail;
    };
    std::vector<AuditEntry> auditFor(int64_t invoiceId);
    bool recordAudit(int64_t invoiceId, const std::string& action, const std::string& detail);

    // ------------------------------------------------------------ payments
    std::vector<Payment> paymentsFor(int64_t invoiceId);
    bool addPayment(Payment& p);
    bool deletePayment(int64_t paymentId);

    // ------------------------------------------------- bank statement import
    /// Every open document of the active company, in the shape the matcher
    /// wants. Cancelled ones are included and flagged rather than dropped, so
    /// a payment against one can be reported instead of silently unmatched.
    std::vector<PayableInvoice> payableInvoices();

    /// The same, for invoices you have *received* and not yet settled — what a
    /// debit on the statement might be paying. Carries the supplier's account,
    /// which is the one extra signal the outgoing direction has: you knew where
    /// the money was going before you sent it.
    std::vector<PayableInvoice> payableReceivedInvoices();

    /// Dedup keys of movements already imported for the active company. The
    /// matcher skips these, so an overlapping statement cannot pay the same
    /// invoice twice.
    std::vector<std::string> importedTransactionIds();

    struct ImportedPayment {
        std::string bankId;         // BankTransaction::dedupKey()
        /// An invoice you issued, being paid to you. Zero for a movement that
        /// pays nothing, or for one that pays a received invoice instead.
        int64_t     invoiceId = 0;
        /// An invoice you received, being paid by you. At most one of the two
        /// is ever set: a movement goes one way.
        int64_t     receivedId = 0;
        std::string paidOn;         // ISO
        Dec         amount;
        std::string note;
    };

    /// Records payments and marks their movements imported, all or nothing.
    /// `sourceFile` and `sourceSha` are kept so a later question about where a
    /// payment came from has an answer.
    bool recordStatementImport(const std::vector<ImportedPayment>& payments,
                               const std::string& sourceFile,
                               const std::string& sourceSha);

    // ------------------------------------------------------ document links
    /// Records that `invoiceId` settles `proformaId`. One invoice may settle
    /// several proformas — a company that sends four advance requests and then
    /// bills once needs all four linked.
    bool linkProforma(int64_t invoiceId, int64_t proformaId);
    std::vector<int64_t> proformasSettledBy(int64_t invoiceId);
    /// 0 when the proforma has not been billed yet.
    int64_t invoiceSettling(int64_t proformaId);

    // -------------------------------------------------------- number series
    /// Proformas have their own counter: they are not tax documents and must
    /// not consume a number from the tax sequence.
    enum class SeriesKind { Invoice, Proforma };

    NumberSeries series(SeriesKind kind = SeriesKind::Invoice);
    bool         setSeries(const NumberSeries& s, SeriesKind kind = SeriesKind::Invoice);
    std::string  peekNextNumber(SeriesKind kind);
    std::string  takeNextNumber(SeriesKind kind);
    /// Peeks at the next free number without consuming it.
    std::string  peekNextNumber();
    /// Consumes the next number (call once the document is actually issued).
    std::string  takeNextNumber();

    // -------------------------------------------------- Finančná správa registers
    // Bulk import of a whole dataset. The replace runs in one transaction, so a
    // failed or cancelled import leaves the previous data intact.
    bool registerImportBegin(RegisterKind kind);
    bool registerImportAdd(const VatSubject& s);
    bool registerImportAdd(const TaxSubject& s);
    bool registerImportCommit(RegisterKind kind, const std::string& datasetDate);
    void registerImportRollback();

    bool lookupVatByIco(const std::string& ico, VatSubject& out);
    bool lookupVatByIcDph(const std::string& icDph, VatSubject& out);
    bool lookupTaxByIco(const std::string& ico, TaxSubject& out);

    int  vatRegisterCount();
    int  taxRegisterCount();
    /// ISO publication date of the imported dataset, empty if never imported.
    std::string vatRegisterUpdated();
    std::string taxRegisterUpdated();

    /// Runs raw SQL. Only for tests that need to simulate an older database.
    bool execForTests(const std::string& sql) { return exec(sql); }

    // ------------------------------------------------------------ archive
    /// The document exactly as it was issued. The invoice data is already
    /// frozen; this freezes the *rendering*, so changing the PDF template does
    /// not change how a document you sent two years ago prints.
    struct IssuedDocument {
        int         version = 0;
        std::string issuedAt;
        std::string pdf;        // raw bytes
        std::string ubl;        // XML, empty for a proforma
        std::string pdfHash;    // sha256, lower-case hex
        std::string ublHash;
    };

    /// Stores one version. Re-issuing after an unlock adds another rather than
    /// overwriting: the earlier document existed, and may have been sent.
    bool storeIssuedDocument(int64_t invoiceId, const std::string& pdf, const std::string& ubl,
                             const std::string& at = "");
    /// Newest version, false when the document was never issued.
    bool loadIssuedDocument(int64_t invoiceId, IssuedDocument& out);
    int  issuedVersionCount(int64_t invoiceId);
    /// Re-hashes the stored bytes and compares. False means the archive row
    /// was altered outside the application.
    bool verifyIssuedDocument(int64_t invoiceId, std::string* detail = nullptr);

    // ---------------------------------------------------------- received
    // Invoices somebody sent *to* you. From 1 January 2027 every taxable
    // person must be able to receive an eFaktúra, whether or not they issue
    // one — so this is the half of the mandate that binds a §7a non-payer.
    //
    // A received document is not yours to change. What is stored is what
    // arrived, byte for byte, plus two things that are yours: a note, and
    // whether you have paid it.

    struct ReceivedInvoice {
        int64_t     id        = 0;
        int64_t     companyId = 0;      ///< which of your companies it was addressed to
        std::string number;
        std::string issueDate;
        std::string dueDate;
        std::string taxPointDate;
        Party       supplier;           ///< as the document states them
        std::string currency = "EUR";
        Dec         payable;            ///< BT-115, as stated
        Dec         taxAmount;          ///< BT-110, as stated
        std::string variableSymbol;
        std::string iban;

        std::string receivedAt;         ///< ISO timestamp of the import
        std::string source;             ///< where it came from: a file, or a poštár
        std::string digest;             ///< sha256 of the original bytes, lower-case hex

        /// What the reader could not reconcile. Kept with the document rather
        /// than recomputed, so the warning shown next year is the warning that
        /// was shown on the day it arrived.
        std::vector<std::string> warnings;

        // Yours, not the sender's.
        std::string note;
        std::string paidOn;             ///< empty while unpaid
        Dec         paidAmount;

        /// Marked as paid at all — on some date, for some amount.
        bool isPaid() const { return !paidOn.empty(); }
        /// What is still owed. Negative would mean overpaid, which is possible
        /// and not this application's business to argue with.
        Dec outstanding() const { return isPaid() ? payable - paidAmount : payable; }
        /// Paid in full, within the half-cent that bank transfers round to.
        bool isSettled() const {
            return isPaid() && outstanding() < Dec::fromRaw(5000);   // 0.005
        }
        /// Unpaid or part-paid, and past its due date on `todayIso`.
        bool isOverdue(const std::string& todayIso) const {
            return !isSettled() && !dueDate.empty() && dueDate < todayIso;
        }
    };

    /// Reads `xml`, stores it and fills `out`. `source` is free text for the
    /// list — a file name, or the name of the delivery service it came from.
    ///
    /// Returns false when the document cannot be read at all. A document that
    /// reads but does not add up **is** stored, with its warnings: refusing it
    /// would leave you with an invoice you must pay and no record of it.
    ///
    /// Importing the same bytes twice does nothing and reports success, with
    /// `out` filled from the row already there and `alreadyHere` set. That is
    /// the same rule the bank import follows, and for the same reason: a
    /// second copy of a payable is worse than no copy. The caller is expected
    /// to say which happened — "imported" and "you already had this" look
    /// identical otherwise, and one of them means the list did not change.
    ///
    /// Sameness is per company. The same file imported under two of your
    /// companies is two documents, because the alternative is an import that
    /// succeeds and shows nothing after you notice the wrong company was
    /// selected.
    bool importReceivedInvoice(const std::string& xml, const std::string& source,
                               ReceivedInvoice& out, bool* alreadyHere = nullptr);

    /// Newest first. Without the original bytes, which are loaded on demand.
    std::vector<ReceivedInvoice> receivedInvoices();
    bool loadReceivedInvoice(int64_t id, ReceivedInvoice& out);
    /// The document exactly as it arrived.
    std::string receivedOriginal(int64_t id);

    /// The two things that are yours to change.
    bool setReceivedNote(int64_t id, const std::string& note);
    /// An empty `paidOn` marks it unpaid again.
    bool setReceivedPaid(int64_t id, const std::string& paidOn, const Dec& amount);

    /// Removes one. For a file imported by mistake — re-importing the right
    /// one is otherwise blocked by nothing, but a wrong row cannot be edited
    /// away because nothing about the document is editable.
    bool deleteReceivedInvoice(int64_t id);

    // ------------------------------------------------------------ catalogue
    // The lines you type every month, typed once. Not a stock system and not a
    // price list: a saved line, with the unit and VAT it usually carries, that
    // can be dropped into an invoice and then edited like any other.

    struct CatalogItem {
        int64_t     id        = 0;
        int64_t     companyId = 0;
        std::string description;
        std::string unit       = "ks";
        std::string unitCodeUn = "H87";
        Dec         unitPrice;
        Dec         vatRate;
        std::string vatCategory = VatCat::Standard;
        /// How often it has been used, and when last. The order the picker
        /// shows: what you reached for last month is what you reach for now.
        int         uses = 0;
        std::string lastUsed;
    };

    /// Most used first, then most recently used, then alphabetical.
    std::vector<CatalogItem> catalogItems();
    /// Inserts or updates. Two items with the same description in one company
    /// are one item — saving the same line twice should not fill the list with
    /// copies of it.
    bool saveCatalogItem(CatalogItem& item);
    bool deleteCatalogItem(int64_t id);
    /// Records that it was put on an invoice, which is what the ordering uses.
    bool noteCatalogUse(int64_t id, const std::string& todayIso);

    // -------------------------------------------------------------- backup
    /// What a backup file turns out to contain. Checked before a restore, so
    /// the confirmation can say what is about to replace your data.
    struct BackupInfo {
        bool        valid = false;
        int         schemaVersion = 0;
        int         invoices = 0;
        int         companies = 0;
        std::string error;
    };

    /// Consistent copy of the live database, written with VACUUM INTO rather
    /// than a file copy: the WAL means the file on disk is not the database.
    /// `path` must not already exist.
    ///
    /// The downloaded Finančná správa registers are left out by default. They
    /// are ~480 MB of public data that can be fetched again in a minute, and
    /// including them would make every backup enormous for no benefit.
    bool backupTo(const std::string& path, bool includeRegisters = false);

    /// Opens the file and checks it really is a Fakturo database.
    static BackupInfo inspectBackup(const std::string& path);

    /// Replaces the live database with `path`. Takes a safety copy of the
    /// current data first and reports where it went, so a restore of the wrong
    /// file is recoverable. The connection is reopened before returning.
    bool restoreFrom(const std::string& path, std::string* safetyCopyPath = nullptr);

    /// Timestamped copy into <database dir>/zalohy, keeping the newest `keep`.
    /// Called on open: the backups people actually have are the automatic ones.
    bool autoBackup(int keep = 10);
    std::string backupDirectory() const;

    std::string  setting(const std::string& key, const std::string& def = "");
    bool         setSetting(const std::string& key, const std::string& value);

    /// The schema this file is at. Public because the About box shows it: when
    /// something has gone wrong and the answer has to come back from the user
    /// rather than from a log, this is the second thing worth knowing.
    int schemaVersion();

    /// A group of writes that must land together or not at all. Everything
    /// inside this class already wraps its own; these exist for callers that
    /// write several settings rows at once, where a failure half way through
    /// would leave a configuration that is part old and part new.
    /// Not nestable — SQLite has no nested BEGIN.
    bool beginTransaction();
    bool commit();
    bool rollback();

private:
    bool exec(const std::string& sql);
    bool migrate();
    bool createBaseSchema();
    int  userVersion();
    bool setUserVersion(int version);
    bool hasColumn(const std::string& table, const std::string& column);
    std::vector<std::string> tableColumns(const std::string& table);
    /// The company columns present both in this build and in the table as it
    /// stands, comma separated. A rebuild must not name a column that the
    /// version it is migrating *from* has never heard of.
    std::string companyCarryOver();
    bool tableHasCompositeNumberKey();
    /// ALTER TABLE ... ADD COLUMN, but only when it is actually missing.
    bool ensureColumn(const std::string& table, const std::string& column,
                      const std::string& declaration);

    std::string   path_;
    sqlite3*      db_        = nullptr;
    sqlite3_stmt* bulkInsert_ = nullptr;  // reused across a bulk register import
    std::string   err_;
};

/// The column names declared in a CREATE TABLE statement, in order. A table
/// rebuild has to name the columns it copies, and the only list that cannot
/// drift from the table being built is the one read out of its own definition.
/// Table constraints — UNIQUE, PRIMARY KEY and the rest — are not columns and
/// are skipped. Out here rather than hidden in the .cpp so it can be tested:
/// a parser that quietly missed a column would scramble a migration.
std::vector<std::string> columnsInDdl(const std::string& ddl);

} // namespace fk
