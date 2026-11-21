#include "PluginPickerMenu.hpp"

#include "EngineController.hpp"
#include "PluginFormatPreference.hpp"
#include "Theme.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QSettings>
#include <QTimer>
#include <QWidgetAction>

#include <algorithm>
#include <map>
#include <vector>

namespace ui {

namespace {

/// How many matches a search shows before it stops. Past this the list is not
/// an answer any more, and the query is what wants narrowing.
constexpr int kMaxMatches = 40;

QString formatLabel(daw::plugins::Format format) {
    switch (format) {
        case daw::plugins::Format::Vst3: return QStringLiteral("VST3");
        case daw::plugins::Format::AudioUnit: return QStringLiteral("AU");
        case daw::plugins::Format::Clap: return QStringLiteral("CLAP");
        case daw::plugins::Format::Internal: return QObject::tr("Built-in");
        case daw::plugins::Format::Unknown: break;
    }
    return {};
}

/// A coarse, human bucket for a plugin.
///
/// The formats disagree about what a category is: VST3 gives a subcategory
/// list ("Fx|Reverb|Stereo"), CLAP gives features ("reverb"), and Audio Units
/// give "Audio Effect" and nothing else — which is why the plugin's own
/// metadata is read first and its *name* second. Guessing from a name is a
/// heuristic, but "Studio Reverb" in a Reverb group beats every Audio Unit
/// piled into Other.
QString categoryOf(const daw::plugins::PluginDescriptor& descriptor) {
    if (descriptor.isInstrument) return QObject::tr("Instruments");

    struct Bucket { const char* key; const char* name; bool nameToo; };
    // Order matters: the first hit wins, so the specific keys come first.
    static const Bucket kBuckets[] = {
        {"reverb", "Reverb", true},        {"room", "Reverb", true},
        {"hall", "Reverb", false},         {"plate", "Reverb", false},
        {"delay", "Delay", true},          {"echo", "Delay", true},
        {"equalizer", "EQ", true},         {"eq", "EQ", false},
        {"filter", "Filter", true},
        {"compressor", "Dynamics", true},  {"limiter", "Dynamics", true},
        {"dynamics", "Dynamics", true},    {"gate", "Dynamics", true},
        {"expander", "Dynamics", true},    {"deesser", "Dynamics", true},
        {"distortion", "Distortion", true},{"saturat", "Distortion", true},
        {"overdrive", "Distortion", true}, {"fuzz", "Distortion", true},
        {"guitar", "Distortion", false},
        {"chorus", "Modulation", true},    {"flanger", "Modulation", true},
        {"phaser", "Modulation", true},    {"tremolo", "Modulation", true},
        {"modulation", "Modulation", true},{"vibrato", "Modulation", true},
        {"pitch", "Pitch", true},          {"harmon", "Pitch", true},
        {"vocoder", "Pitch", true},        {"tuner", "Analyzer", true},
        {"analy", "Analyzer", true},       {"meter", "Analyzer", true},
        {"spectrum", "Analyzer", true},    {"scope", "Analyzer", false},
        {"mastering", "Mastering", true},
        {"restoration", "Restoration", true}, {"denois", "Restoration", true},
        {"declick", "Restoration", true},  {"dereverb", "Restoration", true},
        {"spatial", "Spatial", true},      {"imager", "Spatial", true},
        {"surround", "Spatial", true},     {"panner", "Spatial", true},
        {"generator", "Generator", false}, {"synth", "Generator", false},
        {"utility", "Utility", true},      {"tools", "Utility", false},
    };

    const QString category =
        QString::fromStdString(descriptor.category).toLower();
    for (const Bucket& bucket : kBuckets) {
        if (category.contains(QLatin1String(bucket.key)))
            return QObject::tr(bucket.name);
    }
    const QString name = QString::fromStdString(descriptor.name).toLower();
    for (const Bucket& bucket : kBuckets) {
        if (bucket.nameToo && name.contains(QLatin1String(bucket.key)))
            return QObject::tr(bucket.name);
    }
    return QObject::tr("Other");
}

QString vendorOf(const daw::plugins::PluginDescriptor& descriptor) {
    return descriptor.vendor.empty() ? QObject::tr("Unknown")
                                     : QString::fromStdString(descriptor.vendor);
}

/// What a row says: the plugin, and the format it comes in. The tab is not
/// decoration — QMenu draws everything past it in the shortcut column, right
/// aligned and dimmed, which is exactly where a format badge belongs.
QString rowText(const daw::plugins::PluginDescriptor& descriptor) {
    return QString::fromStdString(descriptor.name) + QLatin1Char('\t') +
           formatLabel(descriptor.format);
}

/// Feeds the menu's keystrokes to its search field.
///
/// A QMenu grabs the keyboard while it is open and answers the keys itself, so
/// a QLineEdit inside it never hears a letter. Anything that is not a menu
/// gesture — the arrows, Enter, Escape, Tab — is handed to the field instead.
class MenuSearchFilter : public QObject {
public:
    MenuSearchFilter(QMenu* menu, QLineEdit* edit)
        : QObject(menu), m_edit(edit) {}

protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() != QEvent::KeyPress) return false;
        auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
            case Qt::Key_Escape:
            case Qt::Key_Return:
            case Qt::Key_Enter:
            case Qt::Key_Up:
            case Qt::Key_Down:
            case Qt::Key_Left:
            case Qt::Key_Right:
            case Qt::Key_Tab:
            case Qt::Key_Backtab:
                return false;   // the menu's own navigation
            default:
                break;
        }
        const bool typing = key->key() == Qt::Key_Backspace ||
                            (!key->text().isEmpty() && key->text().at(0).isPrint());
        if (!typing) return false;
        QApplication::sendEvent(m_edit, key);
        return true;
    }

private:
    QLineEdit* m_edit = nullptr;
};

void applyDarkPluginMenuStyle(QMenu* menu) {
    if (!menu) return;
    const Theme& theme = th();
    const QColor hover = mixColors(theme.well(), theme.textPrimary, 0.14);
    const QColor selected = mixColors(theme.well(), theme.accent, 0.20);
    menu->setObjectName(QStringLiteral("PluginPickerMenu"));
    menu->setStyleSheet(QString(R"(
QMenu { background: %1; color: %2; border: 1px solid %3;
        border-radius: 10px; padding: 5px; }
QMenu::item { min-height: 18px; padding: 4px 22px 4px 9px;
              border-radius: 5px; background: transparent; }
QMenu::item:selected { background: %4; color: %2; }
QMenu::item:disabled { color: %5; }
QMenu::separator { height: 1px; background: %3; margin: 4px 6px; }
QMenu::scroller { height: 14px; background: %1; }
QLineEdit { color: %2; background: %6; border: 1px solid %3;
            border-radius: 6px; padding: 4px 8px; }
QLineEdit:hover { background: %7; }
QLineEdit:focus { border-color: %8; }
)")
        .arg(theme.well().name(), theme.textPrimary.name(),
             theme.separator().name(), selected.name(),
             theme.textSecondary.name(), theme.surface.name(), hover.name(),
             theme.accent.name()));
}

} // namespace

QMenu* buildPluginMenu(QWidget* parent, daw::EngineController* controller,
                       bool instruments,
                       std::function<void(const daw::plugins::PluginDescriptor&)> onPick) {
    auto* menu = new QMenu(parent);
    applyDarkPluginMenuStyle(menu);
    if (!controller) return menu;

    std::vector<daw::plugins::PluginDescriptor> found =
        instruments ? controller->pluginManager().instruments()
                    : controller->pluginManager().effects();
    found = daw::preferredPluginVariants(std::move(found),
                                         ui::preferredPluginFormat());

    if (found.empty()) {
        // Say which of the two situations this is. "No plugins" while a scan is
        // running looks like a bug; saying so does not.
        QAction* empty = menu->addAction(
            controller->pluginManager().isScanning()
                ? QObject::tr("Scanning for plugins…")
                : QObject::tr("No plugins found — scan in Settings ▸ Plugin Manager"));
        empty->setEnabled(false);
        return menu;
    }

    // ── The search field, at the top ──
    auto* edit = new QLineEdit(menu);
    edit->setPlaceholderText(QObject::tr("Search plugins…"));
    edit->setClearButtonEnabled(true);
    edit->setMinimumWidth(230);
    auto* editAction = new QWidgetAction(menu);
    editAction->setDefaultWidget(edit);
    menu->addAction(editAction);
    menu->addSeparator();
    menu->installEventFilter(new MenuSearchFilter(menu, edit));
    QObject::connect(menu, &QMenu::aboutToShow, edit, [edit] {
        // After the popup has taken its grab, or the focus lands back on it.
        QTimer::singleShot(0, edit, [edit] { edit->setFocus(); });
    });

    // ── Grouped, by whatever the user asked for ──
    const bool byCategory =
        QSettings().value(QStringLiteral("plugins/menuGrouping"),
                          QStringLiteral("vendor")).toString() ==
        QLatin1String("category");

    std::map<QString, std::vector<daw::plugins::PluginDescriptor>> groups;
    for (const daw::plugins::PluginDescriptor& descriptor : found) {
        groups[byCategory ? categoryOf(descriptor) : vendorOf(descriptor)]
            .push_back(descriptor);
    }

    // The group submenus, and — hidden until something is typed — one flat row
    // per plugin. Two lists rather than a rebuild: a menu cannot be repopulated
    // while it is open without closing the popup the user is typing into.
    std::vector<QAction*> groupActions;
    for (auto& [name, entries] : groups) {
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.name < b.name; });
        QMenu* submenu = menu->addMenu(
            QObject::tr("%1  (%2)").arg(name).arg(entries.size()));
        applyDarkPluginMenuStyle(submenu);
        groupActions.push_back(submenu->menuAction());
        for (const daw::plugins::PluginDescriptor& descriptor : entries) {
            QAction* action = submenu->addAction(rowText(descriptor));
            QObject::connect(action, &QAction::triggered, parent,
                             [onPick, descriptor] { onPick(descriptor); });
        }
    }

    std::vector<daw::plugins::PluginDescriptor> flat(found.begin(), found.end());
    std::sort(flat.begin(), flat.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    std::vector<QAction*> matchActions;
    matchActions.reserve(flat.size());
    for (const daw::plugins::PluginDescriptor& descriptor : flat) {
        QAction* action = menu->addAction(rowText(descriptor));
        action->setVisible(false);
        QObject::connect(action, &QAction::triggered, parent,
                         [onPick, descriptor] { onPick(descriptor); });
        matchActions.push_back(action);
    }
    QAction* noMatch = menu->addAction(QObject::tr("Nothing matches"));
    noMatch->setEnabled(false);
    noMatch->setVisible(false);

    QObject::connect(edit, &QLineEdit::textChanged, menu,
                     [groupActions, matchActions, noMatch, flat](const QString& query) {
        const QString needle = query.trimmed();
        const bool searching = !needle.isEmpty();
        for (QAction* group : groupActions) group->setVisible(!searching);

        int shown = 0;
        for (std::size_t i = 0; i < matchActions.size(); ++i) {
            bool hit = false;
            if (searching && shown < kMaxMatches) {
                // Name or vendor: "waves" and "reverb" are both things a person
                // types into this box, and only one of them is the name.
                const QString name = QString::fromStdString(flat[i].name);
                const QString vendor = QString::fromStdString(flat[i].vendor);
                hit = name.contains(needle, Qt::CaseInsensitive) ||
                      vendor.contains(needle, Qt::CaseInsensitive);
                if (hit) ++shown;
            }
            matchActions[i]->setVisible(hit);
        }
        noMatch->setVisible(searching && shown == 0);
    });

    return menu;
}

} // namespace ui
