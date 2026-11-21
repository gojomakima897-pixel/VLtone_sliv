#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>

class Reporter final : public QObject {
public:
    Reporter(QString outbox, QString ipcName, QString origin,
             QString recovery, QString sessionId, QString appVersion,
             QString buildId, QObject* parent = nullptr)
        : QObject(parent), m_outbox(std::move(outbox)), m_origin(std::move(origin)),
          m_recovery(std::move(recovery)), m_sessionId(std::move(sessionId)),
          m_appVersion(std::move(appVersion)), m_buildId(std::move(buildId)) {
        connect(&m_socket, &QLocalSocket::readyRead, this, [this] {
            m_tokenBuffer += m_socket.readAll();
            for (;;) {
                const int newline = m_tokenBuffer.indexOf('\n');
                if (newline < 0) break;
                const QString next = QString::fromUtf8(
                    m_tokenBuffer.left(newline)).trimmed();
                m_tokenBuffer.remove(0, newline + 1);
                if (!next.isEmpty()) m_token = next;
            }
            if (!m_token.isEmpty()) {
                m_timer.start(2500);
                drain();
            }
        });
        connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
            m_parentGone = true;
            drain();
        });
        connect(&m_timer, &QTimer::timeout, this, &Reporter::drain);
        m_socket.connectToServer(ipcName, QIODevice::ReadOnly);
        QTimer::singleShot(5000, this, [this] {
            if (m_token.isEmpty()) QCoreApplication::exit(2);
        });
    }

private:
    void drain() {
        if (m_busy || m_token.isEmpty()) return;
        QDir directory(m_outbox);
        const QFileInfoList telemetry = directory.entryInfoList(
            {QStringLiteral("*.telemetry.json")}, QDir::Files, QDir::Name);
        if (!telemetry.isEmpty()) {
            uploadTelemetry(telemetry.front().absoluteFilePath());
            return;
        }
        if (m_parentGone && !m_normalEnded && !m_crashStaged) stageCrash();
        const QFileInfoList crashes = directory.entryInfoList(
            {QStringLiteral("*.crash.json")}, QDir::Files, QDir::Name);
        if (!crashes.isEmpty()) {
            uploadCrash(crashes.front().absoluteFilePath());
            return;
        }
        if (m_parentGone) QTimer::singleShot(2500, qApp, &QCoreApplication::quit);
    }

    void uploadTelemetry(const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) { QFile::remove(path); drain(); return; }
        const QJsonDocument envelope = QJsonDocument::fromJson(file.readAll());
        file.close();
        const QJsonObject body = envelope.object().value(QStringLiteral("body")).toObject();
        for (const QJsonValue& value : body.value(QStringLiteral("events")).toArray())
            if (value.toObject().value(QStringLiteral("kind")).toString() == QLatin1String("session_ended"))
                m_normalEnded = true;
        QNetworkRequest request(QUrl(m_origin + QStringLiteral("/v1/desktop/telemetry/batch")));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
        m_busy = true;
        QNetworkReply* reply = m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, path] {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool success = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
            reply->deleteLater(); m_busy = false;
            if (success) {
                QFile::remove(path);
                drain();
                return;
            }
            if (m_parentGone) {
                if (!m_normalEnded && !m_crashStaged) stageCrash();
                QTimer::singleShot(2500, qApp, &QCoreApplication::quit);
            } else {
                QTimer::singleShot(5000, this, &Reporter::drain);
            }
        });
    }

    QByteArray limitedFile(const QString& path, qint64 maxBytes) const {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        if (file.size() > maxBytes) file.seek(file.size() - maxBytes);
        return file.read(maxBytes);
    }

    QString redactedText(const QByteArray& input) const {
        QString text = QString::fromUtf8(input);
        static const QRegularExpression pathPattern(
            QStringLiteral(R"((?:[A-Za-z]:[\\/]|/)[^\s\]\)\}\"']+)"));
        text.replace(pathPattern, QStringLiteral("<path-redacted>"));
        return text;
    }

    void stageCrash() {
        m_crashStaged = true;
        if (!QDir(m_recovery).exists()) return;
        const QByteArray marker = limitedFile(
            QDir(m_recovery).filePath(QStringLiteral("crash.txt")), 256 * 1024);
        const QByteArray health = limitedFile(
            QDir(m_recovery).filePath(QStringLiteral("health.jsonl")), 2 * 1024 * 1024);
        const QByteArray session = limitedFile(
            QDir(m_recovery).filePath(QStringLiteral("session.json")), 512 * 1024);
        const QByteArray structuredLog = limitedFile(
            QDir(m_recovery).filePath(QStringLiteral("app.log")), 2 * 1024 * 1024);
        const QString redactedMarker = redactedText(marker);
        const QString reason = marker.isEmpty()
            ? QStringLiteral("process_terminated_unexpectedly")
            : redactedMarker.left(512).trimmed();
        const QJsonObject rawSession = QJsonDocument::fromJson(session).object();
        const QJsonObject rawStats = rawSession.value(QStringLiteral("stats")).toObject();
        const QString lastPlugin = redactedText(
            rawStats.value(QStringLiteral("lastPlugin")).toString().toUtf8()).left(160);
        const QJsonObject safeStats{
            {QStringLiteral("dspLoad"), rawStats.value(QStringLiteral("dspLoad"))},
            {QStringLiteral("dspLoadPeak"), rawStats.value(QStringLiteral("dspLoadPeak"))},
            {QStringLiteral("xruns"), rawStats.value(QStringLiteral("xruns"))},
            {QStringLiteral("residentBytes"), rawStats.value(QStringLiteral("residentBytes"))},
            {QStringLiteral("sampleRate"), rawStats.value(QStringLiteral("sampleRate"))},
            {QStringLiteral("bufferFrames"), rawStats.value(QStringLiteral("bufferFrames"))},
            {QStringLiteral("trackCount"), rawStats.value(QStringLiteral("trackCount"))},
            {QStringLiteral("clipCount"), rawStats.value(QStringLiteral("clipCount"))},
            {QStringLiteral("lastPlugin"), lastPlugin},
        };
        const QJsonObject safeSession{
            {QStringLiteral("format"), rawSession.value(QStringLiteral("format"))},
            {QStringLiteral("version"), rawSession.value(QStringLiteral("version"))},
            {QStringLiteral("app"), rawSession.value(QStringLiteral("app"))},
            {QStringLiteral("startedUnixMs"), rawSession.value(QStringLiteral("startedUnixMs"))},
            {QStringLiteral("heartbeat"), rawSession.value(QStringLiteral("heartbeat"))},
            {QStringLiteral("heartbeatUnixMs"), rawSession.value(QStringLiteral("heartbeatUnixMs"))},
            {QStringLiteral("stats"), safeStats},
            {QStringLiteral("outcome"), rawSession.value(QStringLiteral("outcome"))},
            {QStringLiteral("crashReason"), redactedText(
                rawSession.value(QStringLiteral("crashReason")).toString().toUtf8()).left(512)},
        };
        const QString reportId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QJsonObject metadata{
            {QStringLiteral("report_id"), reportId},
            {QStringLiteral("session_id"), m_sessionId},
            {QStringLiteral("build_id"), m_buildId},
            {QStringLiteral("app_version"), m_appVersion},
#if defined(Q_OS_WIN)
            {QStringLiteral("platform"), QStringLiteral("windows")},
#else
            {QStringLiteral("platform"), QStringLiteral("macos")},
#endif
            {QStringLiteral("reason"), reason.left(512)},
            {QStringLiteral("last_plugin"), lastPlugin},
            {QStringLiteral("occurred_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {QStringLiteral("exception_code"), QString()},
            {QStringLiteral("signal"), QString()},
            {QStringLiteral("health_samples"), QJsonArray{}},
            {QStringLiteral("modules"), QJsonArray{}},
        };
        QByteArray artifact = QJsonDocument(QJsonObject{
            {QStringLiteral("format"), QStringLiteral("vlt-crash-bundle-1")},
            {QStringLiteral("crash_marker"), redactedMarker},
            {QStringLiteral("health_log"), redactedText(health)},
            {QStringLiteral("session"), safeSession},
            {QStringLiteral("structured_log"), redactedText(structuredLog)},
        }).toJson(QJsonDocument::Compact);
        QString artifactName = QStringLiteral("crash.json");
        QString artifactType = QStringLiteral("application/json");
        QFile dump(QDir(m_recovery).filePath(QStringLiteral("crash.dmp")));
        if (dump.exists() && dump.size() > 0 && dump.size() <= 50LL * 1024 * 1024 &&
            dump.open(QIODevice::ReadOnly)) {
            artifact = dump.readAll();
            artifactName = QStringLiteral("crash.dmp");
            artifactType = QStringLiteral("application/octet-stream");
        }
        const QString stem = QStringLiteral("%1-%2")
            .arg(QDateTime::currentMSecsSinceEpoch(), 16, 10, QLatin1Char('0'))
            .arg(reportId);
        const QString artifactFile = stem + QStringLiteral(".crash.bin");
        QSaveFile storedArtifact(QDir(m_outbox).filePath(artifactFile));
        if (!storedArtifact.open(QIODevice::WriteOnly) ||
            storedArtifact.write(artifact) != artifact.size() || !storedArtifact.commit()) return;
        const QJsonObject envelope{
            {QStringLiteral("metadata"), metadata},
            {QStringLiteral("artifact_file"), artifactFile},
            {QStringLiteral("artifact_name"), artifactName},
            {QStringLiteral("artifact_type"), artifactType},
        };
        QSaveFile storedEnvelope(QDir(m_outbox).filePath(stem + QStringLiteral(".crash.json")));
        if (!storedEnvelope.open(QIODevice::WriteOnly) ||
            storedEnvelope.write(QJsonDocument(envelope).toJson(QJsonDocument::Compact)) < 0 ||
            !storedEnvelope.commit()) {
            QFile::remove(QDir(m_outbox).filePath(artifactFile));
            return;
        }
        trimCrashes();
    }

    void trimCrashes() {
        QDir directory(m_outbox);
        QFileInfoList envelopes = directory.entryInfoList(
            {QStringLiteral("*.crash.json")}, QDir::Files, QDir::Name);
        while (envelopes.size() > 5) {
            QFile file(envelopes.front().absoluteFilePath());
            if (file.open(QIODevice::ReadOnly)) {
                const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
                const QString artifact = object.value(QStringLiteral("artifact_file")).toString();
                if (QFileInfo(artifact).fileName() == artifact)
                    QFile::remove(directory.filePath(artifact));
            }
            QFile::remove(envelopes.takeFirst().absoluteFilePath());
        }
    }

    void uploadCrash(const QString& envelopePath) {
        QFile envelopeFile(envelopePath);
        if (!envelopeFile.open(QIODevice::ReadOnly)) { QFile::remove(envelopePath); drain(); return; }
        const QJsonObject envelope = QJsonDocument::fromJson(envelopeFile.readAll()).object();
        const QString artifactNameOnDisk = envelope.value(QStringLiteral("artifact_file")).toString();
        if (QFileInfo(artifactNameOnDisk).fileName() != artifactNameOnDisk) {
            QFile::remove(envelopePath); drain(); return;
        }
        const QString artifactPath = QDir(m_outbox).filePath(artifactNameOnDisk);
        QFile artifactFile(artifactPath);
        if (envelope.isEmpty() || !artifactFile.open(QIODevice::ReadOnly) ||
            artifactFile.size() > 50LL * 1024 * 1024) {
            QFile::remove(envelopePath); QFile::remove(artifactPath); drain(); return;
        }
        auto* multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
        QHttpPart metadataPart;
        metadataPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                               QVariant(QStringLiteral("form-data; name=\"metadata\"")));
        metadataPart.setBody(QJsonDocument(envelope.value(QStringLiteral("metadata")).toObject())
                                 .toJson(QJsonDocument::Compact));
        multipart->append(metadataPart);
        QHttpPart artifactPart;
        artifactPart.setHeader(QNetworkRequest::ContentDispositionHeader,
            QVariant(QStringLiteral("form-data; name=\"artifact\"; filename=\"%1\"")
                         .arg(envelope.value(QStringLiteral("artifact_name")).toString())));
        artifactPart.setHeader(QNetworkRequest::ContentTypeHeader,
                               envelope.value(QStringLiteral("artifact_type")).toString());
        artifactPart.setBody(artifactFile.readAll());
        multipart->append(artifactPart);
        QNetworkRequest request(QUrl(m_origin + QStringLiteral("/v1/desktop/crashes")));
        request.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
        m_busy = true;
        QNetworkReply* reply = m_network.post(request, multipart);
        multipart->setParent(reply);
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, envelopePath, artifactPath] {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool success = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
            reply->deleteLater(); m_busy = false;
            if (success) {
                QFile::remove(envelopePath); QFile::remove(artifactPath); drain();
            } else if (m_parentGone) {
                QTimer::singleShot(2500, qApp, &QCoreApplication::quit);
            } else {
                QTimer::singleShot(5000, this, &Reporter::drain);
            }
        });
    }

    QString m_outbox, m_origin, m_recovery, m_sessionId, m_appVersion, m_buildId;
    QLocalSocket m_socket;
    QNetworkAccessManager m_network;
    QTimer m_timer;
    QByteArray m_tokenBuffer;
    QString m_token;
    bool m_parentGone = false;
    bool m_normalEnded = false;
    bool m_crashStaged = false;
    bool m_busy = false;
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("daw_reporter"));
    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption outbox(QStringLiteral("outbox"), {}, QStringLiteral("path"));
    const QCommandLineOption ipc(QStringLiteral("ipc"), {}, QStringLiteral("name"));
    const QCommandLineOption origin(QStringLiteral("origin"), {}, QStringLiteral("url"));
    const QCommandLineOption recovery(QStringLiteral("recovery"), {}, QStringLiteral("path"));
    const QCommandLineOption session(QStringLiteral("session"), {}, QStringLiteral("uuid"));
    const QCommandLineOption version(QStringLiteral("app-version"), {}, QStringLiteral("version"));
    const QCommandLineOption build(QStringLiteral("build-id"), {}, QStringLiteral("id"));
    parser.addOptions({outbox, ipc, origin, recovery, session, version, build});
    parser.process(app);
    if (parser.value(outbox).isEmpty() || parser.value(ipc).isEmpty() ||
        parser.value(origin).isEmpty() || parser.value(session).isEmpty()) return 2;
    Reporter reporter(parser.value(outbox), parser.value(ipc), parser.value(origin),
                      parser.value(recovery), parser.value(session),
                      parser.value(version), parser.value(build));
    return app.exec();
}
