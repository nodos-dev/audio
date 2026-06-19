// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "SystemAudioCapture.h"

#include "nosAudio/AudioConversions.hpp"

#include <algorithm>

namespace nos::audio
{
namespace
{
// Keep at most this many seconds of buffered source samples to stop a stalled
// consumer (or a hung execution graph) from growing memory unbounded. The
// existing WASAPI backend used 5 seconds; we keep the same budget for parity.
constexpr uint32_t MAX_BUFFERED_SECONDS = 5;
} // namespace

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

	// Format renegotiated mid-stream — drop stale samples so the resampler
	// doesn't mix two layouts into a single read.
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

bool SystemAudioCaptureBase::ReadSamples(int32_t* outBuffer, uint32_t numSamples, uint8_t targetChannels, float gain)
{
	std::unique_lock lock(BufferMutex);

	// Nothing has been pushed yet (or the stream is idle): emit silence.
	if (SourceSampleRate == 0 || SourceChannelCount == 0)
	{
		for (uint32_t i = 0; i < numSamples * targetChannels; ++i)
			outBuffer[i] = 0;
		return false;
	}

	const float sampleRateRatio = static_cast<float>(SourceSampleRate) / static_cast<float>(TargetSampleRate);
	const uint32_t sourceSamplesNeeded = static_cast<uint32_t>(numSamples * sampleRateRatio);

	if (CapturedSamples.size() < static_cast<size_t>(sourceSamplesNeeded) * SourceChannelCount)
	{
		for (uint32_t i = 0; i < numSamples * targetChannels; ++i)
			outBuffer[i] = 0;
		return false;
	}

	// Linear interpolation resample + channel map + gain + int24 pack.
	for (uint32_t i = 0; i < numSamples; ++i)
	{
		const float sourceIndex = i * sampleRateRatio;
		const uint32_t sourceIndexInt = static_cast<uint32_t>(sourceIndex);
		const float frac = sourceIndex - sourceIndexInt;

		for (uint8_t ch = 0; ch < targetChannels; ++ch)
		{
			// Fold extra target channels onto source channel 0 (mono fallback).
			const uint8_t sourceChannel = (ch < SourceChannelCount) ? ch : 0;

			const uint32_t idx1 = sourceIndexInt * SourceChannelCount + sourceChannel;
			const uint32_t idx2 = std::min<uint32_t>(idx1 + SourceChannelCount,
													 static_cast<uint32_t>(CapturedSamples.size() - 1));

			if (idx1 < CapturedSamples.size() && idx2 < CapturedSamples.size())
			{
				const float sample1 = CapturedSamples[idx1];
				const float sample2 = CapturedSamples[idx2];
				float interpolated = sample1 + (sample2 - sample1) * frac;
				interpolated *= gain;
				outBuffer[i * targetChannels + ch] = FloatToShiftedInt24(interpolated);
			}
			else
			{
				outBuffer[i * targetChannels + ch] = 0;
			}
		}
	}

	const size_t samplesToRemove = static_cast<size_t>(sourceSamplesNeeded) * SourceChannelCount;
	if (samplesToRemove < CapturedSamples.size())
		CapturedSamples.erase(CapturedSamples.begin(), CapturedSamples.begin() + samplesToRemove);
	else
		CapturedSamples.clear();

	// Post-read drift correction: ReadSamples consumes at exactly real-time
	// rate, so any historical producer/consumer skew (startup gap, frame-drop
	// stall, path-restart burst) would otherwise persist as permanent latency
	// — we just pull from the head forever, staying N ms behind live. Cap the
	// residual buffer at a small smoothing window; anything older gets dropped
	// so the next read snaps back toward live. The discontinuity this causes
	// is audibly a one-shot click, preferable to sustained lag.
	constexpr float MAX_POST_READ_SECONDS = 0.1f;
	const size_t maxKeep = static_cast<size_t>(static_cast<float>(SourceSampleRate) * MAX_POST_READ_SECONDS) *
						   SourceChannelCount;
	if (maxKeep > 0 && CapturedSamples.size() > maxKeep)
	{
		const size_t drop = CapturedSamples.size() - maxKeep;
		CapturedSamples.erase(CapturedSamples.begin(), CapturedSamples.begin() + drop);
	}

	return true;
}
} // namespace nos::audio
