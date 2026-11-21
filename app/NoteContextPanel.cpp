#include "NoteContextPanel.hpp"

#include "Controls.hpp"
#include "Icons.hpp"
#include "PianoRollWindow.hpp"
#include "Theme.hpp"

#include <QColorDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>

#include <algorithm>
#include <cmath>

namespace mt = daw::miditools;

namespace {

// The same proportions as the arrangement's island, so the two panels read as
// one component that happens to appear in two places.
constexpr int kRowHeight = 20;
constexpr int kButton = 22;
constexpr int kPadding = 7;
constexpr int kEndPadding = 14;
constexpr int kShadow = 9;

constexpr int kSpringMs = 300;

ui::IconButton* islandButton(icons::Glyph glyph, const QString& tip,
                             QWidget* parent) {
    auto* button = new ui::IconButton(glyph, tip, parent);
    button->setButtonSize(kButton, kButton);
    return button;
}

QPushButton* islandSwatch(const QString& tip, QWidget* parent) {
    auto* swatch = new QPushButton(parent);
    swatch->setFixedSize(18, kRowHeight);
    swatch->setCursor(Qt::PointingHandCursor);
    swatch->setToolTip(tip);
    return swatch;
}

void paintSwatch(QPushButton* swatch, const QColor& colour) {
    swatch->setStyleSheet(
        QStringLiteral("background:%1;border:1px solid rgba(0,0,0,110);"
                       "border-radius:5px;")
            .arg(colour.name()));
}

QWidget* islandDivider(QWidget* parent) {
    QWidget* line = ui::separatorLine(Qt::Vertical, 16, parent);
    // Tagged, so the trailing-divider trim below can recognise one for certain.
    // Guessing from the width would sooner or later delete a real control.
    line->setProperty("islandSeparator", true);
    return line;
}

/// A note length reads as a division, not as a number of beats: "1/8" is what
/// the user asked for and "0.5 beats" is only how it is stored.
QString formatLength(double beats) {
    struct Named { double beats; const char* name; };
    static const Named table[] = {
        {4.0, "1/1"},       {3.0, "1/2."},      {2.0, "1/2"},
        {1.5, "1/4."},      {4.0 / 3.0, "1/2T"}, {1.0, "1/4"},
        {0.75, "1/8."},     {2.0 / 3.0, "1/4T"}, {0.5, "1/8"},
        {0.375, "1/16."},   {1.0 / 3.0, "1/8T"}, {0.25, "1/16"},
        {0.125, "1/32"},    {0.0625, "1/64"},
    };
    for (const auto& entry : table) {
        if (std::abs(beats - entry.beats) < 0.01) {
            return QString::fromUtf8(entry.name);
        }
    }
    return QString::asprintf("%.2f beats", beats);
}

QString formatPan(double pan) {
    if (std::abs(pan) < 0.005) return QObject::tr("centre");
    return QStringLiteral("%1%2")
        .arg(pan < 0 ? QObject::tr("L") : QObject::tr("R"))
        .arg(int(std::lround(std::abs(pan) * 100)));
}

}  // namespace

NoteContextPanel::NoteContextPanel(PianoRollView* view, QWidget* parent)
    : ui::GlassPanel(parent), m_view(view) {
    // Hung off the top edge of the grid, like the arrangement's plate hangs off
    // the tool strip: it reads as part of the toolbar above rather than as a
    // rectangle parked on the notes.
    setTopAttached(true);
    setAccentColor(th().accent);
    hide();
}

// ── What is selected ────────────────────────────────────────────────────────

NoteContextPanel::Context NoteContextPanel::resolve() const {
    if (!m_enabled || !m_view) return Context::None;
    const int count = m_view->selectionCount();
    if (count == 0) return Context::None;
    return count == 1 ? Context::SingleNote : Context::MultiNote;
}

bool NoteContextPanel::toolEnabled(const char* toolId) const {
    return QSettings()
        .value(QStringLiteral("contextPanel/%1").arg(QLatin1String(toolId)), true)
        .toBool();
}

QWidget* NoteContextPanel::newRow(QHBoxLayout*& row) {
    auto* host = new QWidget(this);
    row = new QHBoxLayout(host);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    return host;
}

void NoteContextPanel::afterEdit() {
    emit projectEdited();
    invalidateBackdrop();
    refresh();
}

// ── Content ─────────────────────────────────────────────────────────────────

QWidget* NoteContextPanel::buildContent(Context context) {
    switch (context) {
        case Context::SingleNote: return buildNotes(/*multiple=*/false);
        case Context::MultiNote:  return buildNotes(/*multiple=*/true);
        case Context::None:       break;
    }
    return nullptr;
}

QWidget* NoteContextPanel::buildNotes(bool multiple) {
    if (!m_view) return nullptr;

    QHBoxLayout* row = nullptr;
    QWidget* host = newRow(row);
    // Each control registers a loader; `refresh` runs them all, so the panel
    // never holds a note pointer — an undo replaces whole note vectors.
    std::vector<std::function<void()>> loaders;

    // The count is the one caption on the plate, and only when it says
    // something a single note wouldn't: how many are about to be changed.
    if (multiple) {
        auto* count = new QLabel(host);
        count->setStyleSheet(QStringLiteral("color:%1;font-size:11px;"
                                            "font-weight:600;")
                                 .arg(th().textSecondary.name()));
        row->addWidget(count);
        loaders.push_back([this, count] {
            count->setText(tr("%1 notes").arg(m_view->selectionSummary().count));
        });
        row->addWidget(islandDivider(host));
    }

    if (toolEnabled("note.colour")) {
        auto* swatch = islandSwatch(multiple ? tr("Colour of every selected note")
                                             : tr("Note colour"),
                                    host);
        connect(swatch, &QAbstractButton::clicked, this, [this] {
            const auto summary = m_view->selectionSummary();
            const QColor start = summary.color ? colorFromRgb(summary.color)
                                               : th().accent;
            const QColor picked =
                QColorDialog::getColor(start, this, tr("Note Colour"));
            if (!picked.isValid()) return;
            m_view->setSelectionColor((uint32_t(picked.red()) << 16) |
                                      (uint32_t(picked.green()) << 8) |
                                      uint32_t(picked.blue()));
            // Colour is invisible unless the roll is showing per-note colours,
            // so picking one switches that on rather than doing nothing.
            m_view->setColorMode(PianoRollView::ColorMode::Custom);
            afterEdit();
        });
        row->addWidget(swatch);
        loaders.push_back([this, swatch] {
            const auto summary = m_view->selectionSummary();
            paintSwatch(swatch, summary.color ? colorFromRgb(summary.color)
                                              : th().textSecondary);
        });

        auto* mute = islandButton(
            icons::Glyph::Power,
            multiple ? tr("Disable / enable all selected notes")
                     : tr("Disable / enable this note"),
            host);
        mute->setCheckable(true);
        mute->setActiveColor(Theme::mute());
        connect(mute, &QAbstractButton::clicked, this, [this](bool on) {
            if (m_updating) return;
            m_view->setSelectionMuted(on);
            afterEdit();
        });
        row->addWidget(mute);
        loaders.push_back([this, mute] {
            mute->setChecked(m_view->selectionSummary().muted);
        });
        row->addWidget(islandDivider(host));
    }

    if (toolEnabled("note.level")) {
        auto* velocity = new ui::MiniSlider(icons::Glyph::Volume, tr("Velocity"),
                                            host);
        velocity->setRange(1.0, 127.0);
        velocity->setStep(1.0);
        velocity->setDefaultValue(100.0);
        velocity->setFormatter(
            [](double v) { return QString::number(int(std::lround(v))); });
        connect(velocity, &ui::MiniSlider::editStarted, this, [this] {
            if (!m_updating) m_view->beginSelectionVelocityEdit();
        });
        connect(velocity, &ui::MiniSlider::valueChanged, this, [this](double v) {
            if (m_updating) return;
            m_view->setSelectionVelocity(int(std::lround(v)));
        });
        // Live while dragging and dirty at the end — the same bargain the note
        // handles make, and the reason a drag doesn't fill the undo stack.
        connect(velocity, &ui::MiniSlider::editFinished, this, [this] {
            m_view->endSelectionVelocityEdit();
            // A note at MIDI's ceiling can clamp before its neighbours. Pull
            // the real average back into the control before the next gesture.
            refresh();
            emit projectEdited();
        });
        row->addWidget(velocity);
        loaders.push_back([this, velocity] {
            velocity->setValue(m_view->selectionSummary().velocity);
        });

        auto* pan = new ui::MiniSlider(icons::Glyph::Pan, tr("Note pan"), host);
        pan->setRange(-1.0, 1.0);
        pan->setStep(0.01);
        pan->setDefaultValue(0.0);
        pan->setFormatter(formatPan);
        connect(pan, &ui::MiniSlider::editStarted, this, [this] {
            if (!m_updating) m_view->beginSelectionEdit();
        });
        connect(pan, &ui::MiniSlider::valueChanged, this, [this](double value) {
            if (m_updating) return;
            m_view->setSelectionPan(float(value));
        });
        connect(pan, &ui::MiniSlider::editFinished, this, [this] {
            m_view->endSelectionEdit(tr("Change Note Pan"));
            emit projectEdited();
        });
        row->addWidget(pan);
        loaders.push_back(
            [this, pan] { pan->setValue(m_view->selectionSummary().pan); });
        row->addWidget(islandDivider(host));
    }

    if (toolEnabled("note.timing")) {
        auto* length = new ui::MiniSlider(icons::Glyph::Clock, tr("Note length"),
                                          host);
        length->setRange(1.0 / 32.0, 8.0);
        length->setStep(1.0 / 64.0);
        length->setDefaultValue(1.0);
        length->setFormatter(formatLength);
        connect(length, &ui::MiniSlider::editStarted, this, [this] {
            if (!m_updating) m_view->beginSelectionEdit();
        });
        connect(length, &ui::MiniSlider::valueChanged, this, [this](double beats) {
            if (m_updating) return;
            m_view->setSelectionLength(beats);
        });
        connect(length, &ui::MiniSlider::editFinished, this, [this] {
            m_view->endSelectionEdit(tr("Change Note Length"));
            emit projectEdited();
        });
        row->addWidget(length);
        loaders.push_back([this, length] {
            length->setValue(m_view->selectionSummary().lengthBeats);
        });

        auto* down = islandButton(icons::Glyph::ArrowDown, tr("Down a semitone"), host);
        auto* up = islandButton(icons::Glyph::ArrowUp, tr("Up a semitone"), host);
        connect(down, &QAbstractButton::clicked, this, [this] {
            m_view->transposeSelection(-1);
            afterEdit();
        });
        connect(up, &QAbstractButton::clicked, this, [this] {
            m_view->transposeSelection(1);
            afterEdit();
        });
        row->addWidget(down);
        row->addWidget(up);

        // The pitch of a single note is worth naming; an average pitch across a
        // chord is not, so the group gets no read-out at all.
        if (!multiple) {
            auto* pitch = new QLabel(host);
            pitch->setStyleSheet(QStringLiteral("color:%1;font-size:11px;"
                                                "font-weight:600;")
                                     .arg(th().textPrimary.name()));
            pitch->setMinimumWidth(28);
            row->addWidget(pitch);
            loaders.push_back([this, pitch] {
                pitch->setText(QString::fromStdString(
                    mt::pitchName(m_view->selectionSummary().pitch)));
            });
        }
        row->addWidget(islandDivider(host));
    }

    if (toolEnabled("note.tools")) {
        // The parameter tools. The panel does not own their dialogs — it asks,
        // and the window opens the one it already has, so a tool opened from
        // here and one opened from the menu are the same window with the same
        // settings.
        const std::tuple<icons::Glyph, Tool, QString> tools[] = {
            {icons::Glyph::Grid, Tool::Quantize, tr("Quantize…")},
            {icons::Glyph::Arpeggio, Tool::Arpeggiator, tr("Arpeggiator…")},
            {icons::Glyph::Strum, Tool::Strum, tr("Strum…")},
            {icons::Glyph::Glue, Tool::Glue, tr("Glue and legato…")},
            {icons::Glyph::Articulate, Tool::Articulate, tr("Articulate…")},
            {icons::Glyph::Dice, Tool::Randomize, tr("Randomize…")},
        };
        for (const auto& [glyph, tool, tip] : tools) {
            auto* button = islandButton(glyph, tip, host);
            connect(button, &QAbstractButton::clicked, this,
                    [this, tool] { emit toolRequested(tool); });
            row->addWidget(button);
        }

        // Two more that only mean anything to a group: one note cannot be
        // turned around in time, and mirroring a single pitch is a no-op.
        if (multiple) {
            auto* reverse = islandButton(icons::Glyph::Rewind,
                                         tr("Reverse the phrase in time"), host);
            connect(reverse, &QAbstractButton::clicked, this, [this] {
                m_view->applyTransform(
                    [](const mt::Notes& n) { return mt::reverseTime(n); },
                    tr("Reverse"));
                afterEdit();
            });
            auto* invert = islandButton(icons::Glyph::Invert,
                                        tr("Turn the pitches upside down"), host);
            connect(invert, &QAbstractButton::clicked, this, [this] {
                m_view->applyTransform(
                    [](const mt::Notes& n) { return mt::invertPitch(n); },
                    tr("Invert"));
                afterEdit();
            });
            row->addWidget(reverse);
            row->addWidget(invert);
        }
        row->addWidget(islandDivider(host));
    }

    if (toolEnabled("note.edit")) {
        auto* duplicate =
            islandButton(icons::Glyph::Plus,
                         multiple ? tr("Duplicate all selected") : tr("Duplicate"),
                         host);
        connect(duplicate, &QAbstractButton::clicked, this, [this] {
            m_view->duplicateSelection();
            afterEdit();
        });
        auto* remove = islandButton(icons::Glyph::Trash,
                                    multiple ? tr("Delete all selected")
                                             : tr("Delete"),
                                    host);
        connect(remove, &QAbstractButton::clicked, this, [this] {
            m_view->deleteSelection();
            afterEdit();
        });
        row->addWidget(duplicate);
        row->addWidget(remove);
    }

    // Drop a trailing divider — the profile settings can switch off whichever
    // group happens to be last, and a rule floating at the end looks broken.
    if (row->count() > 0) {
        QLayoutItem* last = row->itemAt(row->count() - 1);
        QWidget* widget = last ? last->widget() : nullptr;
        if (widget && widget->property("islandSeparator").toBool()) {
            delete row->takeAt(row->count() - 1)->widget();
        }
    }
    if (row->count() == 0) {
        host->deleteLater();
        return nullptr;
    }

    m_applyValues = [this, loaders] {
        if (!m_view) return;
        m_updating = true;
        for (const auto& load : loaders) load();
        m_updating = false;
    };
    m_applyValues();
    return host;
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

void NoteContextPanel::refresh() {
    const Context next = resolve();
    const QString key = next == Context::None ? QString() : m_view->selectionKey();
    if (next == m_context && key == m_contextKey) {
        // Same notes: the values may still have moved under us (an undo, a drag
        // in the grid), so the controls are reloaded but nothing is rebuilt.
        if (m_applyValues) m_applyValues();
        return;
    }
    m_contextKey = key;
    transitionTo(buildContent(next), next);
}

void NoteContextPanel::rebuild() {
    m_context = Context::None;
    m_contextKey.clear();
    refresh();
}

void NoteContextPanel::setPanelEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    rebuild();
}

void NoteContextPanel::relayout() {
    if (!m_content || !isVisible()) return;
    setGeometry(targetGeometry());
    setCornerRadius(plateRect().height() / 2);
}

void NoteContextPanel::resizeEvent(QResizeEvent* ev) {
    ui::GlassPanel::resizeEvent(ev);
    layoutSelf();
}

QRect NoteContextPanel::targetGeometry() const {
    QWidget* host = parentWidget();
    if (!host) return geometry();
    if (!m_content) {
        // Collapsed: keep the centre, so closing reads as a shrink back into the
        // toolbar rather than a slide off to one side.
        const QRect current = geometry();
        return QRect(current.center().x(), 0, 1, current.height());
    }
    const int width = std::min(m_content->sizeHint().width() +
                                   2 * (kShadow + kEndPadding),
                               host->width() - 24);
    const int height = std::min(kRowHeight + 2 * kPadding + kShadow, host->height());
    return QRect((host->width() - width) / 2, 0, std::max(1, width), height);
}

void NoteContextPanel::layoutSelf() {
    if (!m_content) return;
    const QRect inner = plateRect();
    const QSize size = m_content->size();
    m_content->move(inner.center().x() - size.width() / 2,
                    inner.center().y() - size.height() / 2);
}

void NoteContextPanel::transitionTo(QWidget* next, Context context) {
    // stop() destroys the group (DeleteWhenStopped) and never emits finished,
    // so an interrupted swap has to be cleaned up right here.
    if (m_transition) m_transition->stop();
    if (m_outgoing) {
        m_outgoing->deleteLater();
        m_outgoing = nullptr;
    }
    m_outgoing = m_content;
    m_content = next;
    m_context = next ? context : Context::None;
    if (!next) m_applyValues = nullptr;
    if (m_outgoing) m_outgoing->hide();

    if (!m_content) {
        hide();
        return;
    }

    m_content->resize(m_content->sizeHint().width(),
                      std::max(kRowHeight, m_content->sizeHint().height()));
    // A child built after its parent is already on screen stays hidden until it
    // is shown explicitly — without this the plate opens empty, which is
    // exactly what it did the first time.
    m_content->show();
    const bool wasVisible = isVisible();
    const QRect target = targetGeometry();
    if (!wasVisible) {
        // Open from a sliver at the centre of where it is going, so the plate
        // grows out of the toolbar instead of appearing fully formed.
        setGeometry(QRect(target.center().x(), target.y(), 1, target.height()));
        show();
        raise();
    }
    layoutSelf();

    auto* group = new QParallelAnimationGroup(this);
    auto* spring = new QPropertyAnimation(this, "geometry", group);
    spring->setDuration(kSpringMs);
    spring->setStartValue(geometry());
    spring->setEndValue(target);
    spring->setEasingCurve(QEasingCurve::OutBack);
    group->addAnimation(spring);

    // Recapturing the backdrop every frame is the most expensive thing the
    // plate does, and the motion hides the staleness anyway.
    setBackdropFrozen(true);
    connect(group, &QAbstractAnimation::finished, this, [this] {
        setBackdropFrozen(false);
        invalidateBackdrop();
        if (m_outgoing) {
            m_outgoing->deleteLater();
            m_outgoing = nullptr;
        }
        setCornerRadius(plateRect().height() / 2);
        layoutSelf();
        update();
    });
    m_transition = group;
    group->start(QAbstractAnimation::DeleteWhenStopped);
}
