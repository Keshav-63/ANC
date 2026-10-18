#include "dsp/chirp_emitter.h"

#include <cmath>
#include <cstring>

namespace anc {

namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;
}

std::vector<float> ChirpEmitter::makeChirpWaveform(const Config& config, double sampleRate) {
    const size_t frames = static_cast<size_t>(config.chirpSeconds * sampleRate);
    std::vector<float> chirp(frames, 0.0f);
    if (frames == 0 || config.endHz <= config.startHz) return chirp;

    const double t0 = config.chirpSeconds;
    const double ratio = config.endHz / config.startHz;
    const double logRatio = std::log(ratio);
    const double phaseScale = kTwoPi * config.startHz * t0 / logRatio;

    for (size_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / sampleRate;
        const double phase = phaseScale * (std::pow(ratio, t / t0) - 1.0);
        // Hann window (zero at both ends -> click free).
        const double window =
            0.5 * (1.0 - std::cos(kTwoPi * static_cast<double>(n) / static_cast<double>(frames - 1)));
        chirp[n] = static_cast<float>(config.amplitude * window * std::sin(phase));
    }
    return chirp;
}

void ChirpEmitter::prepare(double sampleRate, uint32_t /*maxFrames*/) {
    chirp_ = makeChirpWaveform(config_, sampleRate);
    chirpStartFrames_.clear();
    for (int k = 0; k < config_.repeats; ++k) {
        const double startSeconds = config_.initialDelaySeconds +
                                    static_cast<double>(k) * config_.intervalSeconds;
        chirpStartFrames_.push_back(static_cast<uint64_t>(startSeconds * sampleRate));
    }
    sampleIndex_ = 0;
    emitted_ = 0;
}

void ChirpEmitter::process(const float* /*in*/, float* out, uint32_t frames) {
    std::memset(out, 0, frames * sizeof(float));
    if (chirp_.empty()) {
        sampleIndex_ += frames;
        return;
    }
    const uint64_t chirpLength = chirp_.size();
    for (uint32_t i = 0; i < frames; ++i) {
        const uint64_t index = sampleIndex_ + i;

        // Advance past chirps that have fully played out.
        while (nextChirp_ < chirpStartFrames_.size() &&
               index >= chirpStartFrames_[nextChirp_] + chirpLength) {
            ++nextChirp_;
        }
        // Count chirps that have begun (progress reporting).
        while (emitted_ < static_cast<int>(chirpStartFrames_.size()) &&
               index >= chirpStartFrames_[static_cast<size_t>(emitted_)]) {
            ++emitted_;
        }
        if (nextChirp_ < chirpStartFrames_.size()) {
            const uint64_t start = chirpStartFrames_[nextChirp_];
            if (index >= start) out[i] = chirp_[static_cast<size_t>(index - start)];
        }
    }
    sampleIndex_ += frames;
}

}  // namespace anc
