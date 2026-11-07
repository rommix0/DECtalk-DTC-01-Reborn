#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// C++ port of addon/synthDrivers/dectalkDtc01/ratebooster.py's RateBooster.
//
// The Python source is a ctypes binding to an external sonic.dll (the
// "Sonic" time-domain audio speedup library) and carries no DSP of its own
// -- sonic.dll is not part of this source tree, so there is no literal
// window/hop table to port byte-for-byte. What this file ports faithfully
// is Sonic's *class* of algorithm and its contract: fixed-size analysis
// frames pulled from the input, phase-aligned against the tail of the
// already-produced output via a local cross-correlation search, then
// overlap-added with a linear crossfade -- i.e. WSOLA (Waveform-Similarity
// Overlap-Add), the standard pitch-preserving time-scale method Sonic
// itself is built on. See ratebooster.cpp for the concrete constants.
namespace dtc01 {

// Sample rate this implementation is tuned for: the DTC-01 firmware's own
// rendered audio format (mono, 16-bit PCM, 10 kHz).
constexpr int kRateBoosterSampleRate = 10000;

// Time-compresses 16-bit mono PCM while preserving pitch.
//
// `factor` is the duration-compression ratio: output length is
// approximately input length / factor.
//   - factor == 1.0 (or <= 1.0) is an exact passthrough (returns `in`
//     unchanged), matching RateBooster.active being false at speed 1.0 in
//     the Python source, where the audio path costs nothing.
//   - factor is clamped to at most 6.0, matching RateBooster.speed's own
//     `max(1.0, min(6.0, value))` clamp in ratebooster.py.
std::vector<int16_t> time_compress(const std::vector<int16_t>& in, double factor);

// Streaming form of time_compress(), for audio that arrives as it is
// synthesized: output is released as soon as no later input can change it
// (about 30 ms behind the input), rather than after the whole utterance.
// Everything push() and finish() hand back, concatenated, is identical to
// time_compress() over the concatenated input -- time_compress() remains the
// reference test_ratebooster.cpp holds this to.
class TimeCompressor {
public:
    explicit TimeCompressor(double factor);

    // False when the factor makes this an exact passthrough.
    bool active() const { return active_; }

    // Appends `count` input samples, and appends any final output to `out`.
    void push(const int16_t* samples, size_t count, std::vector<int16_t>& out);

    // Ends the stream: appends the rest of the output to `out`, then resets
    // so the next push() starts a new stream at the same factor.
    void finish(std::vector<int16_t>& out);

private:
    void run_frames(bool final, std::vector<int16_t>& out);

    bool active_ = false;
    double hop_ = 0.0;            // analysis hop, in input samples
    std::vector<int16_t> in_;     // input from stream index base_ onward
    size_t base_ = 0;
    size_t total_ = 0;            // input samples pushed this stream
    std::vector<int16_t> tail_;   // last overlap of output, still to be crossfaded
    double cursor_ = 0.0;         // nominal source position of the next frame
    bool started_ = false;        // the first frame has been taken
};

}  // namespace dtc01
