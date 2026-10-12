#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

// Audio is only inspected in RAM; no samples are persisted by this class.
// This is an explainable *heuristic*, not an ML genre/style classifier.
class ToneBrain {
public:
    void reset(double sampleRate) noexcept {
        rate = std::max(8000.0, sampleRate);
        envelope = 0.0; reference = 0.0002; ticks = 0.0;
        stableTime = 0.0; elapsedSinceChange = 99.0;
        lastProposal = -1; brightness = 0.0; strength = 0.0;
    }

    struct Result { int index = -1; float level = 0.f; float targetDrive = 0.f; bool searching = false; };

    Result update(const float* input, int count, const std::vector<float>& drive,
                  const std::vector<int>& preferences, int current, bool locked) noexcept {
        Result result;
        if (!input || count <= 0 || drive.empty()) return result;

        double sum = 0.0, diff = 0.0, peak = 0.0;
        float prev = input[0];
        for (int i = 0; i < count; ++i) {
            const float x = input[i];
            sum += double(x) * x;
            diff += std::abs(double(x - prev));
            peak = std::max(peak, std::abs(double(x)));
            prev = x;
        }
        const double rms = std::sqrt(sum / count);
        const double dt = count / rate;
        envelope = envelope * std::exp(-dt / 0.16) + rms * (1.0 - std::exp(-dt / 0.16));
        reference = std::max(envelope, reference * std::exp(-dt / 22.0));
        brightness = brightness * 0.91 + (diff / count / (rms + 0.00001)) * 0.09;
        // Absolute DI strength is only a *proxy* for desired saturation. This mapping
        // is intentionally simple and should be calibrated to the user's interface.
        const double envelopeDb = 20.0 * std::log10(std::max(envelope, 1e-8));
        const double intensity = std::clamp((envelopeDb + 47.0) / 38.0, 0.0, 1.0);
        strength = 0.84 * strength + 0.16 * intensity;
        result.level = static_cast<float>(std::clamp(20.0 * std::log10(std::max(rms, 1e-8)), -85.0, 0.0));
        result.targetDrive = static_cast<float>(std::clamp(0.04 + 0.79 * strength + 0.07 * std::min(1.0, brightness), 0.04, 0.94));
        result.searching = rms > 0.0010;
        elapsedSinceChange += dt;
        if (locked || !result.searching) { stableTime = 0.0; lastProposal = -1; return result; }

        ticks += dt;
        if (ticks < 0.4) return result;
        const double window = ticks;
        ticks = 0.0;
        int best = -1;
        double bestScore = 1e10;
        for (size_t i = 0; i < drive.size(); ++i) {
            // Positive feedback rewards familiar tones, negative feedback discourages them.
            const int vote = i < preferences.size() ? preferences[i] : 0;
            const double score = std::abs(drive[i] - result.targetDrive) - 0.055 * std::clamp(vote, -3, 3);
            if (score < bestScore) { bestScore = score; best = static_cast<int>(i); }
        }
        if (best == lastProposal) stableTime += window;
        else { lastProposal = best; stableTime = 0.0; }
        // Hysteresis and minimum dwell time: never change for a momentary pick transient.
        if (best != current && best >= 0 && stableTime >= 2.0 && elapsedSinceChange >= 7.0) {
            result.index = best;
            stableTime = 0.0;
            elapsedSinceChange = 0.0;
        }
        return result;
    }

private:
    double rate = 48000, envelope = 0, reference = 0.0002, ticks = 0;
    double stableTime = 0, elapsedSinceChange = 99, brightness = 0, strength = 0;
    int lastProposal = -1;
};
