#include "AudioSettingsPage.hpp"
#include "EngineController.hpp"
#include "Device/AudioDeviceManager.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

AudioSettingsPage::AudioSettingsPage(daw::EngineController* controller,
                                     QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    auto* form = new QFormLayout;

    // The host API belongs in the label. On Windows one interface appears once
    // per driver family under the same name — MME, WASAPI, ASIO — and which one
    // is chosen decides whether the user gets 5 ms or 50 ms of latency. A list
    // of three identical entries makes that choice unmakeable.
    auto label = [](const audio::DeviceInfo& device) {
        const QString name = QString::fromStdString(device.name);
        return device.hostApi.empty()
                   ? name
                   : QStringLiteral("%1 — %2").arg(
                         name, QString::fromStdString(device.hostApi));
    };

    m_outputs = m_controller->enumerateOutputDevices();
    m_outputCombo = new QComboBox;
    const std::string curOut = m_controller->currentOutputDeviceUid();
    for (const auto& d : m_outputs) {
        m_outputCombo->addItem(label(d), QString::fromStdString(d.uid));
        if (d.uid == curOut)
            m_outputCombo->setCurrentIndex(m_outputCombo->count() - 1);
    }
    form->addRow(tr("Output device"), m_outputCombo);

    m_inputCombo = new QComboBox;
    m_inputCombo->addItem(tr("None"), QString());
    const std::string curIn = m_controller->currentInputDeviceUid();
    for (const auto& d : m_controller->enumerateInputDevices()) {
        m_inputCombo->addItem(label(d), QString::fromStdString(d.uid));
        if (d.uid == curIn)
            m_inputCombo->setCurrentIndex(m_inputCombo->count() - 1);
    }
    form->addRow(tr("Input device"), m_inputCombo);

    // Both lists are filled from the selected device by
    // `refreshDeviceCapabilities`, called once below and again on every change.
    m_sampleRateCombo = new QComboBox;
    form->addRow(tr("Sample rate"), m_sampleRateCombo);
    m_bufferCombo = new QComboBox;
    form->addRow(tr("Buffer size"), m_bufferCombo);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);

    m_deviceNote = new QLabel;
    m_deviceNote->setWordWrap(true);
    m_deviceNote->setVisible(false);
    layout->addWidget(m_deviceNote);

    m_bufferNote = new QLabel;
    m_bufferNote->setWordWrap(true);
    m_bufferNote->setStyleSheet("color:#c08040;");
    m_bufferNote->setVisible(false);
    layout->addWidget(m_bufferNote);
    if (!m_controller->isDeviceOpen()) {
        auto* note = new QLabel(
            tr("Audio device is offline; changes apply when it opens."));
        note->setStyleSheet("color:#c08040;");
        layout->addWidget(note);
    }
    layout->addStretch(1);

    auto* applyButton = new QPushButton(tr("Apply"));
    connect(applyButton, &QPushButton::clicked, this, &AudioSettingsPage::apply);

    // An ASIO driver owns its sample rate and buffer size; its own panel is
    // where they are really set, and a host that hides it leaves the user
    // unable to configure their interface.
    m_controlPanel = new QPushButton(tr("Control Panel…"));
    m_controlPanel->setVisible(false);
    connect(m_controlPanel, &QPushButton::clicked, this, [this] {
        const QString uid = m_outputCombo->currentData().toString();
        if (uid.isEmpty()) return;
        const auto result =
            m_controller->showDeviceControlPanel(uid.toStdString());
        if (!result) {
            QMessageBox::warning(this, tr("Control panel"),
                                 QString::fromStdString(result.message()));
            return;
        }
        // The driver may well have changed the rate or the buffer size while
        // its window was open, so ask it again rather than trusting the form.
        refreshDeviceCapabilities();
    });

    auto* buttonRow = new QHBoxLayout;
    buttonRow->addWidget(m_controlPanel);
    buttonRow->addStretch(1);
    buttonRow->addWidget(applyButton);
    layout->addLayout(buttonRow);

    connect(m_outputCombo, &QComboBox::currentIndexChanged, this,
            &AudioSettingsPage::refreshDeviceCapabilities);
    refreshDeviceCapabilities();
}

void AudioSettingsPage::refreshDeviceCapabilities() {
    const QString uid = m_outputCombo->currentData().toString();

    audio::DeviceInfo device;
    bool asio = false;
    for (const auto& candidate : m_outputs) {
        if (candidate.uid != uid.toStdString()) continue;
        device = candidate;
        asio = candidate.isAsio;
        break;
    }
    if (!uid.isEmpty()) {
        // Best-effort: a device that has gone away, or an ASIO driver already
        // held by another application, simply leaves the candidate lists in
        // place rather than emptying the form.
        (void)m_controller->probeDevice(uid.toStdString(), /*wantInput=*/false,
                                        device);
    }

    const auto keepRate = static_cast<int>(m_controller->sampleRate());
    const auto keepBuffer = static_cast<int>(m_controller->bufferSizeFrames());

    const QSignalBlocker rateBlocker(m_sampleRateCombo);
    m_sampleRateCombo->clear();
    for (double rate : device.sampleRates) {
        m_sampleRateCombo->addItem(QString::number(int(rate)), int(rate));
    }
    if (m_sampleRateCombo->count() == 0) {
        m_sampleRateCombo->addItem(QString::number(keepRate), keepRate);
    }
    if (const int at = m_sampleRateCombo->findData(keepRate); at >= 0) {
        m_sampleRateCombo->setCurrentIndex(at);
    }

    const QSignalBlocker bufferBlocker(m_bufferCombo);
    m_bufferCombo->clear();
    for (uint32_t size : device.bufferSizes) {
        m_bufferCombo->addItem(QString::number(size), int(size));
    }
    if (m_bufferCombo->count() == 0) {
        m_bufferCombo->addItem(QString::number(keepBuffer), keepBuffer);
    }
    if (const int at = m_bufferCombo->findData(keepBuffer); at >= 0) {
        m_bufferCombo->setCurrentIndex(at);
    }

    m_controlPanel->setVisible(device.hasControlPanel);
    if (asio) {
        m_deviceNote->setText(
            tr("ASIO: the driver owns the sample rate and buffer size, and "
               "usually allows one application at a time. Sizes below are the "
               "ones this driver reports."));
        m_deviceNote->setStyleSheet("color:#8090a0;");
        m_deviceNote->setVisible(true);
    } else {
        m_deviceNote->setVisible(false);
    }
}

void AudioSettingsPage::apply() {
    const QString outUid = m_outputCombo->currentData().toString();
    if (!outUid.isEmpty()) m_controller->setOutputDevice(outUid.toStdString());

    const QString inUid = m_inputCombo->currentData().toString();
    m_controller->setInputDevice(inUid.toStdString());

    m_controller->setSampleRateHz(m_sampleRateCombo->currentData().toInt());

    const auto wanted = static_cast<uint32_t>(m_bufferCombo->currentData().toInt());
    const auto result = m_controller->setBufferSizeFrames(wanted);
    const auto settled = m_controller->bufferSizeFrames();
    if (m_bufferNote) {
        const bool refused = !result || settled != wanted;
        m_bufferNote->setVisible(refused);
        if (refused) {
            m_bufferNote->setText(
                tr("This device will not run at %1 frames; it is back at %2.")
                    .arg(wanted)
                    .arg(settled));
        }
    }
    // Whatever the device settled on is what the form should say afterwards.
    if (settled != wanted) {
        const int index = m_bufferCombo->findData(static_cast<int>(settled));
        if (index >= 0) m_bufferCombo->setCurrentIndex(index);
    }
}
