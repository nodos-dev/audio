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

	// Prepare the backend. The capture format is whatever the OS provides — the
	// shared-mode mix format on Windows, a fixed default on macOS — and is
	// reported back via DrainSamples. On failure returns false and leaves a
	// reason in GetLastError().
	virtual bool Initialize() = 0;

	// Begin producing samples into the internal buffer. Idempotent.
	virtual bool Start() = 0;

	// Stop the backend and drain its worker. Safe to call before Start() or
	// more than once.
	virtual void Stop() = 0;

	// Hand over every interleaved Float32 frame captured since the last call:
	// outSamples is swapped with the internal buffer (so it returns cleared),
	// and the device's actual negotiated format is reported via outSampleRate /
	// outChannelCount (both 0 until the backend has produced its first frame).
	// The node ships exactly what the device produced, at the device's own
	// rate — no resampling, no fixed sample count — and labels the AudioPacket
	// with that format. Drift and jitter become honest variable packet sizes
	// that a downstream Resample node reconciles against its own clock.
	virtual void DrainSamples(std::vector<float>& outSamples, uint32_t& outSampleRate, uint8_t& outChannelCount) = 0;

	// Drop any buffered samples. Called on path start so the first tick after a
	// restart doesn't ship the backlog accumulated since Start().
	virtual void DiscardBufferedSamples() = 0;

	virtual const std::string& GetDeviceName() const = 0;
	virtual const std::string& GetLastError() const = 0;

	// Platform factory — defined once per OS in its own TU.
	static std::unique_ptr<ISystemAudioCapture> Create();
};

// Shared capture-buffer scaffolding. Platform backends only have to push
// interleaved Float32 frames via PushInterleavedSamples; the base accumulates
// them and hands the whole batch over on DrainSamples, so the WASAPI /
// ScreenCaptureKit files can stay focused on their respective native API dances.
class SystemAudioCaptureBase : public ISystemAudioCapture
{
public:
	void DrainSamples(std::vector<float>& outSamples, uint32_t& outSampleRate, uint8_t& outChannelCount) override;
	void DiscardBufferedSamples() override { ResetBuffer(); }
	const std::string& GetDeviceName() const override { return DeviceName; }
	const std::string& GetLastError() const override { return LastError; }

protected:
	// Feed interleaved Float32 samples from the platform capture callback.
	// If sourceSampleRate or sourceChannelCount differ from the previous call
	// the internal buffer is reset, so format renegotiation mid-stream can't
	// produce a batch that mixes two layouts.
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
	std::string DeviceName;
	std::string LastError;
};
} // namespace nos::audio
