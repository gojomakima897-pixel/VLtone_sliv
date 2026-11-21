#include "UiConstants.hpp"

#include <QSettings>

#include <algorithm>

namespace ui {

SelectionTint selectionTint() {
    const int stored =
        QSettings().value(kSelectionTintSetting, int(SelectionTint::TrackColour))
            .toInt();
    return SelectionTint(std::clamp(stored, 0, int(SelectionTint::Neutral)));
}

void setSelectionTint(SelectionTint tint) {
    QSettings().setValue(kSelectionTintSetting, int(tint));
}

QColor selectionWash(const QColor& trackColor) {
    const Theme& t = th();
    if (selectionTint() == SelectionTint::Neutral) {
        // Not white, and not the accent: a wash of the foreground, which is
        // whatever reads against the surface in this palette. On a dark theme
        // that is a pale grey, on a light one a dim one — either way it says
        // "selected" without claiming a colour of its own.
        return t.textPrimary;
    }
    // The track's colour, lifted towards the text so a dark track still shows a
    // difference against the row it is sitting on. Its own colour is what the
    // eye is already using to find this track.
    return mixColors(trackColor, t.textPrimary, t.dark ? 0.10 : 0.0);
}

} // namespace ui
