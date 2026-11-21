// Phase-2 verification: the headless controller round-trip — build a project,
// import a clip, export a mixdown, save, and reload — with NO audio device
// (initialize(openDevice=false)), so it runs anywhere.
#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "SettingsStore.hpp"
#include "platform/AudioFileDecoder.hpp"
#include "Recording/RecordingEngine.hpp"
#include "Core/AudioBuffer.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;
using namespace audio;

static int failures = 0;
// Returns the verdict, so a test that only makes sense once something
// succeeded can guard its follow-up checks on it.
static bool check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++failures;
    return cond;
}

// Write a stereo tone WAV the controller can import.
static void writeTone(const std::string& path, SampleRate rate,
                      BufferSize frames) {
    AudioBuffer tone(2, frames);
    for (BufferSize f = 0; f < frames; ++f) {
        const float s = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * f / rate);
        tone.getChannel(0)[f] = s;
        tone.getChannel(1)[f] = s;
    }
    AudioRecorder rec;
    rec.initialize(rate, 2);
    rec.writeWAVFile(path, tone, rate);
}

// Write a genuinely single-channel WAV — what a mono input records. Not the
// same thing as a stereo file with one silent side: this one has no second
// channel at all, and everything downstream has to make one.
static void writeMonoTone(const std::string& path, SampleRate rate,
                          BufferSize frames) {
    AudioBuffer tone(1, frames);
    for (BufferSize f = 0; f < frames; ++f) {
        tone.getChannel(0)[f] =
            0.5f * std::sin(2.0f * 3.14159265f * 440.0f * f / rate);
    }
    AudioRecorder rec;
    rec.initialize(rate, 1);
    rec.writeWAVFile(path, tone, rate);
}

// Write a stereo tone with signal only on the LEFT channel, so a mono fold is
// observable (it spreads the left signal across both channels).
static void writeLeftOnlyTone(const std::string& path, SampleRate rate,
                              BufferSize frames) {
    AudioBuffer tone(2, frames);
    for (BufferSize f = 0; f < frames; ++f) {
        const float s = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * f / rate);
        tone.getChannel(0)[f] = s;
        tone.getChannel(1)[f] = 0.0f;
    }
    AudioRecorder rec;
    rec.initialize(rate, 2);
    rec.writeWAVFile(path, tone, rate);
}

// Peak of one channel of a decoded interleaved buffer.
static float channelPeak(const audio::platform::DecodedAudio& a, int channel) {
    float peak = 0.0f;
    if (a.channels <= channel) return peak;
    for (size_t f = 0; f < a.frames; ++f) {
        peak = std::max(peak, std::fabs(a.interleaved[f * a.channels + channel]));
    }
    return peak;
}

static const daw::ClipModel* findClip(const daw::EngineController& c,
                                       const std::string& trackId,
                                       const std::string& clipId) {
    const auto* t = c.project().findTrack(trackId);
    if (!t) return nullptr;
    for (const auto& clip : t->clips) {
        if (clip.id == clipId) return &clip;
    }
    return nullptr;
}

int main() {
    const auto dir = fs::temp_directory_path() / "daw-controller-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string tonePath = (dir / "tone.wav").string();
    writeTone(tonePath, 48000, 24000); // 0.5 s

    daw::EngineController ctrl;
    check(ctrl.initialize(48000, 512, /*openDevice=*/false).isOk(),
          "controller initialises offline");

    // ── Tracks ──
    const std::string t1 = ctrl.addTrack(daw::TrackKind::Audio, "Guitar");
    check(!t1.empty() && ctrl.project().tracks.size() == 1,
          "adds an audio track");
    check(ctrl.trackNodes(t1) != nullptr &&
              ctrl.trackNodes(t1)->fader != daw::engine::kInvalidNode,
          "track is realised as engine nodes");

    // ── Import a clip at 1.0 s ──
    const std::string c1 = ctrl.importAudio(tonePath, t1, 1.0);
    check(!c1.empty(), "imports an audio clip");
    const auto& track = ctrl.project().tracks[0];
    check(track.clips.size() == 1, "clip is attached to the track");
    check(std::fabs(track.clips[0].durationSeconds - 0.5) < 0.01,
          "clip duration is derived from the file");
    check(std::fabs(ctrl.durationSeconds() - 1.5) < 0.01,
          "timeline duration spans start + clip length");

    // A downloaded file imported onto a new track is one user action: the
    // track and clip disappear together and return together. Audio must never
    // be attached to a lane whose model cannot represent an audio clip.
    {
        daw::EngineController imported;
        imported.initialize(48000, 512, /*openDevice=*/false);
        const std::size_t before = imported.undoDepth();
        const std::string newTrack =
            imported.importAudioToNewTrack(tonePath, 2.0, "Downloaded Tone");
        const auto* model = imported.project().findTrack(newTrack);
        check(model && model->kind == daw::TrackKind::Audio &&
                  model->name == "Downloaded Tone" && model->clips.size() == 1 &&
                  std::fabs(model->clips.front().startSeconds - 2.0) < 1e-9,
              "downloaded audio creates a named audio track at the playhead");
        check(imported.undoDepth() == before + 1,
              "new-track audio import records one undo entry");
        imported.undo();
        check(imported.project().findTrack(newTrack) == nullptr,
              "one undo removes the downloaded track and clip");
        imported.redo();
        check(imported.project().findTrack(newTrack) != nullptr &&
                  imported.project().findTrack(newTrack)->clips.size() == 1,
              "one redo restores the downloaded track and clip");

        const std::size_t failedDepth = imported.undoDepth();
        const std::size_t failedTracks = imported.project().tracks.size();
        check(imported.importAudioToNewTrack(
                  (dir / "missing-download.wav").string(), 0.0)
                  .empty(),
              "failed new-track import reports failure");
        check(imported.project().tracks.size() == failedTracks &&
                  imported.undoDepth() == failedDepth,
              "failed new-track import leaves no track or undo entry");

        const std::string midi =
            imported.addTrack(daw::TrackKind::Instrument, "Instrument");
        check(imported.importAudio(tonePath, midi, 0.0).empty() &&
                  imported.project().findTrack(midi)->clips.empty(),
              "audio import refuses a non-audio track");
    }

    // ── Undo / redo ──
    ctrl.setTrackVolume(t1, 0.25f);
    check(std::fabs(ctrl.project().tracks[0].volume - 0.25f) < 1e-6f,
          "volume change applies");
    ctrl.undo();
    check(std::fabs(ctrl.project().tracks[0].volume - 1.0f) < 1e-6f,
          "undo restores previous volume");
    ctrl.redo();
    check(std::fabs(ctrl.project().tracks[0].volume - 0.25f) < 1e-6f,
          "redo re-applies volume");

    // ── Export mixdown ──
    const std::string exportPath = (dir / "mix.wav").string();
    check(ctrl.exportMixdown(exportPath, false).isOk(), "exports a mixdown");
    check(fs::exists(exportPath), "export file exists");
    daw::ProjectModel dummy;
    platform::DecodedAudio decoded;
    check(platform::decodeAudioFile(exportPath, decoded).isOk(),
          "exported file decodes");
    check(decoded.frames > 60000, "exported mixdown spans the full timeline");
    // The first second (before the clip) is silence; after 1.0 s there is tone.
    if (decoded.frames > 60000 && decoded.channels >= 1) {
        const size_t at0_1s = static_cast<size_t>(0.1 * decoded.sampleRate);
        const size_t at1_2s = static_cast<size_t>(1.2 * decoded.sampleRate);
        const float early = std::fabs(
            decoded.interleaved[at0_1s * decoded.channels]);
        float latePeak = 0.0f;
        for (size_t f = at1_2s; f < at1_2s + 200 && f < decoded.frames; ++f) {
            latePeak = std::max(latePeak,
                std::fabs(decoded.interleaved[f * decoded.channels]));
        }
        check(early < 0.01f, "mixdown is silent before the clip");
        check(latePeak > 0.05f, "mixdown carries the clip audio after 1s");
    }

    // ── Save + reload ──
    const std::string pkg = (dir / "song.vlt").string();
    check(ctrl.saveProject(pkg).isOk(), "saves the project package");
    check(fs::exists(fs::path(pkg) / "Project.vlt"), "Project.vlt written");
    check(fs::exists(fs::path(pkg) / "Content" / "tone.wav"),
          "referenced audio copied into Content/");
    check(fs::is_directory(fs::path(pkg) / "State"),
          "VLT package includes the plugin State folder");

    daw::EngineController ctrl2;
    ctrl2.initialize(48000, 512, false);
    check(ctrl2.openProject(pkg).isOk(), "reopens the saved project");
    check(ctrl2.project().tracks.size() == 1, "track survives the round-trip");
    check(ctrl2.project().tracks.size() == 1 &&
              ctrl2.project().tracks[0].clips.size() == 1,
          "clip survives the round-trip");
    check(ctrl2.project().name == ctrl.project().name,
          "project name round-trips");
    check(std::fabs(ctrl2.durationSeconds() - 1.5) < 0.01,
          "reloaded timeline duration matches");
    check(ctrl2.trackNodes(ctrl2.project().tracks[0].id) != nullptr,
          "reloaded track is re-realised in the engine");

    // ── Pattern container ──
    // A Pattern is both a compact arrangement parent and a summing bus. Its
    // sources remain ordinary instrument tracks, which is what gives each one
    // an independent mixer channel and processing chain.
    {
        daw::EngineController p;
        p.initialize(48000, 512, false);
        const std::string pattern = p.addPattern("Beat");
        const auto* container = p.project().findTrack(pattern);
        check(container && container->kind == daw::TrackKind::Pattern &&
                  daw::isFolder(*container) && daw::carriesAudio(*container),
              "a Pattern is a serialisable summing container");

        const std::string child = p.addPatternSample(pattern, tonePath, 1.0);
        const auto* source = p.project().findTrack(child);
        check(source && source->parentId == pattern &&
                  source->kind == daw::TrackKind::Instrument &&
                  source->instrument.uid == "daw.sampler",
              "dropping a sample creates a child Sampler instrument");
        check(source && source->name == "tone" && source->clips.size() == 1 &&
                  source->clips.front().kind == daw::ClipKind::Midi &&
                  std::fabs(source->clips.front().startSeconds - 1.0) < 1e-9,
              "the Sampler lane uses the file name and starts with a MIDI clip");
        check(source && daw::summingParent(p.project(), child) == pattern &&
                  p.trackNodes(child) != nullptr,
              "the child keeps its own channel and routes into the Pattern bus");

        const std::string originalSamplerSlot =
            source ? source->instrument.id : std::string{};
        const std::string patternCopy = p.duplicatePattern(pattern);
        const daw::TrackModel* copiedPattern =
            p.project().findTrack(patternCopy);
        const daw::TrackModel* copiedSource = nullptr;
        for (const auto& track : p.project().tracks) {
            if (track.parentId == patternCopy) {
                copiedSource = &track;
                break;
            }
        }
        check(copiedPattern && copiedPattern->name == "Beat copy" &&
                  copiedSource && copiedSource->name == "tone" &&
                  copiedSource->instrument.uid == "daw.sampler" &&
                  copiedSource->instrument.id != originalSamplerSlot,
              "duplicating a Pattern clones its sources and plugin slots");
        auto* copiedSampler = copiedSource
                                  ? p.samplerInstance(
                                        copiedSource->id,
                                        copiedSource->instrument.id)
                                  : nullptr;
        check(copiedSampler && copiedSampler->samplePath() == tonePath &&
                  daw::summingParent(p.project(), copiedSource->id) == patternCopy,
              "a duplicated Pattern keeps the loaded sample and reroutes it");
        check(p.undoLabel() == "Duplicate Pattern",
              "Pattern duplication is one undoable action");
        p.undo();
        check(!p.project().findTrack(patternCopy) &&
                  p.project().tracks.size() == 2,
              "undo removes the duplicated Pattern and all of its sources");

        p.setFolderExpanded(pattern, false);
        const auto& collapsed = daw::visibleTracks(p.project());
        check(collapsed.size() == 1 &&
                  p.project().tracks[collapsed.front().index].id == pattern,
              "collapsing a Pattern hides its source lanes");

        const std::string patternPkg = (dir / "pattern.vlt").string();
        check(p.saveProject(patternPkg).isOk(), "a Pattern project saves");
        daw::EngineController reopened;
        reopened.initialize(48000, 512, false);
        check(reopened.openProject(patternPkg).isOk(), "a Pattern project reopens");
        const auto* restored = reopened.project().findTrack(pattern);
        const auto* restoredChild = reopened.project().findTrack(child);
        check(restored && restored->kind == daw::TrackKind::Pattern &&
                  !restored->expanded && restoredChild &&
                  restoredChild->parentId == pattern &&
                  !restored->clips.empty() &&
                  restored->clips.front().kind == daw::ClipKind::Pattern &&
                  !restoredChild->clips.empty() &&
                  restoredChild->clips.front().patternClipId ==
                      restored->clips.front().id,
              "Pattern clip ownership, collapse state and source parenting round-trip");
        const fs::path packagedSample =
            fs::path(patternPkg) / "Content" / "tone.wav";
        auto* restoredSampler = restoredChild
            ? reopened.samplerInstance(child, restoredChild->instrument.id)
            : nullptr;
        check(fs::is_regular_file(packagedSample) && restoredSampler &&
                  restoredSampler->samplePath() == packagedSample.string() &&
                  restoredSampler->rawSample(),
              "Sampler source is copied into Content and loads from the package");

        const fs::path movedPackage = dir / "pattern-portable-copy.vlt";
        fs::copy(patternPkg, movedPackage,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        daw::EngineController moved;
        moved.initialize(48000, 512, false);
        check(moved.openProject(movedPackage.string()).isOk(),
              "a copied VLT package opens from its new location");
        const auto* movedChild = moved.project().findTrack(child);
        auto* movedSampler = movedChild
            ? moved.samplerInstance(child, movedChild->instrument.id)
            : nullptr;
        check(movedSampler &&
                  movedSampler->samplePath() ==
                      (movedPackage / "Content" / "tone.wav").string() &&
                  movedSampler->rawSample(),
              "the copied VLT resolves Sampler audio inside its own Content folder");
    }

    // ── Pattern clips are real arrangement containers ──
    {
        daw::EngineController p;
        p.initialize(48000, 512, false);
        const std::string pattern = p.addPattern("Phrase");
        const std::string source =
            p.addTrack(daw::TrackKind::Instrument, "Keys");
        p.moveTrackToFolder(source, pattern);
        const std::string midi = p.addMidiClip(source, 0.0, 2.0);
        p.addNote(source, midi, 60, 1.0, 4.0);

        const auto* patternTrack = p.project().findTrack(pattern);
        const auto* sourceTrack = p.project().findTrack(source);
        const std::string container = patternTrack && !patternTrack->clips.empty()
                                          ? patternTrack->clips.front().id
                                          : std::string{};
        check(!container.empty() && patternTrack->clips.front().kind ==
                                        daw::ClipKind::Pattern,
              "a Pattern owns a persistent arrangement clip");
        check(sourceTrack && sourceTrack->clips.front().patternClipId == container,
              "a child MIDI clip records which Pattern clip owns it");

        const std::string right = p.splitClip(pattern, container, 1.0);
        check(!right.empty(), "a Pattern clip splits on the arrangement");
        const daw::ClipModel* leftMember = nullptr;
        const daw::ClipModel* rightMember = nullptr;
        sourceTrack = p.project().findTrack(source);
        if (sourceTrack) {
            for (const auto& clip : sourceTrack->clips) {
                if (clip.patternClipId == container) leftMember = &clip;
                if (clip.patternClipId == right) rightMember = &clip;
            }
        }
        check(leftMember && rightMember && leftMember->notes.size() == 1 &&
                  rightMember->notes.size() == 1 &&
                  std::fabs(leftMember->notes.front().lengthBeats - 1.0) < 1e-9 &&
                  std::fabs(rightMember->notes.front().startBeats) < 1e-9 &&
                  std::fabs(rightMember->notes.front().lengthBeats - 3.0) < 1e-9,
              "splitting a Pattern cuts a crossing child note into both halves");
        check(p.undoLabel() == "Split Pattern Clip",
              "the whole nested split is one undo action");

        const std::string copy = p.duplicateClipAt(pattern, right, 2.0);
        check(!copy.empty(), "a Pattern clip duplicates like a MIDI clip");
        const daw::ClipModel* copiedMember = nullptr;
        sourceTrack = p.project().findTrack(source);
        if (sourceTrack) {
            for (const auto& clip : sourceTrack->clips) {
                if (clip.patternClipId == copy) copiedMember = &clip;
            }
        }
        check(copiedMember && std::fabs(copiedMember->startSeconds - 2.0) < 1e-9 &&
                  copiedMember->notes.size() == 1,
              "duplicating a Pattern copies and relinks all child MIDI data");

        p.setClipStartSeconds(pattern, copy, 3.0);
        copiedMember = nullptr;
        sourceTrack = p.project().findTrack(source);
        if (sourceTrack) {
            for (const auto& clip : sourceTrack->clips) {
                if (clip.patternClipId == copy) copiedMember = &clip;
            }
        }
        check(copiedMember && std::fabs(copiedMember->startSeconds - 3.0) < 1e-9,
              "moving a Pattern clip moves its child MIDI clips immediately");

        p.removeClip(pattern, copy);
        bool orphan = false;
        sourceTrack = p.project().findTrack(source);
        if (sourceTrack) {
            for (const auto& clip : sourceTrack->clips)
                orphan |= clip.patternClipId == copy;
        }
        check(!orphan, "deleting a Pattern clip removes its linked MIDI data");
        p.undo();
        check(findClip(p, pattern, copy) != nullptr,
              "undo restores a deleted Pattern clip atomically");
    }

    // Same basename in different source folders must not collapse to one file.
    {
        const fs::path firstDir = dir / "collision-a";
        const fs::path secondDir = dir / "collision-b";
        fs::create_directories(firstDir);
        fs::create_directories(secondDir);
        const std::string firstPath = (firstDir / "take.wav").string();
        const std::string secondPath = (secondDir / "take.wav").string();
        writeTone(firstPath, 48000, 1024);
        writeLeftOnlyTone(secondPath, 48000, 2048);

        daw::ProjectModel collision;
        daw::TrackModel trackWithCollision;
        trackWithCollision.id = daw::newUuid();
        trackWithCollision.kind = daw::TrackKind::Audio;
        daw::ClipModel firstClip;
        firstClip.id = daw::newUuid();
        firstClip.filePath = firstPath;
        firstClip.durationSeconds = 1024.0 / 48000.0;
        daw::ClipModel secondClip;
        secondClip.id = daw::newUuid();
        secondClip.filePath = secondPath;
        secondClip.startSeconds = 1.0;
        secondClip.durationSeconds = 2048.0 / 48000.0;
        trackWithCollision.clips = {firstClip, secondClip};
        collision.tracks.push_back(std::move(trackWithCollision));

        const std::string collisionPackage = (dir / "collision.vlt").string();
        check(daw::ProjectSerializer::save(collision, collisionPackage).isOk(),
              "same-named media files package successfully");
        daw::ProjectModel collisionReloaded;
        check(daw::ProjectSerializer::load(collisionReloaded, collisionPackage).isOk(),
              "same-named media project reloads");
        const auto& savedClips = collisionReloaded.tracks.front().clips;
        check(savedClips.size() == 2 &&
                  savedClips[0].filePath != savedClips[1].filePath &&
                  fs::exists(savedClips[0].filePath) &&
                  fs::exists(savedClips[1].filePath),
              "same-named sources remain distinct packaged files");

        collisionReloaded.tracks.front().clips.front().filePath =
            (dir / "missing.wav").string();
        check(daw::ProjectSerializer::save(collisionReloaded,
                                           (dir / "missing.vlt").string()).isError(),
              "saving reports missing referenced media");
    }

    // ── Waveform envelope ──
    // The arrangement only draws what the cache already holds, so importing has
    // to warm it; a cold cache means clips render blank.
    {
        const daw::WaveformPeaks* wf = ctrl.waveforms().cached(tonePath);
        check(wf != nullptr, "import warms the waveform cache");
        if (wf) {
            check(wf->bucketCount() > 0 && wf->bucketsPerSecond > 0.0,
                  "waveform has buckets");
            check(std::fabs(wf->durationSeconds - 0.5) < 0.01,
                  "waveform duration matches the file");
            float lowest = 0.0f, highest = 0.0f;
            for (size_t i = 0; i < wf->bucketCount(); ++i) {
                lowest = std::min(lowest, wf->minima[i]);
                highest = std::max(highest, wf->maxima[i]);
            }
            check(highest > 0.3f && lowest < -0.3f,
                  "waveform envelope tracks the 0.5-amplitude tone");
        }
    }

    // ── Routing graph ──
    // The document's routing has to reach the engine as a compiled node graph:
    // clips → fader → meter → bus, with sends tapped off the meter. If it does
    // not, playback silently falls back to "everything straight to master".
    {
        daw::EngineController r;
        r.initialize(48000, 512, /*openDevice=*/false);
        const std::string src = r.addTrack(daw::TrackKind::Audio, "Source");
        const std::string bus = r.addTrack(daw::TrackKind::Bus, "Reverb Bus");

        check(r.setTrackOutputBus(src, bus), "track output can target a bus");
        const std::string sendId = r.addSend(src, bus);
        check(!sendId.empty(), "send added to the bus");

        auto graph = r.routingGraph();
        check(graph != nullptr, "compiled graph is published");
        const auto* srcNodes = r.trackNodes(src);
        const auto* busNodes = r.trackNodes(bus);
        check(srcNodes && busNodes, "both tracks have engine nodes");

        auto denseIndexOf = [&](daw::engine::NodeId id) {
            for (std::size_t i = 0; i < graph->nodes.size(); ++i) {
                if (graph->nodes[i].id == id) return int(i);
            }
            return -1;
        };
        auto hasEdge = [&](daw::engine::NodeId producer,
                           daw::engine::NodeId consumer) {
            const int c = denseIndexOf(consumer);
            if (c < 0) return false;
            const auto& node = graph->nodes[std::size_t(c)];
            for (std::uint32_t i = 0; i < node.inputCount; ++i) {
                const auto& edge = graph->inputEdges[node.firstInput + i];
                if (graph->nodes[edge.producer].id == producer) return true;
            }
            return false;
        };

        check(hasEdge(srcNodes->clips, srcNodes->fader), "clips feed the fader");
        check(hasEdge(srcNodes->fader, srcNodes->meter), "fader feeds the meter");
        // Everything routed into the bus joins at its merge point, which sits
        // *ahead of the bus's inserts*. Landing on the bus's fader instead
        // would let the signal through while skipping every plugin on it.
        check(busNodes->sum != daw::engine::kInvalidNode,
              "a bus that is routed into has a merge point");
        check(srcNodes->sum == daw::engine::kInvalidNode,
              "a track nothing is routed into pays for no merge node");
        check(hasEdge(srcNodes->meter, busNodes->sum),
              "the track's output lands on the bus ahead of its inserts");
        check(hasEdge(busNodes->sum, busNodes->fader),
              "and reaches the bus fader through the (here empty) insert chain");
        check(srcNodes->sends.size() == 1, "the send became a node");
        check(hasEdge(srcNodes->meter, srcNodes->sends[0]),
              "a post-fader send taps after the fader");
        check(hasEdge(srcNodes->sends[0], busNodes->sum),
              "the send feeds the bus ahead of its inserts");

        // The bus itself has to reach the master, which is the graph's sink.
        check(graph->sinkNode != daw::engine::kInvalidNode, "graph has a sink");
        const auto& sink = graph->nodes[graph->sinkNode];
        check(sink.inputCount == 1, "the master fader is fed by the master sum");
        const std::uint32_t masterSum =
            graph->inputEdges[sink.firstInput].producer;
        bool busReachesMaster = false;
        const auto& sumNode = graph->nodes[masterSum];
        for (std::uint32_t i = 0; i < sumNode.inputCount; ++i) {
            if (graph->nodes[graph->inputEdges[sumNode.firstInput + i].producer].id ==
                busNodes->meter) {
                busReachesMaster = true;
            }
        }
        check(busReachesMaster, "the bus feeds the master");

        check(!r.setTrackOutputBus(bus, src), "a feedback loop is rejected");
        check(r.project().findTrack(bus)->outputBusId.empty(),
              "rejected routing leaves the old destination in place");

        r.removeTrack(bus);
        check(r.project().findTrack(src)->outputBusId.empty() &&
                  r.project().findTrack(src)->sends.empty(),
              "removing a bus clears edges that pointed at it");
        check(r.trackNodes(bus) == nullptr, "the bus's nodes are gone too");
    }

    // ── Rendering through the new engine ──
    // End to end: a clip on a track, rendered offline by the graph engine and
    // read back from disk. This is the check that the app actually makes sound.
    {
        daw::EngineController r;
        r.initialize(48000, 512, /*openDevice=*/false);
        const std::string track = r.addTrack(daw::TrackKind::Audio, "Tone");
        check(!r.importAudio(tonePath, track, 0.0).empty(), "clip imported");
        check(std::fabs(r.durationSeconds() - 0.5) < 0.01,
              "timeline length follows the clip");

        const std::string mixPath = (dir / "mix.wav").string();
        check(r.exportMixdown(mixPath, false).isOk(), "mixdown renders");

        audio::platform::DecodedAudio rendered;
        check(audio::platform::decodeAudioFile(mixPath, rendered).isOk(),
              "mixdown is readable");
        float peak = 0.0f;
        for (float sample : rendered.interleaved) {
            peak = std::max(peak, std::fabs(sample));
        }
        check(rendered.frames > 20000, "mixdown has the expected length");
        check(peak > 0.45f && peak < 0.55f,
              "rendered peak matches the 0.5-amplitude tone");

        // Muting the track must silence the render — proof the fader is really
        // in the signal path and not just in the document.
        r.setTrackMuted(track, true);
        const std::string mutedPath = (dir / "muted.wav").string();
        check(r.exportMixdown(mutedPath, false).isOk(), "muted mixdown renders");
        audio::platform::DecodedAudio silent;
        (void)audio::platform::decodeAudioFile(mutedPath, silent);
        float mutedPeak = 0.0f;
        for (float sample : silent.interleaved) {
            mutedPeak = std::max(mutedPeak, std::fabs(sample));
        }
        check(mutedPeak < 0.0001f, "a muted track renders silence");

        r.setTrackMuted(track, false);
        r.setTrackVolume(track, 0.5f);
        const std::string halfPath = (dir / "half.wav").string();
        check(r.exportMixdown(halfPath, false).isOk(), "half-gain mixdown renders");
        audio::platform::DecodedAudio half;
        (void)audio::platform::decodeAudioFile(halfPath, half);
        float halfPeak = 0.0f;
        for (float sample : half.interleaved) {
            halfPeak = std::max(halfPeak, std::fabs(sample));
        }
        // Half gain, panned centre: the balance law passes centre at unity.
        check(halfPeak > 0.22f && halfPeak < 0.28f,
              "the fader scales the rendered mix");
    }

    // ── Input monitoring ──
    // The engine gates monitoring behind one global switch; the controller has
    // to keep it in step with the per-track buttons.
    {
        daw::EngineController m;
        m.initialize(48000, 512, /*openDevice=*/false);
        const std::string a = m.addTrack(daw::TrackKind::Audio, "A");
        const std::string b = m.addTrack(daw::TrackKind::Audio, "B");
        check(!m.isInputMonitoringActive(), "monitoring starts off");
        m.setTrackMonitor(a, true);
        check(m.isInputMonitoringActive(), "monitoring follows a track");
        m.setTrackMonitor(b, true);
        m.setTrackMonitor(a, false);
        check(m.isInputMonitoringActive(),
              "monitoring stays on while another track listens");
        m.setTrackMonitor(b, false);
        check(!m.isInputMonitoringActive(),
              "monitoring switches off with the last track");
    }

    // ── Folders and track order ──
    {
        daw::EngineController f;
        f.initialize(48000, 512, /*openDevice=*/false);
        const std::string a = f.addTrack(daw::TrackKind::Audio, "A");
        const std::string b = f.addTrack(daw::TrackKind::Audio, "B");
        const std::string c = f.addTrack(daw::TrackKind::Audio, "C");

        // Reorder: move C to the front.
        check(f.moveTrack(c, 0, ""), "track moves to the top");
        check(f.project().tracks[0].id == c && f.project().tracks[1].id == a,
              "document order follows the move");

        const std::string folder = f.packIntoFolder({a, b}, "Group");
        check(!folder.empty(), "tracks pack into a folder");
        check(f.project().findTrack(a)->parentId == folder &&
                  f.project().findTrack(b)->parentId == folder,
              "packed tracks are parented to the folder");
        check(daw::subtreeOf(f.project(), folder).size() == 2,
              "folder reports both children");
        check(daw::trackDepth(f.project(), a) == 1, "child depth is 1");

        // Collapsing hides the children from the visible order (lanes + rows).
        check(daw::visibleTracks(f.project()).size() == 4,
              "expanded folder shows its children");
        f.setFolderExpanded(folder, false);
        check(daw::visibleTracks(f.project()).size() == 2,
              "collapsed folder hides its children");
        f.setFolderExpanded(folder, true);

        // A folder cannot be filed inside itself or its own child.
        check(!f.moveTrack(folder, 0, a),
              "a folder cannot be dropped into its own child");
        check(!f.moveTrack(folder, 0, folder), "a folder cannot contain itself");

        // Moving the folder carries its children along, in order.
        check(f.moveTrack(folder, f.project().tracks.size(), ""),
              "folder moves to the end");
        const auto& tracks = f.project().tracks;
        check(tracks[tracks.size() - 3].id == folder &&
                  tracks[tracks.size() - 2].id == a &&
                  tracks[tracks.size() - 1].id == b,
              "the folder's subtree moves with it");

        // Dragging a track out of the folder puts it back at the top level.
        check(f.moveTrack(a, 0, ""), "track leaves the folder");
        check(f.project().findTrack(a)->parentId.empty(),
              "track is unparented after leaving");

        f.undo();
        check(!f.project().findTrack(a)->parentId.empty(),
              "undo puts the track back into the folder");
    }

    // ── Mono / stereo per-track fold ──
    // A stereo source with signal only on the left: in stereo the render keeps
    // the left/right imbalance; folded to mono both channels carry it equally.
    {
        const std::string lrPath = (dir / "left.wav").string();
        writeLeftOnlyTone(lrPath, 48000, 24000);

        daw::EngineController r;
        r.initialize(48000, 512, /*openDevice=*/false);
        const std::string track = r.addTrack(daw::TrackKind::Audio, "LR");
        check(!r.importAudio(lrPath, track, 0.0).empty(),
              "left-only clip imported");

        // Stereo: left carries the tone, right is (near) silent.
        r.setTrackMono(track, false);
        const std::string stereoPath = (dir / "stereo.wav").string();
        check(r.exportMixdown(stereoPath, false).isOk(), "stereo mixdown renders");
        audio::platform::DecodedAudio st;
        (void)audio::platform::decodeAudioFile(stereoPath, st);
        const float stL = channelPeak(st, 0);
        const float stR = channelPeak(st, 1);
        check(stL > 0.4f, "stereo keeps the left-channel tone");
        check(stR < 0.01f, "stereo keeps the right channel empty");

        // Mono: the left signal is spread to both channels, at half amplitude.
        r.setTrackMono(track, true);
        const std::string monoPath = (dir / "mono.wav").string();
        check(r.exportMixdown(monoPath, false).isOk(), "mono mixdown renders");
        audio::platform::DecodedAudio mo;
        (void)audio::platform::decodeAudioFile(monoPath, mo);
        const float moL = channelPeak(mo, 0);
        const float moR = channelPeak(mo, 1);
        check(std::fabs(moL - moR) < 0.01f, "mono makes both channels equal");
        check(moL > 0.2f && moL < 0.3f,
              "mono spreads the left tone at half amplitude");

        // Tracks default to stereo; the flag is undoable and survives reload.
        check(!r.addTrack(daw::TrackKind::Audio, "Fresh").empty() &&
                  !r.project().tracks.back().mono,
              "new tracks default to stereo");
        r.setTrackMono(track, false);   // known state
        r.setTrackMono(track, true);
        check(r.project().findTrack(track)->mono, "mono flag applies");
        r.undo();
        check(!r.project().findTrack(track)->mono,
              "undo restores the stereo flag");
        r.redo();

        const std::string monoPkg = (dir / "mono.vlt").string();
        check(r.saveProject(monoPkg).isOk(), "project with mono flag saves");
        daw::EngineController r2;
        r2.initialize(48000, 512, false);
        check(r2.openProject(monoPkg).isOk(), "project with mono flag reopens");
        check(r2.project().findTrack(track)->mono, "mono flag round-trips");
    }

    // ── Split a clip ──
    {
        daw::EngineController r;
        r.initialize(48000, 512, /*openDevice=*/false);
        const std::string track = r.addTrack(daw::TrackKind::Audio, "Cut");
        const std::string clip = r.importAudio(tonePath, track, 0.0);
        check(!clip.empty(), "clip to split imported");   // 0.5 s long

        const std::string right = r.splitClip(track, clip, 0.2);
        check(!right.empty(), "split returns a new clip id");
        check(r.project().tracks[0].clips.size() == 2, "split yields two clips");

        const daw::ClipModel* left = findClip(r, track, clip);
        const daw::ClipModel* rc = findClip(r, track, right);
        check(left && rc, "both halves exist");
        if (left && rc) {
            check(std::fabs(left->durationSeconds - 0.2) < 1e-6,
                  "left half ends at the cut");
            check(std::fabs(rc->startSeconds - 0.2) < 1e-6,
                  "right half starts at the cut");
            check(std::fabs(rc->offsetSeconds - 0.2) < 1e-6,
                  "right half reads on from the cut in the source");
            check(std::fabs(rc->startSeconds -
                            (left->startSeconds + left->durationSeconds)) < 1e-6,
                  "halves are contiguous");
            check(std::fabs(left->durationSeconds + rc->durationSeconds - 0.5) < 1e-6,
                  "the two halves cover the original length");
        }

        // A cut outside the clip is rejected.
        check(r.splitClip(track, clip, 5.0).empty(),
              "a cut past the clip end is rejected");

        r.undo();
        check(r.project().tracks[0].clips.size() == 1, "undo re-joins the clip");
        check(std::fabs(findClip(r, track, clip)->durationSeconds - 0.5) < 1e-6,
              "undo restores the original length");
        r.redo();
        check(r.project().tracks[0].clips.size() == 2, "redo splits again");
    }

    // ── Clip mute / name / duplicate, and track colour reaching its clips ──
    // The four discrete clip edits the context panel drives. Each has to change
    // the document and come back cleanly through undo.
    {
        daw::EngineController r;
        r.initialize(48000, 512, /*openDevice=*/false);
        const std::string track = r.addTrack(daw::TrackKind::Audio, "Props");
        const std::string clip = r.importAudio(tonePath, track, 0.0);  // 0.5 s
        check(!clip.empty(), "clip for property edits imported");

        check(!findClip(r, track, clip)->muted, "a fresh clip is unmuted");
        r.setClipMuted(track, clip, true);
        check(findClip(r, track, clip)->muted, "clip mutes");
        r.undo();
        check(!findClip(r, track, clip)->muted, "undo unmutes the clip");
        r.redo();
        check(findClip(r, track, clip)->muted, "redo re-mutes the clip");
        r.setClipMuted(track, clip, false);

        const std::string original = findClip(r, track, clip)->name;
        r.setClipName(track, clip, "Chorus Gtr");
        check(findClip(r, track, clip)->name == "Chorus Gtr", "clip renames");
        r.undo();
        check(findClip(r, track, clip)->name == original,
              "undo restores the clip name");

        // A clip's colour is its track's colour: recolouring the track has to
        // take the clips already sitting on it with it, or the lane ends up
        // striped in whatever the colour was when each clip landed.
        r.setTrackColor(track, 0x3B82F6);
        check(r.project().findTrack(track)->color == 0x3B82F6, "track recolours");
        check(findClip(r, track, clip)->color == 0x3B82F6,
              "the track's clips follow its colour");

        const std::string copy = r.duplicateClip(track, clip);
        check(!copy.empty() && copy != clip, "duplicate returns a fresh id");
        check(r.project().findTrack(track)->clips.size() == 2,
              "duplicate adds a clip");
        const daw::ClipModel* dup = findClip(r, track, copy);
        check(dup && std::fabs(dup->startSeconds - 0.5) < 1e-6,
              "the copy butts up against the end of the original");
        check(dup && std::fabs(dup->durationSeconds -
                               findClip(r, track, clip)->durationSeconds) < 1e-6,
              "the copy keeps the original's length");
        r.undo();
        check(r.project().findTrack(track)->clips.size() == 1,
              "undo removes the duplicate");
        r.redo();
        check(r.project().findTrack(track)->clips.size() == 2,
              "redo re-adds the duplicate");
        const std::string placed = r.duplicateClipAt(track, clip, 3.25);
        check(findClip(r, track, placed) &&
                  std::fabs(findClip(r, track, placed)->startSeconds - 3.25) < 1e-6,
              "an exact-position duplicate preserves group repeat spacing");
        r.undo();

        // Mute survives a save/reload, so the panel's state isn't a session-only
        // decoration.
        r.setClipMuted(track, clip, true);
        const std::string pkg = (dir / "clipprops.vlt").string();
        check(r.saveProject(pkg).isOk(), "project with clip props saves");
        daw::EngineController r2;
        r2.initialize(48000, 512, false);
        check(r2.openProject(pkg).isOk(), "project with clip props reopens");
        bool sawMuted = false;
        for (const auto& c : r2.project().findTrack(track)->clips)
            if (c.muted) sawMuted = true;
        check(sawMuted, "clip mute round-trips through the project file");
    }

    // ── Region-tool partial edit ──
    // The SelectRegion tool tears the part of a straddling clip inside the
    // region out (split at both edges), then deletes it or moves it. The outer
    // fragments must survive with their original geometry.
    {
        daw::EngineController r;
        r.initialize(48000, 512, /*openDevice=*/false);
        const std::string track = r.addTrack(daw::TrackKind::Audio, "RegDel");
        const std::string clip = r.importAudio(tonePath, track, 0.0);  // 0.5 s

        // Region [0.1, 0.4] over a clip that spans it.
        std::string inner = r.splitClip(track, clip, 0.1);   // [0.1, 0.5)
        (void)r.splitClip(track, inner, 0.4);                // inner = [0.1, 0.4)
        check(r.project().findTrack(track)->clips.size() == 3,
              "a straddling clip splits into three");

        r.removeClip(track, inner);
        const auto& frags = r.project().findTrack(track)->clips;
        check(frags.size() == 2, "deleting the inner piece leaves the outers");
        double minStart = 1.0, maxStart = -1.0;
        for (const auto& c : frags) {
            minStart = std::min(minStart, c.startSeconds);
            maxStart = std::max(maxStart, c.startSeconds + c.durationSeconds);
        }
        check(std::fabs(minStart) < 1e-6 && std::fabs(maxStart - 0.5) < 1e-6,
              "outer fragments still span the full clip");
    }
    {
        daw::EngineController r;
        r.initialize(48000, 512, /*openDevice=*/false);
        const std::string track = r.addTrack(daw::TrackKind::Audio, "RegMove");
        const std::string clip = r.importAudio(tonePath, track, 0.0);

        std::string inner = r.splitClip(track, clip, 0.1);
        (void)r.splitClip(track, inner, 0.4);

        // Moving the torn-out piece shifts only it; the outers stay anchored.
        r.setClipStartSeconds(track, inner, 0.1 + 0.2);
        const auto& after = r.project().findTrack(track)->clips;
        const daw::ClipModel* moved = nullptr;
        for (const auto& c : after)
            if (c.id == inner) moved = &c;
        check(moved && std::fabs(moved->startSeconds - 0.3) < 1e-6,
              "the region piece moves to the new time");
        double minStart = 1.0, maxStart = -1.0;
        for (const auto& c : after) {
            if (c.id == inner) continue;
            minStart = std::min(minStart, c.startSeconds);
            maxStart = std::max(maxStart, c.startSeconds);
        }
        check(std::fabs(minStart) < 1e-6 && std::fabs(maxStart - 0.4) < 1e-6,
              "outer fragments stay in place");
    }

    // ── Trim a clip (non-destructive edge resize) ──
    {
        daw::EngineController r;
        r.initialize(48000, 512, /*openDevice=*/false);
        const std::string track = r.addTrack(daw::TrackKind::Audio, "Trim");
        const std::string clip = r.importAudio(tonePath, track, 0.0);  // 0.5 s

        // Move both edges inward.
        r.setClipTrim(track, clip, 0.1, 0.1, 0.3);
        const daw::ClipModel* c = findClip(r, track, clip);
        check(c && std::fabs(c->startSeconds - 0.1) < 1e-6 &&
                  std::fabs(c->offsetSeconds - 0.1) < 1e-6 &&
                  std::fabs(c->durationSeconds - 0.3) < 1e-6,
              "trim sets start, offset and length together");

        // Over-long / negative values are clamped to the source bounds.
        r.setClipTrim(track, clip, 0.0, -0.5, 10.0);
        c = findClip(r, track, clip);
        check(c && c->offsetSeconds >= 0.0 &&
                  c->offsetSeconds + c->durationSeconds <= 0.5 + 1e-6,
              "trim clamps to the source length");

        // A zero-length request is floored, never collapsed to nothing.
        r.setClipTrim(track, clip, 0.0, 0.0, 0.0);
        c = findClip(r, track, clip);
        check(c && c->durationSeconds > 0.0, "trim keeps a minimum length");
    }

    // ── Clip fades ──
    {
        daw::EngineController r;
        r.initialize(48000, 512, /*openDevice=*/false);
        const std::string track = r.addTrack(daw::TrackKind::Audio, "Fade");
        const std::string clip = r.importAudio(tonePath, track, 0.0);  // 0.5 s

        r.setClipFade(track, clip, 0.1, 0.2);
        r.setClipFadeCurve(track, clip, true, 0.65);
        r.setClipFadeCurve(track, clip, false, -0.4);
        r.setClipFadeMode(track, clip, true, daw::ClipFadeMode::Tape);
        r.setClipFadeMode(track, clip, false, daw::ClipFadeMode::Tape);
        const daw::ClipModel* c = findClip(r, track, clip);
        check(c && std::fabs(c->fadeInSeconds - 0.1) < 1e-6 &&
                  std::fabs(c->fadeOutSeconds - 0.2) < 1e-6,
              "setClipFade stores head/tail fades");
        check(c && std::fabs(c->fadeInCurve - 0.65) < 1e-6 &&
                  std::fabs(c->fadeOutCurve + 0.4) < 1e-6 &&
                  c->fadeInMode == daw::ClipFadeMode::Tape &&
                  c->fadeOutMode == daw::ClipFadeMode::Tape,
              "fade curves and tape modes are stored independently");
        r.undo();
        check(findClip(r, track, clip)->fadeOutMode == daw::ClipFadeMode::Gain,
              "undo restores the previous fade mode");
        r.redo();

        // The two ramps can't sum past the clip length (0.5 s here).
        r.setClipFade(track, clip, 0.4, 0.4);
        c = findClip(r, track, clip);
        check(c && c->fadeInSeconds + c->fadeOutSeconds <= 0.5 + 1e-6,
              "fades clamp so they don't cross over");

        // Channel count is captured at import (tone.wav is stereo).
        check(c && c->channels == 2, "import records the source channel count");
    }

    // ── A mono recording is heard in both ears on a stereo track ──
    //
    // The bug this pins down: a one-channel file played by a track whose mono
    // fold is *off* came out of the left speaker only. Recording from a mono
    // input is the normal case, and switching such a track to stereo — which is
    // what anyone does before reaching for a stereo effect — silenced one side.
    {
        const std::string monoTone = (dir / "one-channel.wav").string();
        writeMonoTone(monoTone, 48000, 24000);

        daw::EngineController m;
        m.initialize(48000, 512, /*openDevice=*/false);
        const std::string track = m.addTrack(daw::TrackKind::Audio, "Mono");
        m.importAudio(monoTone, track, 0.0);
        check(!m.project().findTrack(track)->mono,
              "a fresh track is not folded to mono");

        const std::string wide = (dir / "mono-on-stereo.wav").string();
        check(m.exportMixdown(wide, false).isOk(), "the mono clip renders");
        audio::platform::DecodedAudio out;
        audio::platform::decodeAudioFile(wide, out);
        const float left = channelPeak(out, 0);
        const float right = channelPeak(out, 1);
        check(left > 0.1f, "a mono clip is heard on the left");
        check(right > 0.1f, "a mono clip is heard on the right too");
        check(std::fabs(left - right) < 0.01f,
              "and at the same level on both — it is centred, not panned hard");

        // Panning it must still work: hard left silences the right side.
        m.setTrackPan(track, -1.0f);
        const std::string panned = (dir / "mono-panned.wav").string();
        m.exportMixdown(panned, false);
        audio::platform::DecodedAudio hard;
        audio::platform::decodeAudioFile(panned, hard);
        check(channelPeak(hard, 0) > 0.1f && channelPeak(hard, 1) < 0.01f,
              "and panning a mono clip hard left still empties the right");
    }

    // ── A capture is exactly as wide as the input it is pointed at ──
    //
    // The other half of the mono bug: recording always took a *pair* starting
    // at the chosen input, so a mono microphone on input 1 wrote a stereo file
    // whose right channel was input 2 — silence. Nothing downstream could undo
    // that, and the track's mono fold was the only thing hiding it.
    {
        daw::EngineController m;
        m.initialize(48000, 512, /*openDevice=*/false);
        m.setRecordDirectory((dir / "capture-width").string());
        const std::string track = m.addTrack(daw::TrackKind::Audio, "Mic");
        check(m.project().findTrack(track)->inputChannelCount == 1,
              "a track listens to one input channel unless told otherwise");

        // Read the width off the file the take actually wrote. There is no
        // device in a headless run, so no frames arrive and no clip lands —
        // but the WAV is opened with its channel count at the top of the take,
        // which is the decision under test.
        // Straight out of the WAV header, at offset 22: a take that has captured
        // nothing yet has no frames for a decoder to return, but its channel
        // count was written the moment the file was opened — which is the
        // decision under test.
        const auto writtenChannels = [&]() -> int {
            int found = -1;
            fs::file_time_type newest{};
            for (const auto& entry : fs::directory_iterator(m.recordDirectory())) {
                if (entry.path().extension() != ".wav") continue;
                if (found >= 0 && entry.last_write_time() <= newest) continue;
                std::FILE* file = std::fopen(entry.path().string().c_str(), "rb");
                if (!file) continue;
                unsigned char header[24] = {};
                const size_t read = std::fread(header, 1, sizeof(header), file);
                std::fclose(file);
                if (read < sizeof(header)) continue;
                newest = entry.last_write_time();
                found = int(header[22]) | (int(header[23]) << 8);
            }
            return found;
        };

        // Read it while the take is rolling: a take that captured nothing —
        // which is every take in a run with no audio device — is discarded on
        // stop, file and all.
        check(m.startRecording(track), "a mono take starts");
        const int monoWidth = writtenChannels();
        m.stopRecording();
        check(monoWidth == 1,
              "and writes one channel, not a pair with a silent side");

        m.setTrackInputChannelCount(track, 2);
        check(m.startRecording(track), "a stereo take starts");
        const int stereoWidth = writtenChannels();
        m.stopRecording();
        check(stereoWidth == 2,
              "while a track pointed at a pair still captures both");
    }

    // ── The cycle region ──
    {
        daw::EngineController c;
        c.initialize(48000, 512, /*openDevice=*/false);
        check(!c.isLoopEnabled(), "a new project is not cycling");
        check(c.loopEndSeconds() <= c.loopStartSeconds(),
              "and has no region yet");

        c.setLoopRangeSeconds(4.0, 8.0);
        c.setLoopEnabled(true);
        check(std::fabs(c.loopStartSeconds() - 4.0) < 1e-6 &&
                  std::fabs(c.loopEndSeconds() - 8.0) < 1e-6,
              "a region is set in seconds");

        // The playhead travels round the region and nowhere else: pressing play
        // from before it starts inside it, which the transport alone cannot do
        // — it wraps at the end but never pulls the position in.
        c.seekSeconds(0.0);
        c.play();
        check(std::fabs(c.positionSeconds() - 4.0) < 1e-6,
              "playing from before the cycle jumps into it");
        c.stop();

        c.seekSeconds(20.0);
        c.play();
        check(std::fabs(c.positionSeconds() - 4.0) < 1e-6,
              "and so does playing from past it");
        c.stop();

        c.seekSeconds(6.0);
        c.play();
        check(std::fabs(c.positionSeconds() - 6.0) < 1e-6,
              "while playing from inside it starts where it stood");
        c.stop();

        // Switched off, the playhead is free again.
        c.setLoopEnabled(false);
        c.seekSeconds(0.0);
        c.play();
        check(std::fabs(c.positionSeconds()) < 1e-6,
              "with the cycle off, play starts where the playhead is");
        c.stop();
    }

    // ── A cycle survives a save and reload ──
    {
        daw::EngineController c;
        c.initialize(48000, 512, /*openDevice=*/false);
        c.addTrack(daw::TrackKind::Audio, "Any");
        c.setLoopRangeSeconds(2.0, 10.0);
        c.setLoopEnabled(true);
        const std::string pkg = (dir / "cycle.vlt").string();
        check(c.saveProject(pkg).isOk(), "a project with a cycle saves");

        daw::EngineController back;
        back.initialize(48000, 512, /*openDevice=*/false);
        check(back.openProject(pkg).isOk(), "and reopens");
        check(std::fabs(back.loopStartSeconds() - 2.0) < 1e-6 &&
                  std::fabs(back.loopEndSeconds() - 10.0) < 1e-6,
              "with the same region");
        check(back.isLoopEnabled(), "still armed");
    }

    // ── A cycle is musical: it keeps its bars across a tempo change ──
    {
        daw::EngineController c;
        c.initialize(48000, 512, /*openDevice=*/false);
        c.setTempo(120.0);
        // Bars 1 to 3 at 120 BPM in 4/4: two bars of two seconds each.
        c.setLoopRangeSeconds(0.0, 4.0);
        c.setLoopEnabled(true);
        c.setTempo(240.0);
        check(std::fabs(c.loopEndSeconds() - 2.0) < 1e-3,
              "doubling the tempo halves the cycle's length in seconds");
    }

    // ── Automation clips: the model and the curve ──
    {
        daw::EngineController a;
        a.initialize(48000, 512, /*openDevice=*/false);
        const std::string track = a.addTrack(daw::TrackKind::Audio, "Lead");
        a.importAudio(tonePath, track, 0.0);   // gives the project a length

        daw::AutomationTarget target;
        target.kind = daw::AutomationTargetKind::TrackVolume;
        target.channelId = track;

        const std::string lane = a.addAutomationLane(track, target);
        check(!lane.empty(), "an automation lane is created");
        const daw::TrackModel* laneTrack = a.project().findTrack(lane);
        check(laneTrack && daw::isAutomationLane(*laneTrack),
              "and it is an automation lane");
        check(laneTrack && !daw::carriesAudio(*laneTrack),
              "which carries no signal of its own");
        check(a.trackNodes(lane) == nullptr,
              "so the engine builds it no channel");
        check(laneTrack && laneTrack->parentId == track,
              "it is filed under the track it drives");
        check(a.project().findTrack(track)->expanded,
              "and that track is opened so the lane can be seen");
        check(a.automationLanesOf(track).size() == 1,
              "the track reports its lane");

        const std::string clip = a.addAutomationClip(lane, target, 0.0);
        check(!clip.empty(), "a curve is created on the lane");
        const daw::ClipModel* model = findClip(a, lane, clip);
        check(model && model->kind == daw::ClipKind::Automation,
              "the clip is an automation clip");
        // "As far as the content goes": the tone is half a second, so the
        // eight-bar floor wins here — either way it must cover the arrangement.
        check(model && model->durationSeconds >= a.durationSeconds() - 1e-6,
              "and it spans at least the content on the timeline");
        check(model && std::fabs(model->automation.defaultValue -
                                 daw::normalizedFromGain(1.0)) < 1e-6,
              "a fresh curve starts where the control already stands");

        // Only automation clips go on an automation lane, and they go nowhere
        // else.
        check(daw::trackAccepts(daw::TrackKind::Automation,
                                daw::ClipKind::Automation),
              "an automation lane takes automation clips");
        check(!daw::trackAccepts(daw::TrackKind::Automation,
                                 daw::ClipKind::Audio) &&
                  !daw::trackAccepts(daw::TrackKind::Audio,
                                     daw::ClipKind::Automation),
              "and nothing else swaps places with them");
    }

    // ── The curve is read the way it is drawn ──
    {
        std::vector<daw::AutomationPoint> points;
        points.push_back({0.0, 0.0, daw::AutomationSegment::Linear, 0.0});
        points.push_back({4.0, 1.0, daw::AutomationSegment::Linear, 0.0});
        check(std::fabs(daw::automationValueAt(points, 2.0, 0.0) - 0.5) < 1e-9,
              "a straight segment reads half way at half way");
        check(std::fabs(daw::automationValueAt(points, -1.0, 0.25) - 0.25) < 1e-9,
              "before the first point the curve holds its default, not its "
              "first value");
        check(std::fabs(daw::automationValueAt(points, 99.0, 0.0) - 1.0) < 1e-9,
              "and past the last point it holds that");

        points[0].shape = daw::AutomationSegment::Hold;
        check(std::fabs(daw::automationValueAt(points, 3.99, 0.0)) < 1e-9,
              "a hold segment does not move until the next point");
        check(std::fabs(daw::automationValueAt(points, 4.0, 0.0) - 1.0) < 1e-9,
              "and then it steps");

        points[0].shape = daw::AutomationSegment::Linear;
        points[0].curve = 0.9;
        const double bent = daw::automationValueAt(points, 2.0, 0.0);
        check(bent > 0.5, "bending one way lifts the middle of the segment");
        points[0].curve = -0.9;
        check(daw::automationValueAt(points, 2.0, 0.0) < 0.5,
              "and bending the other drops it");
        // Whichever way it is bent, the ends are still the ends.
        check(std::fabs(daw::automationValueAt(points, 0.0, 0.0)) < 1e-9 &&
                  std::fabs(daw::automationValueAt(points, 4.0, 0.0) - 1.0) < 1e-9,
              "a bend never moves the points it runs between");
        (void)bent;
    }

    // ── Splitting a curve leaves both halves sounding the same ──
    {
        daw::EngineController a;
        a.initialize(48000, 512, /*openDevice=*/false);
        a.setTempo(120.0);                       // one beat = 0.5 s
        const std::string track = a.addTrack(daw::TrackKind::Audio, "Lead");
        daw::AutomationTarget target;
        target.kind = daw::AutomationTargetKind::TrackVolume;
        target.channelId = track;
        const std::string lane = a.addAutomationLane(track, target);
        const std::string clip = a.addAutomationClip(lane, target, 0.0, 8.0);

        // A ramp from silence to unity across the first eight beats.
        std::vector<daw::AutomationPoint> ramp;
        ramp.push_back({0.0, 0.0, daw::AutomationSegment::Linear, 0.0});
        ramp.push_back({8.0, 1.0, daw::AutomationSegment::Linear, 0.0});
        a.setAutomationPoints(lane, clip, ramp);

        // The value at the knife, read before cutting.
        const double atCut = daw::automationValueAt(
            findClip(a, lane, clip)->automation.points, 4.0, 0.0);
        const std::string right = a.splitClip(lane, clip, 2.0);   // beat 4
        check(!right.empty(), "an automation clip splits");

        const daw::ClipModel* left = findClip(a, lane, clip);
        const daw::ClipModel* tail = findClip(a, lane, right);
        check(left && tail, "both halves are there");
        // The seam: the left half must end holding the value the curve had, and
        // the right half must open holding it — otherwise cutting a curve
        // changes it, which is what the arrangement's knife must never do.
        check(left && std::fabs(daw::automationValueAt(
                          left->automation.points, 4.0,
                          left->automation.defaultValue) - atCut) < 1e-6,
              "the left half still holds the value it had at the cut");
        check(tail && std::fabs(daw::automationValueAt(
                          tail->automation.points, 0.0,
                          tail->automation.defaultValue) - atCut) < 1e-6,
              "and the right half opens on it rather than on a default");

        a.undo();
        check(findClip(a, lane, right) == nullptr &&
                  findClip(a, lane, clip)->automation.points.size() == 2,
              "undo puts the whole curve back");
    }

    // ── A curve survives save, reload and a tempo change ──
    {
        daw::EngineController a;
        a.initialize(48000, 512, /*openDevice=*/false);
        a.setTempo(120.0);
        const std::string track = a.addTrack(daw::TrackKind::Audio, "Lead");
        daw::AutomationTarget target;
        target.kind = daw::AutomationTargetKind::TrackPan;
        target.channelId = track;
        const std::string lane = a.addAutomationLane(track, target);
        const std::string clip = a.addAutomationClip(lane, target, 1.0, 4.0);

        std::vector<daw::AutomationPoint> shape;
        shape.push_back({0.0, 0.25, daw::AutomationSegment::Hold, 0.0});
        shape.push_back({2.0, 0.75, daw::AutomationSegment::SCurve, -0.5});
        shape.push_back({4.0, 0.1, daw::AutomationSegment::Linear, 0.0});
        a.setAutomationPoints(lane, clip, shape);

        const std::string pkg = (dir / "automation.vlt").string();
        check(a.saveProject(pkg).isOk(), "a project with a curve saves");

        daw::EngineController back;
        back.initialize(48000, 512, /*openDevice=*/false);
        check(back.openProject(pkg).isOk(), "and reopens");
        const daw::ClipModel* reloaded = findClip(back, lane, clip);
        check(reloaded && reloaded->kind == daw::ClipKind::Automation,
              "the clip comes back as an automation clip");
        check(reloaded && reloaded->automation.points.size() == 3,
              "with every point");
        check(reloaded &&
                  reloaded->automation.points[0].shape ==
                      daw::AutomationSegment::Hold &&
                  reloaded->automation.points[1].shape ==
                      daw::AutomationSegment::SCurve &&
                  std::fabs(reloaded->automation.points[1].curve + 0.5) < 1e-9,
              "and every shape and bend");
        check(reloaded &&
                  reloaded->automation.target.kind ==
                      daw::AutomationTargetKind::TrackPan &&
                  reloaded->automation.target.channelId == track,
              "pointed at what it was pointed at");

        // Musical, like everything else on the timeline: the curve keeps its
        // bars when the tempo moves.
        const double startBefore = reloaded->startSeconds;
        back.setTempo(240.0);
        check(std::fabs(findClip(back, lane, clip)->startSeconds -
                        startBefore / 2.0) < 1e-3,
              "doubling the tempo halves where the curve sits in seconds");
    }

    // ── Re-pointing a copy leaves its shape alone ──
    {
        daw::EngineController a;
        a.initialize(48000, 512, /*openDevice=*/false);
        const std::string one = a.addTrack(daw::TrackKind::Audio, "One");
        const std::string two = a.addTrack(daw::TrackKind::Audio, "Two");
        daw::AutomationTarget target;
        target.kind = daw::AutomationTargetKind::TrackVolume;
        target.channelId = one;
        const std::string lane = a.addAutomationLane(one, target);
        const std::string clip = a.addAutomationClip(lane, target, 0.0, 4.0);
        std::vector<daw::AutomationPoint> shape;
        shape.push_back({0.0, 0.2, daw::AutomationSegment::Linear, 0.4});
        shape.push_back({4.0, 0.9, daw::AutomationSegment::Linear, 0.0});
        a.setAutomationPoints(lane, clip, shape);

        const std::string copy = a.duplicateClipAt(lane, clip, 8.0);
        check(!copy.empty(), "a curve duplicates");
        check(findClip(a, lane, copy)->automation.points == shape,
              "the copy carries the same shape");
        check(findClip(a, lane, copy)->id != clip, "with an id of its own");

        daw::AutomationTarget elsewhere = target;
        elsewhere.channelId = two;
        a.setAutomationTarget(lane, copy, elsewhere);
        check(findClip(a, lane, copy)->automation.target.channelId == two,
              "and can be pointed at another channel");
        check(findClip(a, lane, copy)->automation.points == shape,
              "without its shape being touched — the whole point of copying it");
        check(findClip(a, lane, clip)->automation.target.channelId == one,
              "and the original still drives what it did");

        // One lane, two clips, two different things automated.
        check(a.project().findTrack(lane)->clips.size() == 2,
              "one lane carries both");
    }

    // ── A volume curve is heard ──
    {
        daw::EngineController a;
        a.initialize(48000, 512, /*openDevice=*/false);
        a.setTempo(120.0);
        const std::string track = a.addTrack(daw::TrackKind::Audio, "Lead");
        a.importAudio(tonePath, track, 0.0);     // half a second of tone
        // Zero is a real static fader value, not a mute gate. Automation must
        // be able to raise it just as it can replace any other stored level.
        a.setTrackVolume(track, 0.0f);

        daw::AutomationTarget target;
        target.kind = daw::AutomationTargetKind::TrackVolume;
        target.channelId = track;
        const std::string lane = a.addAutomationLane(track, target);
        const std::string clip = a.addAutomationClip(lane, target, 0.0, 4.0);

        // Unity down to silence across the first beat (half a second at 120),
        // so the tone's own half-second is covered end to end.
        std::vector<daw::AutomationPoint> fade;
        fade.push_back({0.0, daw::normalizedFromGain(1.0),
                        daw::AutomationSegment::Linear, 0.0});
        fade.push_back({1.0, 0.0, daw::AutomationSegment::Linear, 0.0});
        a.setAutomationPoints(lane, clip, fade);

        const std::string path = (dir / "automated-volume.wav").string();
        check(a.exportMixdown(path, false).isOk(), "the automated mix renders");
        audio::platform::DecodedAudio out;
        audio::platform::decodeAudioFile(path, out);
        if (check(out.frames > 12000, "and is long enough to look at")) {
            // Peak over the first eighth of a second against the last eighth of
            // the tone: a curve that is playing makes these very different.
            const auto peakBetween = [&](size_t from, size_t to) {
                float peak = 0.0f;
                const size_t last = std::min<size_t>(to, size_t(out.frames));
                for (size_t f = from; f < last; ++f) {
                    peak = std::max(peak,
                                    std::fabs(out.interleaved[f * out.channels]));
                }
                return peak;
            };
            const float head = peakBetween(0, 6000);
            const float tail = peakBetween(18000, 24000);
            check(head > 0.3f, "the curve starts near unity");
            check(tail < head * 0.35f,
                  "and the fader really does come down across the clip");
        }
    }

    // ── A pan curve is heard and is available to the channel-strip UI ──
    {
        daw::EngineController a;
        a.initialize(48000, 512, /*openDevice=*/false);
        a.setTempo(120.0);
        const std::string track = a.addTrack(daw::TrackKind::Audio, "Pan");
        a.importAudio(tonePath, track, 0.0);

        daw::AutomationTarget target;
        target.kind = daw::AutomationTargetKind::TrackPan;
        target.channelId = track;
        const std::string lane = a.addAutomationLane(track, target);
        const std::string clip = a.addAutomationClip(lane, target, 0.0, 4.0);
        a.setAutomationPoints(
            lane, clip,
            {{0.0, 0.5, daw::AutomationSegment::Linear, 0.0},
             {1.0, 0.0, daw::AutomationSegment::Linear, 0.0}});

        a.seekSeconds(0.25);  // half way through the one-beat move
        const auto displayed = a.automationValueAtPlayhead(target);
        check(displayed && std::fabs(*displayed + 0.5) < 0.02,
              "the strip can read automated pan at the playhead");

        const std::string path = (dir / "automated-pan.wav").string();
        check(a.exportMixdown(path, false).isOk(), "the automated pan renders");
        audio::platform::DecodedAudio out;
        audio::platform::decodeAudioFile(path, out);
        if (check(out.channels >= 2 && out.frames > 18000,
                  "and produces a stereo result")) {
            const auto peakBetween = [&](int channel, size_t from, size_t to) {
                float peak = 0.0f;
                const size_t last = std::min<size_t>(to, size_t(out.frames));
                for (size_t f = from; f < last; ++f) {
                    peak = std::max(
                        peak,
                        std::fabs(out.interleaved[f * out.channels + channel]));
                }
                return peak;
            };
            const float leftTail = peakBetween(0, 18000, 24000);
            const float rightTail = peakBetween(1, 18000, 24000);
            check(leftTail > 0.3f, "the left side stays open");
            check(rightTail < leftTail * 0.35f,
                  "while pan automation closes the right side");
        }
    }

    // ── Folders: the plain kind and the summing kind ──
    //
    // A plain folder is a drawer: no channel, no fader, nothing in the mixer.
    // A summing folder is a bus with tracks in it, and filing a track inside
    // one is what routes it — nobody wires up a group by hand.
    {
        daw::EngineController f;
        f.initialize(48000, 512, /*openDevice=*/false);
        const std::string plain = f.addFolder(/*summing=*/false);
        const std::string group = f.addFolder(/*summing=*/true);
        const daw::TrackModel* plainTrack = f.project().findTrack(plain);
        const daw::TrackModel* groupTrack = f.project().findTrack(group);
        check(plainTrack && daw::isFolder(*plainTrack) && !plainTrack->summing,
              "a plain folder is a folder that does not sum");
        check(groupTrack && daw::isSummingFolder(*groupTrack),
              "a summing folder is a folder that does");
        check(plainTrack && !daw::carriesAudio(*plainTrack),
              "a plain folder has no channel at all");
        check(groupTrack && daw::carriesAudio(*groupTrack),
              "a summing folder does have one");
        check(f.trackNodes(plain) == nullptr,
              "so the engine builds no strip for a plain folder");
        check(f.trackNodes(group) != nullptr &&
                  f.trackNodes(group)->fader != daw::engine::kInvalidNode,
              "and does build one for a summing folder");

        // Filing a track into each kind.
        const std::string inPlain = f.addTrack(daw::TrackKind::Audio, "Loose");
        const std::string inGroup = f.addTrack(daw::TrackKind::Audio, "Grouped");
        f.moveTrackToFolder(inPlain, plain);
        f.moveTrackToFolder(inGroup, group);
        check(f.project().findTrack(inPlain)->outputBusId.empty(),
              "a plain folder does not touch its children's routing");
        check(f.project().findTrack(inGroup)->outputBusId == group,
              "a summing folder routes its children into itself");

        // Taking it back out again returns it to the master.
        f.moveTrackToFolder(inGroup, "");
        check(f.project().findTrack(inGroup)->outputBusId.empty(),
              "and lets go when the track leaves");

        // A route the user made by hand is theirs, not the folder's.
        const std::string bus = f.addTrack(daw::TrackKind::Bus, "Reverb");
        check(f.setTrackOutputBus(inGroup, bus), "the track is routed by hand");
        f.moveTrackToFolder(inGroup, group);
        check(f.project().findTrack(inGroup)->outputBusId == bus,
              "filing it into a summing folder leaves that routing alone");
    }

    // ── Summing can be turned on and off after the fact ──
    {
        daw::EngineController f;
        f.initialize(48000, 512, /*openDevice=*/false);
        const std::string folder = f.addFolder(/*summing=*/false);
        const std::string child = f.addTrack(daw::TrackKind::Audio, "Child");
        f.moveTrackToFolder(child, folder);
        check(f.project().findTrack(child)->outputBusId.empty(),
              "the child starts out feeding the master");

        f.setFolderSumming(folder, true);
        check(f.project().findTrack(child)->outputBusId == folder,
              "turning summing on pulls the child into the new bus");
        check(f.trackNodes(folder) != nullptr,
              "and the folder grows a channel");

        f.setFolderSumming(folder, false);
        check(f.project().findTrack(child)->outputBusId.empty(),
              "turning it off puts the child back on the master");
        check(f.trackNodes(folder) == nullptr, "and takes the channel away");
    }

    // ── A summing folder is heard: its fader is the group's fader ──
    {
        daw::EngineController f;
        f.initialize(48000, 512, /*openDevice=*/false);
        const std::string group = f.addFolder(/*summing=*/true, "Drums");
        const std::string kick = f.addTrack(daw::TrackKind::Audio, "Kick");
        f.importAudio(tonePath, kick, 0.0);
        f.moveTrackToFolder(kick, group);

        const std::string openPath = (dir / "folder-open.wav").string();
        check(f.exportMixdown(openPath, false).isOk(),
              "exports through the summing folder");
        audio::platform::DecodedAudio open;
        audio::platform::decodeAudioFile(openPath, open);
        const float openPeak = std::max(channelPeak(open, 0), channelPeak(open, 1));
        check(openPeak > 0.1f, "the group passes its track to the master");

        f.setTrackVolume(group, 0.0f);
        const std::string shutPath = (dir / "folder-shut.wav").string();
        f.exportMixdown(shutPath, false);
        audio::platform::DecodedAudio shut;
        audio::platform::decodeAudioFile(shutPath, shut);
        check(std::max(channelPeak(shut, 0), channelPeak(shut, 1)) < 0.001f,
              "and pulling the group's fader down silences everything in it");
    }

    // ── Soloing a folder solos what is in it ──
    {
        daw::EngineController f;
        f.initialize(48000, 512, /*openDevice=*/false);
        const std::string folder = f.addFolder(/*summing=*/false, "Drawer");
        const std::string inside = f.addTrack(daw::TrackKind::Audio, "Inside");
        const std::string outside = f.addTrack(daw::TrackKind::Audio, "Outside");
        f.importAudio(tonePath, inside, 0.0);
        f.importAudio(tonePath, outside, 0.0);
        f.moveTrackToFolder(inside, folder);

        f.setTrackSoloed(folder, true);
        const std::string soloPath = (dir / "folder-solo.wav").string();
        f.exportMixdown(soloPath, false);
        audio::platform::DecodedAudio solo;
        audio::platform::decodeAudioFile(soloPath, solo);
        // A plain folder has no channel of its own, so a solo that only opened
        // the folder would mute the whole project — including the very track
        // the user meant to hear.
        check(std::max(channelPeak(solo, 0), channelPeak(solo, 1)) > 0.1f,
              "soloing a plain folder keeps the track inside it audible");

        f.setTrackMuted(outside, true);
        f.setTrackSoloed(folder, false);
        f.setTrackMuted(outside, false);
        f.setTrackSoloed(outside, true);
        const std::string otherPath = (dir / "folder-solo-other.wav").string();
        f.exportMixdown(otherPath, false);
        audio::platform::DecodedAudio other;
        audio::platform::decodeAudioFile(otherPath, other);
        const float otherPeak =
            std::max(channelPeak(other, 0), channelPeak(other, 1));
        f.setTrackSoloed(outside, false);
        const std::string bothPath = (dir / "folder-solo-both.wav").string();
        f.exportMixdown(bothPath, false);
        audio::platform::DecodedAudio both;
        audio::platform::decodeAudioFile(bothPath, both);
        check(std::max(channelPeak(both, 0), channelPeak(both, 1)) >
                  otherPeak * 1.5f,
              "and soloing something else does silence the folder's contents");
    }

    // ── A folder's colour is the group's colour ──
    {
        daw::EngineController f;
        f.initialize(48000, 512, /*openDevice=*/false);
        const std::string folder = f.addFolder(/*summing=*/false);
        const std::string inner = f.addFolder(/*summing=*/true);
        const std::string leaf = f.addTrack(daw::TrackKind::Audio, "Leaf");
        f.importAudio(tonePath, leaf, 0.0);
        f.moveTrackToFolder(inner, folder);
        f.moveTrackToFolder(leaf, inner);

        f.setTrackColor(folder, 0xE0674A);
        check(f.project().findTrack(inner)->color == 0xE0674Au,
              "recolouring a folder recolours the folders inside it");
        check(f.project().findTrack(leaf)->color == 0xE0674Au,
              "and every track below them, however deep");
        check(f.project().findTrack(leaf)->clips[0].color == 0xE0674Au,
              "clips included, like any other recolour");

        // A track outside the folder is not the folder's business.
        const std::string outside = f.addTrack(daw::TrackKind::Audio, "Outside");
        const uint32_t before = f.project().findTrack(outside)->color;
        f.setTrackColor(folder, 0x3B82F6);
        check(f.project().findTrack(outside)->color == before,
              "and nothing outside it changes");
    }

    // ── Folders survive a save and a reload ──
    {
        daw::EngineController f;
        f.initialize(48000, 512, /*openDevice=*/false);
        const std::string plain = f.addFolder(/*summing=*/false, "Drawer");
        const std::string group = f.addFolder(/*summing=*/true, "Group");
        const std::string child = f.addTrack(daw::TrackKind::Audio, "Child");
        f.moveTrackToFolder(child, group);
        const std::string pkg = (dir / "folders.vlt").string();
        check(f.saveProject(pkg).isOk(), "project with folders saves");

        daw::EngineController g;
        g.initialize(48000, 512, /*openDevice=*/false);
        check(g.openProject(pkg).isOk(), "and reloads");
        const auto* reloadedPlain = g.project().findTrack(plain);
        const auto* reloadedGroup = g.project().findTrack(group);
        check(reloadedPlain && daw::isFolder(*reloadedPlain) &&
                  !reloadedPlain->summing,
              "the plain folder comes back plain");
        check(reloadedGroup && daw::isSummingFolder(*reloadedGroup),
              "and the summing folder comes back summing");
        check(g.project().findTrack(child)->outputBusId == group,
              "with its child still routed into it");
        check(g.trackNodes(group) != nullptr && g.trackNodes(plain) == nullptr,
              "and only one of them holding a channel");
    }

    // ── Packing a selection into a folder ──
    {
        daw::EngineController f;
        f.initialize(48000, 512, /*openDevice=*/false);
        const std::string a = f.addTrack(daw::TrackKind::Audio, "A");
        const std::string b = f.addTrack(daw::TrackKind::Audio, "B");
        const std::string c = f.addTrack(daw::TrackKind::Audio, "C");
        const std::string folder = f.packIntoFolder({a, c}, "Pair",
                                                    /*summing=*/true);
        check(!folder.empty(), "packs a selection into a summing folder");
        check(f.project().findTrack(a)->parentId == folder &&
                  f.project().findTrack(c)->parentId == folder,
              "the chosen tracks are inside it");
        check(f.project().findTrack(b)->parentId.empty(),
              "and the one that was not chosen is not");
        check(f.project().findTrack(a)->outputBusId == folder &&
                  f.project().findTrack(c)->outputBusId == folder,
              "packing routes them into the new bus");

        f.undo();
        check(f.project().findTrack(folder) == nullptr,
              "undo removes the folder");
        check(f.project().findTrack(a)->parentId.empty() &&
                  f.project().findTrack(a)->outputBusId.empty(),
              "and puts its tracks back where they were");
    }

    // ── Move a clip to another track ──
    {
        daw::EngineController r;
        r.initialize(48000, 512, /*openDevice=*/false);
        const std::string a = r.addTrack(daw::TrackKind::Audio, "A");
        const std::string b = r.addTrack(daw::TrackKind::Audio, "B");
        const std::string bus = r.addTrack(daw::TrackKind::Bus, "Bus");
        const std::string clip = r.importAudio(tonePath, a, 1.0);

        r.moveClipToTrack(a, clip, b);
        check(findClip(r, a, clip) == nullptr && findClip(r, b, clip) != nullptr,
              "moveClipToTrack moves the clip between audio tracks");
        const daw::ClipModel* moved = findClip(r, b, clip);
        check(moved && std::fabs(moved->startSeconds - 1.0) < 1e-6,
              "moved clip keeps its timeline position");

        // A bus is not an audio lane, so the move is refused.
        r.moveClipToTrack(b, clip, bus);
        check(findClip(r, b, clip) != nullptr && findClip(r, bus, clip) == nullptr,
              "moveClipToTrack refuses non-audio destinations");
    }

    // ── Fades survive save / reload ──
    {
        daw::EngineController r;
        r.initialize(48000, 512, /*openDevice=*/false);
        const std::string track = r.addTrack(daw::TrackKind::Audio, "Persist");
        const std::string clip = r.importAudio(tonePath, track, 0.0);
        r.setClipFade(track, clip, 0.05, 0.07);
        r.setClipFadeCurve(track, clip, true, 0.5);
        r.setClipFadeCurve(track, clip, false, -0.25);
        r.setClipFadeMode(track, clip, true, daw::ClipFadeMode::Tape);
        r.setClipFadeMode(track, clip, false, daw::ClipFadeMode::Tape);

        const std::string pkg = (dir / "fades.vlt").string();
        check(r.saveProject(pkg).isOk(), "save project with fades");

        daw::EngineController r2;
        r2.initialize(48000, 512, /*openDevice=*/false);
        check(r2.openProject(pkg).isOk(), "reopen project with fades");
        const daw::TrackModel& tr = r2.project().tracks.front();
        check(!tr.clips.empty() &&
                  std::fabs(tr.clips.front().fadeInSeconds - 0.05) < 1e-6 &&
                  std::fabs(tr.clips.front().fadeOutSeconds - 0.07) < 1e-6 &&
                  std::fabs(tr.clips.front().fadeInCurve - 0.5) < 1e-6 &&
                  std::fabs(tr.clips.front().fadeOutCurve + 0.25) < 1e-6 &&
                  tr.clips.front().fadeInMode == daw::ClipFadeMode::Tape &&
                  tr.clips.front().fadeOutMode == daw::ClipFadeMode::Tape,
              "fades round-trip through save/reload");
    }

    // ── Per-instance Sample / Clip Editor state (project format v4) ──
    {
        daw::EngineController r;
        r.initialize(48000, 512, /*openDevice=*/false);
        const std::string track = r.addTrack(daw::TrackKind::Audio, "Clip Edit");
        const std::string original = r.importAudio(tonePath, track, 0.0);
        const std::string copy = r.duplicateClip(track, original);

        // Duplicate first, then edit only one side: both clips point at the same
        // WAV, but their editor state is owned by the clip instance.
        r.setClipSampleParameter(track, original, "stretch.mode", 3.0);
        r.setClipSampleParameter(track, original, "stretch.time", 2.0);
        const daw::ClipModel* edited = findClip(r, track, original);
        const daw::ClipModel* untouched = findClip(r, track, copy);
        check(edited && untouched &&
                  std::fabs(edited->durationSeconds - 1.0) < 1e-6 &&
                  std::fabs(untouched->durationSeconds - 0.5) < 1e-6 &&
                  int(edited->sampleEdit.stretchMode) == 3 &&
                  int(untouched->sampleEdit.stretchMode) == 0,
              "two clips from one WAV keep independent stretch settings");

        const double durationAfterTime = edited ? edited->durationSeconds : 0.0;
        r.setClipSampleParameter(track, original, "stretch.pitch", 7.0);
        edited = findClip(r, track, original);
        check(edited && std::fabs(edited->durationSeconds - durationAfterTime) < 1e-9,
              "clip pitch changes independently of its timeline duration");

        const double formantBefore =
            r.clipSampleParameter(track, original, "formant");
        r.setClipSampleParameter(track, original, "formant", -4.0);
        r.commitClipSampleParameterEdit(track, original, "formant",
                                        formantBefore, "Edit Clip Formant");
        r.undo();
        check(std::fabs(r.clipSampleParameter(track, original, "formant")) < 1e-9,
              "one clip-editor gesture is one undo operation");
        r.redo();
        check(std::fabs(r.clipSampleParameter(track, original, "formant") + 4.0) < 1e-9,
              "redo restores the per-clip formant edit");

        // Looping changes the permissible timeline length: the playback region
        // may repeat beyond the end of its source file.
        r.setClipSampleParameter(track, original, "loop.mode", 1.0);
        r.setClipTrim(track, original, 0.0, 0.0, 2.0);
        edited = findClip(r, track, original);
        check(edited && std::fabs(edited->durationSeconds - 2.0) < 1e-9,
              "a looped clip can extend beyond the source duration");

        r.setClipSampleParameter(track, original, "pre.reverse", 1.0);
        r.setClipSampleParameter(track, original, "rootnote", 65.0);
        check(r.clipSampleData(track, original) != nullptr,
              "clip processing produces editor/playback sample data");

        const std::string pkg = (dir / "clip-editor-v4.vlt").string();
        check(r.saveProject(pkg).isOk(), "save project with per-clip editor state");
        daw::EngineController reopened;
        reopened.initialize(48000, 512, false);
        check(reopened.openProject(pkg).isOk(),
              "reopen project with per-clip editor state");
        const daw::ClipModel* savedEdited = findClip(reopened, track, original);
        const daw::ClipModel* savedUntouched = findClip(reopened, track, copy);
        check(savedEdited && savedUntouched &&
                  int(savedEdited->sampleEdit.stretchMode) == 3 &&
                  std::fabs(savedEdited->sampleEdit.stretchTime - 2.0) < 1e-9 &&
                  std::fabs(savedEdited->sampleEdit.stretchPitch - 7.0) < 1e-9 &&
                  std::fabs(savedEdited->sampleEdit.formant + 4.0) < 1e-9 &&
                  savedEdited->sampleEdit.rootNote == 65 &&
                  savedEdited->sampleEdit.reverse &&
                  int(savedUntouched->sampleEdit.stretchMode) == 0 &&
                  std::fabs(savedUntouched->sampleEdit.stretchTime - 1.0) < 1e-9,
              "v4 save/reload preserves distinct edits for identical source media");

        // Baking a precomputed effect costs a pass over the whole file, so it
        // must happen when — and only when — one of those controls moves. The
        // stretch, the pitch and the loop points are applied while the clip
        // plays; re-rendering the sample for them made every mouse move of the
        // Time knob a full re-render, which is what froze the interface.
        const auto bakedOnce = r.clipSampleData(track, original);
        r.setClipSampleParameter(track, original, "stretch.time", 1.75);
        r.setClipSampleParameter(track, original, "stretch.pitch", -3.0);
        r.setClipSampleParameter(track, original, "loop.start", 0.2);
        r.setClipSampleParameter(track, original, "formant", 2.0);
        check(r.clipSampleData(track, original) == bakedOnce,
              "playback controls reuse the clip's rendered sample");
        r.setClipSampleParameter(track, original, "pre.boost", 0.3);
        const auto bakedAgain = r.clipSampleData(track, original);
        check(bakedAgain && bakedAgain != bakedOnce,
              "and a precomputed one renders a new sample");

        // Stretching a clip moves its end along the timeline, so the Time
        // control is pulled toward the grid: near a line it lands exactly on
        // it, and between lines it stays where the pointer put it.
        {
            daw::EngineController g;
            g.initialize(48000, 512, /*openDevice=*/false);
            const std::string gt = g.addTrack(daw::TrackKind::Audio, "Grid");
            const std::string gc = g.importAudio(tonePath, gt, 0.0);
            const double grid = 0.25;          // source is half a second long
            const auto at = [&](double wanted) {
                return g.snappedStretchTime(gt, gc, wanted, grid);
            };
            check(std::fabs(at(1.02) - 1.0) < 1e-9 &&
                      std::fabs(at(1.45) - 1.5) < 1e-9,
                  "the stretch knob is pulled onto a nearby grid line");
            check(std::fabs(at(1.2) - 1.2) < 1e-9,
                  "and left alone between them");
            check(std::fabs(g.snappedStretchTime(gt, gc, 1.02, 0.0) - 1.02) < 1e-9,
                  "with snapping off it never moves");
            // A grid far finer than the knob can resolve must not quantise the
            // whole travel: the detent shrinks with the gap instead.
            check(std::fabs(g.snappedStretchTime(gt, gc, 1.05, 0.01) - 1.05) < 1e-9,
                  "a grid finer than the detent leaves most of the travel free");
            check(std::fabs(g.snappedStretchTime(gt, gc, 3.99, grid) - 4.0) < 1e-9 &&
                      std::fabs(g.snappedStretchTime(gt, gc, 8.0, grid) - 4.0) < 1e-9,
                  "and the result stays inside the knob's range");
        }

        // The re-render is deferred to the next control-thread turn, so an
        // export has to force it — otherwise the file would be one edit behind.
        {
            daw::EngineController d;
            d.initialize(48000, 512, /*openDevice=*/false);
            const std::string dt = d.addTrack(daw::TrackKind::Audio, "Deferred");
            const std::string dc = d.importAudio(tonePath, dt, 0.0);
            const std::string quiet = (dir / "deferred-bake.wav").string();
            d.setClipSampleParameter(dt, dc, "pre.cut", 0.0);
            check(d.exportMixdown(quiet, false).isOk(), "deferred-bake mixdown renders");
            audio::platform::DecodedAudio rendered;
            check(audio::platform::decodeAudioFile(quiet, rendered).isOk(),
                  "deferred-bake mixdown decodes");
            check(channelPeak(rendered, 0) < 0.05f,
                  "an export renders the pending bake instead of the old sample");
        }
    }

    // ── Duplicate track ──
    {
        daw::EngineController r;
        r.initialize(48000, 512, /*openDevice=*/false);
        const std::string src = r.addTrack(daw::TrackKind::Audio, "Src");
        r.importAudio(tonePath, src, 0.5);
        r.ensureInsertSlots(src, 2);

        const std::string dup = r.duplicateTrack(src, /*withInserts=*/true);
        check(!dup.empty() && r.project().tracks.size() == 2,
              "duplicateTrack adds a track");
        const daw::TrackModel* d = r.project().findTrack(dup);
        const daw::TrackModel* s = r.project().findTrack(src);
        check(d && s && d->clips.size() == s->clips.size() &&
                  !d->clips.empty() && d->clips.front().id != s->clips.front().id,
              "duplicate copies clips with fresh ids");
        check(d && !d->inserts.empty(), "duplicate keeps inserts by default");

        const std::string dupNoFx = r.duplicateTrack(src, /*withInserts=*/false);
        const daw::TrackModel* d2 = r.project().findTrack(dupNoFx);
        check(d2 && d2->inserts.empty(),
              "duplicate without plugins drops the inserts");

        r.undo();
        check(r.project().findTrack(dupNoFx) == nullptr,
              "duplicate is undoable");
    }

    // ── Track height ──
    {
        daw::EngineController r;
        r.initialize(48000, 512, /*openDevice=*/false);
        const std::string t = r.addTrack(daw::TrackKind::Audio, "Tall");
        r.setTrackHeight(t, 200.0);
        check(std::fabs(r.project().findTrack(t)->height - 200.0) < 1e-6,
              "setTrackHeight applies");
        r.setTrackHeight(t, 5.0);   // below the clamp floor
        check(r.project().findTrack(t)->height >= 30.0,
              "track height clamps to a sane minimum");
    }

    // ── Metronome renders clicks ──
    {
        daw::EngineController r;
        r.initialize(48000, 512, /*openDevice=*/false);
        const std::string t = r.addTrack(daw::TrackKind::Audio, "Click");
        // A clip only to give the timeline a length to render.
        r.importAudio(tonePath, t, 2.0);       // silence before 2 s
        r.setTempo(120.0);                     // beats at 0, 0.5, 1.0, 1.5 s

        check(!r.isMetronomeEnabled(), "metronome starts off");
        r.setMetronomeEnabled(true);
        check(r.isMetronomeEnabled(), "metronome toggles on");

        const std::string clickPath = (dir / "click.wav").string();
        check(r.exportMixdown(clickPath, false).isOk(), "metronome mixdown renders");
        platform::DecodedAudio click;
        check(platform::decodeAudioFile(clickPath, click).isOk(),
              "metronome mixdown decodes");
        // The region right after the 0.5 s beat (before the 2 s clip) must carry
        // the click — it would be silent without the metronome.
        float clickPeak = 0.0f;
        if (click.channels >= 1) {
            const size_t from = size_t(0.5 * click.sampleRate);
            const size_t to = size_t(0.52 * click.sampleRate);
            for (size_t f = from; f < to && f < click.frames; ++f)
                clickPeak = std::max(clickPeak,
                    std::fabs(click.interleaved[f * click.channels]));
        }
        check(clickPeak > 0.05f, "metronome writes a click on the beat");
    }

    // ── Auditioning a file from the browser ──
    //
    // The engine-level behaviour of the node is checked in engine_graph_test;
    // what matters here is the controller's contract: it arms and disarms, it
    // refuses what will not decode, and — the one that costs memory if it is
    // wrong — an audition is never retained.
    {
        daw::EngineController r;
        r.initialize(48000, 512, /*openDevice=*/false);
        const std::string t = r.addTrack(daw::TrackKind::Audio, "Audio");
        r.importAudio(tonePath, t, 0.0);
        const size_t heldForTheProject = r.decodedFileCount();

        check(!r.previewPlaying(), "nothing is auditioning to begin with");
        check(r.previewFile(tonePath, /*loop=*/false), "a file can be auditioned");
        check(r.previewPath() == tonePath, "and it says which one");
        check(r.previewDurationSeconds() > 0.4 && r.previewDurationSeconds() < 0.6,
              "with the file's own length");
        check(!r.previewFile((dir / "not-here.wav").string(), false),
              "a file that will not decode is refused, not silently ignored");

        // The audition is the point of this: decoded, played, and forgotten.
        // Routing it through the project's sample memo would pin every file the
        // user ever clicked in the browser.
        r.previewFile(tonePath, false);
        r.previewFile(tonePath, true);
        check(r.decodedFileCount() == heldForTheProject,
              "auditioning holds on to nothing");

        r.stopPreview();
        check(r.previewPositionSeconds() >= 0.0,
              "the audition position is readable while stopped");

        // Starting the transport must not leave a preview running underneath it.
        r.previewFile(tonePath, true);
        r.play();
        check(!r.previewPlaying(), "starting playback stops an audition");
        r.stop();
    }

    // ── Space play/pause modes ──
    // The play/pause (Space) behaviour is a switchable transport mode: Resume
    // continues from wherever the playhead stopped, Restart returns to the
    // position where the current run started.
    {
        daw::EngineController t;
        t.initialize(48000, 512, /*openDevice=*/false);
        using Mode = daw::EngineController::PlaybackMode;
        check(t.playbackMode() == Mode::Resume, "playback defaults to resume");

        // Restart: a seek while rolling doesn't move the anchor, so pause + play
        // returns to the run start rather than the mid-run scroll.
        t.setPlaybackMode(Mode::Restart);
        t.seekSeconds(3.0);   // anchor = 3.0
        t.play();
        t.seekSeconds(9.0);   // scrolled forward mid-run
        t.pause();
        check(std::fabs(t.positionSeconds() - 9.0) < 1e-6,
              "pause holds the playhead in place");
        t.play();
        check(std::fabs(t.positionSeconds() - 3.0) < 1e-6,
              "restart mode returns to the run start on play");

        // A seek while paused is a new run: it re-anchors the restart.
        t.pause();
        t.seekSeconds(5.0);
        t.play();
        check(std::fabs(t.positionSeconds() - 5.0) < 1e-6,
              "a seek while paused re-anchors the restart");

        // Resume: the same sequence continues from the paused position.
        t.setPlaybackMode(Mode::Resume);
        t.seekSeconds(3.0);
        t.play();
        t.seekSeconds(9.0);
        t.pause();
        t.play();
        check(std::fabs(t.positionSeconds() - 9.0) < 1e-6,
              "resume mode continues from the paused position");
    }

    // ── MIDI clips and notes ──
    // The whole MIDI model layer is exercised here, with no UI at all: what a
    // MIDI clip is, which lanes accept it, the note edit ops and their clamps,
    // and which of them the undo stack records.
    {
        daw::EngineController m;
        m.initialize(48000, 512, /*openDevice=*/false);

        const std::string midiTrack = m.addTrack(daw::TrackKind::Midi, "Keys");
        const std::string audioTrack = m.addTrack(daw::TrackKind::Audio, "Aud");

        // A clip with no explicit length is one bar — 2 s at 4/4, 120 bpm.
        const std::string clipId = m.addMidiClip(midiTrack, 2.0);
        check(!clipId.empty(), "addMidiClip creates a clip on a MIDI track");
        check(m.addMidiClip(audioTrack, 0.0).empty(),
              "addMidiClip refuses an audio track");

        auto midiClip = [&](const std::string& trackId, const std::string& id) {
            return findClip(m, trackId, id);
        };

        const daw::ClipModel* clip = midiClip(midiTrack, clipId);
        check(clip && clip->kind == daw::ClipKind::Midi,
              "a new MIDI clip is marked as MIDI");
        check(clip && std::fabs(clip->durationSeconds - 2.0) < 1e-9,
              "a new MIDI clip is one bar long at 4/4, 120 bpm");
        // Proves syncTrackClips and updateTimelineDuration both cope with a
        // clip that has no audio behind it.
        check(m.durationSeconds() >= 4.0,
              "a MIDI clip extends the timeline duration");

        const std::string noteId = m.addNote(midiTrack, clipId, 60, 0.0, 1.0, 100);
        check(!noteId.empty() && midiClip(midiTrack, clipId)->notes.size() == 1,
              "addNote appends a note");

        m.setNote(midiTrack, clipId, noteId, 64, 1.5, 0.5);
        {
            const auto& n = midiClip(midiTrack, clipId)->notes[0];
            check(n.pitch == 64 && std::fabs(n.startBeats - 1.5) < 1e-9 &&
                      std::fabs(n.lengthBeats - 0.5) < 1e-9,
                  "setNote moves and resizes a note");
        }
        m.setNote(midiTrack, clipId, noteId, 999, -5.0, 0.0);
        {
            const auto& n = midiClip(midiTrack, clipId)->notes[0];
            check(n.pitch == 127 && n.startBeats == 0.0 &&
                      n.lengthBeats >= 1.0 / 32.0,
                  "setNote clamps pitch, start and length");
        }
        m.setNoteVelocity(midiTrack, clipId, noteId, 300);
        check(midiClip(midiTrack, clipId)->notes[0].velocity == 127,
              "setNoteVelocity clamps to 127");

        // One undo takes the whole note back: the moves and resizes in between
        // are live edits and are deliberately not on the stack.
        m.undo();
        check(midiClip(midiTrack, clipId)->notes.empty(),
              "undo removes the added note — live edits aren't recorded");
        m.redo();
        {
            const auto& notes = midiClip(midiTrack, clipId)->notes;
            // Same id, or this entry's own undo would be pointing at nothing.
            check(notes.size() == 1 && notes[0].id == noteId,
                  "redo restores the note under the same id");
            // Redo replays the note as it was *created*: the moves in between
            // were live edits and were never recorded. Same bargain the
            // arrangement already makes for a clip drag.
            check(notes[0].pitch == 60 && notes[0].startBeats == 0.0,
                  "redo replays the note as created, not as later dragged");
        }

        // removeNote snapshots whatever the note currently is, so an undo has
        // to bring back that exact state — including live edits made since.
        m.setNote(midiTrack, clipId, noteId, 71, 3.25, 0.5);
        m.setNoteVelocity(midiTrack, clipId, noteId, 42);
        m.removeNote(midiTrack, clipId, noteId);
        check(midiClip(midiTrack, clipId)->notes.empty(), "removeNote");
        m.undo();
        {
            const auto& notes = midiClip(midiTrack, clipId)->notes;
            check(notes.size() == 1 && notes[0].id == noteId &&
                      notes[0].pitch == 71 && notes[0].velocity == 42 &&
                      std::fabs(notes[0].startBeats - 3.25) < 1e-9,
                  "undo restores the note's exact state");
        }

        // Clip families: a MIDI clip belongs on a MIDI lane and nowhere else.
        const std::string midiTrack2 = m.addTrack(daw::TrackKind::Midi, "Keys 2");
        m.moveClipToTrack(midiTrack, clipId, audioTrack);
        check(midiClip(midiTrack, clipId) != nullptr,
              "a MIDI clip can't move to an audio lane");
        m.moveClipToTrack(midiTrack, clipId, midiTrack2);
        check(midiClip(midiTrack2, clipId) != nullptr &&
                  midiClip(midiTrack, clipId) == nullptr,
              "a MIDI clip moves between MIDI lanes");

        // Splitting has to divide the notes, not copy them into both halves.
        const std::string longClip = m.addMidiClip(midiTrack, 0.0, 4.0);  // 2 bars
        m.addNote(midiTrack, longClip, 60, 0.0, 1.0);
        m.addNote(midiTrack, longClip, 67, 6.0, 1.0);
        const std::string rightId = m.splitClip(midiTrack, longClip, 2.0);
        check(!rightId.empty(), "a MIDI clip splits");
        {
            const auto* left = midiClip(midiTrack, longClip);
            const auto* right = midiClip(midiTrack, rightId);
            check(left && left->notes.size() == 1 && left->notes[0].pitch == 60,
                  "the left half keeps only the notes before the cut");
            check(right && right->notes.size() == 1 &&
                      right->notes[0].pitch == 67 &&
                      std::fabs(right->notes[0].startBeats - 2.0) < 1e-9,
                  "the right half's notes are rebased to its own start");
            check(left && right && left->notes[0].id != right->notes[0].id,
                  "the two halves don't share note ids");
        }
        m.undo();
        check(midiClip(midiTrack, longClip)->notes.size() == 2 &&
                  midiClip(midiTrack, rightId) == nullptr,
              "undoing a split puts every note back on one clip");
        m.redo();
        check(midiClip(midiTrack, longClip)->notes.size() == 1 &&
                  midiClip(midiTrack, rightId) != nullptr,
              "redoing a split divides them again");
    }

    // ── Instrument slot (document-only placeholder) ──
    {
        daw::EngineController m;
        m.initialize(48000, 512, /*openDevice=*/false);
        const std::string midiTrack = m.addTrack(daw::TrackKind::Midi, "Keys");
        const std::string audioTrack = m.addTrack(daw::TrackKind::Audio, "Aud");
        auto instrumentOf = [&](const std::string& id) {
            const auto* t = m.project().findTrack(id);
            return t ? t->instrument.name : std::string("<missing>");
        };

        check(instrumentOf(midiTrack).empty(), "a new MIDI track has no instrument");
        m.setTrackInstrument(midiTrack, "Sampler");
        check(instrumentOf(midiTrack) == "Sampler", "setTrackInstrument assigns");
        const std::string slotId =
            m.project().findTrack(midiTrack)->instrument.id;
        check(!slotId.empty(), "an assigned instrument gets a slot id");

        m.setTrackInstrument(midiTrack, "Synth");
        check(instrumentOf(midiTrack) == "Synth" &&
                  m.project().findTrack(midiTrack)->instrument.id == slotId,
              "swapping the instrument keeps the slot id");

        m.setTrackInstrument(audioTrack, "Sampler");
        check(instrumentOf(audioTrack).empty(),
              "an audio track refuses an instrument");

        m.undo();
        check(instrumentOf(midiTrack) == "Sampler", "undo restores the previous instrument");
        m.redo();
        check(instrumentOf(midiTrack) == "Synth", "redo re-applies it");

        m.setTrackInstrument(midiTrack, "");
        check(instrumentOf(midiTrack).empty(), "an empty name clears the slot");
    }

    // ── MIDI serialization round-trip ──
    {
        daw::EngineController m;
        m.initialize(48000, 512, /*openDevice=*/false);
        const std::string trackId = m.addTrack(daw::TrackKind::Midi, "Keys");
        m.setTrackInstrument(trackId, "Sampler");
        const std::string clipId = m.addMidiClip(trackId, 1.0);
        const std::string noteId = m.addNote(trackId, clipId, 72, 2.25, 0.75, 88);

        const std::string package = (dir / "midi.vlt").string();
        check(bool(daw::ProjectSerializer::save(m.project(), package)),
              "a project with MIDI saves");

        daw::ProjectModel reloaded;
        check(bool(daw::ProjectSerializer::load(reloaded, package)),
              "a project with MIDI loads");
        const auto* track = reloaded.findTrack(trackId);
        check(track && track->clips.size() == 1, "the MIDI clip survives a reload");
        check(track && track->instrument.name == "Sampler",
              "the instrument slot survives a reload");
        if (track && !track->clips.empty()) {
            const auto& c = track->clips[0];
            check(c.kind == daw::ClipKind::Midi, "the clip reloads as MIDI");
            check(c.notes.size() == 1, "its note survives");
            if (!c.notes.empty()) {
                const auto& n = c.notes[0];
                check(n.id == noteId && n.pitch == 72 && n.velocity == 88 &&
                          std::fabs(n.startBeats - 2.25) < 1e-9 &&
                          std::fabs(n.lengthBeats - 0.75) < 1e-9,
                      "every note field round-trips exactly");
            }
        }
    }

    // A plain audio clip must still reload as audio with no notes — that is the
    // guarantee that projects written before MIDI existed still open correctly,
    // since they carry no "kind" key at all and fall back to the default.
    {
        daw::ProjectModel reloaded;
        check(bool(daw::ProjectSerializer::load(reloaded, pkg)),
              "the earlier audio-only project still loads");
        bool sawAudioClip = false;
        for (const auto& t : reloaded.tracks) {
            for (const auto& c : t.clips) {
                sawAudioClip = true;
                if (c.kind != daw::ClipKind::Audio || !c.notes.empty()) {
                    sawAudioClip = false;
                    break;
                }
            }
        }
        check(sawAudioClip, "audio clips reload as audio with no notes");
    }

    // ── Comp map (model helpers) ──
    // These run on a bare ClipModel: the comp map is pure data, and every
    // controller edit funnels through them, so testing them here covers the
    // swipe/select behaviour without needing a recording.
    {
        daw::ClipModel clip;
        clip.id = "c";
        clip.durationSeconds = 10.0;
        for (int i = 1; i <= 3; ++i) {
            daw::TakeModel take;
            take.id = "t" + std::to_string(i);
            take.name = "Take " + std::to_string(i);
            take.lengthSeconds = 10.0;
            clip.takes.push_back(take);
        }
        check(daw::isLayered(clip), "a clip with takes is layered");
        check(daw::findTake(clip, "t2") != nullptr, "takes are findable by id");
        check(daw::findTake(clip, "nope") == nullptr,
              "an unknown take id finds nothing");

        daw::selectWholeTake(clip, "t1");
        check(clip.comp.size() == 1 && clip.comp[0].takeId == "t1" &&
                  std::fabs(clip.comp[0].endSeconds - 10.0) < 1e-9,
              "selecting a take covers the whole clip");
        check(daw::activeTakeAt(clip, 5.0) == "t1", "the comp plays that take");

        // A swipe in the middle splits the segment into three.
        daw::setCompRange(clip, "t2", 4.0, 6.0);
        check(clip.comp.size() == 3, "a swipe splits the segment it lands in");
        check(daw::activeTakeAt(clip, 2.0) == "t1" &&
                  daw::activeTakeAt(clip, 5.0) == "t2" &&
                  daw::activeTakeAt(clip, 8.0) == "t1",
              "the swept stretch plays the brushed take, the rest is unchanged");

        // Painting the same take over a neighbour merges rather than accumulates.
        daw::setCompRange(clip, "t2", 6.0, 8.0);
        check(clip.comp.size() == 3 &&
                  std::fabs(clip.comp[1].endSeconds - 8.0) < 1e-9,
              "an adjoining swipe of the same take merges into one segment");

        // Swiping backwards is the same gesture as swiping forwards.
        daw::setCompRange(clip, "t3", 3.0, 1.0);
        check(daw::activeTakeAt(clip, 2.0) == "t3",
              "a right-to-left swipe paints the same range");

        // Past the ends: a swipe is clamped to the clip, never stored outside it.
        daw::setCompRange(clip, "t3", -5.0, 25.0);
        check(clip.comp.size() == 1 && clip.comp[0].startSeconds >= 0.0 &&
                  std::fabs(clip.comp[0].endSeconds - 10.0) < 1e-9,
              "a swipe past both ends is clamped to the clip");

        // A segment naming a take that no longer exists is not playable, so
        // normalizing drops it rather than leaving a hole that reads as audio.
        clip.comp.push_back({"ghost", 2.0, 3.0});
        daw::normalizeComp(clip);
        check(daw::activeTakeAt(clip, 2.5) == "t3",
              "a segment naming a missing take is dropped");

        // Punch-in over a plain clip: the existing material becomes take 1
        // instead of being replaced.
        daw::ClipModel plain;
        plain.id = "p";
        plain.filePath = tonePath;
        plain.durationSeconds = 0.5;
        check(!daw::isLayered(plain), "a plain clip is not layered");
        daw::promoteToTake(plain);
        check(plain.takes.size() == 1 && plain.takes[0].filePath == tonePath,
              "promoting keeps the clip's own audio as its first take");
        check(plain.comp.size() == 1 && plain.comp[0].takeId == plain.takes[0].id,
              "the promoted take is what the comp plays");
        daw::promoteToTake(plain);
        check(plain.takes.size() == 1, "promoting twice adds nothing");
    }

    // ── A layered clip through save → load → comp edits ──
    // The fixture is authored as a document, written out and reopened rather
    // than recorded, which needs no device and covers the serializer at the same
    // time: everything the comp needs has to survive the round-trip.
    {
        const std::string takeA = (dir / "takeA.wav").string();
        const std::string takeB = (dir / "takeB.wav").string();
        writeTone(takeA, 48000, 48000);        // 1.0 s
        writeLeftOnlyTone(takeB, 48000, 48000);

        daw::ProjectModel proj;
        proj.name = "Layers";
        daw::TrackModel track;
        track.id = "tr1";
        track.name = "Vocal";
        track.kind = daw::TrackKind::Audio;
        track.recordMode = daw::TrackRecordMode::Layers;
        track.inputChannel = 1;
        daw::ClipModel clip;
        clip.id = "cl1";
        clip.name = "Verse";
        clip.startSeconds = 2.0;
        clip.durationSeconds = 1.0;
        clip.expanded = true;
        const char* paths[2] = {takeA.c_str(), takeB.c_str()};
        for (int i = 0; i < 2; ++i) {
            daw::TakeModel take;
            take.id = "tk" + std::to_string(i + 1);
            take.name = "Take " + std::to_string(i + 1);
            take.filePath = paths[i];
            take.lengthSeconds = 1.0;
            take.channels = 2;
            take.color = 0x112233u + uint32_t(i);
            take.gain = 0.75f;
            clip.takes.push_back(take);
        }
        clip.comp.push_back({"tk1", 0.0, 0.5});
        clip.comp.push_back({"tk2", 0.5, 1.0});
        track.clips.push_back(clip);
        proj.tracks.push_back(track);

        const std::string layeredPkg = (dir / "layers.vlt").string();
        check(daw::ProjectSerializer::save(proj, layeredPkg).isOk(),
              "a project with takes saves");

        daw::EngineController m;
        m.initialize(48000, 512, /*openDevice=*/false);
        check(m.openProject(layeredPkg).isOk(), "a project with takes loads");
        const daw::ClipModel* loaded = findClip(m, "tr1", "cl1");
        check(loaded != nullptr && loaded->takes.size() == 2,
              "both takes survive the round-trip");
        if (loaded && loaded->takes.size() == 2) {
            check(loaded->takes[0].name == "Take 1" &&
                      std::fabs(loaded->takes[0].gain - 0.75f) < 1e-6f &&
                      loaded->takes[1].color == 0x112234u,
                  "take name, gain and colour round-trip");
            check(fs::exists(loaded->takes[0].filePath),
                  "take audio is copied into the package and resolves");
            check(loaded->comp.size() == 2 && loaded->comp[0].takeId == "tk1" &&
                      std::fabs(loaded->comp[1].startSeconds - 0.5) < 1e-9,
                  "the comp map round-trips");
            check(loaded->expanded, "an open comp editor round-trips");
        }
        const auto* loadedTrack = m.project().findTrack("tr1");
        check(loadedTrack &&
                  loadedTrack->recordMode == daw::TrackRecordMode::Layers,
              "the track's recording mode round-trips");

        // One swipe, wrapped the way the timeline wraps a stroke: the whole
        // gesture is a single undo entry.
        m.beginCompEdit("tr1", "cl1");
        m.setCompSegment("tr1", "cl1", "tk1", 0.5, 0.7);
        m.setCompSegment("tr1", "cl1", "tk1", 0.7, 0.9);
        m.endCompEdit();
        loaded = findClip(m, "tr1", "cl1");
        check(loaded && daw::activeTakeAt(*loaded, 0.8) == "tk1",
              "a swipe through the controller repaints the comp");
        m.undo();
        loaded = findClip(m, "tr1", "cl1");
        check(loaded && daw::activeTakeAt(*loaded, 0.8) == "tk2",
              "one undo takes the whole stroke back");

        m.selectTake("tr1", "cl1", "tk2");
        loaded = findClip(m, "tr1", "cl1");
        check(loaded && loaded->comp.size() == 1 &&
                  daw::activeTakeAt(*loaded, 0.1) == "tk2",
              "picking a whole take covers the clip with it");

        const std::string dup = m.duplicateTake("tr1", "cl1", "tk1");
        loaded = findClip(m, "tr1", "cl1");
        check(!dup.empty() && loaded && loaded->takes.size() == 3,
              "duplicating a take adds it to the clip");
        if (loaded && loaded->takes.size() == 3) {
            check(loaded->takes[1].id == dup &&
                      loaded->takes[1].filePath == loaded->takes[0].filePath,
                  "the copy lands after the original and shares its audio");
        }

        m.moveTake("tr1", "cl1", dup, 2);
        loaded = findClip(m, "tr1", "cl1");
        check(loaded && loaded->takes[2].id == dup,
              "reordering moves the take in the stack");
        check(loaded && daw::activeTakeAt(*loaded, 0.1) == "tk2",
              "reordering changes nothing about what plays");

        // Put take 1 back into the first half, so the comp plays both originals
        // and the duplicate is the only take nothing references. That also
        // leaves a seam for the flatten below to crossfade.
        m.beginCompEdit("tr1", "cl1");
        m.setCompSegment("tr1", "cl1", "tk1", 0.0, 0.5);
        m.endCompEdit();

        // `deleteFiles=false`: the copy shares take 1's audio, and this test
        // asserts the file survives.
        check(m.deleteUnusedTakes(/*deleteFiles=*/false) == 1,
              "unused takes are collected");
        loaded = findClip(m, "tr1", "cl1");
        check(loaded && loaded->takes.size() == 2 &&
                  daw::findTake(*loaded, dup) == nullptr,
              "the take the comp never plays is the one that went");
        check(loaded && fs::exists(loaded->takes[0].filePath),
              "the shared audio file is left on disk");

        const std::string baked = m.flattenComp("tr1", "cl1");
        loaded = findClip(m, "tr1", "cl1");
        check(!baked.empty() && loaded && loaded->takes.size() == 3,
              "flattening bakes the comp into a new take, keeping the sources");
        if (loaded) {
            const daw::TakeModel* take = daw::findTake(*loaded, baked);
            check(take && fs::exists(take->filePath),
                  "the baked take has audio on disk");
            if (take) {
                platform::DecodedAudio rendered;
                check(platform::decodeAudioFile(take->filePath, rendered).isOk() &&
                          rendered.frames > 40000,
                      "the baked file spans the clip");
                // Take 2 is left-only; the second half of the comp comes from
                // it, so the render has to carry that asymmetry through.
                if (rendered.frames > 40000 && rendered.channels == 2) {
                    check(channelPeak(rendered, 0) > 0.1f,
                          "the baked comp carries audio");
                }
            }
        }

        // Commit dissolves the container, it does not tidy it: what the comp
        // played becomes the clip's own audio, with no takes, no comp and
        // nothing left to expand — the point of committing is that the layers
        // stop existing.
        m.commitComp("tr1", "cl1");
        loaded = findClip(m, "tr1", "cl1");
        check(loaded && loaded->takes.empty() && loaded->comp.empty(),
              "committing dissolves the layers");
        check(loaded && !daw::isLayered(*loaded) && !loaded->expanded,
              "and the clip can no longer be expanded");
        check(loaded && !loaded->filePath.empty() && fs::exists(loaded->filePath),
              "the clip now plays the baked file itself");
        m.undo();
        loaded = findClip(m, "tr1", "cl1");
        check(loaded && loaded->takes.size() == 3,
              "undo brings the discarded takes back");
        check(loaded && loaded->comp.size() == 1 &&
                  loaded->comp[0].takeId == baked,
              "one undo walks the whole gesture back to the baked comp");
    }

    // ── Importing audio as an extra take ──
    // The same operation a punch-in performs, only with a file instead of a
    // capture: an ordinary clip has to turn into a layered one, keeping what was
    // already there as Take 1.
    {
        const std::string first = (dir / "importA.wav").string();
        const std::string second = (dir / "importB.wav").string();
        writeTone(first, 48000, 48000);
        writeLeftOnlyTone(second, 48000, 48000);

        daw::EngineController m;
        m.newProject();
        const std::string tr = m.addTrack(daw::TrackKind::Audio, "Vocal");
        const std::string cl = m.importAudio(first, tr, 0.0);
        check(!cl.empty(), "the plain clip imports");
        const daw::ClipModel* c = findClip(m, tr, cl);
        check(c && c->takes.empty(), "an imported clip starts with no layers");

        const std::string added = m.addTakeFromFile(tr, cl, second, 0.0);
        c = findClip(m, tr, cl);
        check(!added.empty() && c && c->takes.size() == 2,
              "importing a take promotes the clip's own audio to Take 1");
        if (c && c->takes.size() == 2) {
            check(c->takes[0].filePath == first,
                  "Take 1 is the audio the clip already had");
            check(c->takes[1].id == added && c->takes[1].filePath == second,
                  "Take 2 is the file just imported");
            check(daw::isLayered(*c), "the clip now reads as layered");
            check(daw::activeTakeAt(*c, 0.5) == added,
                  "the newest take is the one that plays");
            check(!c->comp.empty() && c->comp.front().takeId == added,
                  "and the comp is handed to it");
        }

        m.undo();
        c = findClip(m, tr, cl);
        check(c && c->takes.empty() && c->filePath == first,
              "undo returns the clip to a single unlayered take");
    }

    // ── Count-in ──
    // Engaging Record no longer starts a take; the count-in is what fills the
    // gap. The transport stays parked while the clicks run — a pre-roll needs
    // room before the punch point, and a take at bar 1 has none — so the count
    // is a countdown in beats and the take starts where the playhead already is.
    {
        daw::EngineController m;
        m.initialize(48000, 512, /*openDevice=*/false);
        m.setRecordDirectory((dir / "countin").string());
        const std::string tr = m.addTrack(daw::TrackKind::Audio, "Vocal");
        m.setTempo(120.0);                       // one beat = 0.5 s

        m.seekSeconds(6.0);
        check(m.armCountIn({tr}, 3), "a three-beat count-in arms");
        check(m.isCountingIn(), "and it reports itself as counting in");
        check(!m.isRecording(), "nothing is captured while it counts");
        check(m.countInBeatsRemaining() == 3, "three beats are left to count");
        check(std::abs(m.positionSeconds() - 6.0) < 0.01,
              "the transport stays where it was — no rewind");

        check(!m.tickCountIn(0.5), "one beat down, two to go");
        check(m.countInBeatsRemaining() == 2, "and the count says so");
        check(std::abs(m.countInRemainingSeconds() - 1.0) < 0.01,
              "one second of count-in is left at this tempo");
        check(!m.tickCountIn(0.5), "two down");
        check(m.tickCountIn(0.5), "the third beat starts the take");
        check(!m.isCountingIn(), "the count-in is over");
        check(m.isRecording(), "and the recording is running");
        check(std::abs(m.recordingStartSeconds(tr) - 6.0) < 0.01,
              "the take punches in exactly where the playhead was");
        m.stopRecording();
    }

    // A count-in works at the very top of the song, which is where the old
    // rewind-based one silently did nothing at all.
    {
        daw::EngineController m;
        m.initialize(48000, 512, /*openDevice=*/false);
        m.setRecordDirectory((dir / "countin2").string());
        const std::string tr = m.addTrack(daw::TrackKind::Audio, "Vocal");
        m.setTempo(120.0);

        check(m.armCountIn({tr}, 3), "a count-in arms at position zero");
        check(m.countInBeatsRemaining() == 3, "with all three beats to count");
        check(std::abs(m.positionSeconds()) < 0.001, "the playhead has not moved");

        m.cancelCountIn();
        check(!m.isCountingIn(), "cancelling drops the count-in");
        check(!m.isRecording(), "and nothing was recorded");

        // Zero beats is "no count-in", which starts the take immediately.
        check(m.armCountIn({tr}, 0), "no count-in starts the take at once");
        check(m.isRecording() && !m.isCountingIn(),
              "so it is recording and not counting");
        m.stopRecording();
    }

    // The count-in opens the monitor before the take, not with it: a player
    // counted in has to hear themselves to play on the first beat.
    {
        daw::EngineController m;
        m.initialize(48000, 512, /*openDevice=*/false);
        m.setRecordDirectory((dir / "countin3").string());
        const std::string tr = m.addTrack(daw::TrackKind::Audio, "Vocal");
        m.setTempo(120.0);
        auto monitorOf = [&](const std::string& id) {
            const auto* t = m.project().findTrack(id);
            return t && t->monitor;
        };

        check(!monitorOf(tr), "the track starts unmonitored");
        check(m.armCountIn({tr}, 2), "a count-in arms");
        check(monitorOf(tr), "monitoring opens while it counts");

        m.cancelCountIn();
        check(!monitorOf(tr),
              "cancelling puts it back — nothing was recorded, nothing changed");

        check(m.armCountIn({tr}, 1), "count in again");
        check(m.tickCountIn(0.5), "and let it reach the take");
        check(monitorOf(tr), "monitoring is still open while recording");
        m.stopRecording();
        check(!monitorOf(tr),
              "and closes on stop, back to what it was before the count-in");
    }

    // Automatic monitoring can be switched off entirely, and then a recording
    // touches nothing: only a monitor the user opened themselves is heard.
    {
        daw::EngineController m;
        m.initialize(48000, 512, /*openDevice=*/false);
        m.setRecordDirectory((dir / "countin4").string());
        const std::string quiet = m.addTrack(daw::TrackKind::Audio, "Quiet");
        const std::string open = m.addTrack(daw::TrackKind::Audio, "Open");
        m.setTrackInputChannel(open, 1);          // its own input, so no sharing rule
        m.setTrackMonitor(open, true);     // the user's own click
        auto prefs = m.recordingPrefs();
        prefs.autoMonitorOnRecord = false;
        m.setRecordingPrefs(prefs);
        auto monitorOf = [&](const std::string& id) {
            const auto* t = m.project().findTrack(id);
            return t && t->monitor;
        };

        check(m.armCountIn({quiet}, 1), "a count-in arms with auto monitoring off");
        check(!monitorOf(quiet), "and opens nothing");
        check(m.tickCountIn(0.5), "the take starts");
        check(!monitorOf(quiet), "still silent — nobody asked to be monitored");
        m.stopRecording();
        check(!monitorOf(quiet), "and nothing was left switched on");

        m.startRecording(open);
        check(monitorOf(open), "a hand-opened monitor is left alone during a take");
        m.stopRecording();
        check(monitorOf(open), "and is still the user's to close afterwards");
    }

    // ── Smart input monitoring ──
    // The rule: a track about to record opens its monitor only when nothing
    // else is already monitoring the same input, so the performer never hears
    // the source twice. The worked example from the spec, verbatim.
    {
        daw::EngineController m;
        m.initialize(48000, 512, /*openDevice=*/false);
        m.setRecordDirectory((dir / "captures").string());
        const std::string vocal = m.addTrack(daw::TrackKind::Audio, "Vocal");
        const std::string guitar = m.addTrack(daw::TrackKind::Audio, "Guitar");
        const std::string backing = m.addTrack(daw::TrackKind::Audio, "Backing");
        m.setTrackInputChannel(vocal, 1);
        m.setTrackInputChannel(guitar, 2);
        m.setTrackInputChannel(backing, 1);
        m.setTrackMonitor(guitar, true);   // a different input, already listening

        auto monitorOf = [&m](const std::string& id) {
            const auto* t = m.project().findTrack(id);
            return t && t->monitor;
        };
        auto autoOf = [&m](const std::string& id) {
            const auto* t = m.project().findTrack(id);
            return t && t->monitorAuto;
        };

        auto prefs = m.recordingPrefs();
        prefs.monitorStopPolicy =
            daw::EngineController::MonitorStopPolicy::ReturnToPrevious;
        m.setRecordingPrefs(prefs);

        // Nothing monitors input 1 yet, so the vocal gets its monitor opened.
        if (check(m.startRecording(vocal), "recording starts on the vocal")) {
            check(monitorOf(vocal),
                  "an unmonitored input is opened for the performer");
            check(autoOf(vocal), "and it is marked as automatic");
            check(m.project().findTrack(vocal)->armed,
                  "an unarmed target is armed");
            m.stopRecording();
            check(!monitorOf(vocal),
                  "Return to previous puts the monitor back as it was");
            // The mark survives the stop: the track is still under automatic
            // control, and only a manual click hands it back to the user.
            check(autoOf(vocal), "the automatic mark stays after the stop");
            m.setTrackMonitor(vocal, false);
            check(!autoOf(vocal), "a manual click drops the automatic mark");
        }

        // Now hold input 1 open by hand and record the other track on it: the
        // second track must be left alone, or the source is heard twice.
        m.setTrackMonitor(vocal, true);
        if (check(m.startRecording(backing), "recording starts on the backing")) {
            check(!monitorOf(backing),
                  "a second track on the same input is left alone");
            // Left closed, but still automatic: the decision was ours, so the
            // "A" reports it and the next recording re-decides.
            check(autoOf(backing), "the decision not to open is still automatic");
            check(monitorOf(guitar),
                  "another input's monitoring is untouched");
            m.stopRecording();
        }

        // Keep-on and auto-disable, on a fresh input nothing else is watching.
        m.setTrackMonitor(vocal, false);
        prefs.monitorStopPolicy =
            daw::EngineController::MonitorStopPolicy::KeepOn;
        m.setRecordingPrefs(prefs);
        if (m.startRecording(vocal)) {
            m.stopRecording();
            check(monitorOf(vocal), "Keep on leaves the monitor open");
        }
        prefs.monitorStopPolicy =
            daw::EngineController::MonitorStopPolicy::AutoDisable;
        m.setRecordingPrefs(prefs);
        if (m.startRecording(vocal)) {
            m.stopRecording();
            check(!monitorOf(vocal), "Auto-disable closes it on stop");
        }

        // Multi-track capture: both targets record, and each is armed.
        m.setTrackMonitor(vocal, false);
        if (check(m.startRecordingTracks({vocal, guitar}),
                  "recording starts on two tracks at once")) {
            check(m.recordingTracks().size() == 2,
                  "both targets are capturing");
            check(m.recordingStartSeconds(vocal) >= 0.0 &&
                      m.recordingStartSeconds(backing) < 0.0,
                  "a capture reports where it began, and only for its own track");
            m.stopRecording();
            check(!m.isRecording() && m.recordingTracks().empty(),
                  "stopping clears both");
        }
    }

    // ── Record mode resolution ──
    // Three inputs decide what a recording does: the global mode, the track's
    // override, and the held invert key. This is the order they apply in.
    {
        daw::EngineController m;
        m.initialize(48000, 512, /*openDevice=*/false);
        const std::string a = m.addTrack(daw::TrackKind::Audio, "A");
        const std::string b = m.addTrack(daw::TrackKind::Audio, "B");

        m.setRecordMode(daw::RecordMode::Overwrite);
        check(m.effectiveRecordMode(a) == daw::RecordMode::Overwrite,
              "a track on Use Global follows the global mode");
        m.setRecordMode(daw::RecordMode::Layers);
        check(m.effectiveRecordMode(a) == daw::RecordMode::Layers,
              "changing the global mode moves it with it");

        m.setTrackRecordMode(b, daw::TrackRecordMode::Overwrite);
        check(m.effectiveRecordMode(b) == daw::RecordMode::Overwrite &&
                  m.effectiveRecordMode(a) == daw::RecordMode::Layers,
              "a track override wins over the global mode, for that track only");

        m.setRecordModeInverted(true);
        check(m.effectiveRecordMode(a) == daw::RecordMode::Overwrite &&
                  m.effectiveRecordMode(b) == daw::RecordMode::Layers,
              "the held invert flips whatever each track resolved to");
        m.setRecordModeInverted(false);
        check(m.effectiveRecordMode(a) == daw::RecordMode::Layers,
              "releasing the key restores the resolved mode");

        m.setTrackRecordMode(b, daw::TrackRecordMode::UseGlobal);
        check(m.effectiveRecordMode(b) == daw::RecordMode::Layers,
              "clearing the override hands the track back to the global mode");
    }

    // ── Collapsing several edits into one undo entry ──
    // What the AI assistant runs on: it cannot hold an UndoStack::Suspend
    // across its network waits, so its steps record normally and are folded
    // afterwards.
    {
        daw::EngineController m;
        m.initialize(48000.0, 512, /*openDevice=*/false);
        const std::string t = m.addTrack(daw::TrackKind::Instrument, "Piano");
        const std::size_t mark = m.undoDepth();

        m.setTrackVolume(t, 0.25f);
        m.setTrackPan(t, -0.5f);
        const std::string clip = m.addMidiClip(t, 0.0, 2.0);
        check(m.undoDepth() == mark + 3, "three edits, three entries");

        m.collapseUndo(mark, "AI: make a piano");
        check(m.undoDepth() == mark + 1 && m.undoLabel() == "AI: make a piano",
              "collapsing folds them into one labelled entry");

        m.undo();
        const daw::TrackModel* after = m.project().findTrack(t);
        check(after && after->volume == 1.0f && after->pan == 0.0f &&
                  after->clips.empty(),
              "one undo reverts all three edits");
        check(m.undoDepth() == mark, "and leaves the track itself in place");

        m.redo();
        after = m.project().findTrack(t);
        check(after && after->volume == 0.25f && after->pan == -0.5f &&
                  after->clips.size() == 1 && after->clips[0].id == clip,
              "one redo replays all three in order");

        // A group that no longer fits — the caller's mark predates entries the
        // stack's limit discarded — must be refused rather than half-applied.
        const std::size_t before = m.undoDepth();
        m.collapseUndo(before + 5, "impossible");
        check(m.undoDepth() == before, "a mark past the top collapses nothing");
        m.setTrackVolume(t, 0.75f);
        m.collapseUndo(m.undoDepth() - 1, "single");
        check(m.undoLabel() != "single",
              "a group of one is left alone, not relabelled");
    }

    // ── Continuous gestures record endpoints, not mouse-move samples ──
    {
        daw::EngineController m;
        m.initialize(48000.0, 512, /*openDevice=*/false);
        const std::string a = m.addTrack(daw::TrackKind::Audio, "A");
        const std::string b = m.addTrack(daw::TrackKind::Audio, "B");

        const std::size_t volumeMark = m.undoDepth();
        m.setTrackVolumeLive(a, 0.9f);
        m.setTrackVolumeLive(a, 0.7f);
        m.setTrackVolumeLive(a, 0.4f);
        m.setTrackVolumeLive(b, 1.2f);
        m.setTrackVolumeLive(b, 1.5f);
        check(m.undoDepth() == volumeMark,
              "live fader samples do not fill the undo stack");
        m.commitTrackVolumeEdit({{a, 1.0f}, {b, 1.0f}}, "Adjust Volumes");
        check(m.undoDepth() == volumeMark + 1 &&
                  m.undoLabel() == "Adjust Volumes",
              "one multi-track fader gesture creates one entry");
        m.undo();
        check(m.project().findTrack(a)->volume == 1.0f &&
                  m.project().findTrack(b)->volume == 1.0f,
              "one undo restores every fader to its press value");
        m.redo();
        check(m.project().findTrack(a)->volume == 0.4f &&
                  m.project().findTrack(b)->volume == 1.5f,
              "one redo restores the final fader values");

        const std::size_t panMark = m.undoDepth();
        m.setTrackPanLive(a, -0.1f);
        m.setTrackPanLive(a, -0.4f);
        m.setTrackPanLive(a, -0.75f);
        m.commitTrackPanEdit({{a, 0.0f}});
        check(m.undoDepth() == panMark + 1,
              "a pan drag also creates exactly one entry");
        m.undo();
        check(m.project().findTrack(a)->pan == 0.0f,
              "one undo restores pan to the gesture start");
        m.redo();
        check(m.project().findTrack(a)->pan == -0.75f,
              "redo restores the final pan position");

        const std::size_t masterMark = m.undoDepth();
        m.setMasterVolumeLive(0.8f);
        m.setMasterVolumeLive(0.55f);
        m.commitMasterVolumeEdit(1.0f);
        check(m.undoDepth() == masterMark + 1,
              "the master fader gesture is undoable once");
        m.undo();
        check(m.masterVolume() == 1.0f,
              "master undo restores the press value");
        m.redo();
        check(m.masterVolume() == 0.55f,
              "master redo restores the released value");

        const std::string sendId = m.addSend(a, b);
        const float sendBefore = m.project().findTrack(a)->sends.front().level;
        const std::size_t sendMark = m.undoDepth();
        m.setSendLevel(a, sendId, 0.7f);
        m.setSendLevel(a, sendId, 1.1f);
        m.commitSendLevelEdit(a, sendId, sendBefore);
        check(m.undoDepth() == sendMark + 1,
              "a send knob gesture creates one entry");
        m.undo();
        check(m.project().findTrack(a)->sends.front().level == sendBefore,
              "send undo restores the knob's press value");
        m.redo();
        check(m.project().findTrack(a)->sends.front().level == 1.1f,
              "send redo restores the knob's released value");

        const std::string clip = m.importAudio(tonePath, a, 0.0);
        const daw::ProjectModel clipBefore = m.project();
        const std::size_t clipMark = m.undoDepth();
        m.setClipGain(a, clip, 0.8f);
        m.setClipGain(a, clip, 0.5f);
        m.setClipFade(a, clip, 0.05, 0.1);
        m.commitProjectGesture(clipBefore, clipMark, "Edit Clip Gesture");
        check(m.undoDepth() == clipMark + 1,
              "a compound clip drag creates one snapshot entry");
        m.undo();
        check(std::fabs(findClip(m, a, clip)->gain - 1.0f) < 1e-6 &&
                  findClip(m, a, clip)->fadeInSeconds == 0.0 &&
                  findClip(m, a, clip)->fadeOutSeconds == 0.0,
              "one undo restores the whole clip gesture");
        m.redo();
        check(std::fabs(findClip(m, a, clip)->gain - 0.5f) < 1e-6 &&
                  std::fabs(findClip(m, a, clip)->fadeInSeconds - 0.05) < 1e-9 &&
                  std::fabs(findClip(m, a, clip)->fadeOutSeconds - 0.1) < 1e-9,
              "one redo restores the whole clip gesture endpoint");
    }

    // ── A tempo change is a musical move, not a rescale ──
    //
    // The document holds clip times in seconds because that is what the engine
    // plays, but the song is written in bars. So a BPM change has to carry every
    // clip onto the bar it was written against — otherwise the grid slides out
    // from under the whole project — while leaving audio the length it actually
    // is, since nothing is being time-stretched.
    {
        daw::EngineController m;
        m.initialize(48000, 512, /*openDevice=*/false);
        m.setTempo(120.0);                          // one beat = 0.5 s

        const std::string audio = m.addTrack(daw::TrackKind::Audio, "Gtr");
        const std::string keys = m.addTrack(daw::TrackKind::Midi, "Keys");
        const std::string wave = m.importAudio(tonePath, audio, 2.0);  // beat 4
        const std::string midi = m.addMidiClip(keys, 4.0, 2.0);        // beat 8, one bar
        check(!wave.empty() && !midi.empty(), "a clip of each kind is placed");

        const double audioLength = findClip(m, audio, wave)->durationSeconds;
        m.setLoopRangeSeconds(2.0, 6.0);            // beats 4 … 12
        m.seekSeconds(2.0);

        m.setTempo(60.0);                           // one beat = 1 s: half speed
        {
            const daw::ClipModel* a = findClip(m, audio, wave);
            const daw::ClipModel* n = findClip(m, keys, midi);
            check(a && std::fabs(a->startSeconds - 4.0) < 1e-9,
                  "the audio clip still starts on beat 4, now 4 s in");
            check(a && std::fabs(a->durationSeconds - audioLength) < 1e-9,
                  "and is still exactly as long as the audio it plays");
            check(n && std::fabs(n->startSeconds - 8.0) < 1e-9,
                  "the MIDI clip still starts on beat 8");
            check(n && std::fabs(n->durationSeconds - 4.0) < 1e-9,
                  "and is still one bar long — its notes are in beats too");
            check(std::fabs(m.loopStartSeconds() - 4.0) < 1e-6 &&
                      std::fabs(m.loopEndSeconds() - 12.0) < 1e-6,
                  "the loop still runs from beat 4 to beat 12");
            check(std::fabs(m.positionSeconds() - 4.0) < 1e-6,
                  "and the playhead is still parked on beat 4");
        }

        m.undo();
        {
            const daw::ClipModel* a = findClip(m, audio, wave);
            const daw::ClipModel* n = findClip(m, keys, midi);
            check(std::fabs(m.project().tempo - 120.0) < 1e-9, "undo restores the tempo");
            check(a && std::fabs(a->startSeconds - 2.0) < 1e-9,
                  "and puts the audio clip back where it was");
            check(n && std::fabs(n->durationSeconds - 2.0) < 1e-9,
                  "and the MIDI clip back to the length it was");
        }
    }

    // ── The take in flight is the take that lands ──
    //
    // The arrangement draws a recording as the clip it is about to become, and
    // the description it draws from is built by the same code that lands the
    // capture: the pass split, the punch target, the take colour and number.
    {
        daw::EngineController m;
        m.initialize(48000, 512, /*openDevice=*/false);
        m.setRecordDirectory((dir / "preview").string());
        const std::string tr = m.addTrack(daw::TrackKind::Audio, "Vocal");

        check(!m.recordingPreview(tr).active,
              "a track that is not recording has nothing to draw");

        m.seekSeconds(4.0);
        check(m.startRecording(tr), "a take starts");
        const auto preview = m.recordingPreview(tr);
        check(preview.active, "which the arrangement is told about");
        check(preview.spans.size() == 1 &&
                  std::fabs(preview.spans[0].startSeconds - 4.0) < 1e-9,
              "one pass, starting at the punch point");
        check(preview.color == m.project().findTrack(tr)->color,
              "drawn in the track's own colour, not in record red");
        check(!preview.layered, "and as a clip of its own — nothing was punched into");
        m.stopRecording();
    }

    // Layer recording punching into existing material: the take being recorded
    // is the *next take of that clip*, so it is drawn in that take's colour and
    // under its name rather than as a new clip in the track's colour.
    {
        daw::EngineController m;
        m.initialize(48000, 512, /*openDevice=*/false);
        m.setRecordDirectory((dir / "preview-layers").string());
        const std::string tr = m.addTrack(daw::TrackKind::Audio, "Vocal");
        const std::string existing = m.importAudio(tonePath, tr, 0.0);  // 0.5 s
        check(!existing.empty(), "there is already a clip to punch into");

        auto prefs = m.recordingPrefs();
        prefs.mode = daw::RecordMode::Layers;
        m.setRecordingPrefs(prefs);

        m.seekSeconds(0.1);
        check(m.startRecording(tr), "a layer take starts over it");
        const auto preview = m.recordingPreview(tr);
        check(preview.active && preview.layered,
              "the preview knows it is landing inside a clip");
        check(preview.targetClipId == existing, "and which clip that is");
        check(preview.takeIndex == 1,
              "it becomes take 2 — landing promotes the clip's own audio to take 1");
        check(preview.name == "Take 2", "so that is the name it is drawn under");
        check(preview.color == daw::takeColor(m.project().findTrack(tr)->color, 1),
              "in the colour that take will wear once it lands");
        check(preview.color != m.project().findTrack(tr)->color,
              "which is not the track colour — a stack of layers reads as a stack");
        m.stopRecording();
    }

    // Loop recording: the take in flight is split into the passes the transport
    // has actually made, each landing where that pass began — the same split
    // that lands the takes, so the arrangement draws one body per pass.
    {
        daw::EngineController m;
        m.initialize(48000, 512, /*openDevice=*/false);
        m.setRecordDirectory((dir / "preview-loop").string());
        const std::string tr = m.addTrack(daw::TrackKind::Audio, "Vocal");

        m.setLoopRangeSeconds(4.0, 8.0);           // a four-second cycle
        m.setLoopEnabled(true);
        m.seekSeconds(6.0);                        // punch in halfway through it
        check(m.startRecording(tr), "a looped take starts");

        // Two and a half passes: 2 s to the loop end, then 4 s, then 2 s more.
        m.seedRecordingForShot(tr, 8.0);
        const auto preview = m.recordingPreview(tr);
        check(preview.spans.size() == 3, "three passes have been written");
        if (preview.spans.size() == 3) {
            check(std::fabs(preview.spans[0].startSeconds - 6.0) < 1e-9 &&
                      std::fabs(preview.spans[0].endSeconds - 8.0) < 1e-9,
                  "the first runs from the punch point to the loop end");
            check(std::fabs(preview.spans[1].startSeconds - 4.0) < 1e-9 &&
                      std::fabs(preview.spans[1].endSeconds - 8.0) < 1e-9,
                  "the second is the whole cycle, back at the loop start");
            check(std::fabs(preview.spans[2].startSeconds - 4.0) < 1e-9 &&
                      std::fabs(preview.spans[2].captureOffsetSeconds - 6.0) < 1e-9,
                  "and the third is the pass in flight, six seconds into the file");
        }
        check(preview.envelope.size() > 100 && preview.envelopeStepSeconds > 0.0,
              "the envelope is bucketed in recorded time, not in frames drawn");
        m.stopRecording();
    }

    // Track kinds have stable, distinct defaults, while automation lanes are
    // visual children and continue following their owner after recolouring.
    {
        daw::EngineController colours;
        colours.initialize(48000, 512, /*openDevice=*/false);
        const std::vector<daw::TrackKind> kinds = {
            daw::TrackKind::Audio,      daw::TrackKind::Instrument,
            daw::TrackKind::Midi,       daw::TrackKind::Pattern,
            daw::TrackKind::Automation, daw::TrackKind::Bus,
            daw::TrackKind::Aux,        daw::TrackKind::Group,
            daw::TrackKind::Master,     daw::TrackKind::Folder};
        std::set<uint32_t> defaults;
        for (const daw::TrackKind kind : kinds) {
            std::string id;
            if (kind == daw::TrackKind::Pattern)
                id = colours.addPattern();
            else if (kind == daw::TrackKind::Folder)
                id = colours.addFolder(false);
            else
                id = colours.addTrack(kind);
            const auto* track = colours.project().findTrack(id);
            if (track) defaults.insert(track->color);
        }
        check(defaults.size() == kinds.size(),
              "every track kind has a distinct default colour");

        const std::string owner =
            colours.addTrack(daw::TrackKind::Audio, "Automation owner");
        daw::AutomationTarget target;
        target.kind = daw::AutomationTargetKind::TrackVolume;
        target.channelId = owner;
        const std::string lane = colours.addAutomationLane(owner, target);
        const std::string clip =
            colours.addAutomationClip(lane, target, 0.0, 4.0);
        const uint32_t changed = 0x4FBF86;
        colours.setTrackColor(owner, changed);
        const auto* laneTrack = colours.project().findTrack(lane);
        const auto* laneClip = findClip(colours, lane, clip);
        check(laneTrack && laneTrack->color == changed && laneClip &&
                  laneClip->color == changed,
              "automation lane and clips inherit owner colour changes");

        const auto* ownerTrack = colours.project().findTrack(owner);
        check(ownerTrack && ownerTrack->automationExpanded,
              "creating automation reveals its independent disclosure");
        colours.setAutomationExpanded(owner, false);
        ownerTrack = colours.project().findTrack(owner);
        check(ownerTrack && ownerTrack->expanded &&
                  !ownerTrack->automationExpanded,
              "hiding automation does not collapse the owner track");
        bool laneVisible = false;
        for (const auto& row : daw::visibleTracks(colours.project())) {
            if (colours.project().tracks[row.index].id == lane)
                laneVisible = true;
        }
        check(!laneVisible, "hidden automation lane leaves the owner visible");
    }

    // ── Settings store round-trip ──
    const std::string settingsPath = (dir / "settings.json").string();
    {
        daw::SettingsStore s(settingsPath);
        s.setString("outputDevice", "Speakers");
        s.setInt("bufferSize", 256);
        s.setBool("metronome", true);
        check(s.save(), "settings save");
    }
    daw::SettingsStore s2(settingsPath);
    check(s2.getString("outputDevice") == "Speakers" &&
              s2.getInt("bufferSize") == 256 && s2.getBool("metronome"),
          "settings reload round-trips");

    fs::remove_all(dir);
    std::printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures;
}
