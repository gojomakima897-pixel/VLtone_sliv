#pragma once

#include "Theme.hpp"

#include <QDialog>
#include <QHash>
#include <QString>

namespace daw { class EngineController; }

class RecordingSettingsPage;
class ShortcutManager;
class QTabWidget;
class QListWidget;
class QKeySequenceEdit;
class QPushButton;
class QLineEdit;
class QShowEvent;

/// The unified, non-modal settings window: one place for Audio, Themes and
/// Keyboard Shortcuts, titled with the application name. Replaces the standalone
/// Audio Settings dialog and the Settings ▸ Theme submenu.
class SettingsWindow : public QDialog {
    Q_OBJECT
public:
    SettingsWindow(daw::EngineController* controller, ShortcutManager* shortcuts,
                   QWidget* parent = nullptr);

    /// The tabs, by name. They used to be addressed by bare number, and the
    /// comment saying which was which had already gone stale twice.
    enum Tab {
        kAudioTab = 0,
        kTransportTab,
        kRecordingTab,
        kContextPanelTab,
        kBrowserTab,
        kAiTab,
        kAccountTab,
        kRecoveryTab,
        kThemesTab,
        kThemeEditorTab,
        kShortcutsTab,
    };

    /// Bring a specific tab to the front.
    void showTab(int index);

    /// Re-read the recording preferences. The mode also changes from the
    /// transport's Layers button, and an open window has to follow.
    void reloadRecordingPage();

signals:
    /// A context-panel profile or its transparency setting changed.
    void contextPanelSettingsChanged();
    /// The browser's folders, side or preview options changed.
    void browserSettingsChanged();
    /// The assistant's provider, key, model or step limit changed.
    void aiSettingsChanged();
    /// MainWindow owns the unsaved-project decision before credentials vanish.
    void accountLogoutRequested();
    /// The global recording mode changed on the Recording tab.
    void recordModeChanged();
    /// How selected tracks are tinted changed on the Themes tab.
    void selectionTintChanged();

protected:
    void showEvent(QShowEvent* event) override;

private:
    /// Keep the dialog inside the current monitor's usable area. Every tab is
    /// scrollable, so shrinking the shell never hides a setting.
    void constrainToScreen();
    QWidget* buildThemesTab();
    QWidget* buildThemeEditorTab();
    QWidget* buildShortcutsTab();
    void refreshShortcutEditors();
    /// Push the working palette to the app (live), persisting it as "custom".
    void applyEditTheme();
    /// Repaint every colour swatch from the working palette.
    void refreshSwatches();

    daw::EngineController* m_controller = nullptr;
    ShortcutManager* m_shortcuts = nullptr;
    QTabWidget* m_tabs = nullptr;
    RecordingSettingsPage* m_recordingPage = nullptr;
    QListWidget* m_themeList = nullptr;
    QHash<QString, QKeySequenceEdit*> m_editors;   // command id → editor

    Theme m_editTheme;                             // the working custom palette
    QLineEdit* m_themeNameEdit = nullptr;
    QHash<QString, QPushButton*> m_swatches;       // field key → swatch button
};
