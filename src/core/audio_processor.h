#pragma once

#include <cstdint>

namespace anc {

// A mono, floating-point processing block.
//
// prepare() is called once from a non-real-time thread before streaming.
// process() runs on the render (real-time) thread: it must not allocate,
// log, perform I/O or block.
class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;

    virtual void prepare(double sampleRate, uint32_t maxFrames) = 0;
    virtual void process(const float* in, float* out, uint32_t frames) = 0;
    virtual const char* name() const = 0;
};

}  // namespace anc
