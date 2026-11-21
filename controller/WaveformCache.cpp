#include "WaveformCache.hpp"

#include "platform/AudioFileDecoder.hpp"

#include <algorithm>
#include <cmath>

namespace daw {

const WaveformPeaks* WaveformCache::cached(const std::string& filePath) const {
    auto found = m_cache.find(filePath);
    if (found == m_cache.end()) return nullptr;
    return found->second.isValid() ? &found->second : nullptr;
}

const WaveformPeaks* WaveformCache::storeDecoded(
    const std::string& filePath,
    const audio::platform::DecodedAudio& decoded) {
    if (filePath.empty()) return nullptr;
    WaveformPeaks& entry = m_cache[filePath];
    buildPeaks(decoded, entry);
    return entry.isValid() ? &entry : nullptr;
}

void buildPeaks(const audio::platform::DecodedAudio& decoded, WaveformPeaks& out) {
    out = WaveformPeaks{};
    if (decoded.frames == 0 || decoded.channels == 0 || decoded.sampleRate <= 0.0) {
        return;
    }

    const double duration = double(decoded.frames) / double(decoded.sampleRate);
    constexpr size_t kMaxBucketsPerFile = 4'000'000; // ~32 MB base envelope
    const size_t buckets = std::clamp<size_t>(
        size_t(std::ceil(duration * kPeakBucketsPerSecond)), 1,
        kMaxBucketsPerFile);
    const double framesPerBucket =
        std::max(1.0, double(decoded.frames) / double(buckets));

    out.minima.assign(buckets, 0.0f);
    out.maxima.assign(buckets, 0.0f);
    out.durationSeconds = duration;
    out.bucketsPerSecond = double(buckets) / duration;
    out.channels = int(decoded.channels);

    const auto channels = size_t(decoded.channels);
    const float channelScale = 1.0f / float(channels);

    for (size_t bucket = 0; bucket < buckets; ++bucket) {
        const size_t first = size_t(bucket * framesPerBucket);
        size_t last = size_t((bucket + 1) * framesPerBucket);
        last = std::min<size_t>(last, size_t(decoded.frames));
        if (first >= last) continue;

        float lo = 0.0f;
        float hi = 0.0f;
        bool seeded = false;
        for (size_t frame = first; frame < last; ++frame) {
            float sum = 0.0f;
            const size_t base = frame * channels;
            for (size_t ch = 0; ch < channels; ++ch) {
                sum += decoded.interleaved[base + ch];
            }
            const float value = sum * channelScale;
            if (!seeded) {
                lo = hi = value;
                seeded = true;
            } else {
                lo = std::min(lo, value);
                hi = std::max(hi, value);
            }
        }
        out.minima[bucket] = lo;
        out.maxima[bucket] = hi;
    }

    // A factor of four keeps all coarser levels to about one third of the base
    // envelope's memory while making zoomed-out extrema queries effectively
    // constant time per screen pixel.
    const std::vector<float>* previousMin = &out.minima;
    const std::vector<float>* previousMax = &out.maxima;
    double previousRate = out.bucketsPerSecond;
    while (previousMin->size() > 4) {
        WaveformPeaks::Level level;
        const std::size_t count = (previousMin->size() + 3) / 4;
        level.minima.resize(count);
        level.maxima.resize(count);
        level.bucketsPerSecond = previousRate / 4.0;
        for (std::size_t bucket = 0; bucket < count; ++bucket) {
            const std::size_t first = bucket * 4;
            const std::size_t last = std::min(first + 4, previousMin->size());
            float lo = (*previousMin)[first];
            float hi = (*previousMax)[first];
            for (std::size_t i = first + 1; i < last; ++i) {
                lo = std::min(lo, (*previousMin)[i]);
                hi = std::max(hi, (*previousMax)[i]);
            }
            level.minima[bucket] = lo;
            level.maxima[bucket] = hi;
        }
        out.levels.push_back(std::move(level));
        previousMin = &out.levels.back().minima;
        previousMax = &out.levels.back().maxima;
        previousRate = out.levels.back().bucketsPerSecond;
    }
}

const WaveformPeaks* WaveformCache::peaks(const std::string& filePath) {
    if (filePath.empty()) return nullptr;
    auto found = m_cache.find(filePath);
    if (found != m_cache.end()) {
        return found->second.isValid() ? &found->second : nullptr;
    }

    // Remember failures too: an empty entry stops us re-decoding a file that
    // has gone missing on every repaint.
    m_cache.try_emplace(filePath);

    audio::platform::DecodedAudio decoded;
    if (!audio::platform::decodeAudioFile(filePath, decoded)) return nullptr;
    return storeDecoded(filePath, decoded);
}

} // namespace daw
