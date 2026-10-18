#pragma once

#include <cstdint>
#include <vector>

#include "core/audio_processor.h"

namespace anc {

// Phase 2 acoustic loopback source.
//
// Emits a windowed logarithmic sweep (chirp) through the render stream every
// `intervalSeconds`, and outputs silence otherwise. The waveform is
// deterministic so the same chirp can be regenerated in Python for
// cross-correlation. It is precomputed in prepare() (no work in the audio
// thread beyond an indexed copy).
//
// The chirp is an up-sweep from `startHz` to `endHz` over `chirpSeconds`,
// amplitude-shaped with a Hann window:
//   phase(t) = 2*pi * f0 * T / ln(f1/f0) * ((f1/f0)^(t/T) - 1)
class ChirpEmitter : public AudioProcessor {
public:
    struct Config {
        double intervalSeconds = 1.5;
        double chirpSeconds = 0.2;
        double startHz = 200.0;
        double endHz = 6000.0;
        float amplitude = 0.1f;  // -20 dBFS
        double initialDelaySeconds = 0.5;
        int repeats = 20;
    };

    explicit ChirpEmitter(Config config) : config_(config) {}

    void prepare(double sampleRate, uint32_t maxFrames) override;
    void process(const float* in, float* out, uint32_t frames) override;
    const char* name() const override { return "loopback-chirp"; }

    const Config& config() const { return config_; }
    uint64_t processedFrames() const { return sampleIndex_; }
    int chirpsEmitted() const { return emitted_; }
    int totalChirps() const { return config_.repeats; }

    // Frame indices (within the processed output stream) at which each chirp
    // starts; valid after prepare().
    const std::vector<uint64_t>& chirpStartFrames() const { return chirpStartFrames_; }

    // Regenerate the exact chirp waveform in a test/analysis context.
    static std::vector<float> makeChirpWaveform(const Config& config, double sampleRate);

private:
    Config config_{};
    std::vector<float> chirp_;
    std::vector<uint64_t> chirpStartFrames_;
    uint64_t sampleIndex_ = 0;
    size_t nextChirp_ = 0;
    int emitted_ = 0;
};

}  // namespace anc
