#include "AiSettingsPage.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProcessEnvironment>
#include <QSpinBox>
#include <QVBoxLayout>

using ui::aiprefs::Provider;

AiSettingsPage::AiSettingsPage(QWidget* parent) : QWidget(parent) {
    auto* column = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    column->addLayout(form);

    m_provider = new QComboBox(this);
    m_provider->addItem(tr("Claude (Anthropic)"),
                        ui::aiprefs::providerId(Provider::Anthropic));
    m_provider->addItem(tr("GPT (OpenAI)"),
                        ui::aiprefs::providerId(Provider::OpenAi));
    m_provider->setCurrentIndex(
        ui::aiprefs::provider() == Provider::OpenAi ? 1 : 0);
    connect(m_provider, &QComboBox::currentIndexChanged, this, [this] {
        ui::aiprefs::setProvider(currentProvider());
        loadProvider();
        emit changed();
    });
    form->addRow(tr("Model provider"), m_provider);

    // A list, not a field: one key and one URL serve every model a provider
    // offers, and which of them suits the moment is decided while working. The
    // assistant's own header picks between whatever is written here.
    m_model = new QPlainTextEdit(this);
    m_model->setFixedHeight(72);
    m_model->setToolTip(
        tr("One model per line. The assistant's header lets you switch between "
           "them; the first line is used until you pick another."));
    connect(m_model, &QPlainTextEdit::textChanged, this, [this] {
        if (m_loading) return;
        m_modelsDirty = true;
    });
    form->addRow(tr("Models"), m_model);

    m_model->installEventFilter(this);

    m_maxIterations = new QSpinBox(this);
    m_maxIterations->setRange(1, 200);
    m_maxIterations->setValue(ui::aiprefs::maxIterations());
    m_maxIterations->setToolTip(
        tr("How many rounds of tool calls one request may take before it is "
           "cut off. The guard against a request that keeps going."));
    connect(m_maxIterations, &QSpinBox::valueChanged, this, [this](int value) {
        if (m_loading) return;
        ui::aiprefs::setMaxIterations(value);
        emit changed();
    });
    form->addRow(tr("Steps per request"), m_maxIterations);

    m_historyLimit = new QSpinBox(this);
    m_historyLimit->setRange(0, 200);
    m_historyLimit->setSpecialValueText(tr("the whole conversation"));
    m_historyLimit->setValue(ui::aiprefs::historyLimit());
    m_historyLimit->setToolTip(
        tr("How many past requests each new one carries with it. The chat "
           "still shows everything; this is only what is paid for. Lower is "
           "cheaper, higher lets the assistant remember more of what you "
           "asked earlier."));
    connect(m_historyLimit, &QSpinBox::valueChanged, this, [this](int value) {
        if (m_loading) return;
        ui::aiprefs::setHistoryLimit(value);
        emit changed();
    });
    form->addRow(tr("Requests remembered"), m_historyLimit);

    m_streaming = new QCheckBox(tr("Show the answer as it is written"), this);
    m_streaming->setChecked(ui::aiprefs::streaming());
    m_streaming->setToolTip(
        tr("Off waits for the whole reply before showing anything. Some "
           "OpenAI-compatible servers do not stream."));
    connect(m_streaming, &QCheckBox::toggled, this, [this](bool on) {
        if (m_loading) return;
        ui::aiprefs::setStreaming(on);
        emit changed();
    });
    form->addRow(QString(), m_streaming);

    m_keyNote = new QLabel(this);
    m_keyNote->setWordWrap(true);
    column->addWidget(m_keyNote);

    auto* warning = new QLabel(
        tr("The assistant edits the project directly. Each request lands on "
           "the undo stack as one entry, so Ctrl+Z takes back everything a "
           "request did."),
        this);
    warning->setWordWrap(true);
    column->addWidget(warning);

    // Music generation is built and working (`buildMusicGroup`, `loadMusic`,
    // the panel's second mode) but is not offered yet — the user asked for it
    // to be out of the way until it is ready to show. Nothing about it was
    // deleted; it is one line away from coming back.
    column->addStretch(1);

    loadProvider();
}

QWidget* AiSettingsPage::buildMusicGroup() {
    auto* group = new QGroupBox(tr("Music generation"), this);
    auto* column = new QVBoxLayout(group);
    auto* form = new QFormLayout;
    column->addLayout(form);

    m_musicUrl = new QLineEdit(group);
    m_musicUrl->setToolTip(
        tr("The whole endpoint, not a base address. MiniMax's own is "
           "https://api.minimax.io/v1/music_generation; point this at your own "
           "server if the model runs there."));
    connect(m_musicUrl, &QLineEdit::editingFinished, this, [this] {
        if (m_loading) return;
        ui::aiprefs::setMusicUrl(m_musicUrl->text().trimmed());
        loadMusic();
        emit changed();
    });
    form->addRow(tr("Server URL"), m_musicUrl);

    m_musicModel = new QLineEdit(group);
    m_musicModel->setPlaceholderText(QStringLiteral("music-3.0"));
    connect(m_musicModel, &QLineEdit::editingFinished, this, [this] {
        if (m_loading) return;
        ui::aiprefs::setMusicModel(m_musicModel->text().trimmed());
        emit changed();
    });
    form->addRow(tr("Model"), m_musicModel);

    m_musicKey = new QLineEdit(group);
    m_musicKey->setEchoMode(QLineEdit::Password);
    // Never read back into the field, for the same reason as the chat key: the
    // page is built whenever this window opens, on any tab.
    connect(m_musicKey, &QLineEdit::editingFinished, this, [this] {
        if (m_loading) return;
        const QString typed = m_musicKey->text().trimmed();
        if (typed.isEmpty()) return;
        ui::aiprefs::setMusicApiKey(typed);
        m_musicKey->clear();
        loadMusic();
        emit changed();
    });
    auto* keyRow = new QHBoxLayout();
    keyRow->setSpacing(8);
    keyRow->addWidget(m_musicKey, 1);
    m_forgetMusicKey = new QPushButton(tr("Forget"), group);
    connect(m_forgetMusicKey, &QPushButton::clicked, this, [this] {
        ui::aiprefs::setMusicApiKey(QString());
        m_musicKey->clear();
        loadMusic();
        emit changed();
    });
    keyRow->addWidget(m_forgetMusicKey);
    form->addRow(tr("API key"), keyRow);

    m_musicFormat = new QComboBox(group);
    m_musicFormat->addItem(QStringLiteral("mp3"), QStringLiteral("mp3"));
    m_musicFormat->addItem(QStringLiteral("wav"), QStringLiteral("wav"));
    connect(m_musicFormat, &QComboBox::currentIndexChanged, this, [this] {
        if (m_loading) return;
        ui::aiprefs::setMusicFormat(m_musicFormat->currentData().toString());
        emit changed();
    });
    form->addRow(tr("Format"), m_musicFormat);

    m_musicSampleRate = new QSpinBox(group);
    m_musicSampleRate->setRange(8000, 192000);
    m_musicSampleRate->setSingleStep(100);
    m_musicSampleRate->setSuffix(tr(" Hz"));
    connect(m_musicSampleRate, &QSpinBox::valueChanged, this, [this](int value) {
        if (m_loading) return;
        ui::aiprefs::setMusicSampleRate(value);
        emit changed();
    });
    form->addRow(tr("Sample rate"), m_musicSampleRate);

    m_musicBitrate = new QSpinBox(group);
    m_musicBitrate->setRange(32000, 320000);
    m_musicBitrate->setSingleStep(1000);
    m_musicBitrate->setToolTip(tr("Only used when the format is mp3."));
    connect(m_musicBitrate, &QSpinBox::valueChanged, this, [this](int value) {
        if (m_loading) return;
        ui::aiprefs::setMusicBitrate(value);
        emit changed();
    });
    form->addRow(tr("Bitrate"), m_musicBitrate);

    m_musicFolder = new QLineEdit(group);
    m_musicFolder->setToolTip(
        tr("Generated audio is written here and then imported. Keeping it "
           "inside a folder the browser shows means a take can be used again "
           "later."));
    connect(m_musicFolder, &QLineEdit::editingFinished, this, [this] {
        if (m_loading) return;
        ui::aiprefs::setMusicFolder(m_musicFolder->text().trimmed());
        loadMusic();
        emit changed();
    });
    auto* folderRow = new QHBoxLayout();
    folderRow->setSpacing(8);
    folderRow->addWidget(m_musicFolder, 1);
    auto* browse = new QPushButton(tr("Browse…"), group);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString picked = QFileDialog::getExistingDirectory(
            this, tr("Where to save generated audio"),
            ui::aiprefs::musicFolder());
        if (picked.isEmpty()) return;
        ui::aiprefs::setMusicFolder(picked);
        loadMusic();
        emit changed();
    });
    folderRow->addWidget(browse);
    form->addRow(tr("Save audio to"), folderRow);

    m_musicTimeout = new QSpinBox(group);
    m_musicTimeout->setRange(10, 3600);
    m_musicTimeout->setSuffix(tr(" s"));
    m_musicTimeout->setToolTip(
        tr("How long one generation may take before it is given up on. A "
           "four-minute song is not fast."));
    connect(m_musicTimeout, &QSpinBox::valueChanged, this, [this](int value) {
        if (m_loading) return;
        ui::aiprefs::setMusicTimeoutSeconds(value);
        emit changed();
    });
    form->addRow(tr("Timeout"), m_musicTimeout);

    m_musicNote = new QLabel(group);
    m_musicNote->setWordWrap(true);
    column->addWidget(m_musicNote);

    auto* blurb = new QLabel(
        tr("The music mode sends the project's tempo, key, time signature and "
           "track names with the request, and puts what comes back on a new "
           "audio track at the playhead — one Ctrl+Z takes it away."),
        group);
    blurb->setWordWrap(true);
    column->addWidget(blurb);
    return group;
}

void AiSettingsPage::loadMusic() {
    m_loading = true;
    m_musicUrl->setText(ui::aiprefs::musicUrl());
    m_musicModel->setText(ui::aiprefs::musicModel());
    m_musicFolder->setText(ui::aiprefs::musicFolder());
    m_musicSampleRate->setValue(ui::aiprefs::musicSampleRate());
    m_musicBitrate->setValue(ui::aiprefs::musicBitrate());
    m_musicTimeout->setValue(ui::aiprefs::musicTimeoutSeconds());
    m_musicFormat->setCurrentIndex(
        m_musicFormat->findData(ui::aiprefs::musicFormat()));

    m_musicKey->clear();
    const bool stored = ui::aiprefs::hasMusicApiKey();
    m_musicKey->setPlaceholderText(
        stored ? tr("A key is stored — type to replace it")
               : tr("Optional — a server of your own may need none"));
    m_forgetMusicKey->setEnabled(stored);
    m_loading = false;

    const QString fromEnv = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("MINIMAX_API_KEY"));
    m_musicNote->setText(
        fromEnv.isEmpty()
            ? tr("A key typed here is stored unencrypted with the other "
                 "settings. Set MINIMAX_API_KEY in the environment instead to "
                 "keep it off disk. Leave it empty for a server that needs no "
                 "authorization.")
            : tr("MINIMAX_API_KEY is set in the environment and is used "
                 "instead of the key above."));
}

void AiSettingsPage::commitModels() {
    if (!m_modelsDirty || m_loading) return;
    m_modelsDirty = false;
    const Provider provider = currentProvider();
    const QStringList typed =
        m_model->toPlainText().split('\n', Qt::SkipEmptyParts);
    ui::aiprefs::setModels(provider, typed);
    // The active model follows the list: it stays put while it is still on it,
    // and otherwise becomes the first line — the one the user put at the top.
    const QStringList kept = ui::aiprefs::models(provider);
    const QString active = ui::aiprefs::model(provider);
    if (!kept.contains(active))
        ui::aiprefs::setModel(provider, kept.isEmpty() ? QString() : kept.front());
    emit changed();
}

bool AiSettingsPage::eventFilter(QObject* watched, QEvent* event) {
    // A multi-line field has no editingFinished; leaving it is what "done" is.
    if (watched == m_model && event->type() == QEvent::FocusOut) commitModels();
    return QWidget::eventFilter(watched, event);
}

Provider AiSettingsPage::currentProvider() const {
    return ui::aiprefs::providerFromId(m_provider->currentData().toString());
}

void AiSettingsPage::loadProvider() {
    const Provider provider = currentProvider();
    m_loading = true;
    m_model->setPlainText(ui::aiprefs::models(provider).join('\n'));
    m_modelsDirty = false;
    m_model->setPlaceholderText(provider == Provider::OpenAi
                                    ? QStringLiteral("gpt-4o\ngpt-4o-mini")
                                    : QStringLiteral("claude-sonnet-4-5\nclaude-opus-4-1"));
    m_loading = false;
    m_keyNote->setText(
        tr("Provider credentials are held by the VLT account service. The app "
           "never receives or stores OpenAI or Anthropic API keys; usage is "
           "charged against the Demo monthly allowance shown on Account."));
}
