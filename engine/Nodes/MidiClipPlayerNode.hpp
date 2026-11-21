#pragma once

#include "DSP/Simd.hpp"
#include "Graph/Node.hpp"
#include "Midi/MidiEvent.hpp"
#include "Common/RealtimeSnapshot.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace daw::engine {

/// One note on the timeline, in musical time.
///
/// Beats, not samples, and deliberately so: a tempo change has to move the
/// notes, and the only way that happens for free is if the note never held a
/// sample position to begin with. The conversion happens per block, against the
/// transport the whole graph agrees on.
struct MidiNote {
    double startBeats = 0.0;    ///< from the start of the timeline
    double lengthBeats = 1.0;
    std::uint8_t key = 60;
    std::uint8_t velocity = 100;
    std::uint8_t channel = 0;
};

/// Plays MIDI clips: turns a list of notes into note-on and note-off events at
/// the right sample offsets.
///
/// The note list is published as an immutable snapshot, like `ClipPlayerNode`'s
/// clip list, so editing in the piano roll never locks the audio thread.
class MidiClipPlayerNode : public Node {
public:
    using NoteList = std::vector<MidiNote>;

    explicit MidiClipPlayerNode(std::string name = "MIDI Clips")
        : m_name(std::move(name)), m_notes(std::make_shared<const NoteList>()) {}

    std::string_view name() const noexcept override { return m_name; }
    bool isSource() const noexcept override { return true; }

    /// Control thread: swap in a new set of notes.
    void setNotes(std::shared_ptr<const NoteList> notes) {
        m_notes.publish(std::move(notes));
    }
    std::shared_ptr<const NoteList> notes() const { return m_notes.controlCopy(); }

    /// Control thread: sound a note *now*, outside the timeline — a key on the
    /// piano roll's keyboard, the typing keyboard, later a MIDI controller.
    ///
    /// The event is queued rather than pushed straight at the synth: only the
    /// audio thread may touch a MIDI buffer, and it is the one that decides
    /// which block the note lands in. A single-producer queue is enough because
    /// every caller is the UI thread. Full means the audio thread has stopped
    /// draining (no device), in which case dropping is the only honest answer.
    /// Returns false when the event was dropped.
    bool sendLiveEvent(const MidiEvent& event) noexcept {
        const std::uint32_t write = m_liveWrite.load(std::memory_order_relaxed);
        const std::uint32_t next = (write + 1) % kLiveQueue;
        if (next == m_liveRead.load(std::memory_order_acquire)) return false;
        m_liveEvents[write] = event;
        m_liveWrite.store(next, std::memory_order_release);
        return true;
    }

    bool sendLiveNoteOn(std::uint8_t key, std::uint8_t velocity,
                        std::uint8_t channel = 0) noexcept {
        return sendLiveEvent(MidiEvent::noteOn(0, channel, key, velocity));
    }
    bool sendLiveNoteOff(std::uint8_t key, std::uint8_t channel = 0) noexcept {
        return sendLiveEvent(MidiEvent::noteOff(0, channel, key));
    }

    void prepare(const PrepareInfo&) override {
        // Reserved here so `process` never grows it: 128 simultaneous notes is
        // past any keyboard and any chord, and refusing beyond that is the only
        // realtime-safe answer.
        m_sounding.reserve(kMaxSounding);
        m_sounding.clear();
        m_noteCursor = 0;
        m_hasPosition = false;
    }

    void reset() override {
        m_sounding.clear();
        m_noteCursor = 0;
        m_hasPosition = false;
    }

    void process(const ProcessContext& context) override {
        // A MIDI source writes no audio, but the buffer it was handed is
        // recycled and still holds the previous owner's signal.
        for (ChannelCount ch = 0; ch < context.output.numChannels(); ++ch) {
            dsp::clear(context.output.channel(ch));
        }
        if (!context.midiOutput || context.frames == 0) return;

        // Live keys first, and whatever the transport is doing: playing a note
        // from the keyboard has nothing to do with the playhead, and this is
        // the only path that sounds at all while stopped.
        drainLiveEvents(*context.midiOutput);

        const double tempo = context.transport.tempo > 0.0 ? context.transport.tempo : 120.0;
        const double samplesPerBeat = context.sampleRate * 60.0 / tempo;
        if (!(samplesPerBeat > 0.0)) return;

        if (!context.playing) {
            // Stopped mid-note: release whatever was sounding, or the synth
            // holds it forever. This is why the node tracks what it started.
            // Live notes are not in that list — they end when the key is let
            // go, not when the transport stops.
            releaseAll(*context.midiOutput, 0);
            // The next playing block is a fresh transport start even when the
            // playhead has not moved. It must rebuild/chase active notes instead
            // of continuing with the cursor left behind by the previous run.
            m_hasPosition = false;
            context.midiOutput->sort();
            return;
        }

        const double blockStartBeats = context.transport.ppqPosition;
        const double blockEndBeats =
            blockStartBeats + double(context.frames) / samplesPerBeat;

        auto notes = m_notes.read();
        if (!notes) return;

        const bool snapshotChanged = m_notesFor != notes.get();
        const bool jumped =
            m_hasPosition && std::abs(blockStartBeats - m_expectedBeats) > 1e-6;
        // A locate, loop wrap or edited note set invalidates both the active
        // voices and the monotonic start cursor.
        if (snapshotChanged || jumped || !m_hasPosition) {
            releaseAll(*context.midiOutput, 0);
            m_noteCursor = std::size_t(std::lower_bound(
                notes->begin(), notes->end(), blockStartBeats,
                [](const MidiNote& note, double beat) {
                    return note.startBeats < beat;
                }) - notes->begin());
            m_notesFor = notes.get();

            // MIDI note chase: starting or locating into the held portion of a
            // note must sound immediately. Rebuild only on a discontinuity; the
            // normal per-block path remains cursor-based and O(polyphony).
            for (std::size_t i = 0; i < m_noteCursor; ++i) {
                const MidiNote& note = (*notes)[i];
                const double endBeats = note.startBeats + note.lengthBeats;
                if (endBeats <= blockStartBeats) continue;
                const bool endsThisBlock = endBeats < blockEndBeats;
                if (!endsThisBlock && m_sounding.size() >= kMaxSounding)
                    continue;
                context.midiOutput->push(MidiEvent::noteOn(
                    0, note.channel, note.key, note.velocity));
                if (endsThisBlock) {
                    const auto offOffset = FrameCount(
                        (endBeats - blockStartBeats) * samplesPerBeat);
                    context.midiOutput->push(MidiEvent::noteOff(
                        std::min(offOffset, context.frames - 1), note.channel,
                        note.key));
                } else {
                    m_sounding.push_back(
                        Sounding{note.key, note.channel, endBeats});
                }
            }
        }
        m_expectedBeats = blockEndBeats;
        m_hasPosition = true;

        // End only voices that were active before this block. Note-offs are
        // therefore O(polyphony), not a scan of every note in the project.
        for (std::size_t i = 0; i < m_sounding.size();) {
            const Sounding sounding = m_sounding[i];
            if (sounding.endBeats >= blockStartBeats &&
                sounding.endBeats < blockEndBeats) {
                const auto offset =
                    FrameCount((sounding.endBeats - blockStartBeats) * samplesPerBeat);
                context.midiOutput->push(MidiEvent::noteOff(
                    std::min(offset, context.frames - 1), sounding.channel,
                    sounding.key));
                m_sounding[i] = m_sounding.back();
                m_sounding.pop_back();
            } else {
                ++i;
            }
        }

        // Notes are sorted by start. The cursor advances once over the entire
        // arrangement during normal playback and is repositioned by binary
        // search only after a seek/edit.
        while (m_noteCursor < notes->size() &&
               (*notes)[m_noteCursor].startBeats < blockEndBeats) {
            const MidiNote& note = (*notes)[m_noteCursor++];
            if (note.startBeats < blockStartBeats) continue;
            const double endBeats = note.startBeats + note.lengthBeats;
            const auto onOffset = FrameCount(
                (note.startBeats - blockStartBeats) * samplesPerBeat);
            const bool endsThisBlock = endBeats < blockEndBeats;
            if (!endsThisBlock && m_sounding.size() >= kMaxSounding) continue;

            context.midiOutput->push(MidiEvent::noteOn(
                std::min(onOffset, context.frames - 1), note.channel, note.key,
                note.velocity));
            if (endsThisBlock) {
                const auto offOffset =
                    FrameCount((endBeats - blockStartBeats) * samplesPerBeat);
                context.midiOutput->push(MidiEvent::noteOff(
                    std::min(offOffset, context.frames - 1), note.channel, note.key));
            } else {
                m_sounding.push_back(Sounding{note.key, note.channel, endBeats});
            }
        }
        context.midiOutput->sort();
    }

private:
    static constexpr std::size_t kMaxSounding = 128;

    struct Sounding {
        std::uint8_t key;
        std::uint8_t channel;
        double endBeats;
    };

    /// Audio thread: move everything the UI queued into this block, at offset 0.
    /// The queue holds a handful of key presses, so it empties in one pass.
    void drainLiveEvents(MidiBuffer& out) noexcept {
        std::uint32_t read = m_liveRead.load(std::memory_order_relaxed);
        const std::uint32_t write = m_liveWrite.load(std::memory_order_acquire);
        while (read != write) {
            out.push(m_liveEvents[read]);
            read = (read + 1) % kLiveQueue;
        }
        m_liveRead.store(read, std::memory_order_release);
    }

    void releaseAll(MidiBuffer& out, FrameCount offset) noexcept {
        for (const Sounding& note : m_sounding) {
            out.push(MidiEvent::noteOff(offset, note.channel, note.key));
        }
        m_sounding.clear();
    }

    std::string m_name;
    RealtimeSnapshot<NoteList> m_notes;
    /// What this node has started and not yet ended. A vector, not a set: it
    /// holds a handful of entries and `process` must not allocate, so the
    /// capacity is reserved once and the search is a scan of a few elements.
    std::vector<Sounding> m_sounding;
    std::size_t m_noteCursor = 0;
    const NoteList* m_notesFor = nullptr;
    double m_expectedBeats = 0.0;
    bool m_hasPosition = false;

    /// The live-key queue: fixed storage, one producer (the UI), one consumer
    /// (the audio thread). One slot is always left empty so a full queue is
    /// distinguishable from an empty one without a third counter.
    static constexpr std::uint32_t kLiveQueue = 256;
    std::array<MidiEvent, kLiveQueue> m_liveEvents{};
    std::atomic<std::uint32_t> m_liveWrite{0};
    std::atomic<std::uint32_t> m_liveRead{0};
};

} // namespace daw::engine
