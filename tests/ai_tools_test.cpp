// The surface a language model is given over the program, driven with
// hand-written JSON against a real controller. No network is involved and no
// provider is contacted: `callTool` and `AiSession` are pure, which is the
// whole point of keeping them out of app/.
//
// Two halves. First every tool, on its happy path and on the mistake a model is
// most likely to make with it — a wrong id, a bad enum, a value out of range —
// since an error the model can read and correct is what makes the feature work
// at all. Then the agent loop itself: scripted replies in, the document and one
// undo entry out.
#include "EngineController.hpp"
#include "ai/AiSession.hpp"
#include "AudioAnalysis.hpp"
#include "ai/AiTools.hpp"
#include "ai/AiWire.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "Core/AudioBuffer.hpp"
#include "Recording/RecordingEngine.hpp"

namespace fs = std::filesystem;

using json = nlohmann::json;
namespace ai = daw::ai;

static int failures = 0;
static bool check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++failures;
    return cond;
}

static ai::ToolContext gContext;

static ai::ToolResult call(daw::EngineController& c, const char* name,
                           json args = json::object()) {
    return ai::callTool(c, name, args, gContext);
}

/// Does the failure actually say what went wrong? A message that names the
/// problem is what lets the model retry instead of giving up.
static bool mentions(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

/// A short stereo tone, so a search has real decodable files to find.
static void writeTone(const std::string& path, double rate, int frames) {
    audio::AudioBuffer tone(2, audio::BufferSize(frames));
    for (int f = 0; f < frames; ++f) {
        const float v = 0.2f * std::sin(2.0f * 3.14159265f * 220.0f * f / float(rate));
        tone.getChannel(0)[f] = v;
        tone.getChannel(1)[f] = v;
    }
    audio::AudioRecorder rec;
    rec.initialize(audio::SampleRate(rate), 2);
    rec.writeWAVFile(path, tone, audio::SampleRate(rate));
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "daw-ai-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    // ── The registry ──
    {
        const auto& specs = ai::toolSpecs();
        check(specs.size() >= 20, "the registry carries the whole tool set");

        bool wellFormed = true;
        for (const ai::ToolSpec& spec : specs) {
            if (spec.name.empty() || spec.description.size() < 20) wellFormed = false;
            if (!spec.inputSchema.is_object()) wellFormed = false;
            if (spec.inputSchema.value("type", "") != "object") wellFormed = false;
            if (!spec.inputSchema.contains("properties")) wellFormed = false;
        }
        check(wellFormed, "every tool has a name, a description and a schema");

        bool unique = true;
        for (std::size_t i = 0; i < specs.size(); ++i)
            for (std::size_t j = i + 1; j < specs.size(); ++j)
                if (specs[i].name == specs[j].name) unique = false;
        check(unique, "tool names do not collide");

        // Anything named as required has to exist in properties, or the provider
        // rejects the whole request and every tool becomes unavailable at once.
        bool requiredDeclared = true;
        for (const ai::ToolSpec& spec : specs) {
            const json& props = spec.inputSchema.at("properties");
            for (const json& name : spec.inputSchema.value("required", json::array()))
                if (!props.contains(name.get<std::string>())) requiredDeclared = false;
        }
        check(requiredDeclared, "every required argument is declared");
    }

    daw::EngineController c;
    c.initialize(48000.0, 512, /*openDevice=*/false);
    c.setTempo(120.0);

    // ── Unknown tool, malformed arguments ──
    {
        const ai::ToolResult bad = call(c, "make_a_hit");
        check(!bad.ok && mentions(bad.error, "make_a_hit"),
              "an unknown tool name is refused by name");

        // Models sometimes send the arguments as a JSON string rather than an
        // object; accepting that costs nothing and saves a round trip.
        const ai::ToolResult stringy =
            ai::callTool(c, "set_tempo", json("{\"bpm\": 128}"));
        check(stringy.ok && c.tempo() == 128.0,
              "arguments arriving as a JSON string still work");
        c.setTempo(120.0);

        const ai::ToolResult junk =
            ai::callTool(c, "set_tempo", json("not json at all"));
        check(!junk.ok, "arguments that are not JSON are refused, not crashed on");

        const ai::ToolResult missing = call(c, "set_tempo");
        check(!missing.ok && mentions(missing.error, "bpm"),
              "a missing argument is named");

        const ai::ToolResult wrongType =
            call(c, "set_tempo", json{{"bpm", "fast"}});
        check(!wrongType.ok && mentions(wrongType.error, "number"),
              "an argument of the wrong type is named");
    }

    // ── Project ──
    {
        check(call(c, "set_tempo", json{{"bpm", 96}}).ok && c.tempo() == 96.0,
              "set_tempo moves the project tempo");
        const ai::ToolResult tooFast = call(c, "set_tempo", json{{"bpm", 900}});
        check(!tooFast.ok && c.tempo() == 96.0,
              "an impossible tempo is refused and nothing changes");

        check(call(c, "set_time_signature",
                   json{{"numerator", 3}, {"denominator", 4}})
                      .ok &&
                  c.timeSigNumerator() == 3,
              "set_time_signature applies");
        check(!call(c, "set_time_signature",
                    json{{"numerator", 4}, {"denominator", 5}})
                       .ok,
              "a denominator that is not a note value is refused");
        c.setTimeSignature(4, 4);
        c.setTempo(120.0);
    }

    // ── Tracks ──
    std::string pianoId;
    {
        const ai::ToolResult made =
            call(c, "add_track", json{{"kind", "instrument"}, {"name", "Piano"}});
        check(made.ok && made.value.contains("trackId"),
              "add_track returns the id of the track it made");
        pianoId = made.value.value("trackId", "");

        const daw::TrackModel* track = c.project().findTrack(pianoId);
        check(track && track->name == "Piano" &&
                  track->kind == daw::TrackKind::Instrument,
              "and the track is really there, with the name and kind asked for");

        // trackKindFromString falls back to Audio, so an unknown word would
        // quietly make the wrong kind of track. It must be refused instead.
        const ai::ToolResult nonsense =
            call(c, "add_track", json{{"kind", "piano"}, {"name", "X"}});
        check(!nonsense.ok && mentions(nonsense.error, "instrument"),
              "an unknown track kind is refused, and the valid ones listed");

        const ai::ToolResult unknown =
            call(c, "rename_track", json{{"trackId", "nope"}, {"name", "X"}});
        check(!unknown.ok && mentions(unknown.error, "Piano"),
              "a wrong track id is answered with the tracks that do exist");

        check(call(c, "rename_track",
                   json{{"trackId", pianoId}, {"name", "Rhodes"}})
                      .ok &&
                  c.project().findTrack(pianoId)->name == "Rhodes",
              "rename_track applies");
        call(c, "rename_track", json{{"trackId", pianoId}, {"name", "Piano"}});
    }

    // ── Mix ──
    {
        const ai::ToolResult mixed =
            call(c, "set_track_mix",
                 json{{"trackId", pianoId}, {"volumeDb", -6.0}, {"pan", -0.3}});
        const daw::TrackModel* track = c.project().findTrack(pianoId);
        // -6 dB is half the amplitude; the tools speak dB and the document
        // linear gain, and that conversion lives in exactly one place.
        check(mixed.ok && track && std::abs(track->volume - 0.501f) < 0.01f &&
                  std::abs(track->pan + 0.3f) < 0.001f,
              "set_track_mix converts dB to gain and applies pan");

        check(!call(c, "set_track_mix", json{{"trackId", pianoId}, {"pan", 4.0}}).ok &&
                  std::abs(c.project().findTrack(pianoId)->pan + 0.3f) < 0.001f,
              "a pan outside the range is refused and the old value kept");

        const ai::ToolResult nothing =
            call(c, "set_track_mix", json{{"trackId", pianoId}});
        check(!nothing.ok && mentions(nothing.error, "volumeDb"),
              "a call that would change nothing says what it accepts");

        const ai::ToolResult bus =
            call(c, "add_track", json{{"kind", "bus"}, {"name", "Reverb Bus"}});
        const std::string busId = bus.value.value("trackId", "");
        const ai::ToolResult send =
            call(c, "add_send", json{{"trackId", pianoId},
                                     {"destinationTrackId", busId},
                                     {"level", 0.4}});
        const daw::TrackModel* sender = c.project().findTrack(pianoId);
        check(send.ok && sender->sends.size() == 1 &&
                  std::abs(sender->sends[0].level - 0.4f) < 0.001f,
              "add_send routes a track to a bus at the level asked for");
        check(!call(c, "add_send", json{{"trackId", pianoId},
                                        {"destinationTrackId", busId},
                                        {"level", 9.0}})
                   .ok,
              "a send level outside 0-1 is refused");
    }

    // ── Clips and notes ──
    std::string clipId;
    {
        const ai::ToolResult clip =
            call(c, "add_midi_clip",
                 json{{"trackId", pianoId}, {"startBar", 1}, {"lengthBars", 2}});
        check(clip.ok && clip.value.contains("clipId"),
              "add_midi_clip returns the new clip's id");
        clipId = clip.value.value("clipId", "");

        const daw::TrackModel* track = c.project().findTrack(pianoId);
        // Bar 1 is the start of the project, and at 120 BPM a bar is 2 seconds.
        check(track->clips.size() == 1 && track->clips[0].startSeconds == 0.0 &&
                  std::abs(track->clips[0].durationSeconds - 4.0) < 1e-6,
              "bar 1 is the project start, and two bars is four seconds at 120");

        check(!call(c, "add_midi_clip",
                    json{{"trackId", pianoId}, {"startBar", 0}, {"lengthBars", 2}})
                   .ok,
              "bar 0 is refused — bars start at 1");

        // A whole part in one call: the tool the model is meant to lean on.
        json notes = json::array();
        for (int i = 0; i < 4; ++i)
            notes.push_back(json{{"pitch", 60 + i},
                                 {"start", i * 1.0},
                                 {"length", 0.9},
                                 {"velocity", 80 + i}});
        const ai::ToolResult written =
            call(c, "set_clip_notes",
                 json{{"trackId", pianoId}, {"clipId", clipId}, {"notes", notes}});
        track = c.project().findTrack(pianoId);
        check(written.ok && track->clips[0].notes.size() == 4 &&
                  track->clips[0].notes[0].pitch == 60 &&
                  track->clips[0].notes[3].velocity == 83,
              "set_clip_notes writes a whole part in one call");

        // Append rather than replace, so a second part can be layered on.
        json more = json::array();
        more.push_back(json{{"pitch", 72}, {"start", 0.0}, {"length", 4.0}});
        check(call(c, "set_clip_notes", json{{"trackId", pianoId},
                                             {"clipId", clipId},
                                             {"notes", more},
                                             {"mode", "append"}})
                      .ok &&
                  c.project().findTrack(pianoId)->clips[0].notes.size() == 5,
              "mode append keeps what was already in the clip");
        check(call(c, "set_clip_notes", json{{"trackId", pianoId},
                                             {"clipId", clipId},
                                             {"notes", more}})
                      .ok &&
                  c.project().findTrack(pianoId)->clips[0].notes.size() == 1,
              "and the default replaces it");

        // A part longer than the clip must not be silently truncated — notes at
        // or past the end never sound, which is how the assistant's work
        // disappears. The clip grows instead, to a whole bar.
        json late = json::array();
        late.push_back(json{{"pitch", 60}, {"start", 0.0}, {"length", 1.0}});
        late.push_back(json{{"pitch", 62}, {"start", 9.0}, {"length", 1.0}});
        const ai::ToolResult grown =
            call(c, "set_clip_notes",
                 json{{"trackId", pianoId}, {"clipId", clipId}, {"notes", late}});
        const daw::ClipModel& grownClip = c.project().findTrack(pianoId)->clips[0];
        check(grown.ok && grown.value.contains("note") &&
                  std::abs(grownClip.durationSeconds - 6.0) < 1e-6,
              "a part past the clip's end lengthens the clip to hold it");
        check(!call(c, "set_clip_notes",
                    json{{"trackId", pianoId},
                         {"clipId", clipId},
                         {"notes", json::array({json{{"pitch", 60},
                                                     {"start", 0.0},
                                                     {"length", 1.0}}})}})
                   .value.contains("note") &&
                  std::abs(c.project().findTrack(pianoId)->clips[0].durationSeconds -
                           6.0) < 1e-6,
              "and a shorter part afterwards does not shrink it back");

        const ai::ToolResult badNote =
            call(c, "set_clip_notes",
                 json{{"trackId", pianoId},
                      {"clipId", clipId},
                      {"notes", json::array({json{{"pitch", 500},
                                                  {"start", 0},
                                                  {"length", 1}}})}});
        check(!badNote.ok && mentions(badNote.error, "127"),
              "a pitch outside 0-127 is refused with the range named");

        const ai::ToolResult noLength =
            call(c, "set_clip_notes",
                 json{{"trackId", pianoId},
                      {"clipId", clipId},
                      {"notes", json::array({json{{"pitch", 60}, {"start", 0}}})}});
        check(!noLength.ok && mentions(noLength.error, "length"),
              "a note missing a field is refused, naming the field");

        const ai::ToolResult read =
            call(c, "get_clip_notes", json{{"trackId", pianoId}, {"clipId", clipId}});
        check(read.ok && read.value["notes"].size() == 1 &&
                  read.value["notes"][0]["name"] == "C5",
              "get_clip_notes reads them back, naming 60 as C5 like the piano roll");

        const ai::ToolResult copy =
            call(c, "duplicate_clip", json{{"trackId", pianoId}, {"clipId", clipId}});
        check(copy.ok && c.project().findTrack(pianoId)->clips.size() == 2,
              "duplicate_clip repeats a section");

        const std::string copyId = copy.value.value("clipId", "");
        check(call(c, "set_clip_start",
                   json{{"trackId", pianoId}, {"clipId", copyId}, {"startBar", 5}})
                      .ok,
              "set_clip_start moves a clip to another bar");
        const daw::ClipModel* moved = nullptr;
        for (const daw::ClipModel& cl : c.project().findTrack(pianoId)->clips)
            if (cl.id == copyId) moved = &cl;
        check(moved && std::abs(moved->startSeconds - 8.0) < 1e-6,
              "and bar 5 is eight seconds in at 120 BPM in 4/4");

        check(call(c, "remove_clip", json{{"trackId", pianoId}, {"clipId", copyId}}).ok &&
                  c.project().findTrack(pianoId)->clips.size() == 1,
              "remove_clip deletes it");
        check(!call(c, "remove_clip",
                    json{{"trackId", pianoId}, {"clipId", "gone"}})
                   .ok,
              "removing a clip that is not there is refused, not ignored");
    }

    // ── Key and the note transforms ──
    // These sit on the pure functions in MidiTools, which are tested in their
    // own right; what matters here is that the tool reaches them with sane
    // parameters and refuses the arguments a model gets wrong.
    {
        check(call(c, "set_project_key",
                   json{{"root", "F#"}, {"scale", "natural_minor"}})
                      .ok &&
                  c.keyRoot() == 6 && c.projectScale() == "natural_minor",
              "set_project_key stores a tonic and a scale");
        check(call(c, "set_project_key", json{{"root", "Bb"}, {"scale", "minor"}})
                      .ok &&
                  c.keyRoot() == 10 && c.projectScale() == "natural_minor",
              "and \"minor\" is understood as natural minor");
        const ai::ToolResult badRoot =
            call(c, "set_project_key", json{{"root", "H"}, {"scale", "major"}});
        check(!badRoot.ok && mentions(badRoot.error, "note name"),
              "a tonic that is not a note name is refused");
        const ai::ToolResult badScale =
            call(c, "set_project_key", json{{"root", "C"}, {"scale", "klezmer"}});
        check(!badScale.ok && mentions(badScale.error, "mixolydian"),
              "an unknown scale is refused, with the real ones listed");
        // Reported back with sharp spelling — a pitch class does not remember
        // whether the user typed Bb or A#, and inventing document state to keep
        // the spelling is not worth it.
        check(ai::projectSnapshot(c).value("key", std::string()) ==
                  "A# Natural Minor",
              "and the key is reported in the snapshot");

        // A deliberately loose part to transform.
        const std::string tId =
            call(c, "add_track", json{{"kind", "instrument"}, {"name", "T"}})
                .value.value("trackId", "");
        const std::string cId =
            call(c, "add_midi_clip",
                 json{{"trackId", tId}, {"startBar", 1}, {"lengthBars", 1}})
                .value.value("clipId", "");
        json loose = json::array();
        loose.push_back(json{{"pitch", 61}, {"start", 0.03}, {"length", 1.0}});
        loose.push_back(json{{"pitch", 64}, {"start", 1.07}, {"length", 1.0}});
        call(c, "set_clip_notes",
             json{{"trackId", tId}, {"clipId", cId}, {"notes", loose}});

        check(call(c, "transform_notes", json{{"trackId", tId},
                                              {"clipId", cId},
                                              {"operation", "quantize"},
                                              {"grid", 4}})
                      .ok &&
                  c.project().findTrack(tId)->clips[0].notes[0].startBeats == 0.0,
              "quantize pulls a loose start onto the grid");

        check(call(c, "transform_notes", json{{"trackId", tId},
                                              {"clipId", cId},
                                              {"operation", "transpose"},
                                              {"semitones", 12}})
                      .ok &&
                  c.project().findTrack(tId)->clips[0].notes[0].pitch == 73,
              "transpose moves the part");

        // C major: 73 is C#6, which is not in the scale and must move.
        call(c, "set_project_key", json{{"root", "C"}, {"scale", "major"}});
        check(call(c, "transform_notes", json{{"trackId", tId},
                                              {"clipId", cId},
                                              {"operation", "snap_to_scale"}})
                      .ok &&
                  c.project().findTrack(tId)->clips[0].notes[0].pitch == 72,
              "snap_to_scale pulls a note into the project's key");

        check(call(c, "transform_notes", json{{"trackId", tId},
                                              {"clipId", cId},
                                              {"operation", "build_chords"},
                                              {"chord", "minor7"}})
                      .ok &&
                  c.project().findTrack(tId)->clips[0].notes.size() == 8,
              "build_chords stacks a chord on every note");

        const ai::ToolResult badChord =
            call(c, "transform_notes", json{{"trackId", tId},
                                            {"clipId", cId},
                                            {"operation", "build_chords"},
                                            {"chord", "wonky"}});
        check(!badChord.ok && mentions(badChord.error, "minor7"),
              "an unknown chord type is refused, with the real ones listed");

        const ai::ToolResult badOp =
            call(c, "transform_notes",
                 json{{"trackId", tId}, {"clipId", cId}, {"operation", "swing"}});
        check(!badOp.ok, "an unknown transform is refused");

        const std::string emptyClip =
            call(c, "add_midi_clip",
                 json{{"trackId", tId}, {"startBar", 9}, {"lengthBars", 1}})
                .value.value("clipId", "");
        const ai::ToolResult onEmpty =
            call(c, "transform_notes", json{{"trackId", tId},
                                            {"clipId", emptyClip},
                                            {"operation", "humanize"}});
        check(!onEmpty.ok && mentions(onEmpty.error, "set_clip_notes"),
              "transforming an empty clip says to write the notes first");

        call(c, "remove_track", json{{"trackId", tId}});
    }

    // ── Instruments and plugins ──
    {
        const ai::ToolResult instruments =
            call(c, "list_plugins", json{{"kind", "instrument"}});
        check(instruments.ok && instruments.value.contains("plugins"),
              "list_plugins answers with a list");

        // The built-in sampler is always there, scan or no scan, so it is the
        // one plugin every check below can rely on.
        bool sawSampler = false;
        for (const json& p : instruments.value["plugins"])
            if (p.value("uid", "") == "daw.sampler") sawSampler = true;
        check(sawSampler, "and the built-in sampler is always in it");

        check(!call(c, "list_plugins", json{{"kind", "reverb"}}).ok,
              "an unknown plugin kind is refused");

        const ai::ToolResult unknownPlugin =
            call(c, "set_track_instrument", json{{"trackId", pianoId},
                                                 {"format", "vst3"},
                                                 {"uid", "com.made.up"}});
        check(!unknownPlugin.ok && mentions(unknownPlugin.error, "list_plugins"),
              "an invented plugin uid is refused, pointing at list_plugins");

        const ai::ToolResult badFormat =
            call(c, "set_track_instrument", json{{"trackId", pianoId},
                                                 {"format", "aax"},
                                                 {"uid", "x"}});
        check(!badFormat.ok && mentions(badFormat.error, "clap"),
              "an unknown plugin format is refused, with the real ones listed");

        const ai::ToolResult loaded =
            call(c, "set_track_instrument", json{{"trackId", pianoId},
                                                 {"format", "internal"},
                                                 {"uid", "daw.sampler"}});
        const daw::TrackModel* track = c.project().findTrack(pianoId);
        check(loaded.ok && track->instrument.isLoaded() &&
                  loaded.value.value("insertId", "") == track->instrument.id,
              "set_track_instrument fills the slot and returns its id");

        // The instrument slot answers to the same parameter calls as an insert.
        const std::string slotId = track->instrument.id;
        const ai::ToolResult params =
            call(c, "list_plugin_parameters",
                 json{{"channelId", pianoId}, {"insertId", slotId}});
        check(params.ok && params.value["parameters"].size() > 4,
              "list_plugin_parameters reads the sampler's parameters");

        const std::string firstId =
            params.value["parameters"][0].value("id", "");
        const ai::ToolResult wrongParam =
            call(c, "set_insert_parameter", json{{"channelId", pianoId},
                                                 {"insertId", slotId},
                                                 {"parameterId", "loudness"},
                                                 {"value", 1.0}});
        check(!wrongParam.ok && mentions(wrongParam.error, firstId.c_str()),
              "an invented parameter id is refused, listing the real ones");

        const double before =
            c.insertParameter(pianoId, slotId, firstId);
        const std::size_t mark = c.undoDepth();
        const ai::ToolResult setParam =
            call(c, "set_insert_parameter",
                 json{{"channelId", pianoId},
                      {"insertId", slotId},
                      {"parameterId", firstId},
                      {"value", params.value["parameters"][0].value("min", 0.0)}});
        check(setParam.ok && c.undoDepth() == mark + 1,
              "setting a parameter lands on the undo stack, not just the plugin");
        c.undo();
        check(std::abs(c.insertParameter(pianoId, slotId, firstId) - before) < 1e-6,
              "and one undo puts it back");

        const ai::ToolResult instrumentAsEffect =
            call(c, "add_insert", json{{"channelId", "master"},
                                       {"format", "internal"},
                                       {"uid", "daw.sampler"}});
        check(!instrumentAsEffect.ok &&
                  mentions(instrumentAsEffect.error, "set_track_instrument"),
              "putting an instrument in an insert slot is refused, with the fix");

        check(!call(c, "add_insert", json{{"channelId", "nowhere"},
                                          {"format", "internal"},
                                          {"uid", "daw.sampler"}})
                   .ok,
              "an unknown channel is refused");

        // An audio track has no instrument slot, and saying so is more useful
        // than silently doing nothing.
        const std::string audioId =
            call(c, "add_track", json{{"kind", "audio"}, {"name", "Vocal"}})
                .value.value("trackId", "");
        const ai::ToolResult wrongKind =
            call(c, "set_track_instrument", json{{"trackId", audioId},
                                                 {"format", "internal"},
                                                 {"uid", "daw.sampler"}});
        check(!wrongKind.ok && mentions(wrongKind.error, "add_track"),
              "an audio track refuses an instrument and says what to do instead");

        const ai::ToolResult missingFile =
            call(c, "load_sampler",
                 json{{"trackId", pianoId}, {"filePath", "/no/such/sound.wav"}});
        check(!missingFile.ok && mentions(missingFile.error, "sound.wav"),
              "load_sampler refuses a path that is not there, naming it");
    }

    // ── Measuring, so a mix decision can rest on numbers ──
    // The DSP itself is checked against signals synthesised here: a test that
    // only asserted "some number came back" would not catch a band split that
    // is upside down, which is exactly the failure that would make the
    // assistant's mixing advice confidently wrong.
    {
        const double rate = 48000.0;
        const std::size_t frames = 24000;
        std::vector<float> low(frames), high(frames), quiet(frames, 0.0f),
            loud(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            low[i] = 0.5f * std::sin(2.0f * 3.14159265f * 60.0f * i / float(rate));
            high[i] = 0.5f * std::sin(2.0f * 3.14159265f * 9000.0f * i / float(rate));
            loud[i] = i % 2 ? 1.0f : -1.0f;
        }
        const float* lowCh[] = {low.data()};
        const float* highCh[] = {high.data()};
        const float* quietCh[] = {quiet.data()};
        const float* loudCh[] = {loud.data()};

        const daw::analysis::Metrics lowM =
            daw::analysis::measure(lowCh, 1, frames, rate);
        const daw::analysis::Metrics highM =
            daw::analysis::measure(highCh, 1, frames, rate);
        check(lowM.lowFraction > 0.8 && lowM.highFraction < 0.1,
              "a 60 Hz tone reads as almost entirely low");
        check(highM.highFraction > 0.8 && highM.lowFraction < 0.1,
              "a 9 kHz tone reads as almost entirely high");
        check(std::abs(lowM.peakDb + 6.02) < 0.5 && lowM.clipped == 0,
              "a half-scale tone measures about -6 dB and does not clip");
        check(daw::analysis::measure(quietCh, 1, frames, rate).silent,
              "silence is reported as silence rather than -inf noise");
        check(daw::analysis::measure(loudCh, 1, frames, rate).clipped == frames,
              "a full-scale square is reported as clipping every sample");

        // Tonal against percussive: the distinction the model reasons with.
        std::vector<float> note(frames);
        for (std::size_t i = 0; i < frames; ++i)
            note[i] = 0.6f * std::sin(2.0f * 3.14159265f * 440.0f * i / float(rate));
        const float* noteCh[] = {note.data()};
        const daw::analysis::SampleTraits tonal =
            daw::analysis::describe(noteCh, 1, frames, rate);
        check(tonal.tonal && tonal.pitch == 69,
              "a 440 Hz tone is recognised as A, at MIDI 69");

        std::vector<float> hit(frames, 0.0f);
        unsigned seed = 1;
        for (std::size_t i = 0; i < 2000; ++i) {
            seed = seed * 1103515245u + 12345u;
            const float noise = float((seed >> 16) % 2000) / 1000.0f - 1.0f;
            hit[i] = noise * float(1.0 - double(i) / 2000.0);
        }
        const float* hitCh[] = {hit.data()};
        const daw::analysis::SampleTraits percussive =
            daw::analysis::describe(hitCh, 1, frames, rate);
        check(!percussive.tonal && percussive.attackSeconds < 0.02,
              "a noise burst is not called a note, and reads as a fast attack");

        // And through the tools, against a real rendered project.
        daw::EngineController m;
        m.initialize(48000.0, 512, /*openDevice=*/false);
        const std::string tonePath = (dir / "measure.wav").string();
        writeTone(tonePath, 48000, 24000);
        const std::string audioTrack = m.addTrack(daw::TrackKind::Audio, "Tone");
        m.importAudio(tonePath, audioTrack, 0.0);

        const ai::ToolResult measured =
            ai::callTool(m, "analyze_track", json{{"channelId", audioTrack}});
        check(measured.ok && measured.value.contains("peakDb") &&
                  measured.value.contains("headroomDb") &&
                  measured.value["seconds"].get<double>() > 0.0,
              "analyze_track measures a real channel through the engine");

        const ai::ToolResult masterMeasured =
            ai::callTool(m, "analyze_track", json{{"channelId", "master"}});
        check(masterMeasured.ok, "and the master, which is the mix as it stands");

        // Soloing is how one channel is heard alone; it must be handed back.
        check(!m.project().findTrack(audioTrack)->soloed &&
                  m.undoDepth() == m.undoDepth(),
              "measuring leaves the solo state as it found it");

        const std::string silentTrack = m.addTrack(daw::TrackKind::Audio, "Empty");
        const ai::ToolResult silent =
            ai::callTool(m, "analyze_track", json{{"channelId", silentTrack}});
        check(silent.ok && silent.value.value("silent", false),
              "a channel with nothing on it says so, instead of returning -120");

        const ai::ToolResult described =
            ai::callTool(m, "analyze_sample", json{{"filePath", tonePath}});
        check(described.ok && described.value.contains("character") &&
                  described.value.contains("peakDb"),
              "analyze_sample describes a file on disk");
        check(!ai::callTool(m, "analyze_sample",
                            json{{"filePath", "/no/such/file.wav"}})
                   .ok,
              "and refuses a path that is not there");
    }

    // ── Searching the user's own folders ──
    {
        const ai::ToolResult noFolders =
            call(c, "search_files", json{{"query", "kick"}});
        check(!noFolders.ok && mentions(noFolders.error, "browser"),
              "with no folders added there is nowhere to search, and it says so");

        // A small tree, so the search has something real to walk.
        const fs::path root = dir / "samples" / "drums";
        fs::create_directories(root);
        writeTone((root / "Kick_808.wav").string(), 48000, 2048);
        writeTone((root / "snare_tight.wav").string(), 48000, 2048);
        writeTone((dir / "samples" / "pad.wav").string(), 48000, 2048);
        std::ofstream((dir / "samples" / "notes.txt").string()) << "not audio";

        gContext.sampleFolders = {(dir / "samples").string()};

        const ai::ToolResult byName =
            call(c, "search_files", json{{"query", "kick"}});
        check(byName.ok && byName.value["files"].size() == 1 &&
                  byName.value["files"][0]["name"] == "Kick_808.wav",
              "a name search finds a file in a sub-folder, ignoring case");
        check(!byName.value["files"].empty() &&
                  byName.value["files"][0].contains("seconds") &&
                  byName.value["files"][0]["channels"] == 2,
              "and reports what the file is, without decoding it");

        const ai::ToolResult byExt =
            call(c, "search_files", json{{"query", ".wav"}});
        check(byExt.ok && byExt.value["files"].size() == 3,
              "an extension search lists every audio file under the folders");

        const ai::ToolResult nothing =
            call(c, "search_files", json{{"query", "trombone"}});
        check(nothing.ok && nothing.value["files"].empty() &&
                  nothing.value.contains("note"),
              "no match is an answer, not an error");

        const ai::ToolResult text =
            call(c, "search_files", json{{"query", "notes"}});
        check(text.ok && text.value["files"].empty(),
              "files the program cannot decode are never listed");

        const ai::ToolResult capped =
            call(c, "search_files", json{{"query", ""}, {"limit", 2}});
        check(capped.ok && capped.value["files"].size() == 2 &&
                  capped.value.value("truncated", false),
              "a limit is honoured and the truncation is admitted");
        gContext.sampleFolders.clear();
    }

    // ── The snapshot the prompt carries ──
    {
        const json snapshot = ai::projectSnapshot(c);
        check(snapshot.contains("tempo") && snapshot.contains("tracks") &&
                  snapshot["tracks"].size() >= 3,
              "the snapshot carries the tempo and every track");

        bool foundPiano = false;
        for (const json& t : snapshot["tracks"])
            if (t.value("id", "") == pianoId) {
                foundPiano = t.contains("instrument") && t.contains("clips") &&
                             t.contains("sends") && t.contains("volumeDb");
            }
        check(foundPiano,
              "and for a track, its instrument, clips, sends and level");

        const std::string prompt = ai::systemPrompt(c);
        check(prompt.find("C5") != std::string::npos &&
                  prompt.find(pianoId) != std::string::npos,
              "the system prompt states this program's note naming and the project");

        c.setAiInstructions("Always leave 6 dB of headroom on the master.");
        const std::string withRules = ai::systemPrompt(c);
        check(withRules.find("6 dB of headroom") != std::string::npos &&
                  withRules.find("override") != std::string::npos,
              "standing instructions reach the prompt, framed as overriding");
        c.setAiInstructions("");

        ai::ToolContext withSample;
        withSample.attachments.push_back(
            ai::Attachment{"kick_808.wav", "/tmp/kick_808.wav", 0.8, 44100, 1});
        const std::string withFile = ai::systemPrompt(c, withSample);
        check(withFile.find("kick_808.wav") != std::string::npos &&
                  withFile.find("cannot listen") != std::string::npos,
              "an attachment reaches the prompt, with the model told it cannot hear it");
    }

    // ── The agent loop ──
    {
        daw::EngineController m;
        m.initialize(48000.0, 512, /*openDevice=*/false);
        ai::AiSession session(m);

        check(session.begin("make a piano part"), "a run opens");
        check(!session.begin("again"), "and a second one cannot open on top of it");

        // Round one: the model creates a track and a clip.
        ai::ModelReply first;
        first.text = "Making a piano.";
        first.calls.push_back(
            {"c1", "add_track", json{{"kind", "instrument"}, {"name", "Piano"}}});
        check(session.applyReply(first) == ai::AiSession::Step::NeedsRequest,
              "a reply with tool calls asks for another request");

        const std::string trackId =
            session.messages().back().outcomes[0].result.value("trackId", "");
        check(!trackId.empty() && m.project().tracks.size() == 1,
              "the tool really ran against the document");

        ai::ModelReply second;
        second.calls.push_back({"c2", "add_midi_clip",
                                json{{"trackId", trackId},
                                     {"startBar", 1},
                                     {"lengthBars", 1}}});
        second.calls.push_back({"c3", "set_tempo", json{{"bpm", 90}}});
        check(session.applyReply(second) == ai::AiSession::Step::NeedsRequest &&
                  m.tempo() == 90.0,
              "several calls in one reply all run, in order");

        // A failing call must not stop the run: the model reads the error and
        // gets another go.
        ai::ModelReply third;
        third.calls.push_back({"c4", "rename_track",
                               json{{"trackId", "wrong"}, {"name", "X"}}});
        check(session.applyReply(third) == ai::AiSession::Step::NeedsRequest &&
                  !session.messages().back().outcomes[0].ok,
              "a failed tool call is reported back rather than ending the run");

        ai::ModelReply last;
        last.text = "Done — a piano in C minor.";
        check(session.applyReply(last) == ai::AiSession::Step::Finished &&
                  !session.running(),
              "a reply with no tool calls ends the run");

        check(m.undoDepth() == 1 && m.undoLabel() == "AI: make a piano part",
              "everything the run did is one undo entry, named after the request");
        m.undo();
        check(m.project().tracks.empty() && m.tempo() == 120.0,
              "and one undo takes all of it back");
        m.redo();
        check(m.project().tracks.size() == 1 && m.tempo() == 90.0,
              "one redo replays all of it");

        check(session.messages().size() == 8,
              "the transcript holds the user turn, each reply and each result");
    }

    // ── What real models actually send ──
    // Both of these came out of one live run: the note list arrived as a string
    // of JSON, and the call itself arrived as prose rather than through the
    // tool channel. Refusing either threw away a part the model had written.
    {
        daw::EngineController m;
        m.initialize(48000.0, 512, /*openDevice=*/false);
        const std::string track =
            call(m, "add_track", json{{"kind", "instrument"}, {"name", "Keys"}})
                .value.value("trackId", "");
        const std::string clip =
            call(m, "add_midi_clip",
                 json{{"trackId", track}, {"startBar", 1}, {"lengthBars", 4}})
                .value.value("clipId", "");

        const ai::ToolResult stringNotes = call(
            m, "set_clip_notes",
            json{{"trackId", track},
                 {"clipId", clip},
                 {"mode", "replace"},
                 {"notes",
                  "[{\"length\": 1.0, \"pitch\": 72, \"start\": 0, "
                  "\"velocity\": 100}, {\"length\": 1.0, \"pitch\": 74, "
                  "\"start\": 1, \"velocity\": 100}]"}});
        check(stringNotes.ok &&
                  m.project().findTrack(track)->clips[0].notes.size() == 2,
              "a note list sent as a string of JSON is still written");

        const ai::ToolResult notEvenJson =
            call(m, "set_clip_notes",
                 json{{"trackId", track}, {"clipId", clip}, {"notes", "soon"}});
        check(!notEvenJson.ok && mentions(notEvenJson.error, "array"),
              "but a string that is not a note list is still refused");

        // A tool call written into the prose, the way it arrived in the log.
        std::string prose =
            "Sure, here you go. {\"name\": \"set_tempo\", "
            "\"parameters\": {\"bpm\": 128}} Done.";
        std::vector<ai::ToolCall> recovered = ai::toolCallsInText(prose);
        check(recovered.size() == 1 && recovered[0].name == "set_tempo" &&
                  recovered[0].fromText,
              "a call written as prose is recovered, and marked as such");
        check(prose.find('{') == std::string::npos && mentions(prose, "Sure"),
              "the JSON is cut out of what the user reads, the prose is kept");

        // "arguments" and "input" are the other two spellings seen in the wild.
        std::string alt = "{\"name\": \"set_tempo\", \"arguments\": {\"bpm\": 90}}";
        check(ai::toolCallsInText(alt).size() == 1,
              "the argument object is accepted under any of its three names");

        // Ordinary prose that happens to contain JSON must be left alone, or
        // the assistant would start executing things it was only describing.
        std::string innocent =
            "The project file looks like {\"name\": \"My Song\", \"tempo\": 120}.";
        const std::string before = innocent;
        check(ai::toolCallsInText(innocent).empty() && innocent == before,
              "JSON that names no real tool is left as text");

        // Braces inside strings must not end the object early — a note list is
        // full of them once it arrives stringified.
        std::string nested =
            "{\"name\": \"rename_track\", \"parameters\": "
            "{\"trackId\": \"x\", \"name\": \"a } brace\"}}";
        std::vector<ai::ToolCall> tricky = ai::toolCallsInText(nested);
        check(tricky.size() == 1 &&
                  tricky[0].args.value("name", std::string()) == "a } brace",
              "a brace inside a string does not truncate the call");

        // And the whole thing through the session: recovered, run, transcript
        // clean, result marked so the next request sends it as text.
        ai::AiSession session(m);
        session.begin("set the tempo");
        ai::ModelReply proseReply;
        proseReply.text =
            "Setting it now. {\"name\": \"set_tempo\", \"parameters\": "
            "{\"bpm\": 140}}";
        check(session.applyReply(proseReply) == ai::AiSession::Step::NeedsRequest &&
                  m.tempo() == 140.0,
              "a prose call runs against the document like any other");
        check(session.messages().back().outcomes[0].fromText,
              "and its result is marked to go back as text, not as a tool result");
    }

    // ── Refusing to destroy work, and taking a request back ──
    {
        daw::EngineController m;
        m.initialize(48000.0, 512, /*openDevice=*/false);
        const std::string keep = m.addTrack(daw::TrackKind::Audio, "Vocals");

        ai::ToolContext refusing;
        std::string asked;
        refusing.confirmDestructive = [&asked](const std::string& what) {
            asked = what;
            return false;
        };
        const ai::ToolResult refused = ai::callTool(
            m, "remove_track", json{{"trackId", keep}}, refusing);
        check(!refused.ok && m.project().tracks.size() == 1,
              "a refused deletion does not happen");
        check(mentions(asked, "Vocals"),
              "and the question names what would be lost");
        check(mentions(refused.error, "declined") &&
                  mentions(refused.error, "again"),
              "the model is told to stop rather than find another way");

        ai::ToolContext allowing;
        allowing.confirmDestructive = [](const std::string&) { return true; };
        check(ai::callTool(m, "remove_track", json{{"trackId", keep}}, allowing).ok &&
                  m.project().tracks.empty(),
              "an allowed deletion goes through");
        check(ai::callTool(m, "add_track",
                           json{{"kind", "audio"}, {"name", "X"}}, refusing)
                  .ok,
              "and nothing constructive is ever asked about");
    }

    {
        // Reverting a request the user has already worked past — the case
        // plain undo cannot serve, because the entry is no longer on top.
        daw::EngineController m;
        m.initialize(48000.0, 512, /*openDevice=*/false);
        ai::AiSession session(m);

        session.begin("add a bass");
        ai::ModelReply made;
        made.calls.push_back(
            {"a", "add_track", json{{"kind", "instrument"}, {"name", "Bass"}}});
        session.applyReply(made);
        session.applyReply(ai::ModelReply{});
        check(session.checkpoints().size() == 1,
              "a request that changed something leaves a checkpoint");

        // The user carries on afterwards, burying the assistant's entry.
        const std::string mine = m.addTrack(daw::TrackKind::Audio, "Mine");
        m.setTrackVolume(mine, 0.5f);
        check(m.project().tracks.size() == 2, "and the user's own work lands");

        // A snapshot restores the whole document: the user's own later work
        // goes with it. That is the honest behaviour of a checkpoint, and the
        // UI says so before using one — the alternative would be a diff engine
        // that could get it subtly wrong instead.
        check(session.revertTo(0) && m.project().tracks.empty(),
              "reverting puts the whole project back, later work included");
        m.undo();
        check(m.project().tracks.size() == 2 &&
                  m.project().findTrack(mine)->volume == 0.5f,
              "and one undo brings all of it back, so nothing is ever lost");

        // A request that changed nothing is not worth a snapshot.
        session.begin("what is the tempo?");
        session.applyReply(ai::ModelReply{});
        check(session.checkpoints().size() == 1,
              "a request that changed nothing leaves no checkpoint");
        check(!session.revertTo(99), "and an unknown checkpoint is refused");
    }

    // ── Transport errors and the cap ──
    {
        daw::EngineController m;
        m.initialize(48000.0, 512, /*openDevice=*/false);
        ai::AiSession session(m);

        session.begin("anything");
        ai::ModelReply broken;
        broken.error = "the API key was rejected";
        check(session.applyReply(broken) == ai::AiSession::Step::Failed &&
                  !session.running() &&
                  session.lastError() == "the API key was rejected",
              "a transport error ends the run and is kept for the user");

        session.setMaxIterations(2);
        session.begin("loop forever");
        ai::ModelReply spin;
        spin.calls.push_back({"x", "get_project", json::object()});
        check(session.applyReply(spin) == ai::AiSession::Step::NeedsRequest &&
                  session.applyReply(spin) == ai::AiSession::Step::NeedsRequest,
              "the cap allows the rounds it promises");
        check(session.applyReply(spin) == ai::AiSession::Step::Failed &&
                  session.lastError().find("2 rounds") != std::string::npos,
              "and cuts the run off past them, saying so");
    }

    // ── What the wire carries, and what it costs ──
    {
        daw::EngineController m;
        m.initialize(48000.0, 512, /*openDevice=*/false);
        ai::AiSession session(m);
        session.setHistoryLimit(2);

        for (int turn = 0; turn < 4; ++turn) {
            session.begin("turn " + std::to_string(turn));
            ai::ModelReply withCall;
            withCall.calls.push_back({"c", "get_project", json::object()});
            session.applyReply(withCall);
            session.applyReply(ai::ModelReply{});
        }
        check(session.messages().size() == 16,
              "the transcript keeps every turn for the user to read");

        const std::vector<ai::Message> wire = session.wireMessages();
        check(wire.size() < session.messages().size(),
              "but the request does not carry all of it");
        check(!wire.empty() && wire.front().role == ai::Role::User,
              "and it always begins on a user turn — a tool result whose call "
              "was trimmed away has the whole request rejected");

        session.setHistoryLimit(0);
        check(session.wireMessages().size() == session.messages().size(),
              "a limit of zero sends everything");

        session.addUsage({1000, 200, 800});
        session.addUsage({1200, 150, 1100});
        check(session.usage().inputTokens == 2200 &&
                  session.usage().outputTokens == 350 &&
                  session.usage().cachedTokens == 1900,
              "usage accumulates across the steps of a conversation");
        session.clear();
        check(session.usage().inputTokens == 0,
              "and starting again starts the count again");
    }

    // ── Stopping, and what a stopped run leaves behind ──
    {
        daw::EngineController m;
        m.initialize(48000.0, 512, /*openDevice=*/false);
        ai::AiSession session(m);

        session.begin("add three tracks");
        ai::ModelReply one;
        one.calls.push_back(
            {"a", "add_track", json{{"kind", "audio"}, {"name", "One"}}});
        session.applyReply(one);
        session.cancel();

        ai::ModelReply two;
        two.calls.push_back(
            {"b", "add_track", json{{"kind", "audio"}, {"name", "Two"}}});
        check(session.applyReply(two) == ai::AiSession::Step::Finished &&
                  m.project().tracks.size() == 1,
              "stopping runs no further tools");
        check(m.undoDepth() == 1, "and what it did make is still one undo entry");
        m.undo();
        check(m.project().tracks.empty(), "which takes it back");
    }

    // ── The wire: both providers' shapes, and streaming ──
    // None of this needs a network, which is the only reason streaming can be
    // checked at all. The bytes below are the event shapes each provider
    // documents, fed in split across chunk boundaries the way a socket
    // delivers them.
    {
        using namespace daw::ai::wire;

        std::vector<ai::Message> conversation;
        conversation.push_back({ai::Role::User, "make a bass", {}, {}});
        ai::Message assistant{ai::Role::Assistant, "On it.", {}, {}};
        assistant.calls.push_back(
            {"call_1", "add_track", json{{"kind", "instrument"}}, false});
        assistant.calls.push_back(
            {"text-0", "set_tempo", json{{"bpm", 90}}, /*fromText=*/true});
        conversation.push_back(assistant);
        ai::Message results{ai::Role::Tool, "", {}, {}};
        results.outcomes.push_back(
            {"call_1", "add_track", true, json{{"ok", true}}, false});
        results.outcomes.push_back(
            {"text-0", "set_tempo", true, json{{"ok", true}}, /*fromText=*/true});
        conversation.push_back(results);

        const json claude =
            requestBody(Provider::Anthropic, "m", 4096, "SYSTEM", conversation, true);
        check(claude["stream"] == true && claude["max_tokens"] == 4096 &&
                  claude["tools"].size() == ai::toolSpecs().size(),
              "the Anthropic body carries the tools and asks for a stream");
        check(claude["system"][0]["cache_control"]["type"] == "ephemeral" &&
                  claude["tools"].back().contains("cache_control"),
              "and marks the system prompt and the schemas cacheable, which is "
              "what stops a long run paying for them on every step");

        const json& claudeAssistant = claude["messages"][1];
        check(claudeAssistant["content"].size() == 2,
              "a call recovered from prose is not resent as a tool_use — there "
              "is no id on the provider's side for a result to point at");
        const json& claudeResults = claude["messages"][2];
        check(claudeResults["role"] == "user",
              "Anthropic takes tool results as a user turn");
        bool sawTextResult = false;
        for (const json& block : claudeResults["content"])
            if (block["type"] == "text") sawTextResult = true;
        check(sawTextResult,
              "and the prose call's result goes back as text instead");

        // Behind someone else's OpenAI-compatible server the vendor-only
        // fields come off: a strict one rejects an unknown field and fails the
        // whole request, and losing the caching beats losing the connection.
        const json plain = requestBody(Provider::Anthropic, "m", 4096, "SYSTEM",
                                       conversation, true,
                                       /*vendorExtensions=*/false);
        check(plain["system"].is_string() &&
                  !plain["tools"].back().contains("cache_control"),
              "a third-party endpoint gets a body with no vendor extensions");

        const json gpt =
            requestBody(Provider::OpenAi, "m", 4096, "SYSTEM", conversation, true);
        check(gpt["messages"][0]["role"] == "system" &&
                  gpt["stream_options"]["include_usage"] == true,
              "the OpenAI body puts the prompt in a system turn and asks for "
              "usage, which it otherwise omits when streaming");
        check(!requestBody(Provider::OpenAi, "m", 4096, "S", conversation, true,
                           /*vendorExtensions=*/false)
                   .contains("stream_options"),
              "and that request too drops the field a compatible server may "
              "not know");
        // OpenAI prepends the system turn, so the assistant is one further on.
        const json& gptAssistant = gpt["messages"][2];
        check(gptAssistant["role"] == "assistant" &&
                  gptAssistant["tool_calls"].size() == 1 &&
                  gptAssistant["tool_calls"][0]["function"]["arguments"].is_string(),
              "its arguments travel as a string of JSON, not an object");
        bool sawProseResult = false;
        for (const json& turn : gpt["messages"])
            if (turn["role"] == "user" &&
                turn["content"].get<std::string>().find("Result of") == 0)
                sawProseResult = true;
        check(sawProseResult,
              "and a prose call's result becomes a plain user turn here too");

        check(errorMessage(Provider::Anthropic,
                           json{{"error", json{{"message", "bad key"}}}}) ==
                  "bad key",
              "a provider's own error message is preferred to the transport's");

        // ── Anthropic's stream ──
        StreamDecoder claudeStream(Provider::Anthropic);
        claudeStream.feed(
            "event: message_start\n"
            "data: {\"type\":\"message_start\",\"message\":{\"usage\":"
            "{\"input_tokens\":120,\"cache_read_input_tokens\":80}}}\n\n");
        claudeStream.feed(
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
            "{\"type\":\"text_delta\",\"text\":\"Making \"}}\n\n");
        check(claudeStream.takeText() == "Making ",
              "text arrives in pieces and can be shown as it is written");
        check(claudeStream.takeText().empty(),
              "and taking it twice does not repeat it");

        // A tool call, with its arguments split across two chunks — and the
        // second chunk cut in the middle of an event, as a socket would.
        claudeStream.feed(
            "event: content_block_start\n"
            "data: {\"type\":\"content_block_start\",\"index\":1,"
            "\"content_block\":{\"type\":\"tool_use\",\"id\":\"tu_1\","
            "\"name\":\"set_tempo\"}}\n\n"
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":"
            "{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"bp\"}}\n\n"
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_del");
        check(claudeStream.reply().calls.empty(),
              "a half-arrived call is not run — its arguments are still partial");
        claudeStream.feed(
            "ta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\","
            "\"partial_json\":\"m\\\": 128}\"}}\n\n"
            "event: content_block_stop\n"
            "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n");
        check(claudeStream.reply().calls.size() == 1 &&
                  claudeStream.reply().calls[0].name == "set_tempo" &&
                  claudeStream.reply().calls[0].args.value("bpm", 0) == 128,
              "the arguments reassemble across chunk boundaries");

        check(!claudeStream.done(), "and the stream is not over until it says so");
        claudeStream.feed(
            "event: message_delta\n"
            "data: {\"type\":\"message_delta\",\"usage\":{\"output_tokens\":42}}\n\n"
            "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n");
        check(claudeStream.done() && claudeStream.usage().inputTokens == 120 &&
                  claudeStream.usage().outputTokens == 42 &&
                  claudeStream.usage().cachedTokens == 80,
              "the end brings the output count, and the cache saving is visible");

        // ── OpenAI's stream ──
        StreamDecoder gptStream(Provider::OpenAi);
        gptStream.feed(
            "data: {\"choices\":[{\"delta\":{\"content\":\"Sure\"}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
            "\"id\":\"c1\",\"function\":{\"name\":\"set_tempo\","
            "\"arguments\":\"{\\\"bpm\\\":\"}}]}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
            "\"function\":{\"arguments\":\"140}\"}}]}}]}\n\n");
        check(gptStream.takeText() == "Sure", "OpenAI text streams the same way");
        gptStream.feed(
            "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}],"
            "\"usage\":{\"prompt_tokens\":90,\"completion_tokens\":12}}\n\n"
            "data: [DONE]\n\n");
        check(gptStream.done() && gptStream.reply().calls.size() == 1 &&
                  gptStream.reply().calls[0].args.value("bpm", 0) == 140,
              "its calls reassemble too, and [DONE] ends the stream");
        check(gptStream.usage().inputTokens == 90,
              "and the usage it was asked for comes back");

        // An error mid-stream must end it rather than hang the panel.
        StreamDecoder broken(Provider::Anthropic);
        broken.feed("event: error\ndata: {\"type\":\"error\",\"error\":"
                    "{\"message\":\"overloaded\"}}\n\n");
        check(broken.done() && broken.reply().error == "overloaded",
              "an error event ends the stream and carries its reason");
    }

    fs::remove_all(dir);
    std::printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures;
}
