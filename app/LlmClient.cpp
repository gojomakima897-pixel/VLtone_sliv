#include "LlmClient.hpp"
#include "AccountService.hpp"

#include <QNetworkReply>
#include <QUrl>

using json = nlohmann::json;
namespace ai = daw::ai;
namespace wire = daw::ai::wire;

namespace ui {

namespace {

/// Long enough for a model that is thinking, short enough that a dead endpoint
/// does not leave the panel spinning forever.
constexpr int kTimeoutMs = 180'000;

std::string toStd(const QString& s) { return s.toStdString(); }

} // namespace

LlmClient::LlmClient(Provider provider, QObject* parent)
    : QObject(parent), m_provider(provider) {
    m_net.setTransferTimeout(kTimeoutMs);
}

LlmClient::~LlmClient() { cancel(); }

bool LlmClient::busy() const { return !m_inFlight.isNull(); }

QString LlmClient::displayName() const {
    return m_provider == Provider::Anthropic ? QStringLiteral("Claude")
                                             : QStringLiteral("GPT");
}

QString LlmClient::defaultModel() const {
    return m_provider == Provider::Anthropic
               ? QStringLiteral("claude-sonnet-4-5")
               : QStringLiteral("gpt-4o");
}

QNetworkRequest LlmClient::buildRequest() const {
    const bool anthropic = m_provider == Provider::Anthropic;
    auto* account = account::Service::instance();
    const QString provider = anthropic ? QStringLiteral("anthropic/messages")
                                       : QStringLiteral("openai/chat/completions");
    QNetworkRequest request{QUrl(account->apiOrigin() +
                                 QStringLiteral("/v1/desktop/ai/") + provider)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", "Bearer " + m_config.accessToken.toUtf8());
    return request;
}

void LlmClient::answer(ai::ModelReply reply) {
    // Taken by move first: the callback may start the next request, and that
    // must not be clobbered by this one finishing.
    Reply callback = std::move(m_onReply);
    m_onReply = nullptr;
    m_decoder.reset();
    if (callback) callback(std::move(reply));
}

void LlmClient::send(const QString& system,
                     const std::vector<ai::Message>& messages, Reply onReply) {
    cancel();
    m_onReply = std::move(onReply);

    auto* account = account::Service::instance();
    if (!account || !account->authenticated() || m_config.accessToken.isEmpty()) {
        ai::ModelReply reply;
        reply.error = "VLT account authorization is unavailable. Sign in again.";
        answer(std::move(reply));
        return;
    }

    const QString model =
        m_config.model.isEmpty() ? defaultModel() : m_config.model;
    const bool stream = m_config.stream;
    // The Go proxy terminates the account token and adds the provider secret;
    // the desktop never constructs or calls a provider URL.
    const json body = wire::requestBody(m_provider, toStd(model),
                                        m_config.maxTokens, toStd(system),
                                        messages, stream,
                                        /*vendorExtensions=*/true);

    QNetworkReply* http =
        m_net.post(buildRequest(), QByteArray::fromStdString(body.dump()));
    m_inFlight = http;
    m_decoder = stream ? std::make_unique<wire::StreamDecoder>(m_provider)
                       : nullptr;

    if (stream) {
        // Read as it arrives, so the panel can show the answer being written
        // rather than appearing all at once several seconds later.
        connect(http, &QNetworkReply::readyRead, this, [this, http] {
            if (m_inFlight != http || !m_decoder) return;
            const QByteArray chunk = http->readAll();
            m_decoder->feed(
                std::string_view(chunk.constData(), std::size_t(chunk.size())));
            if (m_partialSink) {
                const std::string text = m_decoder->takeText();
                if (!text.empty()) m_partialSink(QString::fromStdString(text));
            }
        });
    }

    connect(http, &QNetworkReply::finished, this, [this, http] {
        http->deleteLater();
        // Cancelled while in flight: the callback was already dropped, and the
        // session has moved on.
        if (m_inFlight != http) return;
        m_inFlight = nullptr;

        const QByteArray payload = http->readAll();
        const int status =
            http->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool failed =
            http->error() != QNetworkReply::NoError || status >= 400;

        // A request that failed answers with a plain JSON error body rather
        // than events, streamed or not, so the failure path is shared.
        json parsed = json::parse(payload.constData(),
                                  payload.constData() + payload.size(), nullptr,
                                  /*allow_exceptions=*/false);

        ai::ModelReply reply;
        if (failed) {
            // The provider's own message is far more useful than Qt's, so it
            // wins whenever the body carried one.
            QString detail;
            if (!parsed.is_discarded())
                detail =
                    QString::fromStdString(wire::errorMessage(m_provider, parsed));
            if (detail.isEmpty() && m_decoder && !m_decoder->reply().error.empty())
                detail = QString::fromStdString(m_decoder->reply().error);
            if (detail.isEmpty()) detail = http->errorString();
            if (detail.isEmpty()) detail = QStringLiteral("request failed");
            reply.error = toStd(displayName() + ": " + detail +
                                (status ? QStringLiteral(" (HTTP %1)").arg(status)
                                        : QString()));
        } else if (m_decoder) {
            // Whatever arrived in the same read as `finished`.
            m_decoder->feed(std::string_view(payload.constData(),
                                             std::size_t(payload.size())));
            if (m_partialSink) {
                const std::string text = m_decoder->takeText();
                if (!text.empty()) m_partialSink(QString::fromStdString(text));
            }
            reply = m_decoder->reply();
            if (m_usageSink) m_usageSink(m_decoder->usage());
        } else if (parsed.is_discarded()) {
            reply.error = toStd(displayName()) + " sent something that is not JSON";
        } else {
            reply = wire::parseReply(m_provider, parsed);
            if (m_usageSink) m_usageSink(wire::parseUsage(m_provider, parsed));
        }
        answer(std::move(reply));
    });
}

void LlmClient::cancel() {
    m_onReply = nullptr;
    m_decoder.reset();
    if (QNetworkReply* http = m_inFlight) {
        m_inFlight = nullptr;
        http->abort();
    }
}

} // namespace ui
