#include "VideoAudioDecoder.h"

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreMedia/CoreMedia.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>

namespace
{
bool isValidTime (CMTime time)
{
    return CMTIME_IS_NUMERIC (time) && std::isfinite (CMTimeGetSeconds (time));
}

bool growBuffer (juce::AudioBuffer<float>& buffer, int channels, int required)
{
    if (required <= buffer.getNumSamples())
        return true;
    const auto capacity = buffer.getNumSamples();
    const auto next = juce::jmax<juce::int64> (required,
        static_cast<juce::int64> (capacity) + juce::jmax (capacity / 2, 4096));
    if (next > std::numeric_limits<int>::max())
        return false;
    buffer.setSize (channels, static_cast<int> (next), true, true, false);
    return true;
}
}

std::unique_ptr<DecodedVideoAudio> decodeVideoAudio (const juce::File& file,
                                                      juce::String& error,
                                                      const std::function<bool()>& shouldCancel)
{
    @autoreleasepool
    {
        const auto path = file.getFullPathName();
        auto* url = [NSURL fileURLWithPath: [NSString stringWithUTF8String: path.toRawUTF8()]];
        auto* asset = [AVURLAsset URLAssetWithURL: url
                                         options: @{ AVURLAssetPreferPreciseDurationAndTimingKey: @YES }];
        if (asset == nil || [asset tracksWithMediaType: AVMediaTypeVideo].count == 0)
        {
            error = "The file has no supported video track.";
            return {};
        }
        auto* audioTrack = [[asset tracksWithMediaType: AVMediaTypeAudio] firstObject];
        if (audioTrack == nil)
        {
            error = "The video file must contain an audio track.";
            return {};
        }
        const auto duration = [asset duration];
        if (! isValidTime (duration) || CMTimeGetSeconds (duration) <= 0.0)
        {
            error = "The video file has no valid duration.";
            return {};
        }

        NSError* nsError = nil;
        auto* reader = [AVAssetReader assetReaderWithAsset: asset error: &nsError];
        if (reader == nil)
        {
            error = "The video audio track could not be opened.";
            return {};
        }
        NSDictionary* settings = @{
            AVFormatIDKey: @(kAudioFormatLinearPCM),
            AVLinearPCMIsFloatKey: @YES,
            AVLinearPCMBitDepthKey: @32,
            AVLinearPCMIsNonInterleaved: @YES,
            AVLinearPCMIsBigEndianKey: @NO
        };
        auto* output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack: audioTrack
                                                                  outputSettings: settings];
        [reader addOutput: output];
        if (! [reader startReading])
        {
            error = "The video audio track could not be decoded.";
            return {};
        }

        auto decoded = std::make_unique<DecodedVideoAudio>();
        int channels = 0;
        int written = 0;
        for (;;)
        {
            if (shouldCancel())
            {
                [reader cancelReading];
                return {};
            }
            auto* sample = [output copyNextSampleBuffer];
            if (sample == nullptr)
                break;

            const auto rawCount = CMSampleBufferGetNumSamples (sample);
            if (rawCount == 0)
            {
                CFRelease (sample);
                continue;
            }
            if (rawCount < 0 || rawCount > std::numeric_limits<int>::max())
            {
                CFRelease (sample);
                error = "The video has an unsupported audio sample block.";
                return {};
            }

            const auto* description = CMSampleBufferGetFormatDescription (sample);
            const auto* format = description != nullptr
                ? CMAudioFormatDescriptionGetStreamBasicDescription (description) : nullptr;
            if (format == nullptr || ! std::isfinite (format->mSampleRate)
                || format->mSampleRate <= 0.0 || format->mChannelsPerFrame < 1
                || format->mChannelsPerFrame > 2
                || (channels != 0 && channels != static_cast<int> (format->mChannelsPerFrame)))
            {
                CFRelease (sample);
                error = "Only mono and stereo video audio tracks are supported.";
                return {};
            }
            if (channels == 0)
            {
                channels = static_cast<int> (format->mChannelsPerFrame);
                decoded->sampleRate = format->mSampleRate;
                const auto estimated = std::ceil (CMTimeGetSeconds (duration) * decoded->sampleRate);
                if (! std::isfinite (estimated) || estimated <= 0.0
                    || estimated > std::numeric_limits<int>::max())
                {
                    CFRelease (sample);
                    error = "The video exceeds the supported number of audio samples.";
                    return {};
                }
                decoded->samples.setSize (channels, static_cast<int> (estimated), false, true, false);
            }
            const auto count = static_cast<int> (rawCount);
            const auto time = CMSampleBufferGetPresentationTimeStamp (sample);
            auto start = isValidTime (time)
                ? static_cast<juce::int64> (std::llround (CMTimeGetSeconds (time) * decoded->sampleRate))
                : static_cast<juce::int64> (written);
            if (std::abs (start - written) <= 2)
                start = written;
            const auto sourceOffset = static_cast<int> (juce::jmax<juce::int64> (0, -start));
            const auto target = juce::jmax<juce::int64> (0, start);
            const auto toCopy = juce::jmax (0, count - sourceOffset);
            if (target + toCopy > std::numeric_limits<int>::max()
                || ! growBuffer (decoded->samples, channels, static_cast<int> (target + toCopy)))
            {
                CFRelease (sample);
                error = "The video exceeds the supported number of audio samples.";
                return {};
            }

            size_t listSize = 0;
            auto status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer (
                sample, &listSize, nullptr, 0, nullptr, nullptr, 0, nullptr);
            if (status != noErr || listSize < sizeof (AudioBufferList)
                || listSize > sizeof (AudioBufferList) + 16 * sizeof (AudioBuffer))
            {
                CFRelease (sample);
                error = "The video audio buffer layout is unsupported.";
                return {};
            }
            auto listStorage = std::make_unique<std::uint8_t[]> (listSize);
            auto* list = reinterpret_cast<AudioBufferList*> (listStorage.get());
            CMBlockBufferRef block = nullptr;
            status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer (
                sample, nullptr, list, listSize,
                nullptr, nullptr, kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block);
            if (status != noErr || list->mNumberBuffers < 1)
            {
                if (block != nullptr) CFRelease (block);
                CFRelease (sample);
                error = "The video audio samples could not be read.";
                return {};
            }
            for (int channel = 0; channel < channels; ++channel)
            {
                const auto interleaved = list->mNumberBuffers == 1 && channels == 2;
                if (! interleaved && static_cast<UInt32> (channel) >= list->mNumberBuffers)
                {
                    if (block != nullptr) CFRelease (block);
                    CFRelease (sample);
                    error = "The video audio channel layout is unsupported.";
                    return {};
                }
                const auto& audioBuffer = list->mBuffers[interleaved ? 0 : channel];
                const auto requiredBytes = static_cast<juce::int64> (count)
                                         * (interleaved ? channels : 1) * sizeof (float);
                if (audioBuffer.mData == nullptr
                    || static_cast<juce::int64> (audioBuffer.mDataByteSize) < requiredBytes)
                {
                    if (block != nullptr) CFRelease (block);
                    CFRelease (sample);
                    error = "The video audio samples have an unsupported format.";
                    return {};
                }
                const auto* source = static_cast<const float*> (audioBuffer.mData);
                auto* destination = decoded->samples.getWritePointer (channel, static_cast<int> (target));
                for (int index = 0; index < toCopy; ++index)
                {
                    const auto value = source[(index + sourceOffset) * (interleaved ? channels : 1)
                                              + (interleaved ? channel : 0)];
                    destination[index] = std::isfinite (value) ? value : 0.0f;
                }
            }
            written = juce::jmax (written, static_cast<int> (target + toCopy));
            if (block != nullptr) CFRelease (block);
            CFRelease (sample);
        }
        if (reader.status != AVAssetReaderStatusCompleted || channels == 0 || written == 0)
        {
            error = "The video audio track could not be decoded completely.";
            return {};
        }
        // The asset may have a short silent video tail after its audio track.
        const auto finalLength = juce::jmax (written,
            static_cast<int> (std::ceil (CMTimeGetSeconds (duration) * decoded->sampleRate)));
        decoded->samples.setSize (channels, finalLength, true, true, false);
        return decoded;
    }
}
