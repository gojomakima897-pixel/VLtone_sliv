#include "ai/AiTools.hpp"

#include "EngineController.hpp"
#include "MidiTools.hpp"
#include "model/Document.hpp"
#include "plugins/PluginManager.hpp"

#include "platform/AudioFileDecoder.hpp"
#include "platform/PathUtils.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <cmath>
#include <cstdio>
#include <optional>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace daw::ai {

namespace {

// ── Units ───────────────────────────────────────────────────────────────────
// The tools speak bars and decibels because that is what a producer — and a
// model trained on how producers talk — thinks in. The document speaks seconds
// and linear gain. All of that conversion happens here and nowhere else.

double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

double gainToDb(double gain) {
    return gain > 1e-6 ? 20.0 * std::log10(gain) : -96.0;
}

/// Rounded to two places: a model that reads back `0.9999999` will helpfully
/// "fix" it on the next call and burn a round trip doing nothing.
double round2(double v) { return std::round(v * 100.0) / 100.0; }

// ── Argument reading ────────────────────────────────────────────────────────
// Every failure has to reach the model as a sentence, so these accumulate into
// one error string rather than throwing. The first failure wins: telling the
// model three things at once about one bad call is noise.

bool wantString(const json& args, const char* key, std::string& out,
                std::string& error) {
    if (!error.empty()) return false;
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) {
        error = std::string("missing required argument '") + key + "'";
        return false;
    }
    if (!it->is_string()) {
        error = std::string("argument '") + key + "' must be a string";
        return false;
    }
    out = it->get<std::string>();
    return true;
}

bool wantNumber(const json& args, const char* key, double& out,
                std::string& error) {
    if (!error.empty()) return false;
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) {
        error = std::string("missing required argument '") + key + "'";
        return false;
    }
    if (!it->is_number()) {
        error = std::string("argument '") + key + "' must be a number";
        return false;
    }
    out = it->get<double>();
    return true;
}

/// Absent leaves `out` alone, so the caller's current value is the default.
/// Present-but-wrong-type is still an error: a model that sent `"0.5"` needs to
/// hear about it rather than have it silently ignored.
bool optNumber(const json& args, const char* key, double& out,
               std::string& error) {
    if (!error.empty()) return false;
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return false;
    if (!it->is_number()) {
        error = std::string("argument '") + key + "' must be a number";
        return false;
    }
    out = it->get<double>();
    return true;
}

bool optBool(const json& args, const char* key, bool& out,
             std::string& error) {
    if (!error.empty()) return false;
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return false;
    if (!it->is_boolean()) {
        error = std::string("argument '") + key + "' must be true or false";
        return false;
    }
    out = it->get<bool>();
    return true;
}

bool optString(const json& args, const char* key, std::string& out,
               std::string& error) {
    if (!error.empty()) return false;
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return false;
    if (!it->is_string()) {
        error = std::string("argument '") + key + "' must be a string";
        return false;
    }
    out = it->get<std::string>();
    return true;
}

/// A model that cannot nest JSON inside a tool call sends the container as a
/// *string* of JSON instead — most often the note list, which is the one
/// argument big enough to tempt it. Accepting that costs nothing and saves a
/// whole round trip; refusing it lost the part the model had already written.
bool wantArray(const json& args, const char* key, json& out,
               std::string& error) {
    if (!error.empty()) return false;
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) {
        error = std::string("missing required argument '") + key + "'";
        return false;
    }
    if (it->is_array()) {
        out = *it;
        return true;
    }
    if (it->is_string()) {
        json parsed = json::parse(it->get<std::string>(), nullptr,
                                  /*allow_exceptions=*/false);
        if (parsed.is_array()) {
            out = std::move(parsed);
            return true;
        }
    }
    error = std::string("argument '") + key + "' must be an array";
    return false;
}

/// What the model is told when the user says no. Phrased so it stops and asks
/// rather than trying the same thing a different way.
constexpr const char* kRefused =
    "the user declined that. Do not try it again — tell them what you wanted "
    "to do and why, and let them decide.";

bool allowed(const ToolContext& ctx, const std::string& what) {
    return !ctx.confirmDestructive || ctx.confirmDestructive(what);
}

ToolResult fail(std::string message) {
    ToolResult r;
    r.ok = false;
    r.error = std::move(message);
    return r;
}

ToolResult done(json value = json::object()) {
    ToolResult r;
    r.ok = true;
    r.value = std::move(value);
    return r;
}

/// Track ids are uuids the model copied from a snapshot; getting one wrong is
/// its most likely mistake, so the message lists what does exist.
std::string unknownTrack(const EngineController& c, const std::string& id) {
    std::string known;
    for (const TrackModel& t : c.project().tracks) {
        if (!known.empty()) known += ", ";
        known += t.name + " (" + t.id + ")";
    }
    return "no track with id '" + id + "'. Tracks in this project: " +
           (known.empty() ? std::string("none") : known);
}

// ── Snapshot pieces ─────────────────────────────────────────────────────────

json insertJson(const InsertModel& slot) {
    return json{{"id", slot.id},
                {"name", slot.name},
                {"format", toString(slot.format)},
                {"uid", slot.uid},
                {"bypassed", slot.bypassed}};
}

json clipJson(const EngineController& c, const ClipModel& clip) {
    json j{{"id", clip.id},
           {"name", clip.name},
           {"kind", toString(clip.kind)},
           {"startBar", round2(secondsToBars(c, clip.startSeconds) + 1.0)},
           {"lengthBars", round2(secondsToBars(c, clip.durationSeconds))}};
    if (clip.kind == ClipKind::Midi) j["noteCount"] = clip.notes.size();
    if (clip.muted) j["muted"] = true;
    return j;
}

json trackJson(const EngineController& c, const TrackModel& t) {
    json j{{"id", t.id},
           {"name", t.name},
           {"kind", toString(t.kind)},
           {"volumeDb", round2(gainToDb(t.volume))},
           {"pan", round2(t.pan)}};
    if (t.muted) j["muted"] = true;
    if (t.soloed) j["soloed"] = true;
    if (t.instrument.isLoaded()) j["instrument"] = insertJson(t.instrument);

    json inserts = json::array();
    for (const InsertModel& slot : t.inserts)
        if (slot.isLoaded()) inserts.push_back(insertJson(slot));
    if (!inserts.empty()) j["inserts"] = std::move(inserts);

    json sends = json::array();
    for (const SendModel& s : t.sends)
        sends.push_back(json{{"id", s.id},
                             {"to", s.destinationTrackId},
                             {"level", round2(s.level)},
                             {"preFader", s.preFader}});
    if (!sends.empty()) j["sends"] = std::move(sends);

    json clips = json::array();
    for (const ClipModel& clip : t.clips) clips.push_back(clipJson(c, clip));
    if (!clips.empty()) j["clips"] = std::move(clips);
    return j;
}

// ── Lookups shared by several tools ─────────────────────────────────────────

const ClipModel* findClip(const EngineController& c, const std::string& trackId,
                          const std::string& clipId) {
    const TrackModel* track = c.project().findTrack(trackId);
    if (!track) return nullptr;
    for (const ClipModel& clip : track->clips)
        if (clip.id == clipId) return &clip;
    return nullptr;
}

/// A channel is a track id or the literal "master". Returns false with a filled
/// error when it is neither.
bool channelExists(const EngineController& c, const std::string& channelId) {
    return channelId == EngineController::kMasterChannelId ||
           c.project().findTrack(channelId) != nullptr;
}

/// Resolve a plugin the model named by format + uid, or explain what is there.
std::optional<plugins::PluginDescriptor> findPlugin(EngineController& c,
                                                    const std::string& format,
                                                    const std::string& uid,
                                                    std::string& error) {
    const plugins::Format fmt = plugins::formatFromString(format);
    if (fmt == plugins::Format::Unknown) {
        error = "unknown plugin format '" + format +
                "'. Use one of: clap, vst3, au, internal";
        return std::nullopt;
    }
    if (const auto found = c.pluginManager().find(fmt, uid)) return found;
    error = "no " + format + " plugin with uid '" + uid +
            "' is installed. Call list_plugins and use a uid from that list "
            "exactly as given — uids cannot be guessed from a plugin's name.";
    return std::nullopt;
}

// A sample library can hold hundreds of thousands of files; these are what
// keep a search from stalling the request. The same shape the browser's own
// search worker uses, and for the same reason.
constexpr std::size_t kSearchLimit = 200;
constexpr std::size_t kSearchVisitLimit = 60000;
constexpr int kSearchDepth = 8;

/// Matches a name, or an extension written as "wav", ".wav" or "*.wav" — the
/// same rule the browser's search field follows, so the two cannot disagree
/// about what a query means.
bool matchesQuery(const std::string& fileName, const std::string& query) {
    if (query.empty()) return true;
    std::string needle = query;
    if (needle.rfind("*.", 0) == 0) needle = needle.substr(2);
    else if (needle[0] == '.') needle = needle.substr(1);
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });

    std::string lowered = fileName;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });

    const auto dot = lowered.rfind('.');
    if (dot != std::string::npos && lowered.substr(dot + 1) == needle) return true;
    return lowered.find(needle) != std::string::npos;
}

/// Pitch-class names for reporting a key back. Sharps, because that is what
/// `pitchName` uses and the two should not disagree.
const std::vector<std::string>& noteNames() {
    static const std::vector<std::string> names = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return names;
}

/// "C", "F#", "Bb" → a pitch class. Empty when it is not a note name.
int pitchClassFromName(const std::string& name, bool& ok) {
    static const std::string letters = "CDEFGAB";
    static const int offsets[] = {0, 2, 4, 5, 7, 9, 11};
    ok = false;
    if (name.empty()) return 0;
    const auto letter = letters.find(char(std::toupper(name[0])));
    if (letter == std::string::npos) return 0;
    int value = offsets[letter];
    for (std::size_t i = 1; i < name.size(); ++i) {
        if (name[i] == '#') ++value;
        else if (name[i] == 'b' || name[i] == 'B') --value;
        else return 0;
    }
    ok = true;
    return ((value % 12) + 12) % 12;
}

miditools::ChordParams::Type chordTypeFromId(const std::string& id, bool& ok) {
    using T = miditools::ChordParams::Type;
    static const std::pair<const char*, T> table[] = {
        {"major", T::Major},         {"minor", T::Minor},
        {"diminished", T::Diminished}, {"augmented", T::Augmented},
        {"major7", T::Major7},       {"minor7", T::Minor7},
        {"dominant7", T::Dominant7}, {"minor7b5", T::Minor7b5},
        {"diminished7", T::Diminished7}, {"sus2", T::Sus2},
        {"sus4", T::Sus4},           {"add9", T::Add9},
        {"major9", T::Major9},       {"minor9", T::Minor9},
        {"power", T::Power},
    };
    for (const auto& [name, type] : table)
        if (id == name) { ok = true; return type; }
    ok = false;
    return T::Major;
}

} // namespace

// ── Units, public ───────────────────────────────────────────────────────────

double beatsPerBar(const EngineController& c) {
    const int num = std::max(1, c.timeSigNumerator());
    const int den = std::max(1, c.timeSigDenominator());
    return num * 4.0 / den;
}

double barsToSeconds(const EngineController& c, double bars) {
    return beatsToSeconds(bars * beatsPerBar(c), c.tempo());
}

double secondsToBars(const EngineController& c, double seconds) {
    const double perBar = beatsPerBar(c);
    return perBar > 0.0 ? secondsToBeats(seconds, c.tempo()) / perBar : 0.0;
}

json ToolResult::toJson() const {
    if (ok) {
        json out = value.is_null() ? json::object() : value;
        if (!out.is_object()) out = json{{"result", value}};
        out["ok"] = true;
        return out;
    }
    return json{{"ok", false}, {"error", error}};
}

json projectSnapshot(const EngineController& c, const ToolContext& ctx) {
    json tracks = json::array();
    for (const TrackModel& t : c.project().tracks) tracks.push_back(trackJson(c, t));

    json master{{"id", EngineController::kMasterChannelId},
                {"volumeDb", round2(gainToDb(c.masterVolume()))}};
    json inserts = json::array();
    for (const InsertModel& slot : c.project().masterInserts)
        if (slot.isLoaded()) inserts.push_back(insertJson(slot));
    if (!inserts.empty()) master["inserts"] = std::move(inserts);

    // Where the user is, in the units the tools take.
    json here{{"playheadBar", round2(secondsToBars(c, c.positionSeconds()) + 1.0)}};
    if (c.isLoopEnabled())
        here["loop"] = json{
            {"fromBar", round2(secondsToBars(c, c.loopStartSeconds()) + 1.0)},
            {"toBar", round2(secondsToBars(c, c.loopEndSeconds()) + 1.0)}};
    if (!ctx.focus.trackId.empty()) {
        here["selectedTrackId"] = ctx.focus.trackId;
        if (const TrackModel* t = c.project().findTrack(ctx.focus.trackId))
            here["selectedTrackName"] = t->name;
    }
    if (!ctx.focus.clipId.empty()) here["selectedClipId"] = ctx.focus.clipId;

    return json{{"name", c.projectName()},
                {"focus", std::move(here)},
                {"tempo", round2(c.tempo())},
                {"key", noteNames()[c.keyRoot()] + " " +
                            miditools::scaleName(
                                miditools::scaleFromId(c.projectScale()))},
                {"timeSignature", std::to_string(c.timeSigNumerator()) + "/" +
                                      std::to_string(c.timeSigDenominator())},
                {"master", std::move(master)},
                {"tracks", std::move(tracks)}};
}

// ── The registry ────────────────────────────────────────────────────────────

namespace {

/// Built with a loop rather than a json initializer-list: a braced list of
/// pairs is ambiguous with nlohmann's array construction, and this reads the
/// same at the call sites.
json obj(std::initializer_list<std::pair<const char*, json>> fields) {
    json out = json::object();
    for (const auto& [key, value] : fields) out[key] = value;
    return out;
}

json prop(const char* type, const char* description) {
    return json{{"type", type}, {"description", description}};
}

json schema(json properties, std::vector<std::string> required) {
    return json{{"type", "object"},
                {"properties", std::move(properties)},
                {"required", std::move(required)}};
}

std::vector<ToolSpec> buildSpecs() {
    std::vector<ToolSpec> specs;
    auto add = [&specs](const char* name, const char* description, json s) {
        specs.push_back({name, description, std::move(s)});
    };

    // ── Reading ──
    add("get_project",
        "The whole project: tempo, time signature, and every track with its "
        "id, kind, level, instrument, inserts, sends and clips. The ids in the "
        "reply are what every other tool takes.",
        schema(json::object(), {}));

    add("get_clip_notes",
        "Every note in a MIDI clip, as pitch, start and length in beats from "
        "the clip's start. Read this before changing notes you did not write.",
        schema(obj({{"trackId", prop("string", "id of the track")},
                    {"clipId", prop("string", "id of the clip")}}),
               {"trackId", "clipId"}));

    add("list_plugins",
        "The plugins installed on this machine. Returns format and uid for "
        "each; those two together name a plugin for set_track_instrument and "
        "add_insert. Never invent a uid — call this first.",
        schema(obj({{"kind",
                     json{{"type", "string"},
                          {"enum", json::array({"instrument", "effect", "all"})},
                          {"description",
                           "instrument for something that makes sound from "
                           "MIDI, effect for something that processes audio"}}},
                    {"nameContains",
                     prop("string",
                          "optional case-insensitive filter on the name")}}),
               {"kind"}));

    add("search_files",
        "Find audio files in the folders the user added to the browser. "
        "Nothing outside those folders is searched. Use this to go and get a "
        "sound rather than asking the user for one — a query matches part of a "
        "file name, or an extension like \".wav\".",
        schema(obj({{"query",
                     prop("string",
                          "part of a file name, or an extension such as "
                          "\".wav\". Empty lists what is there.")},
                    {"limit",
                     prop("integer", "how many to return at most; default 40")}}),
               {"query"}));

    add("list_plugin_parameters",
        "The parameters of one loaded plugin, with each parameter's id, "
        "range, unit and current value. set_insert_parameter takes those ids, "
        "and they cannot be guessed — read them here first.",
        schema(obj({{"channelId",
                     prop("string", "a track id, or \"master\"")},
                    {"insertId",
                     prop("string",
                          "id of the insert slot, or of the track's "
                          "instrument slot")}}),
               {"channelId", "insertId"}));

    add("analyze_track",
        "Render a channel on its own and measure it: level, headroom, whether "
        "it clips, and how its energy splits across low, mid and high. Do this "
        "BEFORE deciding a level or an EQ move — you cannot hear the project, "
        "and these numbers are the only thing standing between a real mix "
        "decision and a guess. Measure the master too, to see where the mix "
        "stands as a whole.",
        schema(obj({{"channelId",
                     prop("string", "a track id, or \"master\" for the mix")},
                    {"fromBar", prop("number", "start of the range; default 1")},
                    {"toBar",
                     prop("number",
                          "end of the range; omit to measure to the end of "
                          "the project")}}),
               {"channelId"}));

    add("analyze_sample",
        "Decode an audio file and describe it: how long, how loud, how fast it "
        "attacks, whether it holds a pitch, and a guess at what it is. Use it "
        "when a file's name does not tell you enough — you cannot listen, and "
        "this is the closest thing to it.",
        schema(obj({{"filePath", prop("string", "absolute path to an audio file")}}),
               {"filePath"}));

    // ── Project ──
    add("set_tempo", "Set the project tempo in BPM (20–300).",
        schema(obj({{"bpm", prop("number", "beats per minute")}}), {"bpm"}));

    add("set_project_key",
        "Record the key the project is in. Nothing is forced into it, but "
        "transform_notes with snap_to_scale uses it, and stating a key keeps "
        "everything you write afterwards consistent. Set it once when you "
        "choose a key, and tell the user which one.",
        schema(obj({{"root",
                     prop("string",
                          "the tonic: C, C#, Db, D, Eb, E, F, F#, G, Ab, A, "
                          "Bb or B")},
                    {"scale",
                     prop("string",
                          "major, natural_minor, harmonic_minor, "
                          "melodic_minor, dorian, phrygian, lydian, "
                          "mixolydian, locrian, pentatonic_major, "
                          "pentatonic_minor, blues, whole_tone or chromatic")}}),
               {"root", "scale"}));

    add("transform_notes",
        "Reshape the notes already in a clip. This is how a part stops "
        "sounding typed in: write the notes, then quantize and humanize them. "
        "Prefer these over recomputing a note list by hand — they are exact, "
        "and they are what the program's own editing menu uses.",
        schema(obj({{"trackId", prop("string", "id of the track")},
                    {"clipId", prop("string", "id of the clip")},
                    {"operation",
                     json{{"type", "string"},
                          {"enum", json::array({"quantize", "humanize",
                                                "legato", "articulate",
                                                "strum", "transpose",
                                                "snap_to_scale", "limit_pitch",
                                                "invert_pitch", "reverse_time",
                                                "ramp_velocity",
                                                "scale_velocity",
                                                "scale_length", "nudge",
                                                "rotate", "build_chords"})},
                          {"description",
                           "quantize pulls starts to a grid; humanize adds "
                           "small timing and velocity spread; legato runs "
                           "each note into the next; articulate shortens and "
                           "accents; strum spreads a chord in time; "
                           "snap_to_scale pulls every note into the project's "
                           "key; build_chords stacks a chord on every note"}}},
                    {"amount",
                     prop("number",
                          "how much, 0 to 1, for quantize, humanize, "
                          "articulate and snap. Defaults to 1 (all the way). "
                          "0.6 to 0.8 keeps a part's feel.")},
                    {"grid",
                     prop("integer",
                          "for quantize: the note value, 4 = quarter, 8 = "
                          "eighth, 16 = sixteenth. Defaults to 16.")},
                    {"triplet", prop("boolean", "for quantize: use a triplet grid")},
                    {"semitones", prop("integer", "for transpose")},
                    {"low", prop("integer", "for limit_pitch: lowest MIDI note")},
                    {"high", prop("integer", "for limit_pitch: highest MIDI note")},
                    {"from", prop("integer", "for ramp_velocity: first note's velocity")},
                    {"to", prop("integer", "for ramp_velocity: last note's velocity")},
                    {"factor",
                     prop("number", "for scale_velocity and scale_length")},
                    {"beats",
                     prop("number", "for nudge, rotate and strum: how far, in beats")},
                    {"chord",
                     prop("string",
                          "for build_chords: major, minor, diminished, "
                          "augmented, major7, minor7, dominant7, sus2, sus4, "
                          "add9, major9, minor9 or power")}}),
               {"trackId", "clipId", "operation"}));

    add("set_time_signature", "Set the project time signature.",
        schema(obj({{"numerator", prop("integer", "beats per bar, 1–32")},
                    {"denominator",
                     prop("integer", "note value: 1, 2, 4, 8, 16 or 32")}}),
               {"numerator", "denominator"}));

    // ── Tracks and mix ──
    add("add_track",
        "Create a track and return its id. An instrument track plays MIDI "
        "through a plugin; an audio track holds recorded or imported audio; a "
        "bus is a destination for sends.",
        schema(obj({{"kind",
                     json{{"type", "string"},
                          {"enum", json::array({"instrument", "audio", "midi",
                                                "bus", "aux", "group",
                                                "folder"})},
                          {"description", "what the track is for"}}},
                    {"name", prop("string", "what to call it")}}),
               {"kind", "name"}));

    add("remove_track",
        "Delete a track and everything on it. Only when the user asked for it.",
        schema(obj({{"trackId", prop("string", "id of the track")}}),
               {"trackId"}));

    add("rename_track", "Change a track's name.",
        schema(obj({{"trackId", prop("string", "id of the track")},
                    {"name", prop("string", "the new name")}}),
               {"trackId", "name"}));

    add("set_track_mix",
        "Set any of a track's mixer controls. Every field is optional — send "
        "only what you want to change.",
        schema(obj({{"trackId", prop("string", "id of the track")},
                    {"volumeDb",
                     prop("number",
                          "fader level in dB; 0 is unity, -6 is half as loud, "
                          "+6 is the maximum")},
                    {"pan", prop("number", "-1 hard left, 0 centre, 1 hard right")},
                    {"muted", prop("boolean", "silence the track")},
                    {"soloed", prop("boolean", "silence everything else")}}),
               {"trackId"}));

    add("add_send",
        "Route a copy of a track's signal to a bus, for a shared reverb or "
        "delay. Create the bus with add_track first.",
        schema(obj({{"trackId", prop("string", "the track that sends")},
                    {"destinationTrackId", prop("string", "the bus receiving it")},
                    {"level", prop("number", "how much, 0 to 1")},
                    {"preFader",
                     prop("boolean",
                          "take the signal before the fader; defaults to "
                          "false")}}),
               {"trackId", "destinationTrackId", "level"}));

    // ── Instruments ──
    add("set_track_instrument",
        "Put a plugin instrument in a track's instrument slot, so its MIDI "
        "clips make sound. Use format and uid from list_plugins.",
        schema(obj({{"trackId", prop("string", "an instrument or MIDI track")},
                    {"format", prop("string", "clap, vst3, au or internal")},
                    {"uid", prop("string", "the plugin's uid from list_plugins")}}),
               {"trackId", "format", "uid"}));

    add("load_sampler",
        "Put the built-in sampler on a track and load an audio file into it, "
        "in one step. The fastest way to make a drum or a one-shot playable "
        "from MIDI. Use a path the user attached or one you were given.",
        schema(obj({{"trackId", prop("string", "an instrument or MIDI track")},
                    {"filePath", prop("string", "absolute path to an audio file")}}),
               {"trackId", "filePath"}));

    // ── Clips and notes ──
    add("add_midi_clip",
        "Create an empty MIDI clip on a track and return its id. Write the "
        "notes into it with set_clip_notes.",
        schema(obj({{"trackId", prop("string", "an instrument or MIDI track")},
                    {"startBar", prop("number", "bar it starts on; bar 1 is the "
                                                "start of the project")},
                    {"lengthBars", prop("number", "how many bars long")}}),
               {"trackId", "startBar", "lengthBars"}));

    add("set_clip_notes",
        "Write notes into a MIDI clip. This is the main tool for composing: "
        "send a whole part in one call rather than a note at a time.",
        schema(obj(
                   {{"trackId", prop("string", "id of the track")},
                    {"clipId", prop("string", "id of the clip")},
                    {"mode",
                     json{{"type", "string"},
                          {"enum", json::array({"replace", "append"})},
                          {"description",
                           "replace discards the clip's existing notes; append "
                           "keeps them. Defaults to replace."}}},
                    {"notes",
                     json{{"type", "array"},
                          {"description", "the notes to write"},
                          {"items",
                           schema(obj({{"pitch",
                                        prop("integer",
                                             "MIDI note number, 0-127. 60 is "
                                             "middle C, shown as C5 in this "
                                             "program")},
                                       {"start",
                                        prop("number",
                                             "beats from the start of the "
                                             "clip; 0 is the clip's first beat")},
                                       {"length", prop("number", "length in beats")},
                                       {"velocity",
                                        prop("integer",
                                             "how hard, 1-127; defaults to "
                                             "100. Vary it — a part where "
                                             "every note is the same velocity "
                                             "sounds mechanical")}}),
                                  {"pitch", "start", "length"})}}}}),
               {"trackId", "clipId", "notes"}));

    add("set_clip_start", "Move a clip to another bar.",
        schema(obj({{"trackId", prop("string", "id of the track")},
                    {"clipId", prop("string", "id of the clip")},
                    {"startBar", prop("number", "the bar to move it to")}}),
               {"trackId", "clipId", "startBar"}));

    add("duplicate_clip",
        "Copy a clip and place the copy straight after the original. The way "
        "to repeat a bar or a section.",
        schema(obj({{"trackId", prop("string", "id of the track")},
                    {"clipId", prop("string", "id of the clip")}}),
               {"trackId", "clipId"}));

    add("remove_clip", "Delete a clip. Only when the user asked for it.",
        schema(obj({{"trackId", prop("string", "id of the track")},
                    {"clipId", prop("string", "id of the clip")}}),
               {"trackId", "clipId"}));

    // ── Effects ──
    add("add_insert",
        "Add an effect plugin to a channel's insert chain and return the new "
        "slot's id. Order matters: inserts process in the order they are "
        "added, so EQ before compression before reverb is the usual shape.",
        schema(obj({{"channelId", prop("string", "a track id, or \"master\"")},
                    {"format", prop("string", "clap, vst3, au or internal")},
                    {"uid", prop("string", "the plugin's uid from list_plugins")},
                    {"index",
                     prop("integer",
                          "position in the chain; omit to append at the end")}}),
               {"channelId", "format", "uid"}));

    add("remove_insert", "Remove an effect from a channel's chain.",
        schema(obj({{"channelId", prop("string", "a track id, or \"master\"")},
                    {"insertId", prop("string", "id of the insert slot")}}),
               {"channelId", "insertId"}));

    add("set_insert_bypassed", "Switch an effect off without removing it.",
        schema(obj({{"channelId", prop("string", "a track id, or \"master\"")},
                    {"insertId", prop("string", "id of the insert slot")},
                    {"bypassed", prop("boolean", "true to bypass")}}),
               {"channelId", "insertId", "bypassed"}));

    add("set_insert_parameter",
        "Set one parameter of a loaded plugin, in the parameter's own units. "
        "Call list_plugin_parameters first: the ids and the ranges differ for "
        "every plugin and cannot be guessed.",
        schema(obj({{"channelId", prop("string", "a track id, or \"master\"")},
                    {"insertId",
                     prop("string", "id of the insert or instrument slot")},
                    {"parameterId",
                     prop("string", "id from list_plugin_parameters")},
                    {"value",
                     prop("number",
                          "the new value, between the parameter's min and max")}}),
               {"channelId", "insertId", "parameterId", "value"}));

    return specs;
}

} // namespace

const std::vector<ToolSpec>& toolSpecs() {
    static const std::vector<ToolSpec> specs = buildSpecs();
    return specs;
}

// ── Dispatch ────────────────────────────────────────────────────────────────

ToolResult callTool(EngineController& c, const std::string& name,
                    const json& rawArgs, const ToolContext& ctx) {
    // A model occasionally sends the arguments as a JSON *string*, or sends
    // nothing at all for a no-argument tool. Both are cheap to accept.
    json args = json::object();
    if (rawArgs.is_object()) {
        args = rawArgs;
    } else if (rawArgs.is_string()) {
        args = json::parse(rawArgs.get<std::string>(), nullptr, false);
        if (args.is_discarded() || !args.is_object())
            return fail("arguments were not a JSON object");
    } else if (!rawArgs.is_null()) {
        return fail("arguments must be a JSON object");
    }

    std::string err;

    // ── Reading ──
    if (name == "get_project") return done(projectSnapshot(c, ctx));

    if (name == "get_clip_notes") {
        std::string trackId, clipId;
        wantString(args, "trackId", trackId, err);
        wantString(args, "clipId", clipId, err);
        if (!err.empty()) return fail(err);
        if (!c.project().findTrack(trackId)) return fail(unknownTrack(c, trackId));
        const ClipModel* clip = findClip(c, trackId, clipId);
        if (!clip) return fail("no clip with id '" + clipId + "' on that track");

        json notes = json::array();
        for (const NoteModel& n : clip->notes)
            notes.push_back(json{{"pitch", n.pitch},
                                 {"name", miditools::pitchName(n.pitch)},
                                 {"start", round2(n.startBeats)},
                                 {"length", round2(n.lengthBeats)},
                                 {"velocity", n.velocity}});
        return done(json{{"clipId", clipId},
                         {"lengthBeats",
                          round2(secondsToBeats(clip->durationSeconds, c.tempo()))},
                         {"notes", std::move(notes)}});
    }

    if (name == "list_plugins") {
        std::string kind, filter;
        wantString(args, "kind", kind, err);
        optString(args, "nameContains", filter, err);
        if (!err.empty()) return fail(err);
        if (kind != "instrument" && kind != "effect" && kind != "all")
            return fail("'kind' must be instrument, effect or all");

        std::vector<plugins::PluginDescriptor> found =
            kind == "instrument" ? c.pluginManager().instruments()
            : kind == "effect"   ? c.pluginManager().effects()
                                 : c.pluginManager().plugins();

        std::string lowered = filter;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char ch) { return char(std::tolower(ch)); });

        json list = json::array();
        for (const plugins::PluginDescriptor& d : found) {
            if (!lowered.empty()) {
                std::string lowerName = d.name;
                std::transform(
                    lowerName.begin(), lowerName.end(), lowerName.begin(),
                    [](unsigned char ch) { return char(std::tolower(ch)); });
                if (lowerName.find(lowered) == std::string::npos) continue;
            }
            list.push_back(json{{"format", toString(d.format)},
                                {"uid", d.uid},
                                {"name", d.name},
                                {"vendor", d.vendor},
                                {"isInstrument", d.isInstrument}});
        }
        json out{{"plugins", std::move(list)}};
        if (out["plugins"].empty())
            out["note"] =
                "nothing is installed for that filter. The built-in sampler "
                "(format \"internal\", uid \"daw.sampler\") is always "
                "available, and load_sampler puts it on a track with a file "
                "in one step.";
        return done(std::move(out));
    }

    if (name == "search_files") {
        std::string query;
        double limit = 40.0;
        wantString(args, "query", query, err);
        optNumber(args, "limit", limit, err);
        if (!err.empty()) return fail(err);
        if (ctx.sampleFolders.empty())
            return fail("the user has not added any folders to the browser, so "
                        "there is nowhere to search. Ask them to add one, or "
                        "use an installed instrument plugin instead.");

        const std::size_t want =
            std::size_t(std::clamp(limit, 1.0, double(kSearchLimit)));
        json found = json::array();
        bool truncated = false;
        std::size_t visited = 0;

        for (const std::string& root : ctx.sampleFolders) {
            if (found.size() >= want) { truncated = true; break; }
            std::error_code ec;
            fs::recursive_directory_iterator it(
                platform::pathFromUtf8(root),
                fs::directory_options::skip_permission_denied, ec);
            if (ec) continue;
            const fs::recursive_directory_iterator end;
            for (; it != end; it.increment(ec)) {
                if (ec) break;
                // A sample library can be enormous, and a search that walks all
                // of it would stall the request for minutes.
                if (++visited > kSearchVisitLimit) { truncated = true; break; }
                if (it.depth() > kSearchDepth) { it.disable_recursion_pending(); continue; }
                if (!it->is_regular_file(ec)) continue;

                const fs::path path = it->path();
                // `isDecodableExtension` wants a bare lower-case suffix, not
                // the ".wav" that `extension()` hands back.
                std::string ext = platform::pathToUtf8(path.extension());
                if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char ch) { return char(std::tolower(ch)); });
                if (!audio::platform::isDecodableExtension(ext)) continue;
                const std::string filename =
                    platform::pathToUtf8(path.filename());
                const std::string utf8Path = platform::pathToUtf8(path);
                if (!matchesQuery(filename, query)) continue;

                audio::platform::AudioFileInfo probed;
                json entry{{"name", filename}, {"path", utf8Path}};
                if (audio::platform::probeAudioFile(utf8Path, probed).isOk()) {
                    entry["seconds"] = round2(probed.durationSeconds());
                    entry["sampleRate"] = int(probed.sampleRate);
                    entry["channels"] = int(probed.channels);
                }
                found.push_back(std::move(entry));
                if (found.size() >= want) { truncated = true; break; }
            }
        }

        json out{{"files", std::move(found)}};
        if (truncated) out["truncated"] = true;
        if (out["files"].empty())
            out["note"] = "nothing matched. Try a shorter query, or an "
                          "extension like \".wav\" to see what is there.";
        return done(std::move(out));
    }

    if (name == "list_plugin_parameters") {
        std::string channelId, insertId;
        wantString(args, "channelId", channelId, err);
        wantString(args, "insertId", insertId, err);
        if (!err.empty()) return fail(err);
        if (!channelExists(c, channelId))
            return fail(unknownTrack(c, channelId));

        const std::vector<plugins::ParameterInfo> params =
            c.insertParameters(channelId, insertId);
        if (params.empty())
            return fail("no loaded plugin with slot id '" + insertId +
                        "' on that channel, or it has no parameters. Slot ids "
                        "come from get_project.");

        json list = json::array();
        for (const plugins::ParameterInfo& p : params)
            list.push_back(
                json{{"id", p.id},
                     {"name", p.name},
                     {"min", round2(p.minValue)},
                     {"max", round2(p.maxValue)},
                     {"unit", p.unit},
                     {"current", round2(c.insertParameter(channelId, insertId, p.id))}});
        return done(json{{"parameters", std::move(list)}});
    }

    if (name == "analyze_track") {
        std::string channelId;
        double fromBar = 1.0, toBar = 0.0;
        wantString(args, "channelId", channelId, err);
        optNumber(args, "fromBar", fromBar, err);
        optNumber(args, "toBar", toBar, err);
        if (!err.empty()) return fail(err);
        if (!channelExists(c, channelId)) return fail(unknownTrack(c, channelId));

        analysis::Metrics metrics;
        const audio::Result result = c.analyzeChannel(
            channelId, barsToSeconds(c, std::max(0.0, fromBar - 1.0)),
            toBar > 0.0 ? barsToSeconds(c, toBar - 1.0) : 0.0, metrics);
        if (!result.isOk())
            return fail(std::string("could not measure that channel: ") +
                        result.message());
        if (metrics.silent)
            return done(json{{"silent", true},
                             {"note",
                              "that channel makes no sound over the range — "
                              "check it has clips, an instrument, and is not "
                              "muted"}});

        json out{{"peakDb", round2(metrics.peakDb)},
                 {"rmsDb", round2(metrics.rmsDb)},
                 {"headroomDb", round2(-metrics.peakDb)},
                 {"lowShare", round2(metrics.lowFraction)},
                 {"midShare", round2(metrics.midFraction)},
                 {"highShare", round2(metrics.highFraction)},
                 {"seconds", round2(metrics.seconds)}};
        if (metrics.clipped > 0) {
            out["clippedSamples"] = metrics.clipped;
            out["warning"] =
                "this channel goes past full scale and is being damaged — "
                "bring its level down, or the levels feeding it";
        }
        return done(std::move(out));
    }

    if (name == "analyze_sample") {
        std::string filePath;
        wantString(args, "filePath", filePath, err);
        if (!err.empty()) return fail(err);

        analysis::SampleTraits traits;
        const audio::Result result = c.analyzeSampleFile(filePath, traits);
        if (!result.isOk())
            return fail("could not read '" + filePath +
                        "': " + result.message());

        json out{{"seconds", round2(traits.level.seconds)},
                 {"peakDb", round2(traits.level.peakDb)},
                 {"character", traits.character},
                 {"attackSeconds", round2(traits.attackSeconds)},
                 {"lowShare", round2(traits.level.lowFraction)},
                 {"midShare", round2(traits.level.midFraction)},
                 {"highShare", round2(traits.level.highFraction)}};
        if (traits.tonal) {
            out["pitch"] = traits.pitch;
            out["pitchName"] = miditools::pitchName(traits.pitch);
            out["note"] =
                "it holds a pitch, so set the sampler's root note to " +
                std::to_string(traits.pitch) +
                " if you want it to play in tune from the keyboard";
        }
        return done(std::move(out));
    }

    // ── Project ──
    if (name == "set_tempo") {
        double bpm = 0.0;
        wantNumber(args, "bpm", bpm, err);
        if (!err.empty()) return fail(err);
        if (bpm < 20.0 || bpm > 300.0)
            return fail("tempo must be between 20 and 300 BPM");
        c.setTempo(bpm);
        return done(json{{"tempo", round2(c.tempo())}});
    }

    if (name == "set_project_key") {
        std::string root, scale;
        wantString(args, "root", root, err);
        wantString(args, "scale", scale, err);
        if (!err.empty()) return fail(err);

        bool ok = false;
        const int pitchClass = pitchClassFromName(root, ok);
        if (!ok)
            return fail("'" + root +
                        "' is not a note name. Use C, C#, Db, D, Eb, E, F, "
                        "F#, G, Ab, A, Bb or B.");
        const miditools::Scale parsed = miditools::scaleFromId(scale);
        if (miditools::scaleId(parsed) != scale && scale != "minor")
            return fail("'" + scale +
                        "' is not a scale. Use major, natural_minor, "
                        "harmonic_minor, melodic_minor, dorian, phrygian, "
                        "lydian, mixolydian, locrian, pentatonic_major, "
                        "pentatonic_minor, blues, whole_tone or chromatic.");
        c.setProjectKey(pitchClass, miditools::scaleId(parsed));
        return done(json{{"key", root + " " + miditools::scaleName(parsed)}});
    }

    if (name == "transform_notes") {
        std::string trackId, clipId, operation;
        wantString(args, "trackId", trackId, err);
        wantString(args, "clipId", clipId, err);
        wantString(args, "operation", operation, err);
        if (!err.empty()) return fail(err);
        const ClipModel* clip = findClip(c, trackId, clipId);
        if (!clip) return fail("no clip with id '" + clipId + "' on that track");
        if (clip->kind != ClipKind::Midi)
            return fail("clip '" + clip->name + "' holds audio, not notes");
        if (clip->notes.empty())
            return fail("that clip has no notes yet — write them with "
                        "set_clip_notes first");

        double amount = 1.0, factor = 1.0, beats = 0.25;
        double grid = 16.0, semitones = 0.0, low = 0.0, high = 127.0;
        double from = 40.0, to = 110.0;
        bool triplet = false;
        std::string chord = "major";
        optNumber(args, "amount", amount, err);
        optNumber(args, "factor", factor, err);
        optNumber(args, "beats", beats, err);
        optNumber(args, "grid", grid, err);
        optNumber(args, "semitones", semitones, err);
        optNumber(args, "low", low, err);
        optNumber(args, "high", high, err);
        optNumber(args, "from", from, err);
        optNumber(args, "to", to, err);
        optBool(args, "triplet", triplet, err);
        optString(args, "chord", chord, err);
        if (!err.empty()) return fail(err);
        amount = std::clamp(amount, 0.0, 1.0);

        const miditools::Scale scale = miditools::scaleFromId(c.projectScale());
        const double clipBeats = secondsToBeats(clip->durationSeconds, c.tempo());
        miditools::Notes notes = clip->notes;

        if (operation == "quantize") {
            miditools::QuantizeParams params;
            params.gridBeats = miditools::gridBeatsFor(
                int(grid), triplet ? miditools::GridFlavour::Triplet
                                   : miditools::GridFlavour::Straight);
            params.strength = amount;
            notes = miditools::quantize(std::move(notes), params);
        } else if (operation == "humanize") {
            // Scaled off `amount` so one number covers it: at 1 this is a
            // 32nd of timing spread and ±14 of velocity, which is about as far
            // as "played rather than typed" goes before it sounds sloppy.
            notes = miditools::humanize(std::move(notes), 0.03 * amount,
                                        int(14 * amount), 1);
        } else if (operation == "legato") {
            notes = miditools::legato(std::move(notes), clip->notes);
        } else if (operation == "articulate") {
            miditools::ArticulateParams params;
            params.amount = amount;
            params.accentEvery = 4;
            notes = miditools::articulate(std::move(notes), params);
        } else if (operation == "strum") {
            miditools::StrumParams params;
            params.spanBeats = beats > 0.0 ? beats : 0.125;
            notes = miditools::strum(std::move(notes), params);
        } else if (operation == "transpose") {
            notes = miditools::transpose(std::move(notes), int(semitones));
        } else if (operation == "snap_to_scale") {
            notes = miditools::snapToScale(std::move(notes), c.keyRoot(), scale);
        } else if (operation == "limit_pitch") {
            if (low >= high) return fail("'low' must be below 'high'");
            notes = miditools::limitPitch(std::move(notes), int(low), int(high));
        } else if (operation == "invert_pitch") {
            notes = miditools::invertPitch(std::move(notes));
        } else if (operation == "reverse_time") {
            notes = miditools::reverseTime(std::move(notes));
        } else if (operation == "ramp_velocity") {
            notes = miditools::rampVelocity(std::move(notes), int(from), int(to));
        } else if (operation == "scale_velocity") {
            notes = miditools::scaleVelocity(std::move(notes), factor);
        } else if (operation == "scale_length") {
            notes = miditools::scaleLength(std::move(notes), factor);
        } else if (operation == "nudge") {
            notes = miditools::nudge(std::move(notes), beats);
        } else if (operation == "rotate") {
            notes = miditools::rotate(std::move(notes), beats, clipBeats);
        } else if (operation == "build_chords") {
            bool ok = false;
            miditools::ChordParams params;
            params.type = chordTypeFromId(chord, ok);
            if (!ok)
                return fail("'" + chord +
                            "' is not a chord type. Use major, minor, "
                            "diminished, augmented, major7, minor7, "
                            "dominant7, sus2, sus4, add9, major9, minor9 or "
                            "power.");
            notes = miditools::buildChords(notes, params);
        } else {
            return fail("'" + operation + "' is not a transform");
        }

        const std::size_t count = notes.size();
        c.setClipNotes(trackId, clipId, std::move(notes),
                       "AI: " + operation);
        return done(json{{"operation", operation}, {"noteCount", count}});
    }

    if (name == "set_time_signature") {
        double num = 0.0, den = 0.0;
        wantNumber(args, "numerator", num, err);
        wantNumber(args, "denominator", den, err);
        if (!err.empty()) return fail(err);
        const int n = int(num), d = int(den);
        if (n < 1 || n > 32) return fail("numerator must be between 1 and 32");
        if (d != 1 && d != 2 && d != 4 && d != 8 && d != 16 && d != 32)
            return fail("denominator must be 1, 2, 4, 8, 16 or 32");
        c.setTimeSignature(n, d);
        return done(json{{"timeSignature",
                          std::to_string(n) + "/" + std::to_string(d)}});
    }

    // ── Tracks ──
    if (name == "add_track") {
        std::string kind, trackName;
        wantString(args, "kind", kind, err);
        wantString(args, "name", trackName, err);
        if (!err.empty()) return fail(err);
        if (kind == "master")
            return fail("the master channel already exists; use channelId "
                        "\"master\" to work on it");
        const TrackKind parsed = trackKindFromString(kind);
        // `trackKindFromString` falls back to Audio, so an unknown word would
        // silently make the wrong kind of track — check the round trip.
        if (toString(parsed) != kind)
            return fail("unknown track kind '" + kind +
                        "'. Use instrument, audio, midi, bus, aux, group or "
                        "folder.");
        const std::string id = c.addTrack(parsed, trackName);
        if (id.empty()) return fail("the track could not be created");
        return done(json{{"trackId", id}, {"name", trackName}, {"kind", kind}});
    }

    if (name == "remove_track") {
        std::string trackId;
        wantString(args, "trackId", trackId, err);
        if (!err.empty()) return fail(err);
        const TrackModel* track = c.project().findTrack(trackId);
        if (!track) return fail(unknownTrack(c, trackId));
        if (!allowed(ctx, "delete the track \"" + track->name + "\" and " +
                              std::to_string(track->clips.size()) +
                              " clip(s) on it"))
            return fail(kRefused);
        c.removeTrack(trackId);
        return done();
    }

    if (name == "rename_track") {
        std::string trackId, trackName;
        wantString(args, "trackId", trackId, err);
        wantString(args, "name", trackName, err);
        if (!err.empty()) return fail(err);
        if (!c.project().findTrack(trackId)) return fail(unknownTrack(c, trackId));
        c.renameTrack(trackId, trackName);
        return done(json{{"name", trackName}});
    }

    if (name == "set_track_mix") {
        std::string trackId;
        wantString(args, "trackId", trackId, err);
        if (!err.empty()) return fail(err);
        const TrackModel* track = c.project().findTrack(trackId);
        if (!track) return fail(unknownTrack(c, trackId));

        json applied = json::object();
        double db = 0.0;
        if (optNumber(args, "volumeDb", db, err)) {
            if (db > 6.0) return fail("volumeDb cannot exceed +6");
            c.setTrackVolume(trackId, float(dbToGain(db)));
            applied["volumeDb"] = round2(db);
        }
        double pan = 0.0;
        if (optNumber(args, "pan", pan, err)) {
            if (pan < -1.0 || pan > 1.0)
                return fail("pan must be between -1 and 1");
            c.setTrackPan(trackId, float(pan));
            applied["pan"] = round2(pan);
        }
        bool flag = false;
        if (optBool(args, "muted", flag, err)) {
            c.setTrackMuted(trackId, flag);
            applied["muted"] = flag;
        }
        if (optBool(args, "soloed", flag, err)) {
            c.setTrackSoloed(trackId, flag);
            applied["soloed"] = flag;
        }
        if (!err.empty()) return fail(err);
        if (applied.empty())
            return fail("nothing to change: send at least one of volumeDb, "
                        "pan, muted or soloed");
        return done(std::move(applied));
    }

    if (name == "add_send") {
        std::string trackId, destId;
        double level = 0.0;
        bool preFader = false;
        wantString(args, "trackId", trackId, err);
        wantString(args, "destinationTrackId", destId, err);
        wantNumber(args, "level", level, err);
        optBool(args, "preFader", preFader, err);
        if (!err.empty()) return fail(err);
        if (!c.project().findTrack(trackId)) return fail(unknownTrack(c, trackId));
        if (!c.project().findTrack(destId)) return fail(unknownTrack(c, destId));
        if (level < 0.0 || level > 1.0)
            return fail("level must be between 0 and 1");

        const std::string sendId = c.addSend(trackId, destId);
        if (sendId.empty())
            return fail("that send would feed a signal back into itself");
        c.setSendLevel(trackId, sendId, float(level));
        if (preFader) c.setSendPreFader(trackId, sendId, true);
        return done(json{{"sendId", sendId}});
    }

    // ── Instruments ──
    if (name == "set_track_instrument") {
        std::string trackId, format, uid;
        wantString(args, "trackId", trackId, err);
        wantString(args, "format", format, err);
        wantString(args, "uid", uid, err);
        if (!err.empty()) return fail(err);
        const TrackModel* track = c.project().findTrack(trackId);
        if (!track) return fail(unknownTrack(c, trackId));
        if (!trackAccepts(track->kind, ClipKind::Midi))
            return fail("track '" + track->name + "' is " +
                        toString(track->kind) +
                        " and cannot hold an instrument. Make an instrument "
                        "track with add_track.");

        const auto descriptor =
            findPlugin(c, format, uid, err);
        if (!descriptor) return fail(err);
        c.setTrackInstrumentPlugin(trackId, *descriptor);

        const TrackModel* after = c.project().findTrack(trackId);
        if (!after || !after->instrument.isLoaded())
            return fail("'" + descriptor->name +
                        "' failed to load — it may be broken or blacklisted");
        return done(json{{"insertId", after->instrument.id},
                         {"name", after->instrument.name}});
    }

    if (name == "load_sampler") {
        std::string trackId, filePath;
        wantString(args, "trackId", trackId, err);
        wantString(args, "filePath", filePath, err);
        if (!err.empty()) return fail(err);
        const TrackModel* track = c.project().findTrack(trackId);
        if (!track) return fail(unknownTrack(c, trackId));
        if (!trackAccepts(track->kind, ClipKind::Midi))
            return fail("track '" + track->name +
                        "' cannot hold an instrument. Make an instrument track "
                        "with add_track.");
        if (!c.loadInstrumentSampler(trackId, filePath))
            return fail("could not load '" + filePath +
                        "' — check the path, and that it is an audio file this "
                        "program can decode");

        const TrackModel* after = c.project().findTrack(trackId);
        return done(json{{"insertId", after ? after->instrument.id : ""}});
    }

    // ── Clips ──
    if (name == "add_midi_clip") {
        std::string trackId;
        double startBar = 1.0, lengthBars = 0.0;
        wantString(args, "trackId", trackId, err);
        wantNumber(args, "startBar", startBar, err);
        wantNumber(args, "lengthBars", lengthBars, err);
        if (!err.empty()) return fail(err);
        const TrackModel* track = c.project().findTrack(trackId);
        if (!track) return fail(unknownTrack(c, trackId));
        if (!trackAccepts(track->kind, ClipKind::Midi))
            return fail("track '" + track->name + "' is " +
                        toString(track->kind) +
                        " and cannot hold MIDI clips");
        if (startBar < 1.0) return fail("startBar starts at 1, not 0");
        if (lengthBars <= 0.0) return fail("lengthBars must be greater than 0");

        const std::string clipId =
            c.addMidiClip(trackId, barsToSeconds(c, startBar - 1.0),
                          barsToSeconds(c, lengthBars));
        if (clipId.empty()) return fail("the clip could not be created");
        return done(json{{"clipId", clipId},
                         {"lengthBeats", round2(lengthBars * beatsPerBar(c))}});
    }

    if (name == "set_clip_notes") {
        std::string trackId, clipId, mode = "replace";
        wantString(args, "trackId", trackId, err);
        wantString(args, "clipId", clipId, err);
        optString(args, "mode", mode, err);
        if (!err.empty()) return fail(err);
        if (mode != "replace" && mode != "append")
            return fail("'mode' must be replace or append");
        if (!c.project().findTrack(trackId)) return fail(unknownTrack(c, trackId));
        const ClipModel* clip = findClip(c, trackId, clipId);
        if (!clip) return fail("no clip with id '" + clipId + "' on that track");
        if (clip->kind != ClipKind::Midi)
            return fail("clip '" + clip->name + "' holds audio, not notes");

        json noteList;
        wantArray(args, "notes", noteList, err);
        if (!err.empty()) return fail(err + " of notes");

        const double clipBeats =
            secondsToBeats(clip->durationSeconds, c.tempo());
        std::vector<NoteModel> notes;
        if (mode == "append") notes = clip->notes;

        double lastEnd = 0.0;
        for (const json& raw : noteList) {
            if (!raw.is_object()) return fail("every note must be an object");
            std::string noteErr;
            double pitch = 0.0, start = 0.0, length = 0.0, velocity = 100.0;
            wantNumber(raw, "pitch", pitch, noteErr);
            wantNumber(raw, "start", start, noteErr);
            wantNumber(raw, "length", length, noteErr);
            optNumber(raw, "velocity", velocity, noteErr);
            if (!noteErr.empty()) return fail("in one of the notes: " + noteErr);
            if (pitch < 0.0 || pitch > 127.0)
                return fail("pitch must be between 0 and 127");
            if (start < 0.0) return fail("a note cannot start before the clip");
            if (length <= 0.0) return fail("a note's length must be above 0");

            NoteModel note;
            note.pitch = int(pitch);
            note.startBeats = start;
            note.lengthBeats = length;
            note.velocity = std::clamp(int(velocity), 1, 127);
            lastEnd = std::max(lastEnd, start + length);
            notes.push_back(note);
        }

        // A part longer than the clip it was written into is the commonest way
        // for the assistant's work to vanish — notes at or past the end never
        // sound. The notes are the intent, so the clip grows to hold them,
        // rounded up to a whole bar and never shrunk. Same reasoning as
        // `importMidiFile`, and it rides the same single undo entry.
        double grewToBars = 0.0;
        if (lastEnd > clipBeats) {
            const double perBar = beatsPerBar(c);
            const double bars = std::ceil(lastEnd / perBar);
            grewToBars = std::max(1.0, bars);
            c.setClipTrim(trackId, clipId, clip->startSeconds, clip->offsetSeconds,
                          barsToSeconds(c, grewToBars));
        }

        const std::size_t count = notes.size();
        c.setClipNotes(trackId, clipId, std::move(notes), "AI: Write Notes");

        json out{{"noteCount", count},
                 {"clipLengthBeats",
                  round2(grewToBars > 0.0 ? grewToBars * beatsPerBar(c) : clipBeats)}};
        if (grewToBars > 0.0)
            out["note"] = "the clip was lengthened to " +
                          std::to_string(int(grewToBars)) +
                          " bar(s) so the whole part sounds";
        return done(std::move(out));
    }

    if (name == "set_clip_start") {
        std::string trackId, clipId;
        double startBar = 1.0;
        wantString(args, "trackId", trackId, err);
        wantString(args, "clipId", clipId, err);
        wantNumber(args, "startBar", startBar, err);
        if (!err.empty()) return fail(err);
        if (!findClip(c, trackId, clipId))
            return fail("no clip with id '" + clipId + "' on track '" + trackId + "'");
        if (startBar < 1.0) return fail("startBar starts at 1, not 0");
        c.setClipStartSeconds(trackId, clipId, barsToSeconds(c, startBar - 1.0));
        return done(json{{"startBar", round2(startBar)}});
    }

    if (name == "duplicate_clip") {
        std::string trackId, clipId;
        wantString(args, "trackId", trackId, err);
        wantString(args, "clipId", clipId, err);
        if (!err.empty()) return fail(err);
        if (!findClip(c, trackId, clipId))
            return fail("no clip with id '" + clipId + "' on track '" + trackId + "'");
        const std::string copy = c.duplicateClip(trackId, clipId);
        if (copy.empty()) return fail("the clip could not be duplicated");
        const ClipModel* made = findClip(c, trackId, copy);
        return done(json{{"clipId", copy},
                         {"startBar",
                          made ? round2(secondsToBars(c, made->startSeconds) + 1.0)
                               : 0.0}});
    }

    if (name == "remove_clip") {
        std::string trackId, clipId;
        wantString(args, "trackId", trackId, err);
        wantString(args, "clipId", clipId, err);
        if (!err.empty()) return fail(err);
        const ClipModel* clip = findClip(c, trackId, clipId);
        if (!clip)
            return fail("no clip with id '" + clipId + "' on track '" + trackId + "'");
        if (!allowed(ctx, "delete the clip \"" + clip->name + "\""))
            return fail(kRefused);
        c.removeClip(trackId, clipId);
        return done();
    }

    // ── Effects ──
    if (name == "add_insert") {
        std::string channelId, format, uid;
        double index = -1.0;
        wantString(args, "channelId", channelId, err);
        wantString(args, "format", format, err);
        wantString(args, "uid", uid, err);
        optNumber(args, "index", index, err);
        if (!err.empty()) return fail(err);
        if (!channelExists(c, channelId)) return fail(unknownTrack(c, channelId));

        const auto descriptor =
            findPlugin(c, format, uid, err);
        if (!descriptor) return fail(err);
        if (descriptor->isInstrument)
            return fail("'" + descriptor->name +
                        "' is an instrument, not an effect — put it in the "
                        "track's instrument slot with set_track_instrument");

        const std::size_t at =
            index >= 0.0 ? std::size_t(index) : std::size_t(-1);
        const std::string insertId = c.addInsert(channelId, *descriptor, at);
        if (insertId.empty())
            return fail("'" + descriptor->name +
                        "' failed to load — it may be broken or blacklisted");
        return done(json{{"insertId", insertId}, {"name", descriptor->name}});
    }

    if (name == "remove_insert") {
        std::string channelId, insertId;
        wantString(args, "channelId", channelId, err);
        wantString(args, "insertId", insertId, err);
        if (!err.empty()) return fail(err);
        if (!channelExists(c, channelId)) return fail(unknownTrack(c, channelId));
        if (!allowed(ctx, "remove an effect from that channel"))
            return fail(kRefused);
        c.removeInsert(channelId, insertId);
        return done();
    }

    if (name == "set_insert_bypassed") {
        std::string channelId, insertId;
        bool bypassed = false;
        wantString(args, "channelId", channelId, err);
        wantString(args, "insertId", insertId, err);
        if (!args.contains("bypassed")) return fail("missing required argument 'bypassed'");
        optBool(args, "bypassed", bypassed, err);
        if (!err.empty()) return fail(err);
        if (!channelExists(c, channelId)) return fail(unknownTrack(c, channelId));
        c.setInsertBypassed(channelId, insertId, bypassed);
        return done(json{{"bypassed", bypassed}});
    }

    if (name == "set_insert_parameter") {
        std::string channelId, insertId, parameterId;
        double value = 0.0;
        wantString(args, "channelId", channelId, err);
        wantString(args, "insertId", insertId, err);
        wantString(args, "parameterId", parameterId, err);
        wantNumber(args, "value", value, err);
        if (!err.empty()) return fail(err);
        if (!channelExists(c, channelId)) return fail(unknownTrack(c, channelId));

        const std::vector<plugins::ParameterInfo> params =
            c.insertParameters(channelId, insertId);
        if (params.empty())
            return fail("no loaded plugin with slot id '" + insertId +
                        "' on that channel");

        const auto found = std::find_if(
            params.begin(), params.end(),
            [&](const plugins::ParameterInfo& p) { return p.id == parameterId; });
        if (found == params.end()) {
            // Name the alternatives: this is the error the model is most likely
            // to hit, and the one it can most usefully correct itself from.
            std::string known;
            for (const plugins::ParameterInfo& p : params) {
                if (known.size() > 800) { known += ", …"; break; }
                if (!known.empty()) known += ", ";
                known += p.id;
            }
            return fail("'" + parameterId +
                        "' is not a parameter of that plugin. Its parameters "
                        "are: " + known);
        }
        if (value < found->minValue || value > found->maxValue)
            return fail("'" + parameterId + "' takes " +
                        std::to_string(found->minValue) + " to " +
                        std::to_string(found->maxValue));

        const double before = c.insertParameter(channelId, insertId, parameterId);
        c.setInsertParameter(channelId, insertId, parameterId, value);
        // The live setter is deliberately not undoable — it is what a knob drag
        // calls — so the edit is committed here, or the assistant's changes
        // would not come back with Ctrl+Z.
        c.commitInsertParameterEdit(channelId, insertId, parameterId, before,
                                    "AI: " + found->name);
        return done(json{{"parameterId", parameterId}, {"value", value}});
    }

    return fail("there is no tool called '" + name + "'");
}

// ── The system prompt ───────────────────────────────────────────────────────

std::string systemPrompt(const EngineController& c, const ToolContext& ctx) {
    // Kept as one literal on purpose: this text is the feature's real
    // behaviour, and it should be readable and editable as prose rather than
    // assembled from fragments scattered through the code.
    static constexpr const char* kRole = R"(You are a music producer working inside a digital audio workstation. You are not advising the user — you are operating the program for them, through the tools you have been given.

When the user asks for something, make it. Create the tracks, load the instruments, write the notes, set the levels, add the effects. Then say in one or two short sentences what you did and what you chose (the key, the tempo, the sound), so they can judge it. Never describe steps you did not take, and never explain how the user could do it themselves.

UNITS AND CONVENTIONS
- Clip positions are in BARS. Bar 1 is the start of the project.
- Note positions are in BEATS FROM THE START OF THEIR CLIP. A note at start 0 is on the clip's first beat.
- Pitch is a MIDI note number. 60 is middle C. NOTE: this program labels 60 as "C5", one octave higher than most software, so when you name notes to the user, say C5 for 60 — that is what they see in the piano roll.
- Velocity is 1-127. Pan is -1 (left) to 1 (right). Track level is in dB, 0 = unity, +6 = maximum.

HOW TO WORK
1. The current project is given below, including a "focus" block saying what the user has selected and where the playhead is. Trust it; you do not need to call get_project first unless you have made changes and need the new ids.
2. To make a part: add_track (instrument) → set_track_instrument or load_sampler → add_midi_clip → set_clip_notes. Do not write notes before the clip exists.
3. Call list_plugins before naming any plugin, and use its uid exactly. Uids cannot be guessed from a name. If nothing suitable is installed, use the built-in sampler with load_sampler.
4. Call list_plugin_parameters before set_insert_parameter. Parameter ids and ranges differ for every plugin.
5. Write a whole part in ONE set_clip_notes call. Never one note per call.
6. After writing a part, shape it with transform_notes rather than by hand: quantize at 0.7-0.9 to tighten without flattening, then humanize at 0.3-0.6, and articulate for phrasing. Doing that in the note list yourself produces worse results and costs more.
7. If you choose a key, record it with set_project_key so everything after stays consistent, and use transform_notes with snap_to_scale if you are unsure a part fits it.

MUSICAL JUDGEMENT
- If the user gives no key, choose one and tell them which. If they give no length, write 4 or 8 bars. If they give no tempo, leave the project's tempo alone.
- Voice chords like a player would: spread them across an octave or more, move the top note as little as possible between changes, and do not stack every triad in root position in the same octave.
- Vary velocity. A part where every note is 100 sounds like a machine. Accent the downbeats, lighten the passing notes.
- Let notes breathe: a quarter note that lasts exactly 1.0 beats is legato. Slightly shorter reads as played.
- When asked to "process" or "mix" a channel, MEASURE IT FIRST with analyze_track, and measure the master too. Then act on what the numbers say: bring a channel down if it eats the headroom, cut lows on something whose low share is high but should not be, and add effects in the usual order — EQ, then dynamics, then reverb or delay via a send to a bus. Measure again afterwards to check you improved it. Guessing at levels you cannot hear is the one thing that makes this feature useless.

RESTRAINT
- Do not delete or overwrite tracks, clips or notes the user already has unless they asked you to. Add to the project instead.
- Do not change the tempo or the time signature unless asked.
- Work in as few tool calls as you can. If a request is unclear enough that you would be guessing at something central — which instrument, how long, what key — make your best choice, do it, and say what you assumed.)";

    std::string out = kRole;

    out += "\n\nCURRENT PROJECT\n";
    out += projectSnapshot(c, ctx).dump(2);
    out +=
        "\n\nThe \"focus\" block is what the user is looking at. When they say "
        "\"this\", \"that track\" or \"the clip\", they mean what is selected "
        "there; when they say \"here\" or \"at this point\", they mean the "
        "playhead's bar, or the loop range if one is set. Work on the focus "
        "unless the request names something else. If nothing is selected and "
        "the request needs a target, say which track you chose.";

    if (!c.aiInstructions().empty()) {
        // Last, and framed as overriding: standing instructions are the user's
        // own rules for this project, and they beat the defaults above.
        out += "\n\nSTANDING INSTRUCTIONS FOR THIS PROJECT\n";
        out += "These come from the user and override anything above that "
               "conflicts with them.\n";
        out += c.aiInstructions();
    }

    out += "\n\nFILES THE USER ATTACHED\n";
    if (ctx.attachments.empty()) {
        out +=
            "None. If a request needs a sample you have not been given, use an "
            "installed instrument plugin instead.";
    } else {
        out +=
            "You cannot listen to these. Judge them by their name, their "
            "length and their channel count, and pass the path to "
            "load_sampler.\n";
        for (const Attachment& a : ctx.attachments) {
            char line[1024];
            std::snprintf(line, sizeof(line),
                          "- %s  (%.2f s, %d Hz, %d ch)\n  path: %s\n",
                          a.name.c_str(), a.seconds, a.sampleRate, a.channels,
                          a.path.c_str());
            out += line;
        }
    }
    return out;
}

} // namespace daw::ai
