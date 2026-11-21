#pragma once

#include <QByteArray>

namespace account::securestorage {

/// Stores the complete desktop credential envelope in the operating system's
/// credential vault. Secret material must never fall back to QSettings.
bool write(const QByteArray& value);
QByteArray read();
bool clear();

} // namespace account::securestorage
