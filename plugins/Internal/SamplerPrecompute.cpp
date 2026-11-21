#include "Internal/SamplerPrecompute.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace daw::plugins::sampler {
namespace {

constexpr double kPi = std::numbers::pi;

using Channels = std::vector<std::vector<float>>;

/// RBJ biquad, the standard cookbook forms. One instance per band; the state is
/// per channel so a stereo sample is filtered coherently.
struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0;
    double z2 = 0.0;

    float process(float input) noexcept {
        const double x = input;
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return float(y);
    }

    static Biquad lowShelf(double f0, double dbGain, double sampleRate) noexcept {
        Biquad q;
        const double a = std::pow(10.0, dbGain / 40.0);
        const double w = 2.0 * kPi * f0 / sampleRate;
        const double cs = std::cos(w);
        const double sn = std::sin(w);
        const double alpha = sn / 2.0 * std::sqrt((a + 1.0 / a) * (1.0 / 0.9 - 1.0) + 2.0);
        const double twoSqrtAAlpha = 2.0 * std::sqrt(a) * alpha;
        const double a0 = (a + 1.0) + (a - 1.0) * cs + twoSqrtAAlpha;
        q.b0 = a * ((a + 1.0) - (a - 1.0) * cs + twoSqrtAAlpha) / a0;
        q.b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cs) / a0;
        q.b2 = a * ((a + 1.0) - (a - 1.0) * cs - twoSqrtAAlpha) / a0;
        q.a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cs) / a0;
        q.a2 = ((a + 1.0) + (a - 1.0) * cs - twoSqrtAAlpha) / a0;
        return q;
    }

    static Biquad highShelf(double f0, double dbGain, double sampleRate) noexcept {
        Biquad q;
        const double a = std::pow(10.0, dbGain / 40.0);
        const double w = 2.0 * kPi * f0 / sampleRate;
        const double cs = std::cos(w);
        const double sn = std::sin(w);
        const double alpha = sn / 2.0 * std::sqrt((a + 1.0 / a) * (1.0 / 0.9 - 1.0) + 2.0);
        const double twoSqrtAAlpha = 2.0 * std::sqrt(a) * alpha;
        const double a0 = (a + 1.0) - (a - 1.0) * cs + twoSqrtAAlpha;
        q.b0 = a * ((a + 1.0) + (a - 1.0) * cs + twoSqrtAAlpha) / a0;
        q.b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cs) / a0;
        q.b2 = a * ((a + 1.0) + (a - 1.0) * cs - twoSqrtAAlpha) / a0;
        q.a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cs) / a0;
        q.a2 = ((a + 1.0) - (a - 1.0) * cs - twoSqrtAAlpha) / a0;
        return q;
    }

    static Biquad peaking(double f0, double dbGain, double q0, double sampleRate) noexcept {
        Biquad q;
        const double a = std::pow(10.0, dbGain / 40.0);
        const double w = 2.0 * kPi * f0 / sampleRate;
        const double alpha = std::sin(w) / (2.0 * q0);
        const double a0 = 1.0 + alpha / a;
        q.b0 = (1.0 + alpha * a) / a0;
        q.b1 = -2.0 * std::cos(w) / a0;
        q.b2 = (1.0 - alpha * a) / a0;
        q.a1 = q.b1;
        q.a2 = (1.0 - alpha / a) / a0;
        return q;
    }
};

void applyBiquad(Channels& channels, const Biquad& filter) {
    for (auto& channel : channels) {
        // Every channel filters through its own copy, so it starts from a clean
        // state: one shared state would leak the left channel into the right.
        Biquad running = filter;
        for (float& sample : channel) sample = running.process(sample);
    }
}

/// The one-pole-per-stage lowpass used for the precomputed CUT/RES, kept
/// separate from the voice's filter because this one runs once over the whole
/// sample and has no modulation to follow.
void applyLowpass(Channels& channels, double cut, double res, double sampleRate) {
    const double cutoff = 20.0 * std::pow(1000.0, std::clamp(cut, 0.0, 1.0));
    const double g = std::tan(kPi * std::min(cutoff, sampleRate * 0.49) / sampleRate);
    const double q = 0.5 + std::clamp(res, 0.0, 1.0) * 11.5;
    const double k = std::max(1.0 / q, 0.05);
    const double a1 = 1.0 / (1.0 + g * (g + k));
    const double a2 = g * a1;
    const double a3 = g * a2;

    for (auto& channel : channels) {
        double ic1 = 0.0;
        double ic2 = 0.0;
        for (float& sample : channel) {
            const double v3 = double(sample) - ic2;
            const double v1 = a1 * ic1 + a2 * v3;
            const double v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0 * v1 - ic1;
            ic2 = 2.0 * v2 - ic2;
            sample = float(v2);
        }
    }
}

/// A Schroeder reverb: four parallel combs into two allpasses. Small, and the
/// only thing a baked-in reverb has to be — it runs once and is never heard
/// changing.
void applyReverb(Channels& channels, double amount, int type, double sampleRate,
                 std::size_t tailFrames) {
    const double scale = sampleRate / 44100.0;
    const double feedback = type == 1 ? 0.86 : 0.72;
    const std::array<double, 4> combs{1116.0, 1188.0, 1277.0, 1356.0};
    const std::array<double, 2> allpasses{556.0, 441.0};
    const double stretch = type == 1 ? 1.8 : 1.0;

    for (std::size_t ch = 0; ch < channels.size(); ++ch) {
        std::vector<float>& data = channels[ch];
        data.resize(data.size() + tailFrames, 0.0f);
        std::vector<float> wet(data.size(), 0.0f);

        for (std::size_t c = 0; c < combs.size(); ++c) {
            // A few samples of offset per channel is what keeps the two sides
            // from being the same reverb twice.
            const std::size_t length =
                std::size_t(combs[c] * scale * stretch) + ch * 23;
            if (length == 0 || length >= data.size()) continue;
            std::vector<float> line(length, 0.0f);
            std::size_t cursor = 0;
            for (std::size_t i = 0; i < data.size(); ++i) {
                const float delayed = line[cursor];
                wet[i] += delayed * 0.25f;
                line[cursor] = data[i] + delayed * float(feedback);
                cursor = (cursor + 1) % length;
            }
        }
        for (double allpass : allpasses) {
            const std::size_t length = std::size_t(allpass * scale);
            if (length == 0 || length >= wet.size()) continue;
            std::vector<float> line(length, 0.0f);
            std::size_t cursor = 0;
            for (std::size_t i = 0; i < wet.size(); ++i) {
                const float delayed = line[cursor];
                const float out = delayed - wet[i] * 0.5f;
                line[cursor] = wet[i] + delayed * 0.5f;
                wet[i] = out;
                cursor = (cursor + 1) % length;
            }
        }
        for (std::size_t i = 0; i < data.size(); ++i) {
            data[i] = float(data[i] * (1.0 - amount * 0.5) + wet[i] * amount);
        }
    }
}

/// Pseudo-stereo: the Haas trick. A few milliseconds of delay on one side reads
/// as width rather than as an echo, which is exactly what the FL control does.
void applyStereoDelay(Channels& channels, double amount, double sampleRate) {
    if (channels.size() < 2) return;
    const std::size_t delay = std::size_t(amount * 0.030 * sampleRate);
    if (delay == 0) return;

    const std::vector<float> right = channels[1];
    for (std::size_t i = 0; i < channels[1].size(); ++i) {
        const float delayed = i >= delay ? right[i - delay] : 0.0f;
        channels[1][i] = float(right[i] * (1.0 - amount * 0.5) + delayed * amount * 0.5);
    }
    const std::vector<float> left = channels[0];
    const std::size_t half = delay / 2;
    if (half == 0) return;
    for (std::size_t i = 0; i < channels[0].size(); ++i) {
        const float delayed = i >= half ? left[i - half] : 0.0f;
        channels[0][i] = float(left[i] * (1.0 - amount * 0.25) + delayed * amount * 0.25);
    }
}

/// Pogo: the sample starts detuned and settles to pitch, which is a read rate
/// that decays back to one. Positive drops the pitch, negative raises it.
void applyPogo(Channels& channels, double amount, double sampleRate) {
    if (channels.empty() || channels[0].empty()) return;
    const double tau = 0.25 * sampleRate;
    const std::size_t sourceFrames = channels[0].size();
    // The rate never reaches zero, so the output is bounded — but a big drop
    // stretches the start a long way, and four times the source is as far as
    // this is allowed to grow.
    const std::size_t limit = sourceFrames * 4;

    Channels out(channels.size());
    for (auto& channel : out) channel.reserve(sourceFrames);

    double position = 0.0;
    std::size_t written = 0;
    while (position < double(sourceFrames - 1) && written < limit) {
        const double rate = std::pow(2.0, -amount * 2.0 * std::exp(-double(written) / tau));
        for (std::size_t ch = 0; ch < channels.size(); ++ch) {
            const std::size_t index = std::size_t(position);
            const double fraction = position - double(index);
            const float a = channels[ch][index];
            const float b = index + 1 < sourceFrames ? channels[ch][index + 1] : a;
            out[ch].push_back(float(a + (b - a) * fraction));
        }
        position += rate;
        ++written;
    }
    channels = std::move(out);
}

void applyNormalize(Channels& channels) {
    float peak = 0.0f;
    for (const auto& channel : channels) {
        for (float sample : channel) peak = std::max(peak, std::abs(sample));
    }
    if (peak <= 0.0f || peak == 1.0f) return;
    const float gain = 1.0f / peak;
    for (auto& channel : channels) {
        for (float& sample : channel) sample *= gain;
    }
}

/// Sweep the sample from the left channel to the right across its length.
void applyFadeStereo(Channels& channels) {
    if (channels.size() < 2 || channels[0].empty()) return;
    const double last = double(channels[0].size() - 1);
    for (std::size_t i = 0; i < channels[0].size(); ++i) {
        const double t = last > 0.0 ? double(i) / last : 0.0;
        const double angle = t * kPi * 0.5;
        channels[0][i] = float(channels[0][i] * std::cos(angle));
        channels[1][i] = float(channels[1][i] * std::sin(angle));
    }
}

} // namespace

bool PrecomputeSettings::isNeutral() const noexcept {
    return boost == 0.0 && eqLow == 0.0 && eqMid == 0.0 && eqHigh == 0.0 &&
           ringMix == 0.0 && cut >= 1.0 && res == 0.0 && reverb == 0.0 &&
           stereoDelay == 0.0 && pogo == 0.0 && !removeDc && !reversePolarity &&
           !normalize && !fadeStereo && !reverse && !swapStereo;
}

std::shared_ptr<const engine::SampleBuffer> precompute(
    const engine::SampleBuffer& raw, const PrecomputeSettings& settings,
    engine::FrameCount& baseFrames) {
    const engine::ChannelCount rawChannels = std::max<engine::ChannelCount>(raw.channels(), 1);
    const engine::FrameCount rawFrames = raw.frames();
    baseFrames = rawFrames;
    if (rawFrames == 0) return nullptr;

    const double sampleRate = raw.sampleRate() > 0.0 ? raw.sampleRate() : 44100.0;

    // Two of these controls only mean something in stereo, and promoting a mono
    // sample is the only way to honour them — which is also what makes "pseudo
    // stereo" pseudo.
    const bool wantsStereo = settings.stereoDelay > 0.0 || settings.fadeStereo;
    const engine::ChannelCount channelCount =
        wantsStereo ? std::max<engine::ChannelCount>(rawChannels, 2) : rawChannels;

    Channels channels(channelCount);
    for (engine::ChannelCount ch = 0; ch < channelCount; ++ch) {
        const float* source = raw.channel(std::min<engine::ChannelCount>(ch, rawChannels - 1));
        channels[ch].assign(source, source + rawFrames);
    }

    // ── Order matters, and follows the signal: shape the sample itself first,
    // then colour it, then place it in space, then set its level. ──
    if (settings.removeDc) {
        for (auto& channel : channels) {
            double sum = 0.0;
            for (float sample : channel) sum += sample;
            const float mean = float(sum / double(channel.size()));
            for (float& sample : channel) sample -= mean;
        }
    }
    if (settings.reversePolarity) {
        for (auto& channel : channels) {
            for (float& sample : channel) sample = -sample;
        }
    }
    if (settings.swapStereo && channels.size() >= 2) std::swap(channels[0], channels[1]);
    if (settings.reverse) {
        for (auto& channel : channels) std::reverse(channel.begin(), channel.end());
    }
    if (settings.pogo != 0.0) applyPogo(channels, settings.pogo, sampleRate);

    if (settings.boost > 0.0) {
        const float gain = float(1.0 + settings.boost * 9.0);
        for (auto& channel : channels) {
            for (float& sample : channel) {
                sample = std::clamp(sample * gain, -1.0f, 1.0f);
            }
        }
    }
    if (settings.eqLow != 0.0) {
        applyBiquad(channels, Biquad::lowShelf(200.0, settings.eqLow * 12.0, sampleRate));
    }
    if (settings.eqMid != 0.0) {
        applyBiquad(channels, Biquad::peaking(1500.0, settings.eqMid * 12.0, 0.8, sampleRate));
    }
    if (settings.eqHigh != 0.0) {
        applyBiquad(channels, Biquad::highShelf(6000.0, settings.eqHigh * 12.0, sampleRate));
    }
    if (settings.ringMix > 0.0) {
        const double frequency = 20.0 * std::pow(400.0, std::clamp(settings.ringFreq, 0.0, 1.0));
        for (auto& channel : channels) {
            for (std::size_t i = 0; i < channel.size(); ++i) {
                const double phase = 2.0 * kPi * frequency * double(i) / sampleRate;
                const double modulated = channel[i] * std::sin(phase);
                channel[i] = float(channel[i] * (1.0 - settings.ringMix) +
                                   modulated * settings.ringMix);
            }
        }
    }
    if (settings.cut < 1.0 || settings.res > 0.0) {
        applyLowpass(channels, settings.cut, settings.res, sampleRate);
    }

    // Everything above changes the *length* of the material; the reverb is the
    // first thing that adds to it, so the base length is fixed here.
    baseFrames = engine::FrameCount(channels[0].size());

    if (settings.reverb > 0.0) {
        const double tailSeconds = settings.reverbType == 1 ? 2.5 : 0.8;
        applyReverb(channels, settings.reverb, settings.reverbType, sampleRate,
                    std::size_t(tailSeconds * sampleRate));
    }
    if (settings.stereoDelay > 0.0) applyStereoDelay(channels, settings.stereoDelay, sampleRate);
    if (settings.fadeStereo) applyFadeStereo(channels);
    if (settings.normalize) applyNormalize(channels);

    const engine::FrameCount frames = engine::FrameCount(channels[0].size());
    if (frames == 0) return nullptr;
    auto out = std::make_shared<engine::SampleBuffer>(channelCount, frames, sampleRate);
    for (engine::ChannelCount ch = 0; ch < channelCount; ++ch) {
        float* destination = out->writableChannel(ch);
        const std::vector<float>& source = channels[ch];
        std::copy_n(source.data(), std::min<std::size_t>(frames, source.size()), destination);
    }
    baseFrames = std::min(baseFrames, frames);
    return out;
}

} // namespace daw::plugins::sampler
