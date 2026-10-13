#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <NeuralAudio/NeuralModel.h>
#include "VstChain.h"

#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <functional>
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
        subtitle.setText("NAM  /  VST3  /  CAB IR  /  SCULPT CH     •     BETA 0.9.1", juce::dontSendNotification);
        subtitle.setFont(juce::Font(juce::FontOptions(11.f)));
        subtitle.setColour(juce::Label::textColourId, kWarm);
        status.setText("Rig manuale: scegli i tuoi NAM e i tuoi VST3. Host VST3 corretto e impostazioni audio preservate.", juce::dontSendNotification);
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
        modeLabel.setText("●  RIG MANUALE",juce::dontSendNotification);
        setAudioChannels(1, 2);
        // The single-window rig is the main component, not an additional dialog.
        rigPanel = std::make_unique<RigPanel>(*this);
        rigViewport.setViewedComponent(rigPanel.get(), false);
        rigViewport.setScrollBarsShown(true, true);
        addAndMakeVisible(rigViewport);
        audioSetupOverlay = std::make_unique<AudioSetupOverlay>(deviceManager, [this] { hideAudioSettings(); });
        addChildComponent(audioSetupOverlay.get());
        audioSetupOverlay->setVisible(false);
        for (auto* component : std::initializer_list<juce::Component*>{
                 &title, &subtitle, &status, &current, &level, &info, &tone3000Brand,
                 &masterLabel, &masterSlider, &tone3000Query, &tone3000Search, &rigButton,
                 &audioButton, &folderButton, &irButton, &previousNamButton, &nextNamButton,
                 &previousPageButton, &nextPageButton, &namSelector, &deviceLabel,
                 &sampleLabel, &categoryLabel, &reasonLabel, &signalLabel, &countLabel,
                 &irLabel, &modeLabel}) component->setVisible(false);
        for (auto& item : libraryNames) item.setVisible(false);
        for (auto& item : libraryButtons) item.setVisible(false);
        setSize(1580, 890);
        startTimerHz(10);
        timerCallback();
    }

    ~GuitarRigWindow() override {
        stopTimer();
        activePluginWindow.reset();
        rigViewport.setViewedComponent(nullptr, false);
        rigPanel.reset();
        audioSetupOverlay.reset();
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
            double outEnergy = 0.0;
            for (int i=0;i<n;++i) {
                const float gain = masterGain.getNextValue();
                const float l = juce::jlimit(-0.85f, 0.85f, processedL[i] * gain);
                const float r = right != nullptr ? juce::jlimit(-0.85f,0.85f,processedR[i]*gain) : l;
                left[i] = l;
                if (right != nullptr) right[i] = r;
                outEnergy += 0.5 * (double(l)*double(l) + double(r)*double(r));
            }
            const double outRms = std::sqrt(outEnergy / std::max(1, n));
            outputDb.store(static_cast<float>(20.0 * std::log10(std::max(1.e-8, outRms))),
                           std::memory_order_relaxed);
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
        g.fillAll(kBg);
    }

    void resized() override {
        const auto bounds = getLocalBounds();
        rigViewport.setBounds(bounds);
        if (rigPanel != nullptr)
            rigPanel->setSize(juce::jmax(1460, bounds.getWidth()), juce::jmax(790, bounds.getHeight()));
        if (audioSetupOverlay != nullptr) {
            const int width = juce::jmin(600, juce::jmax(430, bounds.getWidth()-48));
            const int height = juce::jmin(510, juce::jmax(410, bounds.getHeight()-48));
            audioSetupOverlay->setBounds((bounds.getWidth()-width)/2,
                                         (bounds.getHeight()-height)/2,width,height);
            if (audioSetupOverlay->isVisible()) audioSetupOverlay->toFront(false);
        }
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
        explicit RigPanel(GuitarRigWindow& parent): owner(parent) {
            setOpaque(true);
            auto label = [this](juce::Label& l, const juce::String& text, float size, juce::Colour colour) {
                l.setText(text,juce::dontSendNotification);
                l.setColour(juce::Label::textColourId,colour);
                l.setFont(juce::Font(juce::FontOptions(size,juce::Font::bold)));
                l.setMinimumHorizontalScale(0.68f);
                addAndMakeVisible(l);
            };
            label(brand,"SCULPT / GUITAR RIG",24.f,kText);
            label(subtitle,"NAM  /  VST3  /  SCULPT CHANNEL  /  ONE WINDOW",11.5f,kWarm);
            label(status,"",12.f,kMuted);
            label(inMeter,"-90.0 dBFS",15.f,kGreen);
            label(outMeter,"-90.0 dBFS",15.f,kGreen);
            label(namName,"NAM: BYPASS",15.f,kText);
            label(irName,"IR: NON CARICATA",12.f,kMuted);
            label(sculptName,"SCULPT CH: NON CARICATO",11.f,kMuted);
            label(gateHeading,"NOISE GATE",12.f,kWarm);
            label(masterHeading,"MASTER OUTPUT",12.f,kWarm);
            label(masterReadout,"0.0 dB",14.f,kText);
            label(sculptHeading,"PARAMETRI DEL PLUGIN",11.f,kMuted);
            label(preHeading,"PRE / 4 VST3",12.5f,kWarm);
            label(postHeading,"POST / 4 VST3",12.5f,kWarm);
            label(namHeading,"NAM / CABINET IR",12.5f,kWarm);
            label(inputHeading,"INGRESSO",12.5f,kWarm);
            label(outputHeading,"USCITA",12.5f,kWarm);
            label(sculptSectionHeading,"SCULPT CHANNEL",12.5f,kWarm);
            label(toneLabel,"TONE3000",11.f,kMuted);
            toneQuery.setTextToShowWhenEmpty("Cerca catture NAM online...",kMuted);
            toneQuery.setColour(juce::TextEditor::backgroundColourId,kPanelAlt);
            toneQuery.setColour(juce::TextEditor::textColourId,kText);
            toneQuery.setColour(juce::TextEditor::outlineColourId,kMuted.withAlpha(.35f));
            toneQuery.onReturnKey = [this] { openToneSearch(); };
            addAndMakeVisible(toneQuery);
            toneSearch.setButtonText("CERCA ↗");
            toneSearch.onClick = [this] { openToneSearch(); };
            addAndMakeVisible(toneSearch);

            inputSetup.setButtonText("SETUP AUDIO ▾");
            outputSetup.setButtonText("SETUP AUDIO ▾");
            inputSetup.onClick = [this] { owner.showAudioSettings(); };
            outputSetup.onClick = [this] { owner.showAudioSettings(); };
            addAndMakeVisible(inputSetup); addAndMakeVisible(outputSetup);
            gateBypass.setButtonText("GATE ON");
            gateBypass.onClick = [this] { owner.gateEnabled.store(!owner.gateEnabled.load()); updateLabels(); };
            addAndMakeVisible(gateBypass);
            gateThreshold.setSliderStyle(juce::Slider::LinearHorizontal);
            gateThreshold.setTextBoxStyle(juce::Slider::TextBoxBelow,false,64,18);
            gateThreshold.setTextValueSuffix(" dB");
            gateThreshold.setRange(-85.,-15.,1.);
            gateThreshold.setValue(owner.gateThresholdDb.load(),juce::dontSendNotification);
            gateThreshold.onValueChange = [this]{owner.gateThresholdDb.store((float)gateThreshold.getValue());};
            addAndMakeVisible(gateThreshold);
            masterSlider.setSliderStyle(juce::Slider::LinearVertical);
            masterSlider.setTextBoxStyle(juce::Slider::NoTextBox,false,0,0);
            masterSlider.setRange(-60.,6.,.5);
            masterSlider.setDoubleClickReturnValue(true,0.0);
            masterSlider.onValueChange = [this] {owner.masterGainDb.store((float)masterSlider.getValue());};
            masterSlider.setColour(juce::Slider::trackColourId,kAccent);
            addAndMakeVisible(masterSlider);

            namPicker.setTextWhenNothingSelected("Scegli NAM...");
            namPicker.onChange = [this] {
                const int id = namPicker.getSelectedId();
                if(id>0) owner.selectNam(id-2);
            };
            addAndMakeVisible(namPicker);
            loadNam.setButtonText("IMPORTA NAM..."); loadNam.onClick=[this]{owner.chooseNAMFolder();}; addAndMakeVisible(loadNam);
            loadIr.setButtonText("CARICA IR..."); loadIr.onClick=[this]{owner.chooseIR();}; addAndMakeVisible(loadIr);
            prevNam.setButtonText("◀"); prevNam.onClick=[this]{owner.changeNam(-1);}; addAndMakeVisible(prevNam);
            nextNam.setButtonText("▶"); nextNam.onClick=[this]{owner.changeNam(1);}; addAndMakeVisible(nextNam);

            // Install a VST3 in any slot using a dropdown, not a separate rig window.
            pluginPaths = VstChain::installedVst3();
            for(int i=0;i<9;++i) {
                const int index=i;
                const juce::String title = i<4 ? "PRE " + juce::String(i+1) : (i==4 ? "SCULPT CH" : "POST " + juce::String(i-4));
                label(slotNames[(size_t)i],title,12.f,kText);
                label(slotStatus[(size_t)i],"VUOTO",10.5f,kMuted);
                auto& menu=slotMenus[(size_t)i];
                menu.setTextWhenNothingSelected("Scegli VST3...");
                menu.addItem("Scegli VST3...",1);
                for(int p=0;p<pluginPaths.size();++p){
                    const auto name=pluginPaths.getReference(p).getFileNameWithoutExtension();
                    if(i==4 && !(name.toLowerCase().contains("sculpt") && name.toLowerCase().contains("chan"))) continue;
                    pluginMap[(size_t)i].push_back(p);
                    menu.addItem(name,(int)pluginMap[(size_t)i].size()+1);
                }
                menu.addSeparator();
                menu.addItem("SFOGLIA VST3...",10000);
                menu.onChange=[this,index] {
                    auto& dropdown=slotMenus[(size_t)index];
                    const int id=dropdown.getSelectedId();
                    if(id==10000){owner.browsePluginBundle(index);return;}
                    if(id>=2 && id-2<(int)pluginMap[(size_t)index].size())
                        owner.loadPluginSlot(index,pluginPaths.getReference(pluginMap[(size_t)index][(size_t)(id-2)]));
                };
                menu.setSelectedId(1,juce::dontSendNotification);
                addAndMakeVisible(menu);
                auto& edit=editors[(size_t)i]; edit.setButtonText("EDIT"); edit.onClick=[this,index]{owner.openPluginEditor(index);}; addAndMakeVisible(edit);
                auto& on=power[(size_t)i]; on.setButtonText("ON/OFF"); on.onClick=[this,index]{owner.vstChain.enable(index,!owner.vstChain.enabled(index)); updateLabels();}; addAndMakeVisible(on);
                auto& clear=clearButtons[(size_t)i]; clear.setButtonText("X"); clear.onClick=[this,index]{owner.removeVstSlot(index);slotMenus[(size_t)index].setSelectedId(1,juce::dontSendNotification); updateLabels();}; addAndMakeVisible(clear);
            }
            const char* names[]={"Low","Mid","High","Presence","Output"};
            for(int k=0;k<5;++k){
                paramNames[(size_t)k]=names[k];
                label(knobLabels[(size_t)k],juce::String(names[k]).toUpperCase(),10.f,kMuted);
                auto& sl=knobs[(size_t)k];
                sl.setSliderStyle(juce::Slider::RotaryVerticalDrag);
                sl.setTextBoxStyle(juce::Slider::NoTextBox,false,0,0);
                sl.setRange(0.0,1.0,.001);
                sl.setColour(juce::Slider::rotarySliderFillColourId,kAccent);
                sl.setColour(juce::Slider::thumbColourId,kWarm);
                sl.onValueChange=[this,k]{setSculptParam(k,(float)knobs[(size_t)k].getValue());};
                addAndMakeVisible(sl);
                label(knobValues[(size_t)k],"-",10.5f,kText);
                knobValues[(size_t)k].setJustificationType(juce::Justification::centred);
            }
            variation.setButtonText("VARIATION OFF");
            variation.onClick = [this] { setSculptVariation(); };
            addAndMakeVisible(variation);
            setSize(1580,890);
            startTimerHz(6);
            updateLabels();
        }
        ~RigPanel() override {stopTimer();}

        void paint(juce::Graphics& g) override {
            g.fillAll(kBg);
            auto top=juce::Rectangle<float>(0,0,(float)getWidth(),88.f);
            g.setGradientFill(juce::ColourGradient(kPanelAlt,0,0,kBg,0,88,false));
            g.fillRect(top);
            g.setColour(kAccent);g.fillRoundedRectangle(juce::Rectangle<float>(18.f,16.f,5.f,43.f),2.f);
            auto headings=std::array<juce::String,6>{{"01 / INPUT","02 / PRE VST3","03 / NAM / CAB","04 / SCULPT CH","05 / POST VST3","06 / OUTPUT"}};
            for(size_t i=0;i<sections.size();++i){
                auto r=sections[i].toFloat();
                if(r.isEmpty())continue;
                g.setColour(kPanel);g.fillRoundedRectangle(r,11.f);
                g.setColour(juce::Colours::white.withAlpha(.07f));g.drawRoundedRectangle(r,11.f,1.f);
                g.setColour(kWarm);g.setFont(juce::Font(juce::FontOptions(12.5f,juce::Font::bold)));
                g.drawFittedText(headings[i],sections[i].reduced(12,0).withHeight(32).translated(0,8),juce::Justification::centredLeft,1);
            }
            auto meter=[&](int section,float db){
                auto r=sections[(size_t)section];if(r.isEmpty())return;
                const auto bar=r.reduced(14,0).withTrimmedTop(132).withHeight(14).toFloat();
                g.setColour(juce::Colour(0xff33353b));g.fillRoundedRectangle(bar,3.f);
                auto amt=juce::jlimit(0.f,1.f,(db+70.f)/70.f);
                g.setColour(db>-9.f?kWarm:kGreen);
                if(amt>.005f)g.fillRoundedRectangle(bar.withWidth(bar.getWidth()*amt),3.f);
            };
            meter(0,owner.levelDb.load());meter(5,owner.outputDb.load());
            for(int i=0;i<9;++i){
                auto r=slotRects[(size_t)i].toFloat(); if(r.isEmpty())continue;
                const bool active=owner.vstChain.enabled(i);
                g.setColour(active?juce::Colour(0xff30363a):kPanelAlt);
                g.fillRoundedRectangle(r,8.f);
                g.setColour(active?kGreen.withAlpha(.35f):juce::Colours::white.withAlpha(.07f));
                g.drawRoundedRectangle(r,8.f,1.f);
                g.setColour(active?kGreen:kMuted);
                g.fillEllipse(r.getRight()-15.f,r.getY()+13.f,6.f,6.f);
            }
            g.setColour(kMuted.withAlpha(.4f));
            g.drawLine(18.f,(float)getHeight()-35, (float)getWidth()-18,(float)getHeight()-35,1.f);
        }

        void resized() override {
            const int gap=9,pad=14;
            const int width=getWidth()-2*pad-5*gap;
            const std::array<float,6> fractions{{.117f,.23f,.166f,.14f,.23f,.117f}};
            int x=pad;
            const int y=94, h=getHeight()-94-47;
            for(int i=0;i<6;++i){
                const int w=i==5?getWidth()-pad-x:(int)std::round(width*fractions[(size_t)i]);
                sections[(size_t)i]={x,y,w,h};x+=w+gap;
            }
            auto input=sections[0].reduced(12,0),pre=sections[1].reduced(10,0),nam=sections[2].reduced(12,0);
            auto sculpt=sections[3].reduced(9,0),post=sections[4].reduced(10,0),output=sections[5].reduced(12,0);
            brand.setBounds(32,15,385,31);
            subtitle.setBounds(33,52,430,19);
            toneLabel.setBounds(getWidth()-514,17,90,19);
            toneQuery.setBounds(getWidth()-425,15,252,31);
            toneSearch.setBounds(getWidth()-166,15,140,31);
            status.setBounds(getWidth()/3,54,getWidth()*2/3-38,22);
            inputSetup.setBounds(input.getX(),y+48,input.getWidth(),31);
            inMeter.setBounds(input.getX(),y+108,input.getWidth(),20);
            gateHeading.setBounds(input.getX(),y+191,input.getWidth(),20);
            gateBypass.setBounds(input.getX(),y+225,input.getWidth(),33);
            gateThreshold.setBounds(input.getX(),y+275,input.getWidth(),68);
            outputSetup.setBounds(output.getX(),y+48,output.getWidth(),31);
            outMeter.setBounds(output.getX(),y+108,output.getWidth(),20);
            masterHeading.setBounds(output.getX(),y+191,output.getWidth(),19);
            masterSlider.setBounds(output.getX()+output.getWidth()/2-30,y+226,60,215);
            masterReadout.setBounds(output.getX(),y+463,output.getWidth(),25);
            masterReadout.setJustificationType(juce::Justification::centred);
            namName.setBounds(nam.getX(),y+50,nam.getWidth(),42);
            namPicker.setBounds(nam.getX(),y+105,nam.getWidth(),32);
            prevNam.setBounds(nam.getX(),y+148,35,30);
            nextNam.setBounds(nam.getX()+40,y+148,35,30);
            loadNam.setBounds(nam.getX()+80,y+148,nam.getWidth()-80,30);
            loadIr.setBounds(nam.getX(),y+215,nam.getWidth(),33);
            irName.setBounds(nam.getX(),y+260,nam.getWidth(),52);

            // Four pedal inserts in each bank; compact independent controls per row.
            const int top=y+47, usable=h-62, rowH=(usable-24)/4;
            for(int k=0;k<4;++k){
                auto r=juce::Rectangle<int>(pre.getX(),top+k*(rowH+8),pre.getWidth(),rowH);
                layoutSlot(k,r);
                auto q=juce::Rectangle<int>(post.getX(),top+k*(rowH+8),post.getWidth(),rowH);
                layoutSlot(5+k,q);
            }
            sculptName.setBounds(sculpt.getX(),y+52,sculpt.getWidth(),37);
            auto r=juce::Rectangle<int>(sculpt.getX(),y+96,sculpt.getWidth(),107);
            layoutSlot(4,r);
            sculptHeading.setBounds(sculpt.getX(),y+214,sculpt.getWidth(),20);
            const int knobW=juce::jmax(65,(sculpt.getWidth()-12)/2);
            const int knobH=112;
            for(int k=0;k<5;++k){
                const int col=k%2,row=k/2;
                const int kx=sculpt.getX()+col*(knobW+7), ky=y+253+row*knobH;
                knobLabels[(size_t)k].setBounds(kx,ky,knobW,17);
                knobs[(size_t)k].setBounds(kx+7,ky+18,knobW-14,63);
                knobValues[(size_t)k].setBounds(kx,ky+80,knobW,19);
            }
            variation.setBounds(sculpt.getX(),y+590,sculpt.getWidth(),30);
            labelizeLocation(y);
        }
    private:
        void labelizeLocation(int){}
        void layoutSlot(int index,juce::Rectangle<int> r){
            slotRects[(size_t)index]=r;
            const int x=r.getX()+10,y=r.getY()+7,w=r.getWidth()-20;
            slotNames[(size_t)index].setBounds(x,y,w-20,20);
            slotMenus[(size_t)index].setBounds(x,y+28,w,29);
            slotStatus[(size_t)index].setBounds(x,y+60,w,24);
            const int rowBottom=r.getBottom()-36;
            const int editW=juce::jmax(38,(w-16)*4/10);
            editors[(size_t)index].setBounds(x,rowBottom,editW,27);
            power[(size_t)index].setBounds(x+editW+5,rowBottom,juce::jmax(46,w-editW-45),27);
            clearButtons[(size_t)index].setBounds(x+w-32,rowBottom,32,27);
        }
        void openToneSearch(){
            owner.tone3000Query.setText(toneQuery.getText(),juce::dontSendNotification);
            owner.openTone3000Search();
        }
        juce::AudioProcessorParameter* sculptParam(int index) const {
            auto* processor=owner.vstChain.get(4);
            if(processor==nullptr || index<0 || index>=5)return nullptr;
            auto& params=processor->getParameters();
            for(auto* p:params) if(p!=nullptr && p->getName(64).equalsIgnoreCase(paramNames[(size_t)index]))return p;
            return nullptr;
        }
        void setSculptParam(int index,float value){
            if(updatingKnobs)return;
            if(auto* p=sculptParam(index)){
                p->beginChangeGesture();p->setValueNotifyingHost(value);p->endChangeGesture();
            }
        }
        void setSculptVariation(){
            auto* processor=owner.vstChain.get(4);if(!processor)return;
            for(auto* p:processor->getParameters()){
                if(p && p->getName(64).equalsIgnoreCase("Variation")){
                    p->beginChangeGesture();p->setValueNotifyingHost(p->getValue()>.5f?0.f:1.f);p->endChangeGesture();
                    updateLabels();break;
                }
            }
        }
        void timerCallback() override {updateLabels();}
        void updateLabels(){
            inMeter.setText(juce::String(owner.levelDb.load(),1)+" dBFS",juce::dontSendNotification);
            outMeter.setText(juce::String(owner.outputDb.load(),1)+" dBFS",juce::dontSendNotification);
            gateBypass.setButtonText(owner.gateEnabled.load()?"GATE ON":"GATE OFF");
            const auto m=owner.masterGainDb.load();
            masterSlider.setValue(m,juce::dontSendNotification);
            masterReadout.setText(m<=-60.f?"MUTE":juce::String(m,1)+" dB",juce::dontSendNotification);
            status.setText(owner.status.getText(),juce::dontSendNotification);
            const int selected=owner.requestedIndex.load(std::memory_order_relaxed);
            juce::StringArray names;
            for(const auto& c:owner.captures)names.add(c.name);
            if(names!=cachedNamNames){
                cachedNamNames=names;namPicker.clear(juce::dontSendNotification);
                namPicker.addItem("BYPASS NAM / DI",1);
                for(int i=0;i<names.size();++i)namPicker.addItem(names[i],i+2);
            }
            namPicker.setSelectedId(selected<0?1:selected+2,juce::dontSendNotification);
            namName.setText(selected>=0 && selected<(int)owner.captures.size()?owner.captures[(size_t)selected].name:"BYPASS / DI",juce::dontSendNotification);
            irName.setText(owner.irName.isNotEmpty()?"IR: "+owner.irName:"IR: NON CARICATA",juce::dontSendNotification);
            prevNam.setEnabled(selected>0); nextNam.setEnabled(selected<(int)owner.captures.size()-1);
            sculptName.setText(owner.vstChain.name(4).isNotEmpty()?owner.vstChain.name(4):"CARICA SCULPT CHANNEL",juce::dontSendNotification);
            for(int i=0;i<9;++i){
                const auto name=owner.vstChain.name(i);
                slotStatus[(size_t)i].setText(name.isNotEmpty()?name:"NESSUN PLUGIN",juce::dontSendNotification);
                editors[(size_t)i].setEnabled(name.isNotEmpty());
                power[(size_t)i].setEnabled(name.isNotEmpty());
                clearButtons[(size_t)i].setEnabled(name.isNotEmpty());
                power[(size_t)i].setButtonText(owner.vstChain.enabled(i)?"ON":"BYPASS");
                power[(size_t)i].setToggleState(owner.vstChain.enabled(i),juce::dontSendNotification);
            }
            updatingKnobs=true;
            for(int k=0;k<5;++k){
                auto* p=sculptParam(k);
                knobs[(size_t)k].setEnabled(p!=nullptr);
                if(p!=nullptr && !knobs[(size_t)k].isMouseButtonDown()){
                    knobs[(size_t)k].setValue(p->getValue(),juce::dontSendNotification);
                    knobValues[(size_t)k].setText(p->getCurrentValueAsText(),juce::dontSendNotification);
                }
                if(!p)knobValues[(size_t)k].setText("-",juce::dontSendNotification);
            }
            auto* processor=owner.vstChain.get(4);
            variation.setEnabled(processor!=nullptr);
            if(processor)for(auto* p:processor->getParameters())if(p && p->getName(64).equalsIgnoreCase("Variation")){
                variation.setButtonText(p->getValue()>.5f?"VARIATION ON":"VARIATION OFF");break;
            }
            updatingKnobs=false;
            repaint();
        }
        GuitarRigWindow& owner;
        juce::Array<juce::File> pluginPaths;
        std::array<std::vector<int>,9> pluginMap;
        std::array<juce::Rectangle<int>,6> sections;
        std::array<juce::Rectangle<int>,9> slotRects;
        juce::StringArray cachedNamNames;
        bool updatingKnobs=false;
        std::array<juce::String,5> paramNames;
        juce::Label brand,subtitle,status,inMeter,outMeter,namName,irName,sculptName,gateHeading,masterHeading,masterReadout,sculptHeading;
        juce::Label preHeading,postHeading,namHeading,inputHeading,outputHeading,sculptSectionHeading,toneLabel;
        juce::TextEditor toneQuery;
        juce::TextButton toneSearch,inputSetup,outputSetup,gateBypass,loadNam,loadIr,prevNam,nextNam,variation;
        juce::Slider gateThreshold,masterSlider;
        juce::ComboBox namPicker;
        std::array<juce::Label,9> slotNames,slotStatus;
        std::array<juce::ComboBox,9> slotMenus;
        std::array<juce::TextButton,9> editors,power,clearButtons;
        std::array<juce::Slider,5> knobs;
        std::array<juce::Label,5> knobLabels,knobValues;
    };

    // Same-window audio configuration. Only VST3 editors create additional windows.
    class AudioSetupOverlay final : public juce::Component {
    public:
        AudioSetupOverlay(juce::AudioDeviceManager& manager,std::function<void()> closeCallback)
            : selector(manager,1,2,1,2,false,false,true,false), onClose(std::move(closeCallback)) {
            setOpaque(true);
            closeButton.setButtonText("CHIUDI  ×");
            closeButton.onClick=[this]{if(onClose)onClose();};
            addAndMakeVisible(closeButton);
            addAndMakeVisible(selector);
            heading.setText("CONFIGURAZIONE AUDIO  /  INPUT + OUTPUT",juce::dontSendNotification);
            heading.setFont(juce::Font(juce::FontOptions(15.f,juce::Font::bold)));
            heading.setColour(juce::Label::textColourId,kWarm);
            addAndMakeVisible(heading);
        }
        void paint(juce::Graphics& g) override{
            g.fillAll(kPanel);
            g.setColour(kAccent);g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.f),10.f,2.f);
        }
        void resized() override{
            heading.setBounds(18,12,getWidth()-172,28);
            closeButton.setBounds(getWidth()-144,12,126,29);
            selector.setBounds(16,53,getWidth()-32,getHeight()-70);
        }
    private:
        juce::AudioDeviceSelectorComponent selector;
        std::function<void()> onClose;
        juce::TextButton closeButton;
        juce::Label heading;
    };

    void showRigPanel(){
        if(rigPanel!=nullptr){rigViewport.toFront(false);if(audioSetupOverlay!=nullptr && audioSetupOverlay->isVisible())audioSetupOverlay->toFront(false);}
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
        juce::String error;
        {
            const juce::ScopedLock lock(deviceManager.getAudioCallbackLock());
            const bool loaded=vstChain.load(slot,bundle,sr,kBlock,error);
            if(loaded) status.setText("VST3 caricato: "+vstChain.name(slot),juce::dontSendNotification);
            else status.setText("VST3: "+error,juce::dontSendNotification);
        }
    }
    void removeVstSlot(int slot) {
        if(activePluginEditorSlot==slot){activePluginWindow.reset();activePluginEditorSlot=-1;}
        {
            const juce::ScopedLock lock(deviceManager.getAudioCallbackLock());
            vstChain.remove(slot);
        }
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
        if(audioSetupOverlay==nullptr)return;
        const bool shouldShow=!audioSetupOverlay->isVisible();
        audioSetupOverlay->setVisible(shouldShow);
        if(shouldShow)audioSetupOverlay->toFront(true);
    }
    void hideAudioSettings() {
        if(audioSetupOverlay!=nullptr)audioSetupOverlay->setVisible(false);
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
        auto* deviceBeforeLoad = deviceManager.getCurrentAudioDevice();
        const int sr = static_cast<int>(deviceBeforeLoad ? deviceBeforeLoad->getCurrentSampleRate() : 48000);
        std::vector<Capture> loadedCaptures;
        juce::Array<juce::File> files;
        namSelector.clear(juce::dontSendNotification);
        libraryPage=0;
        if (folder.existsAsFile() && folder.hasFileExtension(".nam")) files.add(folder);
        else files=folder.findChildFiles(juce::File::findFiles, true, "*.nam");
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.getFullPathName() < b.getFullPathName(); });
        NeuralAudio::NeuralModelLoader loader;
        loader.SetDefaultMaxAudioBufferSize(kBlock);
        loader.SetExternalSampleRate(sr);
        loader.SetAudioInputLevelDBu(12.f);
        int fails = 0;
        for (const auto& file : files) {
            if (loadedCaptures.size() >= static_cast<size_t>(kMaxModels)) break;
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
                loadedCaptures.push_back(std::move(e));
            } catch (const std::exception&) { ++fails; }
        }
        {
            const juce::ScopedLock lock(deviceManager.getAudioCallbackLock());
            captures = std::move(loadedCaptures);
            requestedIndex.store(captures.empty() ? -1 : 0, std::memory_order_relaxed);
            activeIndex = requestedIndex.load(std::memory_order_relaxed);
            fadingFrom = -1;
            fadeRemaining = 0;
        }
        namSelector.addItem("BYPASS NAM / SEGNALE DI",1);
        for (int i=0;i<static_cast<int>(captures.size());++i)
            namSelector.addItem(captures[static_cast<size_t>(i)].name,i+2);
        namSelector.setSelectedId(captures.empty()?1:2,juce::dontSendNotification);
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
        modeLabel.setText("●  RIG MANUALE",juce::dontSendNotification);
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
    std::atomic<float> levelDb{-90.f}, outputDb{-90.f}, masterGainDb{0.f};
    std::atomic<double> sampleRateLive{48000.};
    int activeIndex = -1, fadingFrom = -1, fadeRemaining = 0;
    float lowpassZ = 0.0f;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> masterGain;
    std::array<float, kBlock> inputScratch{}, dryInput{}, driven{}, outA{}, outB{};
    juce::AudioBuffer<float> fxBuffer{2,kBlock};
    VstChain vstChain;
    juce::Viewport rigViewport;
    std::unique_ptr<RigPanel> rigPanel;
    std::unique_ptr<AudioSetupOverlay> audioSetupOverlay;
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
    const juce::String getApplicationVersion() override { return "0.9.0-beta-one-window"; }
    bool moreThanOneInstanceAllowed() override { return false; }
    void initialise(const juce::String&) override {
        mainWindow = std::make_unique<SculptMainWindow>();
        mainWindow->setUsingNativeTitleBar(true);
        mainWindow->setContentOwned(new GuitarRigWindow(), true);
        mainWindow->centreWithSize(1580,890);
        mainWindow->setResizable(true,true);
        mainWindow->setVisible(true);
        mainWindow->setFullScreen(true);
    }
    void shutdown() override { mainWindow.reset(); }
    void systemRequestedQuit() override { quit(); }
private:
    std::unique_ptr<SculptMainWindow> mainWindow;
};
START_JUCE_APPLICATION(SculptApp)
