#pragma once

#include <JuceHeader.h>
#include <cstdint>
#include <memory>

// Shows the video frame for a media time supplied by the caller. The audio
// position is the only clock: the picture never plays on its own, it follows.
// Frames are decoded ahead on a background thread; a jump restarts decoding
// from the nearest preceding keyframe while the previous frame stays visible.
class VideoFrameView final : public juce::Component
{
public:
    VideoFrameView();
    ~VideoFrameView() override;

    // Message thread. Opening and decoding run on the decoder thread.
    void open (const juce::File& file);

    // Message thread, once per display refresh. Shows the latest decoded frame
    // at or before `seconds`. While playing, decoding aims ahead by the time a
    // restart takes at `speed`, so a jump lands on time instead of chasing.
    // A changed `seekRevision` marks an explicit seek: it is never mistaken
    // for clock jitter, but restarts decoding only if the target is out of reach.
    void showTime (double seconds, bool playing, double speed, std::uint64_t seekRevision = 0);

    // Presentation time of the frame on screen, or -1 before the first frame.
    double getDisplayedFrameSeconds() const noexcept;
    // Number of reader starts, useful for diagnosing repeated seeks.
    std::uint64_t getDecoderRestartCount() const noexcept;
    juce::String getError() const;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VideoFrameView)
};
