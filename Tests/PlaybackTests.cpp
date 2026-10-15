#define JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP 1
#include "../Source/PluginProcessor.h"
#include "../Source/VideoFrameView.h"

#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <new>
#include <thread>

// Count C++ heap operations only while the audio callback is executing. Test
// buffers, fixture decoders, UI work and assertions are outside this scope.
namespace callbackHeap
{
thread_local bool observing = false;
thread_local unsigned long long allocations = 0;
thread_local unsigned long long deallocations = 0;

void* allocate (std::size_t size)
{
    if (observing) ++allocations;
    if (auto* result = std::malloc (size == 0 ? 1 : size)) return result;
    throw std::bad_alloc();
}
void release (void* pointer) noexcept
{
    if (observing && pointer != nullptr) ++deallocations;
    std::free (pointer);
}
void* allocateAligned (std::size_t size, std::size_t alignment)
{
    if (observing) ++allocations;
    void* result = nullptr;
    if (posix_memalign (&result, alignment, size == 0 ? 1 : size) == 0) return result;
    throw std::bad_alloc();
}
}

void* operator new (std::size_t size) { return callbackHeap::allocate (size); }
void* operator new[] (std::size_t size) { return callbackHeap::allocate (size); }
void operator delete (void* pointer) noexcept { callbackHeap::release (pointer); }
void operator delete[] (void* pointer) noexcept { callbackHeap::release (pointer); }
void operator delete (void* pointer, std::size_t) noexcept { callbackHeap::release (pointer); }
void operator delete[] (void* pointer, std::size_t) noexcept { callbackHeap::release (pointer); }
void* operator new (std::size_t size, std::align_val_t alignment)
{ return callbackHeap::allocateAligned (size, static_cast<std::size_t> (alignment)); }
void* operator new[] (std::size_t size, std::align_val_t alignment)
{ return callbackHeap::allocateAligned (size, static_cast<std::size_t> (alignment)); }
void operator delete (void* pointer, std::align_val_t) noexcept { callbackHeap::release (pointer); }
void operator delete[] (void* pointer, std::align_val_t) noexcept { callbackHeap::release (pointer); }
void operator delete (void* pointer, std::size_t, std::align_val_t) noexcept { callbackHeap::release (pointer); }
void operator delete[] (void* pointer, std::size_t, std::align_val_t) noexcept { callbackHeap::release (pointer); }

namespace
{
using Processor = WaveformPlayerAudioProcessor;
constexpr double sourceRate = 48000.0;
constexpr int shortLength = 31;

float stereoSample (int channel, int frame)
{
    const auto phase = juce::MathConstants<double>::twoPi * frame / sourceRate;
    return channel == 0 ? static_cast<float> (0.125 + 0.1 * std::sin (440.0 * phase))
                        : static_cast<float> (-0.0625 + 0.05 * std::cos (220.0 * phase));
}
float shortSample (int channel, int frame)
{
    return static_cast<float> ((channel == 0 ? 1.0 : -1.0) * (frame + 1) / 128.0);
}

class PlaybackTestApplication final : public juce::JUCEApplication, private juce::Timer
{
public:
    const juce::String getApplicationName() override { return "WaveformPlayer Playback Tests"; }
    const juce::String getApplicationVersion() override { return "0.2"; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void anotherInstanceStarted (const juce::String&) override {}
    void systemRequestedQuit() override { finish(); }

    void initialise (const juce::String&) override
    {
        const auto resultsPath = juce::SystemStats::getEnvironmentVariable ("WAVEFORM_PLAYER_TEST_RESULTS", {});
        const auto fixtureParent = resultsPath.isNotEmpty()
            ? juce::File (resultsPath) : juce::File::getSpecialLocation (juce::File::tempDirectory);
        fixtureDirectory = fixtureParent.getNonexistentChildFile ("WaveformPlayer-playback-tests", {}, false);
        if (! fixtureDirectory.createDirectory())
        {
            expect (false, "create temporary fixture directory");
            finish();
            return;
        }

        stereoFile = fixtureDirectory.getChildFile ("stereo-12-seconds.wav");
        monoFile = fixtureDirectory.getChildFile ("mono-constant.wav");
        shortFile = fixtureDirectory.getChildFile ("short-loop.wav");
        flacFile = fixtureDirectory.getChildFile ("mono-constant.flac");
        mp3File = fixtureDirectory.getChildFile ("stereo-tone.mp3");
        mp4File = fixtureDirectory.getChildFile ("stereo-video.mp4");
        movFile = fixtureDirectory.getChildFile ("stereo-video.mov");
        monoVideoFile = fixtureDirectory.getChildFile ("mono-video.mp4");
        silentVideoFile = fixtureDirectory.getChildFile ("video-without-audio.mp4");
        gopVideoFile = fixtureDirectory.getChildFile ("long-gop.mp4");
        floatFile = fixtureDirectory.getChildFile ("float-extremes.wav");
        invalidFile = fixtureDirectory.getChildFile ("invalid.wav");
        invalidFile.replaceWithText ("This is deliberately not an audio file.");
        expect (writeFixture (stereoFile, 2, 576000, sourceRate, stereoSample), "write stereo WAV fixture");
        expect (writeFixture (monoFile, 1, 44100, 44100.0, [] (int, int) { return 0.25f; }), "write mono WAV fixture");
        expect (writeFixture (shortFile, 2, shortLength, sourceRate, shortSample), "write short WAV fixture");
        expect (writeFixture (flacFile, 1, 8820, 44100.0, [] (int, int) { return 0.125f; }), "write FLAC fixture");
        expect (writeFloatFixture(), "write IEEE float WAV fixture");
        juce::ChildProcess encoder;
        const auto ffmpeg = juce::SystemStats::getEnvironmentVariable ("WAVEFORM_PLAYER_TEST_FFMPEG", "ffmpeg");
        const auto encoded = encoder.start (juce::StringArray { ffmpeg, "-v", "error", "-y", "-i",
            stereoFile.getFullPathName(), "-codec:a", "libmp3lame", "-b:a", "192k", mp3File.getFullPathName() })
            && encoder.waitForProcessToFinish (10000) && encoder.getExitCode() == 0;
        expect (encoded && mp3File.existsAsFile(), "encode synthetic MP3 fixture with ffmpeg");
        if (! encoded) std::cerr << encoder.readAllProcessOutput() << '\n';
        for (const auto& videoFile : { mp4File, movFile })
        {
            juce::ChildProcess videoEncoder;
            const auto videoEncoded = videoEncoder.start (juce::StringArray { ffmpeg, "-v", "error", "-y",
                "-f", "lavfi", "-i", "color=c=black:s=160x90:r=24:d=1", "-i",
                stereoFile.getFullPathName(), "-c:v", "libx264", "-pix_fmt", "yuv420p",
                "-c:a", "aac", "-t", "1", videoFile.getFullPathName() })
                && videoEncoder.waitForProcessToFinish (10000) && videoEncoder.getExitCode() == 0;
            expect (videoEncoded && videoFile.existsAsFile(), "encode synthetic " + videoFile.getFileExtension() + " fixture");
            if (! videoEncoded) std::cerr << videoEncoder.readAllProcessOutput() << '\n';
        }
        juce::ChildProcess monoVideoEncoder;
        const auto monoVideoEncoded = monoVideoEncoder.start (juce::StringArray { ffmpeg, "-v", "error", "-y",
            "-f", "lavfi", "-i", "color=c=black:s=160x90:r=24:d=1",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=1",
            "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest",
            monoVideoFile.getFullPathName() })
            && monoVideoEncoder.waitForProcessToFinish (10000) && monoVideoEncoder.getExitCode() == 0;
        expect (monoVideoEncoded && monoVideoFile.existsAsFile(), "encode synthetic mono MP4 fixture");
        if (! monoVideoEncoded) std::cerr << monoVideoEncoder.readAllProcessOutput() << '\n';
        juce::ChildProcess silentVideoEncoder;
        const auto silentVideoEncoded = silentVideoEncoder.start (juce::StringArray { ffmpeg, "-v", "error", "-y",
            "-f", "lavfi", "-i", "color=c=black:s=160x90:r=24:d=1",
            "-c:v", "libx264", "-pix_fmt", "yuv420p", silentVideoFile.getFullPathName() })
            && silentVideoEncoder.waitForProcessToFinish (10000) && silentVideoEncoder.getExitCode() == 0;
        expect (silentVideoEncoded && silentVideoFile.existsAsFile(), "encode synthetic video-only MP4 fixture");
        if (! silentVideoEncoded) std::cerr << silentVideoEncoder.readAllProcessOutput() << '\n';
        // Four seconds at 24 fps with a single keyframe and B-frames: every
        // seek decodes from the start, and decode order differs from display.
        juce::ChildProcess gopEncoder;
        const auto gopEncoded = gopEncoder.start (juce::StringArray { ffmpeg, "-v", "error", "-y",
            "-f", "lavfi", "-i", "testsrc2=s=320x180:r=24:d=4", "-c:v", "libx264", "-pix_fmt", "yuv420p",
            "-bf", "2", "-x264-params", "keyint=96:min-keyint=96:scenecut=0", gopVideoFile.getFullPathName() })
            && gopEncoder.waitForProcessToFinish (10000) && gopEncoder.getExitCode() == 0;
        expect (gopEncoded && gopVideoFile.existsAsFile(), "encode synthetic long-GOP MP4 fixture");
        if (! gopEncoded) std::cerr << gopEncoder.readAllProcessOutput() << '\n';
        if (failures != 0) { finish(); return; }

        processor = std::make_unique<Processor>();
        configure (*processor, 48000.0, 2);
        addAction ("empty processor", [this]
        {
            expect (processor->getPlaybackSpeed() == 1, "new processor defaults to 1x playback speed");
            processor->play();
            const auto output = render (*processor, 128);
            expect (! processor->getPlaybackSnapshot().loaded, "empty processor reports no loaded audio");
            expect (! processor->getPlaybackSnapshot().playing, "Play without an audio file stays stopped");
            expect (silent (output), "empty processor clears host output");
        });
        addScreenshot ("WaveformPlayer-empty.png", 0, 0);
        addLoad (stereoFile);
        addAction ("WAV metadata and transport", [this] { testTransport(); });
        addAction ("A/B marker lifecycle", [this] { testMarkers(); });
        addAction ("A/B transport bounds", [this] { testMarkerTransport(); });
        addMarkerEditorTests();
        addFileChooserTests();
        addScreenshot ("WaveformPlayer-ui.png", 0, 0, true);
        addScreenshot ("WaveformPlayer-fixed-size.png", 700, 400);
        addAction ("host rates and channel layouts", [this] { testRatesAndLayouts(); });
        addAction ("speed controls and automation", [this] { testSpeedControls(); });
        addAction ("audible position for picture sync", [this] { testAudiblePosition(); });
        addAction ("invalid file request", [this]
        {
            configure (*processor, sourceRate, 2);
            processor->seekToSeconds (2.0);
            processor->play();
            render (*processor, 64);
            beforeInvalid = processor->getPlaybackSnapshot();
            processor->loadAudioFile (invalidFile);
        });
        addWait ("invalid file completion", [this] { return ! processor->getPlaybackSnapshot().loading; });
        addAction ("invalid file preservation", [this]
        {
            const auto snapshot = processor->getPlaybackSnapshot();
            expect (snapshot.loaded && snapshot.file == stereoFile, "invalid load preserves loaded audio");
            expect (snapshot.playing, "invalid load preserves playback state");
            expect (near (snapshot.positionSeconds, beforeInvalid.positionSeconds), "invalid load preserves playback position");
            expect (near (snapshot.durationSeconds, 12.0), "invalid load preserves duration");
            expect (snapshot.error.isNotEmpty(), "invalid load exposes an error");
        });
        addLoad (monoFile);
        addAction ("replacement audio and gain", [this] { testGainAndMono(); });
        addLoad (shortFile);
        addAction ("EOF and looping", [this] { testEndAndLooping(); });
        addAction ("speed EOF and looping", [this] { testSpeedEndAndLooping(); });
        addAction ("A/B audio boundaries", [this] { testMarkerAudio(); });
        addLoad (flacFile);
        addAction ("FLAC decoding", [this]
        {
            configure (*processor, 44100.0, 2);
            processor->setGainDecibels (0.0f);
            processor->setMuted (false);
            processor->play();
            const auto output = render (*processor, 2048);
            expect (near (processor->getPlaybackSnapshot().durationSeconds, 0.2), "FLAC duration is decoded correctly");
            expect (near (output.getSample (0, 2000), 0.125, 2.0e-5), "FLAC PCM reaches the plugin output");
            expect (near (output.getSample (0, 2000), output.getSample (1, 2000), 1.0e-6), "mono FLAC duplicates to stereo outputs");
        });
        addLoad (mp3File);
        addAction ("MP3 decoding", [this]
        {
            configure (*processor, sourceRate, 2);
            processor->setLooping (false);
            processor->seekToSeconds (0.25);
            processor->play();
            const auto output = render (*processor, 2048);
            const auto snapshot = processor->getPlaybackSnapshot();
            expect (snapshot.durationSeconds >= 12.0 && snapshot.durationSeconds < 12.1, "MP3 duration includes at most codec padding");
            expect (near (snapshot.fileSampleRate, sourceRate), "MP3 sample rate is decoded correctly");
            expect (output.getMagnitude (0, 0, 2048) > 0.02f, "MP3 PCM reaches plugin output");
            expect (! near (output.getSample (0, 1000), output.getSample (1, 1000), 1.0e-3), "stereo MP3 preserves distinct channels");
        });
        for (const auto& videoFile : { mp4File, movFile })
        {
            addLoad (videoFile);
            addAction ("video audio and waveform " + videoFile.getFileExtension(), [this, videoFile]
            {
                const auto snapshot = processor->getPlaybackSnapshot();
                expect (snapshot.isVideo && snapshot.file == videoFile,
                        videoFile.getFileExtension() + " is identified as video");
                expect (snapshot.durationSeconds >= 0.95 && snapshot.durationSeconds <= 1.1,
                        videoFile.getFileExtension() + " has a valid media duration");
                expect (snapshot.fileChannels == 2 && near (snapshot.fileSampleRate, sourceRate),
                        videoFile.getFileExtension() + " preserves the audio format");
                expect (snapshot.videoWaveform != nullptr && ! snapshot.videoWaveform->empty(),
                        videoFile.getFileExtension() + " provides waveform data");
                processor->setGainDecibels (0.0f);
                processor->setMuted (false);
                processor->setPlaybackSpeed (1);
                processor->seekToSeconds (0.1);
                processor->play();
                expect (processor->getPlaybackSnapshot().playing
                            && ! processor->getPlaybackSnapshot().renderedPlaying,
                        videoFile.getFileExtension() + " waits for an audio callback before video starts");
                expect (render (*processor, 1024).getMagnitude (0, 0, 1024) > 0.01f,
                        videoFile.getFileExtension() + " audio reaches the host output");
                expect (processor->getPlaybackSnapshot().renderedPlaying,
                        videoFile.getFileExtension() + " reports playback after the audio callback");
                processor->pause();
            });
        }
        addVideoFrameTests();
        addLoad (monoVideoFile);
        addAction ("mono MP4 audio", [this]
        {
            const auto snapshot = processor->getPlaybackSnapshot();
            expect (snapshot.isVideo && snapshot.fileChannels == 1, "mono MP4 keeps one audio channel");
            processor->seekToSeconds (0.1);
            processor->play();
            expect (render (*processor, 1024).getMagnitude (0, 0, 1024) > 0.01f,
                    "mono MP4 audio reaches the host output");
            processor->pause();
        });
        addAction ("reject video without audio", [this]
        {
            processor->loadAudioFile (silentVideoFile);
        });
        addWait ("video-only rejection", [this] { return ! processor->getPlaybackSnapshot().loading; });
        addAction ("retain previous video after rejection", [this]
        {
            const auto snapshot = processor->getPlaybackSnapshot();
            expect (snapshot.loaded && snapshot.file == monoVideoFile && snapshot.isVideo,
                    "video without audio preserves the current video");
            expect (snapshot.error.isNotEmpty(), "video without audio reports a load error");
        });
        addLoad (floatFile);
        addAction ("floating-point sample safety", [this] { testFloatSamples(); });
        addLoad (stereoFile);
        addAction ("save state and restore request", [this]
        {
            configure (*processor, sourceRate, 2);
            processor->pause();
            processor->seekToSeconds (12345.0 / sourceRate);
            processor->setLooping (true);
            processor->setGainDecibels (-9.0f);
            processor->setMuted (true);
            processor->setPlaybackSpeed (4);
            render (*processor, 1);
            processor->getStateInformation (savedState);
            restored = std::make_unique<Processor>();
            configure (*restored, sourceRate, 2);
            restored->setStateInformation (savedState.getData(), static_cast<int> (savedState.getSize()));
        });
        addWait ("state audio reload", [this]
        {
            render (*restored, 1);
            const auto snapshot = restored->getPlaybackSnapshot();
            return snapshot.loaded && ! snapshot.loading;
        });
        addAction ("restored state", [this]
        {
            const auto snapshot = restored->getPlaybackSnapshot();
            expect (snapshot.file == stereoFile, "state restores the audio file");
            expect (! snapshot.playing, "state restore remains paused");
            expect (near (snapshot.positionSeconds, 12345.0 / sourceRate), "state restores sample-aligned playback position");
            expect (restored->isLooping(), "state restores loop setting");
            // The 0.1 dB float range can accumulate a few ulps when normalised,
            // snapped and serialised. This is still 1000x tighter than its step.
            std::cout << "State gain round-trip: " << juce::String (restored->getGainDecibels(), 8)
                      << " dB (target -9 dB).\n";
            expect (near (restored->getGainDecibels(), -9.0, 1.0e-4), "state restores gain");
            expect (restored->isMuted(), "state restores mute");
            expect (restored->getPlaybackSpeed() == 4, "state restores 4x playback speed");
            expect (silent (render (*restored, 128)), "restored paused output is silent");
            juce::MemoryBlock malformed;
            malformed.append ("broken-state", 12);
            restored->setStateInformation (malformed.getData(), static_cast<int> (malformed.getSize()));
            expect (restored->getPlaybackSnapshot().file == stereoFile, "malformed state does not discard loaded audio");
            expect (restored->getPlaybackSpeed() == 4, "malformed state preserves playback speed");
            processor->setPlaybackSpeed (1);
        });
        addAction ("restore legacy state without speed", [this]
        {
            auto xml = Processor::getXmlFromBinary (savedState.getData(), static_cast<int> (savedState.getSize()));
            expect (xml != nullptr, "saved state has XML for legacy-state fixture");
            if (xml == nullptr) return;
            auto legacyState = juce::ValueTree::fromXml (*xml);
            int removed = 0;
            for (int child = legacyState.getNumChildren(); --child >= 0;)
                if (legacyState.getChild (child).getProperty ("id").toString() == "speed")
                {
                    legacyState.removeChild (child, nullptr);
                    ++removed;
                }
            expect (removed == 1, "legacy-state fixture removes the saved speed parameter");
            juce::MemoryBlock legacyData;
            if (auto legacyXml = legacyState.createXml())
                Processor::copyXmlToBinary (*legacyXml, legacyData);
            restored->setPlaybackSpeed (8);
            restored->setStateInformation (legacyData.getData(), static_cast<int> (legacyData.getSize()));
            expect (restored->getPlaybackSpeed() == 1, "legacy state without speed resets prior 8x setting to 1x");
        });
        addWait ("legacy state audio reload", [this]
        {
            render (*restored, 1);
            const auto snapshot = restored->getPlaybackSnapshot();
            return snapshot.loaded && ! snapshot.loading;
        });
        addAction ("legacy state restores transport", [this]
        {
            const auto snapshot = restored->getPlaybackSnapshot();
            expect (snapshot.file == stereoFile && ! snapshot.playing,
                    "legacy state keeps the file and paused transport");
            expect (near (snapshot.positionSeconds, 12345.0 / sourceRate),
                    "legacy state keeps the saved playback position");
        });
        addAction ("restore valid empty state", [this]
        {
            Processor emptySource;
            juce::MemoryBlock emptyState;
            emptySource.getStateInformation (emptyState);
            restored->loadAudioFile (stereoFile);
            restored->setStateInformation (emptyState.getData(), static_cast<int> (emptyState.getSize()));
        });
        addWait ("clear restored audio", [this]
        {
            render (*restored, 1);
            const auto snapshot = restored->getPlaybackSnapshot();
            return ! snapshot.loading && ! snapshot.loaded;
        });
        addAction ("empty state output", [this]
        {
            expect (restored->getPlaybackSnapshot().file == juce::File(), "empty state clears the previous file reference");
            restored->play();
            expect (silent (render (*restored, 128)), "empty state removes previous audio from callback output");
            screenshotReadyAt = juce::Time::getMillisecondCounterHiRes() + 150.0;
        });
        addWait ("superseded load cancellation", [this]
        { return juce::Time::getMillisecondCounterHiRes() >= screenshotReadyAt; });
        addAction ("empty state stays empty", [this]
        {
            const auto snapshot = restored->getPlaybackSnapshot();
            expect (! snapshot.loaded && ! snapshot.loading, "empty state cancels in-flight file load without resurrection");
            expect (silent (render (*restored, 128)), "cancelled file load cannot restore prior audio output");
        });
        addAction ("start concurrent audio callback", [this] { startStress(); });
        addWait ("concurrent loading and playback", [this] { return runStressEdits(); });
        addWait ("final concurrent load", [this]
        {
            render (*processor, 1);
            return ! processor->getPlaybackSnapshot().loading;
        });
        addAction ("concurrent request result", [this]
        {
            const auto snapshot = processor->getPlaybackSnapshot();
            expect (snapshot.loaded && snapshot.file == lastStressFile, "latest concurrent load request wins");
        });
        const auto mediaPath = juce::SystemStats::getEnvironmentVariable ("WAVEFORM_PLAYER_TEST_MEDIA", {});
        if (mediaPath.isNotEmpty())
        {
            const juce::File mediaFile (mediaPath);
            addAction ("load supplied video", [this, mediaFile] { processor->loadAudioFile (mediaFile); });
            addWait ("supplied video audio decoded", [this, mediaFile]
            {
                render (*processor, 128);
                const auto snapshot = processor->getPlaybackSnapshot();
                return ! snapshot.loading && snapshot.loaded && snapshot.isVideo && snapshot.file == mediaFile;
            });
            addAction ("open supplied video preview", [this, mediaFile]
            {
                frameView = std::make_unique<VideoFrameView>();
                frameView->setSize (720, 440);
                frameView->open (mediaFile);
            });
            addWait ("supplied video frame at 30 seconds", [this]
            {
                frameView->showTime (30.0, false, 1.0);
                const auto shown = frameView->getDisplayedFrameSeconds();
                return shown >= 30.0 - 1.0 / 24.0 - 0.001 && shown <= 30.001;
            });
            addAction ("start supplied video playback clock", [this]
            {
                videoClockStartedAt = juce::Time::getMillisecondCounterHiRes();
                videoRestartsBeforeJitter = frameView->getDecoderRestartCount();
                videoFramesSeen = 0;
            });
            addWait ("supplied video keeps advancing under clock jitter", [this]
            {
                const auto elapsed = (juce::Time::getMillisecondCounterHiRes() - videoClockStartedAt) / 1000.0;
                const auto before = frameView->getDisplayedFrameSeconds();
                // Simulate a small backwards correction from a busy host.
                const auto jitter = static_cast<int> (elapsed * 1000.0) / 125 % 2 == 0 ? 0.025 : 0.0;
                frameView->showTime (30.0 + elapsed - jitter, true, 1.0);
                if (frameView->getDisplayedFrameSeconds() != before) ++videoFramesSeen;
                if (elapsed < 2.0) return false;
                expect (videoFramesSeen >= 25, "supplied video advances through at least 25 frames in two seconds");
                expect (frameView->getDecoderRestartCount() == videoRestartsBeforeJitter,
                        "supplied video keeps one reader despite clock jitter");
                return true;
            });
        }
        startedAt = juce::Time::getMillisecondCounterHiRes();
        stageStartedAt = startedAt;
        startTimer (5);
    }

    void shutdown() override
    {
        stopTimer();
        stopStress();
        editor.reset();
        frameView.reset();
        restored.reset();
        processor.reset();
        fixtureDirectory.deleteRecursively();
    }

private:
    struct Step { juce::String name; std::function<bool()> run; };
    std::deque<Step> steps;
    std::unique_ptr<Processor> processor, restored;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    juce::MemoryBlock savedState;
    Processor::PlaybackSnapshot beforeInvalid, beforeChooser;
    juce::File fixtureDirectory, stereoFile, monoFile, shortFile, flacFile, mp3File, mp4File, movFile,
               monoVideoFile, silentVideoFile, gopVideoFile, floatFile, invalidFile;
    std::unique_ptr<VideoFrameView> frameView;
    double videoClockStartedAt = 0.0, videoLatestLead = -1.0;
    int videoFramesSeen = 0;
    bool videoCaughtUp = false;
    std::uint64_t videoRestartsBeforeJitter = 0;
    int checks = 0, failures = 0, callbacks = 0;
    double startedAt = 0.0, stageStartedAt = 0.0, screenshotReadyAt = 0.0;
    float gainBeforeHostMute = 0.0f;
    bool finished = false;
    std::thread stressThread;
    std::atomic<bool> stressRunning { false };
    unsigned long long stressCallbacks = 0, stressAllocations = 0, stressDeallocations = 0;
    bool stressOutputFinite = true;
    double stressStartedAt = 0.0, nextStressEditAt = 0.0;
    int stressEdits = 0;
    juce::File lastStressFile;

    static bool near (double actual, double expected, double tolerance = 1.0e-8)
    { return std::isfinite (actual) && std::abs (actual - expected) <= tolerance; }

    void expect (bool condition, const juce::String& description)
    {
        ++checks;
        if (! condition)
        {
            ++failures;
            std::cerr << "FAIL: " << description << '\n';
        }
    }

    bool writeFixture (const juce::File& file, int channels, int frames, double rate,
                       const std::function<float (int, int)>& sample)
    {
        juce::AudioBuffer<float> audio (channels, frames);
        for (int channel = 0; channel < channels; ++channel)
            for (int frame = 0; frame < frames; ++frame)
                audio.setSample (channel, frame, sample (channel, frame));
        auto stream = file.createOutputStream();
        if (stream == nullptr || ! stream->openedOk()) return false;
        std::unique_ptr<juce::AudioFormat> format;
        if (file.hasFileExtension ("flac")) format = std::make_unique<juce::FlacAudioFormat>();
        else format = std::make_unique<juce::WavAudioFormat>();
        std::unique_ptr<juce::AudioFormatWriter> writer (format->createWriterFor (stream.get(), rate,
            static_cast<unsigned int> (channels), 24, {}, 0));
        if (writer == nullptr) return false;
        stream.release();
        return writer->writeFromAudioSampleBuffer (audio, 0, frames);
    }

    bool writeFloatFixture()
    {
        const auto maximum = std::numeric_limits<float>::max();
        const auto nan = std::numeric_limits<float>::quiet_NaN();
        const auto infinity = std::numeric_limits<float>::infinity();
        const float samples[] { maximum, maximum, -maximum, -maximum,
                                nan, infinity, -infinity, nan,
                                4.0f, 4.0f, 2.0f, -2.0f,
                                0.25f, -0.25f, maximum, -maximum };
        auto stream = floatFile.createOutputStream();
        if (stream == nullptr || ! stream->openedOk()) return false;
        // RIFF WAVE format 3 is native IEEEfloat32 rather than integer PCM.
        stream->write ("RIFF", 4);
        stream->writeInt (36 + static_cast<int> (sizeof (samples)));
        stream->write ("WAVEfmt ", 8);
        stream->writeInt (16);
        stream->writeShort (3);
        stream->writeShort (2);
        stream->writeInt (48000);
        stream->writeInt (48000 * 8);
        stream->writeShort (8);
        stream->writeShort (32);
        stream->write ("data", 4);
        stream->writeInt (static_cast<int> (sizeof (samples)));
        for (const auto sample : samples)
        {
            int bits = 0;
            static_assert (sizeof (bits) == sizeof (sample));
            std::memcpy (&bits, &sample, sizeof (bits));
            stream->writeInt (bits);
        }
        return stream->getStatus().wasOk();
    }

    void configure (Processor& target, double rate, int channels)
    {
        target.stop();
        target.releaseResources();
        auto layout = target.getBusesLayout();
        layout.outputBuses.set (0, channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo());
        expect (target.setBusesLayout (layout), "accept " + juce::String (channels) + " output channel(s)");
        target.setRateAndBufferSizeDetails (rate, 256);
        target.prepareToPlay (rate, 256);
        render (target, 1);
    }

    void testAudiblePosition()
    {
        configure (*processor, sourceRate, 2);
        processor->setPlaybackSpeed (1);
        processor->setLooping (false);
        const auto previousSeekRevision = processor->getPlaybackSnapshot().pictureSeekRevision;
        processor->seekToSeconds (2.0);
        processor->play();
        auto snapshot = processor->getPlaybackSnapshot();
        expect (! snapshot.renderedSeekReady && snapshot.pictureSeekRevision > previousSeekRevision,
                "picture waits for the audio callback after an explicit seek");
        render (*processor, 480);
        snapshot = processor->getPlaybackSnapshot();
        expect (snapshot.renderedSeekReady, "picture resumes after the audio callback applies the seek");
        expect (near (snapshot.positionSeconds, 2.01), "rendered position advances by one block");
        expect (near (snapshot.renderedPositionSeconds, 2.0, 1.0e-6),
                "picture holds at the seek target until it is audible");

        for (int block = 0; block != 4; ++block)
            render (*processor, 480);
        snapshot = processor->getPlaybackSnapshot();
        expect (snapshot.renderedPositionSeconds >= 2.03 - 1.0e-9
                    && snapshot.renderedPositionSeconds <= snapshot.positionSeconds,
                "picture trails the rendered audio by two buffers");

        processor->setOutputDeviceLatencySamples (4800);
        render (*processor, 480);
        snapshot = processor->getPlaybackSnapshot();
        expect (near (snapshot.renderedPositionSeconds, 2.0, 1.0e-6),
                "device latency delays the picture, never before the seek target");
        processor->setOutputDeviceLatencySamples (0);
        processor->pause();
        render (*processor, 64);
        snapshot = processor->getPlaybackSnapshot();
        expect (near (snapshot.renderedPositionSeconds, snapshot.positionSeconds),
                "paused picture position matches the transport");
    }

    // Drives the frame view the way the editor does, with a synthetic clock.
    void addVideoFrameTests()
    {
        constexpr double frame = 1.0 / 24.0;
        const auto showsFrameAt = [this] (double seconds)
        {
            frameView->showTime (seconds, false, 1.0);
            const auto shown = frameView->getDisplayedFrameSeconds();
            return shown >= seconds - frame - 1.0e-3 && shown <= seconds + 1.0e-3;
        };
        addAction ("open long-GOP video frames", [this]
        {
            frameView = std::make_unique<VideoFrameView>();
            frameView->setSize (320, 180);
            frameView->open (gopVideoFile);
        });
        addWait ("paused frame in the middle of a GOP", [showsFrameAt] { return showsFrameAt (2.5); });
        addAction ("brief audio clock regression", [this]
        {
            videoRestartsBeforeJitter = frameView->getDecoderRestartCount();
            videoClockStartedAt = juce::Time::getMillisecondCounterHiRes();
            frameView->showTime (2.47, true, 1.0);
        });
        addWait ("brief regression keeps the video reader", [this]
        {
            frameView->showTime (2.5, true, 1.0);
            if (juce::Time::getMillisecondCounterHiRes() - videoClockStartedAt < 150.0)
                return false;
            expect (frameView->getDecoderRestartCount() == videoRestartsBeforeJitter,
                    "sub-frame clock jitter does not restart video decoding");
            return true;
        });
        addAction ("explicit sub-frame seek", [this]
        {
            videoRestartsBeforeJitter = frameView->getDecoderRestartCount();
            frameView->showTime (2.47, false, 1.0, 1);
        });
        addWait ("explicit sub-frame seek shows the preceding frame", [this]
        {
            frameView->showTime (2.47, false, 1.0, 1);
            const auto shown = frameView->getDisplayedFrameSeconds();
            if (shown < 2.47 - 1.0 / 24.0 - 0.001 || shown > 2.471)
                return false;
            expect (frameView->getDecoderRestartCount() > videoRestartsBeforeJitter,
                    "explicit seek restarts the reader even within clock jitter tolerance");
            return true;
        });
        addWait ("paused frame after a backward jump", [showsFrameAt] { return showsFrameAt (0.5); });
        addAction ("start waveform drag", [this]
        {
            videoClockStartedAt = juce::Time::getMillisecondCounterHiRes();
            videoRestartsBeforeJitter = frameView->getDecoderRestartCount();
            videoFramesSeen = 0;
        });
        addWait ("dragging keeps the picture moving", [this]
        {
            // Dragging the waveform seeks on every mouse move (~60 per second),
            // here forward at half speed through the single long GOP.
            const auto elapsed = (juce::Time::getMillisecondCounterHiRes() - videoClockStartedAt) / 1000.0;
            const auto before = frameView->getDisplayedFrameSeconds();
            frameView->showTime (0.5 + 0.5 * elapsed, false, 1.0, 1000 + static_cast<std::uint64_t> (elapsed * 60.0));
            if (frameView->getDisplayedFrameSeconds() != before) ++videoFramesSeen;
            if (elapsed < 1.5) return false;
            expect (videoFramesSeen >= 10, "dragging shows new frames (" + juce::String (videoFramesSeen) + ")");
            expect (frameView->getDecoderRestartCount() - videoRestartsBeforeJitter <= 1,
                    "dragging forward decodes through instead of restarting per mouse move");
            return true;
        });
        addWait ("paused frame after a forward jump", [showsFrameAt] { return showsFrameAt (3.9); });
        for (const auto speed : { 1, 8 })
        {
            addAction ("start " + juce::String (speed) + "x video clock", [this]
            {
                videoClockStartedAt = juce::Time::getMillisecondCounterHiRes();
                videoLatestLead = -1.0;
                videoFramesSeen = 0;
                videoCaughtUp = false;
            });
            addWait (juce::String (speed) + "x video follows the clock", [this, speed]
            {
                const auto start = speed == 1 ? 1.0 : 0.2;
                const auto elapsed = (juce::Time::getMillisecondCounterHiRes() - videoClockStartedAt) / 1000.0;
                const auto clock = juce::jmin (3.95, start + speed * elapsed);
                const auto before = frameView->getDisplayedFrameSeconds();
                frameView->showTime (clock, true, speed);
                const auto shown = frameView->getDisplayedFrameSeconds();
                if (shown != before) ++videoFramesSeen;
                // The previous position stays on screen until the restarted
                // decoder delivers; measure only once the clock is followed.
                videoCaughtUp = videoCaughtUp || shown <= clock + 1.0e-3;
                if (videoCaughtUp)
                    videoLatestLead = juce::jmax (videoLatestLead, shown - clock);
                if (clock < 3.95) return false;
                expect (videoLatestLead <= 1.0e-3, juce::String (speed) + "x video is never ahead of the audio clock (lead "
                        + juce::String (videoLatestLead, 4) + " s)");
                expect (shown >= 3.95 - 2.0 * frame, juce::String (speed) + "x video reaches the clock position");
                expect (videoFramesSeen >= (speed == 1 ? 40 : 3),
                        juce::String (speed) + "x video advances frame by frame (" + juce::String (videoFramesSeen) + ")");
                return true;
            });
        }
        addAction ("video frames reject audio-only files", [this]
        {
            frameView = std::make_unique<VideoFrameView>();
            frameView->open (stereoFile);
        });
        addWait ("audio-only video error", [this]
        {
            frameView->showTime (0.0, false, 1.0);
            if (frameView->getError().isEmpty()) return false;
            expect (frameView->getDisplayedFrameSeconds() < 0.0, "audio-only file shows no video frame");
            frameView.reset();
            return true;
        });
    }

    juce::AudioBuffer<float> render (Processor& target, int frames)
    {
        juce::AudioBuffer<float> output (target.getTotalNumOutputChannels(), frames);
        for (int channel = 0; channel < output.getNumChannels(); ++channel)
            juce::FloatVectorOperations::fill (output.getWritePointer (channel), 0.75f, frames);
        juce::MidiBuffer midi;
        callbackHeap::observing = true;
        target.processBlock (output, midi);
        callbackHeap::observing = false;
        ++callbacks;
        return output;
    }

    static bool silent (const juce::AudioBuffer<float>& buffer)
    {
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            if (buffer.getMagnitude (channel, 0, buffer.getNumSamples()) != 0.0f) return false;
        return true;
    }

    void addAction (const juce::String& name, std::function<void()> action)
    {
        steps.push_back ({ name, [action = std::move (action)] { action(); return true; } });
    }
    void addWait (const juce::String& name, std::function<bool()> condition)
    { steps.push_back ({ name, std::move (condition) }); }
    void addLoad (const juce::File& file)
    {
        addAction ("load " + file.getFileName(), [this, file] { processor->loadAudioFile (file); });
        addWait ("decode " + file.getFileName(), [this, file]
        {
            render (*processor, 1);
            const auto snapshot = processor->getPlaybackSnapshot();
            if (snapshot.loading) return false;
            expect (snapshot.loaded && snapshot.file == file, "load " + file.getFileName());
            if (! snapshot.loaded || snapshot.file != file)
                std::cerr << "Load error: " << snapshot.error << '\n';
            return true;
        });
    }

    juce::TextButton* openButton() const
    {
        return editor != nullptr ? dynamic_cast<juce::TextButton*> (editor->findChildWithID ("open")) : nullptr;
    }

    static juce::FileBrowserComponent* findFileBrowser (juce::Component& parent)
    {
        if (auto* browser = dynamic_cast<juce::FileBrowserComponent*> (&parent)) return browser;
        for (int child = 0; child < parent.getNumChildComponents(); ++child)
            if (auto* browser = findFileBrowser (*parent.getChildComponent (child))) return browser;
        return nullptr;
    }

    void dismissChooser (const juce::File& selection = {})
    {
        auto* dialog = juce::ModalComponentManager::getInstance()->getModalComponent (0);
        expect (dynamic_cast<juce::FileChooserDialogBox*> (dialog) != nullptr,
                "non-native chooser exposes its modal dialog");
        if (dialog == nullptr) return;
        if (selection != juce::File())
        {
            auto* browser = findFileBrowser (*dialog);
            expect (browser != nullptr, "non-native chooser exposes its file browser");
            if (browser == nullptr) { dialog->exitModalState (0); return; }
            browser->setRoot (selection.getParentDirectory());
            browser->setFileName (selection.getFileName());
            expect (browser->currentFileIsValid() && browser->getSelectedFile (0) == selection,
                    "chooser selects the requested fixture");
        }
        // This invokes JUCE's real asynchronous completion path, including getResult().
        dialog->exitModalState (selection == juce::File() ? 0 : 1);
    }

    void addFileChooserTests()
    {
        addAction ("open non-native file chooser", [this]
        {
            editor.reset (processor->createEditor());
            auto* button = openButton();
            expect (button != nullptr && ! button->getToggleState() && button->isEnabled(),
                    "Folder starts closed and enabled");
            configure (*processor, sourceRate, 2);
            processor->seekToSeconds (2.0);
            processor->addMarker (2.0);
            processor->addMarker (9.0);
            processor->play();
            render (*processor, 64);
            beforeChooser = processor->getPlaybackSnapshot();
            processor->chooseAudioFile (false);
            const auto snapshot = processor->getPlaybackSnapshot();
            expect (snapshot.choosingFile && ! snapshot.loading,
                    "opening the chooser is distinct from decoding audio");
            expect (snapshot.file == beforeChooser.file && snapshot.fileRevision == beforeChooser.fileRevision
                    && snapshot.playing && near (snapshot.positionSeconds, beforeChooser.positionSeconds),
                    "opening the chooser preserves the current audio and transport");
            expect (snapshot.markerSamples == beforeChooser.markerSamples,
                    "opening the chooser preserves A/B markers");
            auto* modals = juce::ModalComponentManager::getInstance();
            auto* firstDialog = modals->getModalComponent (0);
            expect (firstDialog != nullptr && modals->getNumModalComponents() == 1, "one chooser dialog opens");
            processor->chooseAudioFile (false);
            expect (modals->getNumModalComponents() == 1 && modals->getModalComponent (0) == firstDialog,
                    "duplicate chooser requests keep the original dialog");
        });
        addWait ("Folder open state refresh", [this]
        {
            auto* button = openButton();
            return button != nullptr && button->getToggleState() && ! button->isEnabled();
        });
        addAction ("cancel chooser during playback", [this]
        {
            for (const auto& id : { juce::String ("rewind"), juce::String ("play"), juce::String ("pause"),
                                   juce::String ("stop"), juce::String ("forward"), juce::String ("loop"),
                                   "speed" + juce::String (processor->getPlaybackSpeed()), juce::String ("mute"),
                                   juce::String ("volume-off"), juce::String ("volume") })
            {
                auto* control = editor->findChildWithID (id);
                expect (control != nullptr && ! control->isEnabled(), id + " is disabled while choosing a file");
            }
            expect (! silent (render (*processor, 128)), "old audio stays audible while the chooser is open");
            expect (near (processor->getPlaybackSnapshot().positionSeconds,
                          beforeChooser.positionSeconds + 128.0 / sourceRate),
                    "playback continues advancing while choosing a file");
            beforeChooser = processor->getPlaybackSnapshot();
            processor->addMarker (4.0);
            expect (processor->getPlaybackSnapshot().markerSamples == beforeChooser.markerSamples,
                    "marker requests cannot change markers while the chooser is open");
            dismissChooser();
        });
        addWait ("cancelled chooser closes Folder", [this]
        {
            auto* button = openButton();
            return ! processor->getPlaybackSnapshot().choosingFile && button != nullptr
                   && ! button->getToggleState() && button->isEnabled();
        });
        addAction ("cancel preserves current playback", [this]
        {
            const auto snapshot = processor->getPlaybackSnapshot();
            expect (snapshot.loaded && ! snapshot.loading && snapshot.file == beforeChooser.file
                    && snapshot.fileRevision == beforeChooser.fileRevision && snapshot.error == beforeChooser.error,
                    "chooser cancellation preserves the loaded file without a load request");
            expect (snapshot.playing == beforeChooser.playing && near (snapshot.positionSeconds, beforeChooser.positionSeconds),
                    "chooser cancellation preserves playback state and position");
            expect (snapshot.markerSamples == beforeChooser.markerSamples,
                    "chooser cancellation preserves A/B markers");
            processor->chooseAudioFile (false);
        });
        addWait ("Folder reopens for valid selection", [this]
        { return openButton() != nullptr && openButton()->getToggleState(); });
        addAction ("accept valid chooser selection", [this] { dismissChooser (stereoFile); });
        addWait ("valid chooser selection completes", [this]
        {
            const auto snapshot = processor->getPlaybackSnapshot();
            auto* button = openButton();
            return ! snapshot.choosingFile && ! snapshot.loading
                   && snapshot.fileRevision != beforeChooser.fileRevision && button != nullptr
                   && ! button->getToggleState() && button->isEnabled();
        });
        addAction ("valid selection resets playback", [this]
        {
            const auto snapshot = processor->getPlaybackSnapshot();
            expect (snapshot.loaded && snapshot.file == stereoFile && snapshot.error.isEmpty(),
                    "chooser selection loads the chosen audio through the real callback");
            expect (! snapshot.playing && near (snapshot.positionSeconds, 0.0),
                    "successful chooser selection stops and resets playback");
            expect (snapshot.markerSamples[0] == -1 && snapshot.markerSamples[1] == -1
                    && near (snapshot.rangeStartSeconds, 0.0) && near (snapshot.rangeEndSeconds, 12.0),
                    "successful reload of the same file clears markers and restores full-file bounds");
            processor->seekToSeconds (3.0);
            processor->addMarker (3.0);
            processor->addMarker (9.0);
            processor->play();
            render (*processor, 64);
            beforeChooser = processor->getPlaybackSnapshot();
            processor->chooseAudioFile (false);
        });
        addWait ("Folder reopens for invalid selection", [this]
        { return openButton() != nullptr && openButton()->getToggleState(); });
        addAction ("accept invalid chooser selection", [this] { dismissChooser (invalidFile); });
        addWait ("invalid chooser selection completes", [this]
        {
            const auto snapshot = processor->getPlaybackSnapshot();
            auto* button = openButton();
            return ! snapshot.choosingFile && ! snapshot.loading && snapshot.error.isNotEmpty()
                   && button != nullptr && ! button->getToggleState() && button->isEnabled();
        });
        addAction ("invalid chooser selection preserves playback", [this]
        {
            const auto snapshot = processor->getPlaybackSnapshot();
            expect (snapshot.loaded && snapshot.file == beforeChooser.file
                    && snapshot.fileRevision == beforeChooser.fileRevision
                    && near (snapshot.durationSeconds, beforeChooser.durationSeconds),
                    "invalid chooser selection preserves the previous file and duration");
            expect (snapshot.playing == beforeChooser.playing && near (snapshot.positionSeconds, beforeChooser.positionSeconds),
                    "invalid chooser selection preserves playback state and position");
            expect (snapshot.markerSamples == beforeChooser.markerSamples
                    && near (snapshot.rangeStartSeconds, beforeChooser.rangeStartSeconds)
                    && near (snapshot.rangeEndSeconds, beforeChooser.rangeEndSeconds),
                    "invalid chooser selection preserves A/B markers and bounds");
            resetMarkerTestState();
            editor.reset();
        });
        addLoad (stereoFile);
    }

    void captureEditor (const juce::String& name)
    {
        const auto directory = juce::SystemStats::getEnvironmentVariable ("WAVEFORM_PLAYER_TEST_RESULTS", {});
        if (editor == nullptr || directory.isEmpty()) return;
        juce::File outputDirectory (directory);
        outputDirectory.createDirectory();
        const auto screenshotFile = outputDirectory.getChildFile (name);
        screenshotFile.deleteFile();
        auto stream = screenshotFile.createOutputStream();
        juce::PNGImageFormat png;
        expect (stream != nullptr && png.writeImageToStream (
            editor->createComponentSnapshot (editor->getLocalBounds()), *stream), "write editor snapshot " + name);
    }

    juce::Button* speedButton() const
    {
        if (editor == nullptr) return nullptr;
        return dynamic_cast<juce::Button*> (editor->findChildWithID (
            "speed" + juce::String (processor->getPlaybackSpeed())));
    }

    juce::TextButton* muteButton() const
    {
        if (editor == nullptr) return nullptr;
        return dynamic_cast<juce::TextButton*> (editor->findChildWithID ("mute"));
    }

    juce::Slider* volumeSlider() const
    {
        if (editor == nullptr) return nullptr;
        return dynamic_cast<juce::Slider*> (editor->findChildWithID ("volume"));
    }

    juce::TextButton* minimumVolumeButton() const
    {
        if (editor == nullptr) return nullptr;
        return dynamic_cast<juce::TextButton*> (editor->findChildWithID ("volume-off"));
    }

    void testMinimumVolumeButton()
    {
        auto* button = minimumVolumeButton();
        auto* slider = volumeSlider();
        const auto mutedBefore = processor->isMuted();
        expect (button != nullptr && button->isEnabled() == ! mutedBefore && button->onClick,
                "minimum-volume icon is available only during unmuted loaded playback");
        if (button == nullptr || slider == nullptr || ! button->onClick) return;

        const auto before = processor->getPlaybackSnapshot();
        const auto gainBefore = processor->getGainDecibels();
        button->onClick();
        const auto after = processor->getPlaybackSnapshot();
        const auto expectedGain = mutedBefore ? gainBefore : -60.0f;
        expect (near (processor->getGainDecibels(), expectedGain, 1.0e-4),
                mutedBefore ? "muted minimum-volume callback preserves the output gain"
                            : "minimum-volume icon sets the output gain to -60 dB");
        auto* parameter = processor->getParameterState().getRawParameterValue ("gainDb");
        expect (parameter != nullptr && near (parameter->load (std::memory_order_relaxed), expectedGain, 1.0e-4),
                "minimum-volume callback respects mute in the host gain parameter");
        // The slider snaps to 0.1 dB while the host parameter's normalisation
        // round trip can differ by a few float ulps.
        expect (near (slider->getMinimum(), -60.0) && near (slider->getValue(), expectedGain, 1.0e-4),
                "minimum-volume callback keeps the slider synchronised with the permitted gain");
        bool expectedGainIsDisplayed = false;
        for (int child = 0; child < editor->getNumChildComponents(); ++child)
            if (auto* label = dynamic_cast<juce::Label*> (editor->getChildComponent (child)))
                expectedGainIsDisplayed |= label->getText() == juce::String (expectedGain, 1) + " dB";
        expect (expectedGainIsDisplayed, "minimum-volume callback preserves the correct visible dB reading");
        expect (processor->isMuted() == mutedBefore && muteButton() != nullptr
                && muteButton()->getToggleState() == mutedBefore,
                "minimum-volume icon preserves the mute state");
        expect (after.playing == before.playing && near (after.positionSeconds, before.positionSeconds),
                "minimum-volume icon preserves playback and position");
        expect (slider->isEnabled() == ! mutedBefore, "minimum-volume icon preserves mute-dependent slider availability");
        processor->setGainDecibels (gainBefore);
    }

    void expectMuteAccessibility (bool muted)
    {
        auto* button = muteButton();
        auto* handler = button != nullptr ? button->getAccessibilityHandler() : nullptr;
        expect (handler != nullptr, "volume icon exposes an accessibility handler");
        if (handler == nullptr) return;

        const auto title = handler->getTitle();
        expect (title.isNotEmpty() && title == (muted ? "Unmute output" : "Mute output"),
                "screen reader receives the current volume icon action");
        const auto state = handler->getCurrentState();
        expect (state.isCheckable() && state.isChecked() == muted,
                "screen reader receives the current mute toggle state");
    }

    void testMuteButton()
    {
        auto* button = muteButton();
        expect (button != nullptr, "volume icon exposes the mute control");
        if (button == nullptr) return;

        const auto original = processor->getPlaybackSnapshot();
        const auto originalGain = processor->getGainDecibels();
        auto* slider = volumeSlider();
        auto* minimumButton = minimumVolumeButton();
        expect (slider != nullptr && slider->isEnabled() == (original.loaded && ! original.loading),
                "volume slider is available when unmuted audio is ready for playback");
        expect (minimumButton != nullptr
                && minimumButton->isEnabled() == (original.loaded && ! original.loading),
                "minimum-volume icon is enabled only with loaded audio ready for playback");
        expect (minimumButton != nullptr && minimumButton->getButtonText().isEmpty()
                && minimumButton->getWidth() == 33 && minimumButton->getHeight() == 36,
                "minimum-volume icon is textless and has a 33 x 36 pixel click target");
        auto* minimumHandler = minimumButton != nullptr ? minimumButton->getAccessibilityHandler() : nullptr;
        expect (minimumHandler != nullptr && minimumHandler->getTitle() == "Set volume to minimum",
                "screen reader identifies the minimum-volume action");
        expect (button->getButtonText().isEmpty(), "mute icon has no visible Mute or Muted label");
        expect (button->getClickingTogglesState(), "volume icon toggles its state when clicked");
        expect (button->getWidth() == 33 && button->getHeight() == 36,
                "mute icon keeps a 33 x 36 pixel click target");
        expect (button->isEnabled() == (original.loaded && ! original.loading),
                "mute icon is enabled only with loaded audio ready for playback");
        expect (button->getName() == "Mute output" && button->getTooltip().containsIgnoreCase ("mute"),
                "mute icon describes its action for accessibility and tooltips");
        expectMuteAccessibility (false);
        if (! original.loaded || ! button->onClick) return;

        processor->setGainDecibels (-6.0f);
        processor->play();
        expect (! silent (render (*processor, 512)), "loaded audio is audible before clicking the mute icon");
        testMinimumVolumeButton();
        const auto beforeHostMute = processor->getPlaybackSnapshot();
        const auto gainBeforeStaleClick = processor->getGainDecibels();
        auto* muteParameter = processor->getParameterState().getParameter ("mute");
        expect (minimumButton != nullptr && minimumButton->isEnabled() && muteParameter != nullptr,
                "minimum-volume icon is enabled before host mute arrives");
        if (minimumButton != nullptr && minimumButton->onClick && muteParameter != nullptr)
        {
            // No message-loop turn occurs between the host update and this stale UI callback.
            muteParameter->setValueNotifyingHost (1.0f);
            minimumButton->onClick();
            const auto afterStaleClick = processor->getPlaybackSnapshot();
            expect (processor->isMuted() && near (processor->getGainDecibels(), gainBeforeStaleClick, 1.0e-4),
                    "stale minimum-volume callback respects host mute before UI refresh");
            expect (afterStaleClick.playing == beforeHostMute.playing
                    && near (afterStaleClick.positionSeconds, beforeHostMute.positionSeconds),
                    "stale minimum-volume callback preserves playback and position");
            muteParameter->setValueNotifyingHost (0.0f);
        }
        const auto beforeMute = processor->getPlaybackSnapshot();
        // Calling onClick directly skips JUCE's mouse-handler toggle, so mirror
        // that state change before exercising the actual editor callback.
        button->setToggleState (true, juce::dontSendNotification);
        button->onClick();
        const auto afterMute = processor->getPlaybackSnapshot();
        expect (processor->isMuted() && button->getToggleState(), "volume icon click enables mute");
        expect (button->getButtonText().isEmpty() && button->getName() == "Unmute output",
                "muted volume icon stays textless and exposes the unmute action");
        expectMuteAccessibility (true);
        expect (slider != nullptr && ! slider->isEnabled(), "clicking mute disables volume slider interaction");
        expect (minimumButton != nullptr && ! minimumButton->isEnabled(), "clicking mute disables minimum-volume interaction");
        expect (near (processor->getGainDecibels(), -6.0, 1.0e-4), "volume icon mute preserves the selected gain");
        expect (afterMute.playing == beforeMute.playing && near (afterMute.positionSeconds, beforeMute.positionSeconds),
                "volume icon mute preserves playback and position");
        expect (silent (render (*processor, 128)), "volume icon mute immediately silences audio");
        const auto beforeUnmute = processor->getPlaybackSnapshot();
        expect (beforeUnmute.playing && near (beforeUnmute.positionSeconds,
                beforeMute.positionSeconds + 128.0 / sourceRate), "muted audio keeps advancing the transport");
        testMinimumVolumeButton();

        button->setToggleState (false, juce::dontSendNotification);
        button->onClick();
        const auto afterUnmute = processor->getPlaybackSnapshot();
        expect (! processor->isMuted() && ! button->getToggleState(), "second volume icon click disables mute");
        expect (button->getName() == "Mute output" && button->getButtonText().isEmpty(),
                "unmuted volume icon restores its accessible action without adding text");
        expectMuteAccessibility (false);
        expect (slider != nullptr && slider->isEnabled() && near (slider->getValue(), -6.0, 1.0e-4),
                "clicking unmute restores volume slider interaction and the selected gain");
        expect (minimumButton != nullptr && minimumButton->isEnabled(), "clicking unmute restores minimum-volume interaction");
        expect (near (processor->getGainDecibels(), -6.0, 1.0e-4), "volume icon unmute preserves the selected gain");
        expect (afterUnmute.playing == beforeUnmute.playing
                && near (afterUnmute.positionSeconds, beforeUnmute.positionSeconds),
                "volume icon unmute preserves playback and position");
        render (*processor, 512); // Let the 10 ms output-gain ramp settle.
        expect (render (*processor, 64).getMagnitude (0, 0, 64) > 0.02f,
                "volume icon unmute restores audible audio");

        processor->setGainDecibels (originalGain);
        if (original.playing) processor->play();
        else processor->pause();
        processor->seekToSeconds (original.positionSeconds);
    }

    void testSpeedButton()
    {
        auto* button = speedButton();
        expect (button != nullptr && button->isEnabled(), "speed button is available with or without loaded audio");
        if (button == nullptr || ! button->onClick) return;
        const auto before = processor->getPlaybackSnapshot();
        for (const auto speed : { 2, 4, 8, 1 })
        {
            button->onClick();
            expect (processor->getPlaybackSpeed() == speed, "speed button cycles to " + juce::String (speed) + "x");
            expect (button->getComponentID() == "speed" + juce::String (speed),
                    "speed button displays the selected " + juce::String (speed) + "x icon");
        }
        const auto after = processor->getPlaybackSnapshot();
        expect (after.loaded == before.loaded && after.playing == before.playing
                && near (after.positionSeconds, before.positionSeconds),
                "speed button cycle preserves loaded audio, transport and position");
        processor->setPlaybackSpeed (1);
    }

    void addScreenshot (const juce::String& name, int width, int height, bool captureSpeedVariant = false)
    {
        addAction ("create editor for " + name, [this, width, height]
        {
            editor.reset (processor->createEditor());
            expect (editor != nullptr, "create plugin editor");
            if (editor != nullptr)
            {
                // JUCE accessibility handlers require a native window handle.
                // Keep the peer hidden while testing real handlers and taking
                // component snapshots; editor destruction releases the peer.
                editor->setVisible (false);
                editor->addToDesktop (0);
                expect (editor->getWindowHandle() != nullptr, "snapshot editor has a native accessibility peer");
                expect (! editor->isShowing(), "snapshot editor remains hidden");
                expect (editor->getWidth() == 600 && editor->getHeight() == 360, "editor remains 600 x 360 pixels");
                testSpeedButton();
                testMuteButton();
            }
            if (editor != nullptr && width > 0 && height > 0)
            {
                const auto boundsBeforeResize = editor->getBounds();
                expect (! editor->isResizable(), "host resizing is disabled");
                editor->setBoundsConstrained (boundsBeforeResize.withSize (width, height));
                expect (editor->getBounds() == boundsBeforeResize, "host resize attempt preserves fixed editor bounds");
            }
            // Paint once to begin the normal startup animation, then capture
            // the settled interface after the JUCE badge has naturally faded.
            if (editor != nullptr)
                editor->createComponentSnapshot (editor->getLocalBounds());
            screenshotReadyAt = juce::Time::getMillisecondCounterHiRes() + 4500.0;
        });
        addWait ("editor timer refresh", [this]
        { return juce::Time::getMillisecondCounterHiRes() >= screenshotReadyAt; });
        addAction ("capture " + name, [this, name, captureSpeedVariant]
        {
            captureEditor (name);
            if (! captureSpeedVariant) editor.reset();
        });
        if (captureSpeedVariant)
        {
            addAction ("host automates mute for editor snapshot", [this]
            {
                gainBeforeHostMute = processor->getGainDecibels();
                auto* parameter = processor->getParameterState().getParameter ("mute");
                expect (parameter != nullptr, "host can access the mute parameter");
                if (parameter != nullptr) parameter->setValueNotifyingHost (1.0f);
                screenshotReadyAt = juce::Time::getMillisecondCounterHiRes() + 150.0;
            });
            addWait ("muted editor timer refresh", [this]
            { return juce::Time::getMillisecondCounterHiRes() >= screenshotReadyAt; });
            addAction ("capture muted editor", [this]
            {
                auto* button = muteButton();
                expect (processor->isMuted() && button != nullptr && button->getToggleState(),
                        "editor timer synchronises volume icon with host mute automation");
                expect (button != nullptr && button->getButtonText().isEmpty() && button->getName() == "Unmute output",
                        "host-muted volume icon stays textless and exposes the unmute action");
                expectMuteAccessibility (true);
                auto* slider = volumeSlider();
                expect (slider != nullptr && ! slider->isEnabled(), "host mute automation disables volume slider interaction");
                expect (minimumVolumeButton() != nullptr && ! minimumVolumeButton()->isEnabled(),
                        "host mute automation disables minimum-volume interaction");
                expect (slider != nullptr && near (slider->getValue(), gainBeforeHostMute, 1.0e-4)
                        && near (processor->getGainDecibels(), gainBeforeHostMute, 1.0e-4),
                        "host mute automation preserves the selected gain and slider reading");
                captureEditor ("WaveformPlayer-muted.png");
                if (auto* parameter = processor->getParameterState().getParameter ("mute"))
                    parameter->setValueNotifyingHost (0.0f);
                screenshotReadyAt = juce::Time::getMillisecondCounterHiRes() + 150.0;
            });
            addWait ("unmuted editor timer refresh", [this]
            { return juce::Time::getMillisecondCounterHiRes() >= screenshotReadyAt; });
            addAction ("host mute reset", [this]
            {
                auto* button = muteButton();
                expect (! processor->isMuted() && button != nullptr && ! button->getToggleState(),
                        "editor timer synchronises volume icon when host automation unmutes");
                expectMuteAccessibility (false);
                auto* slider = volumeSlider();
                expect (slider != nullptr && slider->isEnabled(), "host unmute automation restores volume slider interaction");
                expect (minimumVolumeButton() != nullptr && minimumVolumeButton()->isEnabled(),
                        "host unmute automation restores minimum-volume interaction");
                expect (slider != nullptr && near (slider->getValue(), gainBeforeHostMute, 1.0e-4)
                        && near (processor->getGainDecibels(), gainBeforeHostMute, 1.0e-4),
                        "host unmute automation preserves the selected gain and slider reading");
            });
            addAction ("select 8x for editor snapshot", [this]
            {
                if (auto* button = speedButton())
                    for (int click = 0; click < 3; ++click)
                        if (button->onClick) button->onClick();
                expect (processor->getPlaybackSpeed() == 8, "editor snapshot selects 8x through the speed button");
                screenshotReadyAt = juce::Time::getMillisecondCounterHiRes() + 150.0;
            });
            addWait ("8x editor timer refresh", [this]
            { return juce::Time::getMillisecondCounterHiRes() >= screenshotReadyAt; });
            addAction ("capture 8x editor", [this]
            {
                captureEditor ("WaveformPlayer-speed-8x.png");
                processor->setPlaybackSpeed (1);
                editor.reset();
            });
        }
    }

    void resetMarkerTestState()
    {
        processor->pause();
        processor->clearMarkers();
        processor->setPlaybackSpeed (1);
        processor->setLooping (false);
        processor->setGainDecibels (0.0f);
        processor->setMuted (false);
        processor->seekToSeconds (0.0);
        render (*processor, 1);
    }

    void setMarkers (double first, double second)
    {
        processor->pause();
        processor->clearMarkers();
        processor->addMarker (first);
        processor->addMarker (second);
        render (*processor, 1);
    }

    void testMarkers()
    {
        resetMarkerTestState();
        configure (*processor, sourceRate, 2);
        processor->seekToSeconds (6.0);
        processor->addMarker (2.0);
        auto snapshot = processor->getPlaybackSnapshot();
        expect (snapshot.markerSamples[0] == 96000 && snapshot.markerSamples[1] == -1
                && near (snapshot.rangeStartSeconds, 0.0) && near (snapshot.rangeEndSeconds, 12.0),
                "first marker retains full-file playback bounds");
        expect (! snapshot.playing && near (snapshot.positionSeconds, 6.0),
                "first marker uses its explicit time and preserves a different paused position");
        processor->addMarker (96000.49 / sourceRate);
        expect (processor->getPlaybackSnapshot().markerSamples == snapshot.markerSamples
                && near (processor->getPlaybackSnapshot().positionSeconds, 6.0),
                "same source sample ignores a duplicate marker");
        processor->addMarker (9.0);
        snapshot = processor->getPlaybackSnapshot();
        expect (snapshot.markerSamples[0] == 96000 && snapshot.markerSamples[1] == 432000
                && near (snapshot.rangeStartSeconds, 2.0) && near (snapshot.rangeEndSeconds, 9.0)
                && ! snapshot.playing && near (snapshot.positionSeconds, 2.0),
                "forward second marker establishes A/B and returns paused position to A");
        processor->seekToSeconds (6.0);
        expect (processor->getPlaybackSnapshot().markerSamples == snapshot.markerSamples,
                "ordinary seek leaves fixed markers in place");
        processor->addMarker (11.0);
        snapshot = processor->getPlaybackSnapshot();
        expect (snapshot.markerSamples[0] == 528000 && snapshot.markerSamples[1] == -1
                && ! snapshot.playing && near (snapshot.positionSeconds, 6.0)
                && near (snapshot.rangeStartSeconds, 0.0) && near (snapshot.rangeEndSeconds, 12.0),
                "third marker may start outside the old range without moving paused position");

        processor->clearMarkers();
        processor->seekToSeconds (8.0);
        processor->play();
        render (*processor, 16);
        processor->addMarker (7.0);
        expect (processor->getPlaybackSnapshot().markerSamples[0] == 336000
                && near (processor->getPlaybackSnapshot().positionSeconds, 384016.0 / sourceRate),
                "explicit first marker preserves the latest audio callback position during playback");
        processor->addMarker (3.0);
        snapshot = processor->getPlaybackSnapshot();
        expect (snapshot.markerSamples[0] == 144000 && snapshot.markerSamples[1] == 336000
                && snapshot.playing && near (snapshot.positionSeconds, 3.0),
                "reverse second marker sorts A/B and returns playing position to A");
        expect (near (render (*processor, 1).getSample (0, 0), stereoSample (0, 144000), 2.0e-5),
                "playing marker pair resumes audio at A");
        processor->seekToSeconds (5.0);
        render (*processor, 8);
        processor->addMarker (11.0);
        snapshot = processor->getPlaybackSnapshot();
        expect (snapshot.markerSamples[0] == 528000 && snapshot.markerSamples[1] == -1
                && snapshot.playing && near (snapshot.positionSeconds, 240008.0 / sourceRate),
                "third marker preserves the latest playing position");
        processor->clearMarkers();
        auto cleared = processor->getPlaybackSnapshot();
        expect (cleared.markerSamples[0] == -1 && cleared.markerSamples[1] == -1
                && cleared.playing && near (cleared.positionSeconds, snapshot.positionSeconds),
                "clear preserves playing state and position");
        setMarkers (2.0, 9.0);
        processor->seekToSeconds (6.0);
        processor->clearMarkers();
        cleared = processor->getPlaybackSnapshot();
        expect (! cleared.playing && near (cleared.positionSeconds, 6.0)
                && near (cleared.rangeStartSeconds, 0.0) && near (cleared.rangeEndSeconds, 12.0),
                "clear restores full bounds without moving paused position");
        resetMarkerTestState();
    }

    void testMarkerTransport()
    {
        resetMarkerTestState();
        setMarkers (2.0, 9.0);
        processor->seekToSeconds (-100.0);
        render (*processor, 1);
        expect (near (processor->getPlaybackSnapshot().positionSeconds, 2.0), "A/B seek clamps at A");
        processor->seekToSeconds (100.0);
        render (*processor, 1);
        expect (! processor->getPlaybackSnapshot().playing
                && near (processor->getPlaybackSnapshot().positionSeconds, 9.0), "paused A/B seek may stop at B");
        processor->seekToSeconds (8.0);
        processor->skipSeconds (-5.0);
        render (*processor, 1);
        expect (near (processor->getPlaybackSnapshot().positionSeconds, 3.0), "A/B Rewind keeps the five-second step");
        processor->skipSeconds (-5.0);
        render (*processor, 1);
        expect (near (processor->getPlaybackSnapshot().positionSeconds, 2.0), "A/B Rewind clamps at A");
        processor->skipSeconds (5.0);
        render (*processor, 1);
        expect (near (processor->getPlaybackSnapshot().positionSeconds, 7.0), "A/B Fast Forward keeps the five-second step");
        processor->skipSeconds (5.0);
        render (*processor, 1);
        expect (near (processor->getPlaybackSnapshot().positionSeconds, 9.0), "A/B Fast Forward clamps at B");
        processor->play();
        const auto output = render (*processor, 64);
        expect (near (output.getSample (0, 0), stereoSample (0, 96000), 2.0e-5), "Play from B restarts at A");
        processor->pause();
        const auto paused = processor->getPlaybackSnapshot();
        expect (silent (render (*processor, 64))
                && near (processor->getPlaybackSnapshot().positionSeconds, paused.positionSeconds)
                && processor->getPlaybackSnapshot().markerSamples == paused.markerSamples,
                "A/B Pause preserves position and markers while rendering silence");
        processor->stop();
        expect (silent (render (*processor, 1)) && near (processor->getPlaybackSnapshot().positionSeconds, 2.0),
                "A/B Stop returns to A and emits silence");
        processor->play();
        render (*processor, 1);
        processor->seekToSeconds (9.0);
        expect (silent (render (*processor, 4)) && ! processor->getPlaybackSnapshot().playing
                && near (processor->getPlaybackSnapshot().positionSeconds, 2.0),
                "playing seek to B with Loop Off stops at A without out-of-range output");
        processor->setLooping (true);
        processor->play();
        render (*processor, 1);
        processor->seekToSeconds (9.0);
        expect (near (render (*processor, 1).getSample (0, 0), stereoSample (0, 96000), 2.0e-5)
                && processor->getPlaybackSnapshot().playing,
                "playing seek to B with Loop On resumes at A");
        resetMarkerTestState();
    }

    juce::Component* waveformComponent() const
    {
        if (editor != nullptr)
            for (int child = 0; child < editor->getNumChildComponents(); ++child)
                if (editor->getChildComponent (child)->getTitle() == "Audio waveform")
                    return editor->getChildComponent (child);
        return nullptr;
    }

    static juce::Point<float> waveformPointAt (juce::Component& waveform, double seconds)
    {
        const auto plot = waveform.getLocalBounds().reduced (16, 0).withTrimmedTop (52).withTrimmedBottom (25).toFloat();
        return { plot.getX() + plot.getWidth() * static_cast<float> (seconds / 12.0), plot.getCentreY() };
    }

    static juce::MouseEvent mouseEvent (juce::Component& target, juce::Point<float> position,
                                       bool shift = false, bool rightButton = false)
    {
        const auto now = juce::Time::getCurrentTime();
        return { juce::Desktop::getInstance().getMainMouseSource(), position,
                 juce::ModifierKeys ((rightButton ? juce::ModifierKeys::rightButtonModifier
                                                 : juce::ModifierKeys::leftButtonModifier)
                                    | (shift ? juce::ModifierKeys::shiftModifier : 0)),
                 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &target, &target, now, position, now, 1, false };
    }

    void addMarkerEditorTests()
    {
        addAction ("right-click editor marker workflow", [this]
        {
            resetMarkerTestState();
            editor.reset (processor->createEditor());
            auto* waveform = waveformComponent();
            expect (waveform != nullptr, "editor exposes the waveform component for marker clicks");
            if (waveform == nullptr) return;
            const auto clickAt = [waveform] (double seconds, bool shift = false)
            {
                const auto event = mouseEvent (*waveform, waveformPointAt (*waveform, seconds), shift, true);
                waveform->mouseDown (event);
                waveform->mouseUp (event);
            };
            processor->seekToSeconds (6.0);
            waveform->mouseDown (mouseEvent (*waveform, waveformPointAt (*waveform, 9.0), false, true));
            waveform->mouseDrag (mouseEvent (*waveform, waveformPointAt (*waveform, 8.0), false, true));
            waveform->mouseUp (mouseEvent (*waveform, waveformPointAt (*waveform, 8.0), false, true));
            auto snapshot = processor->getPlaybackSnapshot();
            expect (snapshot.markerSamples[0] == 432000 && snapshot.markerSamples[1] == -1
                    && ! snapshot.playing && near (snapshot.positionSeconds, 6.0),
                    "first right-click uses its point without seeking, including its following right drag");
            clickAt (9.0);
            expect (processor->getPlaybackSnapshot().markerSamples == snapshot.markerSamples
                    && near (processor->getPlaybackSnapshot().positionSeconds, 6.0),
                    "right-click at the same source sample ignores a duplicate marker");
            clickAt (2.0);
            snapshot = processor->getPlaybackSnapshot();
            expect (snapshot.markerSamples[0] == 96000 && snapshot.markerSamples[1] == 432000
                    && ! snapshot.playing && near (snapshot.positionSeconds, 2.0),
                    "reverse second right-click sorts A/B and returns paused position to A");
            waveform->mouseDown (mouseEvent (*waveform, waveformPointAt (*waveform, 11.0), false, true));
            waveform->mouseDrag (mouseEvent (*waveform, waveformPointAt (*waveform, 1.0), false, true));
            waveform->mouseUp (mouseEvent (*waveform, waveformPointAt (*waveform, 1.0), false, true));
            snapshot = processor->getPlaybackSnapshot();
            expect (snapshot.markerSamples[0] == 528000 && snapshot.markerSamples[1] == -1
                    && near (snapshot.positionSeconds, 2.0) && near (snapshot.rangeStartSeconds, 0.0)
                    && near (snapshot.rangeEndSeconds, 12.0),
                    "third right-click uses a point outside the old A/B range without seeking or starting a drag");
            clickAt (6.0, true);
            expect (processor->getPlaybackSnapshot().markerSamples[0] == -1
                    && near (processor->getPlaybackSnapshot().positionSeconds, 2.0),
                    "Shift-right-click at blank space clears a single marker without seeking");
            clickAt (2.0);
            clickAt (9.0);
            snapshot = processor->getPlaybackSnapshot();
            expect (! editor->keyPressed (juce::KeyPress ('m'))
                    && processor->getPlaybackSnapshot().markerSamples == snapshot.markerSamples
                    && near (processor->getPlaybackSnapshot().positionSeconds, snapshot.positionSeconds),
                    "M is unhandled and leaves A/B and the playhead unchanged");
            screenshotReadyAt = juce::Time::getMillisecondCounterHiRes() + 150.0;
        });
        addWait ("A/B Ready UI refresh", [this] { return juce::Time::getMillisecondCounterHiRes() >= screenshotReadyAt; });
        addAction ("A/B Ready and editor reopen", [this]
        {
            auto* rewind = editor->findChildWithID ("rewind");
            auto* stop = editor->findChildWithID ("stop");
            expect (rewind != nullptr && stop != nullptr && ! rewind->isEnabled() && ! stop->isEnabled()
                    && processor->getPlaybackSnapshot().status == "Ready", "paused A has Ready UI with Rewind and Stop disabled");
            const auto markers = processor->getPlaybackSnapshot().markerSamples;
            editor.reset();
            editor.reset (processor->createEditor());
            expect (processor->getPlaybackSnapshot().markerSamples == markers, "closing and reopening editor preserves A/B");
            processor->seekToSeconds (9.0);
            render (*processor, 1);
            screenshotReadyAt = juce::Time::getMillisecondCounterHiRes() + 150.0;
        });
        addWait ("A/B endpoint UI refresh", [this] { return juce::Time::getMillisecondCounterHiRes() >= screenshotReadyAt; });
        addAction ("A/B endpoint controls", [this]
        {
            auto* forward = editor->findChildWithID ("forward");
            auto* stop = editor->findChildWithID ("stop");
            expect (forward != nullptr && stop != nullptr && ! forward->isEnabled() && stop->isEnabled()
                    && processor->getPlaybackSnapshot().status == "Paused", "paused B has Paused UI with Fast Forward disabled");
            processor->seekToSeconds (6.0);
            render (*processor, 1);
            editor->createComponentSnapshot (editor->getLocalBounds());
            screenshotReadyAt = juce::Time::getMillisecondCounterHiRes() + 4500.0;
        });
        addWait ("A/B waveform screenshot refresh", [this] { return juce::Time::getMillisecondCounterHiRes() >= screenshotReadyAt; });
        addAction ("A/B waveform mouse gestures", [this]
        {
            captureEditor ("WaveformPlayer-ab-range.png");
            auto* waveform = waveformComponent();
            expect (waveform != nullptr, "editor exposes the waveform component for mouse gestures");
            if (waveform != nullptr)
            {
                const auto pointAt = [waveform] (double seconds) { return waveformPointAt (*waveform, seconds); };
                const auto markers = processor->getPlaybackSnapshot().markerSamples;
                waveform->mouseDown (mouseEvent (*waveform, pointAt (2.0)));
                expect (near (processor->getPlaybackSnapshot().positionSeconds, 2.0), "ordinary marker click still seeks");
                waveform->mouseDrag (mouseEvent (*waveform, pointAt (7.0)));
                waveform->mouseUp (mouseEvent (*waveform, pointAt (7.0)));
                expect (near (processor->getPlaybackSnapshot().positionSeconds, 7.0, 1.0 / sourceRate)
                        && processor->getPlaybackSnapshot().markerSamples == markers, "ordinary waveform drag seeks without moving A/B");
                waveform->mouseDown (mouseEvent (*waveform, pointAt (9.0), true));
                waveform->mouseUp (mouseEvent (*waveform, pointAt (9.0), true));
                expect (near (processor->getPlaybackSnapshot().positionSeconds, 9.0)
                        && processor->getPlaybackSnapshot().markerSamples == markers,
                        "Shift-left-click on a marker tick seeks without clearing A/B");
                waveform->mouseDown (mouseEvent (*waveform, pointAt (6.0), true));
                waveform->mouseUp (mouseEvent (*waveform, pointAt (6.0), true));
                expect (near (processor->getPlaybackSnapshot().positionSeconds, 6.0, 1.0 / sourceRate), "Shift on waveform blank space still seeks");
                processor->play();
                render (*processor, 8);
                const auto beforeClear = processor->getPlaybackSnapshot();
                waveform->mouseDown (mouseEvent (*waveform, pointAt (4.0), true, true));
                waveform->mouseDrag (mouseEvent (*waveform, pointAt (11.0), true, true));
                waveform->mouseUp (mouseEvent (*waveform, pointAt (11.0), true, true));
                const auto cleared = processor->getPlaybackSnapshot();
                expect (cleared.markerSamples[0] == -1 && cleared.markerSamples[1] == -1
                        && cleared.playing && near (cleared.positionSeconds, beforeClear.positionSeconds),
                        "Shift-right-click at blank space clears A/B without seeking, including its following right drag");
            }
            captureEditor ("WaveformPlayer-ab-cleared.png");
            resetMarkerTestState();
            editor.reset();
        });
    }

    void testTransport()
    {
        configure (*processor, sourceRate, 2);
        auto snapshot = processor->getPlaybackSnapshot();
        expect (snapshot.fileName == stereoFile.getFileName(), "loaded file name is available");
        expect (near (snapshot.durationSeconds, 12.0), "WAV duration uses source frame count");
        expect (near (snapshot.fileSampleRate, sourceRate), "WAV sample rate is available");
        expect (! snapshot.playing && near (snapshot.positionSeconds, 0.0), "successful load stops and resets position");

        processor->seekToSeconds (12345.49 / sourceRate);
        render (*processor, 1);
        expect (near (processor->getPlaybackSnapshot().positionSeconds, 12345.0 / sourceRate), "paused seek rounds to nearest source sample (down)");
        processor->seekToSeconds (12345.51 / sourceRate);
        render (*processor, 1);
        expect (near (processor->getPlaybackSnapshot().positionSeconds, 12346.0 / sourceRate), "paused seek rounds to nearest source sample (up)");
        expect (! processor->getPlaybackSnapshot().playing, "seek while paused does not start playback");
        processor->play();
        auto output = render (*processor, 128);
        expect (near (output.getSample (0, 0), stereoSample (0, 12346), 2.0e-5), "Play starts at the sample-accurate seek target");
        expect (near (processor->getPlaybackSnapshot().positionSeconds, 12474.0 / sourceRate), "playing advances by the rendered sample count");
        processor->pause();
        const auto pausedPosition = processor->getPlaybackSnapshot().positionSeconds;
        expect (silent (render (*processor, 128)), "Pause emits silence");
        expect (near (processor->getPlaybackSnapshot().positionSeconds, pausedPosition), "Pause preserves position");
        processor->play();
        output = render (*processor, 128);
        expect (near (output.getSample (0, 0), stereoSample (0, 12474), 2.0e-5), "Play resumes from paused position");
        processor->seekToSeconds (100001.0 / sourceRate);
        output = render (*processor, 128);
        expect (near (output.getSample (1, 0), stereoSample (1, 100001), 2.0e-5), "seek during playback jumps to target and continues");
        expect (processor->getPlaybackSnapshot().playing, "seek during playback remains playing");
        processor->stop();
        expect (silent (render (*processor, 128)), "Stop emits silence");
        expect (near (processor->getPlaybackSnapshot().positionSeconds, 0.0), "Stop returns to origin");
        processor->seekToSeconds (6.0);
        processor->skipSeconds (-5.0);
        render (*processor, 1);
        expect (near (processor->getPlaybackSnapshot().positionSeconds, 1.0), "Rewind jumps backward 5 seconds");
        processor->skipSeconds (-5.0);
        render (*processor, 1);
        expect (near (processor->getPlaybackSnapshot().positionSeconds, 0.0), "Rewind clamps at origin");
        processor->skipSeconds (5.0);
        render (*processor, 1);
        expect (near (processor->getPlaybackSnapshot().positionSeconds, 5.0), "Fast Forward jumps forward 5 seconds");
        processor->seekToSeconds (11.0);
        processor->skipSeconds (5.0);
        render (*processor, 1);
        expect (near (processor->getPlaybackSnapshot().positionSeconds, 12.0), "Fast Forward clamps at duration");
        processor->seekToSeconds (-100.0);
        render (*processor, 1);
        expect (near (processor->getPlaybackSnapshot().positionSeconds, 0.0), "negative seek clamps at origin");
    }

    void testRatesAndLayouts()
    {
        for (const auto rate : { 44100.0, 48000.0, 88200.0, 96000.0 })
            for (const auto channels : { 1, 2 })
                for (const auto speed : { 1, 2, 4, 8 })
            {
                const auto label = juce::String (rate, 0) + " Hz / " + juce::String (channels)
                    + " channel(s) / " + juce::String (speed) + "x";
                configure (*processor, rate, channels);
                processor->setGainDecibels (0.0f);
                processor->setMuted (false);
                processor->setLooping (false);
                processor->setPlaybackSpeed (speed);
                processor->seekToSeconds (1.0);
                processor->play();
                const auto output = render (*processor, 257);
                expect (processor->getPlaybackSnapshot().playing, label + " plays independently of DAW transport");
                expect (near (processor->getPlaybackSnapshot().positionSeconds,
                              1.0 + 257.0 * speed / rate, 1.0e-9), label + " advances source position at the selected speed");
                bool samplesCorrect = true;
                for (int frame = 0; frame < output.getNumSamples(); ++frame)
                {
                    const auto sourceFrame = 48000.0 + frame * sourceRate * speed / rate;
                    const auto first = static_cast<int> (sourceFrame);
                    const auto fraction = sourceFrame - first;
                    const auto sampleAt = [first, fraction] (int channel)
                    {
                        const auto a = stereoSample (channel, first);
                        return a + fraction * (stereoSample (channel, first + 1) - a);
                    };
                    if (channels == 2)
                        for (int channel = 0; channel < channels; ++channel)
                            samplesCorrect = samplesCorrect && near (output.getSample (channel, frame), sampleAt (channel), 2.0e-5);
                    else
                        samplesCorrect = samplesCorrect && near (output.getSample (0, frame),
                            0.5 * (sampleAt (0) + sampleAt (1)), 2.0e-5);
                }
                expect (samplesCorrect, label + " samples the expected source frames with channel conversion");
            }
        processor->setPlaybackSpeed (1);
    }

    void testSpeedControls()
    {
        expect (processor->getPlaybackSpeed() == 1, "default playback speed is 1x");
        for (const auto speed : { 2, 4, 8, 1 })
        {
            processor->cyclePlaybackSpeed();
            expect (processor->getPlaybackSpeed() == speed, "speed cycle selects " + juce::String (speed) + "x");
        }
        processor->setPlaybackSpeed (4);
        for (const auto invalid : { -1, 0, 3, 5, 16 })
        {
            processor->setPlaybackSpeed (invalid);
            expect (processor->getPlaybackSpeed() == 4, "invalid speed " + juce::String (invalid) + " preserves 4x");
        }

        auto* choice = dynamic_cast<juce::AudioParameterChoice*> (processor->getParameterState().getParameter ("speed"));
        expect (choice != nullptr, "speed is an automatable AudioParameterChoice");
        if (choice != nullptr)
        {
            expect (choice->choices.size() == 4, "speed automation offers four choices");
            int index = 0;
            for (const auto speed : { 1, 2, 4, 8 })
            {
                processor->setPlaybackSpeed (speed);
                expect (choice->getIndex() == index, "setting " + juce::String (speed) + "x updates automation index");
                ++index;
            }
            index = 0;
            for (const auto speed : { 1, 2, 4, 8 })
            {
                choice->setValueNotifyingHost (choice->convertTo0to1 (static_cast<float> (index++)));
                expect (processor->getPlaybackSpeed() == speed
                        && processor->getPlaybackSnapshot().playbackSpeed == speed,
                        "host automation selects " + juce::String (speed) + "x in controls and snapshots");
            }
        }

        configure (*processor, sourceRate, 2);
        processor->setPlaybackSpeed (1);
        processor->seekToSeconds (1.0);
        processor->play();
        render (*processor, 64);
        const auto playingPosition = processor->getPlaybackSnapshot().positionSeconds;
        processor->setPlaybackSpeed (8);
        auto snapshot = processor->getPlaybackSnapshot();
        expect (snapshot.playing && near (snapshot.positionSeconds, playingPosition),
                "changing speed while playing preserves transport and position");
        const auto faster = render (*processor, 17);
        expect (near (faster.getSample (0, 0), stereoSample (0, 48064), 2.0e-5),
                "speed change starts from the current source position");
        expect (near (processor->getPlaybackSnapshot().positionSeconds, 48200.0 / sourceRate),
                "next callback advances at 8x after speed change");
        processor->pause();
        render (*processor, 1);
        const auto pausedPosition = processor->getPlaybackSnapshot().positionSeconds;
        processor->setPlaybackSpeed (2);
        snapshot = processor->getPlaybackSnapshot();
        expect (! snapshot.playing && near (snapshot.positionSeconds, pausedPosition),
                "changing speed while paused preserves transport and position");
        expect (silent (render (*processor, 64))
                && near (processor->getPlaybackSnapshot().positionSeconds, pausedPosition),
                "speed change while paused continues to emit silence without advancing");
        processor->play();
        const auto resumed = render (*processor, 17);
        expect (near (resumed.getSample (0, 0), stereoSample (0, 48200), 2.0e-5)
                && near (processor->getPlaybackSnapshot().positionSeconds, 48234.0 / sourceRate),
                "resume uses the selected 2x speed from the paused position");
        processor->setPlaybackSpeed (1);
    }

    void testGainAndMono()
    {
        auto snapshot = processor->getPlaybackSnapshot();
        expect (! snapshot.playing && near (snapshot.positionSeconds, 0.0), "replacement while playing stops and returns to origin");
        configure (*processor, 44100.0, 2);
        processor->setLooping (true);
        processor->setMuted (false);
        processor->play();
        for (const auto gainDb : { -60.0f, 6.0f })
        {
            processor->setGainDecibels (gainDb);
            render (*processor, 2205); // Let the 10 ms gain ramp settle.
            const auto output = render (*processor, 64);
            const auto expected = 0.25f * juce::Decibels::decibelsToGain (gainDb, -100.0f);
            expect (near (processor->getGainDecibels(), gainDb), "gain value tracks " + juce::String (gainDb) + " dB");
            expect (near (output.getSample (0, 0), expected, 2.0e-5), "audio gain reaches " + juce::String (gainDb) + " dB");
            expect (near (output.getSample (0, 0), output.getSample (1, 0), 1.0e-6), "mono WAV duplicates to both stereo outputs");
        }
        processor->setMuted (true);
        expect (processor->isMuted() && silent (render (*processor, 64)), "Mute immediately silences output");
        processor->setMuted (false);
        render (*processor, 2205);
        expect (render (*processor, 64).getMagnitude (0, 0, 64) > 0.1f, "unmute restores output");
        processor->setGainDecibels (-100.0f);
        expect (near (processor->getGainDecibels(), -60.0), "gain clamps to -60 dB minimum");
        processor->setGainDecibels (100.0f);
        expect (near (processor->getGainDecibels(), 6.0), "gain clamps to +6 dB maximum");
        processor->setGainDecibels (0.0f);
        render (*processor, 2205);
    }

    void testEndAndLooping()
    {
        configure (*processor, sourceRate, 2);
        processor->setGainDecibels (0.0f);
        processor->setMuted (false);
        processor->setLooping (false);
        processor->seekToSeconds (27.0 / sourceRate);
        processor->play();
        const auto ending = render (*processor, 64);
        bool tailCorrect = true;
        for (int channel = 0; channel < 2; ++channel)
            for (int frame = 0; frame < 64; ++frame)
                tailCorrect = tailCorrect && near (ending.getSample (channel, frame),
                    frame < 4 ? shortSample (channel, 27 + frame) : 0.0f, 2.0e-5);
        expect (tailCorrect, "Loop OFF renders final samples then clears remaining block");
        expect (! processor->getPlaybackSnapshot().playing, "Loop OFF stops at EOF");
        expect (near (processor->getPlaybackSnapshot().positionSeconds, 0.0), "Loop OFF returns to origin at EOF");
        expect (silent (render (*processor, 64)), "block after EOF stays silent");
        processor->setLooping (true);
        processor->seekToSeconds (27.0 / sourceRate);
        processor->play();
        const auto looping = render (*processor, 64);
        bool wrapsCorrectly = true;
        for (int channel = 0; channel < 2; ++channel)
            for (int frame = 0; frame < 64; ++frame)
                wrapsCorrectly = wrapsCorrectly && near (looping.getSample (channel, frame),
                    shortSample (channel, (27 + frame) % shortLength), 2.0e-5);
        expect (wrapsCorrectly, "Loop ON wraps the entire file sample-for-sample within a block");
        expect (processor->getPlaybackSnapshot().playing, "Loop ON remains playing across EOF");
        expect (near (processor->getPlaybackSnapshot().positionSeconds, 29.0 / sourceRate), "loop position wraps by source sample count");
    }

    void testSpeedEndAndLooping()
    {
        for (const auto speed : { 1, 2, 4, 8 })
        {
            configure (*processor, sourceRate, 2);
            processor->setGainDecibels (0.0f);
            processor->setMuted (false);
            processor->setLooping (false);
            processor->setPlaybackSpeed (speed);
            processor->seekToSeconds (27.0 / sourceRate);
            processor->play();
            const auto output = render (*processor, 64);
            bool tailCorrect = true;
            for (int channel = 0; channel < 2; ++channel)
                for (int frame = 0; frame < output.getNumSamples(); ++frame)
                {
                    const auto sourceFrame = 27 + frame * speed;
                    const auto expected = sourceFrame < shortLength ? shortSample (channel, sourceFrame) : 0.0f;
                    tailCorrect = tailCorrect && near (output.getSample (channel, frame), expected, 2.0e-5);
                }
            const auto label = juce::String (speed) + "x";
            expect (tailCorrect, label + " emits valid final source frames then silence at EOF");
            const auto snapshot = processor->getPlaybackSnapshot();
            expect (! snapshot.playing && near (snapshot.positionSeconds, 0.0), label + " stops and resets position at EOF");
            expect (silent (render (*processor, 64)), label + " stays silent after EOF");
        }

        // At 8 kHz / 8x the source advances 48 frames per host sample, crossing
        // more than the entire 31-frame fixture. At 96 kHz interpolation also
        // crosses the loop seam at fractional source positions.
        for (const auto rate : { 8000.0, 48000.0, 96000.0 })
            for (const auto speed : { 1, 2, 4, 8 })
            {
                configure (*processor, rate, 2);
                processor->setLooping (true);
                processor->setPlaybackSpeed (speed);
                processor->seekToSeconds (27.0 / sourceRate);
                processor->play();
                const auto output = render (*processor, 64);
                const auto increment = sourceRate * speed / rate;
                bool samplesCorrect = true;
                for (int channel = 0; channel < 2; ++channel)
                    for (int frame = 0; frame < output.getNumSamples(); ++frame)
                    {
                        const auto sourceFrame = std::fmod (27.0 + frame * increment, static_cast<double> (shortLength));
                        const auto first = static_cast<int> (sourceFrame);
                        const auto fraction = sourceFrame - first;
                        const auto a = shortSample (channel, first);
                        const auto b = shortSample (channel, (first + 1) % shortLength);
                        const auto expected = a + fraction * (b - a);
                        samplesCorrect = samplesCorrect && near (output.getSample (channel, frame), expected, 2.0e-5);
                    }
                const auto label = juce::String (rate, 0) + " Hz / " + juce::String (speed) + "x";
                expect (samplesCorrect, label + " wraps short-loop samples and interpolates across the seam");
                const auto snapshot = processor->getPlaybackSnapshot();
                expect (snapshot.playing, label + " remains playing across short-loop EOF");
                expect (near (snapshot.positionSeconds,
                              std::fmod (27.0 + output.getNumSamples() * increment,
                                         static_cast<double> (shortLength)) / sourceRate),
                        label + " wraps position after every source-length crossing");
            }
        processor->setPlaybackSpeed (1);
    }

    void testMarkerAudio()
    {
        resetMarkerTestState();
        // Interior samples make a read at B or a wrap to file origin audible.
        // The five-sample range also forces 8x / 8 kHz to cross many loops per
        // host sample, while 96 kHz visits the half-sample interpolation seam.
        constexpr int firstMarker = 5, secondMarker = 10, rangeLength = secondMarker - firstMarker;
        struct Case { double rate; int speed; };
        for (const auto testCase : { Case { 8000.0, 8 }, Case { 44100.0, 8 }, Case { 96000.0, 1 } })
            for (const auto looping : { false, true })
            {
                configure (*processor, testCase.rate, 2);
                setMarkers (firstMarker / sourceRate, secondMarker / sourceRate);
                processor->setLooping (looping);
                processor->setPlaybackSpeed (testCase.speed);
                processor->seekToSeconds ((secondMarker - 1) / sourceRate);
                processor->play();
                const auto output = render (*processor, 64);
                const auto increment = sourceRate * testCase.speed / testCase.rate;
                bool samplesCorrect = true;
                for (int channel = 0; channel < 2; ++channel)
                    for (int frame = 0; frame < output.getNumSamples(); ++frame)
                    {
                        auto sourceFrame = secondMarker - 1.0 + frame * increment;
                        if (looping)
                            sourceFrame = firstMarker + std::fmod (sourceFrame - firstMarker, static_cast<double> (rangeLength));
                        double expected = 0.0;
                        if (sourceFrame < secondMarker)
                        {
                            const auto first = static_cast<int> (sourceFrame);
                            const auto second = first + 1 < secondMarker ? first + 1 : looping ? firstMarker : first;
                            const auto fraction = sourceFrame - first;
                            const auto a = shortSample (channel, first);
                            expected = a + fraction * (shortSample (channel, second) - a);
                        }
                        samplesCorrect = samplesCorrect && near (output.getSample (channel, frame), expected, 2.0e-5);
                    }
                const auto label = "A/B " + juce::String (testCase.rate, 0) + " Hz / " + juce::String (testCase.speed)
                    + "x / Loop " + (looping ? "On" : "Off");
                expect (samplesCorrect, label + " renders only legal range samples and interpolates at B correctly");
                const auto snapshot = processor->getPlaybackSnapshot();
                const auto expectedPosition = looping
                    ? firstMarker + std::fmod (rangeLength - 1.0 + output.getNumSamples() * increment, static_cast<double> (rangeLength))
                    : static_cast<double> (firstMarker);
                expect (snapshot.playing == looping && near (snapshot.positionSeconds, expectedPosition / sourceRate),
                        label + " preserves multi-loop overshoot or stops at A");
                if (! looping)
                    expect (silent (render (*processor, 16)), label + " stays silent after reaching B");
            }

        configure (*processor, 44100.0, 2);
        setMarkers (15.0 / sourceRate, 16.0 / sourceRate);
        processor->setPlaybackSpeed (8);
        processor->setLooping (true);
        processor->play();
        const auto oneSampleLoop = render (*processor, 16);
        bool oneSampleCorrect = true;
        for (int channel = 0; channel < 2; ++channel)
            for (int frame = 0; frame < oneSampleLoop.getNumSamples(); ++frame)
                oneSampleCorrect = oneSampleCorrect
                    && near (oneSampleLoop.getSample (channel, frame), shortSample (channel, 15), 2.0e-5);
        expect (oneSampleCorrect && processor->getPlaybackSnapshot().playing,
                "one-source-sample A/B range loops safely at high speed and a different host rate");
        resetMarkerTestState();
    }

    void testFloatSamples()
    {
        processor->setGainDecibels (6.0f);
        processor->setMuted (false);
        processor->setLooping (false);
        for (const auto channels : { 2, 1 })
        {
            configure (*processor, 96000.0, channels);
            processor->seekToSeconds (0.0);
            processor->play();
            const auto output = render (*processor, 16);
            bool finite = true;
            for (int channel = 0; channel < channels; ++channel)
                for (int frame = 0; frame < output.getNumSamples(); ++frame)
                    finite = finite && std::isfinite (output.getSample (channel, frame));
            const auto label = juce::String (channels) + " output channel(s)";
            expect (finite, "extreme finite float WAV renders finite samples at +6 dB / " + label);
            expect (output.getSample (0, 4) == 0.0f && output.getSample (0, 6) == 0.0f,
                    "NaN and infinities in float WAV are sanitized to zero / " + label);
            expect (near (output.getSample (0, 8), 4.0 * juce::Decibels::decibelsToGain (6.0f), 2.0e-5),
                    "ordinary float WAV samples retain headroom above 1.0 / " + label);
        }
        processor->setGainDecibels (0.0f);
    }

    void startStress()
    {
        configure (*processor, sourceRate, 2);
        processor->setMuted (false);
        processor->setGainDecibels (0.0f);
        processor->setLooping (true);
        processor->play();
        stressRunning.store (true);
        stressStartedAt = nextStressEditAt = juce::Time::getMillisecondCounterHiRes();
        stressThread = std::thread ([this]
        {
            // Allocate the host buffers once, before observing this thread.
            juce::AudioBuffer<float> output (2, 256);
            juce::MidiBuffer midi;
            while (stressRunning.load())
            {
                callbackHeap::observing = true;
                processor->processBlock (output, midi);
                callbackHeap::observing = false;
                ++stressCallbacks;
                for (int channel = 0; channel < 2; ++channel)
                    for (int sample = 0; sample < 256; ++sample)
                        stressOutputFinite = stressOutputFinite && std::isfinite (output.getSample (channel, sample));
                std::this_thread::sleep_for (std::chrono::microseconds (500));
            }
            stressAllocations = callbackHeap::allocations;
            stressDeallocations = callbackHeap::deallocations;
        });
    }

    void stopStress()
    {
        stressRunning.store (false);
        if (stressThread.joinable()) stressThread.join();
    }

    bool runStressEdits()
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        if (now - stressStartedAt >= 1000.0)
        {
            stopStress();
            processor->setPlaybackSpeed (1);
            callbacks += static_cast<int> (stressCallbacks);
            callbackHeap::allocations += stressAllocations;
            callbackHeap::deallocations += stressDeallocations;
            expect (stressCallbacks >= 100 && stressEdits >= 30, "concurrent test exercises callback and loader overlap");
            expect (stressOutputFinite, "concurrent file replacement produces finite audio samples");
            std::cout << "Concurrent load/playback: " << stressCallbacks << " callbacks, " << stressEdits
                      << " file requests; " << stressAllocations << " allocations, "
                      << stressDeallocations << " deallocations in the audio thread.\n";
            return true;
        }
        if (now >= nextStressEditAt)
        {
            ++stressEdits;
            lastStressFile = stressEdits % 2 == 0 ? stereoFile : shortFile;
            processor->loadAudioFile (lastStressFile);
            processor->setLooping (stressEdits % 3 != 0);
            processor->setPlaybackSpeed (Processor::playbackSpeeds[static_cast<std::size_t> (stressEdits)
                % Processor::playbackSpeeds.size()]);
            processor->seekToSeconds (0.002 * (stressEdits % 10));
            processor->play();
            if (stressEdits % 7 == 0) processor->pause();
            nextStressEditAt = now + 10.0;
        }
        return false;
    }

    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        if (now - startedAt > 60000.0 || now - stageStartedAt > 15000.0)
        {
            expect (false, "timeout waiting for " + (steps.empty() ? juce::String ("completion") : steps.front().name));
            finish();
            return;
        }
        if (steps.empty()) { finish(); return; }
        if (steps.front().run())
        {
            steps.pop_front();
            stageStartedAt = now;
        }
    }

    void finish()
    {
        if (finished) return;
        finished = true;
        stopTimer();
        stopStress();
        expect (callbackHeap::allocations == 0, "audio callbacks perform no C++ heap allocation");
        expect (callbackHeap::deallocations == 0, "audio callbacks perform no C++ heap deallocation");
        std::cout << "Playback tests: " << checks << " checks, " << failures << " failures; "
                  << callbacks << " audio callbacks, " << callbackHeap::allocations
                  << " allocations, " << callbackHeap::deallocations << " deallocations.\n";
        setApplicationReturnValue (failures == 0 ? 0 : 1);
        quit();
    }
};
}

START_JUCE_APPLICATION (PlaybackTestApplication)
JUCE_MAIN_FUNCTION_DEFINITION
