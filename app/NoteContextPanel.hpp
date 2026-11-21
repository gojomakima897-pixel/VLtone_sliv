#pragma once

#include "GlassPanel.hpp"

#include <QPointer>
#include <QString>

#include <functional>
#include <vector>

class PianoRollView;
class QAbstractAnimation;
class QHBoxLayout;

/// The piano roll's context panel: one floating plate over the note grid that
/// shows what can be done to the notes selected right now, and nothing else.
///
/// The same idea as the arrangement's `ContextPanel`, applied to notes — and
/// the same rules: icon-sized controls only, no captions, content swapped with
/// enough motion that the swap is legible. It is a separate class rather than
/// another context inside `ContextPanel` because that one is driven by
/// `ui::SelectionModel` (tracks and clips), while a note selection lives in the
/// piano roll's own view and never enters that model.
///
/// Everything it does goes through `PianoRollView`: live setters for the
/// continuous values (velocity, pan, length — dragged per mouse-move, exactly
/// like the note handles themselves) and `applyTransform` for the discrete
/// commands, so each of those is one labelled undo entry. Tools that need
/// parameters do not open their own dialogs; the panel reports the request and
/// the window opens the dialog it already owns.
class NoteContextPanel : public ui::GlassPanel {
    Q_OBJECT
public:
    /// The parameter tools, which the panel cannot run on its own — each has a
    /// dialog owned by `PianoRollWindow`.
    enum class Tool {
        Quantize, Arpeggiator, Chord, Strum, Glue, Articulate, Randomize
    };

    NoteContextPanel(PianoRollView* view, QWidget* parent = nullptr);

    /// Re-read the selection and swap the content if what is selected changed
    /// kind or identity. Cheap when nothing has.
    void refresh();
    /// Rebuild from scratch — after the tool profiles change, or a new clip.
    void rebuild();
    /// Re-centre the plate after the grid resizes.
    void relayout();

    /// The user's View-menu toggle. An off panel stays hidden however many
    /// notes are selected.
    void setPanelEnabled(bool enabled);
    bool isPanelEnabled() const { return m_enabled; }

signals:
    /// The document changed; the window marks the project dirty and repaints.
    void projectEdited();
    /// A tool with parameters was asked for.
    void toolRequested(NoteContextPanel::Tool tool);

protected:
    void resizeEvent(QResizeEvent*) override;

private:
    /// What the panel is showing. One note and several notes get different
    /// content: a single note can show its own values, a group can only offer
    /// what makes sense applied to all of them at once.
    enum class Context { None, SingleNote, MultiNote };

    Context resolve() const;
    QWidget* buildContent(Context context);
    /// The controls shared by both contexts, plus the ones only a group gets.
    QWidget* buildNotes(bool multiple);
    void transitionTo(QWidget* next, Context context);

    QRect targetGeometry() const;
    void layoutSelf();
    bool toolEnabled(const char* toolId) const;
    QWidget* newRow(QHBoxLayout*& row);
    /// Repaint, mark dirty, and pull the document back into the controls.
    void afterEdit();

    PianoRollView* m_view = nullptr;

    Context m_context = Context::None;
    /// Identity of the selection, so re-selecting the same notes after an
    /// unrelated refresh doesn't rebuild and restart the animation.
    QString m_contextKey;
    QWidget* m_content = nullptr;
    QWidget* m_outgoing = nullptr;
    QPointer<QAbstractAnimation> m_transition;
    /// Pushes the document's values into the current controls.
    std::function<void()> m_applyValues;
    bool m_enabled = true;
    /// Suppresses control signals while loading, so writing a slider's value
    /// into it doesn't read back as the user having moved it.
    bool m_updating = false;
};
