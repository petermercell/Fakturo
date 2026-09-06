# Fakturo

A minimal C++17 / Qt6 invoicing application for **Slovak and Czech** sole
traders and small companies, with **Peppol BIS Billing 3.0 (UBL 2.1)** export
and import.

Four tabs. That is the whole app.

| Tab | What it does |
| --- | --- |
| **Faktúry** | List, create, edit, duplicate, credit-note, export PDF, export Peppol XML |
| **Odberatelia** | Customer address book |
| **Moja firma** | Company details, bank accounts, invoice number series |
| **Registre** | Local copies of the Slovak bulk registers; Czechia needs none |

## What it does

- Invoices, credit notes, proformas and §4 advance tax documents
- Per-line and per-document discounts; UN/ECE Rec 20 unit codes
- Foreign-currency VAT restatement in the §26 ods. 1 order (base to EUR at the
  ECB rate of the day before the tax point, *then* the VAT)
- Payment QR codes — PAY by square for EUR, QR Platba for CZK
- PDF output, and the exact issued bytes kept with a hash
- Bank statement import (Fio CSV, camt.053) with payment matching both directions
- A **Prijaté** tab that receives, stores and pays supplier e-invoices
- Backup and restore

## Design

Two layers, and the split is the whole design.

- **`invoice_core`** — pure C++17 + SQLite, no Qt. Money, VAT, dates, UBL,
  validation, bank statements, the SAPI protocol and the database all live here,
  behind functions that can be tested without a GUI. Money is fixed-point
  (`Dec`, int64 scaled by 1e6); `double` is never used for an amount.
- **`invoice_gui`** — the Qt screens, built as a static library so tests link
  the same code the application runs.

There are **no third-party runtime dependencies** beyond SQLite, zlib and Qt.
QR encoding, LZMA1, SHA-256, XML and JSON are all implemented here.

## Building on macOS

```bash
brew install qt cmake sqlite

cmake -B build-brew -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" -DCMAKE_BUILD_TYPE=Release
cmake --build build-brew -j
open build-brew/fakturo.app
```

Tests are built only when a `tests/` directory is present; this tree ships
without one, so `FAKTURO_BUILD_TESTS` defaults to `OFF` and nothing extra needs
to be passed.

### Disk image

```bash
cmake --build build-brew --target dmg
```

This runs `scripts/make-dmg.sh`, which bundles the Qt frameworks with
`macdeployqt`, re-signs ad-hoc afterwards (macdeployqt invalidates the
linker's signature), checks with `otool` that nothing still points at
`/opt/homebrew`, and packs a drag-to-`/Applications` image.

The result is **unsigned by a developer identity and not notarized**. On another
Mac, Gatekeeper will refuse it until the quarantine flag is removed:

```bash
xattr -dr com.apple.quarantine /Applications/Fakturo.app
```

## Building on Linux

Built and packaged on Rocky Linux 9 against the Qt online-installer build; any
distribution with Qt 6.4 or newer works.

```bash
sudo dnf install cmake ninja-build gcc-c++ sqlite-devel zlib-devel \
                 xcb-util-cursor libxkbcommon-x11 fuse-libs

export CMAKE_PREFIX_PATH=$HOME/Qt6/6.5.3/gcc_64:$CMAKE_PREFIX_PATH
cmake -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux
```

### AppImage

```bash
./scripts/make-appimage.sh build-linux
```

`scripts/make-appimage.sh` installs into an `AppDir`, renders the freedesktop
PNG icon from `resources/Fakturo.ico`, and runs `linuxdeploy` with its Qt plugin
to bundle Qt, the xcb platform plugin, the image formats and the TLS backends,
rewriting every `RUNPATH` to `$ORIGIN`. It takes the Qt to deploy from
`Qt6Core_DIR` in the build's `CMakeCache.txt`, so it can only ever deploy the Qt
the binary was actually linked against. OpenSSL is copied in by hand, because Qt
`dlopen`s it and no dependency walker can see it. It ends with an `ldd` sweep —
run with `LD_LIBRARY_PATH` cleared, or it would resolve the bundled libraries
back to the developer's Qt and report a clean AppDir as broken — that names
anything still pointing outside the AppDir. That report is the release gate.

The image carries Qt, not the C library, so it needs **glibc 2.34 or newer**:
Rocky, Alma and RHEL 9, Fedora 35+, Ubuntu 22.04+, Debian 12+. It also needs
`libfuse.so.2` to mount itself, which several distributions no longer ship —
`dnf install fuse-libs`, `apt install libfuse2`, or run it with
`--appimage-extract-and-run`.

## Options

| Option | Default | Meaning |
| --- | --- | --- |
| `FAKTURO_BUILD_GUI` | `ON` | Build the Qt6 desktop application |
| `FAKTURO_BUILD_TESTS` | `ON` if `tests/` exists, else `OFF` | Build the unit tests |
| `FAKTURO_WERROR` | `OFF` | Treat warnings as errors |

## Status

macOS and Linux are both built, packaged and used — an ad-hoc-signed `.dmg` and
an `x86_64` AppImage, neither carrying a developer signature. Windows is in the
build system but not exercised. Sending over the Slovak SAPI transport is not
finished; receiving, reading, storing and paying a supplier's e-invoice is.

## Licence

**GNU General Public License v3.0 or later** — see [LICENSE](LICENSE).
Every source file carries the notice and an `SPDX-License-Identifier` line.

Qt is used under the LGPL v3 — the Homebrew build on macOS, the online-installer
build bundled in the Linux AppImage — which the GPL v3 permits.
Everything else linked in — SQLite (public domain) and zlib — is compatible.
