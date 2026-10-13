#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

// Nine hosted VST3 insert positions: PRE 1-4, SCULPT CHANNEL, POST 1-4.
// Plugin creation, removal and state changes happen only with the audio callback stopped.
// Bypass toggles are atomic; the audio callback never uses locks or allocates plugin instances.
// Live-host musical clock. Every hosted VST3 receives the same timing and tempo
// through AudioProcessor::setPlayHead(). The clock advances once per host block,
// never once per plugin: PRE, Sculpt Channel and POST remain phase-aligned.
class SculptRigPlayHead final : public juce::AudioPlayHead {
public:
    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo position;
        const double tempo = bpm.load(std::memory_order_relaxed);
        const double beat = ppq.load(std::memory_order_relaxed);
        const auto sample = samples.load(std::memory_order_relaxed);
        const double rate = sampleRate.load(std::memory_order_relaxed);
        position.setBpm(tempo);
        position.setTimeSignature(TimeSignature{4, 4});
        position.setPpqPosition(beat);
        position.setPpqPositionOfLastBarStart(std::floor(beat / 4.0) * 4.0);
        position.setBarCount(static_cast<int64_t>(std::floor(beat / 4.0)));
        position.setTimeInSamples(sample);
        position.setTimeInSeconds(static_cast<double>(sample) / rate);
        position.setIsPlaying(true); // Standalone live signal chain: transport always rolling.
        position.setIsRecording(false);
        position.setIsLooping(false);
        return position;
    }

    void prepare(double rate) noexcept {
        sampleRate.store(std::max(8000.0, rate), std::memory_order_relaxed);
        samples.store(0, std::memory_order_relaxed);
        ppq.store(0.0, std::memory_order_relaxed);
    }

    void beginBlock(double newBpm) noexcept {
        if (std::isfinite(newBpm))
            bpm.store(std::clamp(newBpm, 40.0, 250.0), std::memory_order_relaxed);
    }

    void endBlock(int count) noexcept {
        if (count <= 0) return;
        const double rate = sampleRate.load(std::memory_order_relaxed);
        const double current = bpm.load(std::memory_order_relaxed);
        ppq.store(ppq.load(std::memory_order_relaxed) +
                  static_cast<double>(count) * current / (60.0 * rate),
                  std::memory_order_relaxed);
        samples.store(samples.load(std::memory_order_relaxed) + count, std::memory_order_relaxed);
    }

private:
    std::atomic<double> sampleRate{48000.0};
    std::atomic<double> bpm{120.0};
    std::atomic<double> ppq{0.0};
    std::atomic<int64_t> samples{0};
};

class VstChain final {
public:
    static constexpr int preSlots = 4;
    static constexpr int sculptSlot = 4;
    static constexpr int postFirst = 5;
    static constexpr int totalSlots = 9;

    VstChain() { formats.addDefaultFormats(); }
    ~VstChain() { release(); }

    VstChain(const VstChain&) = delete;
    VstChain& operator=(const VstChain&) = delete;

    juce::AudioPluginInstance* get(int i) const noexcept {
        return valid(i) ? slots[static_cast<size_t>(i)].processor.get() : nullptr;
    }
    juce::File bundlePath(int i) const {
        return valid(i) ? slots[static_cast<size_t>(i)].bundleFile : juce::File();
    }
    juce::String name(int i) const {
        if (!valid(i)) return {};
        return slots[static_cast<size_t>(i)].displayName;
    }
    bool enabled(int i) const noexcept {
        return valid(i) && slots[static_cast<size_t>(i)].active.load(std::memory_order_relaxed);
    }
    void enable(int i, bool value) noexcept {
        if (valid(i)) slots[static_cast<size_t>(i)].active.store(value, std::memory_order_relaxed);
    }

    bool load(int i, const juce::File& bundle, double rate, int blockSize, juce::String& error) {
        if (!valid(i) || !bundle.exists() || !bundle.hasFileExtension("vst3")) {
            error = "Seleziona un plugin .vst3 installato sul Mac.";
            return false;
        }
#if JUCE_PLUGINHOST_VST3
        juce::VST3PluginFormat format;
        juce::OwnedArray<juce::PluginDescription> candidates;
        format.findAllTypesForFile(candidates, bundle.getFullPathName());
        if (candidates.isEmpty()) {
            error = "Il VST3 non contiene un effetto caricabile, o e' incompatibile con questo Mac.";
            return false;
        }
        // Prefer an audio effect rather than instruments in a multi-plugin bundle.
        const juce::PluginDescription* chosen = nullptr;
        for (auto* d : candidates) {
            if (!d->isInstrument) { chosen = d; break; }
        }
        if (chosen == nullptr) {
            error = "Sono consentiti VST3 audio FX, non strumenti virtuali.";
            return false;
        }
        if (i == sculptSlot) {
            const auto nm = (chosen->name + " " + bundle.getFileNameWithoutExtension()).toLowerCase();
            if (!nm.contains("sculpt") || !(nm.contains("channel") || nm.contains("chan"))) {
                error = "Questo slot e' dedicato a Sculpt Channel. Seleziona Sculpt Channel.vst3.";
                return false;
            }
        }
        auto plugin = formats.createPluginInstance(*chosen, rate, blockSize, error);
        if (!plugin) {
            if (error.isEmpty()) error = "Caricamento VST3 non riuscito.";
            return false;
        }
        auto& slot = slots[static_cast<size_t>(i)];
        if (slot.processor && prepared) slot.processor->releaseResources();
        slot.processor = std::move(plugin);
        slot.processor->setPlayHead(&transport);
        slot.displayName = chosen->name.isNotEmpty() ? chosen->name : bundle.getFileNameWithoutExtension();
        slot.bundleFile = bundle;
        slot.active.store(true, std::memory_order_release);
        if (prepared && slot.processor)
            prepareProcessor(*slot.processor, sampleRateLive, blockSizeLive);
        return true;
#else
        error = "La build non ha abilitato JUCE_PLUGINHOST_VST3.";
        return false;
#endif
    }

    void remove(int i) {
        if (!valid(i)) return;
        auto& s = slots[static_cast<size_t>(i)];
        if (s.processor && prepared) s.processor->releaseResources();
        s.processor.reset();
        s.displayName.clear();
        s.bundleFile = juce::File();
        s.active.store(false, std::memory_order_release);
    }

    // A single musical clock drives all nine slots: samples, PPQ, 4/4, playing, BPM.
    // Called by the live audio callback once per chunk, NOT by each plugin.
    void beginBlock(double bpm) noexcept { transport.beginBlock(bpm); }
    void endBlock(int numSamples) noexcept { transport.endBlock(numSamples); }

    void prepare(double sampleRate, int blockSize) {
        sampleRateLive = sampleRate;
        blockSizeLive = blockSize;
        transport.prepare(sampleRate);
        for (auto& s : slots) {
            if (!s.processor) continue;
            prepareProcessor(*s.processor, sampleRate, blockSize);
        }
        prepared = true;
    }

    void release() {
        if (!prepared) return;
        for (auto& s : slots) if (s.processor) s.processor->releaseResources();
        prepared = false;
    }

    // The host's working buffer always has two channels. For mono-only processors,
    // JUCE allows a buffer with additional channels, while the first channel carries audio.
    void process(int i, juce::AudioBuffer<float>& buffer) noexcept {
        if (!valid(i)) return;
        auto& s = slots[static_cast<size_t>(i)];
        if (!s.processor || !s.active.load(std::memory_order_relaxed)) return;
        const int ins = s.processor->getTotalNumInputChannels();
        const int outs = s.processor->getTotalNumOutputChannels();
        if (outs < 1 || ins < 1 || ins > 2 || outs > 2) return; // Unsupported layouts are safely bypassed.
        juce::MidiBuffer noMidi;
        if (ins == 1 && outs == 1) {
            juce::AudioBuffer<float> mono(buffer.getArrayOfWritePointers(),1,buffer.getNumSamples());
            s.processor->processBlock(mono,noMidi);
            if (buffer.getNumChannels() > 1)
                buffer.copyFrom(1,0,buffer,0,0,buffer.getNumSamples());
        } else {
            s.processor->processBlock(buffer,noMidi);
            if (outs == 1 && buffer.getNumChannels() > 1)
                buffer.copyFrom(1,0,buffer,0,0,buffer.getNumSamples());
        }
    }

    static juce::Array<juce::File> installedVst3() {
        juce::Array<juce::File> files;
        const juce::File paths[] = {
            juce::File("/Library/Audio/Plug-Ins/VST3"),
            juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                .getChildFile("Library/Audio/Plug-Ins/VST3")
        };
        for (const auto& root : paths) {
            if (!root.isDirectory()) continue;
            auto items = root.findChildFiles(juce::File::findDirectories | juce::File::findFiles, true, "*.vst3");
            for (const auto& f : items) if (f.hasFileExtension("vst3") && !files.contains(f)) files.add(f);
        }
        return files;
    }


    void prepareProcessor(juce::AudioPluginInstance& p, double sampleRate, int blockSize) {
        p.setPlayHead(&transport);
        juce::AudioProcessor::BusesLayout stereo;
        stereo.inputBuses.add(juce::AudioChannelSet::stereo());
        stereo.outputBuses.add(juce::AudioChannelSet::stereo());
        bool ok = p.setBusesLayout(stereo);
        if (!ok) {
            juce::AudioProcessor::BusesLayout mono;
            mono.inputBuses.add(juce::AudioChannelSet::mono());
            mono.outputBuses.add(juce::AudioChannelSet::mono());
            ok = p.setBusesLayout(mono);
        }
        p.enableAllBuses();
        const int ins = juce::jmax(1, p.getTotalNumInputChannels());
        const int outs = juce::jmax(1, p.getTotalNumOutputChannels());
        p.setPlayConfigDetails(ins, outs, sampleRate, blockSize);
        p.setRateAndBufferSizeDetails(sampleRate, blockSize);
        p.setNonRealtime(false);
        p.prepareToPlay(sampleRate, blockSize);
        p.suspendProcessing(false);
        p.reset();
    }

private:
    struct Slot {
        std::unique_ptr<juce::AudioPluginInstance> processor;
        juce::String displayName;
        juce::File bundleFile;
        std::atomic<bool> active{false};
    };
    static bool valid(int index) noexcept { return index >= 0 && index < totalSlots; }
    juce::AudioPluginFormatManager formats;
    SculptRigPlayHead transport; // Must outlive all hosted plugin instances.
    std::array<Slot, totalSlots> slots;
    double sampleRateLive = 48000.0;
    int blockSizeLive = 2048;
    bool prepared = false;
};
