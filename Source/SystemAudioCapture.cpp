// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "SystemAudioCapture.h"

namespace nos::audio
{
// Safety valve: if the consumer stops draining (e.g. the path is stopped while
// the backend keeps capturing) cap the backlog so memory can't grow without
// bound. In normal operation the node drains every tick, so the buffer never
// gets near this. The original WASAPI backend used 5 seconds; keep parity.
constexpr uint32_t MAX_BUFFERED_SECONDS = 5;

void SystemAudioCaptureBase::ResetBuffer()
{
	std::lock_guard lock(BufferMutex);
	CapturedSamples.clear();
}

void SystemAudioCaptureBase::PushInterleavedSamples(const float* samples,
													uint32_t frameCount,
													uint32_t sourceSampleRate,
													uint8_t sourceChannelCount)
{
	if (!samples || frameCount == 0 || sourceChannelCount == 0 || sourceSampleRate == 0)
		return;

	std::lock_guard lock(BufferMutex);

	// Format renegotiated mid-stream — drop the stale batch so a single drain
	// doesn't hand out two layouts at once.
	if (sourceSampleRate != SourceSampleRate || sourceChannelCount != SourceChannelCount)
	{
		CapturedSamples.clear();
		SourceSampleRate = sourceSampleRate;
		SourceChannelCount = sourceChannelCount;
	}

	const size_t sampleCount = static_cast<size_t>(frameCount) * sourceChannelCount;
	CapturedSamples.insert(CapturedSamples.end(), samples, samples + sampleCount);

	const size_t maxSamples = static_cast<size_t>(SourceSampleRate) * SourceChannelCount * MAX_BUFFERED_SECONDS;
	if (CapturedSamples.size() > maxSamples)
	{
		const size_t overflow = CapturedSamples.size() - maxSamples;
		CapturedSamples.erase(CapturedSamples.begin(), CapturedSamples.begin() + overflow);
	}
}

void SystemAudioCaptureBase::DrainSamples(std::vector<float>& outSamples,
										  uint32_t& outSampleRate,
										  uint8_t& outChannelCount)
{
	std::lock_guard lock(BufferMutex);
	// Swap rather than copy: outSamples takes the captured batch and leaves its
	// (now-empty) storage behind for the next accumulation, so the steady state
	// allocates nothing.
	outSamples.clear();
	outSamples.swap(CapturedSamples);
	outSampleRate = SourceSampleRate;
	outChannelCount = SourceChannelCount;
}
} // namespace nos::audio
