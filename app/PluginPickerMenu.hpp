#pragma once

#include <QString>
#include <functional>

class QMenu;
class QWidget;

namespace daw {
class EngineController;
namespace plugins { struct PluginDescriptor; }
} // namespace daw

namespace ui {

/// A menu of scanned plugins, grouped by vendor, filtered to instruments or to
/// effects. Shared by the insert slots and the instrument slot so the two can
/// never present different lists.
///
/// The menu is built fresh each time it is opened: the scan runs in the
/// background and the set of plugins can change under a menu that was built
/// once and cached.
QMenu* buildPluginMenu(QWidget* parent, daw::EngineController* controller,
                       bool instruments,
                       std::function<void(const daw::plugins::PluginDescriptor&)> onPick);

} // namespace ui
