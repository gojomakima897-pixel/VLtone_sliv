#pragma once

#include <cstddef>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

namespace daw {

/// Coarse-grained closure-pair undo/redo, mirroring the Swift UndoStack. Each
/// entry is an (undo, redo) pair with a label. The `isApplying` guard stops an
/// undo/redo from recording itself as a new entry.
class UndoStack {
public:
    struct Entry {
        std::string label;
        std::function<void()> undo;
        std::function<void()> redo;
    };

    explicit UndoStack(size_t limit = 100) : m_limit(limit) {}

    /// Record an operation that has *already* been performed.
    void push(std::string label,
              std::function<void()> undo,
              std::function<void()> redo) {
        if (m_applying) return;
        m_redoStack.clear();
        m_undoStack.push_back({std::move(label), std::move(undo), std::move(redo)});
        if (m_undoStack.size() > m_limit) {
            m_undoStack.erase(m_undoStack.begin());
        }
    }

    /// The cap. Past it the oldest entry is dropped for every new one, so the
    /// depth stops growing — which a caller that counts entries has to know.
    std::size_t limit() const noexcept { return m_limit; }

    bool canUndo() const { return !m_undoStack.empty(); }
    bool canRedo() const { return !m_redoStack.empty(); }

    /// How many entries are on the undo stack. Paired with `collapse` it lets a
    /// caller mark where a long operation began and fold everything since.
    size_t depth() const { return m_undoStack.size(); }

    /// Replace the last `count` entries with one, so an operation the user
    /// thinks of as a single action undoes as a single action. Unlike
    /// `Suspend`, this needs no guard held across the work, which is what makes
    /// it usable for something spanning network waits (the AI assistant): the
    /// steps record normally and are folded once they are all done.
    ///
    /// Refuses when `count` does not fit — the stack's own limit may have
    /// discarded the start of the group — leaving the entries individual rather
    /// than building one that only undoes half of what it claims.
    void collapse(size_t count, std::string label) {
        if (count < 2 || count > m_undoStack.size()) return;
        auto first = m_undoStack.end() - static_cast<std::ptrdiff_t>(count);
        std::vector<Entry> group(std::make_move_iterator(first),
                                 std::make_move_iterator(m_undoStack.end()));
        m_undoStack.erase(first, m_undoStack.end());
        m_undoStack.push_back(
            {std::move(label),
             [group] {
                 for (auto it = group.rbegin(); it != group.rend(); ++it)
                     if (it->undo) it->undo();
             },
             [group] {
                 for (const Entry& e : group)
                     if (e.redo) e.redo();
             }});
        // A collapse only ever describes work just done, so the redo branch is
        // already dead; clearing it keeps that explicit.
        m_redoStack.clear();
    }

    /// Forget entries recorded after a caller's depth marker. This is the
    /// companion to a snapshot-based gesture: a complex live edit may need to
    /// call ordinary undoable primitives while it is in flight, then replace
    /// those implementation details with one before/after entry on release.
    void discardSince(size_t depth) {
        if (depth >= m_undoStack.size()) return;
        m_undoStack.erase(m_undoStack.begin() +
                              static_cast<std::ptrdiff_t>(depth),
                          m_undoStack.end());
        m_redoStack.clear();
    }

    std::string undoLabel() const {
        return m_undoStack.empty() ? "" : m_undoStack.back().label;
    }
    std::string redoLabel() const {
        return m_redoStack.empty() ? "" : m_redoStack.back().label;
    }

    void undo() {
        if (m_undoStack.empty()) return;
        Entry e = std::move(m_undoStack.back());
        m_undoStack.pop_back();
        m_applying = true;
        if (e.undo) e.undo();
        m_applying = false;
        m_redoStack.push_back(std::move(e));
    }

    void redo() {
        if (m_redoStack.empty()) return;
        Entry e = std::move(m_redoStack.back());
        m_redoStack.pop_back();
        m_applying = true;
        if (e.redo) e.redo();
        m_applying = false;
        m_undoStack.push_back(std::move(e));
    }

    void clear() {
        m_undoStack.clear();
        m_redoStack.clear();
    }

    bool isApplying() const { return m_applying; }

    /// RAII guard that suppresses recording while a compound edit runs, so a
    /// multi-step operation (packing tracks into a folder, say) lands on the
    /// stack as the single entry its caller pushes afterwards.
    class Suspend {
    public:
        explicit Suspend(UndoStack& stack)
            : m_stack(stack), m_previous(stack.m_applying) {
            stack.m_applying = true;
        }
        ~Suspend() { m_stack.m_applying = m_previous; }
        Suspend(const Suspend&) = delete;
        Suspend& operator=(const Suspend&) = delete;

    private:
        UndoStack& m_stack;
        bool m_previous;
    };

private:
    size_t m_limit;
    bool m_applying = false;
    std::vector<Entry> m_undoStack;
    std::vector<Entry> m_redoStack;
};

} // namespace daw
