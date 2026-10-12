#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <NeuralAudio/NeuralModel.h>
#include "ToneBrain.h"

#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <memory>
#include <vector>

namespace {
constexpr int kBlock = 2048;
constexpr int kMaxModels = 24;
const juce::Colour kBg(0xff101512), kPanel(0xff1c2621), kAccent(0xffd2ae69), kText(0xffedebdf);

struct Capture {
    juce::String name;
    juce::File file;
    std::unique_ptr<NeuralAudio::NeuralModel> model;
    float character = 0.5f;
    float recommendedInput = 0.0f;
    float recommendedOutput = 0.0f;
};

// Character measurement uses generated sine probes in RAM, not a recorded guitar.
// The result is an approximate distortion score, not a faithful musical description.
float measureCharacter(NeuralAudio::NeuralModel& model, double sampleRate) {
    constexpr int length = 8192, chunkSize = 512;
    std::array<float, length> x{}, y{};
    auto test = [&](float amplitude) {
        const double omega = 2.0 * juce::MathConstants<double>::pi * 220.0 / sampleRate;
        for (int i = 0; i < length; ++i)
            x[static_cast<size_t>(i)] = amplitude * std::sin(omega * i);
        for (int i = 0; i < length; i += chunkSize)
            model.Process(x.data() + i, y.data() + i, chunkSize);
        // Fundamental projection plus residual. Guard against silence/gated test tones.
        double s = 0, c = 0, mean = 0, total = 0;
        constexpr int start = 4096;
        for (int i = start; i < length; ++i) {
            const double v = y[static_cast<size_t>(i)];
            s += v * std::sin(omega * i);
            c += v * std::cos(omega * i);
            mean += v;
        }
        const double n = length - start;
        s *= 2.0 / n; c *= 2.0 / n; mean /= n;
        for (int i = start; i < length; ++i) {
            const double predicted = mean + s * std::sin(omega * i) + c * std::cos(omega * i);
            const double d = y[static_cast<size_t>(i)] - predicted;
            total += d * d;
        }
        const double fund = 0.5 * (s*s + c*c) * n;
        return fund < 1e-10 ? 0.0 : std::sqrt(total / (fund + total + 1e-12));
    };
    const double score = std::max(test(0.045f), test(0.22f));
    return static_cast<float>(std::clamp((score - 0.018) / 0.43, 0.0, 1.0));
}

float gainFromDb(float db) { return std::pow(10.0f, db / 20.0f); }
}

class AutoToneWindow final : public juce::AudioAppComponent, private juce::Timer {
public:
    AutoToneWindow() {
        setOpaque(true);
        title.setText("SCULPT   /   AUTO TONE", juce::dontSendNotification);
        title.setFont(juce::Font(juce::FontOptions(25.0f, juce::Font::bold)));
        title.setColour(juce::Label::textColourId, kAccent);
        subtitle.setText("Mac beta 0.1   •   Guitar in → Auto NAM → Audio out", juce::dontSendNotification);
        subtitle.setColour(juce::Label::textColourId, kText.withAlpha(0.66f));
        status.setText("1. Imposta l'interfaccia audio  2. Carica una cartella di .nam  3. Suona", juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, kText);
        status.setJustificationType(juce::Justification::centredLeft);
        current.setText("NESSUN PROFILO", juce::dontSendNotification);
        current.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
        current.setColour(juce::Label::textColourId, kText);
        level.setText("INPUT  —", juce::dontSendNotification);
        level.setColour(juce::Label::textColourId, kText.withAlpha(0.74f));
        info.setText("Motore euristico: usa dinamica, attacchi e carattere misurato dei NAM. Non riconosce ancora i gusti musicali.",juce::dontSendNotification);
        info.setColour(juce::Label::textColourId, kText.withAlpha(0.62f));
        info.setJustificationType(juce::Justification::topLeft);

        audioButton.setButtonText("AUDIO / SSL");
        folderButton.setButtonText("CARICA NAM");
        irButton.setButtonText("CARICA CAB IR");
        lockButton.setButtonText("LOCK: OFF");
        autoButton.setButtonText("AUTO: ON");
        likeButton.setButtonText("♥  MI PIACE");
        skipButton.setButtonText("NON MI PIACE");
        for (auto* c : std::initializer_list<juce::Component*>{&title,&subtitle,&status,&current,&level,&info,
                 &audioButton,&folderButton,&irButton,&lockButton,&autoButton,&likeButton,&skipButton}) addAndMakeVisible(c);
        auto setupButton = [](juce::TextButton& b) {
            b.setColour(juce::TextButton::buttonColourId, kPanel);
            b.setColour(juce::TextButton::buttonOnColourId, kAccent.darker());
            b.setColour(juce::TextButton::textColourOffId, kText);
        };
        for (auto* b : {&audioButton,&folderButton,&irButton,&lockButton,&autoButton,&likeButton,&skipButton}) setupButton(*b);
        audioButton.onClick = [this] { showAudioSettings(); };
        folderButton.onClick = [this] { chooseNAMFolder(); };
        irButton.onClick = [this] { chooseIR(); };
        lockButton.onClick = [this] {
            locked.store(!locked.load());
            lockButton.setButtonText(locked.load() ? "LOCK: ON" : "LOCK: OFF");
        };
        autoButton.onClick = [this] {
            automatic.store(!automatic.load());
            autoButton.setButtonText(automatic.load() ? "AUTO: ON" : "AUTO: OFF");
        };
        likeButton.onClick = [this] { rate(+1); };
        skipButton.onClick = [this] { rate(-1); };
        for (auto& value : feedback) value.store(0);
        setSize(760, 475);
        setAudioChannels(1, 2);
        startTimerHz(10);
    }

    ~AutoToneWindow() override { stopTimer(); shutdownAudio(); }

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override {
        const auto maxBuffer = std::max(kBlock, samplesPerBlockExpected);
        (void) maxBuffer; // engine processes in 2048-sample chunks
        inputScratch.fill(0);
        outA.fill(0); outB.fill(0);
        brain.reset(sampleRate);
        sampleRateLive.store(sampleRate);
        cab.prepare({ sampleRate, static_cast<juce::uint32>(kBlock), 1 });
        cab.reset();
        activeIndex = captures.empty() ? -1 : 0;
        fadingFrom = -1;
        fadeRemaining = 0;
        shownIndex.store(activeIndex);
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override {
        auto* audio = bufferToFill.buffer;
        if (audio == nullptr || audio->getNumChannels() == 0) return;
        const int start = bufferToFill.startSample;
        const int count = bufferToFill.numSamples;
        const int numChannels = audio->getNumChannels();
        if (captures.empty()) {
            audio->clear(start, count);
            levelDb.store(-90.0f);
            return;
        }
        int offset = 0;
        while (offset < count) {
            const int n = std::min(kBlock, count - offset);
            const float* input = audio->getReadPointer(0, start + offset);
            std::copy_n(input, n, inputScratch.begin());
            const int desired = requestedIndex.load();
            if (desired >= 0 && desired < static_cast<int>(captures.size()) && desired != activeIndex && fadeRemaining <= 0) {
                fadingFrom = activeIndex;
                activeIndex = desired;
                fadeRemaining = 2048; // approx 43ms @48 kHz
                shownIndex.store(activeIndex);
            }
            if (activeIndex >= 0) {
                auto& entry = captures[static_cast<size_t>(activeIndex)];
                const float gi = gainFromDb(juce::jlimit(-18.f, 18.f, entry.recommendedInput));
                const float go = 0.25f * gainFromDb(juce::jlimit(-18.f, 18.f, entry.recommendedOutput));
                for (int i=0;i<n;++i) driven[static_cast<size_t>(i)] = inputScratch[static_cast<size_t>(i)] * gi;
                entry.model->Process(driven.data(), outA.data(), static_cast<size_t>(n));
                for (int i=0;i<n;++i) outA[static_cast<size_t>(i)] *= go;
            } else {
                for (int i=0;i<n;++i) outA[static_cast<size_t>(i)] = inputScratch[static_cast<size_t>(i)] * 0.25f;
            }
            if (fadeRemaining > 0 && fadingFrom >= 0) {
                auto& oldEntry = captures[static_cast<size_t>(fadingFrom)];
                const float gi = gainFromDb(juce::jlimit(-18.f,18.f,oldEntry.recommendedInput));
                const float go = 0.25f * gainFromDb(juce::jlimit(-18.f,18.f,oldEntry.recommendedOutput));
                for (int i=0;i<n;++i) driven[static_cast<size_t>(i)] = inputScratch[static_cast<size_t>(i)] * gi;
                oldEntry.model->Process(driven.data(), outB.data(), static_cast<size_t>(n));
                for (int i=0;i<n;++i) {
                    const float a = 1.0f - juce::jlimit(0.f, 1.f, float(fadeRemaining - i) / 2048.0f);
                    outA[static_cast<size_t>(i)] = outB[static_cast<size_t>(i)] * go * (1.0f-a) + outA[static_cast<size_t>(i)] * a;
                }
                fadeRemaining -= n;
                if (fadeRemaining <= 0) fadingFrom = -1;
            }
            if (irLoaded.load()) {
                cabBuffer.copyFrom(0,0,outA.data(),n);
                juce::dsp::AudioBlock<float> block(cabBuffer);
                auto slice = block.getSubBlock(0,static_cast<size_t>(n));
                juce::dsp::ProcessContextReplacing<float> context(slice);
                cab.process(context);
                std::copy_n(cabBuffer.getReadPointer(0), n, outA.begin());
            } else {
                // Basic lowpass for safer playback without a cabinet IR; not a cabinet simulation.
                const float pole = std::exp(-2.0f * juce::MathConstants<float>::pi * 5500.f /
                                static_cast<float>(std::max(8000.0, sampleRateLive.load())));
                for (int i=0;i<n;++i) {
                    lowpassZ = (1-pole)*outA[static_cast<size_t>(i)] + pole*lowpassZ;
                    outA[static_cast<size_t>(i)] = lowpassZ;
                }
            }
            auto* left = audio->getWritePointer(0, start + offset);
            for (int i=0;i<n;++i) left[i] = juce::jlimit(-0.85f,0.85f,outA[static_cast<size_t>(i)]);
            for (int ch=1;ch<numChannels;++ch) audio->copyFrom(ch,start+offset,*audio,0,start+offset,n);
            if (automatic.load()) {
                for (size_t j = 0; j < currentFeedback.size(); ++j)
                    currentFeedback[j] = feedback[j].load(std::memory_order_relaxed);
                auto result = brain.update(inputScratch.data(), n, characters, currentFeedback,
                                          activeIndex, locked.load());
                levelDb.store(result.level);
                analysisTarget.store(result.targetDrive);
                if (result.index >= 0) requestedIndex.store(result.index);
            } else {
                double sum=0;
                for (int i=0;i<n;++i) sum += inputScratch[static_cast<size_t>(i)]*inputScratch[static_cast<size_t>(i)];
                levelDb.store(20.f*std::log10(std::max(1.e-8, std::sqrt(sum/n))));
            }
            offset += n;
        }
    }

    void releaseResources() override { cab.reset(); }

    void paint(juce::Graphics& g) override {
        g.fillAll(kBg);
        auto panel = getLocalBounds().reduced(18);
        panel.removeFromTop(94);
        g.setColour(kPanel); g.fillRoundedRectangle(panel.toFloat(), 11.0f);
        g.setColour(kAccent.withAlpha(.38f)); g.drawRoundedRectangle(panel.toFloat(), 11.f, 1.f);
        g.setColour(kAccent);
        const int meterWidth = juce::jlimit(0, 510, static_cast<int>((levelDb.load() + 70.0f) / 70.0f * 510.0f));
        g.fillRoundedRectangle(juce::Rectangle<float>(213.f,248.f,static_cast<float>(meterWidth),5.f), 2.f);
    }

    void resized() override {
        title.setBounds(30,20,620,35); subtitle.setBounds(31,58,650,23);
        audioButton.setBounds(32,115,135,36);
        folderButton.setBounds(176,115,165,36);
        irButton.setBounds(350,115,165,36);
        autoButton.setBounds(524,115,100,36);
        lockButton.setBounds(632,115,96,36);
        status.setBounds(32,160,695,45);
        current.setBounds(32,202,690,42);
        level.setBounds(32,239,175,22);
        likeButton.setBounds(32,292,150,36);
        skipButton.setBounds(192,292,155,36);
        info.setBounds(32,356,685,63);
    }

private:
    void showAudioSettings() {
        juce::DialogWindow::LaunchOptions opt;
        auto* selector = new juce::AudioDeviceSelectorComponent(deviceManager, 1, 2, 1, 2, false, false, true, false);
        selector->setSize(520, 400);
        opt.content.setOwned(selector);
        opt.dialogTitle = "Ingresso / Uscita audio";
        opt.dialogBackgroundColour = kBg;
        opt.escapeKeyTriggersCloseButton = true;
        opt.useNativeTitleBar = true;
        opt.resizable = false;
        opt.launchAsync();
        status.setText("Se cambi sample rate, ricarica la cartella NAM per riallineare i modelli.",juce::dontSendNotification);
    }

    void chooseNAMFolder() {
        fileChooser = std::make_unique<juce::FileChooser>("Seleziona una cartella di modelli .nam", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory));
        fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                [this](const juce::FileChooser& chooser) {
            const auto folder = chooser.getResult();
            if (folder.isDirectory()) {
                status.setText("Caricamento dei NAM e analisi timbrica...",juce::dontSendNotification);
                juce::MessageManager::callAsync([safe = juce::Component::SafePointer<AutoToneWindow>(this), folder]() {
                    if (safe != nullptr) safe->loadModels(folder);
                });
            }
        });
    }

    void loadModels(const juce::File& folder) {
        const auto* deviceBeforeShutdown = deviceManager.getCurrentAudioDevice();
        const int sr = static_cast<int>(deviceBeforeShutdown ? deviceBeforeShutdown->getCurrentSampleRate() : 48000);
        shutdownAudio(); // invariant: models may only be created/destroyed while callback is stopped
        captures.clear(); characters.clear(); currentFeedback.clear();
        for (auto& r : feedback) r.store(0);
        auto files = folder.findChildFiles(juce::File::findFiles, true, "*.nam");
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.getFullPathName() < b.getFullPathName(); });
        NeuralAudio::NeuralModelLoader loader;
        loader.SetDefaultMaxAudioBufferSize(kBlock);
        loader.SetExternalSampleRate(sr);
        loader.SetAudioInputLevelDBu(12.f); // adjustable in a later beta; not measured from interface
        int fails = 0;
        for (const auto& file : files) {
            if (captures.size() >= static_cast<size_t>(kMaxModels)) break;
            try {
                auto ptr = std::unique_ptr<NeuralAudio::NeuralModel>(loader.CreateFromFile(
                    std::filesystem::path(file.getFullPathName().toStdString())));
                if (!ptr) { ++fails; continue; }
                Capture e;
                e.file = file;
                e.name = file.getFileNameWithoutExtension();
                e.recommendedInput = ptr->GetRecommendedInputDBAdjustment();
                e.recommendedOutput = ptr->GetRecommendedOutputDBAdjustment();
                e.character = measureCharacter(*ptr, static_cast<double>(sr));
                e.model = std::move(ptr);
                characters.push_back(e.character);
                currentFeedback.push_back(0);
                captures.push_back(std::move(e));
            } catch (const std::exception&) { ++fails; }
        }
        requestedIndex.store(captures.empty() ? -1 : 0);
        shownIndex.store(captures.empty() ? -1 : 0);
        setAudioChannels(1,2);
        if (captures.empty()) {
            status.setText("Nessun NAM compatibile caricato. Controlla i file .nam.",juce::dontSendNotification);
            current.setText("NESSUN PROFILO",juce::dontSendNotification);
        } else {
            status.setText(juce::String(captures.size())+" NAM caricati. Suona: la scelta si aggiorna automaticamente. Scartati: "+juce::String(fails), juce::dontSendNotification);
            current.setText(captures.front().name,juce::dontSendNotification);
        }
    }

    void chooseIR() {
        irChooser = std::make_unique<juce::FileChooser>("Seleziona una cabinet IR WAV", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory), "*.wav");
        irChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                               [this](const juce::FileChooser& chooser) {
            const auto f = chooser.getResult();
            if (f.existsAsFile()) {
                cab.loadImpulseResponse(f, juce::dsp::Convolution::Stereo::no,
                    juce::dsp::Convolution::Trim::yes, 0,
                    juce::dsp::Convolution::Normalise::yes);
                irLoaded.store(true);
                irName = f.getFileName();
                status.setText("IR caricata: "+irName+". Suona per cercare il NAM.",juce::dontSendNotification);
            }
        });
    }

    void rate(int delta) {
        const int i = shownIndex.load();
        if (i < 0 || i >= static_cast<int>(captures.size())) return;
        auto& item = feedback[static_cast<size_t>(i)];
        const int vote = juce::jlimit(-3,3,item.load()+delta);
        item.store(vote);
        // Preference snapshot is updated on the audio thread via atomics; no vector mutation.
        status.setText(captures[static_cast<size_t>(i)].name + (delta>0 ? " · gradimento aggiunto" : " · penalizzato nelle ricerche"),juce::dontSendNotification);
    }

    void timerCallback() override {
        const int i = shownIndex.load();
        if (i>=0 && i < static_cast<int>(captures.size()))
            current.setText(captures[static_cast<size_t>(i)].name + "   /   " +
                juce::String(juce::roundToInt(captures[static_cast<size_t>(i)].character * 100.0f)) + "% DRIVE",juce::dontSendNotification);
        level.setText("INPUT  " + juce::String(levelDb.load(), 1) + " dBFS",juce::dontSendNotification);
        repaint(juce::Rectangle<int>(206,244,520,14));
    }

    juce::Label title, subtitle, status, current, level, info;
    juce::TextButton audioButton, folderButton, irButton, lockButton, autoButton, likeButton, skipButton;
    std::unique_ptr<juce::FileChooser> fileChooser, irChooser;
    std::vector<Capture> captures;
    std::vector<float> characters;
    std::vector<int> currentFeedback; // allocated while audio is stopped; refreshed from atomics in callback
    std::array<std::atomic<int>, kMaxModels> feedback;
    ToneBrain brain;
    std::atomic<int> requestedIndex{-1}, shownIndex{-1};
    std::atomic<bool> locked{false}, automatic{true}, irLoaded{false};
    std::atomic<float> levelDb{-90.f}, analysisTarget{0.f};
    std::atomic<double> sampleRateLive{48000.};
    int activeIndex = -1, fadingFrom = -1, fadeRemaining = 0;
    float lowpassZ = 0.0f;
    std::array<float, kBlock> inputScratch{}, driven{}, outA{}, outB{};
    juce::AudioBuffer<float> cabBuffer{1,kBlock};
    juce::dsp::Convolution cab;
    juce::String irName;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AutoToneWindow)
};

class SculptMainWindow final : public juce::DocumentWindow {
public:
    SculptMainWindow() : juce::DocumentWindow("Sculpt AutoTone — Beta", kBg, juce::DocumentWindow::allButtons) {}
    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class SculptApp final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Sculpt AutoTone"; }
    const juce::String getApplicationVersion() override { return "0.1.0-beta"; }
    bool moreThanOneInstanceAllowed() override { return false; }
    void initialise(const juce::String&) override {
        mainWindow = std::make_unique<SculptMainWindow>();
        mainWindow->setUsingNativeTitleBar(true);
        mainWindow->setContentOwned(new AutoToneWindow(), true);
        mainWindow->centreWithSize(760,475);
        mainWindow->setResizable(false,false);
        mainWindow->setVisible(true);
    }
    void shutdown() override { mainWindow.reset(); }
    void systemRequestedQuit() override { quit(); }
private:
    std::unique_ptr<SculptMainWindow> mainWindow;
};
START_JUCE_APPLICATION(SculptApp)
