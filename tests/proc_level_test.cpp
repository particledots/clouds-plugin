// Whole-plug-in level test: builds the real plug-in processor class (the same PluginProcessor.cpp as the AU / VST3), feeds it audio through
// processBlock() with its parameters at their defaults, and checks what comes out. This proves the Output Level control is really wired in:
//  - the parameter exists and its default is the unity-making value,
//  - with Dry/Wet = 0 the output equals the input within 0.1 dB in every mode that has a dry path,
//  - moving the Output Level parameter changes the output by exactly that many dB,
//  - loud wet sounds never exceed 0 dBFS.
// Run by the workflow (and by tests/mutation_check.sh's wiring check, which removes the stage from processBlock and expects this to FAIL).
#include "PluginProcessor.h"
#include "OutputStage.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { ++checks; if (! (cond)) { ++failures; std::printf ("  FAIL: "); std::printf (__VA_ARGS__); std::printf ("\n"); } } while (0)

static const char* kModeName[4] = { "Granular", "Pitch shifter/stretch", "Looping delay", "Spectral madness" };
static double toDb (double power) { return 10.0 * std::log10 (power + 1e-30); }

static void setParam (CloudsAudioProcessor& p, const char* id, float plainValue)
{
    auto* prm = p.apvts.getParameter (id);
    jassert (prm != nullptr);
    prm->setValueNotifyingHost (prm->convertTo0to1 (plainValue));
}

struct Result { double dbL = 0, dbR = 0; float peak = 0; };

static Result run (CloudsAudioProcessor& p, double amp, double seconds = 5.0)
{
    const double sr = 48000.0; const int block = 480;
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    const int blocks = (int) (seconds * sr / block);
    double phL = 0, phR = 0, siL = 0, siR = 0, soL = 0, soR = 0; Result r;
    for (int b = 0; b < blocks; ++b)
    {
        std::vector<float> inL (block), inR (block);
        for (int i = 0; i < block; ++i)
        {
            phL += 2.0 * 3.14159265358979 * 220.0 / sr; phR += 2.0 * 3.14159265358979 * 330.0 / sr;
            inL[i] = (float) (amp * std::sin (phL)); inR[i] = (float) (amp * std::sin (phR));
            buf.setSample (0, i, inL[i]); buf.setSample (1, i, inR[i]);
        }
        p.processBlock (buf, midi);
        for (int i = 0; i < block; ++i)
        {
            r.peak = std::max ({ r.peak, std::fabs (buf.getSample (0, i)), std::fabs (buf.getSample (1, i)) });
            if (b > blocks / 2) { siL += (double) inL[i] * inL[i]; siR += (double) inR[i] * inR[i];
                                  soL += (double) buf.getSample (0, i) * buf.getSample (0, i); soR += (double) buf.getSample (1, i) * buf.getSample (1, i); }
        }
    }
    r.dbL = toDb (soL) - toDb (siL); r.dbR = toDb (soR) - toDb (siR);
    return r;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    {   // the parameter and its default
        CloudsAudioProcessor p;
        auto* prm = p.apvts.getParameter ("output_level");
        CHECK (prm != nullptr, "no output_level parameter");
        if (prm != nullptr)
        {
            const float def = *p.apvts.getRawParameterValue ("output_level");
            std::printf ("Output Level parameter: default %.3f dB, shown as \"%s\"\n", def, prm->getText (prm->getValue(), 0).toRawUTF8());
            CHECK (std::fabs (def - OutputStage::kDefaultLevelDb) < 0.001f, "default is %f dB, expected %f dB", def, OutputStage::kDefaultLevelDb);
        }
    }

    {   // a preset / project saved before this version has no Output Level in its state: it must come up at the default, not at 0 dB
        CloudsAudioProcessor src;
        setParam (src, "dry_wet", 0.25f);
        juce::MemoryBlock mb; src.getStateInformation (mb);
        auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
        CHECK (xml != nullptr, "state could not be read back");
        if (xml != nullptr)
        {
            int removed = 0;
            for (int i = xml->getNumChildElements(); --i >= 0;)
                if (auto* c = xml->getChildElement (i)) if (c->getStringAttribute ("id") == "output_level") { xml->removeChildElement (c, true); ++removed; }
            CHECK (removed == 1, "expected one output_level entry in the saved state, found %d", removed);
            juce::MemoryBlock old; juce::AudioProcessor::copyXmlToBinary (*xml, old);

            CloudsAudioProcessor fresh;       // as Logic does: a new instance at its defaults, then the saved state
            fresh.setStateInformation (old.getData(), (int) old.getSize());
            const float lvl = *fresh.apvts.getRawParameterValue ("output_level");
            const float dw  = *fresh.apvts.getRawParameterValue ("dry_wet");
            std::printf ("Old preset (no Output Level in it) loaded into a new instance: Output Level %.2f dB, Dry/Wet %.2f (the preset's own 0.25 is kept)\n", lvl, dw);
            CHECK (std::fabs (lvl - OutputStage::kDefaultLevelDb) < 0.001f, "old preset left Output Level at %f dB", lvl);
            CHECK (std::fabs (dw - 0.25f) < 0.001f, "old preset's Dry/Wet was not restored (%f)", dw);

            CloudsAudioProcessor moved;       // an instance whose knob was already turned: what does an old preset do to it?
            setParam (moved, "output_level", 0.0f);
            moved.setStateInformation (old.getData(), (int) old.getSize());
            std::printf ("   (for information: an instance already at 0 dB that loads an old preset ends at %.2f dB)\n", (double) *moved.apvts.getRawParameterValue ("output_level"));
        }
    }

    std::printf ("Fully dry (Dry/Wet = 0) through the real plug-in processor, defaults otherwise, input -18 dBFS:\n");
    for (int mode = 0; mode < 4; ++mode)
    {
        CloudsAudioProcessor p;
        p.setPlayConfigDetails (2, 2, 48000.0, 480);
        p.prepareToPlay (48000.0, 480);
        setParam (p, "mode", (float) mode);
        setParam (p, "dry_wet", 0.0f);
        const auto r = run (p, 0.125);
        std::printf ("   %-22s %+.3f dB (L) %+.3f dB (R)\n", kModeName[mode], r.dbL, r.dbR);
        CHECK (std::fabs (r.dbL) <= 0.1 && std::fabs (r.dbR) <= 0.1, "%s: dry output %+.3f / %+.3f dB relative to the input", kModeName[mode], r.dbL, r.dbR);
    }

    std::printf ("Moving the Output Level parameter:\n");
    for (float db : { -24.0f, -9.03f, 0.0f, 6.0f })
    {
        CloudsAudioProcessor p;
        p.setPlayConfigDetails (2, 2, 48000.0, 480);
        p.prepareToPlay (48000.0, 480);
        setParam (p, "mode", (float) CloudsEngine::LoopingDelay);
        setParam (p, "dry_wet", 0.0f);
        setParam (p, "output_level", db);
        const auto r = run (p, 0.125);
        const double expected = (double) db - 9.03;   // the firmware's own -9.03 dB plus the chosen level, relative to the input
        std::printf ("   Output Level %+.2f dB -> output %+.3f dB relative to the input (expected about %+.2f)\n", db, r.dbL, expected);
        CHECK (std::fabs (r.dbL - expected) < 0.1, "Output Level %f dB gave %f dB, expected %f dB", db, r.dbL, expected);
    }

    std::printf ("Loud input, every mode, Output Level at +24 dB:\n");
    {
        float worst = 0.0f;
        for (int mode = 0; mode < 4; ++mode)
        {
            CloudsAudioProcessor p;
            p.setPlayConfigDetails (2, 2, 48000.0, 480);
            p.prepareToPlay (48000.0, 480);
            setParam (p, "mode", (float) mode);
            setParam (p, "dry_wet", 1.0f);
            setParam (p, "output_level", 24.0f);
            const auto r = run (p, 0.95, 4.0);
            worst = std::max (worst, r.peak);
            CHECK (r.peak <= 1.0f, "%s: peak %f exceeds 0 dBFS", kModeName[mode], r.peak);
        }
        std::printf ("   largest peak in any mode: %.4f (limit 1.0000)\n", worst);
    }

    {   // the limiter switch (version 0.3.0): parameter, default, old presets, and what it does in the real processor
        CloudsAudioProcessor p;
        auto* lp = p.apvts.getParameter ("limiter");
        CHECK (lp != nullptr, "no limiter parameter");
        if (lp != nullptr) CHECK (*p.apvts.getRawParameterValue ("limiter") > 0.5f, "limiter is not ON by default");

        CloudsAudioProcessor src;
        juce::MemoryBlock mb; src.getStateInformation (mb);
        auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
        CHECK (xml != nullptr, "state could not be read back");
        if (xml != nullptr)
        {
            int removed = 0;
            for (int i = xml->getNumChildElements(); --i >= 0;)
                if (auto* c = xml->getChildElement (i)) if (c->getStringAttribute ("id") == "limiter") { xml->removeChildElement (c, true); ++removed; }
            CHECK (removed == 1, "expected one limiter entry in the saved state, found %d", removed);
            juce::MemoryBlock old; juce::AudioProcessor::copyXmlToBinary (*xml, old);
            CloudsAudioProcessor fresh; fresh.setStateInformation (old.getData(), (int) old.getSize());
            CHECK (*fresh.apvts.getRawParameterValue ("limiter") > 0.5f, "a preset saved before the switch existed came up with the limiter OFF");
        }

        float peakOn = 0, peakOff = 0;
        for (int pass = 0; pass < 2; ++pass)
            for (int mode = 0; mode < 4; ++mode)
            {
                CloudsAudioProcessor q;
                q.setPlayConfigDetails (2, 2, 48000.0, 480);
                q.prepareToPlay (48000.0, 480);
                setParam (q, "mode", (float) mode);
                setParam (q, "dry_wet", 1.0f);
                setParam (q, "output_level", 24.0f);
                setParam (q, "limiter", pass == 0 ? 1.0f : 0.0f);
                const auto r = run (q, 0.95, 4.0);
                float& peak = pass == 0 ? peakOn : peakOff;
                peak = std::max (peak, r.peak);
            }
        std::printf ("Limiter switch, loud wet input at +24 dB: on peak %.4f, off peak %.4f\n", peakOn, peakOff);
        CHECK (peakOn <= 1.0f, "limiter on: peak %f exceeds 0 dBFS", peakOn);
        CHECK (peakOff > 1.0f, "limiter off: peak %f never passed 0 dBFS (the switch did nothing)", peakOff);
    }

    std::printf ("\nproc_level_test: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
