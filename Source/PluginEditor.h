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
    std::unique_ptr<juce::Drawable> volumeOffIcon, volumeUpIcon;
    juce::TooltipWindow tooltips { this, 650 };

    juce::TextButton openButton, rewindButton, playButton, pauseButton, stopButton,
                     forwardButton, loopButton, speedButton, muteButton;
    juce::Slider volumeSlider;
    juce::Label titleLabel, subtitleLabel, volumeValueLabel;
    juce::Rectangle<int> volumePanelBounds, volumeOffIconBounds, volumeUpIconBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformPlayerAudioProcessorEditor)
};
