# VLT Studio Pro

VLT Studio Pro is a cross-platform digital audio workstation for Windows and macOS.

## Current version

**0.0.1**

This version is the first Qt 6 desktop release of the VLT Studio Pro line. The release uses a portable C++23 audio engine and a Qt Widgets interface.

## What is included

- realtime audio engine with tracks, clips, transport, tempo and mixing;
- recording, offline rendering and MIDI editing;
- VST3, CLAP and AU plug-in hosting;
- built-in sampler and audio-file browser;
- mixer, piano roll, automation and channel-strip presets;
- portable `.vlt` project packages;
- crash recovery journal and diagnostics reporter;
- integrated web browser and account platform;
- Windows x64 and macOS desktop build configurations.

## Update 0.0.1

- migrated the desktop application to Qt 6 and CMake;
- unified the C++23 engine, controller and desktop UI;
- added Windows x64 Release packaging with ZIP and Inno Setup installer output;
- added macOS app/package/DMG build flow;
- added portable project save/open support;
- added plug-in scanning, mixer, piano roll and sampler workflows;
- added crash recovery, headless self-test and CTest coverage;
- added the account, public web and administration workspaces.

## Install and build

Start with the step-by-step guide:

- [INSTALL.md](INSTALL.md) — Windows installation, Windows build and macOS build;
- [BUILD.md](BUILD.md) — complete toolchain, CI and packaging reference.

Quick macOS build:

```bash
brew install cmake ninja qt qtwebengine qtserialport portaudio libsndfile nlohmann-json
cmake --preset macos
cmake --build build
ctest --test-dir build --output-on-failure
./build/bin/daw
```

Quick Windows build:

```powershell
.\packaging\windows\build.ps1
```

The Windows build produces artifacts under `build-windows\\artifacts`. Use the `*-x64-Setup.exe` file for a normal installation or the ZIP file for a portable copy.

## Project format

A `.vlt` project is a portable package containing a project manifest, referenced media under `Content/`, and plug-in state under `State/`. Existing legacy `.dawp` packages remain readable.

## Repository layout

- `engine/` — realtime audio engine;
- `core/` — device I/O and media decoding;
- `controller/` — document model, editing and serialization;
- `app/` — Qt desktop application;
- `backend/`, `web/`, `admin/` — account and web platform;
- `tests/` — CTest and integration tests;
- `packaging/` — Windows and macOS packaging scripts.

## Platform note

This repository currently targets desktop Windows and macOS. It does not contain an iOS target; see [INSTALL.md](INSTALL.md) for the platform requirements.
