#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

class MainWindow;
class QLocalServer;
class QTimer;

/// Writes a bounded, privacy-filtered local outbox. Network I/O belongs to
/// daw_reporter so a crash cannot strand the final diagnostics in this process.
class TelemetryClient final : public QObject {
    Q_OBJECT
public:
    explicit TelemetryClient(MainWindow* window, QObject* parent = nullptr);

private:
    void startReporter();
    void sample();
    void flushSample();
    void enqueue(const QString& kind, const QJsonObject& payload);
    void trimOutbox();

    MainWindow* m_window = nullptr;
    QLocalServer* m_ipc = nullptr;
    QTimer* m_sampleTimer = nullptr;
    QString m_outbox;
    QString m_sessionId;
    QJsonObject m_latest;
    double m_processSum = 0.0;
    double m_systemSum = 0.0;
    double m_dspSum = 0.0;
    double m_dspPeak = 0.0;
    int m_sampleCount = 0;
    qint64 m_sequence = 0;
};
