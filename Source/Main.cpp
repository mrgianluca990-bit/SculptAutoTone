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
const juce::Colour kBg(0xff0f1013), kPanel(0xff1a1c21), kPanelAlt(0xff22252a),
                   kAccent(0xffbd514b), kWarm(0xffd4b18b), kGreen(0xff8fb69b),
                   kText(0xffe9e7e1), kMuted(0xff96989f);

// Custom draw: no operating-system default buttons, no downloaded fonts or images.
class SculptLook final : public juce::LookAndFeel_V4 {
public:
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override {
        return juce::Font(juce::FontOptions(buttonHeight >= 42 ? 13.0f : 11.5f, juce::Font::bold));
    }
    void drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour&,
                              bool hovered, bool down) override {
        const auto r = b.getLocalBounds().toFloat().reduced(1.0f);
        const bool active = b.getToggleState();
        const bool primary = b.getComponentID() == "auto" || b.getComponentID() == "lock";
        auto fill = active ? (b.getComponentID() == "lock" ? kWarm.darker(0.68f) : kGreen.darker(0.75f)) : kPanelAlt;
        if (primary && !active) fill = juce::Colour(0xff333339);
        if (hovered) fill = fill.brighter(0.10f);
        if (down) fill = fill.brighter(0.15f);
        g.setColour(fill);
        g.fillRoundedRectangle(r, 8.0f);
        g.setColour(active ? (b.getComponentID() == "lock" ? kWarm : kGreen).withAlpha(0.68f) : juce::Colours::white.withAlpha(0.09f));
        g.drawRoundedRectangle(r, 8.0f, 1.0f);
    }
    void drawButtonText(juce::Graphics& g, juce::TextButton& b, bool, bool) override {
        g.setColour(kText);
        g.setFont(getTextButtonFont(b, b.getHeight()));
        g.drawFittedText(b.getButtonText(), b.getLocalBounds().reduced(8, 4),
                         juce::Justification::centred, 1, 0.78f);
    }
};

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
        setLookAndFeel(&sculptLook);
        title.setText("SCULPT  /  AUTO TONE", juce::dontSendNotification);
        title.setFont(juce::Font(juce::FontOptions(27.f, juce::Font::bold)));
        title.setColour(juce::Label::textColourId, kText);
        subtitle.setText("LIVE NEURAL TONE FINDER     •     BETA 0.2", juce::dontSendNotification);
        subtitle.setFont(juce::Font(juce::FontOptions(11.f)));
        subtitle.setColour(juce::Label::textColourId, kWarm);
        status.setText("Collega la scheda audio e scegli una cartella NAM per cominciare.", juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, kMuted);
        status.setFont(juce::Font(juce::FontOptions(12.5f)));
        current.setText("NESSUN PROFILO", juce::dontSendNotification);
        current.setFont(juce::Font(juce::FontOptions(27.0f, juce::Font::bold)));
        current.setColour(juce::Label::textColourId, kText);
        current.setJustificationType(juce::Justification::centred);
        current.setMinimumHorizontalScale(0.65f);
        level.setColour(juce::Label::textColourId, kText);
        level.setFont(juce::Font(juce::FontOptions(21.f, juce::Font::bold)));
        info.setText("Selezione euristica, non riconoscimento dello stile musicale.", juce::dontSendNotification);
        info.setColour(juce::Label::textColourId, kMuted);
        info.setFont(juce::Font(juce::FontOptions(11.5f)));
        info.setJustificationType(juce::Justification::topLeft);

        for (auto* l : {&deviceLabel, &sampleLabel, &categoryLabel, &reasonLabel,
                         &signalLabel, &countLabel, &irLabel, &modeLabel}) {
            l->setColour(juce::Label::textColourId, kMuted);
            l->setFont(juce::Font(juce::FontOptions(12.0f)));
            addAndMakeVisible(l);
        }
        categoryLabel.setJustificationType(juce::Justification::centred);
        categoryLabel.setColour(juce::Label::textColourId, kWarm);
        categoryLabel.setFont(juce::Font(juce::FontOptions(13.f, juce::Font::bold)));
        reasonLabel.setJustificationType(juce::Justification::centred);
        reasonLabel.setFont(juce::Font(juce::FontOptions(13.f)));
        reasonLabel.setColour(juce::Label::textColourId, kText.withAlpha(0.8f));
        irLabel.setJustificationType(juce::Justification::centred);
        modeLabel.setJustificationType(juce::Justification::centred);
        modeLabel.setColour(juce::Label::textColourId, kGreen);
        signalLabel.setColour(juce::Label::textColourId, kGreen);
        deviceLabel.setText("Dispositivo: da selezionare",juce::dontSendNotification);
        sampleLabel.setText("Core Audio",juce::dontSendNotification);
        categoryLabel.setText("CARICA UNA CARTELLA DI PROFILI .NAM",juce::dontSendNotification);
        reasonLabel.setText("L'app analizza la dinamica del segnale in ingresso.",juce::dontSendNotification);
        countLabel.setText("0 MODELLI CARICATI",juce::dontSendNotification);
        irLabel.setText("Cabinet IR: nessuna",juce::dontSendNotification);

        audioButton.setButtonText("CONFIGURA AUDIO");
        folderButton.setButtonText("CARICA CARTELLA NAM");
        irButton.setButtonText("CARICA CABINET IR");
        lockButton.setButtonText("LOCK");
        autoButton.setButtonText("AUTO");
        autoButton.setComponentID("auto");
        lockButton.setComponentID("lock");
        likeButton.setButtonText("♥  MI PIACE");
        skipButton.setButtonText("PASSA OLTRE");
        for (auto* c : std::initializer_list<juce::Component*>{&title,&subtitle,&status,&current,&level,&info,
                 &audioButton,&folderButton,&irButton,&lockButton,&autoButton,&likeButton,&skipButton})
            addAndMakeVisible(c);
        for (auto* b : {&audioButton,&folderButton,&irButton,&lockButton,&autoButton,&likeButton,&skipButton}) {
            b->setColour(juce::TextButton::textColourOffId, kText);
        }
        for (int i=0;i<5;++i) {
            auto& label = suggestionNames[static_cast<size_t>(i)];
            label.setColour(juce::Label::textColourId, kText);
            label.setFont(juce::Font(juce::FontOptions(13.5f, juce::Font::bold)));
            label.setMinimumHorizontalScale(0.7f);
            addAndMakeVisible(label);
            auto& b = suggestionButtons[static_cast<size_t>(i)];
            b.setButtonText("PROVA");
            b.setColour(juce::TextButton::textColourOffId, kText);
            addAndMakeVisible(b);
            b.onClick = [this,i] {
                const int index = suggestedIndexes[static_cast<size_t>(i)];
                if (index < 0 || index >= static_cast<int>(captures.size())) return;
                automatic.store(false);
                locked.store(true);
                requestedIndex.store(index);
                refreshControls();
                status.setText("Profilo selezionato manualmente. Premi AUTO per riprendere la ricerca.",juce::dontSendNotification);
            };
        }
        audioButton.onClick = [this] { showAudioSettings(); };
        folderButton.onClick = [this] { chooseNAMFolder(); };
        irButton.onClick = [this] { chooseIR(); };
        lockButton.onClick = [this] { locked.store(!locked.load()); refreshControls(); };
        autoButton.onClick = [this] {
            automatic.store(!automatic.load());
            if (automatic.load()) locked.store(false);
            refreshControls();
        };
        likeButton.onClick = [this] { rate(+1); };
        skipButton.onClick = [this] { rate(-1); };
        for (auto& value : feedback) value.store(0);
        refreshControls();
        setSize(1200, 740);
        setAudioChannels(1, 2);
        startTimerHz(10);
        timerCallback();
    }

    ~AutoToneWindow() override {
        stopTimer(); shutdownAudio();
        setLookAndFeel(nullptr);
    }

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
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff202023), 0.f, 0.f, kBg, 0.f, 740.f, false));
        g.fillAll();
        // Branded header: analogue red accent and restrained studio hardware styling.
        g.setColour(kAccent);
        g.fillRoundedRectangle(juce::Rectangle<float>(24.f, 28.f, 5.f, 42.f), 2.f);
        g.setColour(kMuted.withAlpha(0.25f));
        g.drawLine(24.f, 94.f, 1176.f, 94.f, 1.f);
        auto panel = [&](float x, float y, float w, float h) {
            auto r=juce::Rectangle<float>(x,y,w,h);
            g.setColour(kPanel);
            g.fillRoundedRectangle(r, 13.f);
            g.setColour(juce::Colours::white.withAlpha(0.09f));
            g.drawRoundedRectangle(r, 13.f, 1.f);
        };
        auto caption = [&](const juce::String& t, float x, float y, float w) {
            g.setColour(kWarm);
            g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
            g.drawText(t, juce::Rectangle<float>(x,y,w,20),juce::Justification::centredLeft);
        };
        panel(24,112,250,548);
        panel(292,112,555,548);
        panel(865,112,310,548);
        caption("01  /  AUDIO & CONTROL",43,132,218);
        caption("02  /  CURRENT TONE",316,132,310);
        caption("03  /  NEXT MATCHES",886,132,262);

        g.setColour(kMuted.withAlpha(0.18f));
        g.drawLine(42.f,270.f,256.f,270.f,1.f);
        g.drawLine(42.f,412.f,256.f,412.f,1.f);
        caption("LIVE SIGNAL",43,285,210);
        caption("CONTROLS",43,420,210);
        caption("TONE PROFILE",316,185,310);

        // In real operation this meter follows the input level. A missing device is never shown as active.
        const float meterX=43.f, meterY=317.f, meterW=210.f, meterH=11.f;
        g.setColour(juce::Colour(0xff34383c));
        g.fillRoundedRectangle(juce::Rectangle<float>(meterX,meterY,meterW,meterH), 4.f);
        const float db=levelDb.load();
        const float amount=juce::jlimit(0.f,1.f,(db+65.f)/65.f);
        if (amount>0.002f) {
            g.setGradientFill(juce::ColourGradient(kGreen,meterX,meterY,kWarm,meterX+meterW,meterY,false));
            g.fillRoundedRectangle(juce::Rectangle<float>(meterX,meterY,amount*meterW,meterH),4.f);
        }
        g.setColour(kMuted);
        g.setFont(juce::Font(juce::FontOptions(10.5f)));
        g.drawText("-65",43,332,38,17,juce::Justification::centredLeft);
        g.drawText("0 dBFS",195,332,56,17,juce::Justification::centredRight);

        // Tone display: understated concentric instrumentation graphic.
        const float cx=569.f, cy=381.f;
        for (int i=3; i>=1; --i) {
            g.setColour(kAccent.withAlpha((4-i)*0.065f));
            g.fillEllipse(cx-24.f*i,cy-24.f*i,48.f*i,48.f*i);
        }
        for (int i=1; i<=3; ++i) {
            g.setColour(kWarm.withAlpha(0.15f));
            g.drawEllipse(cx-23.f*i,cy-23.f*i,46.f*i,46.f*i,1.f);
        }
        juce::Path trace;
        const float drive=analysisTarget.load();
        for (int i=0;i<=240;++i) {
            const float x=430.f + i*1.17f;
            const float envelope=std::pow(std::max(0.f,1.f-std::abs(i-120.f)/120.f),1.6f);
            const float y=cy+std::sin(i*0.16f)*envelope*(13.f+17.f*drive);
            if(i==0) trace.startNewSubPath(x,y); else trace.lineTo(x,y);
        }
        g.setColour(kWarm.withAlpha(0.91f));
        g.strokePath(trace,juce::PathStrokeType(2.f));
        g.setColour(kMuted.withAlpha(0.15f));
        g.drawLine(420.f,cy,720.f,cy,1.f);

        g.setColour(kPanelAlt);
        g.fillRoundedRectangle(juce::Rectangle<float>(314.f,495.f,510.f,108.f),10.f);
        caption("ANALISI DEL SEGNALE",329,505,210);
        g.setColour(kMuted);
        g.setFont(juce::Font(juce::FontOptions(11.f)));
        g.drawText("La scelta usa intensità e risposta alla distorsione. Non riconosce brani o generi.",
                    329,571,480,25,juce::Justification::centredLeft,true);

        for (int i=0;i<5;++i) {
            auto r=juce::Rectangle<float>(884.f,185.f+i*80.f,273.f,70.f);
            const bool chosen=suggestedIndexes[static_cast<size_t>(i)]==shownIndex.load()
                && shownIndex.load()>=0;
            g.setColour(chosen ? juce::Colour(0xff302b2c) : kPanelAlt);
            g.fillRoundedRectangle(r,9.f);
            g.setColour(chosen ? kAccent.withAlpha(0.7f) : juce::Colours::white.withAlpha(0.06f));
            g.drawRoundedRectangle(r,9.f,1.f);
            g.setColour(kMuted);
            g.setFont(juce::Font(juce::FontOptions(10.5f,juce::Font::bold)));
            const juce::String n=juce::String(i+1).paddedLeft('0',2);
            g.drawText(n,895,192+i*80,26,18,juce::Justification::centredLeft);
        }
        g.setColour(kMuted.withAlpha(0.30f));
        g.drawLine(24.f,674.f,1175.f,674.f,1.f);
        g.setColour(kMuted);
        g.setFont(juce::Font(juce::FontOptions(10.5f)));
        g.drawText("LOCAL NAM ENGINE   /   NESSUNA REGISTRAZIONE SU DISCO",916,687,260,24,juce::Justification::centredRight,true);
    }

    void resized() override {
        title.setBounds(43,22,625,40);
        subtitle.setBounds(44,65,670,21);
        audioButton.setBounds(43,172,211,43);
        deviceLabel.setBounds(43,221,211,23);
        sampleLabel.setBounds(43,246,211,20);
        level.setBounds(43,348,150,29);
        signalLabel.setBounds(43,382,210,24);
        autoButton.setBounds(43,450,101,46);
        lockButton.setBounds(153,450,101,46);
        folderButton.setBounds(43,512,211,41);
        irButton.setBounds(43,560,211,41);
        countLabel.setBounds(43,615,211,22);
        current.setBounds(322,221,494,60);
        categoryLabel.setBounds(327,280,484,32);
        irLabel.setBounds(327,454,484,28);
        reasonLabel.setBounds(329,532,480,32);
        modeLabel.setBounds(329,614,480,27);
        for (int i=0;i<5;++i) {
            const int y=199+i*80;
            suggestionNames[static_cast<size_t>(i)].setBounds(927,y,140,42);
            suggestionButtons[static_cast<size_t>(i)].setBounds(1080,y+8,66,31);
        }
        likeButton.setBounds(870,618,142,30);
        skipButton.setBounds(1020,618,142,30);
        status.setBounds(27,681,860,26);
        info.setBounds(43,637,214,24);
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
        auto* deviceBeforeShutdown = deviceManager.getCurrentAudioDevice();
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

    void refreshControls() {
        autoButton.setToggleState(automatic.load(), juce::dontSendNotification);
        lockButton.setToggleState(locked.load(), juce::dontSendNotification);
        modeLabel.setText(locked.load() ? "●  PROFILO BLOCCATO" :
                          automatic.load() ? "●  RICERCA AUTOMATICA ATTIVA" : "●  SELEZIONE MANUALE",
                          juce::dontSendNotification);
        modeLabel.setColour(juce::Label::textColourId,locked.load() ? kWarm : kGreen);
    }

    void timerCallback() override {
        const int selected=shownIndex.load();
        const bool hasModel = selected>=0 && selected<static_cast<int>(captures.size());
        if(hasModel) {
            const auto& e=captures[static_cast<size_t>(selected)];
            current.setText(e.name,juce::dontSendNotification);
            const auto character=e.character;
            categoryLabel.setText(character<0.31f ? "LOW DRIVE  /  CLEAN" :
                                  character<0.69f ? "MID DRIVE  /  CRUNCH" : "HIGH DRIVE  /  SATURATED",
                                  juce::dontSendNotification);
        } else {
            current.setText("NESSUN PROFILO",juce::dontSendNotification);
            categoryLabel.setText("CARICA UNA CARTELLA DI PROFILI .NAM",juce::dontSendNotification);
        }
        level.setText(juce::String(levelDb.load(),1)+" dBFS",juce::dontSendNotification);
        const float db = levelDb.load();
        signalLabel.setText(db > -65.f ? "●  SEGNALE IN INGRESSO" : "○  IN ATTESA DI SEGNALE",juce::dontSendNotification);
        signalLabel.setColour(juce::Label::textColourId,db > -65.f ? kGreen : kMuted);
        auto* device = deviceManager.getCurrentAudioDevice();
        deviceLabel.setText(device ? device->getName() : "Nessuna scheda selezionata",juce::dontSendNotification);
        sampleLabel.setText(juce::String(juce::roundToInt(sampleRateLive.load()))+" Hz   /   Core Audio",juce::dontSendNotification);
        countLabel.setText(juce::String(captures.size())+" PROFILI NAM IN LIBRERIA",juce::dontSendNotification);
        irLabel.setText(irName.isNotEmpty() ? "CABINET IR  /  "+irName : "CABINET IR  /  NON CARICATA",juce::dontSendNotification);
        reasonLabel.setText(hasModel ? "Intensità stimata: "+juce::String(juce::roundToInt(analysisTarget.load()*100.f))+
                                    "%  ·  Drive del profilo: "+juce::String(juce::roundToInt(captures[static_cast<size_t>(selected)].character*100.f))+"%" :
                                     "In attesa di una libreria di modelli...",juce::dontSendNotification);
        std::vector<int> ranked;
        ranked.reserve(captures.size());
        for (int i=0;i<static_cast<int>(captures.size());++i) ranked.push_back(i);
        const float target=analysisTarget.load();
        auto metric=[&](int i) {
            return std::abs(captures[static_cast<size_t>(i)].character-target) -
                0.055f*juce::jlimit(-3,3,feedback[static_cast<size_t>(i)].load());
        };
        std::sort(ranked.begin(),ranked.end(),[&](int a,int b) { return metric(a)<metric(b); });
        for (int i=0;i<5;++i) {
            const bool visible=i<static_cast<int>(ranked.size());
            suggestedIndexes[static_cast<size_t>(i)]=visible?ranked[static_cast<size_t>(i)]:-1;
            auto& l=suggestionNames[static_cast<size_t>(i)];
            auto& b=suggestionButtons[static_cast<size_t>(i)];
            l.setVisible(visible); b.setVisible(visible);
            if (visible) l.setText(captures[static_cast<size_t>(ranked[static_cast<size_t>(i)])].name,juce::dontSendNotification);
        }
        repaint();
    }

    SculptLook sculptLook;
    juce::Label title, subtitle, status, current, level, info;
    juce::Label deviceLabel, sampleLabel, categoryLabel, reasonLabel, signalLabel, countLabel, irLabel, modeLabel;
    std::array<juce::Label,5> suggestionNames;
    std::array<juce::TextButton,5> suggestionButtons;
    std::array<int,5> suggestedIndexes{{-1,-1,-1,-1,-1}};
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
    const juce::String getApplicationVersion() override { return "0.2.0-beta-ui"; }
    bool moreThanOneInstanceAllowed() override { return false; }
    void initialise(const juce::String&) override {
        mainWindow = std::make_unique<SculptMainWindow>();
        mainWindow->setUsingNativeTitleBar(true);
        mainWindow->setContentOwned(new AutoToneWindow(), true);
        mainWindow->centreWithSize(1200,740);
        mainWindow->setResizable(false,false);
        mainWindow->setVisible(true);
    }
    void shutdown() override { mainWindow.reset(); }
    void systemRequestedQuit() override { quit(); }
private:
    std::unique_ptr<SculptMainWindow> mainWindow;
};
START_JUCE_APPLICATION(SculptApp)
