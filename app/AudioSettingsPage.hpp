#pragma once

#include "Device/AudioDeviceManager.hpp"

#include <QWidget>

#include <vector>

namespace daw { class EngineController; }

class QComboBox;
class QLabel;
class QPushButton;

/// The audio-device settings form (output/input device, sample rate, buffer
/// size) with its own Apply button. Extracted from the old AudioSettingsDialog
/// so it can live as a tab in the unified settings window.
class AudioSettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit AudioSettingsPage(daw::EngineController* controller,
                               QWidget* parent = nullptr);

    /// Push the current selections into the engine.
    void apply();

private slots:
    /// Ask the newly selected output device what it supports and rebuild the
    /// rate and buffer lists from the answer. Done on selection rather than
    /// during enumeration: probing every device costs seconds, and on Windows
    /// it would load every ASIO driver on the machine.
    void refreshDeviceCapabilities();

private:
    daw::EngineController* m_controller = nullptr;
    /// Enumerated once. Rebuilding the list costs a PortAudio sweep, which is
    /// far too slow to redo every time the combo moves.
    std::vector<audio::DeviceInfo> m_outputs;
    QComboBox* m_outputCombo = nullptr;
    QComboBox* m_inputCombo = nullptr;
    QComboBox* m_sampleRateCombo = nullptr;
    QComboBox* m_bufferCombo = nullptr;
    QLabel* m_bufferNote = nullptr;
    QPushButton* m_controlPanel = nullptr;
    QLabel* m_deviceNote = nullptr;
};
