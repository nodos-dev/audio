// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nos::audio
{
// Platform-agnostic contract for capturing the host's system audio output
// (loopback). One concrete backend is linked per target OS; the node code
// only ever sees this interface. See the *Windows.cpp / *Mac.mm files for
// the actual WASAPI / ScreenCaptureKit implementations.
class ISystemAudioCapture
{
public:
	virtual ~ISystemAudioCapture() = default;

	// Prepare the backend for the node's requested target format. On failure
	// returns false and leaves a user-readable reason in GetLastError().
	virtual bool Initialize(uint32_t sampleRate, uint8_t channelCount) = 0;

	// Begin producing samples into the internal ring buffer. Idempotent.
	virtual bool Start() = 0;

	// Stop the backend and drain its worker. Safe to call before Start() or
	// more than once.
	virtual void Stop() = 0;

	// Pull numSamples interleaved shifted-int24 frames into outBuffer at the
	// requested channel layout and gain. Returns true when real audio was
	// delivered, false when the buffer was filled with silence because the
	// backend has not produced enough data yet.
	virtual bool ReadSamples(int32_t* outBuffer, uint32_t numSamples, uint8_t targetChannels, float gain) = 0;

	// Drop any audio that has accumulated in the internal ring buffer. Called
	// on path start so the consumer doesn't have to pay for latency that built
	// up between Start() and the first ReadSamples.
	virtual void DiscardBufferedSamples() = 0;

	virtual const std::string& GetDeviceName() const = 0;
	virtual const std::string& GetLastError() const = 0;

	// Platform factory — defined once per OS in its own TU.
	static std::unique_ptr<ISystemAudioCapture> Create();
};

// Shared ring-buffer + resampler scaffolding. Platform backends only have to
// push interleaved float frames via PushInterleavedSamples; the base handles
// rate conversion, gain, and int24 packing so the WASAPI / ScreenCaptureKit
// files can stay focused on their respective native API dances.
class SystemAudioCaptureBase : public ISystemAudioCapture
{
public:
	bool ReadSamples(int32_t* outBuffer, uint32_t numSamples, uint8_t targetChannels, float gain) override;
	void DiscardBufferedSamples() override { ResetBuffer(); }
	const std::string& GetDeviceName() const override { return DeviceName; }
	const std::string& GetLastError() const override { return LastError; }

protected:
	// Feed interleaved Float32 samples from the platform capture callback.
	// If sourceSampleRate or sourceChannelCount differ from the previous call
	// the internal buffer is reset, so format renegotiation mid-stream can't
	// produce torn audio.
	void PushInterleavedSamples(const float* samples,
								uint32_t frameCount,
								uint32_t sourceSampleRate,
								uint8_t sourceChannelCount);

	// Drop any buffered samples — used when the node reinitializes.
	void ResetBuffer();

	std::mutex BufferMutex;
	std::vector<float> CapturedSamples;
	uint32_t SourceSampleRate = 0;
	uint8_t SourceChannelCount = 0;
	uint32_t TargetSampleRate = 0;
	uint8_t TargetChannelCount = 0;
	std::string DeviceName;
	std::string LastError;
};
} // namespace nos::audio
