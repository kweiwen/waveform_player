#include "VideoFrameView.h"

#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iterator>
#include <mutex>
#include <thread>

namespace
{
// Bound the lookahead by memory as well as frame count. Each 4K BGRA frame is
// about 33 MB, while small videos can afford a deeper queue for host jitter.
constexpr double prefetchBudgetBytes = 128.0 * 1024.0 * 1024.0;
constexpr double timeEpsilon = 0.001;

double nowSeconds() { return juce::Time::getMillisecondCounterHiRes() / 1000.0; }

struct Frame
{
    CVPixelBufferRef pixels = nullptr; // Retained.
    double seconds = 0.0;
    std::uint64_t jump = 0;
};
}

// This file is compiled without ARC: every alloc/retain is balanced by hand.
struct VideoFrameView::Impl
{
    explicit Impl (VideoFrameView& ownerIn) : owner (ownerIn)
    {
        view = [[NSView alloc] initWithFrame: NSMakeRect (0, 0, 16, 16)];
        // A layer-hosting view: the layer tree is ours, with y pointing up.
        view.layer = [CALayer layer];
        view.wantsLayer = YES;
        view.layer.backgroundColor = CGColorGetConstantColor (kCGColorBlack);
        imageLayer = [[CALayer alloc] init];
        imageLayer.contentsGravity = kCAGravityResize;
        [view.layer addSublayer: imageLayer];
        host.setView (view);
        owner.addAndMakeVisible (host);
    }

    ~Impl()
    {
        stopDecoder();
        host.setView (nullptr);
        [imageLayer removeFromSuperlayer];
        [imageLayer release];
        [view release];
        clearFrames();
        if (displayed.pixels != nullptr)
            CVPixelBufferRelease (displayed.pixels);
        [track release];
        [asset release];
    }

    void open (const juce::File& file)
    {
        jassert (! decoder.joinable()); // One file per view.
        path = file.getFullPathName();
        decoder = std::thread ([this] { run(); });
    }

    void showTime (double seconds, bool playing, double speed, std::uint64_t seekRevision)
    {
        CVPixelBufferRef replaced = nullptr;
        bool changed = false, relayout = false;
        {
            // A busy decoder may briefly own the queue while recycling pixel
            // buffers. Skip this refresh instead of stalling the host UI thread.
            const std::unique_lock<std::mutex> hold (lock, std::try_to_lock);
            if (! hold.owns_lock())
                return;
            if (track == nil)
                return;
            if (geometryChanged)
            {
                geometryChanged = false;
                relayout = true;
                shownSize = naturalSize;
                shownTransform = transform;
            }
            target = seconds;

            // Restart decoding when the time moved before what the current
            // reader can serve, or so far ahead that decoding through would
            // take longer than a restart. While playing, aim ahead by the
            // measured restart time so the new frames arrive on time.
            const auto lead = playing ? juce::jmax (1.0, speed) * restartSeconds : 0.0;
            const auto reach = juce::jmax (decodedUntil, jumpSeconds) + 1.0 + lead;
            // The audio clock can move backwards briefly when a host changes
            // callback size or timing. A sub-frame regression is not a seek;
            // restarting the reader for it causes a visible freeze. An explicit
            // seek gets no tolerance, but still restarts only when the current
            // reader cannot serve it: dragging seeks on every mouse move, and
            // restarting each time would never let a frame through.
            const auto explicitSeek = seekRevision != lastSeekRevision;
            const auto backwardsTolerance = explicitSeek ? timeEpsilon : juce::jmax (2.0 * frameSeconds, 0.05);
            if (jumpRequest == 0 || seconds < floorSeconds - backwardsTolerance || seconds > reach)
                requestJump (seconds, seconds + lead);
            lastSeekRevision = seekRevision;

            auto best = -1;
            // Frames from the previous reader remain queued until the new
            // reader delivers its first frame. Never present those after a seek.
            if (! frames.empty() && frames.front().jump == jumpRequest)
            {
                for (int index = 0; index < static_cast<int> (frames.size()); ++index)
                {
                    if (frames[static_cast<size_t> (index)].seconds > seconds + timeEpsilon)
                        break;
                    best = index;
                }
            }
            const auto stale = displayed.pixels == nullptr || displayed.jump != jumpRequest;
            // After a paused restart the first frame can start a hair after the
            // requested time; show it rather than the previous position. While
            // playing, the clock reaches it, and showing it early would lead.
            if (best < 0 && ! playing && stale && ! frames.empty()
                && frames.front().jump == jumpRequest
                && frames.front().seconds - seconds <= frameSeconds + timeEpsilon)
                best = 0;
            if (best >= 0)
            {
                const auto next = frames[static_cast<size_t> (best)];
                for (int index = 0; index < best; ++index)
                {
                    CVPixelBufferRelease (frames.front().pixels);
                    frames.pop_front();
                }
                frames.pop_front();
                if (next.seconds <= seconds + timeEpsilon)
                    floorSeconds = juce::jmax (floorSeconds, next.seconds);
                replaced = displayed.pixels;
                displayed = next;
                changed = true;
            }
        }
        wake.notify_one();

        if (relayout)
            layoutImage();
        if (changed)
        {
            [CATransaction begin];
            [CATransaction setDisableActions: YES];
            imageLayer.contents = (id) CVPixelBufferGetIOSurface (displayed.pixels);
            [CATransaction commit];
            displayedSeconds.store (displayed.seconds);
            if (replaced != nullptr)
                CVPixelBufferRelease (replaced);
        }
    }

    void layoutImage()
    {
        host.setBounds (owner.getLocalBounds());
        const auto width = static_cast<double> (owner.getWidth());
        const auto height = static_cast<double> (owner.getHeight());
        if (shownSize.width <= 0.0 || shownSize.height <= 0.0 || width <= 0.0 || height <= 0.0)
            return;
        // preferredTransform is expressed in y-down video coordinates; the
        // layer is y-up, so mirror its rotation before applying it.
        const auto linear = CGAffineTransformMake (shownTransform.a, -shownTransform.b,
                                                   -shownTransform.c, shownTransform.d, 0.0, 0.0);
        const auto rotated = CGRectApplyAffineTransform (
            CGRectMake (0.0, 0.0, shownSize.width, shownSize.height), linear).size;
        const auto scale = juce::jmin (width / std::abs (rotated.width), height / std::abs (rotated.height));
        [CATransaction begin];
        [CATransaction setDisableActions: YES];
        imageLayer.bounds = CGRectMake (0.0, 0.0, shownSize.width, shownSize.height);
        imageLayer.position = CGPointMake (width * 0.5, height * 0.5);
        imageLayer.affineTransform = CGAffineTransformConcat (linear, CGAffineTransformMakeScale (scale, scale));
        [CATransaction commit];
    }

    juce::String getError() const
    {
        const std::unique_lock<std::mutex> hold (lock, std::try_to_lock);
        if (! hold.owns_lock())
            return {};
        return error;
    }

    // Caller holds the lock.
    void requestJump (double cover, double start)
    {
        floorSeconds = cover;
        jumpSeconds = start;
        decodedUntil = start;
        ++jumpRequest;
    }

    // Caller holds the lock.
    bool needsFrames() const
    {
        auto ahead = 0;
        for (const auto& frame : frames)
            if (frame.seconds > target + timeEpsilon)
                ++ahead;
        return ahead < prefetchFrames;
    }

    void clearFrames()
    {
        for (auto& frame : frames)
            CVPixelBufferRelease (frame.pixels);
        frames.clear();
    }

    void stopDecoder()
    {
        {
            const std::lock_guard<std::mutex> hold (lock);
            quit = true;
        }
        wake.notify_one();
        if (decoder.joinable())
            decoder.join();
        quit = false;
    }

    bool openAsset()
    {
        @autoreleasepool
        {
            auto* url = [NSURL fileURLWithPath: [NSString stringWithUTF8String: path.toRawUTF8()]];
            auto* newAsset = [AVURLAsset URLAssetWithURL: url
                                                options: @{ AVURLAssetPreferPreciseDurationAndTimingKey: @YES }];
            auto* newTrack = newAsset != nil ? [[newAsset tracksWithMediaType: AVMediaTypeVideo] firstObject] : nil;
            const std::lock_guard<std::mutex> hold (lock);
            if (newTrack == nil)
            {
                error = "The file has no supported video track.";
                return false;
            }
            const auto minimum = newTrack.minFrameDuration;
            const auto rate = static_cast<double> (newTrack.nominalFrameRate);
            frameSeconds = CMTIME_IS_NUMERIC (minimum) && CMTimeGetSeconds (minimum) > 0.0
                ? CMTimeGetSeconds (minimum) : (rate > 0.0 ? 1.0 / rate : 1.0 / 30.0);
            durationSeconds = CMTimeGetSeconds (newAsset.duration);
            naturalSize = newTrack.naturalSize;
            transform = newTrack.preferredTransform;
            const auto bytesPerFrame = juce::jmax (1.0, std::abs (naturalSize.width)
                                                         * std::abs (naturalSize.height) * 4.0);
            prefetchFrames = juce::jlimit (4, 24, static_cast<int> (prefetchBudgetBytes / bytesPerFrame));
            geometryChanged = true;
            asset = [newAsset retain];
            track = [newTrack retain];
            return true;
        }
    }

    bool openReader (double start, AVAssetReader*& reader, AVAssetReaderTrackOutput*& output)
    {
        @autoreleasepool
        {
            NSError* failure = nil;
            reader = [[AVAssetReader alloc] initWithAsset: asset error: &failure];
            if (reader == nil)
                return false;
            NSDictionary* settings = @{
                (id) kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                (id) kCVPixelBufferIOSurfacePropertiesKey: @{}
            };
            output = [[AVAssetReaderTrackOutput alloc] initWithTrack: track outputSettings: settings];
            output.alwaysCopiesSampleData = NO;
            if (! [reader canAddOutput: output])
                return false;
            [reader addOutput: output];
            const auto last = std::isfinite (durationSeconds) ? juce::jmax (0.0, durationSeconds - frameSeconds) : start;
            // Include the frame just before the requested time. Starting the
            // output exactly at a non-frame timestamp skips the frame that
            // should be shown while paused.
            const auto outputStart = juce::jlimit (0.0, last, start - frameSeconds - timeEpsilon);
            reader.timeRange = CMTimeRangeMake (CMTimeMakeWithSeconds (outputStart, 600000),
                                                kCMTimePositiveInfinity);
            return [reader startReading];
        }
    }

    static void closeReader (AVAssetReader*& reader, AVAssetReaderTrackOutput*& output)
    {
        if (reader != nil)
            [reader cancelReading];
        [output release];
        [reader release];
        output = nil;
        reader = nil;
    }

    void run()
    {
        if (! openAsset())
            return;
        AVAssetReader* reader = nil;
        AVAssetReaderTrackOutput* output = nil;
        std::uint64_t handled = 0;
        double restartBegan = 0.0;
        bool awaitingFirst = false;
        for (;;)
        {
            auto restart = false;
            auto start = 0.0;
            {
                std::unique_lock<std::mutex> hold (lock);
                wake.wait (hold, [&]
                {
                    return quit || jumpRequest != handled || (reader != nil && ! endOfStream && needsFrames());
                });
                if (quit)
                    break;
                if (jumpRequest != handled)
                {
                    handled = jumpRequest;
                    start = jumpSeconds;
                    endOfStream = false;
                    restart = true;
                }
            }
            if (restart)
            {
                closeReader (reader, output);
                restartBegan = nowSeconds();
                awaitingFirst = true;
                if (! openReader (start, reader, output))
                {
                    closeReader (reader, output);
                    const std::lock_guard<std::mutex> hold (lock);
                    error = "The video track could not be decoded.";
                    continue;
                }
                readerStarts.fetch_add (1, std::memory_order_relaxed);
            }

            CMSampleBufferRef sample = nullptr;
            @autoreleasepool { sample = [output copyNextSampleBuffer]; }
            if (sample == nullptr)
            {
                const auto failed = reader.status == AVAssetReaderStatusFailed;
                const std::lock_guard<std::mutex> hold (lock);
                endOfStream = true;
                if (failed)
                    error = "The video track could not be decoded.";
                continue;
            }
            auto* pixels = CMSampleBufferGetImageBuffer (sample);
            const auto time = CMSampleBufferGetPresentationTimeStamp (sample);
            if (pixels != nullptr && CMTIME_IS_NUMERIC (time))
            {
                const Frame frame { CVPixelBufferRetain (pixels), CMTimeGetSeconds (time), handled };
                const std::lock_guard<std::mutex> hold (lock);
                if (jumpRequest != handled)
                {
                    CVPixelBufferRelease (frame.pixels); // Superseded by a newer jump.
                }
                else
                {
                    if (awaitingFirst)
                    {
                        awaitingFirst = false;
                        clearFrames();
                        restartSeconds = 0.7 * restartSeconds + 0.3 * (nowSeconds() - restartBegan);
                    }
                    auto position = frames.end();
                    while (position != frames.begin() && std::prev (position)->seconds > frame.seconds)
                        --position;
                    frames.insert (position, frame);
                    decodedUntil = juce::jmax (decodedUntil, frame.seconds);
                    // Keep one frame at or before the target; older ones are unreachable.
                    while (frames.size() > 1 && frames[1].seconds <= target + timeEpsilon)
                    {
                        CVPixelBufferRelease (frames.front().pixels);
                        frames.pop_front();
                    }
                }
            }
            CFRelease (sample);
        }
        closeReader (reader, output);
    }

    VideoFrameView& owner;
    juce::NSViewComponent host;
    NSView* view = nil;
    CALayer* imageLayer = nil;
    juce::String path;
    std::thread decoder;

    // Message thread only.
    Frame displayed;
    CGSize shownSize {};
    CGAffineTransform shownTransform = CGAffineTransformIdentity;
    std::atomic<double> displayedSeconds { -1.0 };
    std::atomic<std::uint64_t> readerStarts { 0 };

    // Shared with the decoder thread under `lock`.
    mutable std::mutex lock;
    std::condition_variable wake;
    std::deque<Frame> frames;
    bool quit = false, endOfStream = false, geometryChanged = false;
    std::uint64_t jumpRequest = 0;
    std::uint64_t lastSeekRevision = 0;
    double target = 0.0, jumpSeconds = 0.0, floorSeconds = 0.0, decodedUntil = 0.0;
    double restartSeconds = 0.2, frameSeconds = 1.0 / 30.0, durationSeconds = 0.0;
    int prefetchFrames = 4;
    AVAsset* asset = nil;
    AVAssetTrack* track = nil;
    CGSize naturalSize {};
    CGAffineTransform transform = CGAffineTransformIdentity;
    juce::String error;
};

VideoFrameView::VideoFrameView() : impl (std::make_unique<Impl> (*this)) {}
VideoFrameView::~VideoFrameView() = default;

void VideoFrameView::open (const juce::File& file) { impl->open (file); }
void VideoFrameView::showTime (double seconds, bool playing, double speed, std::uint64_t seekRevision)
{ impl->showTime (seconds, playing, speed, seekRevision); }
double VideoFrameView::getDisplayedFrameSeconds() const noexcept { return impl->displayedSeconds.load(); }
std::uint64_t VideoFrameView::getDecoderRestartCount() const noexcept { return impl->readerStarts.load(); }
juce::String VideoFrameView::getError() const { return impl->getError(); }
void VideoFrameView::resized() { impl->layoutImage(); }
void VideoFrameView::paint (juce::Graphics& g) { g.fillAll (juce::Colours::black); }
