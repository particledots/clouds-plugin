#pragma once
#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>
#include "CloudsEngine.h"
#include "OutputStage.h"

class CloudsAudioProcessor : public juce::AudioProcessor
{
public:
    CloudsAudioProcessor();
    ~CloudsAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    using AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // The module's TRIG input: called by the editor's Trigger button (MIDI note-ons also fire it).
    void requestTrigger() { triggerRequested.store (true); }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    CloudsEngine engine;
    OutputStage::Stage outputStage;
    std::vector<float> scratchL, scratchR, inL, inR;
    std::atomic<bool> triggerRequested { false };
    int lastMidiNote = 60;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CloudsAudioProcessor)
};
