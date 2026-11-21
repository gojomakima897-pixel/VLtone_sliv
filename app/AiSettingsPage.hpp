#pragma once

#include "AiPrefs.hpp"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

/// Settings ▸ AI: provider/model preferences and agent limits. Credentials are
/// owned by the account service and never appear on this page.
///
/// Writes through on every change, like the other settings pages — there is no
/// OK button anywhere in this window. Everything goes through `ui::aiprefs`, so
/// the panel and this page cannot disagree about where a value lives. The
/// per-provider fields are reloaded when the provider changes.
class AiSettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit AiSettingsPage(QWidget* parent = nullptr);

signals:
    /// Anything on this page changed; the shell reloads the panel.
    void changed();

protected:
    bool eventFilter(QObject* watched, class QEvent* event) override;

private:
    /// Fill the per-provider fields from the settings for `provider`.
    void loadProvider();
    /// The music endpoint's own group. Its own key, its own model, its own
    /// server: none of the chat settings apply to it.
    /// Write the model list back and keep the active model on it. A
    /// multi-line field has no "editing finished", so this runs on focus-out.
    void commitModels();
    bool m_modelsDirty = false;

    QWidget* buildMusicGroup();
    void loadMusic();
    ui::aiprefs::Provider currentProvider() const;

    QComboBox* m_provider = nullptr;
    class QPlainTextEdit* m_model = nullptr;
    QSpinBox* m_maxIterations = nullptr;
    QSpinBox* m_historyLimit = nullptr;
    QCheckBox* m_streaming = nullptr;
    QLabel* m_keyNote = nullptr;

    QLineEdit* m_musicUrl = nullptr;
    QLineEdit* m_musicModel = nullptr;
    QLineEdit* m_musicKey = nullptr;
    QPushButton* m_forgetMusicKey = nullptr;
    QComboBox* m_musicFormat = nullptr;
    QSpinBox* m_musicSampleRate = nullptr;
    QSpinBox* m_musicBitrate = nullptr;
    QLineEdit* m_musicFolder = nullptr;
    QSpinBox* m_musicTimeout = nullptr;
    QLabel* m_musicNote = nullptr;
    /// Set while `loadProvider` is filling the fields, so writing them back does
    /// not overwrite the provider that was just switched away from.
    bool m_loading = false;
};
