#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

class PuponvstAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    PuponvstAudioProcessorEditor(PuponvstAudioProcessor&);
    ~PuponvstAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    class OscilloscopeComponent final : public juce::Component,
                                        private juce::Timer
    {
    public:
        explicit OscilloscopeComponent(PuponvstAudioProcessor&);
        ~OscilloscopeComponent() override;

        void paint(juce::Graphics&) override;

    private:
        void timerCallback() override;

        PuponvstAudioProcessor& processor;
        juce::Array<float> samples;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OscilloscopeComponent)
    };

    PuponvstAudioProcessor& processor;
    OscilloscopeComponent oscilloscope { processor };
    juce::TextButton bypassButton { "Bypass" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PuponvstAudioProcessorEditor)
};