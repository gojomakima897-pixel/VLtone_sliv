// MIDI through the engine, and from a clip into an instrument.
//
// The fixture's instrument half is the crudest synth that can be measured: it
// writes the held note's velocity as a DC level and silence otherwise. A host
// that loses a note-on writes silence, one that loses a note-off writes
// forever, and one that mistimes either puts the edge at the wrong sample — all
// three visible in one array of floats.
#include "Clap/ClapFactory.hpp"
#include "Graph/AudioGraph.hpp"
#include "Graph/GraphProcessor.hpp"
#include "Host/PluginNode.hpp"
#include "Midi/MidiEvent.hpp"
#include "Nodes/BasicNodes.hpp"
#include "Nodes/MidiClipPlayerNode.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace daw;
using namespace daw::plugins;
using namespace daw::engine;

static int failures = 0;
static void check(bool condition, const char* what) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++failures;
}

namespace {

constexpr FrameCount kBlock = 128;

struct OutputBuffer {
    OutputBuffer(ChannelCount channels, FrameCount frames)
        : storage(std::size_t(channels) * frames, 0.0f), pointers(channels),
          channels(channels), frames(frames) {
        for (ChannelCount ch = 0; ch < channels; ++ch) {
            pointers[ch] = storage.data() + std::size_t(ch) * frames;
        }
    }
    AudioBlock block() { return AudioBlock(pointers.data(), channels, frames); }
    std::vector<float> storage;
    std::vector<float*> pointers;
    ChannelCount channels;
    FrameCount frames;
};

/// Records the MIDI it was handed, so a routing assertion can look at it.
class RecorderNode : public Node {
public:
    std::string_view name() const noexcept override { return "recorder"; }
    FrameCount latencySamples() const noexcept override { return m_latency; }
    void setLatency(FrameCount latency) { m_latency = latency; }

    void process(const ProcessContext& context) override {
        for (ChannelCount ch = 0; ch < context.output.numChannels(); ++ch) {
            std::fill_n(context.output.data(ch), context.frames, 0.0f);
        }
        received.clear();
        for (const MidiBuffer* buffer : context.midiInputs) {
            if (!buffer) continue;
            for (const MidiEvent& event : buffer->events()) received.push_back(event);
        }
    }

    std::vector<MidiEvent> received;

private:
    FrameCount m_latency = 0;
};

PrepareInfo makeInfo() {
    PrepareInfo info;
    info.sampleRate = 48000.0;
    info.maxBlockSize = kBlock;
    info.channels = 2;
    return info;
}

/// 120 BPM at 48 kHz: one beat is 24000 samples, so a block of 128 is a small
/// fraction of a beat and note edges land well inside it.
TransportInfo transportAt(double beats) {
    TransportInfo transport;
    transport.tempo = 120.0;
    transport.ppqPosition = beats;
    return transport;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const PrepareInfo info = makeInfo();

    // ── The buffer ──
    {
        MidiBuffer buffer;
        buffer.reserve(4);
        check(buffer.push(MidiEvent::noteOn(10, 0, 60, 100)), "an event goes in");
        check(buffer.push(MidiEvent::noteOff(5, 0, 60)), "and another");
        buffer.sort();
        check(buffer.events()[0].frameOffset == 5 && buffer.events()[1].frameOffset == 10,
              "sorting puts them in time order");

        buffer.push(MidiEvent::noteOn(1, 0, 61, 100));
        buffer.push(MidiEvent::noteOn(2, 0, 62, 100));
        // Full: refusing is the only realtime-safe answer, because growing
        // would allocate on the audio thread.
        check(!buffer.push(MidiEvent::noteOn(3, 0, 63, 100)),
              "a full buffer refuses rather than reallocating");
        check(buffer.size() == 4, "and keeps what it had");

        const MidiEvent zeroVelocity{0, 0x90, 60, 0};
        check(zeroVelocity.isNoteOff() && !zeroVelocity.isNoteOn(),
              "a note-on with zero velocity is a note-off");
    }

    // ── The delay ──
    {
        MidiDelay delay;
        delay.prepare(64, 16);
        MidiBuffer in;
        in.reserve(8);
        MidiBuffer out;
        out.reserve(8);

        in.push(MidiEvent::noteOn(10, 0, 60, 100));
        delay.process(in, out, kBlock);
        check(out.size() == 1 && out.events()[0].frameOffset == 74,
              "an event inside the block is shifted by the delay");

        // Past the end of the block: held, and emitted by the next one at the
        // offset that keeps its distance from the note it followed.
        in.clear();
        in.push(MidiEvent::noteOn(100, 0, 62, 100));
        delay.process(in, out, kBlock);
        check(out.empty(), "an event pushed past the block end is held back");
        in.clear();
        delay.process(in, out, kBlock);
        check(out.size() == 1 && out.events()[0].frameOffset == 36,
              "and arrives in the next block at the right offset");
    }

    // ── The clip player ──
    {
        auto player = std::make_shared<MidiClipPlayerNode>();
        auto notes = std::make_shared<MidiClipPlayerNode::NoteList>();
        // 24000 samples per beat: a note at beat 0.001 starts at sample 24.
        notes->push_back(MidiNote{0.001, 0.002, 64, 99, 0});
        player->setNotes(notes);

        AudioGraph graph;
        auto recorder = std::make_shared<RecorderNode>();
        const NodeId playerId = graph.adoptNode(player);
        const NodeId recorderId = graph.adoptNode(recorder);
        graph.connect(playerId, recorderId);
        graph.setSink(recorderId);
        auto compiled = graph.compile(info);
        check(compiled.has_value(), "a graph with a MIDI source compiles");

        GraphProcessor processor(4);
        processor.setGraph(*compiled);
        OutputBuffer output(2, kBlock);

        processor.process(output.block(), kBlock, 0, true, true, transportAt(0.0));
        check(recorder->received.size() == 2, "the note's on and off both arrive");
        if (recorder->received.size() == 2) {
            check(recorder->received[0].isNoteOn() &&
                      recorder->received[0].frameOffset == 24,
                  "the note-on lands on the sample the beat falls on");
            check(recorder->received[1].isNoteOff() &&
                      recorder->received[1].frameOffset == 72,
                  "and the note-off at the end of its length");
        }

        // A note still sounding when the transport stops has to be released, or
        // the synth holds it forever — the one MIDI bug every user notices.
        auto held = std::make_shared<MidiClipPlayerNode::NoteList>();
        held->push_back(MidiNote{0.0, 100.0, 64, 99, 0});   // far longer than a block
        player->setNotes(held);
        processor.process(output.block(), kBlock, 0, true, true, transportAt(0.0));
        check(recorder->received.size() == 1 && recorder->received[0].isNoteOn(),
              "a long note starts");
        processor.process(output.block(), kBlock, 0, false, true, transportAt(0.001));
        check(recorder->received.size() == 1 && recorder->received[0].isNoteOff(),
              "and is released when the transport stops, not left hanging");

        // Same for a locate: what was sounding belonged to where the playhead
        // was, not to where it has jumped to.
        processor.process(output.block(), kBlock, 0, true, true, transportAt(0.0));
        check(!recorder->received.empty() && recorder->received[0].isNoteOn(),
              "it starts again from the top");
        processor.process(output.block(), kBlock, 0, true, true, transportAt(50.0));
        bool sawOff = false;
        bool sawChasedOn = false;
        for (const MidiEvent& event : recorder->received) {
            if (event.isNoteOff()) sawOff = true;
            if (event.isNoteOn() && event.frameOffset == 0)
                sawChasedOn = true;
        }
        check(sawOff, "a jump in the playhead releases what was sounding");
        check(sawChasedOn,
              "starting inside a held note chases it from the new playhead");

        processor.process(output.block(), kBlock, 0, false, true,
                          transportAt(50.0));
        check(recorder->received.size() == 1 &&
                  recorder->received[0].isNoteOff(),
              "a chased note is released when playback stops");

        // ── Live keys ──
        //
        // A key pressed on the piano roll's keyboard or the typing keyboard has
        // nothing to do with the playhead: it must sound with the transport
        // stopped, and it must not be swept up by the release that a stop does
        // to the clip's own notes.
        player->setNotes(std::make_shared<MidiClipPlayerNode::NoteList>());
        processor.process(output.block(), kBlock, 0, false, true, transportAt(0.0));
        check(recorder->received.empty(), "nothing sounds by itself when stopped");

        check(player->sendLiveNoteOn(67, 100), "a live note is queued");
        processor.process(output.block(), kBlock, 0, false, true, transportAt(0.0));
        check(recorder->received.size() == 1 && recorder->received[0].isNoteOn() &&
                  recorder->received[0].data1 == 67,
              "and reaches the instrument with the transport stopped");

        processor.process(output.block(), kBlock, 0, false, true, transportAt(0.0));
        check(recorder->received.empty(),
              "a stopped transport does not release a live note");

        check(player->sendLiveNoteOff(67), "the key comes up");
        processor.process(output.block(), kBlock, 0, false, true, transportAt(0.0));
        check(recorder->received.size() == 1 && recorder->received[0].isNoteOff() &&
                  recorder->received[0].data1 == 67,
              "and the note ends");

        // The queue is bounded and must refuse rather than overwrite: an audio
        // thread that has stopped draining (no device) must not corrupt what is
        // already in flight.
        int queued = 0;
        while (player->sendLiveNoteOn(60, 100)) ++queued;
        check(queued > 0 && queued < 512, "the live queue fills and then refuses");
        processor.process(output.block(), kBlock, 0, false, true, transportAt(0.0));
        check(int(recorder->received.size()) == queued,
              "and every queued event arrives, once");
    }

    // ── MIDI is compensated by the same amount as the audio ──
    //
    // Without this the notes stay where they were while the audio moves, and an
    // instrument behind a compensated effect plays early by exactly that
    // effect's latency.
    {
        AudioGraph graph;
        auto player = std::make_shared<MidiClipPlayerNode>();
        auto notes = std::make_shared<MidiClipPlayerNode::NoteList>();
        notes->push_back(MidiNote{0.0, 0.002, 64, 99, 0});
        player->setNotes(notes);

        auto recorder = std::make_shared<RecorderNode>();
        const NodeId playerId = graph.adoptNode(player);
        const NodeId recorderId = graph.adoptNode(recorder);
        // A second path into the recorder, through a latency, forces the
        // compiler to delay the direct one to match.
        const NodeId slow = graph.addNode(std::make_unique<LatencyNode>("slow", 32));
        graph.connect(playerId, recorderId);
        graph.connect(playerId, slow);
        graph.connect(slow, recorderId);
        graph.setSink(recorderId);

        auto compiled = graph.compile(info);
        check(compiled.has_value() && !(*compiled)->delays.empty(),
              "the early path gets a compensation delay");
        check((*compiled)->midiDelays.size() == (*compiled)->delays.size(),
              "and a MIDI delay to go with it");

        GraphProcessor processor(4);
        processor.setGraph(*compiled);
        OutputBuffer output(2, kBlock);
        processor.process(output.block(), kBlock, 0, true, true, transportAt(0.0));

        // The note leaves the player at offset 0 and arrives at 32, because the
        // direct edge was the early one and got the compensating delay.
        //
        // It arrives exactly once, not twice: MIDI is *not* passed through a
        // node automatically. `LatencyNode` writes audio and nothing else, so
        // the second path carries no notes. That is the rule — a node forwards
        // MIDI only if it chooses to — and it is what makes an instrument in an
        // insert chain the thing that consumes the notes rather than the first
        // effect that happens to be in front of it.
        std::size_t noteOns = 0;
        FrameCount arrivedAt = 0;
        for (const MidiEvent& event : recorder->received) {
            if (!event.isNoteOn()) continue;
            ++noteOns;
            arrivedAt = event.frameOffset;
        }
        check(noteOns == 1, "a node that writes no MIDI does not forward any either");
        check(arrivedAt == 32,
              "and the compensated edge shifts its notes by the same 32 samples as its audio");
    }

    // ── Audio effects do not swallow MIDI on the way to a later instrument ──
    {
        ClapFactory factory;
        const std::vector<PluginDescriptor> found = factory.inspect(DAW_TEST_CLAP_PATH);
        const PluginDescriptor* effect = nullptr;
        for (const PluginDescriptor& candidate : found) {
            if (!candidate.isInstrument) effect = &candidate;
        }
        check(effect != nullptr, "the fixture has an audio effect for MIDI pass-through");
        if (effect) {
            auto player = std::make_shared<MidiClipPlayerNode>();
            auto notes = std::make_shared<MidiClipPlayerNode::NoteList>();
            notes->push_back(MidiNote{0.0, 0.25, 60, 100, 0});
            player->setNotes(notes);
            auto fx = std::make_shared<PluginNode>("audio fx", factory.create(*effect));
            auto recorder = std::make_shared<RecorderNode>();

            AudioGraph graph;
            const NodeId playerId = graph.adoptNode(player);
            const NodeId fxId = graph.adoptNode(fx);
            const NodeId recorderId = graph.adoptNode(recorder);
            graph.connect(playerId, fxId);
            graph.connect(fxId, recorderId);
            graph.setSink(recorderId);
            auto compiled = graph.compile(info);
            check(compiled.has_value(), "a MIDI clip → audio effect chain compiles");
            if (compiled) {
                GraphProcessor processor(2);
                processor.setGraph(*compiled);
                OutputBuffer output(2, kBlock);
                processor.process(output.block(), kBlock, 0, true, true,
                                  transportAt(0.0));
                bool received = false;
                for (const MidiEvent& event : recorder->received) {
                    if (event.isNoteOn()) received = true;
                }
                check(received,
                      "an audio-only plugin passes MIDI to a downstream VSTi");
            }
        }
    }

    // ── A clip into an instrument, end to end ──
    {
        ClapFactory factory;
        const std::vector<PluginDescriptor> found = factory.inspect(DAW_TEST_CLAP_PATH);
        const PluginDescriptor* tone = nullptr;
        for (const PluginDescriptor& candidate : found) {
            if (candidate.isInstrument) tone = &candidate;
        }
        check(tone != nullptr, "the fixture's instrument is scanned as one");
        if (!tone) {
            std::printf("\nFAILURES PRESENT\n");
            return 1;
        }

        auto instance = factory.create(*tone);
        check(instance != nullptr, "the instrument instantiates");
        if (!instance) {
            std::printf("\nFAILURES PRESENT\n");
            return 1;
        }
        check(instance->descriptor().mainInputChannels == 0,
              "the live instance reports no audio input, so the graph treats it as a source");

        auto synth = std::make_shared<PluginNode>("tone", std::move(instance));
        auto player = std::make_shared<MidiClipPlayerNode>();
        auto notes = std::make_shared<MidiClipPlayerNode::NoteList>();
        // Starts at the top of the block, lasts well past its end.
        notes->push_back(MidiNote{0.0, 1.0, 64, 127, 0});
        player->setNotes(notes);

        AudioGraph graph;
        const NodeId playerId = graph.adoptNode(player);
        const NodeId synthId = graph.adoptNode(synth);
        graph.connect(playerId, synthId);
        graph.setSink(synthId);
        auto compiled = graph.compile(info);
        check(compiled.has_value(), "a clip-into-instrument graph compiles");

        GraphProcessor processor(4);
        processor.setGraph(*compiled);
        OutputBuffer output(2, kBlock);
        for (float& sample : output.storage) sample = -5.0f;   // stale contents
        processor.process(output.block(), kBlock, 0, true, true, transportAt(0.0));

        // Velocity 127 arrives at the plugin as 1.0, and the fixture holds it
        // as a DC level for as long as the note is on.
        check(std::fabs(output.storage[kBlock - 1] - 1.0f) < 1e-3f,
              "a note in a clip makes the instrument sound");
        check(std::fabs(output.storage[0] - 1.0f) < 1e-3f,
              "from the first sample of the block the note starts on");

        // And it stops when the note does.
        auto shortNote = std::make_shared<MidiClipPlayerNode::NoteList>();
        shortNote->push_back(MidiNote{0.0, 0.001, 64, 127, 0});   // ends at sample 24
        player->setNotes(shortNote);
        processor.process(output.block(), kBlock, 0, true, true, transportAt(0.0));
        check(std::fabs(output.storage[kBlock - 1]) < 1e-3f,
              "and falls silent again when the note ends");
    }

    // ── Automation drives a plugin parameter ──
    //
    // The curve is evaluated on the audio thread, against this block's
    // playhead, rather than pushed from the control thread — a curve is a
    // function of the playhead, and only `process` knows it to the sample.
    // Pushing it through the event ring would quantise every automated
    // parameter to the UI's 33 ms tick.
    {
        ClapFactory factory;
        const std::vector<PluginDescriptor> found = factory.inspect(DAW_TEST_CLAP_PATH);
        const PluginDescriptor* gain = nullptr;
        for (const PluginDescriptor& candidate : found) {
            if (!candidate.isInstrument) gain = &candidate;
        }
        check(gain != nullptr, "the fixture's effect is there to automate");
        if (!gain) {
            std::printf("\nFAILURES PRESENT\n");
            return 1;
        }

        auto instance = factory.create(*gain);
        check(instance != nullptr, "it instantiates");
        if (!instance) {
            std::printf("\nFAILURES PRESENT\n");
            return 1;
        }
        // Parameter 0 is Gain, 0…2. The plugin has 64 samples of latency, so
        // what comes out at sample i was multiplied by the gain in force at
        // i - 64 — which is exactly what makes the timing assertion below mean
        // something.
        auto node = std::make_shared<PluginNode>("gain", std::move(instance));

        auto curves = std::make_shared<PluginNode::AutomationCurves>();
        PluginNode::AutomationCurve curve;
        curve.parameterIndex = 0;
        curve.defaultValue = 1.0;
        // A step, which needs three points: two at the same level make the
        // segment flat, and the third is the jump. Two points alone would be a
        // ramp — the curve is interpolated, not held.
        curve.points.emplace_back(0.0, 0.25);
        curve.points.emplace_back(0.999, 0.25);
        curve.points.emplace_back(1.0, 1.0);
        curves->push_back(curve);
        node->setAutomation(curves);

        AudioGraph graph;
        const NodeId sourceId = graph.addNode(std::make_unique<SourceNode>(
            "src",
            [](void*, const AudioBlock& output, FrameCount frames, SamplePos) {
                for (ChannelCount ch = 0; ch < output.numChannels(); ++ch) {
                    float* data = output.data(ch);
                    for (FrameCount i = 0; i < frames; ++i) data[i] = 1.0f;
                }
            },
            nullptr));
        const NodeId gainId = graph.adoptNode(node);
        graph.connect(sourceId, gainId);
        graph.setSink(gainId);
        auto compiled = graph.compile(info);
        check(compiled.has_value(), "an automated plugin compiles into the graph");

        GraphProcessor processor(4);
        processor.setGraph(*compiled);
        OutputBuffer output(2, kBlock);

        // Two blocks: the first fills the plugin's 64-sample delay line, the
        // second carries signal that has been through the gain.
        processor.process(output.block(), kBlock, 0, true, true, transportAt(0.0));
        processor.process(output.block(), kBlock, kBlock, true, true,
                          transportAt(double(kBlock) / 24000.0));
        check(std::fabs(output.storage[kBlock - 1] - 0.25f) < 1e-3f,
              "the curve's value at the playhead is what the plugin applies");

        // Beat 1 is sample 24000. Run up to it, then straddle it: the step has
        // to be applied *inside* the block it falls in, not deferred to the
        // next one. The plugin's 64 samples of latency mean the change shows up
        // 64 samples later in the output than in the gain, which is why both
        // values have to appear across two blocks rather than one.
        const SamplePos across = 24000 - 32;
        for (SamplePos at = 0; at < across; at += kBlock) {
            processor.process(output.block(), kBlock, at, true, true,
                              transportAt(double(at) / 24000.0));
        }
        bool sawLow = false;
        bool sawHigh = false;
        for (SamplePos at = across; at < across + 3 * kBlock; at += kBlock) {
            processor.process(output.block(), kBlock, at, true, true,
                              transportAt(double(at) / 24000.0));
            for (FrameCount i = 0; i < kBlock; ++i) {
                if (std::fabs(output.storage[i] - 0.25f) < 1e-3f) sawLow = true;
                if (std::fabs(output.storage[i] - 1.0f) < 1e-3f) sawHigh = true;
            }
        }
        check(sawLow && sawHigh,
              "a breakpoint inside a block is applied inside that block, not at its edge");

        // A curve with no points holds its default rather than jumping to zero.
        auto flat = std::make_shared<PluginNode::AutomationCurves>();
        PluginNode::AutomationCurve empty;
        empty.parameterIndex = 0;
        empty.defaultValue = 2.0;
        flat->push_back(empty);
        node->setAutomation(flat);
        processor.process(output.block(), kBlock, 0, true, true, transportAt(0.0));
        processor.process(output.block(), kBlock, kBlock, true, true,
                          transportAt(double(kBlock) / 24000.0));
        check(std::fabs(output.storage[kBlock - 1] - 2.0f) < 1e-3f,
              "a curve with no breakpoints holds its default value");
    }

    std::printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures;
}
