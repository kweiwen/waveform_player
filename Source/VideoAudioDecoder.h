#pragma once

#include <JuceHeader.h>
#include <functional>
#include <memory>

struct DecodedVideoAudio
{
    juce::AudioBuffer<float> samples;
    double sampleRate = 0.0;
};

// Runs on the loader thread. The caller keeps the old media when this fails.
std::unique_ptr<DecodedVideoAudio> decodeVideoAudio (const juce::File& file,
                                                      juce::String& error,
                                                      const std::function<bool()>& shouldCancel);
