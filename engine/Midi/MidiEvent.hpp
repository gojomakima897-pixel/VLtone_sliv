#pragma once

#include "Common/RealtimeSort.hpp"

#include "Common/Types.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace daw::engine {

/// One MIDI channel-voice message, timed inside a block.
///
/// Three bytes and an offset — the wire format, not a parsed note. Everything
/// the engine routes fits in that: note on/off, controllers, pitch bend,
/// pressure. SysEx does not, and is deliberately out of scope; nothing in this
/// project produces or consumes it, and carrying it would mean variable-length
/// events in a buffer that must not allocate.
struct MidiEvent {
    FrameCount frameOffset = 0;   ///< from the start of the block
    std::uint8_t status = 0;      ///< status byte, channel included
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;

    static constexpr std::uint8_t kNoteOff = 0x80;
    static constexpr std::uint8_t kNoteOn = 0x90;
    static constexpr std::uint8_t kPolyPressure = 0xA0;
    static constexpr std::uint8_t kControlChange = 0xB0;
    static constexpr std::uint8_t kProgramChange = 0xC0;
    static constexpr std::uint8_t kChannelPressure = 0xD0;
    static constexpr std::uint8_t kPitchBend = 0xE0;

    std::uint8_t type() const noexcept { return std::uint8_t(status & 0xF0); }
    std::uint8_t channel() const noexcept { return std::uint8_t(status & 0x0F); }

    /// A note-on with zero velocity is a note-off — a convention old enough
    /// that plenty of real files and controllers still rely on it, and one
    /// that a synth reading only the status byte would get wrong.
    bool isNoteOn() const noexcept { return type() == kNoteOn && data2 > 0; }
    bool isNoteOff() const noexcept {
        return type() == kNoteOff || (type() == kNoteOn && data2 == 0);
    }

    static MidiEvent noteOn(FrameCount offset, std::uint8_t channel, std::uint8_t key,
                            std::uint8_t velocity) noexcept {
        return {offset, std::uint8_t(kNoteOn | (channel & 0x0F)), key, velocity};
    }
    static MidiEvent noteOff(FrameCount offset, std::uint8_t channel,
                             std::uint8_t key) noexcept {
        return {offset, std::uint8_t(kNoteOff | (channel & 0x0F)), key, 0};
    }
};

/// How many events one node can emit or receive in a block.
///
/// 512 is far past what music produces — a dense 32nd-note chord at 200 BPM is
/// a handful per block — and generous enough that hitting it means something is
/// wrong rather than merely busy.
inline constexpr std::size_t kMidiEventsPerBlock = 512;

/// One block's worth of MIDI, with a fixed capacity.
///
/// `push` refuses rather than growing: this is written from `process`, and a
/// reallocation there is a dropout. The capacity is set at prepare time and is
/// generous enough that refusing means something is genuinely wrong (a stuck
/// controller flooding, a clip with thousands of notes on one beat), in which
/// case dropping the overflow is the only realtime-safe answer.
class MidiBuffer {
public:
    void reserve(std::size_t capacity) { m_events.reserve(capacity); }
    void clear() noexcept { m_events.clear(); }

    bool push(const MidiEvent& event) noexcept {
        if (m_events.size() >= m_events.capacity()) return false;
        m_events.push_back(event);
        return true;
    }

    /// Stable, so two events at the same frame keep the order they were added —
    /// which is what makes a note-off before a note-on at the same offset stay
    /// that way, instead of cutting the note it was meant to precede.
    void sort() noexcept {
        stableRealtimeSort(m_events.begin(), m_events.end(),
                           [](const MidiEvent& a, const MidiEvent& b) {
                               return a.frameOffset < b.frameOffset;
                           });
    }

    std::span<const MidiEvent> events() const noexcept { return m_events; }
    bool empty() const noexcept { return m_events.empty(); }
    std::size_t size() const noexcept { return m_events.size(); }

private:
    std::vector<MidiEvent> m_events;
};

/// Delays MIDI by the same number of samples as the audio on the same edge.
///
/// Without this, latency compensation would move the audio and leave the notes
/// where they were: an instrument behind a compensated effect would be played
/// early by exactly that effect's latency. Events that fall past the end of the
/// block are held and emitted by a later one, which is why this has state.
class MidiDelay {
public:
    void prepare(FrameCount delaySamples, std::size_t capacity) {
        m_delay = delaySamples;
        m_pending.reserve(capacity);
        m_pending.clear();
    }
    void reset() noexcept { m_pending.clear(); }
    FrameCount delaySamples() const noexcept { return m_delay; }

    void process(const MidiBuffer& input, MidiBuffer& output, FrameCount frames) noexcept {
        output.clear();

        // What was held back from earlier blocks comes first, and it is already
        // in this block's timebase.
        std::size_t keep = 0;
        for (MidiEvent event : m_pending) {
            if (event.frameOffset < frames) {
                output.push(event);
            } else {
                event.frameOffset -= frames;
                m_pending[keep++] = event;
            }
        }
        m_pending.resize(keep);

        for (MidiEvent event : input.events()) {
            const FrameCount shifted = event.frameOffset + m_delay;
            event.frameOffset = shifted;
            if (shifted < frames) {
                output.push(event);
            } else if (m_pending.size() < m_pending.capacity()) {
                event.frameOffset = shifted - frames;
                m_pending.push_back(event);
            }
        }
        output.sort();
    }

private:
    std::vector<MidiEvent> m_pending;
    FrameCount m_delay = 0;
};

} // namespace daw::engine
