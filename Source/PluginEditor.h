#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

class WaveformPlayerAudioProcessorEditor : public juce::AudioProcessorEditor,
                                          private juce::Timer
{
public:
    explicit WaveformPlayerAudioProcessorEditor (WaveformPlayerAudioProcessor&);
    ~WaveformPlayerAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class PlayerLookAndFeel;
    class WaveformDisplay;

    void timerCallback() override;
    void updateFromProcessor();
    void configureButton (juce::TextButton&, const juce::String&, const juce::String&);

    WaveformPlayerAudioProcessor& audioProcessor;
    juce::Typeface::Ptr interfaceTypeface;
    std::unique_ptr<PlayerLookAndFeel> playerLookAndFeel;
    std::unique_ptr<WaveformDisplay> waveform;
    juce::TooltipWindow tooltips { this, 650 };

    juce::TextButton openButton, rewindButton, playButton, pauseButton, stopButton,
                     forwardButton, loopButton, speedButton, muteButton, minimumVolumeButton;
    juce::Slider volumeSlider;
    juce::Label titleLabel, subtitleLabel, volumeValueLabel;
    juce::Rectangle<int> volumePanelBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformPlayerAudioProcessorEditor)
};
