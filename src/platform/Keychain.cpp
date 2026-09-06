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

#include "Keychain.h"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <TargetConditionals.h>
#include <cassert>
#endif

namespace fk {
namespace keychain {

#ifdef __APPLE__
namespace {

/// A CFTypeRef that releases itself. Every early return below would otherwise
/// be a leak, and the leaked object is a dictionary that has the secret in it.
class Cf {
public:
    explicit Cf(CFTypeRef ref = nullptr) : ref_(ref) {}
    ~Cf() { if (ref_) CFRelease(ref_); }
    Cf(const Cf&) = delete;
    Cf& operator=(const Cf&) = delete;

    CFTypeRef get() const { return ref_; }
    /// For an out-parameter that returns a retained object. Asserts the slot
    /// is empty: writing over a held reference would leak the old one, and the
    /// old one is a dictionary with a secret in it.
    CFTypeRef* slot() { assert(!ref_); return &ref_; }
    explicit operator bool() const { return ref_ != nullptr; }

private:
    CFTypeRef ref_ = nullptr;
};

CFStringRef cfString(const std::string& text) {
    return CFStringCreateWithBytes(nullptr,
                                   reinterpret_cast<const UInt8*>(text.data()),
                                   static_cast<CFIndex>(text.size()),
                                   kCFStringEncodingUTF8, false);
}

/// The query that names one entry: generic password, this service, this
/// account. Caller owns the result.
CFMutableDictionaryRef queryFor(const std::string& service, const std::string& account) {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!q) return nullptr;

    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);

    CFStringRef s = cfString(service);
    CFStringRef a = cfString(account);
    // Both keys or no query at all. Dropping one would leave a query that
    // looks fine and matches too much — and SecItemDelete with no match limit
    // deletes everything it matches, which would be every secret for every
    // company and both environments on one click of "remove".
    if (!s || !a) {
        if (s) CFRelease(s);
        if (a) CFRelease(a);
        CFRelease(q);
        return nullptr;
    }
    CFDictionarySetValue(q, kSecAttrService, s); CFRelease(s);
    CFDictionarySetValue(q, kSecAttrAccount, a); CFRelease(a);
    return q;
}

std::string explain(OSStatus status) {
    switch (status) {
        case errSecSuccess:       return {};
        case errSecItemNotFound:  return "V Kľúčenke nič nie je uložené.";
        case errSecDuplicateItem: return "V Kľúčenke už taký záznam je.";
        case errSecAuthFailed:
        case errSecUserCanceled:  return "Prístup do Kľúčenky bol zamietnutý.";
        case errSecInteractionNotAllowed:
            return "Kľúčenka je zamknutá. Odomknite ju a skúste znova.";
        default: break;
    }
    // Their message is in the user's language and is better than anything
    // guessed here; the code goes with it because it is what a search finds.
    std::string out = "Kľúčenka odmietla operáciu (" + std::to_string(static_cast<long>(status)) + ")";
#if !TARGET_OS_IPHONE
    if (CFStringRef message = SecCopyErrorMessageString(status, nullptr)) {
        char buffer[512] = {0};
        if (CFStringGetCString(message, buffer, sizeof(buffer), kCFStringEncodingUTF8))
            out += std::string(": ") + buffer;
        CFRelease(message);
    }
#endif
    return out + ".";
}

void report(std::string* error, OSStatus status) {
    if (error) *error = explain(status);
}

} // namespace

bool available() { return true; }

bool store(const std::string& service, const std::string& account,
           const std::string& secret, std::string* error) {
    if (error) error->clear();
    if (service.empty() || account.empty()) {
        if (error) *error = "Chýba meno záznamu v Kľúčenke.";
        return false;
    }
    // An empty secret means "forget it" rather than "store nothing": an entry
    // holding an empty password would read back as a configured connection.
    if (secret.empty()) return remove(service, account, error);

    Cf query(queryFor(service, account));
    if (!query) { if (error) *error = "Kľúčenka: nepodarilo sa zostaviť dopyt."; return false; }
    CFMutableDictionaryRef q = reinterpret_cast<CFMutableDictionaryRef>(
        const_cast<void*>(query.get()));

    Cf data(CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(secret.data()),
                         static_cast<CFIndex>(secret.size())));
    if (!data) { if (error) *error = "Kľúčenka: nepodarilo sa pripraviť údaje."; return false; }

    // Update first. SecItemAdd on an existing item returns duplicate rather
    // than replacing, and a "secret saved" message over an unchanged old
    // secret is the exact failure this screen must not have.
    Cf changes(CFDictionaryCreateMutable(nullptr, 1, &kCFTypeDictionaryKeyCallBacks,
                                         &kCFTypeDictionaryValueCallBacks));
    if (!changes) { if (error) *error = "Kľúčenka: nepodarilo sa pripraviť zmenu."; return false; }
    CFDictionarySetValue(reinterpret_cast<CFMutableDictionaryRef>(
                             const_cast<void*>(changes.get())),
                         kSecValueData, data.get());

    OSStatus status = SecItemUpdate(
        q, reinterpret_cast<CFDictionaryRef>(const_cast<void*>(changes.get())));
    if (status == errSecSuccess) return true;

    if (status == errSecItemNotFound) {
        CFDictionarySetValue(q, kSecValueData, data.get());
        // Deliberately *not* setting kSecAttrAccessible. Without
        // kSecUseDataProtectionKeychain this lands in the legacy file-based
        // keychain, where that attribute is accepted and ignored — so it would
        // have promised a "this device only" guarantee it does not deliver,
        // and risked errSecParam for nothing. The item is not marked
        // synchronizable, so it does not go to iCloud; that is the property
        // that actually holds. The data protection keychain is not an option
        // here: it needs a keychain-access-group entitlement, which an
        // unsigned local build does not have.
        status = SecItemAdd(q, nullptr);
        if (status == errSecSuccess) return true;
    }
    report(error, status);
    return false;
}

bool load(const std::string& service, const std::string& account,
          std::string& out, bool* found, std::string* error) {
    if (found) *found = false;
    if (error) error->clear();

    Cf query(queryFor(service, account));
    if (!query) { if (error) *error = "Kľúčenka: nepodarilo sa zostaviť dopyt."; return false; }
    CFMutableDictionaryRef q = reinterpret_cast<CFMutableDictionaryRef>(
        const_cast<void*>(query.get()));
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);

    Cf result;
    const OSStatus status = SecItemCopyMatching(q, result.slot());
    if (status == errSecItemNotFound) return false;      // not an error
    if (status != errSecSuccess) { report(error, status); return false; }
    if (!result || CFGetTypeID(result.get()) != CFDataGetTypeID()) {
        if (error) *error = "Kľúčenka vrátila neočakávaný typ údajov.";
        return false;
    }

    CFDataRef data = reinterpret_cast<CFDataRef>(const_cast<void*>(result.get()));
    out.assign(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
               static_cast<size_t>(CFDataGetLength(data)));
    if (found) *found = true;
    return true;
}

bool has(const std::string& service, const std::string& account, std::string* error) {
    if (error) error->clear();
    Cf query(queryFor(service, account));
    if (!query) { if (error) *error = "Kľúčenka: nepodarilo sa zostaviť dopyt."; return false; }
    CFMutableDictionaryRef q = reinterpret_cast<CFMutableDictionaryRef>(
        const_cast<void*>(query.get()));
    // Attributes, not data: this answers "is it there" without the secret
    // being copied into this process, and without prompting.
    CFDictionarySetValue(q, kSecReturnAttributes, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);

    Cf result;
    const OSStatus status = SecItemCopyMatching(q, result.slot());
    if (status == errSecSuccess) return true;
    // Absence is an answer, not a failure. Anything else — a locked keychain,
    // a refused prompt — is a failure and must not be reported as absence.
    if (status != errSecItemNotFound) report(error, status);
    return false;
}

bool remove(const std::string& service, const std::string& account, std::string* error) {
    if (error) error->clear();
    Cf query(queryFor(service, account));
    if (!query) { if (error) *error = "Kľúčenka: nepodarilo sa zostaviť dopyt."; return false; }

    const OSStatus status = SecItemDelete(
        reinterpret_cast<CFDictionaryRef>(const_cast<void*>(query.get())));
    if (status == errSecSuccess || status == errSecItemNotFound) return true;
    report(error, status);
    return false;
}

#else   // ------------------------------------------------ not Apple

namespace {
const char* unsupported() {
    return "Bezpečné úložisko hesiel je zatiaľ len na macOS. "
           "Heslo sa neuloží.";
}
} // namespace

bool available() { return false; }

bool store(const std::string&, const std::string&, const std::string&, std::string* error) {
    if (error) *error = unsupported();
    return false;
}

bool load(const std::string&, const std::string&, std::string&, bool* found, std::string* error) {
    if (found) *found = false;
    if (error) *error = unsupported();
    return false;
}

bool has(const std::string&, const std::string&, std::string* error) {
    if (error) error->clear();      // nothing is stored, and that is not a fault
    return false;
}

bool remove(const std::string&, const std::string&, std::string* error) {
    if (error) error->clear();
    return true;      // nothing was ever stored, so nothing is left behind
}

#endif

} // namespace keychain
} // namespace fk
