#include "Internal/SamplerInstance.hpp"

#include "Internal/SampleDecoder.hpp"
#include "Internal/SamplerPrecompute.hpp"
#include "platform/PathUtils.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <unordered_map>

namespace daw::plugins::sampler {
namespace {

using json = nlohmann::json;

constexpr std::string_view kUid = "daw.sampler";
constexpr int kStateVersion = 1;

/// id → index, built once. `parameterIndexForId` is called per parameter on
/// every project load and by every automation lane, so a linear scan of 136
/// strings is not the right shape.
const std::unordered_map<std::string_view, std::int32_t>& indexById() {
    static const std::unordered_map<std::string_view, std::int32_t> map = [] {
        std::unordered_map<std::string_view, std::int32_t> built;
        const std::span<const ParameterInfo> table = parameterTable();
        built.reserve(table.size());
        for (const ParameterInfo& info : table) {
            built.emplace(std::string_view(info.id), std::int32_t(info.index));
        }
        return built;
    }();
    return map;
}

double clampToRange(std::uint32_t index, double value) {
    const ParameterInfo& info = parameterTable()[index];
    const double clamped = std::clamp(value, info.minValue, info.maxValue);
    return info.isStepped ? std::round(clamped) : clamped;
}

} // namespace

SamplerInstance::SamplerInstance() {
    m_descriptor = staticDescriptor();
    const std::span<const ParameterInfo> table = parameterTable();
    for (std::uint32_t i = 0; i < kParameterCount; ++i) {
        m_values[i].store(table[i].defaultValue, std::memory_order_relaxed);
    }
}

SamplerInstance::~SamplerInstance() = default;

const PluginDescriptor& SamplerInstance::staticDescriptor() noexcept {
    static const PluginDescriptor descriptor = [] {
        PluginDescriptor d;
        d.format = Format::Internal;
        d.uid = std::string(kUid);
        // No file behind it. The path is set to the uid rather than left empty
        // so anything that keys on a path — the cache, a project written by an
        // older build — still has something stable to hold.
        d.path = std::string(kUid);
        d.name = "Sampler";
        d.vendor = "VLT Studio Pro";
        d.version = "1.0";
        d.category = "Instrument";
        d.isInstrument = true;
        d.hasEditor = false;
        d.wantsMidi = true;
        d.mainInputChannels = 0;
        d.mainOutputChannels = 2;
        return d;
    }();
    return descriptor;
}

std::string_view SamplerInstance::uid() noexcept { return kUid; }

// ── Configuration ──────────────────────────────────────────────────────────

bool SamplerInstance::setBusLayout(const PluginBusLayout&, PluginBusLayout& accepted) {
    accepted = busLayout();
    return true;
}

PluginBusLayout SamplerInstance::busLayout() const {
    PluginBusLayout layout;
    layout.outputs.push_back(2);
    return layout;
}

bool SamplerInstance::activate(const PluginProcessInfo& info) {
    m_sampleRate = info.sampleRate > 0.0 ? info.sampleRate : 48000.0;
    m_maxBlockSize = info.maxBlockSize;
    m_active = true;
    reset();
    return true;
}

void SamplerInstance::deactivate() {
    m_active = false;
    m_processing = false;
    reset();
}

void SamplerInstance::stopProcessing() {
    m_processing = false;
    // Every format may clear its tails here, and a sampler that kept its voices
    // would resume a note the transport has already left behind.
    for (Voice& voice : m_voices) voice.kill();
}

// ── Parameters ─────────────────────────────────────────────────────────────

std::span<const ParameterInfo> SamplerInstance::parameters() const noexcept {
    return parameterTable();
}

std::int32_t SamplerInstance::parameterIndexForId(std::string_view id) const noexcept {
    const auto& map = indexById();
    const auto found = map.find(id);
    return found == map.end() ? -1 : found->second;
}

double SamplerInstance::parameterValue(std::uint32_t index) const noexcept {
    if (index >= kParameterCount) return 0.0;
    return m_values[index].load(std::memory_order_relaxed);
}

std::string SamplerInstance::parameterText(std::uint32_t index, double plainValue) const {
    return sampler::parameterText(index, plainValue);
}

void SamplerInstance::setParameter(std::uint32_t index, double plainValue) {
    if (index >= kParameterCount) return;
    const double value = clampToRange(index, plainValue);
    m_values[index].store(value, std::memory_order_relaxed);
    if (isPrecomputed(index) || Param(index) == Param::KeepOnDisk) {
        m_precomputeDirty.store(true, std::memory_order_release);
    }
}

void SamplerInstance::setParameterFromHost(std::uint32_t index, double plainValue) {
    setParameter(index, plainValue);
}

void SamplerInstance::pumpMainThread() {
    if (m_precomputeDirty.exchange(false, std::memory_order_acq_rel)) {
        rebuildProcessedSample();
    }
}

// ── The sample ─────────────────────────────────────────────────────────────

bool SamplerInstance::loadSample(const std::string& path) {
    std::shared_ptr<const engine::SampleBuffer> decoded = decodeSample(path);
    if (!decoded || decoded->frames() == 0) return false;

    m_raw = std::move(decoded);
    m_samplePath = path;
    m_sampleName = platform::pathToUtf8(
        platform::pathFromUtf8(path).filename());
    rebuildProcessedSample();
    return true;
}

void SamplerInstance::clearSample() {
    m_raw.reset();
    m_samplePath.clear();
    m_sampleName.clear();
    for (Voice& voice : m_voices) voice.kill();
    m_sample.publish({});
}

void SamplerInstance::rebuildProcessedSample() {
    if (!m_raw) {
        m_sample.publish({});
        return;
    }

    auto data = std::make_shared<SampleData>();
    data->path = m_samplePath;
    data->name = m_sampleName;

    const bool keepOnDisk =
        m_values[std::uint32_t(Param::KeepOnDisk)].load(std::memory_order_relaxed) >= 0.5;

    PrecomputeSettings settings;
    const auto value = [&](Param parameter) {
        return m_values[std::uint32_t(parameter)].load(std::memory_order_relaxed);
    };
    settings.boost = value(Param::PreBoost);
    settings.eqLow = value(Param::PreEqLow);
    settings.eqMid = value(Param::PreEqMid);
    settings.eqHigh = value(Param::PreEqHigh);
    settings.ringMix = value(Param::PreRingMix);
    settings.ringFreq = value(Param::PreRingFreq);
    settings.cut = value(Param::PreCut);
    settings.res = value(Param::PreRes);
    settings.reverbType = int(std::lround(value(Param::PreReverbType)));
    settings.reverb = value(Param::PreReverb);
    settings.stereoDelay = value(Param::PreStereoDelay);
    settings.pogo = value(Param::PrePogo);
    settings.removeDc = value(Param::PreRemoveDc) >= 0.5;
    settings.reversePolarity = value(Param::PreReversePolarity) >= 0.5;
    settings.normalize = value(Param::PreNormalize) >= 0.5;
    settings.fadeStereo = value(Param::PreFadeStereo) >= 0.5;
    settings.reverse = value(Param::PreReverse) >= 0.5;
    settings.swapStereo = value(Param::PreSwapStereo) >= 0.5;

    // "Keep on disk" is exactly the documented trade: no baked copy, so no
    // precomputed effects. Nothing else about playback changes.
    if (keepOnDisk || settings.isNeutral()) {
        data->audio = m_raw;
        data->baseFrames = m_raw->frames();
    } else {
        engine::FrameCount baseFrames = m_raw->frames();
        std::shared_ptr<const engine::SampleBuffer> baked =
            precompute(*m_raw, settings, baseFrames);
        if (baked) {
            data->audio = std::move(baked);
            data->baseFrames = baseFrames;
        } else {
            data->audio = m_raw;
            data->baseFrames = m_raw->frames();
        }
    }

    m_sample.publish(std::shared_ptr<const SampleData>(std::move(data)));
}

std::shared_ptr<const SampleData> SamplerInstance::sample() const {
    return m_sample.controlCopy();
}

std::shared_ptr<const engine::SampleBuffer> SamplerInstance::rawSample() const {
    return m_raw;
}

std::string SamplerInstance::samplePath() const { return m_samplePath; }
std::string SamplerInstance::sampleName() const { return m_sampleName; }

// ── State ──────────────────────────────────────────────────────────────────

bool SamplerInstance::saveState(std::vector<std::uint8_t>& out) const {
    return saveProjectState(out, m_samplePath);
}

bool SamplerInstance::saveProjectState(
    std::vector<std::uint8_t>& out,
    const std::string& packagedSampleName) const {
    json document;
    document["version"] = kStateVersion;
    document["sample"] = packagedSampleName;
    json values = json::object();
    const std::span<const ParameterInfo> table = parameterTable();
    for (const ParameterInfo& info : table) {
        values[info.id] = m_values[info.index].load(std::memory_order_relaxed);
    }
    document["params"] = std::move(values);

    const std::string text = document.dump();
    out.assign(text.begin(), text.end());
    return true;
}

bool SamplerInstance::loadState(std::span<const std::uint8_t> state) {
    return loadProjectState(state, {});
}

bool SamplerInstance::loadProjectState(
    std::span<const std::uint8_t> state,
    const std::string& contentDirectory) {
    if (state.empty()) return false;
    json document = json::parse(state.begin(), state.end(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) return false;

    if (document.contains("params") && document["params"].is_object()) {
        for (const auto& [id, value] : document["params"].items()) {
            if (!value.is_number()) continue;
            const std::int32_t index = parameterIndexForId(id);
            // An unknown id is a parameter this build does not have — a project
            // from a newer version. Ignoring it is the whole compatibility
            // story, and beats refusing the state wholesale.
            if (index < 0) continue;
            m_values[std::uint32_t(index)].store(
                clampToRange(std::uint32_t(index), value.get<double>()),
                std::memory_order_relaxed);
        }
    }

    std::string path = document.value("sample", std::string{});
    if (!path.empty() && !contentDirectory.empty() &&
        platform::pathFromUtf8(path).is_relative()) {
        // Project state written by this build contains a basename. Taking the
        // filename again also keeps a malformed/hand-edited state from walking
        // outside Content with ../ segments.
        path = platform::pathToUtf8(
            platform::pathFromUtf8(contentDirectory) /
            platform::pathFromUtf8(path).filename());
    }
    if (path.empty()) {
        clearSample();
    } else if (path != m_samplePath || !m_raw) {
        // A missing file must not wipe the reference: the user moved the media,
        // and the panel showing the path is what lets them find it again.
        if (!loadSample(path)) {
            m_samplePath = path;
            m_sampleName = platform::pathToUtf8(
                platform::pathFromUtf8(path).filename());
            m_raw.reset();
            m_sample.publish({});
        }
    } else {
        rebuildProcessedSample();
    }
    m_precomputeDirty.store(false, std::memory_order_release);
    return true;
}

// ── Audio ──────────────────────────────────────────────────────────────────

SamplerSettings SamplerInstance::snapshot() const noexcept {
    const auto value = [&](Param parameter) {
        return m_values[std::uint32_t(parameter)].load(std::memory_order_relaxed);
    };

    SamplerSettings s;
    s.volume = value(Param::Volume);
    s.pan = value(Param::Pan);
    s.pitchRange = value(Param::PitchRange);
    s.pitchSemitones = value(Param::Pitch) * s.pitchRange;
    s.modX = value(Param::ModX);
    s.modY = value(Param::ModY);
    s.rootNote = int(std::lround(value(Param::RootNote)));

    s.ampEnvOn = value(Param::AmpEnvOn) >= 0.5;
    s.ampEnv.delay = value(Param::AmpDelay);
    s.ampEnv.attack = value(Param::AmpAttack);
    s.ampEnv.hold = value(Param::AmpHold);
    s.ampEnv.decay = value(Param::AmpDecay);
    s.ampEnv.sustain = value(Param::AmpSustain);
    s.ampEnv.release = value(Param::AmpRelease);
    s.ampEnv.attackTension = value(Param::AmpAttackTension);
    s.ampEnv.decayTension = value(Param::AmpDecayTension);
    s.ampEnv.releaseTension = value(Param::AmpReleaseTension);

    s.startOffset = value(Param::StartOffset);
    s.endOffset = value(Param::EndOffset);
    s.fadeIn = value(Param::FadeIn);
    s.fadeOut = value(Param::FadeOut);
    s.loopMode = int(std::lround(value(Param::LoopMode)));
    s.loopStart = value(Param::LoopStart);
    s.loopEnd = value(Param::LoopEnd);
    s.stretchMode = int(std::lround(value(Param::StretchMode)));
    s.stretchTime = value(Param::StretchTime);
    s.stretchPitch = value(Param::StretchPitch);
    s.formant = value(Param::Formant);

    for (std::uint32_t t = 0; t < kModTargetCount; ++t) {
        const auto mod = [&](ModParam parameter) {
            return m_values[indexOf(ModTarget(t), parameter)].load(std::memory_order_relaxed);
        };
        ModSettings& target = s.mod[t];
        target.envOn = mod(ModParam::EnvOn) >= 0.5;
        target.envAmount = mod(ModParam::EnvAmount);
        target.env.delay = mod(ModParam::EnvDelay);
        target.env.attack = mod(ModParam::EnvAttack);
        target.env.hold = mod(ModParam::EnvHold);
        target.env.decay = mod(ModParam::EnvDecay);
        target.env.sustain = mod(ModParam::EnvSustain);
        target.env.release = mod(ModParam::EnvRelease);
        target.env.attackTension = mod(ModParam::EnvAttackTension);
        target.env.decayTension = mod(ModParam::EnvDecayTension);
        target.env.releaseTension = mod(ModParam::EnvReleaseTension);
        target.lfoAmount = mod(ModParam::LfoAmount);
        target.lfoSpeed = mod(ModParam::LfoSpeed);
        target.lfoDelay = mod(ModParam::LfoDelay);
        target.lfoAttack = mod(ModParam::LfoAttack);
        target.lfoTempo = mod(ModParam::LfoTempo) >= 0.5;
        target.lfoGlobal = mod(ModParam::LfoGlobal) >= 0.5;
        target.lfoShape = int(std::lround(mod(ModParam::LfoShape)));
    }
    return s;
}

void SamplerInstance::noteOn(int key, int channel, float velocity) noexcept {
    auto sample = m_sample.read();
    if (!sample) return;

    // FL-style "Cut itself": a fresh trigger owns the sampler immediately.
    // Kill rather than release so an envelope tail cannot overlap the new
    // bass hit. This intentionally applies across keys and MIDI channels of
    // this one Sampler instance, never to another track or instrument.
    const bool cutItself =
        m_values[std::uint32_t(Param::CutItself)].load(std::memory_order_relaxed) >= 0.5;
    if (cutItself) {
        for (Voice& voice : m_voices) voice.kill();
    }

    Voice* chosen = nullptr;
    for (Voice& voice : m_voices) {
        if (!voice.active()) { chosen = &voice; break; }
    }
    if (!chosen) {
        // Steal a released voice before a held one: the note whose key is still
        // down is the one the player expects to keep hearing.
        for (Voice& voice : m_voices) {
            if (!voice.releasing()) continue;
            if (!chosen || voice.startedAt() < chosen->startedAt()) chosen = &voice;
        }
    }
    if (!chosen) {
        chosen = &m_voices[0];
        for (Voice& voice : m_voices) {
            if (voice.startedAt() < chosen->startedAt()) chosen = &voice;
        }
    }

    chosen->start(key, channel, velocity, snapshot(), *sample, m_sampleRate);
    chosen->setStartedAt(++m_voiceStamp);
}

void SamplerInstance::noteOff(int key, int channel) noexcept {
    const bool ampEnvOn =
        m_values[std::uint32_t(Param::AmpEnvOn)].load(std::memory_order_relaxed) >= 0.5;
    const bool looping =
        m_values[std::uint32_t(Param::LoopMode)].load(std::memory_order_relaxed) >= 0.5;
    for (Voice& voice : m_voices) {
        if (!voice.active() || voice.releasing()) continue;
        if (voice.key() != key) continue;
        if (channel >= 0 && voice.channel() != channel) continue;
        voice.release(!ampEnvOn && looping);
    }
}

void SamplerInstance::applyEvent(const PluginEvent& event, std::uint32_t) noexcept {
    switch (event.kind) {
        case PluginEvent::Kind::ParamValue:
            if (event.paramIndex < kParameterCount) {
                const double value = clampToRange(event.paramIndex, event.value);
                m_values[event.paramIndex].store(value, std::memory_order_relaxed);
                // Automation cannot reach a precomputed knob (they are not
                // automatable), but a project load pushes stored values through
                // this same path — so the re-bake is requested here too, and
                // performed on the control thread's next pump.
                if (isPrecomputed(event.paramIndex) ||
                    Param(event.paramIndex) == Param::KeepOnDisk) {
                    m_precomputeDirty.store(true, std::memory_order_release);
                }
            }
            break;
        case PluginEvent::Kind::NoteOn:
            noteOn(int(event.key), int(event.channel), float(event.value));
            break;
        case PluginEvent::Kind::NoteOff:
            noteOff(int(event.key), int(event.channel));
            break;
        case PluginEvent::Kind::NoteChoke:
            for (Voice& voice : m_voices) {
                if (voice.active() && voice.key() == int(event.key)) voice.kill();
            }
            break;
        case PluginEvent::Kind::ParamGestureBegin:
        case PluginEvent::Kind::ParamGestureEnd:
            break;
    }
}

void SamplerInstance::renderSlice(const PluginProcessContext& context,
                                  const SamplerSettings& settings, std::uint32_t offset,
                                  std::uint32_t frames) noexcept {
    if (frames == 0) return;
    auto sample = m_sample.read();
    if (!sample || !sample->audio) return;

    float* slice[engine::kMaxChannels];
    const std::uint16_t channels =
        std::min<std::uint16_t>(context.outputChannels, engine::kMaxChannels);
    for (std::uint16_t ch = 0; ch < channels; ++ch) {
        slice[ch] = context.outputs[ch] + offset;
    }

    const double tempo = context.transport.tempo > 0.0 ? context.transport.tempo : 120.0;
    for (Voice& voice : m_voices) {
        if (!voice.active()) continue;
        voice.render(*sample, settings, slice, channels, frames, m_sampleRate, tempo,
                     m_globalPhase);
    }

    // The free-running phases the Global switch reads. Advanced per slice so a
    // block split by events still moves them exactly once per frame.
    const double seconds = double(frames) / m_sampleRate;
    for (std::uint32_t t = 0; t < kModTargetCount; ++t) {
        const ModSettings& mod = settings.mod[t];
        const double rate = mod.lfoTempo ? mod.lfoSpeed * tempo / 60.0 : mod.lfoSpeed;
        m_globalPhase[t] += rate * seconds;
        if (m_globalPhase[t] > 1e6) m_globalPhase[t] = std::fmod(m_globalPhase[t], 1.0);
    }
}

void SamplerInstance::process(const PluginProcessContext& context) noexcept {
    const std::uint32_t frames = context.frames;
    if (!context.outputs || context.outputChannels == 0) return;

    // The arena hands over recycled buffers, so every channel is written here
    // before a single voice adds into it.
    for (std::uint16_t ch = 0; ch < context.outputChannels; ++ch) {
        std::fill_n(context.outputs[ch], frames, 0.0f);
    }

    SamplerSettings settings = snapshot();
    std::uint32_t cursor = 0;
    for (const PluginEvent& event : context.inputEvents) {
        const std::uint32_t at = std::min(event.frameOffset, frames);
        if (at > cursor) {
            renderSlice(context, settings, cursor, at - cursor);
            cursor = at;
        }
        applyEvent(event, at);
        // A parameter that moved mid-block changes what the rest of it sounds
        // like — including the settings a note started here is given.
        if (event.kind == PluginEvent::Kind::ParamValue) settings = snapshot();
    }
    if (cursor < frames) renderSlice(context, settings, cursor, frames - cursor);
}

void SamplerInstance::reset() noexcept {
    for (Voice& voice : m_voices) voice.kill();
    for (double& phase : m_globalPhase) phase = 0.0;
}

std::uint32_t SamplerInstance::tailSamples() const noexcept {
    // The release is what keeps sounding after the last note-off; the reverb
    // tail is already inside the sample, so it is not counted twice.
    const double release =
        m_values[std::uint32_t(Param::AmpRelease)].load(std::memory_order_relaxed);
    return std::uint32_t(release * m_sampleRate);
}

} // namespace daw::plugins::sampler
