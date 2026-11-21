#pragma once

#include "Host/PluginInstance.hpp"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QWidget>

namespace daw { class EngineController; }

class QTimer;
class QVBoxLayout;
class QComboBox;
class QLabel;
class QToolButton;
namespace ui { class IconButton; }

/// A window holding one plugin's own editor.
///
/// The plugin's view is parented into a native child widget: `QWidget::winId()`
/// is an `NSView*` on macOS and an `HWND` on Windows, which is exactly what
/// CLAP, VST3 and AU each want — so no per-platform source file is needed here.
///
/// The slot is addressed by (channelId, insertId) and **re-resolved on every
/// use**, never cached as a pointer: undo can rewrite the document, and a
/// rebuild can replace the live instance, underneath an open window. Same
/// discipline as PianoRollWindow.
class PluginEditorWindow : public QWidget, public daw::plugins::PluginEditorHost {
    Q_OBJECT
public:
    PluginEditorWindow(daw::EngineController* controller, QString channelId,
                       QString insertId, QWidget* parent = nullptr);
    ~PluginEditorWindow() override;

    const QString& channelId() const { return m_channelId; }
    const QString& insertId() const { return m_insertId; }
    const QString& pluginUid() const { return m_pluginUid; }

    /// False when the plugin has no GUI or refused to open one; the caller
    /// should fall back to the generic parameter panel.
    bool isEmbedded() const { return m_embedded; }

    /// Let go of the plugin's view *now*, because the plugin itself is about to
    /// be destroyed — a Replace, a Remove, an undo, a project being closed.
    ///
    /// Without this the window would keep the plugin's own NSView/HWND embedded
    /// in it past the plugin's lifetime and hand it back on a later close, by
    /// which time the slot holds a different plugin entirely.
    void detachFromPlugin();

    /// Headless checks and screenshots only. The dock's toggle is offered only
    /// where a plugin has a native view of its own, which no offscreen run can
    /// produce — so these open it, pump it, and read back what it is showing.
    void setParameterDockVisibleForTest(bool visible);
    void pollForTest();
    QStringList parameterDockOrderForTest() const;
    QString parameterDockActiveForTest() const { return m_dockActive; }

    // ── PluginEditorHost ──
    void onEditorResized(std::uint32_t width, std::uint32_t height) noexcept override;
    void onEditorClosed() noexcept override;
    double contentScaleFactor() const noexcept override;

signals:
    /// The window is going away; the registry drops its entry.
    void closing(const QString& channelId, const QString& insertId);
    void nestedPluginEditorRequested(const QString& channelId,
                                     const QString& insertId);
    void projectEdited();
    /// A parameter knob requested automation by gesture or context menu. The window itself
    /// cannot make a lane — that is the document's business — so it says which
    /// parameter and lets the main window do the work.
    void automationRequested(const QString& channelId, const QString& insertId,
                             const QString& parameterId);

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    /// The live plugin behind the slot, or null if it went away.
    daw::plugins::PluginInstance* instance() const;
    /// Build the fallback panel of sliders for a plugin with no GUI.
    void buildGenericEditor();
    void refreshGenericEditor();
    void buildWrapper();
    void refreshWrapper();
    void rebuildEditorContent();
    void clearEditorContent();
    /// The column of parameter knobs that can sit *beside* a plugin's own view.
    ///
    /// A third-party GUI is a foreign native window: its mouse events never
    /// reach Qt, so host gestures and host context menus cannot be attached to
    /// the plugin's own controls. This panel is ours, carries the same
    /// parameters, and gives both creation paths somewhere to live.
    QWidget* buildParameterDock();
    void refreshParameterDock();
    /// Re-place the cells in the grid from `m_dockCells`, which is the order
    /// they are shown in — the promotion of a parameter the plugin is moving is
    /// nothing more than that list being reordered.
    void layOutParameterDock();
    /// The plugin's own words for a value — "440 Hz", "2:1" — or the number.
    QString parameterText(const QString& parameterId, double plain) const;
    void setParameterDockVisible(bool visible);
    /// How much width the dock is taking right now — nothing when it is
    /// hidden. Every place the window's width is computed back from the
    /// plugin's own view has to add this, or opening the dock crops the plugin.
    int dockWidth() const;
    void applyTheme();

    daw::EngineController* m_controller = nullptr;
    QString m_channelId;
    QString m_insertId;
    QString m_pluginUid;

    QWidget* m_container = nullptr;      // native, holds the plugin's view
    QVBoxLayout* m_layout = nullptr;
    QWidget* m_wrapper = nullptr;
    ui::IconButton* m_power = nullptr;
    QLabel* m_pluginName = nullptr;
    QLabel* m_pluginFormat = nullptr;
    QComboBox* m_channelMode = nullptr;
    QToolButton* m_leftChannel = nullptr;
    QToolButton* m_rightChannel = nullptr;
    QComboBox* m_sidechain = nullptr;
    QWidget* m_generic = nullptr;        // fallback panel, when there is no GUI
    /// The row the editor content lives in: the plugin's view (or the fallback
    /// panel) and, beside it, our own parameter dock.
    QWidget* m_content = nullptr;
    class QHBoxLayout* m_contentRow = nullptr;
    QWidget* m_dock = nullptr;
    QToolButton* m_dockToggle = nullptr;
    class QGridLayout* m_dockGrid = nullptr;
    /// The dock's cells in display order. Reordered when the plugin moves a
    /// parameter, so the one being turned rises to the top.
    QList<QWidget*> m_dockCells;
    /// Which parameter the plugin last moved by itself.
    QString m_dockActive;
    QTimer* m_poll = nullptr;            // refreshes wrapper + fallback panel
    bool m_refreshingWrapper = false;
    bool m_embedded = false;
    /// The instance the view was opened on. Compared, never dereferenced after
    /// the plugin retires: `instance()` re-reads the slot, and after a Replace
    /// that is a *different* plugin, which must not be told to close an editor
    /// it never opened.
    daw::plugins::PluginInstance* m_openedOn = nullptr;
    /// True while the window is resizing itself on the plugin's instruction, so
    /// the resulting resizeEvent is not echoed back as a host-side resize.
    bool m_applyingPluginSize = false;
};
