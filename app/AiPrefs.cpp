#include "AiPrefs.hpp"

#include <QSettings>
#include <QStandardPaths>


#include <algorithm>

namespace ui::aiprefs {

namespace {

QString key(const char* name) {
    return QStringLiteral("ai/") + QLatin1String(name);
}

/// Per-provider keys are suffixed rather than nested, so both providers'
/// settings survive switching between them.
QString key(const char* name, Provider provider) {
    return key(name) + QStringLiteral(".") + providerId(provider);
}

} // namespace

QString providerId(Provider provider) {
    return provider == Provider::OpenAi ? QStringLiteral("openai")
                                        : QStringLiteral("anthropic");
}

Provider providerFromId(const QString& id) {
    return id == QLatin1String("openai") ? Provider::OpenAi : Provider::Anthropic;
}

Provider provider() {
    return providerFromId(
        QSettings().value(key("provider"), QStringLiteral("anthropic")).toString());
}

void setProvider(Provider provider) {
    QSettings().setValue(key("provider"), providerId(provider));
}

QString model(Provider provider) {
    return QSettings().value(key("model", provider)).toString();
}

void setModel(Provider provider, const QString& value) {
    QSettings().setValue(key("model", provider), value);
}

QStringList models(Provider provider) {
    QStringList listed =
        QSettings().value(key("models", provider)).toStringList();
    listed.removeAll(QString());
    // The model actually in use is always offered, even if it was typed into
    // an older build that had no list — otherwise switching away from it in the
    // panel would be a one-way door.
    const QString active = model(provider);
    if (!active.isEmpty() && !listed.contains(active)) listed.prepend(active);
    return listed;
}

void setModels(Provider provider, const QStringList& value) {
    QStringList cleaned;
    for (const QString& entry : value) {
        const QString trimmed = entry.trimmed();
        if (!trimmed.isEmpty() && !cleaned.contains(trimmed)) cleaned.append(trimmed);
    }
    QSettings().setValue(key("models", provider), cleaned);
}

Mode mode() {
    // Music generation still has scripted coverage, but no shipping provider
    // proxy in v1. Never restore the former direct-provider mode.
    QSettings().remove(key("mode"));
    return Mode::Assistant;
}

void setMode(Mode mode) {
    Q_UNUSED(mode);
    QSettings().remove(key("mode"));
}

QString musicUrl() {
    return QSettings()
        .value(key("music.url"),
               QStringLiteral("https://api.minimax.io/v1/music_generation"))
        .toString();
}

void setMusicUrl(const QString& url) {
    QSettings().setValue(key("music.url"), url);
}

QString musicModel() {
    return QSettings().value(key("music.model"), QStringLiteral("music-3.0"))
        .toString();
}

void setMusicModel(const QString& model) {
    QSettings().setValue(key("music.model"), model);
}

bool hasMusicApiKey() {
    return !QSettings().value(key("music.apiKey")).toString().isEmpty();
}

QString musicApiKey() {
    return QSettings().value(key("music.apiKey")).toString();
}

void setMusicApiKey(const QString& value) {
    QSettings settings;
    if (value.isEmpty()) settings.remove(key("music.apiKey"));
    else settings.setValue(key("music.apiKey"), value);
}

QString musicFormat() {
    const QString stored =
        QSettings().value(key("music.format"), QStringLiteral("mp3")).toString();
    return stored == QLatin1String("wav") ? stored : QStringLiteral("mp3");
}

void setMusicFormat(const QString& format) {
    QSettings().setValue(key("music.format"), format);
}

int musicSampleRate() {
    return std::clamp(QSettings().value(key("music.sampleRate"), 44100).toInt(),
                      8000, 192000);
}

void setMusicSampleRate(int rate) {
    QSettings().setValue(key("music.sampleRate"), std::clamp(rate, 8000, 192000));
}

int musicBitrate() {
    return std::clamp(QSettings().value(key("music.bitrate"), 256000).toInt(),
                      32000, 320000);
}

void setMusicBitrate(int bitrate) {
    QSettings().setValue(key("music.bitrate"), std::clamp(bitrate, 32000, 320000));
}

QString musicFolder() {
    const QString stored = QSettings().value(key("music.folder")).toString();
    if (!stored.isEmpty()) return stored;
    // Inside Music rather than a hidden application folder: the file browser
    // starts there too, so a generated take is one click away from being used
    // again somewhere else.
    const QString music =
        QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    const QString base = music.isEmpty()
                             ? QStandardPaths::writableLocation(
                                   QStandardPaths::AppLocalDataLocation)
                             : music;
    return base + QStringLiteral("/VLT Studio Pro Generated");
}

void setMusicFolder(const QString& folder) {
    QSettings().setValue(key("music.folder"), folder);
}

int musicTimeoutSeconds() {
    return std::clamp(QSettings().value(key("music.timeout"), 300).toInt(), 10,
                      3600);
}

void setMusicTimeoutSeconds(int seconds) {
    QSettings().setValue(key("music.timeout"), std::clamp(seconds, 10, 3600));
}

bool musicInstrumental() {
    return QSettings().value(key("music.instrumental"), false).toBool();
}

void setMusicInstrumental(bool instrumental) {
    QSettings().setValue(key("music.instrumental"), instrumental);
}

int maxIterations() {
    return std::clamp(QSettings().value(key("maxIterations"), 24).toInt(), 1, 200);
}

void setMaxIterations(int iterations) {
    QSettings().setValue(key("maxIterations"), std::clamp(iterations, 1, 200));
}

QStringList savedPrompts() {
    return QSettings().value(key("savedPrompts")).toStringList();
}

void setSavedPrompts(const QStringList& prompts) {
    QSettings().setValue(key("savedPrompts"), prompts);
}

int historyLimit() {
    return std::clamp(QSettings().value(key("historyLimit"), 12).toInt(), 0, 200);
}

void setHistoryLimit(int turns) {
    QSettings().setValue(key("historyLimit"), std::clamp(turns, 0, 200));
}

bool streaming() { return QSettings().value(key("streaming"), true).toBool(); }

void setStreaming(bool on) { QSettings().setValue(key("streaming"), on); }

bool visible() { return QSettings().value(key("visible"), false).toBool(); }

void setVisible(bool visible) { QSettings().setValue(key("visible"), visible); }

int width() {
    return std::clamp(QSettings().value(key("width"), 320).toInt(), 240, 720);
}

void setWidth(int width) {
    QSettings().setValue(key("width"), std::clamp(width, 240, 720));
}

} // namespace ui::aiprefs
