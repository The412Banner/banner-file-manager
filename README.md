# Banner File Manager

The in-container file manager for **Bannerlator** — a fork of the
[Winlator File Manager](https://github.com/brunodev85/wfm) by BrunoSX (MIT).

Ships on disk as `wfm.exe` (so the Bannerlator launch path is unchanged) and is deployed
into a container's `C:\windows\wfm.exe`.

## Changes vs upstream WFM
- **Copy / move / delete no longer use shell32 `SHFileOperation`.** They are implemented
  directly on Win32 file APIs (`CopyFileW` / `MoveFileExW` / `DeleteFileW` +
  recursion), which sidesteps the Wine `shell32` copy-paste crash seen on Proton 10.0-4.
- Rebranded surface (About dialog / app name).

## Build
- **Windows:** as upstream — `w64devkit` + `Makefile` / `build.bat`.
- **CI (x86-64):** `.github/workflows/build-x64.yml` cross-builds `wfm.exe` with mingw-w64 on Ubuntu.

The x86-64 build is universal: it runs in both Bannerlator container arches — natively under
Box64 in x86-64 containers, and via wowbox64 in arm64ec containers.

Original project © BrunoSX, MIT — see `LICENSE`.

![Screenshot](screenshot.png)
