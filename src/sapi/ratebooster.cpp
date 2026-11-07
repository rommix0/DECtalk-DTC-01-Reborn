#include "ratebooster.hpp"

#include <algorithm>
#include <cmath>

namespace dtc01 {

namespace {

// WSOLA constants, tuned for 10 kHz mono speech (see the header comment for
// why these aren't literal Sonic constants).
constexpr int kFrameSamples = 200;    // 20 ms: a handful of pitch periods for voiced speech.
constexpr int kOverlapSamples = 50;   // 5 ms crossfade region between consecutive frames.
constexpr int kSynthesisHop = kFrameSamples - kOverlapSamples;  // 150: new samples appended per frame.
constexpr int kSearchTolerance = 40;  // +/- samples searched for the best phase alignment.

// Normalized cross-correlation between two equal-length windows. Used to
// find the source offset whose overlap region best lines up in phase with
// the tail already written to the output, which is what avoids the
// phase-cancellation artifacts a naive fixed-hop OLA produces on
// voiced/tonal audio.
double correlation(const int16_t* a, const int16_t* b, int n) {
    double sum = 0.0, energyA = 0.0, energyB = 0.0;
    for (int i = 0; i < n; ++i) {
        double av = static_cast<double>(a[i]);
        double bv = static_cast<double>(b[i]);
        sum += av * bv;
        energyA += av * av;
        energyB += bv * bv;
    }
    double denom = std::sqrt(energyA * energyB);
    if (denom < 1e-9) return 0.0;  // silence on either side: treat as no preference.
    return sum / denom;
}

int16_t clamp16(double v) {
    if (v > 32767.0) return 32767;
    if (v < -32768.0) return -32768;
    return static_cast<int16_t>(std::lround(v));
}

}  // namespace

std::vector<int16_t> time_compress(const std::vector<int16_t>& in, double factor) {
    double f = factor;
    if (f < 1.0) f = 1.0;
    f = std::min(f, 6.0);  // matches RateBooster.speed's own clamp in ratebooster.py.

    if (f <= 1.0 + 1e-9) {
        return in;  // exact passthrough, matches RateBooster.active == false at speed 1.0.
    }

    const int n = static_cast<int>(in.size());
    if (n < kFrameSamples) {
        return in;  // too short to frame meaningfully.
    }

    std::vector<int16_t> out;
    out.reserve(static_cast<size_t>(n / f) + kFrameSamples);

    const double analysisHop = kSynthesisHop * f;

    // The first frame goes straight through -- there is no output tail yet
    // to phase-align against.
    out.insert(out.end(), in.begin(), in.begin() + kFrameSamples);

    // Nominal (undithered) source schedule. Advanced independently of the
    // per-frame search jitter below so the overall compression ratio does
    // not drift away from `factor` over a long utterance.
    double sourceCursor = analysisHop;

    while (true) {
        int nominal = static_cast<int>(std::lround(sourceCursor));
        if (nominal + kFrameSamples > n) break;

        int loSearch = std::max(0, nominal - kSearchTolerance);
        int hiSearch = std::min(n - kFrameSamples, nominal + kSearchTolerance);
        const int16_t* tail = &out[out.size() - kOverlapSamples];

        int bestStart = nominal;
        double bestScore = -2.0;
        for (int cand = loSearch; cand <= hiSearch; ++cand) {
            double score = correlation(tail, &in[cand], kOverlapSamples);
            if (score > bestScore) {
                bestScore = score;
                bestStart = cand;
            }
        }

        // Crossfade the new frame's first kOverlapSamples over the tail
        // already in `out`, then append the rest of the frame unmodified.
        size_t fadeBase = out.size() - kOverlapSamples;
        for (int i = 0; i < kOverlapSamples; ++i) {
            double t = static_cast<double>(i) / (kOverlapSamples - 1);
            double mixed = out[fadeBase + i] * (1.0 - t) + in[bestStart + i] * t;
            out[fadeBase + i] = clamp16(mixed);
        }
        out.insert(out.end(), in.begin() + bestStart + kOverlapSamples,
                   in.begin() + bestStart + kFrameSamples);

        sourceCursor += analysisHop;
    }

    return out;
}

TimeCompressor::TimeCompressor(double factor) {
    double f = factor;
    if (f < 1.0) f = 1.0;
    f = std::min(f, 6.0);
    active_ = f > 1.0 + 1e-9;
    hop_ = kSynthesisHop * f;
    cursor_ = hop_;
}

void TimeCompressor::push(const int16_t* samples, size_t count, std::vector<int16_t>& out) {
    if (!active_) {
        out.insert(out.end(), samples, samples + count);
        return;
    }
    in_.insert(in_.end(), samples, samples + count);
    total_ += count;
    if (!started_) {
        if (total_ < static_cast<size_t>(kFrameSamples)) {
            return;  // might yet end shorter than a frame: time_compress's passthrough
        }
        // The first frame goes straight through, as in time_compress; its last
        // overlap stays pending for the next frame's crossfade.
        const auto overlap_at = in_.begin() + (kFrameSamples - kOverlapSamples);
        out.insert(out.end(), in_.begin(), overlap_at);
        tail_.assign(overlap_at, in_.begin() + kFrameSamples);
        started_ = true;
    }
    run_frames(false, out);
}

void TimeCompressor::finish(std::vector<int16_t>& out) {
    if (active_) {
        if (started_) {
            run_frames(true, out);
            out.insert(out.end(), tail_.begin(), tail_.end());
        } else {
            out.insert(out.end(), in_.begin(), in_.end());
        }
    }
    in_.clear();
    tail_.clear();
    base_ = 0;
    total_ = 0;
    cursor_ = hop_;
    started_ = false;
}

void TimeCompressor::run_frames(bool final, std::vector<int16_t>& out) {
    const int n = static_cast<int>(total_);
    for (;;) {
        const int nominal = static_cast<int>(std::lround(cursor_));
        // Mid-stream a frame is taken only once its whole search window has
        // arrived, so it is chosen exactly as time_compress chooses it knowing
        // the final length; finish() takes the remaining frames with that length.
        if (final ? nominal + kFrameSamples > n
                  : nominal + kSearchTolerance + kFrameSamples > n) {
            break;
        }

        const int lo = std::max(0, nominal - kSearchTolerance);
        const int hi = std::min(n - kFrameSamples, nominal + kSearchTolerance);
        int best = nominal;
        double bestScore = -2.0;
        for (int cand = lo; cand <= hi; ++cand) {
            double score = correlation(tail_.data(), &in_[cand - base_], kOverlapSamples);
            if (score > bestScore) {
                bestScore = score;
                best = cand;
            }
        }

        const int16_t* frame = &in_[best - base_];
        for (int i = 0; i < kOverlapSamples; ++i) {
            double t = static_cast<double>(i) / (kOverlapSamples - 1);
            double mixed = tail_[i] * (1.0 - t) + frame[i] * t;
            tail_[i] = clamp16(mixed);
        }
        out.insert(out.end(), tail_.begin(), tail_.end());
        out.insert(out.end(), frame + kOverlapSamples, frame + kFrameSamples - kOverlapSamples);
        tail_.assign(frame + kFrameSamples - kOverlapSamples, frame + kFrameSamples);

        cursor_ += hop_;
    }

    // Drop input no later frame can reach: a search starts kSearchTolerance
    // before its frame's nominal position, and positions only move forward.
    // The next frame's position can lie beyond the input received so far, so
    // never drop more than there is.
    const size_t keep_from = std::min(total_, static_cast<size_t>(std::max(
        0, static_cast<int>(std::lround(cursor_)) - kSearchTolerance)));
    if (keep_from > base_ + 8192) {
        in_.erase(in_.begin(), in_.begin() + (keep_from - base_));
        base_ = keep_from;
    }
}

}  // namespace dtc01
