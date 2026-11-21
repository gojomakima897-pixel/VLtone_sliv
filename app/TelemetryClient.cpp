#include "TelemetryClient.hpp"

#include "AccountService.hpp"
#include "MainWindow.hpp"
#include "PlatformDiagnostics.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <algorithm>

namespace {
QString reporterPath() {
    QDir directory(QCoreApplication::applicationDirPath());
#if defined(Q_OS_WIN)
    const QString name = QStringLiteral("daw_reporter.exe");
#else
    const QString name = QStringLiteral("daw_reporter");
#endif
    const QString path = directory.filePath(name);
    return QFile::exists(path) ? path : QString();
}
}

TelemetryClient::TelemetryClient(MainWindow* window, QObject* parent)
    : QObject(parent), m_window(window), m_ipc(new QLocalServer(this)),
      m_sampleTimer(new QTimer(this)),
      m_sessionId(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    m_outbox = QDir(base).filePath(QStringLiteral("diagnostics-outbox"));
    QDir().mkpath(m_outbox);
    startReporter();
    enqueue(QStringLiteral("session_started"), {
        {QStringLiteral("app_version"), QCoreApplication::applicationVersion()},
        {QStringLiteral("build_id"), QCoreApplication::applicationVersion()},
        {QStringLiteral("hardware"), PlatformDiagnostics::hardwareSnapshot()},
    });
    m_sampleTimer->setInterval(1000);
    connect(m_sampleTimer, &QTimer::timeout, this, &TelemetryClient::sample);
    m_sampleTimer->start();
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        if (m_sampleCount) flushSample();
        enqueue(QStringLiteral("session_ended"), {{QStringLiteral("reason"), QStringLiteral("normal")}});
    });
}

void TelemetryClient::startReporter() {
    auto* service = account::Service::instance();
    const QString executable = reporterPath();
    if (!service || service->reporterToken().isEmpty() || executable.isEmpty()) return;
    const QString ipcName = QStringLiteral("vlt-telemetry-%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!m_ipc->listen(ipcName)) return;
    connect(m_ipc, &QLocalServer::newConnection, this, [this, service] {
        while (QLocalSocket* socket = m_ipc->nextPendingConnection()) {
            socket->write(service->reporterToken().toUtf8() + '\n');
            socket->flush();
            // Deliberately kept open: disconnect is the reporter's reliable
            // parent-death signal and carries no further secret material.
        }
    });
    connect(service, &account::Service::authenticatedChanged, this,
            [this, service](bool authenticated) {
        if (!authenticated || service->reporterToken().isEmpty()) return;
        const QByteArray line = service->reporterToken().toUtf8() + '\n';
        for (QLocalSocket* socket : m_ipc->findChildren<QLocalSocket*>()) {
            if (socket->state() == QLocalSocket::ConnectedState) {
                socket->write(line);
                socket->flush();
            }
        }
    });
    const QStringList arguments{
        QStringLiteral("--outbox"), m_outbox,
        QStringLiteral("--ipc"), ipcName,
        QStringLiteral("--origin"), service->apiOrigin(),
        QStringLiteral("--recovery"), m_window->recoverySessionDir(),
        QStringLiteral("--session"), m_sessionId,
        QStringLiteral("--app-version"), QCoreApplication::applicationVersion(),
        QStringLiteral("--build-id"), QCoreApplication::applicationVersion(),
    };
    QProcess::startDetached(executable, arguments);
}

void TelemetryClient::sample() {
    m_latest = m_window->telemetrySnapshot();
    m_processSum += m_latest.value(QStringLiteral("process_cpu")).toDouble();
    m_systemSum += m_latest.value(QStringLiteral("system_cpu")).toDouble();
    m_dspSum += m_latest.value(QStringLiteral("dsp_load")).toDouble();
    m_dspPeak = std::max(m_dspPeak, m_latest.value(QStringLiteral("dsp_peak")).toDouble());
    ++m_sampleCount;
    if (m_sampleCount >= 300) flushSample();
}

void TelemetryClient::flushSample() {
    if (!m_sampleCount) return;
    m_latest.insert(QStringLiteral("process_cpu"), m_processSum / m_sampleCount);
    m_latest.insert(QStringLiteral("system_cpu"), m_systemSum / m_sampleCount);
    m_latest.insert(QStringLiteral("dsp_load"), m_dspSum / m_sampleCount);
    m_latest.insert(QStringLiteral("dsp_peak"), m_dspPeak);
    enqueue(QStringLiteral("sample"), m_latest);
    m_processSum = m_systemSum = m_dspSum = m_dspPeak = 0.0;
    m_sampleCount = 0;
}

void TelemetryClient::enqueue(const QString& kind, const QJsonObject& payload) {
    const QJsonObject event{
        {QStringLiteral("event_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("session_id"), m_sessionId},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("occurred_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("payload"), payload},
    };
    const QJsonObject envelope{{QStringLiteral("body"),
        QJsonObject{{QStringLiteral("events"), QJsonArray{event}}}}};
    const QString name = QStringLiteral("%1-%2-%3.telemetry.json")
        .arg(QDateTime::currentMSecsSinceEpoch(), 16, 10, QLatin1Char('0'))
        .arg(++m_sequence, 6, 10, QLatin1Char('0'))
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSaveFile file(QDir(m_outbox).filePath(name));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
        file.commit();
    }
    trimOutbox();
}

void TelemetryClient::trimOutbox() {
    constexpr qint64 kLimit = 250LL * 1024 * 1024;
    QDir directory(m_outbox);
    QFileInfoList samples = directory.entryInfoList(
        {QStringLiteral("*.telemetry.json")}, QDir::Files, QDir::Time | QDir::Reversed);
    qint64 total = 0;
    for (const QFileInfo& item : directory.entryInfoList(QDir::Files)) total += item.size();
    for (const QFileInfo& sample : samples) {
        if (total <= kLimit) break;
        total -= sample.size();
        QFile::remove(sample.absoluteFilePath());
    }
}
