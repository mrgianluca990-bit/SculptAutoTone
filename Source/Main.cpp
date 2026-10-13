#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <NeuralAudio/NeuralModel.h>
#include "VstChain.h"

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
        auto fill = active ? juce::Colour(0xff34483e) : kPanelAlt;
        if (hovered) fill = fill.brighter(0.10f);
        if (down) fill = fill.brighter(0.15f);
        g.setColour(fill);
        g.fillRoundedRectangle(r, 8.0f);
        g.setColour(active ? kGreen.withAlpha(0.68f) : juce::Colours::white.withAlpha(0.09f));
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
    float recommendedInput = 0.0f;
    float recommendedOutput = 0.0f;
};

float gainFromDb(float db) { return std::pow(10.0f, db / 20.0f); }
}

class GuitarRigWindow final : public juce::AudioAppComponent, private juce::Timer {
public:
    GuitarRigWindow() {
        setOpaque(true);
        setLookAndFeel(&sculptLook);
        title.setText("SCULPT  /  GUITAR RIG", juce::dontSendNotification);
        title.setFont(juce::Font(juce::FontOptions(27.f, juce::Font::bold)));
        title.setColour(juce::Label::textColourId, kText);
        subtitle.setText("NAM  /  VST3  /  CAB IR  /  SCULPT CH     •     BETA 0.7", juce::dontSendNotification);
        subtitle.setFont(juce::Font(juce::FontOptions(11.f)));
        subtitle.setColour(juce::Label::textColourId, kWarm);
        status.setText("Collega la scheda audio, configura VST3 e scegli liberamente una cattura NAM.", juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, kMuted);
        status.setFont(juce::Font(juce::FontOptions(12.5f)));
        current.setText("NESSUN PROFILO", juce::dontSendNotification);
        current.setFont(juce::Font(juce::FontOptions(27.0f, juce::Font::bold)));
        current.setColour(juce::Label::textColourId, kText);
        current.setJustificationType(juce::Justification::centred);
        current.setMinimumHorizontalScale(0.65f);
        level.setColour(juce::Label::textColourId, kText);
        level.setFont(juce::Font(juce::FontOptions(21.f, juce::Font::bold)));
        info.setText("Rig completamente manuale. Sei tu a scegliere i NAM e i VST3.", juce::dontSendNotification);
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
        categoryLabel.setText("CARICA UN PROFILO O UNA CARTELLA .NAM",juce::dontSendNotification);
        reasonLabel.setText("Scegli un NAM dalla libreria oppure usa il segnale DI.",juce::dontSendNotification);
        countLabel.setText("0 MODELLI CARICATI",juce::dontSendNotification);
        irLabel.setText("Cabinet IR: nessuna",juce::dontSendNotification);

        // Master output: UI writes an atomic dB target; audio applies a smoothed gain
        // after NAM, cabinet IR, Sculpt Channel and the POST VST3 slots.
        masterLabel.setText("0.0 dB", juce::dontSendNotification);
        masterLabel.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        masterLabel.setColour(juce::Label::textColourId, kText);
        masterLabel.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(masterLabel);
        masterSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        masterSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        masterSlider.setRange(-60.0, 6.0, 0.5);
        masterSlider.setDoubleClickReturnValue(true, 0.0);
        masterSlider.setColour(juce::Slider::trackColourId, kAccent);
        masterSlider.setColour(juce::Slider::backgroundColourId, kPanelAlt);
        masterSlider.setColour(juce::Slider::thumbColourId, kWarm);
        masterSlider.setTooltip("Volume dopo tutti gli insert VST3. Da -60 dB (mute) a +6 dB. Doppio clic: 0 dB.");
        masterSlider.onValueChange = [this] {
            const float db = static_cast<float>(masterSlider.getValue());
            masterGainDb.store(db, std::memory_order_relaxed);
            masterLabel.setText(db <= -60.f ? "MUTE" :
                                juce::String(db > 0.f ? "+" : "") + juce::String(db, 1) + " dB",
                                juce::dontSendNotification);
        };
        addAndMakeVisible(masterSlider);
        masterSlider.setValue(0.0, juce::dontSendNotification);

        // TONE3000 integration: direct search via the official website in the user's
        // browser, without scraping, API secrets, or unsupported catalog endpoints.
        // Future in-app results/downloads require TONE3000 OAuth PKCE approval.
        tone3000Brand.setText("TONE3000  /  CERCA CATTURE NAM ONLINE", juce::dontSendNotification);
        tone3000Brand.setColour(juce::Label::textColourId, kWarm);
        tone3000Brand.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        tone3000Brand.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(tone3000Brand);

        tone3000Query.setTextToShowWhenEmpty("Cerca: Plexi, JCM800, Fender...", kMuted);
        tone3000Query.setFont(juce::Font(juce::FontOptions(13.f)));
        tone3000Query.setColour(juce::TextEditor::backgroundColourId, kPanelAlt);
        tone3000Query.setColour(juce::TextEditor::textColourId, kText);
        tone3000Query.setColour(juce::TextEditor::outlineColourId, kMuted.withAlpha(0.25f));
        tone3000Query.setColour(juce::TextEditor::focusedOutlineColourId, kAccent);
        tone3000Query.setColour(juce::CaretComponent::caretColourId, kWarm);
        tone3000Query.setReturnKeyStartsNewLine(false);
        tone3000Query.onReturnKey = [this] { openTone3000Search(); };
        tone3000Query.setTooltip("Cerca sul catalogo ufficiale TONE3000; i risultati si aprono nel browser.");
        addAndMakeVisible(tone3000Query);
        tone3000Search.setButtonText("CERCA ONLINE  ↗");
        tone3000Search.setTooltip("Mostra i risultati su tone3000.com. Nessuna chiave API richiesta.");
        tone3000Search.onClick = [this] { openTone3000Search(); };
        tone3000Search.setColour(juce::TextButton::textColourOffId, kText);
        addAndMakeVisible(tone3000Search);
        rigButton.setButtonText("APRI RIG  /  VST3 PRE · SCULPT CH · POST");
        rigButton.setTooltip("Nove insert VST3: quattro pre NAM, Sculpt Channel dopo IR, quattro post IR.");
        rigButton.setColour(juce::TextButton::textColourOffId, kText);
        rigButton.onClick = [this] { showRigPanel(); };
        addAndMakeVisible(rigButton);
        audioButton.setButtonText("CONFIGURA AUDIO");
        folderButton.setButtonText("CARICA NAM / CARTELLA");
        irButton.setButtonText("CARICA CABINET IR");
        previousNamButton.setButtonText("◀ NAM");
        nextNamButton.setButtonText("NAM ▶");
        previousPageButton.setButtonText("◀ PAGINA");
        nextPageButton.setButtonText("PAGINA ▶");
        for (auto* c : std::initializer_list<juce::Component*>{&title,&subtitle,&status,&current,&level,&info,
                 &audioButton,&folderButton,&irButton,&previousNamButton,&nextNamButton,&previousPageButton,&nextPageButton})
            addAndMakeVisible(c);
        for (auto* b : {&audioButton,&folderButton,&irButton,&previousNamButton,&nextNamButton,&previousPageButton,&nextPageButton})
            b->setColour(juce::TextButton::textColourOffId, kText);

        // Explicit NAM selection is the only way to change captures; no background ranking.
        namSelector.setTextWhenNothingSelected("Carica una libreria NAM");
        namSelector.setColour(juce::ComboBox::backgroundColourId, kPanelAlt);
        namSelector.setColour(juce::ComboBox::textColourId, kText);
        namSelector.setColour(juce::ComboBox::outlineColourId, kAccent.withAlpha(0.50f));
        namSelector.setColour(juce::ComboBox::arrowColourId, kWarm);
        namSelector.onChange = [this] {
            const int id=namSelector.getSelectedId();
            if (id > 0) selectNam(id-2);  // id 1 = bypass; 2+ = loaded captures
        };
        addAndMakeVisible(namSelector);
        namSelector.addItem("BYPASS NAM / SEGNALE DI",1);
        namSelector.setSelectedId(1,juce::dontSendNotification);

        for (int i=0;i<5;++i) {
            auto& label=libraryNames[static_cast<size_t>(i)];
            label.setColour(juce::Label::textColourId,kText);
            label.setFont(juce::Font(juce::FontOptions(13.5f, juce::Font::bold)));
            label.setMinimumHorizontalScale(0.7f);
            addAndMakeVisible(label);
            auto& b=libraryButtons[static_cast<size_t>(i)];
            b.setButtonText("USA");
            b.setColour(juce::TextButton::textColourOffId,kText);
            addAndMakeVisible(b);
            b.onClick=[this,i] {
                const int index=libraryIndexes[static_cast<size_t>(i)];
                if (index >= 0) selectNam(index);
            };
        }
        audioButton.onClick = [this] { showAudioSettings(); };
        folderButton.onClick = [this] { chooseNAMFolder(); };
        folderButton.setTooltip("Scegli un profilo .nam oppure una cartella di catture.");
        irButton.onClick = [this] { chooseIR(); };
        previousNamButton.onClick=[this] { changeNam(-1); };
        nextNamButton.onClick=[this] { changeNam(1); };
        previousPageButton.onClick=[this] { libraryPage=juce::jmax(0,libraryPage-5);timerCallback(); };
        nextPageButton.onClick=[this] {
            if (libraryPage+5 < static_cast<int>(captures.size())) libraryPage+=5;
            timerCallback();
        };
        modeLabel.setText("●  SELEZIONE NAM MANUALE",juce::dontSendNotification);
        setSize(1200, 740);
        setAudioChannels(1, 2);
        startTimerHz(10);
        timerCallback();
    }

    ~GuitarRigWindow() override {
        stopTimer();
        activePluginWindow.reset();
        shutdownAudio();
        setLookAndFeel(nullptr);
    }

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override {
        const auto maxBuffer = std::max(kBlock, samplesPerBlockExpected);
        (void) maxBuffer; // engine processes in 2048-sample chunks
        inputScratch.fill(0);
        outA.fill(0); outB.fill(0);
        sampleRateLive.store(sampleRate);
        masterGain.reset(sampleRate, 0.03); // 30 ms gain smoothing; audio thread only
        const float initialDb = masterGainDb.load(std::memory_order_relaxed);
        masterGain.setCurrentAndTargetValue(initialDb <= -60.f ? 0.f : gainFromDb(initialDb));
        cab.prepare({ sampleRate, static_cast<juce::uint32>(kBlock), 1 });
        cab.reset();
        vstChain.prepare(sampleRate, kBlock);
        gateEnvelope = 0.f;
        gateGain = 0.f;
        const int chosen=requestedIndex.load(std::memory_order_relaxed);
        activeIndex=(chosen>=0 && chosen<static_cast<int>(captures.size())) ? chosen : -1;
        fadingFrom = -1;
        fadeRemaining = 0;
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override {
        auto* audio = bufferToFill.buffer;
        if (audio == nullptr || audio->getNumChannels() == 0) return;
        const int start = bufferToFill.startSample;
        const int count = bufferToFill.numSamples;
        const int numChannels = audio->getNumChannels();
        int offset = 0;
        while (offset < count) {
            const int n = std::min(kBlock, count - offset);
            const float* input = audio->getReadPointer(0, start + offset);
            // Preserve dry DI for input metering. Tone choice is always manual.
            std::copy_n(input, n, dryInput.begin());
            const float threshold = gainFromDb(gateThresholdDb.load(std::memory_order_relaxed));
            const bool doGate = gateEnabled.load(std::memory_order_relaxed);
            const float attackCoeff = 0.08f;
            const float releaseCoeff = 0.0009f;
            for (int j=0;j<n;++j) {
                const float x=dryInput[static_cast<size_t>(j)];
                const float magnitude=std::abs(x);
                gateEnvelope += (magnitude > gateEnvelope ? attackCoeff : releaseCoeff) * (magnitude-gateEnvelope);
                const float goal = (!doGate || gateEnvelope >= threshold) ? 1.f : 0.f;
                gateGain += (goal-gateGain) * 0.008f;
                const float y = x * gateGain;
                fxBuffer.setSample(0,j,y);
                fxBuffer.setSample(1,j,y);
            }
            // PRE: four independent VST3 inserts. The NAM input is collapsed to mono
            // after PRE because Neural Amp Modeler expects a single DI channel.
            juce::AudioBuffer<float> vstView(fxBuffer.getArrayOfWritePointers(), 2, n);
            for (int si=0;si<4;++si) vstChain.process(si, vstView);
            const auto* preL=fxBuffer.getReadPointer(0);
            const auto* preR=fxBuffer.getReadPointer(1);
            for (int j=0;j<n;++j)
                inputScratch[static_cast<size_t>(j)] = 0.5f*(preL[j]+preR[j]);
            const int desired = requestedIndex.load();
            if (desired >= -1 && desired < static_cast<int>(captures.size()) && desired != activeIndex && fadeRemaining <= 0) {
                fadingFrom = activeIndex;
                activeIndex = desired;
                fadeRemaining = 2048; // approx 43ms @48 kHz
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
            if (fadeRemaining > 0) {
                float oldGain=1.0f;
                if (fadingFrom >= 0) {
                    auto& oldEntry = captures[static_cast<size_t>(fadingFrom)];
                    const float gi = gainFromDb(juce::jlimit(-18.f,18.f,oldEntry.recommendedInput));
                    oldGain = 0.25f * gainFromDb(juce::jlimit(-18.f,18.f,oldEntry.recommendedOutput));
                    for (int i=0;i<n;++i) driven[static_cast<size_t>(i)] = inputScratch[static_cast<size_t>(i)] * gi;
                    oldEntry.model->Process(driven.data(), outB.data(), static_cast<size_t>(n));
                } else {
                    // The old signal was clean DI; crossfade it when enabling a NAM.
                    for (int i=0;i<n;++i) outB[static_cast<size_t>(i)] = inputScratch[static_cast<size_t>(i)] * 0.25f;
                }
                for (int i=0;i<n;++i) {
                    const float a = 1.0f - juce::jlimit(0.f,1.f,float(fadeRemaining-i)/2048.0f);
                    outA[static_cast<size_t>(i)] = outB[static_cast<size_t>(i)] * oldGain * (1.f-a) + outA[static_cast<size_t>(i)]*a;
                }
                fadeRemaining -= n;
                if (fadeRemaining <= 0) fadingFrom=-1;
            }
            if (irLoaded.load()) {
                cabBuffer.copyFrom(0,0,outA.data(),n);
                juce::dsp::AudioBlock<float> block(cabBuffer);
                auto slice = block.getSubBlock(0,static_cast<size_t>(n));
                juce::dsp::ProcessContextReplacing<float> context(slice);
                cab.process(context);
                std::copy_n(cabBuffer.getReadPointer(0), n, outA.begin());
            } else if (activeIndex >= 0) {
                // Simple lowpass for a loaded amp without a cabinet IR; not a cabinet simulation.
                const float pole = std::exp(-2.0f * juce::MathConstants<float>::pi * 5500.f /
                                static_cast<float>(std::max(8000.0, sampleRateLive.load())));
                for (int i=0;i<n;++i) {
                    lowpassZ = (1-pole)*outA[static_cast<size_t>(i)] + pole*lowpassZ;
                    outA[static_cast<size_t>(i)] = lowpassZ;
                }
            }
            // Dedicated SCULPT CHANNEL after cabinet IR, then four independent POST VST3.
            fxBuffer.copyFrom(0, 0, outA.data(), n);
            fxBuffer.copyFrom(1, 0, outA.data(), n);
            vstChain.process(4, vstView);
            for (int si=5;si<9;++si) vstChain.process(si, vstView);
            // Final master gain: after all nine VST3 inserts; preserve stereo post effects.
            const float masterDb = masterGainDb.load(std::memory_order_relaxed);
            masterGain.setTargetValue(masterDb <= -60.f ? 0.f : gainFromDb(masterDb));
            auto* left = audio->getWritePointer(0, start + offset);
            auto* right = numChannels > 1 ? audio->getWritePointer(1, start + offset) : nullptr;
            const auto* processedL = fxBuffer.getReadPointer(0);
            const auto* processedR = fxBuffer.getReadPointer(1);
            for (int i=0;i<n;++i) {
                const float gain = masterGain.getNextValue();
                left[i] = juce::jlimit(-0.85f, 0.85f, processedL[i] * gain);
                if (right != nullptr) right[i] = juce::jlimit(-0.85f,0.85f,processedR[i]*gain);
            }
            for (int ch=2;ch<numChannels;++ch) audio->copyFrom(ch,start+offset,*audio,0,start+offset,n);
            // Level meter only: no dynamics-to-tone inference and no automatic switching.
            double energy=0.0;
            for (int i=0;i<n;++i) {
                const double x=dryInput[static_cast<size_t>(i)];
                energy+=x*x;
            }
            levelDb.store(static_cast<float>(20.0*std::log10(
                std::max(1.e-8,std::sqrt(energy/std::max(1,n))))));
            offset += n;
        }
    }

    void releaseResources() override { vstChain.release(); cab.reset(); }

    void paint(juce::Graphics& g) override {
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff202023), 0.f, 0.f, kBg, 0.f, 740.f, false));
        g.fillAll();
        // Branded header: analogue red accent and restrained studio hardware styling.
        g.setColour(kAccent);
        g.fillRoundedRectangle(juce::Rectangle<float>(24.f, 28.f, 5.f, 42.f), 2.f);
        g.setColour(kPanel);
        g.fillRoundedRectangle(juce::Rectangle<float>(707.f, 11.f, 468.f, 78.f), 10.f);
        g.setColour(kAccent.withAlpha(0.45f));
        g.drawRoundedRectangle(juce::Rectangle<float>(707.f, 11.f, 468.f, 78.f), 10.f, 1.f);
        g.setColour(kMuted.withAlpha(0.25f));
        g.drawLine(24.f, 99.f, 1176.f, 99.f, 1.f);
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
        caption("02  /  NAM / CABINET",316,132,310);
        caption("03  /  NAM LIBRARY",886,132,262);

        g.setColour(kMuted.withAlpha(0.18f));
        g.drawLine(42.f,270.f,256.f,270.f,1.f);
        g.drawLine(42.f,398.f,256.f,398.f,1.f);
        g.drawLine(42.f,476.f,256.f,476.f,1.f);
        caption("LIVE SIGNAL",43,285,210);
        caption("MASTER OUTPUT",43,407,148);
        caption("CONTROLS",43,484,210);
        caption("NAM CAPTURE  /  SCELTA MANUALE",316,185,340);

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
        const float drive=0.46f; // Decorative trace only, never drives model selection.
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
        caption("SIGNAL CHAIN",329,505,210);
        g.setColour(kMuted);
        g.setFont(juce::Font(juce::FontOptions(11.f)));
        g.drawText("INPUT  >  GATE  >  PRE VST3  >  NAM  >  IR  >  SCULPT CH  >  POST  >  OUTPUT",
                    329,571,480,25,juce::Justification::centredLeft,true);

        for (int i=0;i<5;++i) {
            auto r=juce::Rectangle<float>(884.f,185.f+i*80.f,273.f,70.f);
            const bool chosen=libraryIndexes[static_cast<size_t>(i)]==requestedIndex.load()
                && requestedIndex.load()>=0;
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
        g.drawText("MANUAL GUITAR RIG  /  VST3  /  NAM",903,687,275,24,juce::Justification::centredRight,true);
    }

    void resized() override {
        title.setBounds(43,22,635,40);
        subtitle.setBounds(44,65,655,21);
        tone3000Brand.setBounds(723,14,431,23);
        tone3000Query.setBounds(722,43,285,33);
        tone3000Search.setBounds(1016,43,145,33);
        audioButton.setBounds(43,172,211,43);
        deviceLabel.setBounds(43,221,211,23);
        sampleLabel.setBounds(43,246,211,20);
        level.setBounds(43,346,150,27);
        signalLabel.setBounds(43,375,210,22);
        masterLabel.setBounds(170,404,84,22);
        masterSlider.setBounds(40,430,216,34);
        previousNamButton.setBounds(43,508,101,37);
        nextNamButton.setBounds(153,508,101,37);
        folderButton.setBounds(43,554,211,35);
        irButton.setBounds(43,597,211,35);
        countLabel.setBounds(43,635,211,20);
        current.setBounds(322,221,494,60);
        categoryLabel.setBounds(327,276,484,29);
        namSelector.setBounds(331,315,476,34);
        irLabel.setBounds(327,454,484,28);
        reasonLabel.setBounds(329,532,480,32);
        modeLabel.setBounds(329,614,190,27);
        rigButton.setBounds(532,612,286,35);
        for (int i=0;i<5;++i) {
            const int y=199+i*80;
            libraryNames[static_cast<size_t>(i)].setBounds(927,y,140,42);
            libraryButtons[static_cast<size_t>(i)].setBounds(1080,y+8,66,31);
        }
        previousPageButton.setBounds(870,618,142,30);
        nextPageButton.setBounds(1020,618,142,30);
        status.setBounds(27,681,860,26);
        info.setBounds(0,0,0,0); // already explained in the central analysis panel
    }

private:
    class HostedEditorWindow final : public juce::DocumentWindow {
    public:
        explicit HostedEditorWindow(juce::AudioPluginInstance& processor)
            : juce::DocumentWindow(processor.getName(), kBg, juce::DocumentWindow::closeButton) {
            juce::AudioProcessorEditor* editor = processor.createEditorIfNeeded();
            if (editor == nullptr) editor = new juce::GenericAudioProcessorEditor(processor);
            setContentOwned(editor, true);
            setUsingNativeTitleBar(true);
            setResizable(false,false);
            centreWithSize(juce::jmax(450,editor->getWidth()), juce::jmax(280,editor->getHeight()));
        }
        void closeButtonPressed() override { setVisible(false); }
    };

    class RigPanel final : public juce::Component, private juce::Timer {
    public:
        explicit RigPanel(GuitarRigWindow& parent) : owner(parent) {
            setOpaque(true);
            gateLabel.setText("INPUT NOISE GATE  /  FISSO, PRIMA DEI VST3",juce::dontSendNotification);
            gateLabel.setColour(juce::Label::textColourId,kWarm);
            gateLabel.setFont(juce::Font(juce::FontOptions(14.f,juce::Font::bold)));
            addAndMakeVisible(gateLabel);
            gateBypass.setButtonText("GATE ON");
            gateBypass.onClick=[this]{owner.gateEnabled.store(!owner.gateEnabled.load());updateLabels();};
            addAndMakeVisible(gateBypass);
            threshold.setSliderStyle(juce::Slider::LinearHorizontal);
            threshold.setTextBoxStyle(juce::Slider::TextBoxRight,false,75,23);
            threshold.setRange(-85.,-15.,1.);
            threshold.setValue(owner.gateThresholdDb.load(),juce::dontSendNotification);
            threshold.setTextValueSuffix(" dB");
            threshold.onValueChange=[this]{owner.gateThresholdDb.store(static_cast<float>(threshold.getValue()));};
            addAndMakeVisible(threshold);
            caption.setText("PRE 1–4  →  NAM  →  IR  →  SCULPT CHANNEL  →  POST 1–4  →  MASTER",juce::dontSendNotification);
            caption.setColour(juce::Label::textColourId,kMuted);
            caption.setJustificationType(juce::Justification::centred);
            caption.setFont(juce::Font(juce::FontOptions(13.f,juce::Font::bold)));
            addAndMakeVisible(caption);
            for (int i=0;i<9;++i) {
                auto& label=labels[static_cast<size_t>(i)];
                label.setColour(juce::Label::textColourId,kText);
                label.setFont(juce::Font(juce::FontOptions(12.7f,juce::Font::bold)));
                label.setMinimumHorizontalScale(0.7f);
                addAndMakeVisible(label);
                auto& load=loads[static_cast<size_t>(i)];
                load.setButtonText(i==4 ? "SCULPT CH" : "CARICA VST3");
                load.onClick=[this,i]{owner.showPluginMenu(i);};
                addAndMakeVisible(load);
                auto& edit=editors[static_cast<size_t>(i)];
                edit.setButtonText("EDIT");
                edit.onClick=[this,i]{owner.openPluginEditor(i);};
                addAndMakeVisible(edit);
                auto& bypass=bypasses[static_cast<size_t>(i)];
                bypass.onClick=[this,i]{owner.vstChain.enable(i,!owner.vstChain.enabled(i));updateLabels();};
                addAndMakeVisible(bypass);
                auto& remove=removes[static_cast<size_t>(i)];
                remove.setButtonText("X");
                remove.onClick=[this,i]{owner.removeVstSlot(i);updateLabels();};
                addAndMakeVisible(remove);
            }
            hint.setText("Installa i VST3 sul Mac. CARICA apre la lista locale; EDIT apre la GUI originale del plugin. Le modifiche agli slot interrompono brevemente l'audio.",juce::dontSendNotification);
            hint.setColour(juce::Label::textColourId,kMuted);
            hint.setFont(juce::Font(juce::FontOptions(11.5f)));
            hint.setJustificationType(juce::Justification::centred);
            addAndMakeVisible(hint);
            setSize(1100,750);
            startTimerHz(2);
            updateLabels();
        }
        ~RigPanel() override { stopTimer(); }
        void paint(juce::Graphics& g) override {
            g.fillAll(kBg);
            auto drawPanel=[&](int x,int y,int w,int h,const juce::String& text){
                g.setColour(kPanel); g.fillRoundedRectangle(juce::Rectangle<float>(x,y,w,h),11.f);
                g.setColour(kAccent.withAlpha(.42f));g.drawRoundedRectangle(juce::Rectangle<float>(x,y,w,h),11.f,1.f);
                g.setColour(kWarm);g.setFont(juce::Font(juce::FontOptions(14.f,juce::Font::bold)));
                g.drawText(text,x+18,y+13,w-36,22,juce::Justification::centredLeft);
            };
            drawPanel(16,16,1068,92,"01 / INPUT");
            drawPanel(16,148,520,403,"02 / PRE NAM   ·   4 VST3");
            drawPanel(549,148,535,403,"04 / POST NAM / POST SCULPT CH   ·   4 VST3");
            drawPanel(16,569,1068,106,"03 / SCULPT CHANNEL   ·   DOPO NAM + IR");
            g.setColour(kMuted);g.setFont(juce::Font(juce::FontOptions(12.f)));
            g.drawText("NAM + CABINET IR sono gestiti dalla finestra principale.",20,111,1060,28,juce::Justification::centred);
        }
        void resized() override {
            gateLabel.setBounds(40,54,450,26);
            gateBypass.setBounds(505,54,114,28);
            threshold.setBounds(638,51,415,34);
            caption.setBounds(20,112,1060,30);
            for(int i=0;i<9;++i){
                int x= i<4 ? 32 : (i==4 ? 32 : 565);
                int w= i==4 ? 1028 : (i<4 ? 490 : 503);
                int y= i<4 ? 200+i*86 : (i==4 ? 607 : 200+(i-5)*86);
                labels[static_cast<size_t>(i)].setBounds(x+8,y,w/2-8,31);
                const int right=x+w;
                loads[static_cast<size_t>(i)].setBounds(right-298,y,111,31);
                editors[static_cast<size_t>(i)].setBounds(right-181,y,55,31);
                bypasses[static_cast<size_t>(i)].setBounds(right-121,y,82,31);
                removes[static_cast<size_t>(i)].setBounds(right-34,y,32,31);
            }
            hint.setBounds(24,689,1050,35);
        }
    private:
        void timerCallback() override {updateLabels();}
        void updateLabels(){
            gateBypass.setButtonText(owner.gateEnabled.load()?"GATE ON":"GATE OFF");
            for(int i=0;i<9;++i){
                const auto name=owner.vstChain.name(i);
                const juce::String prefix= i<4 ? "PRE "+juce::String(i+1) : (i==4 ? "SCULPT CH" : "POST "+juce::String(i-4));
                auto text=prefix+"  /  "+(name.isNotEmpty()?name:juce::String("VUOTO"));
                labels[static_cast<size_t>(i)].setText(text,juce::dontSendNotification);
                editors[static_cast<size_t>(i)].setEnabled(name.isNotEmpty());
                bypasses[static_cast<size_t>(i)].setEnabled(name.isNotEmpty());
                removes[static_cast<size_t>(i)].setEnabled(name.isNotEmpty());
                bypasses[static_cast<size_t>(i)].setButtonText(owner.vstChain.enabled(i)?"ON":"BYPASS");
            }
        }
        GuitarRigWindow& owner;
        juce::Label gateLabel,caption,hint;
        juce::TextButton gateBypass;
        juce::Slider threshold;
        std::array<juce::Label,9> labels;
        std::array<juce::TextButton,9> loads,editors,bypasses,removes;
    };

    void showRigPanel() {
        juce::DialogWindow::LaunchOptions opt;
        opt.content.setOwned(new RigPanel(*this));
        opt.dialogTitle = "SCULPT GUITAR RIG  /  VST3 FX";
        opt.dialogBackgroundColour = kBg;
        opt.useNativeTitleBar = true;
        opt.escapeKeyTriggersCloseButton = true;
        opt.resizable = false;
        opt.launchAsync();
    }

    void showPluginMenu(int slot) {
        juce::PopupMenu menu;
        const auto paths=VstChain::installedVst3();
        for(int i=0;i<paths.size();++i) {
            const auto& file=paths.getReference(i);
            if(slot==VstChain::sculptSlot && !file.getFileNameWithoutExtension().toLowerCase().contains("sculpt")) continue;
            menu.addItem(i+1,file.getFileNameWithoutExtension());
        }
        if (paths.isEmpty()) menu.addItem(99998,"Nessun plugin VST3 trovato",false);
        menu.addSeparator();
        menu.addItem(99999,"SCEGLI UN FILE .VST3... ");
        menu.showMenuAsync(juce::PopupMenu::Options(),
            [safe=juce::Component::SafePointer<GuitarRigWindow>(this),slot,paths](int id) {
                if (safe==nullptr || id==0) return;
                if(id==99999) safe->browsePluginBundle(slot);
                else if(id>=1 && id<=paths.size()) safe->loadPluginSlot(slot,paths.getReference(id-1));
            });
    }

    void browsePluginBundle(int slot) {
        auto home=juce::File("/Library/Audio/Plug-Ins/VST3");
        if (!home.isDirectory()) home=juce::File::getSpecialLocation(juce::File::userHomeDirectory);
        vstChooser=std::make_unique<juce::FileChooser>("Scegli plugin VST3",home,"*.vst3");
        vstChooser->launchAsync(juce::FileBrowserComponent::openMode |
             juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectDirectories,
             [safe=juce::Component::SafePointer<GuitarRigWindow>(this),slot](const juce::FileChooser& chooser){
                 if(safe!=nullptr && chooser.getResult().exists()) safe->loadPluginSlot(slot,chooser.getResult());
             });
    }

    void loadPluginSlot(int slot, const juce::File& bundle) {
        if(activePluginEditorSlot==slot){activePluginWindow.reset();activePluginEditorSlot=-1;}
        auto* device=deviceManager.getCurrentAudioDevice();
        const double sr=device?device->getCurrentSampleRate():sampleRateLive.load();
        shutdownAudio(); // Plugin binaries must never be swapped during audio callbacks.
        juce::String error;
        const bool loaded=vstChain.load(slot,bundle,sr,kBlock,error);
        setAudioChannels(1,2);
        if(loaded) status.setText("VST3 caricato: "+vstChain.name(slot),juce::dontSendNotification);
        else status.setText("VST3: "+error,juce::dontSendNotification);
    }
    void removeVstSlot(int slot) {
        if(activePluginEditorSlot==slot){activePluginWindow.reset();activePluginEditorSlot=-1;}
        shutdownAudio();
        vstChain.remove(slot);
        setAudioChannels(1,2);
        status.setText("Slot VST3 svuotato.",juce::dontSendNotification);
    }
    void openPluginEditor(int slot) {
        auto* plugin=vstChain.get(slot);
        if(plugin==nullptr) return;
        activePluginWindow.reset();
        activePluginWindow=std::make_unique<HostedEditorWindow>(*plugin);
        activePluginEditorSlot=slot;
        activePluginWindow->setVisible(true);
        activePluginWindow->toFront(true);
    }

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

    // User-initiated online search; authoritative TONE3000 result pages handle login,
    // attribution, creator licenses and downloads. No third-party catalog is scraped.
    void openTone3000Search() {
        const auto query = tone3000Query.getText().trim();
        auto url = juce::URL("https://www.tone3000.com/search");
        if (query.isNotEmpty())
            url = url.withParameter("q", query);
        if (url.launchInDefaultBrowser())
            status.setText("TONE3000 aperto nel browser. Scarica il NAM, estrailo e premi IMPORTA NAM SCARICATI.",
                           juce::dontSendNotification);
        else
            status.setText("Impossibile aprire il browser. Visita www.tone3000.com/search.",
                           juce::dontSendNotification);
    }

    void chooseNAMFolder() {
        auto downloads = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("Downloads");
        if (! downloads.isDirectory())
            downloads = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        fileChooser = std::make_unique<juce::FileChooser>("Scegli un .nam o una cartella di .nam", downloads, "*.nam");
        fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories
                                 | juce::FileBrowserComponent::canSelectFiles,
                                [this](const juce::FileChooser& chooser) {
            const auto folder = chooser.getResult();
            if (folder.isDirectory() || (folder.existsAsFile() && folder.hasFileExtension(".nam"))) {
                status.setText("Caricamento dei profili NAM...",juce::dontSendNotification);
                juce::MessageManager::callAsync([safe = juce::Component::SafePointer<GuitarRigWindow>(this), folder]() {
                    if (safe != nullptr) safe->loadModels(folder);
                });
            }
        });
    }

    void loadModels(const juce::File& folder) {
        auto* deviceBeforeShutdown = deviceManager.getCurrentAudioDevice();
        const int sr = static_cast<int>(deviceBeforeShutdown ? deviceBeforeShutdown->getCurrentSampleRate() : 48000);
        shutdownAudio(); // invariant: models may only be created/destroyed while callback is stopped
        captures.clear();
        namSelector.clear(juce::dontSendNotification);
        libraryPage=0;
        juce::Array<juce::File> files;
        if (folder.existsAsFile() && folder.hasFileExtension(".nam")) files.add(folder);
        else files=folder.findChildFiles(juce::File::findFiles, true, "*.nam");
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
                e.model = std::move(ptr);
                captures.push_back(std::move(e));
            } catch (const std::exception&) { ++fails; }
        }
        requestedIndex.store(captures.empty() ? -1 : 0);
        namSelector.addItem("BYPASS NAM / SEGNALE DI",1);
        for (int i=0;i<static_cast<int>(captures.size());++i)
            namSelector.addItem(captures[static_cast<size_t>(i)].name,i+2);
        namSelector.setSelectedId(captures.empty()?1:2,juce::dontSendNotification);
        setAudioChannels(1,2);
        if (captures.empty()) {
            status.setText("Nessun NAM caricato. Il rig resta utilizzabile con il segnale DI e i VST3.",juce::dontSendNotification);
            current.setText("NESSUN PROFILO",juce::dontSendNotification);
        } else {
            status.setText(juce::String(captures.size())+" NAM caricati. Seleziona manualmente la cattura. Scartati: "+juce::String(fails), juce::dontSendNotification);
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
                status.setText("IR caricata: "+irName+".",juce::dontSendNotification);
            }
        });
    }

    // NAM changes are user-initiated. The existing audio callback crossfades captures.
    void selectNam(int index) {
        if (index < -1 || index >= static_cast<int>(captures.size())) return;
        requestedIndex.store(index, std::memory_order_relaxed);
        namSelector.setSelectedId(index+2, juce::dontSendNotification);
        libraryPage=index<0?0:(index/5)*5;
        status.setText(index<0 ? "NAM bypass: segnale DI tramite PRE / POST VST3"
                               : "NAM selezionato: "+captures[static_cast<size_t>(index)].name,
                       juce::dontSendNotification);
        timerCallback();
    }

    void changeNam(int offset) {
        if (captures.empty()) return;
        const int current=requestedIndex.load(std::memory_order_relaxed);
        const int next=juce::jlimit(-1,static_cast<int>(captures.size())-1,current+offset);
        selectNam(next);
    }

    void timerCallback() override {
        const int selected=requestedIndex.load(std::memory_order_relaxed);
        const bool hasModel=selected>=0 && selected<static_cast<int>(captures.size());
        current.setText(hasModel ? captures[static_cast<size_t>(selected)].name
                                 : juce::String("NAM BYPASS  /  SEGNALE DI"),juce::dontSendNotification);
        categoryLabel.setText(hasModel ? "CATTURA SELEZIONATA MANUALMENTE"
                                       : "CARICA UN PROFILO O UNA CARTELLA .NAM",juce::dontSendNotification);
        level.setText(juce::String(levelDb.load(),1)+" dBFS",juce::dontSendNotification);
        const float db=levelDb.load();
        signalLabel.setText(db > -65.f ? "●  SEGNALE IN INGRESSO" : "○  IN ATTESA DI SEGNALE",juce::dontSendNotification);
        signalLabel.setColour(juce::Label::textColourId,db > -65.f ? kGreen : kMuted);
        auto* device=deviceManager.getCurrentAudioDevice();
        deviceLabel.setText(device ? device->getName() : "Nessuna scheda selezionata",juce::dontSendNotification);
        sampleLabel.setText(juce::String(juce::roundToInt(sampleRateLive.load()))+" Hz   /   Core Audio",juce::dontSendNotification);
        countLabel.setText(juce::String(captures.size())+" PROFILI NAM IN LIBRERIA",juce::dontSendNotification);
        irLabel.setText(irName.isNotEmpty()?"CABINET IR  /  "+irName:"CABINET IR  /  NON CARICATA",juce::dontSendNotification);
        reasonLabel.setText(hasModel?"NAM: "+captures[static_cast<size_t>(selected)].name
                                     : juce::String("Il segnale DI passa nei VST3 anche senza NAM."),juce::dontSendNotification);
        modeLabel.setText("●  CONTROLLO MANUALE",juce::dontSendNotification);
        for (int i=0;i<5;++i) {
            const int index=libraryPage+i;
            const bool visible=index<static_cast<int>(captures.size());
            libraryIndexes[static_cast<size_t>(i)]=visible ? index : -1;
            auto& label=libraryNames[static_cast<size_t>(i)];
            auto& button=libraryButtons[static_cast<size_t>(i)];
            label.setVisible(visible);
            button.setVisible(visible);
            if(visible) label.setText(captures[static_cast<size_t>(index)].name,juce::dontSendNotification);
        }
        previousNamButton.setEnabled(!captures.empty() && selected>=0);
        nextNamButton.setEnabled(!captures.empty() && selected < static_cast<int>(captures.size())-1);
        previousPageButton.setEnabled(libraryPage>0);
        nextPageButton.setEnabled(libraryPage+5 < static_cast<int>(captures.size()));
        repaint();
    }

    SculptLook sculptLook;
    juce::Label title, subtitle, status, current, level, info, tone3000Brand, masterLabel;
    juce::Slider masterSlider;
    juce::TextEditor tone3000Query;
    juce::ComboBox namSelector;
    juce::TextButton tone3000Search, rigButton;
    juce::Label deviceLabel, sampleLabel, categoryLabel, reasonLabel, signalLabel, countLabel, irLabel, modeLabel;
    std::array<juce::Label,5> libraryNames;
    std::array<juce::TextButton,5> libraryButtons;
    std::array<int,5> libraryIndexes{{-1,-1,-1,-1,-1}};
    juce::TextButton audioButton, folderButton, irButton, previousNamButton, nextNamButton, previousPageButton, nextPageButton;
    int libraryPage=0;
    std::unique_ptr<juce::FileChooser> fileChooser, irChooser, vstChooser;
    std::unique_ptr<HostedEditorWindow> activePluginWindow;
    int activePluginEditorSlot=-1;
    std::vector<Capture> captures;
    std::atomic<int> requestedIndex{-1};
    std::atomic<bool> irLoaded{false};
    std::atomic<float> levelDb{-90.f}, masterGainDb{0.f};
    std::atomic<double> sampleRateLive{48000.};
    int activeIndex = -1, fadingFrom = -1, fadeRemaining = 0;
    float lowpassZ = 0.0f;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> masterGain;
    std::array<float, kBlock> inputScratch{}, dryInput{}, driven{}, outA{}, outB{};
    juce::AudioBuffer<float> fxBuffer{2,kBlock};
    VstChain vstChain;
    std::atomic<bool> gateEnabled{true};
    std::atomic<float> gateThresholdDb{-55.f};
    float gateEnvelope=0.f, gateGain=0.f;
    juce::AudioBuffer<float> cabBuffer{1,kBlock};
    juce::dsp::Convolution cab;
    juce::String irName;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GuitarRigWindow)
};

class SculptMainWindow final : public juce::DocumentWindow {
public:
    SculptMainWindow() : juce::DocumentWindow("Sculpt Guitar Rig — Beta", kBg, juce::DocumentWindow::allButtons) {}
    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class SculptApp final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Sculpt Guitar Rig"; }
    const juce::String getApplicationVersion() override { return "0.7.0-beta-manual-rig"; }
    bool moreThanOneInstanceAllowed() override { return false; }
    void initialise(const juce::String&) override {
        mainWindow = std::make_unique<SculptMainWindow>();
        mainWindow->setUsingNativeTitleBar(true);
        mainWindow->setContentOwned(new GuitarRigWindow(), true);
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
