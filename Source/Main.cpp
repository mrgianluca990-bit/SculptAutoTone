#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <NeuralAudio/NeuralModel.h>
#include "VstChain.h"
#include "GuitarTuner.h"
#include <BinaryData.h>

#include <algorithm>
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
const juce::Colour kBg(0xff090c0e), kPanel(0xff17191a), kPanelAlt(0xff232629),
                   kAccent(0xffff713b), kWarm(0xffe8ae69), kGreen(0xff45d99a),
                   kText(0xffe9e7df), kMuted(0xffa6a5a0), kCyan(0xff3bd4ef);
const std::array<juce::Colour, 8> kPedalColours {{
    juce::Colour(0xffb43729),juce::Colour(0xffcf922d),juce::Colour(0xff1c7cb6),
    juce::Colour(0xff28824c),juce::Colour(0xff873d9a),juce::Colour(0xff196db1),
    juce::Colour(0xff159482),juce::Colour(0xff777a7c)}};

// Polished metal hardware: drawn using JUCE vectors (retina-safe, no opaque assets).
void drawHardwareScrew(juce::Graphics& g, float x, float y, float radius=4.6f) {
    const auto ring=juce::Rectangle<float>(x-radius,y-radius,radius*2.f,radius*2.f);
    g.setColour(juce::Colours::black.withAlpha(.68f));g.fillEllipse(ring.expanded(1.4f));
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xffa5a198),x-radius,y-radius,
                                           juce::Colour(0xff242628),x+radius,y+radius,false));
    g.fillEllipse(ring);
    g.setColour(juce::Colour(0xff101213));g.fillEllipse(ring.reduced(radius*.32f));
    g.setColour(juce::Colour(0xffc4bbb0).withAlpha(.72f));
    g.drawLine(x-radius*.43f,y+radius*.43f,x+radius*.43f,y-radius*.43f,1.35f);
}
void drawGrille(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour edge) {
    g.setColour(juce::Colour(0xff080a0a));g.fillRoundedRectangle(area,2.5f);
    for (float xx=area.getX()+5.f;xx<area.getRight()-3.f;xx+=7.f){
        g.setColour(edge.withAlpha(.22f));g.drawLine(xx,area.getY()+3,xx+1.2f,area.getBottom()-3,1.5f);
    }
    g.setColour(edge.withAlpha(.43f));g.drawRoundedRectangle(area,2.5f,.8f);
}

// Custom draw: no operating-system default buttons, no downloaded fonts or images.
class SculptLook final : public juce::LookAndFeel_V4 {
public:
    juce::Font getTextButtonFont(juce::TextButton&, int h) override {
        return juce::Font(juce::FontOptions(h >= 34 ? 11.5f : 10.5f, juce::Font::bold));
    }
    juce::Font getComboBoxFont(juce::ComboBox&) override {
        return juce::Font(juce::FontOptions(12.5f, juce::Font::bold));
    }
    void drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour&,
                              bool hover, bool pressed) override {
        auto area=b.getLocalBounds().toFloat().reduced(1.3f);
        const bool active=b.getToggleState();
        const auto caption=b.getButtonText().toUpperCase();
        const bool special= caption.contains("TAP") || caption=="SALVA" ||
                            caption.contains("TUNER ON") || caption=="GATE ON";
        const auto edge=active || special ? kWarm : juce::Colour(0xff596068);
        auto top=juce::Colour(0xff303235);
        if (active) top=juce::Colour(0xff473122);
        if (hover) top=top.brighter(0.15f);
        if (pressed) top=top.darker(0.2f);
        g.setGradientFill(juce::ColourGradient(top,area.getX(),area.getY(),kBg,area.getX(),area.getBottom(),false));
        g.fillRoundedRectangle(area,5.f);
        g.setColour(edge.withAlpha(active || special ? 0.72f : 0.43f));
        g.drawRoundedRectangle(area,5.f,1.f);
        g.setColour(juce::Colours::white.withAlpha(0.12f));
        g.drawLine(area.getX()+5.f,area.getY()+2.f,area.getRight()-5.f,area.getY()+2.f,0.6f);
    }
    void drawButtonText(juce::Graphics& g, juce::TextButton& b, bool,bool) override {
        g.setColour(b.isEnabled() ? (b.getToggleState()?kWarm:kText) : kMuted.withAlpha(.5f));
        g.setFont(getTextButtonFont(b,b.getHeight()));
        if(b.getTooltip().containsIgnoreCase("ricerca alfabetica")) {
            auto textArea=b.getLocalBounds().reduced(9,3);
            textArea.removeFromRight(19);
            g.drawFittedText(b.getButtonText(),textArea,juce::Justification::centredLeft,1,.7f);
            const float x=(float)b.getWidth()-16.f,y=(float)b.getHeight()*.5f;
            juce::Path arrow;arrow.startNewSubPath(x-4.f,y-2.f);arrow.lineTo(x,y+2.f);arrow.lineTo(x+4.f,y-2.f);
            g.setColour(kWarm);g.strokePath(arrow,juce::PathStrokeType(1.5f));
        }else{
            g.drawFittedText(b.getButtonText(),b.getLocalBounds().reduced(4,2),juce::Justification::centred,1,0.75f);
        }
    }
    void drawComboBox(juce::Graphics& g, int w,int h,bool,
                      int,int,int,int,juce::ComboBox& box) override {
        auto r=juce::Rectangle<float>(0.f,0.f,(float)w,(float)h).reduced(1.f);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff353839),0.f,0.f,
                                                juce::Colour(0xff12171a),0.f,(float)h,false));
        g.fillRoundedRectangle(r,5.f);
        g.setColour(box.hasKeyboardFocus(false)?kWarm.withAlpha(.8f):juce::Colour(0xff849098).withAlpha(.65f));
        g.drawRoundedRectangle(r,5.f,1.f);
        const auto x=(float)w-19.f,y=(float)h/2.f;
        juce::Path arrow;arrow.startNewSubPath(x-5,y-2);arrow.lineTo(x,y+3);arrow.lineTo(x+5,y-2);
        g.setColour(kText);g.strokePath(arrow,juce::PathStrokeType(1.7f));
    }
    void drawRotarySlider(juce::Graphics& g,int x,int y,int w,int h,float pos,
                          float start,float end,juce::Slider& slider) override {
        juce::Graphics::ScopedSaveState saved(g);
        const float diam=juce::jmax(24.f,juce::jmin((float)w,(float)h)*0.70f);
        const float cx=(float)x+(float)w*.5f,cy=(float)y+(float)h*.49f;
        const float r=diam*.5f;
        const float valueAngle=start+(end-start)*juce::jlimit(0.f,1.f,pos);
        const bool enabled=slider.isEnabled();
        const juce::Colour accent=enabled?kAccent:juce::Colour(0xff575b5b);
        // Two concentric machined rings, lit segments and individually spaced graduations.
        g.setColour(juce::Colours::black.withAlpha(.55f));
        g.fillEllipse(cx-r-8.f,cy-r-6.f,diam+16.f,diam+16.f);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xffbeb6a6),cx-r,cy-r,
                                                juce::Colour(0xff242525),cx+r,cy+r,false));
        g.fillEllipse(cx-r-4.f,cy-r-4.f,diam+8.f,diam+8.f);
        g.setColour(juce::Colour(0xff0d0d0e));
        g.fillEllipse(cx-r+1.f,cy-r+1.f,diam-2.f,diam-2.f);
        juce::Path track;
        track.addCentredArc(cx,cy,r+8.f,r+8.f,0.f,start,end,true);
        g.setColour(juce::Colour(0xff57514c).withAlpha(.70f));
        g.strokePath(track,juce::PathStrokeType(2.8f));
        juce::Path progress;
        progress.addCentredArc(cx,cy,r+8.f,r+8.f,0.f,start,valueAngle,true);
        g.setColour(accent.withAlpha(.96f));
        g.strokePath(progress,juce::PathStrokeType(3.3f,juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
        for(int t=0;t<=16;++t){
            const float angle=start+(end-start)*(float)t/16.f;
            const float rr=r+14.f;
            g.setColour(enabled ? (t%4==0?kWarm.withAlpha(.92f):kWarm.withAlpha(.49f))
                                :kMuted.withAlpha(.28f));
            const float inner=rr-(t%4==0?5.f:3.f);
            g.drawLine(cx+std::sin(angle)*inner,cy-std::cos(angle)*inner,
                       cx+std::sin(angle)*rr,cy-std::cos(angle)*rr,
                       t%4==0?1.45f:.85f);
        }
        // Physical knurl, satin machined skirt and brushed charcoal face.
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff91918d),cx-r,cy-r,
                                               juce::Colour(0xff191b1d),cx+r,cy+r,false));
        g.fillEllipse(cx-r,cy-r,diam,diam);
        g.setColour(juce::Colour(0xffd5cabe).withAlpha(.42f));
        g.drawEllipse(cx-r+1.f,cy-r+1.f,diam-2.f,diam-2.f,1.f);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff636563),cx-r*.66f,cy-r*.72f,
                                               juce::Colour(0xff101315),cx+r*.7f,cy+r*.8f,false));
        const float faceR=r*.81f;
        g.fillEllipse(cx-faceR,cy-faceR,2.f*faceR,2.f*faceR);
        g.setColour(juce::Colour(0xffc7c1b4).withAlpha(.13f));
        for(int ring=1;ring<=3;++ring){
            const float rr=faceR-(float)ring*2.f;
            if(rr>2)g.drawEllipse(cx-rr,cy-rr,2*rr,2*rr,.45f);
        }
        // Illuminated white/amber position index recessed into the knob face.
        const float tip=faceR*.89f,base=faceR*.48f;
        g.setColour(juce::Colours::black.withAlpha(.75f));
        g.drawLine(cx+std::sin(valueAngle)*base+1.f,cy-std::cos(valueAngle)*base+1.f,
                   cx+std::sin(valueAngle)*tip+1.f,cy-std::cos(valueAngle)*tip+1.f,4.f);
        g.setColour(enabled?juce::Colour(0xffffe5bc):kMuted);
        g.drawLine(cx+std::sin(valueAngle)*base,cy-std::cos(valueAngle)*base,
                   cx+std::sin(valueAngle)*tip,cy-std::cos(valueAngle)*tip,2.7f);
        g.setColour(juce::Colours::white.withAlpha(.14f));
        g.drawEllipse(cx-faceR,cy-faceR,faceR*2,faceR*2,.75f);
    }
    void drawLinearSlider(juce::Graphics& g,int x,int y,int w,int h,float sliderPos,
                          float,float,juce::Slider::SliderStyle style,juce::Slider&) override {
        const bool vertical=style==juce::Slider::LinearVertical;
        auto bar=vertical?juce::Rectangle<float>((float)x+w/2.f-5.f,(float)y+8.f,10.f,(float)h-16.f)
                         :juce::Rectangle<float>((float)x+5.f,(float)y+h/2.f-4.f,(float)w-10.f,8.f);
        g.setColour(juce::Colour(0xff08090a));g.fillRoundedRectangle(bar,4.f);
        g.setColour(juce::Colour(0xff494d4e));g.drawRoundedRectangle(bar,4.f,1.f);
        if (vertical){
            const float fillTop=juce::jlimit(bar.getY(),bar.getBottom(),sliderPos);
            g.setColour(kAccent);g.fillRoundedRectangle(juce::Rectangle<float>(bar.getX()+2.f,fillTop,bar.getWidth()-4.f,bar.getBottom()-fillTop),2.f);
        }else{
            const float fillRight=juce::jlimit(bar.getX(),bar.getRight(),sliderPos);
            g.setColour(kCyan);g.fillRoundedRectangle(juce::Rectangle<float>(bar.getX()+2.f,bar.getY()+2.f,
                                         juce::jmax(0.f,fillRight-bar.getX()-2.f),bar.getHeight()-4.f),2.f);
        }
        const float sx=vertical?bar.getCentreX():sliderPos,sy=vertical?sliderPos:bar.getCentreY();
        g.setColour(juce::Colours::black.withAlpha(.7f));g.fillEllipse(sx-10.f,sy-10.f,20.f,20.f);
        g.setColour(vertical?kText:kCyan);g.fillEllipse(sx-7.f,sy-7.f,14.f,14.f);
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
        subtitle.setText("NAM  /  VST3  /  CAB IR  /  SCULPT CH      /      BETA 1.3.6", juce::dontSendNotification);
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
        tone3000Search.setButtonText("CERCA ONLINE  >");
        tone3000Search.setTooltip("Mostra i risultati su tone3000.com. Nessuna chiave API richiesta.");
        tone3000Search.onClick = [this] { openTone3000Search(); };
        tone3000Search.setColour(juce::TextButton::textColourOffId, kText);
        addAndMakeVisible(tone3000Search);
        rigButton.setButtonText("RIG  /  PRE  /  SCULPT CH  /  NAM/CAB  /  POST");
        rigButton.setTooltip("Quattro VST3 PRE, Sculpt Channel fisso prima del NAM, quattro VST3 POST.");
        rigButton.setColour(juce::TextButton::textColourOffId, kText);
        rigButton.onClick = [this] { showRigPanel(); };
        addAndMakeVisible(rigButton);
        audioButton.setButtonText("SETUP AUDIO");
        folderButton.setButtonText("CARICA NAM / CARTELLA");
        irButton.setButtonText("CARICA CABINET IR");
        previousNamButton.setButtonText("< NAM");
        nextNamButton.setButtonText("NAM >");
        previousPageButton.setButtonText("< PAGINA");
        nextPageButton.setButtonText("PAGINA >");
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
        modeLabel.setText("*  RIG MANUALE",juce::dontSendNotification);
        setAudioChannels(1, 2);
        // The single-window rig is the main component, not an additional dialog.
        rigPanel = std::make_unique<RigPanel>(*this);
        // Sculpt Channel is a mandatory fixed insert before NAM. Load installed VST3 automatically.
        bool sculptFound = false;
        for (const auto& file : VstChain::installedVst3()) {
            const auto name = file.getFileNameWithoutExtension().toLowerCase();
            if (name.contains("sculpt") && name.contains("chan")) {
                sculptFound = true;
                loadPluginSlot(VstChain::sculptSlot, file);
                if (vstChain.get(VstChain::sculptSlot) != nullptr) break;
            }
        }
        if (!sculptFound)
            status.setText("SCULPT CHANNEL non installato: seleziona il VST3 nello slot dedicato.",
                           juce::dontSendNotification);
        rigViewport.setViewedComponent(rigPanel.get(), false);
        rigViewport.setScrollBarsShown(true, true);
        addAndMakeVisible(rigViewport);
        quickTitle.setText("5 PRESET RAPIDI  /  RICHIAMA O MEMORIZZA", juce::dontSendNotification);
        quickTitle.setColour(juce::Label::textColourId, kWarm);
        quickTitle.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        addAndMakeVisible(quickTitle);
        for (int i=0;i<5;++i) {
            quickNames[(size_t)i].setColour(juce::Label::textColourId,kText);
            quickNames[(size_t)i].setFont(juce::Font(juce::FontOptions(11.f,juce::Font::bold)));
            quickNames[(size_t)i].setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(quickNames[(size_t)i]);
            quickRecall[(size_t)i].setButtonText(juce::String(i+1)+"  RICHIAMA");
            quickRecall[(size_t)i].onClick=[this,i]{loadQuickSlot(i);};
            addAndMakeVisible(quickRecall[(size_t)i]);
            quickStore[(size_t)i].setButtonText("MEMORIZZA");
            quickStore[(size_t)i].onClick=[this,i]{storeQuickSlot(i);};
            addAndMakeVisible(quickStore[(size_t)i]);
            quickClear[(size_t)i].setButtonText("X");
            quickClear[(size_t)i].setTooltip("Svuota lo slot rapido (con conferma).");
            quickClear[(size_t)i].onClick=[this,i]{confirmDeleteQuickSlot(i);};
            addAndMakeVisible(quickClear[(size_t)i]);
        }
        refreshQuickSlots();
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
        tuner.prepare(sampleRate);
        tunerMuteGain.reset(sampleRate, 0.015);
        tunerMuteGain.setCurrentAndTargetValue(tunerMute.load() ? 0.f : 1.f);
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
            // Expose the *same* host tempo/PPQ/transport to all VST3s in this block.
            vstChain.beginBlock(static_cast<double>(tempoBpm.load(std::memory_order_relaxed)));
            const float* input = audio->getReadPointer(0, start + offset);
            // Preserve dry DI for input metering. Tone choice is always manual.
            std::copy_n(input, n, dryInput.begin());
            tuner.push(dryInput.data(), n); // raw DI ahead of gate and effects
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
            // PRE 1-4 -> SCULPT CHANNEL (fixed) -> NAM, which requires mono DI.
            juce::AudioBuffer<float> vstView(fxBuffer.getArrayOfWritePointers(), 2, n);
            for (int si=0;si<4;++si) vstChain.process(si, vstView);
            vstChain.process(VstChain::sculptSlot, vstView);
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
                const float gi = gainFromDb(juce::jlimit(-24.f, 24.f, entry.recommendedInput + namInputGainDb.load(std::memory_order_relaxed)));
                const float go = 0.25f * gainFromDb(juce::jlimit(-24.f, 24.f, entry.recommendedOutput + namOutputGainDb.load(std::memory_order_relaxed)));
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
                    const float gi = gainFromDb(juce::jlimit(-24.f,24.f,oldEntry.recommendedInput + namInputGainDb.load(std::memory_order_relaxed)));
                    oldGain = 0.25f * gainFromDb(juce::jlimit(-24.f,24.f,oldEntry.recommendedOutput + namOutputGainDb.load(std::memory_order_relaxed)));
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
            // Cabinet IR -> four independent POST VST3, preserving their stereo output.
            fxBuffer.copyFrom(0, 0, outA.data(), n);
            fxBuffer.copyFrom(1, 0, outA.data(), n);
            for (int si=5;si<9;++si) vstChain.process(si, vstView);
            // Final master gain: after all nine VST3 inserts; preserve stereo post effects.
            const float masterDb = masterGainDb.load(std::memory_order_relaxed);
            masterGain.setTargetValue(masterDb <= -60.f ? 0.f : gainFromDb(masterDb));
            tunerMuteGain.setTargetValue(tunerMute.load(std::memory_order_relaxed) ? 0.f : 1.f);
            auto* left = audio->getWritePointer(0, start + offset);
            auto* right = numChannels > 1 ? audio->getWritePointer(1, start + offset) : nullptr;
            const auto* processedL = fxBuffer.getReadPointer(0);
            const auto* processedR = fxBuffer.getReadPointer(1);
            double outEnergy = 0.0, outEnergyL=0.0, outEnergyR=0.0;
            for (int i=0;i<n;++i) {
                const float gain = masterGain.getNextValue() * tunerMuteGain.getNextValue();
                const float l = juce::jlimit(-0.85f, 0.85f, processedL[i] * gain);
                const float r = right != nullptr ? juce::jlimit(-0.85f,0.85f,processedR[i]*gain) : l;
                left[i] = l;
                if (right != nullptr) right[i] = r;
                outEnergyL += double(l)*double(l);
                outEnergyR += double(r)*double(r);
                outEnergy += 0.5 * (double(l)*double(l) + double(r)*double(r));
            }
            outputDbLeft.store(static_cast<float>(20.0*std::log10(std::max(1.e-8,std::sqrt(outEnergyL/std::max(1,n))))),
                               std::memory_order_relaxed);
            outputDbRight.store(static_cast<float>(20.0*std::log10(std::max(1.e-8,std::sqrt(outEnergyR/std::max(1,n))))),
                                std::memory_order_relaxed);
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
            vstChain.endBlock(n); // Advance timeline exactly once, after PRE + POST.
            offset += n;
        }
    }

    void releaseResources() override { vstChain.release(); cab.reset(); }

    void paint(juce::Graphics& g) override {
        g.fillAll(kBg);
        const float yy=(float)getHeight()-108.f;
        auto frame=juce::Rectangle<float>(10.f,yy+2.f,(float)getWidth()-20.f,98.f);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff252323),10.f,yy,
                            juce::Colour(0xff101112),10.f,yy+105.f,false));
        g.fillRoundedRectangle(frame,10.f);
        g.setColour(kWarm.withAlpha(.58f));g.drawRoundedRectangle(frame,10.f,1.f);
        const int pad=18,gap=8,tile=(getWidth()-2*pad-4*gap)/5;
        for(int i=0;i<5;++i){
            const int x=pad+i*(tile+gap);
            auto r=juce::Rectangle<float>((float)x,(float)getHeight()-81.f,(float)tile,72.f);
            g.setGradientFill(juce::ColourGradient(juce::Colour(0xff272828),r.getX(),r.getY(),
                                                  juce::Colour(0xff121415),r.getX(),r.getBottom(),false));
            g.fillRoundedRectangle(r,6.f);
            g.setColour(juce::Colour(0xff686159).withAlpha(.38f));g.drawRoundedRectangle(r,6.f,1.f);
            g.setColour(kAccent);g.fillEllipse(r.getX()+8.f,r.getY()+10.f,8.f,8.f);
            drawHardwareScrew(g,r.getRight()-10.f,r.getY()+11.f,3.4f);
            drawHardwareScrew(g,r.getRight()-10.f,r.getBottom()-11.f,3.4f);
        }
    }

    void resized() override {
        const auto bounds = getLocalBounds();
        constexpr int bankHeight = 108;
        rigViewport.setBounds(bounds.withTrimmedBottom(bankHeight));
        if (rigPanel != nullptr)
            rigPanel->setSize(juce::jmax(1510, bounds.getWidth()),
                              juce::jmax(824, bounds.getHeight()-bankHeight));
        const int footerTop=bounds.getBottom()-bankHeight;
        quickTitle.setBounds(24,footerTop+8,700,17);
        const int pad=18, gap=8;
        const int tile=(bounds.getWidth()-2*pad-4*gap)/5;
        for(int i=0;i<5;++i){
            const int x=pad+i*(tile+gap);
            quickNames[(size_t)i].setBounds(x+22,footerTop+32,tile-75,17);
            quickClear[(size_t)i].setBounds(x+tile-59,footerTop+29,26,22);
            quickRecall[(size_t)i].setBounds(x+8,footerTop+54,(tile*2)/3-4,32);
            quickStore[(size_t)i].setBounds(x+(tile*2)/3+5,footerTop+54,tile/3-12,32);
        }
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
        HostedEditorWindow(juce::AudioPluginInstance& processor,
                           std::function<void(const juce::Image&)> snapshotSink)
            : juce::DocumentWindow(processor.getName(), kBg, juce::DocumentWindow::closeButton),
              onSnapshot(std::move(snapshotSink)) {
            auto* editor = processor.createEditorIfNeeded();
            if (editor == nullptr) editor = new juce::GenericAudioProcessorEditor(processor);
            editorView=editor;
            setContentOwned(editor, true);
            setUsingNativeTitleBar(true);
            setResizable(false,false);
            centreWithSize(juce::jmax(450,editor->getWidth()), juce::jmax(280,editor->getHeight()));
        }
        void closeButtonPressed() override {
            // A still image only: no VST editor is embedded inside the rack.
            // Some GPU/native editors cannot be snapshotted; the rack has a styled fallback.
            if (editorView != nullptr && onSnapshot && editorView->getWidth()>0 && editorView->getHeight()>0) {
                const auto image=editorView->createComponentSnapshot(editorView->getLocalBounds(),true,.55f);
                if (image.isValid()) onSnapshot(image);
            }
            setVisible(false);
        }
    private:
        juce::AudioProcessorEditor* editorView=nullptr; // Owned by DocumentWindow content
        std::function<void(const juce::Image&)> onSnapshot;
    };

    // Searchable plugin catalogue. Stays inside the rig window (no extra host window).
    // The search is by VST3 bundle name; only choosing a row loads any code.
    class VstPickerOverlay final : public juce::Component, private juce::ListBoxModel {
    public:
        VstPickerOverlay() : results("VST3 disponibili", this) {
            setOpaque(false);
            title.setText("SELEZIONA PLUGIN VST3",juce::dontSendNotification);
            title.setColour(juce::Label::textColourId,kText);
            title.setFont(juce::Font(juce::FontOptions(20.f,juce::Font::bold)));
            addAndMakeVisible(title);
            caption.setText("Digita il nome. Elenco alfabetico A-Z.",juce::dontSendNotification);
            caption.setColour(juce::Label::textColourId,kMuted);
            caption.setFont(juce::Font(juce::FontOptions(12.f)));
            addAndMakeVisible(caption);
            query.setTextToShowWhenEmpty("Cerca plugin (es. delay, compressor, fabfilter)...",kMuted);
            query.setReturnKeyStartsNewLine(false);
            query.setColour(juce::TextEditor::backgroundColourId,juce::Colour(0xff12171a));
            query.setColour(juce::TextEditor::textColourId,kText);
            query.setColour(juce::TextEditor::outlineColourId,kWarm);
            query.setColour(juce::TextEditor::focusedOutlineColourId,kWarm);
            query.setColour(juce::CaretComponent::caretColourId,kWarm);
            query.setFont(juce::Font(juce::FontOptions(15.f)));
            query.onTextChange=[this]{filterResults();};
            query.onReturnKey=[this]{chooseCurrent();};
            query.onEscapeKey=[this]{close();};
            addAndMakeVisible(query);
            results.setOpaque(true);
            results.setColour(juce::ListBox::backgroundColourId,juce::Colour(0xff101315));
            results.setColour(juce::ListBox::outlineColourId,kWarm.withAlpha(.35f));
            results.setColour(juce::ListBox::textColourId,kText);
            results.setRowHeight(40);
            results.setOutlineThickness(1);
            addAndMakeVisible(results);
            count.setColour(juce::Label::textColourId,kMuted);
            count.setFont(juce::Font(juce::FontOptions(11.5f)));
            addAndMakeVisible(count);
            choose.setButtonText("CARICA VST3");
            choose.onClick=[this]{chooseCurrent();};
            addAndMakeVisible(choose);
            browse.setButtonText("SFOGLIA FILE .VST3...");
            browse.onClick=[this]{const auto slot=activeSlot;close();if(onBrowse)onBrowse(slot);};
            addAndMakeVisible(browse);
            dismiss.setButtonText("ANNULLA");
            dismiss.onClick=[this]{close();};
            addAndMakeVisible(dismiss);
            setVisible(false);
        }
        ~VstPickerOverlay() override { results.setModel(nullptr); }
        void open(int slot,const juce::Array<juce::File>& paths,
                  std::function<void(int,const juce::File&)> selectFn,
                  std::function<void(int)> browseFn) {
            activeSlot=slot;
            onChoose=std::move(selectFn);onBrowse=std::move(browseFn);
            candidates.clear();
            for(const auto& p:paths){
                if(slot==VstChain::sculptSlot){
                    const auto lower=p.getFileNameWithoutExtension().toLowerCase();
                    if(!lower.contains("sculpt") || !(lower.contains("channel")||lower.contains("chan")))continue;
                }
                candidates.push_back(p);
            }
            std::stable_sort(candidates.begin(),candidates.end(),[](const juce::File& a,const juce::File& b){
                const auto na=a.getFileNameWithoutExtension().toLowerCase();
                const auto nb=b.getFileNameWithoutExtension().toLowerCase();
                const int byName=na.compare(nb);
                return byName==0 ? a.getFullPathName().compareIgnoreCase(b.getFullPathName())<0 : byName<0;
            });
            title.setText(slot==VstChain::sculptSlot?"SCULPT CHANNEL VST3":
                "SCEGLI VST3 - "+juce::String(slot<4?"PRE ":"POST ")+juce::String(slot<4?slot+1:slot-4),
                juce::dontSendNotification);
            query.clear();
            filterResults();
            setVisible(true);
            toFront(true);
            query.grabKeyboardFocus();
        }
        void close(){setVisible(false);onChoose=nullptr;onBrowse=nullptr;}
        int getNumRows() override {return (int)filtered.size();}
        void paintListBoxItem(int row,juce::Graphics& g,int width,int height,bool selected) override {
            if(row<0 || row>=(int)filtered.size())return;
            if(selected){
                g.setColour(juce::Colour(0xff3b2c1f));g.fillRect(0,0,width,height);
                g.setColour(kWarm);g.fillRect(0,0,3,height);
            }else if(row%2==0){g.setColour(juce::Colour(0xff1b1f20));g.fillRect(0,0,width,height);}
            const auto& file=filtered[(size_t)row];
            g.setColour(selected?kWarm:kText);
            g.setFont(juce::Font(juce::FontOptions(13.7f,juce::Font::bold)));
            g.drawFittedText(file.getFileNameWithoutExtension(),12,3,width-24,19,juce::Justification::centredLeft,1,.9f);
            g.setColour(kMuted);g.setFont(juce::Font(juce::FontOptions(10.f)));
            g.drawFittedText(file.getParentDirectory().getFullPathName(),12,23,width-24,14,juce::Justification::centredLeft,1,.65f);
            g.setColour(juce::Colours::white.withAlpha(.05f));g.drawLine(10.f,(float)height-1.f,(float)width-10.f,(float)height-1.f,.6f);
        }
        void listBoxItemDoubleClicked(int row,const juce::MouseEvent&) override {chooseRow(row);}
        void returnKeyPressed(int row) override {chooseRow(row);}
        void paint(juce::Graphics& g) override {
            g.fillAll(juce::Colours::black.withAlpha(.76f));
            auto p=dialogBounds().toFloat();
            g.setGradientFill(juce::ColourGradient(juce::Colour(0xff353331),p.getX(),p.getY(),
                                                juce::Colour(0xff0c0e10),p.getRight(),p.getBottom(),false));
            g.fillRoundedRectangle(p,12.f);
            g.setColour(kWarm.withAlpha(.85f));g.drawRoundedRectangle(p,12.f,1.4f);
            drawHardwareScrew(g,p.getX()+14,p.getY()+14,4.5f);
            drawHardwareScrew(g,p.getRight()-14,p.getY()+14,4.5f);
            drawHardwareScrew(g,p.getX()+14,p.getBottom()-14,4.5f);
            drawHardwareScrew(g,p.getRight()-14,p.getBottom()-14,4.5f);
            drawGrille(g,{p.getX()+28,p.getBottom()-14,p.getWidth()-56,5},kWarm);
        }
        void resized() override {
            const auto b=dialogBounds();
            title.setBounds(b.getX()+30,b.getY()+24,b.getWidth()-60,27);
            caption.setBounds(b.getX()+30,b.getY()+53,b.getWidth()-60,18);
            query.setBounds(b.getX()+30,b.getY()+85,b.getWidth()-60,40);
            results.setBounds(b.getX()+30,b.getY()+137,b.getWidth()-60,b.getHeight()-233);
            count.setBounds(b.getX()+30,b.getBottom()-90,b.getWidth()-60,19);
            browse.setBounds(b.getX()+30,b.getBottom()-60,170,35);
            dismiss.setBounds(b.getRight()-213,b.getBottom()-60,82,35);
            choose.setBounds(b.getRight()-126,b.getBottom()-60,96,35);
        }
        void mouseDown(const juce::MouseEvent& e) override {
            if(!dialogBounds().contains(e.getPosition()))close();
        }
    private:
        juce::Rectangle<int> dialogBounds() const {
            const int w=juce::jmin(610,juce::jmax(420,getWidth()-60));
            const int h=juce::jmin(620,juce::jmax(410,getHeight()-80));
            return {(getWidth()-w)/2,(getHeight()-h)/2,w,h};
        }
        void filterResults(){
            filtered.clear();
            const auto needle=query.getText().trim();
            for(const auto& p:candidates)
                if(needle.isEmpty() || p.getFileNameWithoutExtension().containsIgnoreCase(needle))
                    filtered.push_back(p);
            results.updateContent();
            if(!filtered.empty())results.selectRow(0);
            count.setText(juce::String(filtered.size())+" plugin trovati ("+juce::String(candidates.size())+
                         " installati). Doppio clic o Invio per caricare.",juce::dontSendNotification);
            choose.setEnabled(!filtered.empty());
        }
        void chooseCurrent(){chooseRow(results.getSelectedRow());}
        void chooseRow(int row){
            if(row<0||row>=(int)filtered.size())return;
            const int slot=activeSlot;
            const auto file=filtered[(size_t)row];
            auto callback=onChoose;
            close();
            if(callback)callback(slot,file);
        }
        int activeSlot=-1;
        std::vector<juce::File> candidates,filtered;
        std::function<void(int,const juce::File&)> onChoose;
        std::function<void(int)> onBrowse;
        juce::Label title,caption,count;
        juce::TextEditor query;
        juce::ListBox results;
        juce::TextButton choose,browse,dismiss;
    };

    class RigPanel final : public juce::Component, private juce::Timer {
        // Transparent hit-targets make the hardware-themed footswitches and
        // plugin mini-displays respond without painting opaque widgets over artwork.
        class InvisibleButton final : public juce::Button {
        public:
            InvisibleButton() : juce::Button("Rack hardware control") {}
            void paintButton(juce::Graphics&, bool, bool) override {}
        };
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
            label(brand,"SCULPT GUITAR RIG",25.f,kText);
            label(subtitle,"Standalone NAM + VST3 Rig",11.5f,kMuted);
            label(status,"",12.f,kMuted);
            label(inputDeviceInfo,"Dispositivo: configurazione audio",11.5f,kText);
            label(outputDeviceInfo,"Dispositivo: configurazione audio",11.5f,kText);
            label(inputPortInfo,"INPUT  /  canale selezionato",10.6f,kMuted);
            label(outputPortInfo,"OUTPUT  /  canali selezionati",10.6f,kMuted);
            inputDeviceInfo.setJustificationType(juce::Justification::centred);
            outputDeviceInfo.setJustificationType(juce::Justification::centred);
            label(inMeter,"-90.0 dBFS",15.f,kGreen);
            label(outMeter,"-90.0 dBFS",15.f,kGreen);
            label(namName,"NAM: BYPASS",15.f,kText);
            label(irName,"IR: NON CARICATA",12.f,kMuted);
            label(sculptName,"SCULPT CH: NON CARICATO",11.f,kMuted);
            label(gateHeading,"NOISE GATE",12.f,kWarm);
            label(tunerHeading,"ACCORDATORE  /  DI",11.7f,kWarm);
            label(tunerNote,"--",30.f,kText);
            tunerNote.setJustificationType(juce::Justification::centred);
            label(tunerInfo,"-- Hz / -- cents",11.f,kMuted);
            tunerInfo.setJustificationType(juce::Justification::centred);
            tunerButton.setButtonText("TUNER / MUTE");
            tunerButton.onClick = [this] {
                owner.tunerMute.store(!owner.tunerMute.load(std::memory_order_relaxed), std::memory_order_relaxed);
                updateLabels();
            };
            addAndMakeVisible(tunerButton);
            label(masterHeading,"MASTER OUTPUT",12.f,kWarm);
            label(masterReadout,"0.0 dB",14.f,kText);
            label(tapHeading,"TAP / HOST SYNC",12.f,kWarm);
            label(tapReadout,"120 BPM",22.f,kText);
            tapReadout.setJustificationType(juce::Justification::centred);
            tapButton.setButtonText("TAP TO SYNC");
            tapButton.onClick = [this] { recordTap(); };
            addAndMakeVisible(tapButton);
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
            toneSearch.setButtonText("CERCA >");
            toneSearch.onClick = [this] { openToneSearch(); };
            addAndMakeVisible(toneSearch);
            label(presetHeading,"PRESET RIG",11.5f,kWarm);
            presetPicker.addItem("NUOVO PRESET...",1);
            presetPicker.onChange=[this]{
                const int id=presetPicker.getSelectedId();
                if(id>=2 && id-2<presetCatalog.size()){
                    const auto name=presetCatalog[id-2];
                    if(owner.loadNamedPreset(name)) presetNameEditor.setText(name,juce::dontSendNotification);
                } else if(id==1) presetNameEditor.clear();
            };
            addAndMakeVisible(presetPicker);
            presetNameEditor.setTextToShowWhenEmpty("Nome preset...",kMuted);
            presetNameEditor.setColour(juce::TextEditor::backgroundColourId,kPanelAlt);
            presetNameEditor.setColour(juce::TextEditor::textColourId,kText);
            presetNameEditor.onReturnKey=[this]{savePreset();};
            addAndMakeVisible(presetNameEditor);
            presetSave.setButtonText("SALVA");presetSave.onClick=[this]{savePreset();};
            addAndMakeVisible(presetSave);
            presetRefresh.setButtonText("R");presetRefresh.setTooltip("Aggiorna l'elenco preset");
            presetRefresh.onClick=[this]{refreshPresets(true);};
            addAndMakeVisible(presetRefresh);
            presetDelete.setButtonText("ELIMINA");
            presetDelete.setTooltip("Scegli un preset da eliminare, senza doverlo caricare. Conferma richiesta.");
            presetDelete.onClick=[this]{choosePresetToDelete();};
            addAndMakeVisible(presetDelete);
            refreshPresets(true);

            inputSetup.setButtonText("SETUP AUDIO & MIDI  >");
            outputSetup.setButtonText("SETUP AUDIO & MIDI  >");
            inputSetup.onClick = [this] { owner.showAudioSettings(); };
            outputSetup.onClick = [this] { owner.showAudioSettings(); };
            addAndMakeVisible(inputSetup); addAndMakeVisible(outputSetup);
            gateBypass.setButtonText("GATE ON");
            gateBypass.onClick = [this] { owner.gateEnabled.store(!owner.gateEnabled.load()); updateLabels(); };
            addAndMakeVisible(gateBypass);
            gateThreshold.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            gateThreshold.setTextBoxStyle(juce::Slider::TextBoxBelow,false,80,20);
            gateThreshold.setTextValueSuffix(" dB");
            gateThreshold.setRange(-85.,-15.,1.);
            gateThreshold.setValue(owner.gateThresholdDb.load(),juce::dontSendNotification);
            gateThreshold.onValueChange = [this]{owner.gateThresholdDb.store((float)gateThreshold.getValue());};
            addAndMakeVisible(gateThreshold);
            masterSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
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
            prevNam.setButtonText("<"); prevNam.onClick=[this]{owner.changeNam(-1);}; addAndMakeVisible(prevNam);
            nextNam.setButtonText(">"); nextNam.onClick=[this]{owner.changeNam(1);}; addAndMakeVisible(nextNam);
            label(namInputHeading,"NAM INPUT",11.f,kWarm);
            label(namOutputHeading,"NAM OUTPUT",11.f,kWarm);
            for(int i=0;i<2;++i){
                auto& slider=namGains[(size_t)i];
                slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
                slider.setTextBoxStyle(juce::Slider::TextBoxBelow,false,64,21);
                slider.setDoubleClickReturnValue(true,0.0);
                slider.setRange(-12.,12.,.5);
                slider.setTextValueSuffix(" dB");
                slider.setValue(0.,juce::dontSendNotification);
                slider.onValueChange=[this,i]{
                    if(i==0)owner.namInputGainDb.store((float)namGains[0].getValue());
                    else owner.namOutputGainDb.store((float)namGains[1].getValue());
                };
                addAndMakeVisible(slider);
            }

            // Searchable A-Z picker replaces the unreadable long ComboBox list.
            // Every effect is loaded explicitly; the DSP and preset model are unchanged.
            pluginPaths = VstChain::installedVst3();
            for(int i=0;i<9;++i) {
                const int index=i;
                const juce::String moduleTitle = i<4 ? "PRE " + juce::String(i+1) :
                    (i==4 ? "SCULPT CH" : "POST " + juce::String(i-4));
                label(slotNames[(size_t)i],moduleTitle,12.f,kText);
                label(slotStatus[(size_t)i],"VUOTO",10.5f,kMuted);
                auto& selector=slotMenus[(size_t)i];
                selector.setButtonText(i==VstChain::sculptSlot?"SCULPT CHANNEL VST3":"Scegli VST3...");
                selector.setTooltip("Digita il nome del plugin: ricerca alfabetica A-Z");
                selector.onClick=[this,index]{showVstSearch(index);};
                addAndMakeVisible(selector);
                if (i != VstChain::sculptSlot) {
                    auto& foot = footswitches[(size_t)i];
                    foot.setTooltip("Footswitch: attiva / bypassa il VST3 caricato");
                    foot.onClick=[this,index]{
                        if(owner.vstChain.get(index)!=nullptr){
                            owner.vstChain.enable(index,!owner.vstChain.enabled(index));
                            updateLabels();
                        }
                    };
                    addAndMakeVisible(foot);
                    auto& preview = previewButtons[(size_t)i];
                    preview.setTooltip("Anteprima rack: clicca per aprire l'editor VST3 (o caricare un plugin)");
                    preview.onClick=[this,index]{
                        if(owner.vstChain.get(index)!=nullptr)owner.openPluginEditor(index);
                        else showVstSearch(index);
                    };
                    addAndMakeVisible(preview);
                }
                auto& clear=clearButtons[(size_t)i]; clear.setButtonText("X"); clear.onClick=[this,index]{
                    if (index == VstChain::sculptSlot) return;
                    owner.removeVstSlot(index);updateLabels();
                }; addAndMakeVisible(clear);
                if(i==VstChain::sculptSlot) clear.setVisible(false);
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
            sculptFaceButton.setTooltip("Apri l'editor Sculpt Channel. Se assente, cerca il VST3.");
            sculptFaceButton.onClick=[this]{
                if(owner.vstChain.get(VstChain::sculptSlot)!=nullptr)
                    owner.openPluginEditor(VstChain::sculptSlot);
                else showVstSearch(VstChain::sculptSlot);
            };
            addAndMakeVisible(sculptFaceButton);
            variation.setButtonText("VARIATION OFF");
            variation.onClick = [this] { setSculptVariation(); };
            addAndMakeVisible(variation);
            // Last child: visually modal within the existing application window.
            addChildComponent(vstFinder);
            vstFinder.setVisible(false);
            setSize(1580,860);
            startTimerHz(6);
            updateLabels();
        }
        ~RigPanel() override {stopTimer();}

        void showVstSearch(int slot){
            // Re-enumerate: an installed VST3 may have been added since app launch.
            pluginPaths=VstChain::installedVst3();
            vstFinder.setBounds(getLocalBounds());
            vstFinder.open(slot,pluginPaths,
                [safe=juce::Component::SafePointer<RigPanel>(this)](int index,const juce::File& file){
                    if(safe!=nullptr){safe->owner.loadPluginSlot(index,file);safe->updateLabels();}
                },
                [safe=juce::Component::SafePointer<RigPanel>(this)](int index){
                    if(safe!=nullptr)safe->owner.browsePluginBundle(index);
                });
        }

        void setPluginSnapshot(int slot, const juce::Image& image) {
            if (slot<0 || slot>=9 || slot==VstChain::sculptSlot || !image.isValid())return;
            // Cache at reduced resolution, preventing an oversized editor framebuffer.
            const int targetW=juce::jmin(640,image.getWidth());
            const int targetH=juce::jmax(1,(int)std::round((double)image.getHeight()*targetW/image.getWidth()));
            pluginSnapshots[(size_t)slot]=image.rescaled(targetW,targetH);
            previewIds[(size_t)slot]=owner.vstChain.bundlePath(slot).getFullPathName()+"|"+owner.vstChain.name(slot);
            repaint(slotRects[(size_t)slot]);
        }

        void invalidateAllPluginSnapshots() {
            for(auto& im:pluginSnapshots) im=juce::Image();
            for(auto& id:previewIds) id.clear();
            repaint();
        }

        void paint(juce::Graphics& g) override {
            g.fillAll(kBg);
            g.setGradientFill(juce::ColourGradient(juce::Colour(0xff212426),0.f,0.f,
                                                   kBg,0.f,80.f,false));
            g.fillRect(juce::Rectangle<float>(0,0,(float)getWidth(),74.f));
            // Native window controls remain owned by macOS; the header is an app toolbar.
            g.setColour(kAccent);
            g.fillRoundedRectangle(juce::Rectangle<float>(19.f,14.f,4.f,44.f),2.f);
            g.setColour(kWarm.withAlpha(.32f));
            g.drawLine(102.f,64.f,430.f,64.f,.8f);
            // Standalone titlebar engine indicator, as in the approved visual reference.
            const float ledX=(float)getWidth()-39.f;
            g.setColour(kGreen.withAlpha(.18f));g.fillEllipse(ledX-7.f,21.f,14.f,14.f);
            g.setColour(kGreen);g.fillEllipse(ledX-3.5f,24.5f,7.f,7.f);

            g.setColour(juce::Colours::black.withAlpha(.72f));
            g.drawLine(0,74.f,(float)getWidth(),74.f,2.f);

            const std::array<juce::String,6> names {{
                "INPUT", "PRE VST3", "SCULPT CH (FISSO)","NAM / CAB", "POST VST3", "OUTPUT"}};
            const std::array<juce::String,6> subtitles {{
                "AUDIO  /  MIDI  /  TUNER", "PLUGIN VST3  /  PRE AMP", "CHANNEL PROCESSOR",
                "CATTURE NAM + IR", "PLUGIN VST3  /  POST AMP", "AUDIO  /  MASTER"}};
            for(int i=0;i<6;++i){
                const auto r=sections[(size_t)i].toFloat();
                if(r.isEmpty())continue;
                g.setGradientFill(juce::ColourGradient(juce::Colour(0xff262626),r.getX(),r.getY(),
                                                        juce::Colour(0xff111313),r.getRight(),r.getBottom(),false));
                g.fillRoundedRectangle(r,10.f);
                g.setColour(juce::Colour(0xff777571).withAlpha(.28f));
                g.drawRoundedRectangle(r.reduced(0.6f),10.f,1.2f);
                g.setColour(kWarm.withAlpha(.50f));
                g.drawLine(r.getX()+20.f,r.getY()+1.5f,r.getRight()-20.f,r.getY()+1.5f,1.2f);
                g.setColour(juce::Colours::white.withAlpha(.023f));
                for(int yy=8;yy<(int)r.getHeight()-10;yy+=5)
                    g.drawLine(r.getX()+6.f,r.getY()+yy,r.getRight()-6.f,r.getY()+yy,.55f);
                g.setColour(juce::Colours::black.withAlpha(.36f));
                g.drawRoundedRectangle(r.reduced(4.f),8.f,.8f);
                g.setColour(kWarm);
                g.setFont(juce::Font(juce::FontOptions(20.f,juce::Font::bold)));
                g.drawText(juce::String(i+1)+".",r.getX()+14.f,r.getY()+12.f,28.f,29.f,juce::Justification::centredLeft);
                g.setColour(kText);
                g.setFont(juce::Font(juce::FontOptions(15.2f,juce::Font::bold)));
                g.drawFittedText(names[(size_t)i],
                    juce::Rectangle<int>((int)r.getX()+45,(int)r.getY()+12,(int)r.getWidth()-51,27),
                    juce::Justification::centredLeft,1,.7f);
                g.setColour(kMuted);
                g.setFont(juce::Font(juce::FontOptions(10.5f,juce::Font::plain)));
                g.drawFittedText(subtitles[(size_t)i],
                    juce::Rectangle<int>((int)r.getX()+45,(int)r.getY()+40,(int)r.getWidth()-51,16),
                    juce::Justification::centredLeft,1,.7f);
                // Four large, machined bolts and continuous edging on every module.
                for(float xx:{r.getX()+9.f,r.getRight()-9.f})
                    for(float yy:{r.getY()+10.f,r.getBottom()-10.f})drawHardwareScrew(g,xx,yy,4.f);
                const auto vent=juce::Rectangle<float>(r.getX()+19.f,r.getBottom()-28.f,r.getWidth()-38.f,7.f);
                if(vent.getWidth()>30)drawGrille(g,vent,kWarm);
                g.setColour(kWarm.withAlpha(.14f));
                g.drawLine(r.getX()+4.f,r.getY()+65.f,r.getRight()-4.f,r.getY()+65.f,.9f);
                g.drawLine(r.getX()+4.f,r.getBottom()-34.f,r.getRight()-4.f,r.getBottom()-34.f,.9f);
            }
            if(sections[0].isEmpty() || sections[5].isEmpty())return;
            const auto in=sections[0],out=sections[5],sculpt=sections[2],nam=sections[3];
            auto subhead=[&](juce::Rectangle<int> r,int yy,const juce::String& t){
                g.setColour(kText);g.setFont(juce::Font(juce::FontOptions(11.8f,juce::Font::bold)));
                g.drawText(t,r.getX()+16,yy,r.getWidth()-32,20,juce::Justification::centredLeft);
            };
            auto rule=[&](juce::Rectangle<int> r,int yy){
                g.setColour(juce::Colour(0xff8c8170).withAlpha(.18f));
                g.drawLine((float)r.getX()+14.f,(float)yy,(float)r.getRight()-14.f,(float)yy,.9f);
            };
            auto ledMeter=[&](juce::Rectangle<int> r,float db,int localY,bool mono){
                // INPUT reads dryInput/levelDb before gate, NAM and every VST3.
                // OUTPUT reads processedL/R after all effects and master.
                const int x=r.getCentreX()-(mono?19:28);
                const int y=r.getY()+localY;
                const int width=mono?93:108;
                g.setColour(kBg);g.fillRoundedRectangle(juce::Rectangle<float>((float)x-10.f,(float)y-8.f,(float)width,130.f),6.f);
                for(int n=0;n<13;++n){
                    const float yy=(float)y+(12-n)*8.8f;
                    const auto color=n>=11?juce::Colour(0xffed4e36):n>=8?kWarm:kGreen;
                    const int columns=mono?1:2;
                    for(int ch=0;ch<columns;++ch){
                        const float channelDb=mono?db:(ch==0?owner.outputDbLeft.load():owner.outputDbRight.load());
                        const bool lit=(channelDb> -60.f+(float)n*5.f);
                        g.setColour(lit?color:color.withAlpha(.18f));
                        g.fillRect(x+ch*23,(int)yy,mono?29:14,6);
                    }
                }
                g.setColour(kMuted);g.setFont(juce::Font(juce::FontOptions(10.f)));
                for(int n=0;n<4;++n){
                    static constexpr int ticks[] = {0,-12,-36,-60};
                    g.drawText(juce::String(ticks[n]),x+(mono?39:49),y-8+n*37,30,14,juce::Justification::centredLeft);
                }
                g.setColour(mono?kGreen:kWarm);
                g.drawText(mono?"DI IN":"L     R",x-5,y+116,60,15,juce::Justification::centred);
            };
            ledMeter(in,owner.levelDb.load(),214,true);
            ledMeter(out,owner.outputDb.load(),214,false);
            subhead(in,in.getY()+198,"LIVELLO INPUT");
            subhead(out,out.getY()+198,"LIVELLO OUTPUT");
            rule(in,in.getY()+387);rule(out,out.getY()+387);
            rule(in,in.getY()+563);rule(out,out.getY()+594);
            // Chromatic tuner indicators are genuinely driven by the DI detector.
            const auto nx=(float)in.getCentreX();
            const auto tunerY=(float)in.getY()+455.f;
            for(int side : {-1,1}){
                const auto cents=juce::jlimit(-35.f,35.f,tunerCurrentCents);
                const bool inTune=tunerPitchValid && std::abs(cents)<5.0f;
                g.setColour(inTune?kGreen:kWarm.withAlpha(.7f));
                g.fillRoundedRectangle(juce::Rectangle<float>(nx+(float)side*73.f-2.f,tunerY-16.f,4.f,45.f),2.f);
            }
            g.setColour(kMuted.withAlpha(.45f));
            g.drawLine(in.getX()+18.f,tunerY+47.f,in.getRight()-18.f,tunerY+47.f,1.f);
            g.setColour(tunerPitchValid && std::abs(tunerCurrentCents)<5.f?kGreen:kWarm);
            const float marker=nx+juce::jlimit(-50.f,50.f,tunerCurrentCents)/50.f*(in.getWidth()-48.f)/2.f;
            (void) marker;

            // Precision-cut hardware fasteners and grille around the channel DSP.
            for(float yy:{sculpt.getY()+151.f,sculpt.getY()+241.f,sculpt.getBottom()-58.f})
                for(float xx:{sculpt.getX()+20.f,sculpt.getRight()-20.f})drawHardwareScrew(g,xx,yy,5.f);
            drawGrille(g,juce::Rectangle<float>((float)sculpt.getX()+20.f,(float)sculpt.getY()+231.f,
                                     (float)sculpt.getWidth()-40.f,7.f),kWarm);
            // The faceplate is the editor button: engraved typography, no EDIT control.
            const auto proc=juce::Rectangle<float>((float)sculpt.getX()+14.f,(float)sculpt.getY()+169.f,
                                                    (float)sculpt.getWidth()-28.f,57.f);
            g.setColour(juce::Colours::black.withAlpha(.62f));
            g.fillRoundedRectangle(proc.expanded(1.5f),5.f);
            g.setGradientFill(juce::ColourGradient(juce::Colour(0xff4f4a41),proc.getX(),proc.getY(),
                                                  juce::Colour(0xff131619),proc.getRight(),proc.getBottom(),false));
            g.fillRoundedRectangle(proc,4.f);
            g.setColour(juce::Colour(0xffafa18a).withAlpha(.65f));
            g.drawRoundedRectangle(proc.reduced(.6f),4.f,1.f);
            g.setColour(juce::Colour(0xfff3d19f));
            g.setFont(juce::Font(juce::FontOptions(18.2f,juce::Font::bold)));
            g.drawFittedText("SCULPT",juce::Rectangle<int>((int)proc.getX()+14,(int)proc.getY()+5,
                             (int)proc.getWidth()-28,24),juce::Justification::centred,1,.9f);
            g.setColour(kText.withAlpha(.82f));
            g.setFont(juce::Font(juce::FontOptions(9.2f,juce::Font::bold)));
            g.drawFittedText("C H A N N E L",juce::Rectangle<int>((int)proc.getX()+11,
                             (int)proc.getY()+31,(int)proc.getWidth()-22,17),
                             juce::Justification::centred,1,.85f);
            drawHardwareScrew(g,proc.getX()+8.f,proc.getCentreY(),3.f);
            drawHardwareScrew(g,proc.getRight()-8.f,proc.getCentreY(),3.f);
            g.setColour(kGreen);g.fillEllipse(sculpt.getX()+19.f,sculpt.getY()+116.f,8.f,8.f);

            // A real cabinet photograph/illustration borrowed from the approved concept.
            // Single compact image atlas: photo-real decorative hardware details.
            static const juce::Image skinAtlas=juce::ImageCache::getFromMemory(
                BinaryData::SkinAtlas_png,BinaryData::SkinAtlas_pngSize);
            if(skinAtlas.isValid()){
                const auto cabR=juce::Rectangle<float>((float)nam.getX()+18.f,(float)nam.getY()+476.f,
                                                       (float)nam.getWidth()-36.f,180.f);
                g.drawImage(skinAtlas,(int)cabR.getX(),(int)cabR.getY(),(int)cabR.getWidth(),
                            (int)cabR.getHeight(),0,360,330,180);
                g.setColour(kWarm.withAlpha(.50f));g.drawRoundedRectangle(cabR.reduced(.5f),5.f,1.1f);
                drawHardwareScrew(g,cabR.getX()+7.f,cabR.getY()+7.f,3.f);
                drawHardwareScrew(g,cabR.getRight()-7.f,cabR.getY()+7.f,3.f);
                drawHardwareScrew(g,cabR.getX()+7.f,cabR.getBottom()-7.f,3.f);
                drawHardwareScrew(g,cabR.getRight()-7.f,cabR.getBottom()-7.f,3.f);
            }
            // Precision pedal chassis. No baked-in controls: empty and occupied states
            // share the same frame, while the real VST3 editor snapshot is optional.
            for(int i=0;i<9;++i){
                if(i==VstChain::sculptSlot)continue;
                const auto r=slotRects[(size_t)i].toFloat();
                if(r.isEmpty())continue;
                const int ix=i<4?i:i-1;
                const auto colour=kPedalColours[(size_t)ix];
                const bool loaded=owner.vstChain.get(i)!=nullptr;
                const bool enabled=loaded && owner.vstChain.enabled(i);
                const auto shell=r.reduced(.8f);
                g.setColour(juce::Colours::black.withAlpha(.86f));
                g.fillRoundedRectangle(shell.expanded(1.5f),6.f);
                g.setGradientFill(juce::ColourGradient(colour.darker(.22f),shell.getX(),shell.getY(),
                                               colour.darker(.73f),shell.getRight(),shell.getBottom(),false));
                g.fillRoundedRectangle(shell,5.5f);
                g.setColour(colour.brighter(.35f).withAlpha(.88f));
                g.drawRoundedRectangle(shell.reduced(.5f),5.f,1.1f);
                g.setColour(juce::Colours::white.withAlpha(.14f));
                g.drawLine(shell.getX()+12.f,shell.getY()+2.f,shell.getRight()-12.f,shell.getY()+2.f,.75f);
                g.setColour(juce::Colours::black.withAlpha(.40f));
                g.drawRoundedRectangle(shell.reduced(4.f),4.f,.65f);

                // A subdued glass display, not a generated image of a nonexistent plugin.
                // A genuine editor snapshot, if captured, is letterboxed with original ratio.
                const auto rack=juce::Rectangle<float>(r.getX()+67.f,r.getY()+49.f,
                                                       r.getWidth()-81.f,r.getHeight()-61.f);
                g.setColour(juce::Colours::black.withAlpha(.70f));
                g.fillRoundedRectangle(rack.expanded(2.f),3.5f);
                g.setGradientFill(juce::ColourGradient(juce::Colour(0xff111517),rack.getX(),rack.getY(),
                                                juce::Colour(0xff080a0c),rack.getRight(),rack.getBottom(),false));
                g.fillRoundedRectangle(rack,3.f);
                if(loaded && pluginSnapshots[(size_t)i].isValid()){
                    const auto& snapshot=pluginSnapshots[(size_t)i];
                    const float scale=juce::jmin((rack.getWidth()-10.f)/snapshot.getWidth(),
                                                  (rack.getHeight()-10.f)/snapshot.getHeight());
                    const int dw=juce::jmax(1,(int)std::round(snapshot.getWidth()*scale));
                    const int dh=juce::jmax(1,(int)std::round(snapshot.getHeight()*scale));
                    g.drawImage(snapshot,(int)(rack.getCentreX()-dw*.5f),
                                (int)(rack.getCentreY()-dh*.5f),dw,dh,
                                0,0,snapshot.getWidth(),snapshot.getHeight());
                }else if(loaded){
                    const auto margin=juce::jmin(14.f,rack.getHeight()*.18f);
                    g.setColour(colour.brighter(.55f).withAlpha(.64f));
                    g.fillRoundedRectangle(juce::Rectangle<float>(rack.getX()+8.f,rack.getY()+margin,
                                      3.f,rack.getHeight()-2.f*margin),1.5f);
                    g.setColour(kText);
                    g.setFont(juce::Font(juce::FontOptions(13.f,juce::Font::bold)));
                    g.drawFittedText(owner.vstChain.name(i),
                         juce::Rectangle<int>((int)rack.getX()+18,(int)rack.getY()+7,
                                              (int)rack.getWidth()-27,(int)rack.getHeight()-24),
                         juce::Justification::centred,2,.75f);
                    g.setColour(kMuted.withAlpha(.72f));
                    g.setFont(juce::Font(juce::FontOptions(9.f)));
                    g.drawFittedText("VST3  /  CLICCA PER APRIRE",
                         juce::Rectangle<int>((int)rack.getX()+12,(int)rack.getBottom()-20,
                                              (int)rack.getWidth()-24,14),
                         juce::Justification::centred,1,.7f);
                }else{
                    drawGrille(g,rack.reduced(9.f,12.f),colour);
                    g.setColour(juce::Colour(0xffadb5b5).withAlpha(.68f));
                    g.setFont(juce::Font(juce::FontOptions(10.2f,juce::Font::bold)));
                    g.drawFittedText("SLOT LIBERO",rack.toNearestInt().reduced(9,5),
                                     juce::Justification::centred,1,.8f);
                }
                g.setColour(juce::Colour(0xff93918b).withAlpha(.75f));
                g.drawRoundedRectangle(rack.reduced(.5f),3.f,1.f);
                // Dedicated footswitch LED and mechanical actuator. Only this toggles bypass.
                const float footX=r.getX()+35.f,footY=r.getY()+99.f;
                g.setGradientFill(juce::ColourGradient(juce::Colour(0xffbdb9ac),footX-13.f,footY-13.f,
                                                       juce::Colour(0xff202427),footX+13.f,footY+13.f,false));
                g.fillEllipse(footX-15.f,footY-15.f,30.f,30.f);
                g.setColour(juce::Colour(0xff151617));g.drawEllipse(footX-15.f,footY-15.f,30.f,30.f,1.6f);
                g.setColour(juce::Colour(0xff202628));g.fillEllipse(footX-8.f,footY-8.f,16.f,16.f);
                const float ledX=r.getX()+35.f,ledY=r.getY()+53.f;
                g.setColour((enabled?kAccent:juce::Colour(0xff55342f)).withAlpha(.21f));
                g.fillEllipse(ledX-11.f,ledY-11.f,22.f,22.f);
                g.setColour(enabled?juce::Colour(0xffff6644):juce::Colour(0xff61312d));
                g.fillEllipse(ledX-5.f,ledY-5.f,10.f,10.f);
                g.setColour(juce::Colour(0xfff3c6ae).withAlpha(enabled?.84f:.23f));
                g.drawEllipse(ledX-5.f,ledY-5.f,10.f,10.f,1.f);
                for(float xx:{r.getX()+7.f,r.getRight()-7.f})
                    for(float yy:{r.getY()+7.f,r.getBottom()-7.f})drawHardwareScrew(g,xx,yy,3.25f);
            }
            // Visual arrows mark processing direction; ordering is the actual engine order.
            for(int i=0;i<5;++i){
                const float x=(sections[(size_t)i].getRight()+sections[(size_t)i+1].getX())*.5f;
                const float y=(float)sections[0].getY()+sections[0].getHeight()*.53f;
                juce::Path path;path.startNewSubPath(x-2.f,y-8.f);path.lineTo(x+2.f,y);path.lineTo(x-2.f,y+8.f);
                g.setColour(kWarm.withAlpha(.75f));g.strokePath(path,juce::PathStrokeType(1.6f));
            }
        }

        void resized() override {
            const int gap=9,pad=12;
            const int available=getWidth()-2*pad-5*gap;
            const std::array<float,6> fractions{{.144f,.198f,.159f,.168f,.198f,.133f}};
            int x=pad; const int y=78,h=getHeight()-90;
            for(int i=0;i<6;++i){
                const int w=i==5?getWidth()-pad-x:(int)std::round(available*fractions[(size_t)i]);
                sections[(size_t)i]={x,y,w,h};x+=w+gap;
            }
            auto input=sections[0].reduced(13,0),pre=sections[1].reduced(9,0),
                 sculpt=sections[2].reduced(11,0),nam=sections[3].reduced(13,0),
                 post=sections[4].reduced(9,0),output=sections[5].reduced(13,0);
            brand.setBounds(100,10,315,31);
            subtitle.setBounds(101,43,315,21);
            presetHeading.setBounds(412,13,76,23);
            presetPicker.setBounds(491,11,176,32);
            presetNameEditor.setBounds(675,11,166,32);
            presetSave.setBounds(849,11,73,32);
            presetRefresh.setBounds(930,11,35,32);
            presetDelete.setBounds(491,47,90,24);
            toneLabel.setBounds(getWidth()-522,17,81,20);
            toneQuery.setBounds(getWidth()-441,11,272,32);
            toneSearch.setBounds(getWidth()-163,11,133,32);
            status.setBounds(594,47,getWidth()-624,22);
            inputSetup.setBounds(input.getX(),y+66,input.getWidth(),33);
            inputDeviceInfo.setBounds(input.getX(),y+122,input.getWidth(),31);
            inputPortInfo.setBounds(input.getX(),y+162,input.getWidth(),30);
            inMeter.setBounds(input.getX(),y+349,input.getWidth(),28);
            tunerHeading.setBounds(input.getX(),y+394,input.getWidth(),22);
            tunerNote.setBounds(input.getX(),y+419,input.getWidth(),67);
            tunerInfo.setBounds(input.getX(),y+486,input.getWidth(),22);
            tunerButton.setBounds(input.getX(),y+514,input.getWidth(),36);
            gateHeading.setBounds(input.getX(),y+573,input.getWidth(),22);
            gateBypass.setBounds(input.getX(),y+601,input.getWidth(),32);
            gateThreshold.setBounds(input.getX(),y+643,input.getWidth(),90);
            outputSetup.setBounds(output.getX(),y+66,output.getWidth(),33);
            outputDeviceInfo.setBounds(output.getX(),y+122,output.getWidth(),31);
            outputPortInfo.setBounds(output.getX(),y+162,output.getWidth(),30);
            outMeter.setBounds(output.getX(),y+349,output.getWidth(),28);
            masterHeading.setBounds(output.getX(),y+395,output.getWidth(),23);
            masterSlider.setBounds(output.getCentreX()-66,y+433,132,134);
            masterReadout.setBounds(output.getX(),y+569,output.getWidth(),26);
            masterReadout.setJustificationType(juce::Justification::centred);
            tapHeading.setBounds(output.getX(),y+611,output.getWidth(),23);
            tapReadout.setBounds(output.getX(),y+635,output.getWidth(),37);
            tapButton.setBounds(output.getX(),y+680,output.getWidth(),38);
            namName.setBounds(nam.getX(),y+68,nam.getWidth(),27);
            namPicker.setBounds(nam.getX(),y+112,nam.getWidth(),34);
            prevNam.setBounds(nam.getX(),y+160,36,31);
            nextNam.setBounds(nam.getX()+42,y+160,36,31);
            loadNam.setBounds(nam.getX()+84,y+160,nam.getWidth()-84,31);
            const int namGap=10;
            const int namCol=(nam.getWidth()-namGap)/2;
            namInputHeading.setBounds(nam.getX(),y+210,namCol,20);
            namOutputHeading.setBounds(nam.getX()+namCol+namGap,y+210,namCol,20);
            namInputHeading.setJustificationType(juce::Justification::centred);
            namOutputHeading.setJustificationType(juce::Justification::centred);
            namGains[0].setBounds(nam.getX(),y+234,namCol,117);
            namGains[1].setBounds(nam.getX()+namCol+namGap,y+234,namCol,117);
            loadIr.setBounds(nam.getX(),y+365,nam.getWidth(),36);
            irName.setBounds(nam.getX(),y+413,nam.getWidth(),28);
            const int top=y+67, usable=h-85, rowH=(usable-24)/4;
            for(int k=0;k<4;++k){
                layoutSlot(k,{pre.getX(),top+k*(rowH+8),pre.getWidth(),rowH});
                layoutSlot(5+k,{post.getX(),top+k*(rowH+8),post.getWidth(),rowH});
            }
            sculptName.setBounds(sculpt.getX(),y+73,sculpt.getWidth(),31);
            sculptFaceButton.setBounds(sculpt.getX()+14,y+169,sculpt.getWidth()-28,57);
            layoutSlot(4,{sculpt.getX(),y+90,sculpt.getWidth(),128});
            sculptHeading.setBounds(sculpt.getX(),y+240,sculpt.getWidth(),23);
            const int knobW=(sculpt.getWidth()-14)/2;
            for(int k=0;k<5;++k){
                const int col=k%2,row=k/2;
                const int kx=k==4?sculpt.getCentreX()-knobW/2:
                             sculpt.getX()+col*(knobW+12);
                const int ky=y+271+row*128;
                knobLabels[(size_t)k].setBounds(kx,ky,knobW,20);
                knobs[(size_t)k].setBounds(kx+3,ky+24,knobW-6,77);
                knobValues[(size_t)k].setBounds(kx,ky+102,knobW,20);
            }
            variation.setBounds(sculpt.getX(),y+662,sculpt.getWidth(),37);
            vstFinder.setBounds(getLocalBounds());
            if(vstFinder.isVisible())vstFinder.toFront(true);
        }
    private:
        // The delete menu is separate from the load menu. Choosing an item here NEVER recalls it.
        void choosePresetToDelete(){
            const auto names=owner.namedPresetNames();
            if(names.isEmpty())return;
            juce::PopupMenu menu;
            for(int i=0;i<names.size();++i)menu.addItem(i+1,names[i]);
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&presetDelete),
                [safe=juce::Component::SafePointer<RigPanel>(this),names](int selectedId){
                    if(safe==nullptr || selectedId<1 || selectedId>names.size())return;
                    safe->confirmDeleteNamedPreset(names[selectedId-1]);
                });
        }
        void confirmDeleteNamedPreset(const juce::String& name){
            auto options=juce::MessageBoxOptions::makeOptionsOkCancel(
                juce::MessageBoxIconType::WarningIcon,"Eliminare il preset?",
                "Eliminare definitivamente il preset '"+name+"'?\n\nIl rig attualmente in uso non cambia.\nGli slot rapidi restano memorizzati.",
                "ELIMINA","ANNULLA",this);
            juce::AlertWindow::showAsync(options,
                [safe=juce::Component::SafePointer<RigPanel>(this),name](int result){
                    if(result!=1 || safe==nullptr)return;
                    if(safe->owner.deleteNamedPreset(name)){
                        safe->refreshPresets(true);
                        if(safe->owner.currentPresetName.isEmpty()){
                            safe->presetPicker.setSelectedId(1,juce::dontSendNotification);
                            safe->presetNameEditor.clear();
                        }
                        safe->lastLoadedName=safe->owner.currentPresetName;
                    }
                });
        }
        void savePreset(){
            const auto name=presetNameEditor.getText().trim();
            if(owner.saveNamedPreset(name)) refreshPresets(true);
        }
        void refreshPresets(bool force){
            const auto names=owner.namedPresetNames();
            if(!force && names==presetCatalog)return;
            presetCatalog=names;
            presetPicker.clear(juce::dontSendNotification);
            presetPicker.addItem("NUOVO PRESET...",1);
            for(int i=0;i<names.size();++i)presetPicker.addItem(names[i],i+2);
            const int id=names.indexOf(owner.currentPresetName);
            presetPicker.setSelectedId(id>=0?id+2:1,juce::dontSendNotification);
            presetDelete.setEnabled(!names.isEmpty());
            if(owner.currentPresetName.isNotEmpty())presetNameEditor.setText(owner.currentPresetName,juce::dontSendNotification);
        }
        void labelizeLocation(int){}
        void layoutSlot(int index,juce::Rectangle<int> r){
            slotRects[(size_t)index]=r;
            const int x=r.getX()+10,y=r.getY()+7,w=r.getWidth()-20;
            if(index==VstChain::sculptSlot){
                const bool missing=owner.vstChain.get(index)==nullptr;
                slotNames[(size_t)index].setBounds(0,0,0,0);
                slotMenus[(size_t)index].setBounds(x,y+27,w,missing?28:0);
                slotMenus[(size_t)index].setVisible(missing);
                slotStatus[(size_t)index].setBounds(0,0,0,0);
                clearButtons[(size_t)index].setVisible(false);
                footswitches[(size_t)index].setBounds(0,0,0,0);
                previewButtons[(size_t)index].setBounds(0,0,0,0);
                return;
            }
            slotNames[(size_t)index].setBounds(x,y,65,27);
            slotMenus[(size_t)index].setBounds(x+65,y,juce::jmax(35,w-104),30);
            slotStatus[(size_t)index].setBounds(0,0,0,0);
            footswitches[(size_t)index].setBounds(r.getX()+20,r.getY()+84,30,30);
            // Full-pedal hit area; it is behind picker/footswitch/X in z-order.
            previewButtons[(size_t)index].setBounds(r.reduced(4));
            previewButtons[(size_t)index].toBack();
            clearButtons[(size_t)index].setBounds(r.getRight()-34,r.getY()+9,24,24);
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
        void recordTap() {
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (lastTapMs > 0.0) {
                const double dt = now - lastTapMs;
                // Reject accidental double-taps. Reset *tap history*, not host BPM,
                // after inactivity: plugins never lose a valid tempo.
                if (dt >= 240.0 && dt <= 1500.0) {
                    tapIntervals[static_cast<std::size_t>(tapCursor % 4)] = dt;
                    ++tapCursor;
                    tapCount = std::min(tapCount + 1, 4);
                    double sum = 0.0;
                    for (int k = 0; k < tapCount; ++k)
                        sum += tapIntervals[static_cast<std::size_t>(k)];
                    const double avgMs = sum / tapCount;
                    const int bpm = juce::jlimit(40, 250, static_cast<int>(std::lround(60000.0 / avgMs)));
                    owner.tempoBpm.store(bpm, std::memory_order_relaxed);
                } else if (dt > 1500.0) {
                    tapCount = 0;
                    tapCursor = 0;
                }
            }
            lastTapMs = now;
            updateLabels();
        }
        void timerCallback() override {updateLabels();}
        void updateLabels(){
            inMeter.setText(juce::String(owner.levelDb.load(),1)+" dBFS",juce::dontSendNotification);
            outMeter.setText(juce::String(owner.outputDb.load(),1)+" dBFS",juce::dontSendNotification);
            if(auto* dev=owner.deviceManager.getCurrentAudioDevice()){
                inputDeviceInfo.setText(dev->getName(),juce::dontSendNotification);
                outputDeviceInfo.setText(dev->getName(),juce::dontSendNotification);
                const auto cfg=owner.deviceManager.getAudioDeviceSetup();
                inputPortInfo.setText("Input: " + juce::String(cfg.inputChannels.countNumberOfSetBits()) + " ch attivi",juce::dontSendNotification);
                outputPortInfo.setText("Output: " + juce::String(cfg.outputChannels.countNumberOfSetBits()) + " ch attivi",juce::dontSendNotification);
            }else{
                inputDeviceInfo.setText("Nessun dispositivo",juce::dontSendNotification);
                outputDeviceInfo.setText("Nessun dispositivo",juce::dontSendNotification);
            }
            gateBypass.setButtonText(owner.gateEnabled.load()?"GATE ON":"GATE OFF");
            if(!gateThreshold.isMouseButtonDown()) gateThreshold.setValue(owner.gateThresholdDb.load(),juce::dontSendNotification);
            const bool mute = owner.tunerMute.load(std::memory_order_relaxed);
            tunerButton.setButtonText(mute ? "TUNER ON / MUTE" : "TUNER / AUDIO ON");
            tunerButton.setToggleState(mute, juce::dontSendNotification);
            const auto pitch = owner.tuner.detect();
            tunerPitchValid = pitch.midi >= 0 && pitch.confidence > 0.65f;
            tunerCurrentCents = pitch.cents;
            if (tunerPitchValid) {
                static constexpr const char* notes[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                const int noteIndex = ((pitch.midi % 12) + 12) % 12;
                tunerNote.setText(juce::String(notes[noteIndex]) + juce::String(pitch.midi / 12 - 1),juce::dontSendNotification);
                tunerInfo.setText(juce::String(pitch.hz,1) + " Hz  " + (pitch.cents>=0.f?"+":"") +
                    juce::String(pitch.cents,1) + " cents",juce::dontSendNotification);
            } else {
                tunerNote.setText("--",juce::dontSendNotification);
                tunerInfo.setText("Attendo una nota...",juce::dontSendNotification);
            }
            const int bpm = owner.tempoBpm.load(std::memory_order_relaxed);
            tapReadout.setText(bpm > 0 ? juce::String(bpm) + " BPM" : juce::String("-- BPM"),juce::dontSendNotification);
            const auto m=owner.masterGainDb.load();
            masterSlider.setValue(m,juce::dontSendNotification);
            masterReadout.setText(m<=-60.f?"MUTE":juce::String(m,1)+" dB",juce::dontSendNotification);
            status.setText(owner.status.getText(),juce::dontSendNotification);
            if(lastLoadedName!=owner.currentPresetName){
                lastLoadedName=owner.currentPresetName;
                refreshPresets(true);
            }
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
            if(!namGains[0].isMouseButtonDown())namGains[0].setValue(owner.namInputGainDb.load(),juce::dontSendNotification);
            if(!namGains[1].isMouseButtonDown())namGains[1].setValue(owner.namOutputGainDb.load(),juce::dontSendNotification);
            prevNam.setEnabled(selected>0); nextNam.setEnabled(selected<(int)owner.captures.size()-1);
            sculptName.setText(owner.vstChain.name(4).isNotEmpty() ? "ON / " + owner.vstChain.name(4)
                               : "SCULPT CH: CARICA VST3",juce::dontSendNotification);
            for(int i=0;i<9;++i){
                const auto name=owner.vstChain.name(i);
                const auto newId=owner.vstChain.bundlePath(i).getFullPathName()+"|"+name;
                if(previewIds[(size_t)i]!=newId){
                    pluginSnapshots[(size_t)i]=juce::Image();
                    previewIds[(size_t)i]=newId;
                }
                slotStatus[(size_t)i].setText(name.isNotEmpty()?name:"",juce::dontSendNotification);
                // The selector always shows the real loaded plugin (or the empty state).
                const auto selectorText=name.isNotEmpty()?name:
                    (i==VstChain::sculptSlot?"CARICA SCULPT CH...":"Scegli VST3...");
                if(slotMenus[(size_t)i].getButtonText()!=selectorText)
                    slotMenus[(size_t)i].setButtonText(selectorText);
                clearButtons[(size_t)i].setEnabled(i != VstChain::sculptSlot && name.isNotEmpty());
                if (i == VstChain::sculptSlot) {
                    clearButtons[(size_t)i].setVisible(false);
                    const bool missing=name.isEmpty();
                    slotMenus[(size_t)i].setVisible(missing);

                }
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
        VstPickerOverlay vstFinder;
        juce::Label presetHeading,namInputHeading,namOutputHeading;
        juce::ComboBox presetPicker;
        juce::TextEditor presetNameEditor;
        juce::TextButton presetSave,presetRefresh,presetDelete;
        juce::StringArray presetCatalog;
        juce::String lastLoadedName;
        std::array<juce::Slider,2> namGains;
        std::array<juce::Rectangle<int>,6> sections;
        std::array<juce::Rectangle<int>,9> slotRects;
        std::array<juce::Image,9> pluginSnapshots;
        std::array<juce::String,9> previewIds;
        juce::StringArray cachedNamNames;
        bool updatingKnobs=false;
        bool tunerPitchValid=false;
        float tunerCurrentCents=0.f;
        std::array<juce::String,5> paramNames;
        juce::Label brand,subtitle,status,inMeter,outMeter,inputDeviceInfo,outputDeviceInfo,inputPortInfo,outputPortInfo,namName,irName,sculptName,gateHeading,masterHeading,masterReadout,sculptHeading;
        juce::Label tunerHeading,tunerNote,tunerInfo,tapHeading,tapReadout;
        juce::TextButton tunerButton,tapButton;
        double lastTapMs = 0.0;
        std::array<double,4> tapIntervals{};
        int tapCount = 0, tapCursor = 0;
        juce::Label preHeading,postHeading,namHeading,inputHeading,outputHeading,sculptSectionHeading,toneLabel;
        juce::TextEditor toneQuery;
        juce::TextButton toneSearch,inputSetup,outputSetup,gateBypass,loadNam,loadIr,prevNam,nextNam,variation;
        juce::Slider gateThreshold,masterSlider;
        juce::ComboBox namPicker;
        std::array<juce::Label,9> slotNames,slotStatus;
        std::array<juce::TextButton,9> slotMenus;
        std::array<juce::TextButton,9> clearButtons;
        InvisibleButton sculptFaceButton;
        std::array<InvisibleButton,9> footswitches,previewButtons;
        std::array<juce::Slider,5> knobs;
        std::array<juce::Label,5> knobLabels,knobValues;
    };

    // Same-window audio configuration. Only VST3 editors create additional windows.
    class AudioSetupOverlay final : public juce::Component {
    public:
        AudioSetupOverlay(juce::AudioDeviceManager& manager,std::function<void()> closeCallback)
            : selector(manager,1,2,1,2,false,false,true,false), onClose(std::move(closeCallback)) {
            setOpaque(true);
            closeButton.setButtonText("CHIUDI  X");
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
        if(rigPanel!=nullptr) rigPanel->showVstSearch(slot);
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
        if (slot == VstChain::sculptSlot) return; // fixed module cannot be removed
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
        activePluginWindow=std::make_unique<HostedEditorWindow>(*plugin,
            [safe=juce::Component::SafePointer<GuitarRigWindow>(this),slot](const juce::Image& image){
                if(safe!=nullptr && safe->rigPanel!=nullptr)
                    safe->rigPanel->setPluginSnapshot(slot,image);
            });
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

    // Presets are self-contained XML snapshots of host parameters and VST3 state blobs.
    // Referenced NAM, IR and plugin bundles must remain installed at saved paths.
    juce::File presetDirectory() const {
        auto root=juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                        .getChildFile("Sculpt Guitar Rig").getChildFile("Presets");
        return root;
    }
    juce::File quickDirectory() const { return presetDirectory().getChildFile("QuickSlots"); }
    juce::File namedPresetFile(const juce::String& name) const {
        return presetDirectory().getChildFile(juce::File::createLegalFileName(name)+".sgrig");
    }
    juce::File quickPresetFile(int index) const {
        return quickDirectory().getChildFile("Slot"+juce::String(index+1)+".sgrig");
    }
    juce::StringArray namedPresetNames() const {
        juce::StringArray names;
        const auto files=presetDirectory().findChildFiles(juce::File::findFiles,false,"*.sgrig");
        for(const auto& f:files)names.add(f.getFileNameWithoutExtension());
        names.sort(true);
        return names;
    }
    // Serialising plugin state calls vendor code; stop audio callbacks during that action.
    std::unique_ptr<juce::XmlElement> snapshotRig(const juce::String& displayName) {
        auto root=std::make_unique<juce::XmlElement>("SCULPT_RIG_PRESET");
        root->setAttribute("version",1);
        root->setAttribute("name",displayName);
        root->setAttribute("gate",gateEnabled.load());
        root->setAttribute("gateDb",(double)gateThresholdDb.load());
        root->setAttribute("masterDb",(double)masterGainDb.load());
        root->setAttribute("bpm",tempoBpm.load());
        root->setAttribute("namInputDb",(double)namInputGainDb.load());
        root->setAttribute("namOutputDb",(double)namOutputGainDb.load());
        root->setAttribute("irPath",irLoaded.load()?irFile.getFullPathName():juce::String());
        auto* nam=root->createNewChildElement("NAM_LIBRARY");
        {
            const juce::ScopedLock lock(deviceManager.getAudioCallbackLock());
            const int selected=requestedIndex.load();
            nam->setAttribute("selectedPath",selected>=0 && selected<(int)captures.size()
                                      ? captures[(size_t)selected].file.getFullPathName() : juce::String());
            for(const auto& c:captures){
                auto* item=nam->createNewChildElement("CAPTURE");
                item->setAttribute("file",c.file.getFullPathName());
            }
            auto* plugins=root->createNewChildElement("PLUGINS");
            for(int i=0;i<VstChain::totalSlots;++i){
                auto* entry=plugins->createNewChildElement("SLOT");
                entry->setAttribute("index",i);
                entry->setAttribute("enabled",i==VstChain::sculptSlot || vstChain.enabled(i));
                entry->setAttribute("file",vstChain.bundlePath(i).getFullPathName());
                if(auto* processor=vstChain.get(i)){
                    juce::MemoryBlock state;
                    processor->getStateInformation(state);
                    entry->setAttribute("state",state.toBase64Encoding());
                }
            }
        }
        return root;
    }
    bool savePresetFile(const juce::File& file,const juce::String& name){
        auto document=snapshotRig(name);
        if(file.getParentDirectory().createDirectory().failed()){
            status.setText("PRESET: impossibile creare la cartella di salvataggio.",juce::dontSendNotification);
            return false;
        }
        juce::TemporaryFile temp(file);
        if(!temp.getFile().replaceWithText(document->toString()) ||
           !temp.overwriteTargetFileWithTemporary()){
            status.setText("PRESET: errore di scrittura su disco.",juce::dontSendNotification);
            return false;
        }
        return true;
    }
    bool saveNamedPreset(juce::String name){
        name=name.trim();
        if(name.isEmpty() || name.length()>64){
            status.setText("Inserisci un nome preset da 1 a 64 caratteri.",juce::dontSendNotification);
            return false;
        }
        name=juce::File::createLegalFileName(name);
        if(name.isEmpty())return false;
        if(!savePresetFile(namedPresetFile(name),name))return false;
        currentPresetName=name;
        status.setText("PRESET SALVATO: "+name,juce::dontSendNotification);
        refreshQuickSlots();
        return true;
    }
    bool storeQuickSlot(int index){
        if(index<0 || index>=5)return false;
        const auto name=currentPresetName.isNotEmpty() ? currentPresetName : juce::String("Rig ")+juce::String(index+1);
        if(!savePresetFile(quickPresetFile(index),name))return false;
        status.setText("MEMORIZZATO SLOT "+juce::String(index+1)+": "+name,juce::dontSendNotification);
        refreshQuickSlots();
        return true;
    }
    void refreshQuickSlots(){
        for(int i=0;i<5;++i){
            const auto file=quickPresetFile(i);
            juce::String name="VUOTO";
            if(file.existsAsFile()){
                auto xml=juce::XmlDocument::parse(file);
                if(xml && xml->hasTagName("SCULPT_RIG_PRESET"))
                    name=xml->getStringAttribute("name", "Preset "+juce::String(i+1));
            }
            quickNames[(size_t)i].setText(juce::String(i+1)+"  /  "+name,juce::dontSendNotification);
            quickRecall[(size_t)i].setEnabled(file.existsAsFile());
            quickClear[(size_t)i].setVisible(file.existsAsFile());
        }
    }
    // Delete the saved file only: do not unload plugins, change DSP or overwrite quick slots.
    bool deleteNamedPreset(const juce::String& name){
        if(name.isEmpty())return false;
        const auto file=namedPresetFile(name);
        if(!file.existsAsFile()){
            status.setText("PRESET NON TROVATO: "+name,juce::dontSendNotification);
            return false;
        }
        if(!file.deleteFile()){
            status.setText("IMPOSSIBILE ELIMINARE IL PRESET: "+name,juce::dontSendNotification);
            return false;
        }
        if(currentPresetName==name)currentPresetName.clear();
        status.setText("PRESET ELIMINATO: "+name+" (rig attuale invariato)",juce::dontSendNotification);
        return true;
    }
    bool deleteQuickSlot(int index){
        if(index<0 || index>=5)return false;
        const auto file=quickPresetFile(index);
        if(!file.existsAsFile())return false;
        if(!file.deleteFile()){
            status.setText("IMPOSSIBILE SVUOTARE LO SLOT "+juce::String(index+1),juce::dontSendNotification);
            return false;
        }
        status.setText("SLOT RAPIDO "+juce::String(index+1)+" SVUOTATO (rig attuale invariato)",juce::dontSendNotification);
        refreshQuickSlots();
        return true;
    }
    void confirmDeleteQuickSlot(int index){
        if(index<0 || index>=5 || !quickPresetFile(index).existsAsFile())return;
        const auto slotName=quickNames[(size_t)index].getText();
        auto options=juce::MessageBoxOptions::makeOptionsOkCancel(
            juce::MessageBoxIconType::WarningIcon,"Svuotare lo slot rapido?",
            "Eliminare "+slotName+"?\n\nIl preset normale eventualmente associato non sarà eliminato.\nIl rig attualmente caricato resterà invariato.",
            "SVUOTA","ANNULLA",this);
        juce::AlertWindow::showAsync(options,
            [safe=juce::Component::SafePointer<GuitarRigWindow>(this),index](int result){
                if(result==1 && safe!=nullptr)safe->deleteQuickSlot(index);
            });
    }
    bool loadNamedPreset(const juce::String& name){
        const bool ok=loadPresetFile(namedPresetFile(name));
        if(ok)currentPresetName=name;
        return ok;
    }
    bool loadQuickSlot(int index){
        if(index<0 || index>=5)return false;
        const bool ok=loadPresetFile(quickPresetFile(index));
        if(ok) status.setText("RICHIAMATO SLOT "+juce::String(index+1),juce::dontSendNotification);
        return ok;
    }
    bool loadPresetFile(const juce::File& file){
        if(!file.existsAsFile()){
            status.setText("PRESET: file non trovato.",juce::dontSendNotification);return false;
        }
        auto root=juce::XmlDocument::parse(file);
        if(!root || !root->hasTagName("SCULPT_RIG_PRESET") || root->getIntAttribute("version")!=1){
            status.setText("PRESET: formato sconosciuto o file danneggiato.",juce::dontSendNotification);return false;
        }
        auto* nam=root->getChildByName("NAM_LIBRARY");
        auto* plugins=root->getChildByName("PLUGINS");
        if(!nam || !plugins){
            status.setText("PRESET: NAM o VST3 non descritti nel file.",juce::dontSendNotification);return false;
        }
        // Validate *all* referenced files before touching any running slot.
        juce::Array<juce::File> namFiles;
        for(auto* node=nam->getFirstChildElement();node!=nullptr;node=node->getNextElement()){
            if(!node->hasTagName("CAPTURE"))continue;
            juce::File capture(node->getStringAttribute("file"));
            if(!capture.existsAsFile()){
                status.setText("PRESET: NAM mancante: "+capture.getFullPathName(),juce::dontSendNotification);
                return false;
            }
            namFiles.add(capture);
        }
        struct SlotState {juce::String path,encoded;bool enabled=false;};
        std::array<SlotState,VstChain::totalSlots> entries;
        std::array<bool,VstChain::totalSlots> seen{};
        for(auto* node=plugins->getFirstChildElement();node!=nullptr;node=node->getNextElement()){
            if(!node->hasTagName("SLOT"))continue;
            const int i=node->getIntAttribute("index",-1);
            if(i<0 || i>=VstChain::totalSlots || seen[(size_t)i]){
                status.setText("PRESET: slot VST3 non valido.",juce::dontSendNotification);return false;
            }
            seen[(size_t)i]=true;
            auto& e=entries[(size_t)i];
            e.path=node->getStringAttribute("file");
            e.encoded=node->getStringAttribute("state");
            e.enabled=node->getBoolAttribute("enabled",false);
            if(e.path.isNotEmpty() && !juce::File(e.path).exists()){
                status.setText("PRESET: VST3 mancante: "+e.path,juce::dontSendNotification);return false;
            }
        }
        for(bool found:seen)if(!found){
            status.setText("PRESET: elenco VST3 incompleto.",juce::dontSendNotification);return false;
        }
        const auto irPath=root->getStringAttribute("irPath");
        if(irPath.isNotEmpty() && !juce::File(irPath).existsAsFile()){
            status.setText("PRESET: cabinet IR mancante: "+irPath,juce::dontSendNotification);return false;
        }
        // Close any active editor before replacing/restoring its processor.
        activePluginWindow.reset();activePluginEditorSlot=-1;
        if(rigPanel!=nullptr)rigPanel->invalidateAllPluginSnapshots();
        juce::StringArray errors;
        const double rate=sampleRateLive.load();
        for(int i=0;i<VstChain::totalSlots;++i){
            const auto& entry=entries[(size_t)i];
            const auto installed=vstChain.bundlePath(i).getFullPathName();
            if(entry.path.isEmpty()){
                if(i!=VstChain::sculptSlot){
                    const juce::ScopedLock lock(deviceManager.getAudioCallbackLock());
                    vstChain.remove(i);
                }
                continue;
            }
            if(entry.path!=installed || vstChain.get(i)==nullptr){
                juce::String error;
                bool loaded=false;
                {
                    const juce::ScopedLock lock(deviceManager.getAudioCallbackLock());
                    loaded=vstChain.load(i,juce::File(entry.path),rate,kBlock,error);
                }
                if(!loaded){errors.add(juce::String(i+1)+": "+error);continue;}
            }
            juce::MemoryBlock state;
            if(entry.encoded.isNotEmpty()){
                if(!state.fromBase64Encoding(entry.encoded)){
                    errors.add("Stato plugin danneggiato: slot "+juce::String(i+1));continue;
                }
                const juce::ScopedLock lock(deviceManager.getAudioCallbackLock());
                if(auto* processor=vstChain.get(i))processor->setStateInformation(state.getData(),(int)state.getSize());
            }
            vstChain.enable(i,i==VstChain::sculptSlot?true:entry.enabled);
        }
        // Compare NAM file lists: no unnecessary model reload if current library is identical.
        bool same=namFiles.size()==(int)captures.size();
        if(same)for(int i=0;i<namFiles.size();++i)
            if(namFiles.getReference(i)!=captures[(size_t)i].file){same=false;break;}
        const auto wanted=nam->getStringAttribute("selectedPath");
        if(!same){
            loadModelsFromFiles(namFiles,wanted,false);
            if(captures.size()!=static_cast<size_t>(namFiles.size()))
                errors.add("Alcune catture NAM non sono state caricate correttamente");
            if(wanted.isNotEmpty()){
                const int index=requestedIndex.load();
                if(index<0 || index>=(int)captures.size() ||
                   captures[(size_t)index].file.getFullPathName()!=wanted)
                    errors.add("NAM selezionato non disponibile: "+wanted);
            }
        }else{
            int index=-1;
            for(int i=0;i<(int)captures.size();++i)
                if(captures[(size_t)i].file.getFullPathName()==wanted){index=i;break;}
            selectNam(index);
        }
        if(irPath.isNotEmpty()){
            const juce::File chosenIr(irPath);
            if(irFile!=chosenIr || !irLoaded.load()){
                cab.loadImpulseResponse(chosenIr,juce::dsp::Convolution::Stereo::no,
                    juce::dsp::Convolution::Trim::yes,0,juce::dsp::Convolution::Normalise::yes);
            }
            irFile=chosenIr;irName=chosenIr.getFileName();irLoaded.store(true);
        }else{
            irLoaded.store(false);irName.clear();irFile=juce::File();
        }
        gateEnabled.store(root->getBoolAttribute("gate",true));
        gateThresholdDb.store(juce::jlimit(-85.f,-15.f,(float)root->getDoubleAttribute("gateDb",-65.)));
        masterGainDb.store(juce::jlimit(-60.f,6.f,(float)root->getDoubleAttribute("masterDb",0.)));
        tempoBpm.store(juce::jlimit(40,250,root->getIntAttribute("bpm",120)));
        namInputGainDb.store(juce::jlimit(-12.f,12.f,(float)root->getDoubleAttribute("namInputDb",0.)));
        namOutputGainDb.store(juce::jlimit(-12.f,12.f,(float)root->getDoubleAttribute("namOutputDb",0.)));
        // Tuner mute deliberately stays under the performer's control, not the preset.
        currentPresetName=root->getStringAttribute("name");
        if(!errors.isEmpty()){
            status.setText("PRESET PARZIALE / "+errors.joinIntoString("; "),juce::dontSendNotification);
            return false;
        }
        status.setText("PRESET CARICATO: "+currentPresetName,juce::dontSendNotification);
        return true;
    }

    void loadModels(const juce::File& folder) {
        juce::Array<juce::File> files;
        if(folder.existsAsFile() && folder.hasFileExtension(".nam")) files.add(folder);
        else if(folder.isDirectory()) files=folder.findChildFiles(juce::File::findFiles,true,"*.nam");
        std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.getFullPathName()<b.getFullPathName();});
        loadModelsFromFiles(files, {}, true);
    }

    void loadModelsFromFiles(const juce::Array<juce::File>& files, const juce::String& selection,
                             bool chooseFirst) {

        auto* deviceBeforeLoad = deviceManager.getCurrentAudioDevice();
        const int sr = static_cast<int>(deviceBeforeLoad ? deviceBeforeLoad->getCurrentSampleRate() : 48000);
        std::vector<Capture> loadedCaptures;
        namSelector.clear(juce::dontSendNotification);
        libraryPage=0;
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
            int selected = captures.empty() || !chooseFirst ? -1 : 0;
            if(selection.isNotEmpty()){
                selected=-1;
                for(int i=0;i<(int)captures.size();++i)
                    if(captures[(size_t)i].file.getFullPathName()==selection){selected=i;break;}
            }
            requestedIndex.store(selected, std::memory_order_relaxed);
            activeIndex = requestedIndex.load(std::memory_order_relaxed);
            fadingFrom = -1;
            fadeRemaining = 0;
        }
        namSelector.addItem("BYPASS NAM / SEGNALE DI",1);
        for (int i=0;i<static_cast<int>(captures.size());++i)
            namSelector.addItem(captures[static_cast<size_t>(i)].name,i+2);
        namSelector.setSelectedId(requestedIndex.load()+2,juce::dontSendNotification);
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
                irFile = f;
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
        signalLabel.setText(db > -65.f ? "*  SEGNALE IN INGRESSO" : "o  IN ATTESA DI SEGNALE",juce::dontSendNotification);
        signalLabel.setColour(juce::Label::textColourId,db > -65.f ? kGreen : kMuted);
        auto* device=deviceManager.getCurrentAudioDevice();
        deviceLabel.setText(device ? device->getName() : "Nessuna scheda selezionata",juce::dontSendNotification);
        sampleLabel.setText(juce::String(juce::roundToInt(sampleRateLive.load()))+" Hz   /   Core Audio",juce::dontSendNotification);
        countLabel.setText(juce::String(captures.size())+" PROFILI NAM IN LIBRERIA",juce::dontSendNotification);
        irLabel.setText(irName.isNotEmpty()?"CABINET IR  /  "+irName:"CABINET IR  /  NON CARICATA",juce::dontSendNotification);
        reasonLabel.setText(hasModel?"NAM: "+captures[static_cast<size_t>(selected)].name
                                     : juce::String("Il segnale DI passa nei VST3 anche senza NAM."),juce::dontSendNotification);
        modeLabel.setText("*  RIG MANUALE",juce::dontSendNotification);
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
    std::atomic<float> levelDb{-90.f}, outputDb{-90.f}, outputDbLeft{-90.f}, outputDbRight{-90.f}, masterGainDb{0.f};
    std::atomic<float> namInputGainDb{0.f}, namOutputGainDb{0.f};
    std::atomic<double> sampleRateLive{48000.};
    int activeIndex = -1, fadingFrom = -1, fadeRemaining = 0;
    float lowpassZ = 0.0f;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> masterGain;
    std::array<float, kBlock> inputScratch{}, dryInput{}, driven{}, outA{}, outB{};
    juce::AudioBuffer<float> fxBuffer{2,kBlock};
    VstChain vstChain;
    GuitarTuner tuner;
    std::atomic<bool> tunerMute{false};
    std::atomic<int> tempoBpm{120};
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> tunerMuteGain;
    juce::Viewport rigViewport;
    juce::Label quickTitle;
    std::array<juce::Label,5> quickNames;
    std::array<juce::TextButton,5> quickRecall,quickStore,quickClear;
    juce::String currentPresetName;
    std::unique_ptr<RigPanel> rigPanel;
    std::unique_ptr<AudioSetupOverlay> audioSetupOverlay;
    std::atomic<bool> gateEnabled{true};
    std::atomic<float> gateThresholdDb{-65.f};
    float gateEnvelope=0.f, gateGain=0.f;
    juce::AudioBuffer<float> cabBuffer{1,kBlock};
    juce::dsp::Convolution cab;
    juce::String irName;
    juce::File irFile;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GuitarRigWindow)
};

class SculptMainWindow final : public juce::DocumentWindow {
public:
    SculptMainWindow() : juce::DocumentWindow("Sculpt Guitar Rig - Analog GUI", kBg, juce::DocumentWindow::allButtons) {}
    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class SculptApp final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Sculpt Guitar Rig"; }
    const juce::String getApplicationVersion() override { return "1.3.6-beta-search"; }
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
