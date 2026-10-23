#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

// Simple flat look for rotary knobs.
class CloudsLookAndFeel : public juce::LookAndFeel_V4
{
public:
    CloudsLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
};

// Waveform of the recording buffer with a marker at the Position setting (and a band for Size where that is a place on the tape).
class WaveformDisplay : public juce::Component, private juce::Timer
{
public:
    explicit WaveformDisplay (CloudsAudioProcessor&);
    ~WaveformDisplay() override;
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    CloudsAudioProcessor& proc;
    WaveformPicture::Snapshot snap;
    bool haveSnap = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformDisplay)
};

class CloudsEditor : public juce::AudioProcessorEditor
{
public:
    explicit CloudsEditor (CloudsAudioProcessor&);
    ~CloudsEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAtt  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<SliderAtt> att;
    };

    struct Menu
    {
        juce::ComboBox box;
        juce::Label label;
        std::unique_ptr<ComboAtt> att;
    };

    void addKnob (Knob&, const juce::String& paramId, const juce::String& name);
    void addMenu (Menu&, const juce::String& paramId, const juce::String& name, const juce::StringArray& items);

    CloudsAudioProcessor& proc;
    CloudsLookAndFeel laf;
    WaveformDisplay waveform { proc };

    Knob position, size, pitch, density, texture;
    Knob dryWet, spread, feedback, reverb, outputLevel;
    Menu mode, quality, pitchSnap;
    juce::ToggleButton limiter { "LIMITER" };
    juce::ToggleButton freeze { "FREEZE" }, pitchMidi { "PITCH FOLLOWS MIDI" };
    juce::TextButton trigger { "TRIGGER" };
    std::unique_ptr<juce::ParameterAttachment> snapReadoutAtt;   // keeps the Pitch readout in step with the Pitch Snap menu
    std::unique_ptr<ButtonAtt> limiterAtt, freezeAtt, pitchMidiAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CloudsEditor)
};
