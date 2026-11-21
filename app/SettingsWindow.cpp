#include "SettingsWindow.hpp"
#include "AiSettingsPage.hpp"
#include "AccountSettingsPage.hpp"
#include "BrowserSettingsPage.hpp"
#include "RecoverySettingsPage.hpp"
#include "AudioSettingsPage.hpp"
#include "ContextPanelPage.hpp"
#include "ShortcutManager.hpp"
#include "Theme.hpp"
#include "UiConstants.hpp"
#include "RecordingSettingsPage.hpp"
#include "TransportSettingsPage.hpp"

#include <QApplication>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScreen>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

namespace {
// The editable colours of a theme, paired with a pointer-to-member so the
// editor can read and write each one generically.
struct ColorField {
    const char* key;
    const char* label;
    QColor Theme::* member;
};
const std::vector<ColorField>& colorFields() {
    static const std::vector<ColorField> fields = {
        {"background", "Background", &Theme::background},
        {"surface", "Surface", &Theme::surface},
        {"surfaceElevated", "Surface (elevated)", &Theme::surfaceElevated},
        {"headerBackground", "Header", &Theme::headerBackground},
        {"transportBackground", "Transport", &Theme::transportBackground},
        {"toolbarBackground", "Toolbar", &Theme::toolbarBackground},
        {"textPrimary", "Text", &Theme::textPrimary},
        {"textSecondary", "Text (secondary)", &Theme::textSecondary},
        {"accent", "Accent", &Theme::accent},
        {"accentHighlight", "Accent (highlight)", &Theme::accentHighlight},
        {"waveform", "Waveform", &Theme::waveform},
        {"cursor", "Playhead", &Theme::cursor},
        {"gridLine", "Grid line", &Theme::gridLine},
        {"gridLineStrong", "Grid line (strong)", &Theme::gridLineStrong},
        {"selection", "Selection", &Theme::selection},
    };
    return fields;
}

/// A settings page owns its natural size, while the dialog owns the viewport.
/// This breaks the size-hint chain that used to let a long page push the whole
/// window beyond the top and bottom of a smaller display.
QScrollArea* scrollablePage(QWidget* page) {
    page->setMinimumSize(0, 0);
    page->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    // Explanatory copy is the usual source of an enormous page size hint.
    // Let it gain height instead of forcing the whole dialog (or a horizontal
    // scrollbar) wider than the screen. Compact form labels stay single-line.
    for (QLabel* label : page->findChildren<QLabel*>()) {
        if (label->text().size() < 56) continue;
        label->setWordWrap(true);
        label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }
    auto* scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    scroll->setMinimumSize(0, 0);
    scroll->setWidget(page);
    return scroll;
}
} // namespace

SettingsWindow::SettingsWindow(daw::EngineController* controller,
                               ShortcutManager* shortcuts, QWidget* parent)
    : QDialog(parent), m_controller(controller), m_shortcuts(shortcuts) {
    setWindowTitle(tr("Settings — %1").arg(QApplication::applicationName()));
    resize(640, 560);
    setSizeGripEnabled(true);

    m_tabs = new QTabWidget(this);
    m_tabs->setMinimumSize(0, 0);
    m_tabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    const auto addPage = [this](QWidget* page, const QString& label) {
        m_tabs->addTab(scrollablePage(page), label);
    };

    addPage(new AudioSettingsPage(m_controller, this), tr("Audio"));
    addPage(new TransportSettingsPage(m_controller, this), tr("Transport"));
    m_recordingPage = new RecordingSettingsPage(m_controller, this);
    connect(m_recordingPage, &RecordingSettingsPage::recordModeChanged, this,
            &SettingsWindow::recordModeChanged);
    addPage(m_recordingPage, tr("Recording"));
    auto* contextPage = new ContextPanelPage(this);
    connect(contextPage, &ContextPanelPage::changed, this,
            &SettingsWindow::contextPanelSettingsChanged);
    addPage(contextPage, tr("Context Panel"));
    auto* browserPage = new BrowserSettingsPage(this);
    connect(browserPage, &BrowserSettingsPage::changed, this,
            &SettingsWindow::browserSettingsChanged);
    addPage(browserPage, tr("Browser"));
    auto* aiPage = new AiSettingsPage(this);
    connect(aiPage, &AiSettingsPage::changed, this,
            &SettingsWindow::aiSettingsChanged);
    addPage(aiPage, tr("AI"));
    auto* accountPage = new AccountSettingsPage(this);
    connect(accountPage, &AccountSettingsPage::logoutRequested, this,
            &SettingsWindow::accountLogoutRequested);
    addPage(accountPage, tr("Account"));
    addPage(new RecoverySettingsPage(this), tr("Recovery"));
    addPage(buildThemesTab(), tr("Themes"));
    addPage(buildThemeEditorTab(), tr("Theme Editor"));
    addPage(buildShortcutsTab(), tr("Keyboard Shortcuts"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);

    auto* col = new QVBoxLayout(this);
    col->addWidget(m_tabs, 1);
    col->addWidget(buttons);

    constrainToScreen();
}

void SettingsWindow::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    constrainToScreen();
    // Native frame margins become reliable only after the first show. Clamp a
    // second time so a window manager cannot leave the title bar off-screen.
    QTimer::singleShot(0, this, &SettingsWindow::constrainToScreen);
}

void SettingsWindow::constrainToScreen() {
    QScreen* target = parentWidget() ? parentWidget()->screen() : screen();
    if (!target) target = QApplication::primaryScreen();
    if (!target) return;

    constexpr int kScreenInset = 20;
    constexpr int kPreferredHeight = 560;
    constexpr int kAbsoluteHeightCap = 680;
    const QRect available =
        target->availableGeometry().adjusted(kScreenInset, kScreenInset,
                                             -kScreenInset, -kScreenInset);
    if (available.isEmpty()) return;

    const int heightCap = std::max(1, std::min(kAbsoluteHeightCap,
                                               available.height()));
    setMaximumHeight(heightCap);
    const int targetHeight = std::min({height(), kPreferredHeight, heightCap});
    resize(std::min(width(), available.width()),
           targetHeight);

    // Keep the whole shell, including its title bar, reachable. The contents
    // stay available through the per-tab vertical scrollbar.
    QRect frame = frameGeometry();
    int x = frame.x();
    int y = frame.y();
    if (frame.right() > available.right())
        x -= frame.right() - available.right();
    if (frame.bottom() > available.bottom())
        y -= frame.bottom() - available.bottom();
    x = std::max(available.left(), x);
    y = std::max(available.top(), y);
    move(x, y);
}

void SettingsWindow::showTab(int index) {
    if (m_tabs) m_tabs->setCurrentIndex(index);
}

void SettingsWindow::reloadRecordingPage() {
    if (m_recordingPage) m_recordingPage->reload();
}

QWidget* SettingsWindow::buildThemesTab() {
    auto* page = new QWidget;
    auto* col = new QVBoxLayout(page);
    col->addWidget(new QLabel(tr("Choose a colour theme — it applies immediately.")));

    m_themeList = new QListWidget(page);
    {
        // Populated with the signal blocked, and the handler connected only
        // afterwards: `currentItemChanged` *persists* a theme, and no window
        // should be able to change the user's palette by being opened. That
        // matters most when the active theme is a custom one — no row matches
        // it, so any current-row the view picks for itself would be a silent
        // switch to a preset.
        QSignalBlocker block(m_themeList);
        for (const Theme& preset : ThemeManager::instance().presets()) {
            auto* item = new QListWidgetItem(preset.name, m_themeList);
            item->setData(Qt::UserRole, preset.id);
            if (preset.id == ThemeManager::instance().themeId())
                m_themeList->setCurrentItem(item);
        }
    }
    connect(m_themeList, &QListWidget::currentItemChanged, this,
            [](QListWidgetItem* cur, QListWidgetItem*) {
                if (cur)
                    ThemeManager::instance().setThemeId(
                        cur->data(Qt::UserRole).toString());
            });
    // Keep the selection in step if the theme changes elsewhere (e.g. undo of a
    // future settings action, or another surface switching it).
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] {
        const QString id = ThemeManager::instance().themeId();
        for (int i = 0; i < m_themeList->count(); ++i) {
            if (m_themeList->item(i)->data(Qt::UserRole).toString() == id) {
                QSignalBlocker block(m_themeList);
                m_themeList->setCurrentRow(i);
                break;
            }
        }
    });

    col->addWidget(m_themeList, 1);

    // How a selected track is washed. It belongs beside the palette because it
    // is a palette decision, and it is a real choice rather than a default:
    // the track's own colour tells you *which* track at a glance, while a
    // neutral wash never argues with the colour it is sitting on.
    auto* tintGroup = new QGroupBox(tr("Selected tracks"), page);
    auto* tintCol = new QVBoxLayout(tintGroup);
    auto* byColour = new QRadioButton(tr("Tint with the track's own colour"),
                                      tintGroup);
    auto* neutral = new QRadioButton(tr("Tint with a neutral wash"), tintGroup);
    (ui::selectionTint() == ui::SelectionTint::Neutral ? neutral : byColour)
        ->setChecked(true);
    const auto storeTint = [this](ui::SelectionTint tint) {
        ui::setSelectionTint(tint);
        // Nothing owns "the selection colour" as a widget property; every
        // surface reads it while painting. A theme change is the signal they
        // all already listen to, so it is the honest way to say "repaint".
        ThemeManager::instance().apply();
        emit selectionTintChanged();
    };
    connect(byColour, &QRadioButton::toggled, this, [storeTint](bool on) {
        if (on) storeTint(ui::SelectionTint::TrackColour);
    });
    connect(neutral, &QRadioButton::toggled, this, [storeTint](bool on) {
        if (on) storeTint(ui::SelectionTint::Neutral);
    });
    tintCol->addWidget(byColour);
    tintCol->addWidget(neutral);
    col->addWidget(tintGroup);
    return page;
}

QWidget* SettingsWindow::buildThemeEditorTab() {
    m_editTheme = ThemeManager::instance().theme();   // start from the active one

    auto* page = new QWidget;
    auto* col = new QVBoxLayout(page);
    col->addWidget(new QLabel(
        tr("Design your own palette — changes apply live. Export it to share, "
           "or import a theme file.")));

    // Name row.
    auto* nameRow = new QHBoxLayout;
    nameRow->addWidget(new QLabel(tr("Name")));
    m_themeNameEdit = new QLineEdit(m_editTheme.name.isEmpty() ? tr("Custom")
                                                              : m_editTheme.name);
    connect(m_themeNameEdit, &QLineEdit::textEdited, this, [this](const QString& s) {
        m_editTheme.name = s;
    });
    nameRow->addWidget(m_themeNameEdit, 1);
    col->addLayout(nameRow);

    // A scrollable grid of colour swatches.
    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    auto* gridingHost = new QWidget;
    auto* grid = new QGridLayout(gridingHost);
    grid->setContentsMargins(2, 2, 2, 2);
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(7);

    const auto& fields = colorFields();
    for (int i = 0; i < int(fields.size()); ++i) {
        const ColorField& f = fields[size_t(i)];
        const int rowN = i / 2;
        const int colN = (i % 2) * 2;

        auto* swatch = new QPushButton;
        swatch->setFixedSize(46, 22);
        swatch->setCursor(Qt::PointingHandCursor);
        swatch->setToolTip(tr("Pick a colour"));
        m_swatches.insert(f.key, swatch);
        QColor Theme::* member = f.member;
        connect(swatch, &QPushButton::clicked, this, [this, member] {
            const QColor start = m_editTheme.*member;
            const QColor picked = QColorDialog::getColor(
                start, this, tr("Choose colour"),
                QColorDialog::ShowAlphaChannel);
            if (!picked.isValid()) return;
            m_editTheme.*member = picked;
            applyEditTheme();
        });

        grid->addWidget(new QLabel(tr(f.label)), rowN, colN);
        grid->addWidget(swatch, rowN, colN + 1);
    }
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(2, 1);
    scroll->setWidget(gridingHost);
    col->addWidget(scroll, 1);

    // Actions: reseed from the active theme, export and import.
    auto* fromCurrent = new QPushButton(tr("Start From Active Theme"));
    connect(fromCurrent, &QPushButton::clicked, this, [this] {
        m_editTheme = ThemeManager::instance().theme();
        m_editTheme.name = tr("Custom");
        if (m_themeNameEdit) m_themeNameEdit->setText(m_editTheme.name);
        refreshSwatches();
        applyEditTheme();
    });
    auto* exportBtn = new QPushButton(tr("Export…"));
    connect(exportBtn, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Export Theme"),
            m_editTheme.name + ".dawtheme.json",
            tr("VLT Studio Pro Theme (*.json *.dawtheme.json)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            QMessageBox::warning(this, tr("Export failed"),
                                 tr("Could not write %1").arg(path));
            return;
        }
        file.write(QJsonDocument(ThemeManager::toJson(m_editTheme))
                       .toJson(QJsonDocument::Indented));
    });
    auto* importBtn = new QPushButton(tr("Import…"));
    connect(importBtn, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Import Theme"), QString(),
            tr("VLT Studio Pro Theme (*.json *.dawtheme.json);;All Files (*)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return;
        const auto doc = QJsonDocument::fromJson(file.readAll());
        if (!doc.isObject()) {
            QMessageBox::warning(this, tr("Import failed"),
                                 tr("%1 is not a valid theme file").arg(path));
            return;
        }
        m_editTheme = ThemeManager::fromJson(doc.object(),
                                             ThemeManager::instance().theme());
        if (m_themeNameEdit) m_themeNameEdit->setText(m_editTheme.name);
        refreshSwatches();
        applyEditTheme();
    });

    auto* actions = new QHBoxLayout;
    actions->addWidget(fromCurrent);
    actions->addStretch(1);
    actions->addWidget(importBtn);
    actions->addWidget(exportBtn);
    col->addLayout(actions);

    refreshSwatches();
    return page;
}

void SettingsWindow::applyEditTheme() {
    ThemeManager::instance().applyCustomTheme(m_editTheme, /*persist=*/true);
    refreshSwatches();
    // Keep the preset list from highlighting a stale row now that "custom" won.
    if (m_themeList) {
        QSignalBlocker block(m_themeList);
        m_themeList->setCurrentRow(-1);
    }
}

void SettingsWindow::refreshSwatches() {
    for (const ColorField& f : colorFields()) {
        auto* swatch = m_swatches.value(f.key);
        if (!swatch) continue;
        const QColor c = m_editTheme.*(f.member);
        // A checkerboard-free preview: solid fill with a hairline border.
        swatch->setStyleSheet(
            QString("background: %1; border: 1px solid rgba(128,128,128,0.6); "
                    "border-radius: 5px;")
                .arg(c.name(QColor::HexArgb)));
    }
}

QWidget* SettingsWindow::buildShortcutsTab() {
    auto* page = new QWidget;
    auto* col = new QVBoxLayout(page);
    col->addWidget(new QLabel(
        tr("Click a shortcut field and press the new keys. If a key is already "
           "used you'll be asked before reassigning it. Shortcuts follow the "
           "physical key, so they work in every keyboard layout. The same "
           "binding is shown as Command on macOS and Ctrl on Windows/Linux.")));

    auto* table = new QTableWidget(page);
    table->setColumnCount(3);
    table->setHorizontalHeaderLabels(
        {tr("Command"), tr("Category"), tr("Shortcut")});
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);

    auto commands = m_shortcuts->commands();   // copy so we can sort for display
    std::sort(commands.begin(), commands.end(),
              [](const ShortcutManager::Command& a,
                 const ShortcutManager::Command& b) {
                  if (a.category != b.category) return a.category < b.category;
                  return a.label < b.label;
              });

    table->setRowCount(int(commands.size()));
    for (int r = 0; r < int(commands.size()); ++r) {
        const ShortcutManager::Command& c = commands[r];
        table->setItem(r, 0, new QTableWidgetItem(c.label));
        table->setItem(r, 1, new QTableWidgetItem(c.category));

        auto* edit = new QKeySequenceEdit(m_shortcuts->shortcut(c.id));
        edit->setMaximumSequenceLength(1);
        m_editors.insert(c.id, edit);
        const QString id = c.id;
        connect(edit, &QKeySequenceEdit::editingFinished, this, [this, id, edit] {
            const QKeySequence seq =
                m_shortcuts->canonicalShortcut(edit->keySequence());
            if (edit->keySequence() != seq) {
                QSignalBlocker block(edit);
                edit->setKeySequence(seq);
            }
            if (seq == m_shortcuts->shortcut(id)) return;   // no real change

            const QString other = m_shortcuts->conflict(id, seq);
            if (!other.isEmpty()) {
                const auto res = QMessageBox::question(
                    this, tr("Shortcut in use"),
                    tr("\"%1\" is already assigned to \"%2\".\n\n"
                       "Reassign it to \"%3\"?")
                        .arg(seq.toString(QKeySequence::NativeText),
                             m_shortcuts->label(other), m_shortcuts->label(id)),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (res != QMessageBox::Yes) {
                    QSignalBlocker block(edit);
                    edit->setKeySequence(m_shortcuts->shortcut(id));
                    return;
                }
                m_shortcuts->setShortcut(other, QKeySequence());
                if (auto* e = m_editors.value(other)) {
                    QSignalBlocker block(e);
                    e->setKeySequence(QKeySequence());
                }
            }
            m_shortcuts->setShortcut(id, seq);
            // A Cyrillic key captured by QKeySequenceEdit is stored as its
            // physical Latin position. Reflect that canonical form at once so
            // the field and the menu show the same cross-layout binding.
            QSignalBlocker block(edit);
            edit->setKeySequence(m_shortcuts->shortcut(id));
        });
        table->setCellWidget(r, 2, edit);
    }

    auto* resetButton = new QPushButton(tr("Reset All to Defaults"));
    connect(resetButton, &QPushButton::clicked, this, [this] {
        for (auto it = m_editors.constBegin(); it != m_editors.constEnd(); ++it)
            m_shortcuts->resetToDefault(it.key());
        refreshShortcutEditors();
    });
    auto* buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    buttonRow->addWidget(resetButton);

    col->addWidget(table, 1);
    col->addLayout(buttonRow);
    return page;
}

void SettingsWindow::refreshShortcutEditors() {
    for (auto it = m_editors.constBegin(); it != m_editors.constEnd(); ++it) {
        QSignalBlocker block(it.value());
        it.value()->setKeySequence(m_shortcuts->shortcut(it.key()));
    }
}
