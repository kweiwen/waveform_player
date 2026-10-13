#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class WaveformPlayerAudioProcessor : public juce::AudioProcessor
{
public:
    struct PlaybackSnapshot
    {
        bool loaded = false;
        bool loading = false;
        bool choosingFile = false;
        bool playing = false;
        double positionSeconds = 0.0;
        double durationSeconds = 0.0;
        double fileSampleRate = 0.0;
        int playbackSpeed = 1;
        std::uint64_t fileRevision = 0;
        juce::String fileName;
        juce::String status;
        juce::String error;
        juce::File file;
    };

    WaveformPlayerAudioProcessor();
    ~WaveformPlayerAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;
    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // File selection is message-thread only. Decoding runs on the loader thread.
    void chooseAudioFile (bool useNativeDialog = true);
    void loadAudioFile (const juce::File& file);
    void play();
    void pause();
    void stop();
    void seekToSeconds (double seconds);
    void skipSeconds (double seconds);
    void setLooping (bool shouldLoop);
    bool isLooping() const noexcept;
    void setGainDecibels (float decibels);
    float getGainDecibels() const noexcept;
    void setMuted (bool shouldMute);
    bool isMuted() const noexcept;
    static constexpr std::array<int, 4> playbackSpeeds { 1, 2, 4, 8 };
    void setPlaybackSpeed (int multiplier);
    int getPlaybackSpeed() const noexcept;
    void cyclePlaybackSpeed();

    // Snapshot strings/files are copied under a non-audio-thread metadata lock.
    PlaybackSnapshot getPlaybackSnapshot() const;
    juce::File getLoadedFile() const;
    juce::AudioProcessorValueTreeState& getParameterState() noexcept { return parameters; }

private:
    struct DecodedAudio
    {
        juce::AudioBuffer<float> samples;
        double sampleRate = 0.0;
        std::uint64_t revision = 0;
    };

    struct LoadRequest
    {
        juce::File file;
        double initialPositionSeconds = 0.0;
        std::uint64_t revision = 0;
        bool clear = false;
    };

    struct ControlSnapshot
    {
        bool shouldPlay = false;
        double seekSeconds = 0.0;
        std::uint64_t transportRevision = 0;
        std::uint64_t seekRevision = 0;
    };

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void requestAudioFile (const juce::File&, double initialPositionSeconds, bool clear = false);
    void loaderLoop();
    std::unique_ptr<DecodedAudio> decodeFile (const LoadRequest&, juce::AudioFormatManager&,
                                             juce::String& error);
    void reclaimRetiredAudio();
    void publishAudio (std::unique_ptr<DecodedAudio>, const LoadRequest&);
    void setTransportCommand (bool shouldPlay, bool resetPosition);
    void setSeekCommand (double seconds);
    bool readControls (ControlSnapshot&) const noexcept;
    const DecodedAudio* protectPublishedAudio() noexcept;
    void publishPlaybackState() noexcept;

    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float>* gainParameter = nullptr;
    std::atomic<float>* loopParameter = nullptr;
    std::atomic<float>* muteParameter = nullptr;
    std::atomic<float>* speedParameter = nullptr;
    std::unique_ptr<juce::FileChooser> fileChooser;
    std::atomic<bool> choosingFile { false };

    mutable std::mutex metadataMutex;
    PlaybackSnapshot metadata;
    mutable std::mutex controlMutex;
    // Writers serialize off the audio thread; the audio thread takes a bounded,
    // lock-free snapshot. Separate revisions let a seek preserve play/pause.
    std::atomic<std::uint64_t> controlEpoch { 0 };
    std::atomic<std::uint64_t> transportCommandRevision { 0 };
    std::atomic<std::uint64_t> seekCommandRevision { 0 };
    std::atomic<bool> requestedPlaying { false };
    std::atomic<double> requestedSeekSeconds { 0.0 };

    std::mutex requestMutex;
    std::condition_variable requestCondition;
    LoadRequest pendingRequest;
    bool requestPending = false;
    std::atomic<bool> shuttingDown { false };
    std::atomic<std::uint64_t> latestRequestRevision { 0 };
    std::thread loaderThread;

    // The loader owns every asset and reclaims retired assets off the audio
    // thread. The hazard pointer protects exactly the asset used by a callback.
    std::unique_ptr<DecodedAudio> currentOwnedAudio;
    std::vector<std::unique_ptr<DecodedAudio>> retiredAudio;
    std::atomic<const DecodedAudio*> publishedAudio { nullptr };
    std::atomic<const DecodedAudio*> audioHazard { nullptr };

    // Audio-thread-only playback state, except prepare/release while suspended.
    double outputSampleRate = 44100.0;
    double sourcePosition = 0.0;
    bool audioPlaying = false;
    std::uint64_t audioAssetRevision = 0;
    std::uint64_t audioTransportRevision = 0;
    std::uint64_t audioSeekRevision = 0;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> outputGain;

    std::atomic<double> reportedPositionSeconds { 0.0 };
    std::atomic<bool> reportedPlaying { false };
    std::atomic<std::uint64_t> reportedAssetRevision { 0 };
    std::atomic<std::uint64_t> appliedTransportRevision { 0 };
    std::atomic<std::uint64_t> appliedSeekRevision { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformPlayerAudioProcessor)
};
