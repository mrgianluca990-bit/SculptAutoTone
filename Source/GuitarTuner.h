#pragma once
// Lightweight poly-free guitar tuner. The audio thread only decimates incoming DI
// to a lock-free atomic ring; YIN-style pitch estimation runs on the message thread.
// No heap allocations or FFT processing on the real-time callback.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

class GuitarTuner final {
public:
    struct Pitch {
        float hz = 0.0f;
        float cents = 0.0f;
        float confidence = 0.0f;
        int midi = -1;
    };

    void prepare(double sampleRate) noexcept {
        decimation = std::max(1, static_cast<int>(std::lround(sampleRate / 12000.0)));
        reducedSampleRate.store(sampleRate / decimation, std::memory_order_relaxed);
        phase = 0;
        writeHead.store(0, std::memory_order_relaxed);
        for (auto& s : samples) s.store(0.f, std::memory_order_relaxed);
    }

    void push(const float* data, int count) noexcept {
        if (data == nullptr || count <= 0) return;
        auto head = writeHead.load(std::memory_order_relaxed);
        for (int i = 0; i < count; ++i) {
            if (++phase >= decimation) {
                phase = 0;
                samples[static_cast<std::size_t>(head % capacity)].store(data[i], std::memory_order_relaxed);
                ++head;
            }
        }
        writeHead.store(head, std::memory_order_release);
    }

    // Called from the UI timer, never from the audio callback.
    Pitch detect() const noexcept {
        Pitch result;
        constexpr int window = 1024, captured = 1536;
        const auto head = writeHead.load(std::memory_order_acquire);
        const double rate = reducedSampleRate.load(std::memory_order_relaxed);
        if (head < captured || rate < 4000.0) return result;
        std::array<float, captured> data{};
        const auto begin = head - captured;
        for (int i = 0; i < captured; ++i)
            data[static_cast<std::size_t>(i)] = samples[static_cast<std::size_t>((begin + i) % capacity)].load(std::memory_order_relaxed);

        double power = 0.0;
        for (int i = 0; i < window; ++i) power += static_cast<double>(data[static_cast<std::size_t>(i)]) * data[static_cast<std::size_t>(i)];
        if (std::sqrt(power / window) < 0.0018) return result; // ignore DI noise and silence

        const int minLag = std::max(2, static_cast<int>(rate / 1300.0));
        const int maxLag = std::min(350, static_cast<int>(rate / 70.0));
        if (maxLag < minLag + 2 || window + maxLag >= captured) return result;
        std::array<double, 352> cmnd{};
        double cumulative = 0.0;
        for (int tau = 1; tau <= maxLag + 1; ++tau) {
            double difference = 0.0;
            for (int i = 0; i < window; ++i) {
                const double d = static_cast<double>(data[static_cast<std::size_t>(i)]) -
                                 data[static_cast<std::size_t>(i + tau)];
                difference += d * d;
            }
            cumulative += difference;
            cmnd[static_cast<std::size_t>(tau)] = difference * tau / std::max(cumulative, 1.0e-18);
        }
        // First clear minimum prevents picking subharmonics of a stronger harmonic.
        int candidate = -1;
        constexpr double yinThreshold = 0.18;
        for (int tau = minLag; tau < maxLag; ++tau) {
            if (cmnd[static_cast<std::size_t>(tau)] < yinThreshold) {
                while (tau + 1 <= maxLag && cmnd[static_cast<std::size_t>(tau + 1)] < cmnd[static_cast<std::size_t>(tau)]) ++tau;
                candidate = tau;
                break;
            }
        }
        if (candidate < 0) {
            double minimum = 0.23;
            for (int tau = minLag; tau <= maxLag; ++tau) {
                if (cmnd[static_cast<std::size_t>(tau)] < minimum) {
                    minimum = cmnd[static_cast<std::size_t>(tau)]; candidate = tau;
                }
            }
        }
        if (candidate < 0) return result;
        double lag = candidate;
        if (candidate > minLag && candidate < maxLag) {
            const double left = cmnd[static_cast<std::size_t>(candidate - 1)];
            const double center = cmnd[static_cast<std::size_t>(candidate)];
            const double right = cmnd[static_cast<std::size_t>(candidate + 1)];
            const double denom = left - 2.0 * center + right;
            if (std::abs(denom) > 1e-10)
                lag += std::clamp(0.5 * (left - right) / denom, -0.5, 0.5);
        }
        const double hz = rate / std::max(1.0, lag);
        if (!std::isfinite(hz) || hz < 70.0 || hz > 1300.0) return result;
        const double midiValue = 69.0 + 12.0 * std::log2(hz / 440.0);
        result.midi = static_cast<int>(std::lround(midiValue));
        result.hz = static_cast<float>(hz);
        result.cents = static_cast<float>(100.0 * (midiValue - result.midi));
        result.confidence = static_cast<float>(std::clamp(1.0 - cmnd[static_cast<std::size_t>(candidate)], 0.0, 1.0));
        return result;
    }

private:
    static constexpr std::uint64_t capacity = 4096;
    std::array<std::atomic<float>, capacity> samples{};
    std::atomic<std::uint64_t> writeHead{0};
    std::atomic<double> reducedSampleRate{12000.0};
    int decimation = 4, phase = 0;
};
