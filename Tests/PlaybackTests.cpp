#define JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP 1
#include "../Source/PluginProcessor.h"

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
        fixtureDirectory = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getNonexistentChildFile ("WaveformPlayer-playback-tests", {}, false);
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
        if (failures != 0) { finish(); return; }

        processor = std::make_unique<Processor>();
        configure (*processor, 48000.0, 2);
        addAction ("empty processor", [this]
        {
            processor->play();
            const auto output = render (*processor, 128);
            expect (! processor->getPlaybackSnapshot().loaded, "empty processor reports no loaded audio");
            expect (! processor->getPlaybackSnapshot().playing, "Play without an audio file stays stopped");
            expect (silent (output), "empty processor clears host output");
        });
        addScreenshot ("WaveformPlayer-empty.png", 0, 0);
        addLoad (stereoFile);
        addAction ("WAV metadata and transport", [this] { testTransport(); });
        addScreenshot ("WaveformPlayer-ui.png", 0, 0);
        addScreenshot ("WaveformPlayer-minimum.png", 700, 400);
        addAction ("host rates and channel layouts", [this] { testRatesAndLayouts(); });
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
            expect (silent (render (*restored, 128)), "restored paused output is silent");
            juce::MemoryBlock malformed;
            malformed.append ("broken-state", 12);
            restored->setStateInformation (malformed.getData(), static_cast<int> (malformed.getSize()));
            expect (restored->getPlaybackSnapshot().file == stereoFile, "malformed state does not discard loaded audio");
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
        startedAt = juce::Time::getMillisecondCounterHiRes();
        stageStartedAt = startedAt;
        startTimer (5);
    }

    void shutdown() override
    {
        stopTimer();
        stopStress();
        editor.reset();
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
    Processor::PlaybackSnapshot beforeInvalid;
    juce::File fixtureDirectory, stereoFile, monoFile, shortFile, flacFile, mp3File, floatFile, invalidFile;
    int checks = 0, failures = 0, callbacks = 0;
    double startedAt = 0.0, stageStartedAt = 0.0, screenshotReadyAt = 0.0;
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

    void addScreenshot (const juce::String& name, int width, int height)
    {
        addAction ("create editor for " + name, [this, width, height]
        {
            editor.reset (processor->createEditor());
            expect (editor != nullptr, "create plugin editor");
            if (editor != nullptr && width > 0 && height > 0) editor->setSize (width, height);
            // Paint once to begin the normal startup animation, then capture
            // the settled interface after the JUCE badge has naturally faded.
            if (editor != nullptr)
                editor->createComponentSnapshot (editor->getLocalBounds());
            screenshotReadyAt = juce::Time::getMillisecondCounterHiRes() + 4500.0;
        });
        addWait ("editor timer refresh", [this]
        { return juce::Time::getMillisecondCounterHiRes() >= screenshotReadyAt; });
        addAction ("capture " + name, [this, name]
        {
            const auto directory = juce::SystemStats::getEnvironmentVariable ("WAVEFORM_PLAYER_TEST_RESULTS", {});
            if (editor != nullptr && directory.isNotEmpty())
            {
                juce::File outputDirectory (directory);
                outputDirectory.createDirectory();
                const auto screenshotFile = outputDirectory.getChildFile (name);
                screenshotFile.deleteFile();
                auto stream = screenshotFile.createOutputStream();
                juce::PNGImageFormat png;
                expect (stream != nullptr && png.writeImageToStream (
                    editor->createComponentSnapshot (editor->getLocalBounds()), *stream), "write editor snapshot " + name);
            }
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
            {
                const auto label = juce::String (rate, 0) + " Hz / " + juce::String (channels) + " channel(s)";
                configure (*processor, rate, channels);
                processor->setGainDecibels (0.0f);
                processor->setMuted (false);
                processor->setLooping (false);
                processor->seekToSeconds (1.0);
                processor->play();
                const auto output = render (*processor, 257);
                expect (processor->getPlaybackSnapshot().playing, label + " plays independently of DAW transport");
                expect (near (processor->getPlaybackSnapshot().positionSeconds, 1.0 + 257.0 / rate, 1.0 / sourceRate), label + " plays at fixed 1.0x speed");
                expect (output.getMagnitude (0, 0, 257) > 0.001f, label + " produces audio");
                if (channels == 2)
                {
                    expect (near (output.getSample (0, 0), stereoSample (0, 48000), 2.0e-5), label + " preserves left channel");
                    expect (near (output.getSample (1, 0), stereoSample (1, 48000), 2.0e-5), label + " preserves right channel");
                }
            }
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
