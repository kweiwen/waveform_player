#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "VideoAudioDecoder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <limits>

namespace
{
std::shared_ptr<const std::vector<WaveformPlayerAudioProcessor::WaveformPeak>>
makeVideoWaveform (const juce::AudioBuffer<float>& samples)
{
    using Peak = WaveformPlayerAudioProcessor::WaveformPeak;
    const auto length = samples.getNumSamples();
    const auto count = juce::jmin (1024, length);
    auto peaks = std::make_shared<std::vector<Peak>>();
    peaks->reserve (static_cast<std::size_t> (count));
    const auto* channel = samples.getReadPointer (0);
    for (int index = 0; index < count; ++index)
    {
        const auto first = static_cast<int> (static_cast<juce::int64> (index) * length / count);
        const auto end = static_cast<int> (static_cast<juce::int64> (index + 1) * length / count);
        auto low = channel[first], high = channel[first];
        for (int sample = first + 1; sample < end; ++sample)
        {
            low = juce::jmin (low, channel[sample]);
            high = juce::jmax (high, channel[sample]);
        }
        peaks->push_back ({ low, high });
    }
    return peaks;
}
}

juce::AudioProcessorValueTreeState::ParameterLayout
WaveformPlayerAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "gainDb", 1 }, "Volume",
        juce::NormalisableRange<float> { -60.0f, 6.0f, 0.1f }, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "loop", 1 }, "Loop", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "mute", 1 }, "Mute", false));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "speed", 1 }, "Playback Speed",
        juce::StringArray { "1x", "2x", "4x", "8x" }, 0));
    return layout;
}

WaveformPlayerAudioProcessor::WaveformPlayerAudioProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "WaveformPlayerState", createParameterLayout())
{
    static_assert (std::atomic<double>::is_always_lock_free, "Playback requires lock-free double atomics");
    static_assert (std::atomic<std::uint64_t>::is_always_lock_free, "Playback requires lock-free revisions");
    static_assert (std::atomic<const DecodedAudio*>::is_always_lock_free, "Playback requires lock-free pointers");
    gainParameter = parameters.getRawParameterValue ("gainDb");
    loopParameter = parameters.getRawParameterValue ("loop");
    muteParameter = parameters.getRawParameterValue ("mute");
    speedParameter = parameters.getRawParameterValue ("speed");
    metadata.status = "Open a media file";
    outputGain.setCurrentAndTargetValue (1.0f);
    loaderThread = std::thread ([this] { loaderLoop(); });
}

WaveformPlayerAudioProcessor::~WaveformPlayerAudioProcessor()
{
    fileChooser.reset();
    shuttingDown.store (true);
    requestCondition.notify_one();
    if (loaderThread.joinable())
        loaderThread.join();
    // JUCE destroys a processor after its audio callback has been detached.
    publishedAudio.store (nullptr);
    retiredAudio.clear();
    currentOwnedAudio.reset();
}

const juce::String WaveformPlayerAudioProcessor::getName() const { return JucePlugin_Name; }
bool WaveformPlayerAudioProcessor::acceptsMidi() const { return JucePlugin_WantsMidiInput != 0; }
bool WaveformPlayerAudioProcessor::producesMidi() const { return JucePlugin_ProducesMidiOutput != 0; }
bool WaveformPlayerAudioProcessor::isMidiEffect() const { return JucePlugin_IsMidiEffect != 0; }
double WaveformPlayerAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int WaveformPlayerAudioProcessor::getNumPrograms() { return 1; }
int WaveformPlayerAudioProcessor::getCurrentProgram() { return 0; }
void WaveformPlayerAudioProcessor::setCurrentProgram (int index) { juce::ignoreUnused (index); }
const juce::String WaveformPlayerAudioProcessor::getProgramName (int index) { juce::ignoreUnused (index); return {}; }
void WaveformPlayerAudioProcessor::changeProgramName (int index, const juce::String& name) { juce::ignoreUnused (index, name); }
bool WaveformPlayerAudioProcessor::hasEditor() const { return true; }
juce::AudioProcessorEditor* WaveformPlayerAudioProcessor::createEditor() { return new WaveformPlayerAudioProcessorEditor (*this); }

void WaveformPlayerAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    outputSampleRate = std::isfinite (sampleRate) && sampleRate > 0.0 ? sampleRate : 44100.0;
    outputGain.reset (outputSampleRate, 0.01);
    outputGain.setCurrentAndTargetValue (isMuted() ? 0.0f : juce::Decibels::decibelsToGain (getGainDecibels()));
}

void WaveformPlayerAudioProcessor::releaseResources()
{
    audioHazard.store (nullptr);
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool WaveformPlayerAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet().isDisabled()
        && (layouts.getMainOutputChannelSet() == juce::AudioChannelSet::mono()
         || layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo());
}
#endif

bool WaveformPlayerAudioProcessor::readControls (ControlSnapshot& result) const noexcept
{
    // Never spin on a writer in the real-time callback. A concurrent edit may
    // defer command application by one block.
    for (int attempt = 0; attempt != 2; ++attempt)
    {
        const auto before = controlEpoch.load();
        if ((before & 1u) != 0)
            continue;
        ControlSnapshot next;
        next.shouldPlay = requestedPlaying.load();
        next.seekSeconds = requestedSeekSeconds.load();
        next.markerSamples = {{ requestedMarkerA.load(), requestedMarkerB.load() }};
        next.transportRevision = transportCommandRevision.load();
        next.seekRevision = seekCommandRevision.load();
        if (before == controlEpoch.load())
        {
            result = next;
            return true;
        }
    }
    return false;
}

std::pair<juce::int64, juce::int64> WaveformPlayerAudioProcessor::playbackBounds (
    const std::array<juce::int64, 2>& markers, juce::int64 length) noexcept
{
    if (markers[1] >= 0)
    {
        // During file replacement a callback may still hold the previous asset.
        const auto start = juce::jlimit<juce::int64> (0, length, markers[0]);
        const auto end = juce::jlimit (start, length, markers[1]);
        if (start < end)
            return { start, end };
    }
    return { 0, length };
}

const WaveformPlayerAudioProcessor::DecodedAudio*
WaveformPlayerAudioProcessor::protectPublishedAudio() noexcept
{
    // Sequential consistency orders publication and hazard protection against
    // loader reclamation. No asset is dereferenced until publication is checked.
    for (int attempt = 0; attempt != 2; ++attempt)
    {
        const auto* asset = publishedAudio.load();
        audioHazard.store (asset);
        if (asset == publishedAudio.load())
            return asset;
    }
    audioHazard.store (nullptr);
    return nullptr;
}

void WaveformPlayerAudioProcessor::publishPlaybackState() noexcept
{
    reportedPlaying.store (audioPlaying, std::memory_order_relaxed);
    reportedAssetRevision.store (audioAssetRevision, std::memory_order_relaxed);
    appliedTransportRevision.store (audioTransportRevision, std::memory_order_relaxed);
    appliedSeekRevision.store (audioSeekRevision, std::memory_order_relaxed);
}

void WaveformPlayerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused (midi);
    buffer.clear();
    const auto* asset = protectPublishedAudio();
    if (asset == nullptr)
    {
        audioHazard.store (nullptr);
        return;
    }

    if (asset->revision != audioAssetRevision)
    {
        audioAssetRevision = asset->revision;
        sourcePosition = 0.0;
        audioPlaying = false;
        audioTransportRevision = 0;
        audioSeekRevision = 0;
        audioMarkers = {{ -1, -1 }};
        outputSamplesSinceJump = 0;
    }

    const auto sourceLength = asset->samples.getNumSamples();
    ControlSnapshot commands;
    if (readControls (commands))
    {
        audioMarkers = commands.markerSamples;
        const auto bounds = playbackBounds (audioMarkers, sourceLength);
        if (commands.seekRevision != audioSeekRevision)
        {
            const auto seekSample = std::llround (juce::jlimit (0.0, sourceLength / asset->sampleRate,
                                                              commands.seekSeconds) * asset->sampleRate);
            sourcePosition = static_cast<double> (juce::jlimit (bounds.first, bounds.second, seekSample));
            audioSeekRevision = commands.seekRevision;
            outputSamplesSinceJump = 0;
        }
        if (commands.transportRevision != audioTransportRevision)
        {
            if (commands.shouldPlay && ! audioPlaying)
                outputSamplesSinceJump = 0;
            audioPlaying = commands.shouldPlay;
            if (audioPlaying && (sourcePosition < bounds.first || sourcePosition >= bounds.second))
                sourcePosition = static_cast<double> (bounds.first);
            audioTransportRevision = commands.transportRevision;
        }
    }

    const auto bounds = playbackBounds (audioMarkers, sourceLength);
    const auto start = static_cast<double> (bounds.first);
    const auto end = static_cast<double> (bounds.second);
    const auto rangeLength = end - start;
    const bool muted = isMuted();
    const bool looping = isLooping();
    outputGain.setTargetValue (muted ? 0.0f : juce::Decibels::decibelsToGain (getGainDecibels()));
    const auto increment = asset->sampleRate / outputSampleRate * getPlaybackSpeed();
    const auto* left = asset->samples.getReadPointer (0);
    const auto* right = asset->samples.getReadPointer (asset->samples.getNumChannels() > 1 ? 1 : 0);
    const auto outputChannels = juce::jmin (2, buffer.getNumChannels());
    auto* outLeft = outputChannels > 0 ? buffer.getWritePointer (0) : nullptr;
    auto* outRight = outputChannels > 1 ? buffer.getWritePointer (1) : nullptr;
    const auto toFiniteFloat = [] (double value) noexcept
    {
        constexpr auto limit = static_cast<double> (std::numeric_limits<float>::max());
        return static_cast<float> (juce::jlimit (-limit, limit, value));
    };

    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        const auto smoothedGain = outputGain.getNextValue();
        if (! audioPlaying)
            continue;
        if (sourcePosition >= end)
        {
            outputSamplesSinceJump = 0;
            if (looping)
                sourcePosition = start + std::fmod (sourcePosition - start, rangeLength);
            else
            {
                audioPlaying = false;
                sourcePosition = start;
                continue;
            }
        }

        const auto first = static_cast<int> (sourcePosition);
        const auto second = first + 1 < bounds.second ? first + 1
                                                    : (looping ? static_cast<int> (bounds.first) : first);
        // Float WAV samples may use the full finite float range. Double
        // intermediates prevent interpolation, gain and stereo summing from
        // overflowing before the final conversion, while preserving headroom.
        const auto fraction = sourcePosition - first;
        const auto gain = muted ? 0.0 : static_cast<double> (smoothedGain);
        const auto firstLeft = static_cast<double> (left[first]);
        const auto firstRight = static_cast<double> (right[first]);
        const auto leftSample = (firstLeft + fraction * (static_cast<double> (left[second]) - firstLeft)) * gain;
        const auto rightSample = (firstRight + fraction * (static_cast<double> (right[second]) - firstRight)) * gain;
        if (outRight != nullptr)
        {
            outLeft[sample] = toFiniteFloat (leftSample);
            outRight[sample] = toFiniteFloat (rightSample);
        }
        else if (outLeft != nullptr)
        {
            outLeft[sample] = toFiniteFloat (0.5 * (leftSample + rightSample));
        }
        sourcePosition += increment;
        ++outputSamplesSinceJump;
        if (sourcePosition >= end)
        {
            outputSamplesSinceJump = 0;
            if (looping)
                sourcePosition = start + std::fmod (sourcePosition - start, rangeLength);
            else
            {
                audioPlaying = false;
                sourcePosition = start;
            }
        }
    }

    reportedPositionSeconds.store (sourcePosition / asset->sampleRate, std::memory_order_relaxed);

    // A rendered sample is heard roughly one buffer after the callback that
    // delivers it, plus device latency. The callback runs about one buffer
    // ahead, so the speaker trails the block end by two buffers and the device
    // latency. Audio rendered before the last jump (seek, loop or start) never
    // reaches the picture: the picture holds at the jump until it is audible.
    const auto latency = 2 * static_cast<juce::int64> (buffer.getNumSamples())
                       + juce::jmax (0, outputDeviceLatencySamples.load (std::memory_order_relaxed));
    const auto lag = audioPlaying ? juce::jmin (latency, outputSamplesSinceJump) : juce::int64 { 0 };
    const auto epoch = audibleEpoch.load (std::memory_order_relaxed);
    audibleEpoch.store (epoch + 1, std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_release);
    audibleSeconds.store ((sourcePosition - static_cast<double> (lag) * increment) / asset->sampleRate,
                          std::memory_order_relaxed);
    audibleLimitSeconds.store (sourcePosition / asset->sampleRate, std::memory_order_relaxed);
    audibleSpeed.store (audioPlaying ? increment * outputSampleRate / asset->sampleRate : 0.0,
                        std::memory_order_relaxed);
    audibleCallbackMs.store (juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
    audibleHoldMs.store (audioPlaying ? static_cast<double> (latency - lag) * 1000.0 / outputSampleRate : 0.0,
                         std::memory_order_relaxed);
    audibleEpoch.store (epoch + 2, std::memory_order_release);
    publishPlaybackState();
    audioHazard.store (nullptr);
}

void WaveformPlayerAudioProcessor::setTransportCommand (bool shouldPlay, bool resetPosition)
{
    const auto start = resetPosition ? getPlaybackSnapshot().rangeStartSeconds : 0.0;
    const std::lock_guard<std::mutex> lock (controlMutex);
    controlEpoch.fetch_add (1);
    requestedPlaying.store (shouldPlay);
    transportCommandRevision.fetch_add (1);
    if (resetPosition)
    {
        requestedSeekSeconds.store (start);
        seekCommandRevision.fetch_add (1);
    }
    controlEpoch.fetch_add (1);
}

void WaveformPlayerAudioProcessor::setSeekCommand (double seconds)
{
    const std::lock_guard<std::mutex> lock (controlMutex);
    controlEpoch.fetch_add (1);
    requestedSeekSeconds.store (seconds);
    seekCommandRevision.fetch_add (1);
    controlEpoch.fetch_add (1);
}

void WaveformPlayerAudioProcessor::play() { setTransportCommand (true, false); }
void WaveformPlayerAudioProcessor::pause() { setTransportCommand (false, false); }
void WaveformPlayerAudioProcessor::stop() { setTransportCommand (false, true); }

void WaveformPlayerAudioProcessor::seekToSeconds (double seconds)
{
    if (! std::isfinite (seconds))
        return;
    const auto snapshot = getPlaybackSnapshot();
    if (! snapshot.loaded)
        return;
    const auto clamped = juce::jlimit (snapshot.rangeStartSeconds, snapshot.rangeEndSeconds, seconds);
    const auto sourceSample = std::llround (clamped * snapshot.fileSampleRate);
    setSeekCommand (sourceSample / snapshot.fileSampleRate);
}

void WaveformPlayerAudioProcessor::skipSeconds (double seconds)
{
    if (std::isfinite (seconds))
        seekToSeconds (getPlaybackSnapshot().positionSeconds + seconds);
}

void WaveformPlayerAudioProcessor::addMarker (double seconds)
{
    const auto snapshot = getPlaybackSnapshot();
    if (! std::isfinite (seconds) || ! snapshot.loaded || snapshot.loading || snapshot.choosingFile)
        return;
    const auto sample = std::llround (juce::jlimit (0.0, snapshot.durationSeconds, seconds) * snapshot.fileSampleRate);
    const std::lock_guard<std::mutex> lock (controlMutex);
    const auto first = requestedMarkerA.load();
    if (first == sample && requestedMarkerB.load() < 0)
        return;

    controlEpoch.fetch_add (1);
    if (first < 0 || requestedMarkerB.load() >= 0)
    {
        requestedMarkerA.store (sample);
        requestedMarkerB.store (-1);
    }
    else
    {
        const auto start = juce::jmin (first, sample);
        requestedMarkerA.store (start);
        requestedMarkerB.store (juce::jmax (first, sample));
        requestedSeekSeconds.store (start / snapshot.fileSampleRate);
        seekCommandRevision.fetch_add (1);
    }
    controlEpoch.fetch_add (1);
}

void WaveformPlayerAudioProcessor::clearMarkers()
{
    const std::lock_guard<std::mutex> lock (controlMutex);
    controlEpoch.fetch_add (1);
    requestedMarkerA.store (-1);
    requestedMarkerB.store (-1);
    controlEpoch.fetch_add (1);
}

void WaveformPlayerAudioProcessor::setLooping (bool shouldLoop)
{
    parameters.getParameter ("loop")->setValueNotifyingHost (shouldLoop ? 1.0f : 0.0f);
}
bool WaveformPlayerAudioProcessor::isLooping() const noexcept { return loopParameter->load (std::memory_order_relaxed) >= 0.5f; }
void WaveformPlayerAudioProcessor::setGainDecibels (float decibels)
{
    if (std::isfinite (decibels))
    {
        auto* parameter = parameters.getParameter ("gainDb");
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (juce::jlimit (-60.0f, 6.0f, decibels)));
    }
}
float WaveformPlayerAudioProcessor::getGainDecibels() const noexcept { return gainParameter->load (std::memory_order_relaxed); }
void WaveformPlayerAudioProcessor::setMuted (bool shouldMute)
{
    parameters.getParameter ("mute")->setValueNotifyingHost (shouldMute ? 1.0f : 0.0f);
}
bool WaveformPlayerAudioProcessor::isMuted() const noexcept { return muteParameter->load (std::memory_order_relaxed) >= 0.5f; }

void WaveformPlayerAudioProcessor::setPlaybackSpeed (int multiplier)
{
    const auto choice = std::find (playbackSpeeds.begin(), playbackSpeeds.end(), multiplier);
    if (choice == playbackSpeeds.end())
        return;
    auto* parameter = parameters.getParameter ("speed");
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (
        static_cast<float> (std::distance (playbackSpeeds.begin(), choice))));
}

void WaveformPlayerAudioProcessor::setOutputDeviceLatencySamples (int samples) noexcept
{
    outputDeviceLatencySamples.store (juce::jmax (0, samples), std::memory_order_relaxed);
}

double WaveformPlayerAudioProcessor::getAudiblePositionSeconds (double fallback) const noexcept
{
    for (int attempt = 0; attempt != 4; ++attempt)
    {
        const auto before = audibleEpoch.load (std::memory_order_acquire);
        if ((before & 1u) != 0)
            continue;
        const auto seconds = audibleSeconds.load (std::memory_order_relaxed);
        const auto limit = audibleLimitSeconds.load (std::memory_order_relaxed);
        const auto speed = audibleSpeed.load (std::memory_order_relaxed);
        const auto callbackMs = audibleCallbackMs.load (std::memory_order_relaxed);
        const auto holdMs = audibleHoldMs.load (std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_acquire);
        if (before != audibleEpoch.load (std::memory_order_relaxed))
            continue;
        // Advance with wall time between callbacks, but never past audio that
        // has not been rendered yet; a stalled host leaves the picture there.
        const auto elapsedMs = juce::jmax (0.0, juce::Time::getMillisecondCounterHiRes() - callbackMs - holdMs);
        return juce::jmin (limit, seconds + speed * elapsedMs / 1000.0);
    }
    return fallback;
}

int WaveformPlayerAudioProcessor::getPlaybackSpeed() const noexcept
{
    const auto value = speedParameter->load (std::memory_order_relaxed);
    const auto index = std::isfinite (value)
        ? juce::roundToInt (juce::jlimit (0.0f, static_cast<float> (playbackSpeeds.size() - 1), value))
        : 0;
    return playbackSpeeds[static_cast<std::size_t> (index)];
}

void WaveformPlayerAudioProcessor::cyclePlaybackSpeed()
{
    const auto current = std::find (playbackSpeeds.begin(), playbackSpeeds.end(), getPlaybackSpeed());
    const auto next = std::next (current);
    setPlaybackSpeed (next == playbackSpeeds.end() ? playbackSpeeds.front() : *next);
}

WaveformPlayerAudioProcessor::PlaybackSnapshot WaveformPlayerAudioProcessor::getPlaybackSnapshot() const
{
    PlaybackSnapshot result;
    {
        const std::lock_guard<std::mutex> lock (metadataMutex);
        result = metadata;
    }
    result.playbackSpeed = getPlaybackSpeed();
    result.choosingFile = choosingFile.load (std::memory_order_relaxed);
    if (! result.loaded)
        return result;

    result.positionSeconds = reportedPositionSeconds.load (std::memory_order_relaxed);
    result.playing = reportedPlaying.load (std::memory_order_relaxed);
    result.renderedPositionSeconds = getAudiblePositionSeconds (result.positionSeconds);
    result.renderedPlaying = result.playing;
    const std::lock_guard<std::mutex> lock (controlMutex);
    result.markerSamples = {{ requestedMarkerA.load(), requestedMarkerB.load() }};
    const auto bounds = playbackBounds (result.markerSamples,
        std::llround (result.durationSeconds * result.fileSampleRate));
    result.rangeStartSeconds = bounds.first / result.fileSampleRate;
    result.rangeEndSeconds = bounds.second / result.fileSampleRate;
    const bool pendingAsset = reportedAssetRevision.load (std::memory_order_relaxed) != result.fileRevision;
    result.renderedAssetReady = ! pendingAsset;
    result.pictureSeekRevision = seekCommandRevision.load();
    result.renderedSeekReady = result.pictureSeekRevision == appliedSeekRevision.load (std::memory_order_relaxed);
    if (pendingAsset || ! result.renderedSeekReady)
        result.positionSeconds = requestedSeekSeconds.load();
    if (pendingAsset || transportCommandRevision.load() != appliedTransportRevision.load (std::memory_order_relaxed))
        result.playing = requestedPlaying.load();
    result.positionSeconds = juce::jlimit (result.rangeStartSeconds, result.rangeEndSeconds, result.positionSeconds);
    if (! result.loading && result.error.isEmpty())
        result.status = result.playing ? "Playing" : (result.positionSeconds != result.rangeStartSeconds ? "Paused" : "Ready");
    return result;
}

juce::File WaveformPlayerAudioProcessor::getLoadedFile() const { return getPlaybackSnapshot().file; }

void WaveformPlayerAudioProcessor::chooseAudioFile (bool useNativeDialog)
{
    if (choosingFile.load (std::memory_order_relaxed) || getPlaybackSnapshot().loading)
        return;

    fileChooser = std::make_unique<juce::FileChooser> (
        "Open audio or video file", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
        "*.wav;*.mp3;*.flac;*.mp4;*.mov", useNativeDialog);
    choosingFile.store (true, std::memory_order_relaxed);
    try
    {
        fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& chooser)
            {
                const auto file = chooser.getResult();
                choosingFile.store (false, std::memory_order_relaxed);
                if (file != juce::File())
                    loadAudioFile (file);
            });
    }
    catch (...)
    {
        choosingFile.store (false, std::memory_order_relaxed);
        fileChooser.reset();
        throw;
    }
}

void WaveformPlayerAudioProcessor::loadAudioFile (const juce::File& file) { requestAudioFile (file, 0.0); }

void WaveformPlayerAudioProcessor::requestAudioFile (const juce::File& file, double initialPositionSeconds,
                                                    bool clear)
{
    {
        const std::lock_guard<std::mutex> requestLock (requestMutex);
        pendingRequest = { file, initialPositionSeconds, latestRequestRevision.fetch_add (1) + 1, clear };
        requestPending = true;
        const std::lock_guard<std::mutex> metadataLock (metadataMutex);
        metadata.loading = true;
        metadata.error.clear();
        metadata.status = clear ? "Clearing media file" : "Loading " + file.getFileName() + "...";
    }
    requestCondition.notify_one();
}

std::unique_ptr<WaveformPlayerAudioProcessor::DecodedAudio>
WaveformPlayerAudioProcessor::decodeFile (const LoadRequest& request, juce::AudioFormatManager& manager,
                                         juce::String& error)
{
    if (! request.file.existsAsFile())
    {
        error = "The media file could not be found.";
        return {};
    }
    const auto isVideo = request.file.hasFileExtension ("mp4") || request.file.hasFileExtension ("mov");
    if (isVideo)
    {
        auto video = decodeVideoAudio (request.file, error, [this, &request]
        {
            return shuttingDown.load() || request.revision != latestRequestRevision.load();
        });
        if (video == nullptr)
            return {};
        auto decoded = std::make_unique<DecodedAudio>();
        decoded->samples = std::move (video->samples);
        decoded->sampleRate = video->sampleRate;
        decoded->revision = request.revision;
        decoded->isVideo = true;
        decoded->videoWaveform = makeVideoWaveform (decoded->samples);
        return decoded;
    }
    auto reader = std::unique_ptr<juce::AudioFormatReader> (manager.createReaderFor (request.file));
    if (reader == nullptr)
    {
        error = "The file is unsupported or could not be decoded. Choose WAV, MP3, FLAC, MP4 or MOV.";
        return {};
    }
    if (reader->numChannels < 1 || reader->numChannels > 2)
    {
        error = "Only mono and stereo audio files are supported.";
        return {};
    }
    if (! std::isfinite (reader->sampleRate) || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
    {
        error = "The audio file has no valid audio samples.";
        return {};
    }
    if (reader->lengthInSamples > std::numeric_limits<int>::max())
    {
        error = "The audio file exceeds the supported number of source samples.";
        return {};
    }
    const auto frames = static_cast<std::uint64_t> (reader->lengthInSamples);
    const auto frameBytes = static_cast<std::uint64_t> (reader->numChannels) * sizeof (float);
    if (frames > (std::numeric_limits<std::size_t>::max() - 128u) / frameBytes)
    {
        error = "The audio file exceeds the supported memory address range.";
        return {};
    }

    auto decoded = std::make_unique<DecodedAudio>();
    decoded->sampleRate = reader->sampleRate;
    decoded->revision = request.revision;
    decoded->samples.setSize (static_cast<int> (reader->numChannels), static_cast<int> (reader->lengthInSamples));
    constexpr int chunkSize = 65536;
    for (int offset = 0; offset < decoded->samples.getNumSamples();)
    {
        reclaimRetiredAudio();
        if (shuttingDown.load() || request.revision != latestRequestRevision.load())
            return {};
        const auto count = juce::jmin (chunkSize, decoded->samples.getNumSamples() - offset);
        if (! reader->read (&decoded->samples, offset, count, offset, true, true))
        {
            error = "The audio file could not be decoded completely.";
            return {};
        }
        // Non-finite values in floating-point WAV files must not reach a host.
        for (int channel = 0; channel < decoded->samples.getNumChannels(); ++channel)
        {
            auto* samples = decoded->samples.getWritePointer (channel, offset);
            for (int sample = 0; sample < count; ++sample)
                if (! std::isfinite (samples[sample]))
                    samples[sample] = 0.0f;
        }
        offset += count;
    }
    return decoded;
}

void WaveformPlayerAudioProcessor::reclaimRetiredAudio()
{
    retiredAudio.erase (std::remove_if (retiredAudio.begin(), retiredAudio.end(), [this] (const auto& asset)
    {
        return audioHazard.load() != asset.get();
    }), retiredAudio.end());
}

void WaveformPlayerAudioProcessor::publishAudio (std::unique_ptr<DecodedAudio> decoded, const LoadRequest& request)
{
    // Serialize publication with a newer request, so a superseded decoder can
    // never replace the file the user selected most recently.
    const std::lock_guard<std::mutex> requestLock (requestMutex);
    if (shuttingDown.load() || request.revision != latestRequestRevision.load())
        return;
    // Complete any potentially allocating bookkeeping before altering the
    // current playback asset, so allocation failure retains the previous file.
    PlaybackSnapshot nextMetadata;
    if (decoded != nullptr)
    {
        const std::lock_guard<std::mutex> metadataLock (metadataMutex);
        nextMetadata = metadata;
        nextMetadata.loaded = true;
        nextMetadata.loading = false;
        nextMetadata.file = request.file;
        nextMetadata.fileName = request.file.getFileName();
        nextMetadata.fileSampleRate = decoded->sampleRate;
        nextMetadata.fileChannels = decoded->samples.getNumChannels();
        nextMetadata.isVideo = decoded->isVideo;
        nextMetadata.videoWaveform = decoded->videoWaveform;
        nextMetadata.durationSeconds = decoded->samples.getNumSamples() / decoded->sampleRate;
    }
    nextMetadata.fileRevision = request.revision;
    nextMetadata.error.clear();
    nextMetadata.status = decoded != nullptr ? "Ready" : "Open a media file";
    if (currentOwnedAudio != nullptr)
        retiredAudio.reserve (retiredAudio.size() + 1);
    {
        const std::lock_guard<std::mutex> controlLock (controlMutex);
        controlEpoch.fetch_add (1);
        requestedPlaying.store (false);
        requestedMarkerA.store (-1);
        requestedMarkerB.store (-1);
        requestedSeekSeconds.store (decoded != nullptr
            ? juce::jlimit (0.0, nextMetadata.durationSeconds, request.initialPositionSeconds)
            : 0.0);
        transportCommandRevision.fetch_add (1);
        seekCommandRevision.fetch_add (1);
        controlEpoch.fetch_add (1);
    }
    if (currentOwnedAudio != nullptr)
        retiredAudio.push_back (std::move (currentOwnedAudio));
    currentOwnedAudio = std::move (decoded);
    publishedAudio.store (currentOwnedAudio.get());
    {
        const std::lock_guard<std::mutex> metadataLock (metadataMutex);
        metadata = std::move (nextMetadata);
    }
    reclaimRetiredAudio();
}

void WaveformPlayerAudioProcessor::loaderLoop()
{
    juce::AudioFormatManager manager;
    manager.registerBasicFormats();
    while (! shuttingDown.load())
    {
        reclaimRetiredAudio();
        LoadRequest request;
        {
            std::unique_lock<std::mutex> lock (requestMutex);
            requestCondition.wait_for (lock, std::chrono::milliseconds (50), [this]
            {
                return shuttingDown.load() || requestPending;
            });
            if (shuttingDown.load())
                break;
            if (! requestPending)
                continue;
            request = pendingRequest;
            requestPending = false;
        }

        juce::String error;
        std::unique_ptr<DecodedAudio> decoded;
        try
        {
            if (request.clear)
            {
                publishAudio ({}, request);
                continue;
            }
            decoded = decodeFile (request, manager, error);
            if (decoded != nullptr)
            {
                publishAudio (std::move (decoded), request);
                continue;
            }
        }
        catch (const std::bad_alloc&)
        {
            error = "There is not enough memory to load this media file.";
        }
        catch (...)
        {
            error = "The media file could not be loaded.";
        }
        if (decoded == nullptr)
        {
            const std::lock_guard<std::mutex> requestLock (requestMutex);
            if (request.revision == latestRequestRevision.load() && ! shuttingDown.load())
            {
                const std::lock_guard<std::mutex> metadataLock (metadataMutex);
                metadata.loading = false;
                metadata.error = error.isNotEmpty() ? error : "The media file could not be loaded.";
                metadata.status = metadata.error;
            }
        }
    }
}

void WaveformPlayerAudioProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    auto state = parameters.copyState();
    const auto snapshot = getPlaybackSnapshot();
    state.setProperty ("filePath", snapshot.loaded ? snapshot.file.getFullPathName() : juce::String(), nullptr);
    state.setProperty ("positionSeconds", snapshot.positionSeconds, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destination);
}

void WaveformPlayerAudioProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName (parameters.state.getType()))
        return;
    const auto state = juce::ValueTree::fromXml (*xml);
    const bool hasStoredSpeed = state.getChildWithProperty ("id", "speed").isValid();
    parameters.replaceState (state);
    // Older sessions predate this parameter and must start at normal speed.
    if (! hasStoredSpeed)
        setPlaybackSpeed (playbackSpeeds.front());
    stop();
    const juce::String path = state.getProperty ("filePath");
    const double position = state.getProperty ("positionSeconds", 0.0);
    if (path.isNotEmpty())
        requestAudioFile (juce::File (path), std::isfinite (position) ? juce::jmax (0.0, position) : 0.0);
    else
        requestAudioFile ({}, 0.0, true);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new WaveformPlayerAudioProcessor();
}
