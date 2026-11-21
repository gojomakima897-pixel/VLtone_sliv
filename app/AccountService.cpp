#include "AccountService.hpp"

#include "PlatformDiagnostics.hpp"
#include "SecureStorage.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QSysInfo>
#include <QThread>
#include <QTimeZone>
#include <QTimer>
#include <QUuid>

#include <openssl/evp.h>

#include <algorithm>

namespace account {

Service* Service::s_instance = nullptr;

namespace {
#ifndef VLT_DEFAULT_API_ORIGIN
#define VLT_DEFAULT_API_ORIGIN "http://localhost:8080"
#endif

QByteArray decodeBase64Url(QByteArray value) {
    while (value.size() % 4) value.append('=');
    return QByteArray::fromBase64(value, QByteArray::Base64UrlEncoding);
}

bool verifyEntitlement(const QString& token, const QString& publicKey,
                       QJsonObject* claims) {
    const QList<QByteArray> parts = token.toUtf8().split('.');
    if (parts.size() != 3) return false;
    const QByteArray key = QByteArray::fromBase64(publicKey.toUtf8());
    const QByteArray signature = decodeBase64Url(parts.at(2));
    if (key.size() != 32 || signature.size() != 64) return false;
    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char*>(key.constData()), size_t(key.size()));
    if (!pkey) return false;
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    const QByteArray signedBody = parts.at(0) + '.' + parts.at(1);
    const bool valid = context && EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, pkey) == 1 &&
        EVP_DigestVerify(context,
            reinterpret_cast<const unsigned char*>(signature.constData()), size_t(signature.size()),
            reinterpret_cast<const unsigned char*>(signedBody.constData()), size_t(signedBody.size())) == 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(pkey);
    if (!valid) return false;
    const QJsonDocument payload = QJsonDocument::fromJson(decodeBase64Url(parts.at(1)));
    if (!payload.isObject()) return false;
    *claims = payload.object();
    return claims->value(QStringLiteral("scope")).toString() == QLatin1String("offline");
}

QString platformName() {
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#else
    return QStringLiteral("unsupported");
#endif
}
}

Service::Service(QObject* parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this)),
      m_refreshTimer(new QTimer(this)) {
    Q_ASSERT(!s_instance);
    s_instance = this;
    m_apiOrigin = qEnvironmentVariable(
        "VLT_API_ORIGIN", QString::fromUtf8(VLT_DEFAULT_API_ORIGIN));
    while (m_apiOrigin.endsWith('/')) m_apiOrigin.chop(1);

    // One-way privacy migration from pre-account builds. Values are removed
    // locally and are never read into memory or transmitted to VLT servers.
    QSettings settings;
    for (const QString& key : {
             QStringLiteral("ai/apiKey.openai"), QStringLiteral("ai/apiKey.anthropic"),
             QStringLiteral("ai/baseUrl.openai"), QStringLiteral("ai/baseUrl.anthropic"),
             QStringLiteral("ai/music.apiKey")}) {
        settings.remove(key);
    }
    settings.setValue(QStringLiteral("account/legacyAiSecretsRemoved"), true);
    m_refreshTimer->setInterval(12 * 60 * 1000);
    connect(m_refreshTimer, &QTimer::timeout, this, &Service::beginRestore);
}

Service* Service::instance() { return s_instance; }

QString Service::installationId() const {
    QSettings settings;
    QString value = settings.value(QStringLiteral("account/installationId")).toString();
    if (QUuid(value).isNull()) {
        value = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(QStringLiteral("account/installationId"), value);
    }
    return value;
}

void Service::beginRestore() {
    if (m_busy) return;
    const QJsonDocument stored = QJsonDocument::fromJson(securestorage::read());
    if (!stored.isObject()) {
        emit authenticationRequired(tr("Sign in to continue."), false);
        return;
    }
    const QJsonObject credentials = stored.object();
    m_refreshToken = credentials.value(QStringLiteral("refresh_token")).toString();
    if (m_refreshToken.isEmpty()) {
        securestorage::clear();
        emit authenticationRequired(tr("Sign in to continue."), false);
        return;
    }
    setBusy(true);
    QNetworkReply* reply = postJson(QStringLiteral("/v1/desktop/auth/refresh"), {
        {QStringLiteral("refresh_token"), m_refreshToken},
        {QStringLiteral("app_version"), QCoreApplication::applicationVersion()},
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, credentials] {
        setBusy(false);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError && status >= 200 && status < 300) {
            handleSessionReply(reply, true);
            return;
        }
        const bool transportFailure = status == 0;
        reply->deleteLater();
        QString reason;
        if (transportFailure && acceptOffline(credentials, &reason)) return;
        if (!transportFailure) securestorage::clear();
        emit authenticationRequired(reason.isEmpty() ? tr("Online sign-in is required.") : reason,
                                    transportFailure);
    });
}

void Service::login(const QString& email, const QString& password) {
    if (m_busy) return;
    setBusy(true);
    const QJsonObject hardware = PlatformDiagnostics::hardwareSnapshot();
    QNetworkReply* reply = postJson(QStringLiteral("/v1/desktop/auth/login"), {
        {QStringLiteral("email"), email.trimmed()},
        {QStringLiteral("password"), password},
        {QStringLiteral("installation_id"), installationId()},
        {QStringLiteral("device_name"), platformName() == QLatin1String("windows")
             ? tr("Windows device") : tr("Mac device")},
        {QStringLiteral("platform"), platformName()},
        {QStringLiteral("os_version"), QSysInfo::prettyProductName()},
        {QStringLiteral("app_version"), QCoreApplication::applicationVersion()},
        {QStringLiteral("hardware"), hardware},
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        setBusy(false);
        handleSessionReply(reply, false);
    });
}

void Service::handleSessionReply(QNetworkReply* reply, bool refresh) {
    const QByteArray body = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonDocument document = QJsonDocument::fromJson(body);
    reply->deleteLater();
    if (status < 200 || status >= 300 || !document.isObject()) {
        const QJsonObject error = document.object();
        const QString code = error.value(QStringLiteral("code")).toString(
            refresh ? QStringLiteral("refresh_failed") : QStringLiteral("login_failed"));
        const QString message = error.value(QStringLiteral("message")).toString(
            tr("The server could not complete sign-in."));
        if (refresh) emit authenticationRequired(message, false);
        else emit errorOccurred(code, message);
        return;
    }
    acceptSession(document.object());
}

void Service::acceptSession(const QJsonObject& response) {
    m_accessToken = response.value(QStringLiteral("access_token")).toString();
    m_refreshToken = response.value(QStringLiteral("refresh_token")).toString();
    m_reporterToken = response.value(QStringLiteral("reporter_token")).toString();
    m_offlineEntitlement = response.value(QStringLiteral("offline_entitlement")).toString();
    m_publicKey = response.value(QStringLiteral("public_key")).toString();
    m_lastServerTime = QDateTime::fromString(response.value(QStringLiteral("server_time")).toString(), Qt::ISODate).toSecsSinceEpoch();
    const QJsonObject user = response.value(QStringLiteral("user")).toObject();
    const QJsonObject device = response.value(QStringLiteral("device")).toObject();
    const QJsonObject quota = response.value(QStringLiteral("quota")).toObject();
    m_snapshot.userId = user.value(QStringLiteral("id")).toString();
    m_snapshot.email = user.value(QStringLiteral("email")).toString();
    m_snapshot.nickname = user.value(QStringLiteral("nickname")).toString();
    m_snapshot.deviceId = device.value(QStringLiteral("id")).toString();
    m_snapshot.tokenLimit = qint64(quota.value(QStringLiteral("base_limit")).toDouble()) +
                            qint64(quota.value(QStringLiteral("adjustment")).toDouble());
    m_snapshot.tokensUsed = qint64(quota.value(QStringLiteral("used_tokens")).toDouble());
    m_snapshot.tokensRemaining = qint64(quota.value(QStringLiteral("remaining_tokens")).toDouble());
    m_snapshot.cycleEndsAt = QDateTime::fromString(quota.value(QStringLiteral("ends_at")).toString(), Qt::ISODate);
    m_snapshot.lastSyncAt = QDateTime::fromSecsSinceEpoch(m_lastServerTime, QTimeZone::UTC);
    m_snapshot.offline = false;
    persistCredentials(response);
    m_authenticated = true;
    m_refreshTimer->start();
    emit snapshotChanged();
    emit authenticatedChanged(true);
}

void Service::persistCredentials(const QJsonObject& response) {
    QJsonObject stored;
    for (const QString& key : {QStringLiteral("refresh_token"), QStringLiteral("reporter_token"),
                               QStringLiteral("offline_entitlement"), QStringLiteral("public_key"),
                               QStringLiteral("server_time"), QStringLiteral("offline_expires_at")}) {
        stored.insert(key, response.value(key));
    }
    const QJsonObject user = response.value(QStringLiteral("user")).toObject();
    stored.insert(QStringLiteral("user"), QJsonObject{
        {QStringLiteral("id"), user.value(QStringLiteral("id"))},
        {QStringLiteral("email"), user.value(QStringLiteral("email"))},
        {QStringLiteral("nickname"), user.value(QStringLiteral("nickname"))},
    });
    const QJsonObject device = response.value(QStringLiteral("device")).toObject();
    stored.insert(QStringLiteral("device"), QJsonObject{
        {QStringLiteral("id"), device.value(QStringLiteral("id"))},
    });
    const QJsonObject quota = response.value(QStringLiteral("quota")).toObject();
    QJsonObject storedQuota;
    for (const QString& key : {QStringLiteral("base_limit"), QStringLiteral("adjustment"),
                               QStringLiteral("used_tokens"), QStringLiteral("remaining_tokens"),
                               QStringLiteral("ends_at")}) {
        storedQuota.insert(key, quota.value(key));
    }
    stored.insert(QStringLiteral("quota"), storedQuota);
    stored.insert(QStringLiteral("last_observed_time"),
                  response.value(QStringLiteral("server_time")));
    if (!securestorage::write(QJsonDocument(stored).toJson(QJsonDocument::Compact))) {
        emit errorOccurred(QStringLiteral("secure_storage_failed"),
                           tr("The operating-system credential vault could not save this session."));
    }
}

bool Service::acceptOffline(const QJsonObject& credentials, QString* reason) {
    const QString entitlement = credentials.value(QStringLiteral("offline_entitlement")).toString();
    const QString publicKey = credentials.value(QStringLiteral("public_key")).toString();
    QJsonObject claims;
    if (!verifyEntitlement(entitlement, publicKey, &claims)) {
        *reason = tr("The stored offline entitlement is invalid.");
        return false;
    }
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 lastServer = QDateTime::fromString(credentials.value(QStringLiteral("server_time")).toString(), Qt::ISODate).toSecsSinceEpoch();
    const qint64 lastObserved = QDateTime::fromString(
        credentials.value(QStringLiteral("last_observed_time")).toString(),
        Qt::ISODate).toSecsSinceEpoch();
    if (now + 300 < std::max(lastServer, lastObserved)) {
        *reason = tr("The system clock was moved backwards. Connect to the server to continue.");
        return false;
    }
    if (claims.value(QStringLiteral("exp")).toInteger() <= now) {
        *reason = tr("The 72-hour offline allowance has expired.");
        return false;
    }
    const QJsonObject user = credentials.value(QStringLiteral("user")).toObject();
    const QJsonObject device = credentials.value(QStringLiteral("device")).toObject();
    const QJsonObject quota = credentials.value(QStringLiteral("quota")).toObject();
    m_refreshToken = credentials.value(QStringLiteral("refresh_token")).toString();
    m_reporterToken = credentials.value(QStringLiteral("reporter_token")).toString();
    m_offlineEntitlement = entitlement;
    m_publicKey = publicKey;
    m_lastServerTime = lastServer;
    m_snapshot.userId = user.value(QStringLiteral("id")).toString();
    m_snapshot.email = user.value(QStringLiteral("email")).toString();
    m_snapshot.nickname = user.value(QStringLiteral("nickname")).toString();
    m_snapshot.deviceId = device.value(QStringLiteral("id")).toString();
    m_snapshot.tokenLimit = qint64(quota.value(QStringLiteral("base_limit")).toDouble()) + qint64(quota.value(QStringLiteral("adjustment")).toDouble());
    m_snapshot.tokensUsed = qint64(quota.value(QStringLiteral("used_tokens")).toDouble());
    m_snapshot.tokensRemaining = qint64(quota.value(QStringLiteral("remaining_tokens")).toDouble());
    m_snapshot.cycleEndsAt = QDateTime::fromString(quota.value(QStringLiteral("ends_at")).toString(), Qt::ISODate);
    m_snapshot.lastSyncAt = QDateTime::fromSecsSinceEpoch(lastServer, QTimeZone::UTC);
    m_snapshot.offline = true;
    m_authenticated = true;
    m_refreshTimer->start();
    QJsonObject updated = credentials;
    updated.insert(QStringLiteral("last_observed_time"),
                   QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!securestorage::write(QJsonDocument(updated).toJson(QJsonDocument::Compact))) {
        *reason = tr("The operating-system credential vault could not update the offline clock guard.");
        m_authenticated = false;
        m_refreshTimer->stop();
        return false;
    }
    emit snapshotChanged();
    emit authenticatedChanged(true);
    return true;
}

void Service::logout() {
    if (!m_accessToken.isEmpty()) {
        QNetworkReply* reply = postJson(QStringLiteral("/v1/desktop/auth/logout"), {}, m_accessToken);
        QTimer::singleShot(5000, reply, [reply] {
            if (reply->isRunning()) reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            reply->deleteLater();
            finishLogout();
        });
        return;
    }
    finishLogout();
}

void Service::finishLogout() {
    securestorage::clear();
    m_refreshTimer->stop();
    m_accessToken.clear(); m_refreshToken.clear(); m_reporterToken.clear();
    m_offlineEntitlement.clear(); m_publicKey.clear(); m_snapshot = {};
    m_authenticated = false;
    emit snapshotChanged();
    emit authenticatedChanged(false);
    emit logoutFinished();
}

void Service::installHeadlessTestSession() {
    m_snapshot.email = QStringLiteral("selftest@vlt.invalid");
    m_snapshot.nickname = QStringLiteral("Self-test");
    m_snapshot.deviceId = QStringLiteral("00000000-0000-0000-0000-000000000001");
    m_snapshot.lastSyncAt = QDateTime::currentDateTimeUtc();
    m_authenticated = true;
}

void Service::setBusy(bool busy) {
    if (m_busy == busy) return;
    m_busy = busy;
    emit busyChanged(busy);
}

QNetworkReply* Service::postJson(const QString& path, const QJsonObject& body,
                                 const QString& bearer) {
    QNetworkRequest request(QUrl(m_apiOrigin + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    if (!bearer.isEmpty()) request.setRawHeader("Authorization", "Bearer " + bearer.toUtf8());
    return m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

} // namespace account
