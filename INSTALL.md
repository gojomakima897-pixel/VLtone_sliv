# Установка и сборка VLT Studio Pro

## Быстрая установка в Windows

Если в разделе **Releases** опубликован установщик:

1. Скачайте `VLT-Studio-Pro-0.0.1-x64-Setup.exe`.
2. Запустите файл и завершите установку.
3. Откройте VLT Studio Pro через меню «Пуск».
4. Установщик регистрирует проекты `.vlt` и сохраняет пользовательские настройки отдельно от каталога программы.

Для portable-режима скачайте `VLT-Studio-Pro-0.0.1-windows-x64.zip`, распакуйте архив и запустите `daw.exe` или исполняемый файл из папки staging, если имя указано иначе в артефакте.

## Сборка Windows из исходников

### Что установить

- Windows 10/11 x64;
- Visual Studio 2022 с рабочей нагрузкой **Desktop development with C++**;
- MSVC v143 и Windows SDK 10.0.26100.0;
- CMake 3.24 или новее;
- Ninja;
- Git;
- Qt 6.8.3 для `msvc2022_64`, включая WebEngine и SerialPort;
- Inno Setup 6.7.1 — для создания установщика.

### Сборка

Откройте PowerShell в корне репозитория:

```powershell
.\packaging\windows\build.ps1
```

Скрипт подготавливает vcpkg, собирает Release x64, запускает CTest и формирует артефакты:

```text
build-windows\artifacts\
```

Для сборки без Reverb:

```powershell
.\packaging\windows\build.ps1 -DisableReverb
```

## Сборка macOS

Этот репозиторий содержит desktop-приложение для macOS, а не iOS-приложение.

```bash
brew install cmake ninja qt qtwebengine qtserialport portaudio libsndfile nlohmann-json
cmake --preset macos
cmake --build build
ctest --test-dir build --output-on-failure
./build/bin/daw
```

Для создания `.app`, `.pkg` и `.dmg`:

```bash
packaging/macos/build-pkg.sh
```

## Поддержка iOS

Готового iOS-target'а в этом репозитории нет. Нельзя просто открыть существующий desktop-target в Xcode и получить приложение для iPhone/iPad. Для iOS понадобится отдельный Qt iOS kit, мобильный UI, iOS signing/provisioning и адаптация аудио- и плагинного слоя.

## Полная документация

- [README.md](README.md);
- [BUILD.md](BUILD.md);
- [CMakePresets.json](CMakePresets.json).
