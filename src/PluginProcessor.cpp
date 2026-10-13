#include "PluginProcessor.h"
#include "PluginEditor.h"

using APF = juce::AudioParameterFloat;
using APB = juce::AudioParameterBool;
using APC = juce::AudioParameterChoice;

juce::AudioProcessorValueTreeState::ParameterLayout CloudsAudioProcessor::createLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    auto unit = [] (const char* id, const char* name, float def)
    {
        return std::make_unique<APF> (juce::ParameterID { id, 1 }, name, juce::NormalisableRange<float> (0.f, 1.f, 0.0001f), def,
                                      juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return juce::String (v, 2); }));
    };

    p.push_back (unit ("position", "Position", 0.5f));
    p.push_back (unit ("size",     "Size",     0.5f));
    p.push_back (std::make_unique<APF> (juce::ParameterID { "pitch", 1 }, "Pitch",
                 juce::NormalisableRange<float> (-48.f, 48.f, 0.01f), 0.f, juce::AudioParameterFloatAttributes().withLabel ("st")));
    p.push_back (unit ("density",  "Density",  0.7f));
    p.push_back (unit ("texture",  "Texture",  0.5f));
    p.push_back (unit ("dry_wet",  "Dry/Wet",  0.5f));
    p.push_back (unit ("spread",   "Stereo Spread", 0.5f));
    p.push_back (unit ("feedback", "Feedback", 0.0f));
    p.push_back (unit ("reverb",   "Reverb",   0.0f));
    p.push_back (std::make_unique<APB> (juce::ParameterID { "freeze", 1 }, "Freeze", false));
    p.push_back (std::make_unique<APB> (juce::ParameterID { "pitch_midi", 1 }, "Pitch Follows MIDI", false));
    p.push_back (std::make_unique<APC> (juce::ParameterID { "mode", 1 }, "Mode",
                 juce::StringArray { "Granular", "Pitch shifter / time stretch", "Looping delay", "Spectral madness" }, 0));
    p.push_back (std::make_unique<APC> (juce::ParameterID { "quality", 1 }, "Quality",
                 juce::StringArray { "Stereo, 16-bit", "Mono, 16-bit", "Stereo, 8-bit (low-fi)", "Mono, 8-bit (low-fi)" }, 0));
    return { p.begin(), p.end() };
}

CloudsAudioProcessor::CloudsAudioProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
}

bool CloudsAudioProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto out = l.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono())
           && l.getMainInputChannelSet() == out;
}

void CloudsAudioProcessor::prepareToPlay (double sr, int maxBlock)
{
    engine.prepare (sr);
    scratchL.assign ((size_t) maxBlock, 0.f);
    scratchR.assign ((size_t) maxBlock, 0.f);
    inL.assign ((size_t) maxBlock, 0.f);
    inR.assign ((size_t) maxBlock, 0.f);
    triggerRequested.store (false);
    setLatencySamples (engine.latencyInHostSamples());
}

void CloudsAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int chans = buffer.getNumChannels();

    if ((size_t) n > scratchL.size())
    {   // host exceeded its promised max block size
        scratchL.resize ((size_t) n); scratchR.resize ((size_t) n);
        inL.resize ((size_t) n);      inR.resize ((size_t) n);
    }

    auto v = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };

    // MIDI note-on = the module's TRIG input. The last note can also transpose the pitch (note 60 = no change).
    bool trig = triggerRequested.exchange (false);
    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        if (msg.isNoteOn()) { trig = true; lastMidiNote = msg.getNoteNumber(); }
    }
    midi.clear();
    if (trig) engine.trigger();

    CloudsEngine::Params prm;
    prm.position = v ("position"); prm.size = v ("size"); prm.density = v ("density"); prm.texture = v ("texture");
    prm.pitchSemitones = v ("pitch") + (v ("pitch_midi") > 0.5f ? (float) (lastMidiNote - 60) : 0.0f);
    prm.dryWet = v ("dry_wet"); prm.stereoSpread = v ("spread"); prm.feedback = v ("feedback"); prm.reverb = v ("reverb");
    prm.freeze = v ("freeze") > 0.5f;
    prm.mode = (int) v ("mode");
    prm.quality = (int) v ("quality");

    const float* l = buffer.getReadPointer (0);
    const float* r = buffer.getReadPointer (chans > 1 ? 1 : 0);
    std::copy (l, l + n, inL.begin());
    std::copy (r, r + n, inR.begin());

    engine.process (inL.data(), inR.data(), scratchL.data(), scratchR.data(), n, prm);
    buffer.copyFrom (0, 0, scratchL.data(), n);
    if (chans > 1) buffer.copyFrom (1, 0, scratchR.data(), n);
}

juce::AudioProcessorEditor* CloudsAudioProcessor::createEditor()
{
    return new CloudsEditor (*this);
}

void CloudsAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary (*xml, dest);
}

void CloudsAudioProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new CloudsAudioProcessor(); }
