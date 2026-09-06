; SPDX-License-Identifier: GPL-3.0-or-later
; Fakturo — invoicing for Slovak and Czech sole traders
; Copyright (C) 2026 Peter Mercell
;
; This program is free software: you can redistribute it and/or modify
; it under the terms of the GNU General Public License as published by
; the Free Software Foundation, either version 3 of the License, or
; (at your option) any later version.
;
; This program is distributed in the hope that it will be useful,
; but WITHOUT ANY WARRANTY; without even the implied warranty of
; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
; GNU General Public License for more details.
;
; You should have received a copy of the GNU General Public License
; along with this program.  If not, see <https://www.gnu.org/licenses/>.

; fakturo.iss - Inno Setup script. Driven by scripts/make-windows.ps1, which
; passes the version and the staged folder in rather than having them written
; down here where they would drift from the build.
;
;   ISCC.exe /DAppVersion=0.15.0 /DStageDir=...\Fakturo /DOutDir=...  fakturo.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\build-msvc\Release"
#endif
#ifndef OutDir
  #define OutDir "..\build-msvc"
#endif

[Setup]
AppId={{7E2A9C41-5B3D-4F18-9A6E-FA2026FAKTURO}
AppName=Fakturo
AppVersion={#AppVersion}
AppVerName=Fakturo {#AppVersion}
AppPublisher=studio 202 s. r. o.
DefaultDirName={autopf}\Fakturo
DefaultGroupName=Fakturo
UninstallDisplayIcon={app}\Fakturo.exe
OutputDir={#OutDir}
OutputBaseFilename=Fakturo-{#AppVersion}-windows-setup
SetupIconFile=Fakturo.ico
Compression=lzma2/max
SolidCompression=yes

; 64-bit only. The money arithmetic leans on 64-bit integers throughout, and
; nobody is running a 32-bit Windows in 2026.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; Per-user by default, so no administrator prompt. Anyone who wants it in
; Program Files for everyone can choose that at the first screen.
PrivilegesRequiredOverridesAllowed=dialog
PrivilegesRequired=lowest

WizardStyle=modern

[Languages]
Name: "slovak";  MessagesFile: "compiler:Languages\Slovak.isl"
Name: "czech";   MessagesFile: "compiler:Languages\Czech.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; \
    GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Everything windeployqt put in the staged folder, subdirectories and all —
; the platform plugin lives in platforms\, and the application will not start
; without it.
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Fakturo";           Filename: "{app}\Fakturo.exe"
Name: "{group}\Odinštalovať Fakturo"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Fakturo";     Filename: "{app}\Fakturo.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Fakturo.exe"; Description: "{cm:LaunchProgram,Fakturo}"; \
    Flags: nowait postinstall skipifsilent

; Nothing in [UninstallDelete]. The database lives in the user's own data
; directory and is not this installer's to remove: someone reinstalling a
; version would lose every invoice they have ever issued.
