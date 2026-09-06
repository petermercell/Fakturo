First public release. Fakturo is a minimal invoicing application for Slovak and
Czech sole traders and small companies, with Peppol BIS Billing 3.0 (UBL 2.1)
export and import.

**Download:** `Fakturo-0.15.0.dmg` (macOS) · `Fakturo-0.15.0-x86_64.AppImage` (Linux) · `Fakturo-0.15.0-windows-setup.exe` or `Fakturo-0.15.0-windows-portable.zip` (Windows)

## Requirements

**macOS** — macOS 14 (Sonoma) or later, Apple Silicon.

**Linux** — x86_64 with glibc 2.34 or newer. That covers Rocky, Alma and RHEL 9,
Fedora 35 and later, Ubuntu 22.04 and later, and Debian 12 and later. It will not
start on CentOS 7 or Ubuntu 20.04. X11 and Wayland are both fine.

**Windows** — Windows 10 or later, 64-bit. It runs on ARM64 Windows through
x64 emulation, untested.

## Installing

### macOS

The app is signed ad-hoc — not with an Apple Developer identity, and not
notarized — so Gatekeeper will refuse it the first time. Open the disk image,
drag **Fakturo** to Applications, then either right-click it → **Open** and
confirm once, or:

```
xattr -dr com.apple.quarantine /Applications/Fakturo.app
```

Verify the download if you like:

```
shasum -a 256 Fakturo-0.15.0.dmg
```

### Linux

The AppImage carries Qt and everything else it needs. Nothing is installed, and
nothing is written outside your home directory.

```
chmod +x Fakturo-0.15.0-x86_64.AppImage
./Fakturo-0.15.0-x86_64.AppImage
```

An AppImage mounts itself with FUSE 2, which several distributions no longer
ship — Rocky 9 among them. Install it once:

```
sudo dnf install fuse-libs      # Fedora, RHEL, Rocky, Alma
sudo apt install libfuse2       # Debian, Ubuntu
```

or run it without FUSE at all:

```
./Fakturo-0.15.0-x86_64.AppImage --appimage-extract-and-run
```

There is no menu entry unless you integrate it yourself — AppImageLauncher does
it, or a `.desktop` file of your own pointing at wherever you keep the file.

Verify the download if you like:

```
sha256sum Fakturo-0.15.0-x86_64.AppImage
```

### Windows

Two ways, and they install the same program. Neither is signed, so Windows
SmartScreen will show **"Windows protected your PC"** the first time — click
**More info**, then **Run anyway**. Silencing that needs a code-signing
certificate, which is a yearly cost and is not set up.

**`Fakturo-0.15.0-windows-setup.exe`** installs per user by default, so it
raises no administrator prompt; the first screen offers Program Files for all
users instead. Slovak, Czech and English. It adds a Start menu entry and,
optionally, a desktop icon.

**`Fakturo-0.15.0-windows-portable.zip`** installs nothing. Extract it and run
`Fakturo\Fakturo.exe` from wherever you put it — a USB stick is fine. Qt and
everything else it needs are in the folder beside the executable.

Your data lives in `%APPDATA%\Fakturo\`, not in the program folder, whichever
you use. **Uninstalling deliberately leaves it there**, so reinstalling or
switching between the two keeps every invoice you have issued. If you want it
gone, delete that folder yourself.

If the program refuses to start with a missing `VCRUNTIME140.dll`, run
`vc_redist.x64.exe` — it is included beside the executable — and try again.
Most machines already have it.

Verify the download if you like, in PowerShell:

```
Get-FileHash Fakturo-0.15.0-windows-setup.exe -Algorithm SHA256
```

## What it does

- Invoices, credit notes, proformas and §4 advance tax documents
  (UNCL1001 380 / 381 / 325 / 386)
- Peppol BIS Billing 3.0 (UBL 2.1) export, and import of supplier e-invoices
  in the **Prijaté** tab — you can receive, read, store and pay an e-faktúra
- Per-line and per-document discounts; UN/ECE Rec 20 unit codes
- Foreign-currency VAT restatement in the §26 ods. 1 order: the base converted
  to euro at the ECB rate of the day before the tax point, and the VAT computed
  from the euro base — not the other way round
- Payment QR codes — PAY by square for EUR, QR Platba for CZK
- PDF output; an issued document is locked and kept byte-for-byte with a hash
- Bank statement import (Fio CSV, camt.053), matching payments in both
  directions
- Multiple companies with a seller snapshot per invoice, two bank accounts each,
  customer address book, item catalogue, backup and restore

## Not in this release

- Sending over the Slovak SAPI transport. Receiving is done; sending is not.
- Reminders (upomienky) and emailing a document
- No signature on any of the three builds: no Apple Developer identity or
  notarization on macOS, and the AppImage and both Windows downloads are
  unsigned

## Built with

Qt 6, C++17 and SQLite. The macOS build uses Qt 6.11.1; the Linux AppImage
bundles Qt 6.5.3; the Windows build is MSVC 2022 against Qt 6.5.3. No
third-party runtime dependencies beyond SQLite, zlib and Qt — the QR encoder,
LZMA1, SHA-256, XML and JSON are all implemented in the project. Database
schema version 18.

## Licence

GNU General Public License v3.0 or later.
