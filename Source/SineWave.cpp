// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>

#include <nosSysVulkan/Helpers.hpp>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "nosAudio/Audio_generated.h"
#include "nosAudio/AudioConversions.hpp"

namespace nos::audio
{
struct SineWave : NodeContext
{
	nosResult OnCreate(nosFbNodePtr) override
	{
		return NOS_RESULT_SUCCESS;
	}

	void OnPathStart() override
	{
		CurrentSampleIndex = 0;
		TimeSoFar = 0;
		AccumulatedSampleNumerator = 0;
	}
	
	nosResult ExecuteNode(NodeExecuteParams const& pins) override
	{
		auto& waveFrequency = *pins.GetPinData<float>(NOS_NAME("WaveFrequency"));
		auto& waveAmplitude = *pins.GetPinData<float>(NOS_NAME("WaveAmplitude"));
		auto& sampleRate = *pins.GetPinData<uint32_t>(NOS_NAME("SampleRate"));
		uint8_t channelCount = *pins.GetPinData<uint8_t>(NOS_NAME("ChannelCount"));

		// Only support fixed step timing
		if (pins.TimingMode != NOS_EXECUTION_TIMING_MODE_FIXED_STEP) {
			return NOS_RESULT_FAILED;
		}
		
		// Check for invalid timing values
		if (pins.FixedStepTiming.DeltaSeconds.y == 0) {
			return NOS_RESULT_FAILED;
		}
		
		uint64_t deltaNumerator = pins.FixedStepTiming.DeltaSeconds.x;
		uint64_t deltaDenominator = pins.FixedStepTiming.DeltaSeconds.y;
		
		AccumulatedSampleNumerator += deltaNumerator * static_cast<uint64_t>(sampleRate);
		
		uint32_t numSamples = static_cast<uint32_t>(AccumulatedSampleNumerator / deltaDenominator);
		AccumulatedSampleNumerator %= deltaDenominator; // Keep remainder for next frame
		
		// Create or resize audio buffer only if needed (with 1.5x headroom to avoid frequent reallocations)
		size_t requiredBufferSize = numSamples * sizeof(uint32_t) * channelCount;
		size_t allocatedBufferSize = AudioPacketBuffer ? sys::vulkan::GetResourceInfo(AudioPacketBuffer)->Size : 0;
		
		if (!AudioPacketBuffer || requiredBufferSize > allocatedBufferSize)
		{
			AudioPacketBuffer = {};
			
			// Allocate 1.5x the required size to reduce frequency of reallocations
			size_t newBufferSize = requiredBufferSize * 1.5f;
			
			nosBufferInfo audioBufferDesc = {};
			audioBufferDesc.Size = static_cast<uint32_t>(newBufferSize);
			audioBufferDesc.Usage = nosBufferUsage(NOS_BUFFER_USAGE_STORAGE_BUFFER | NOS_BUFFER_USAGE_TRANSFER_DST | NOS_BUFFER_USAGE_TRANSFER_SRC);
			audioBufferDesc.MemoryFlags = NOS_MEMORY_FLAGS_HOST_VISIBLE;
			audioBufferDesc.ElementType = NOS_BUFFER_ELEMENT_TYPE_INT32;
			
			AudioPacketBuffer = sys::vulkan::CreateBuffer(audioBufferDesc, "SineWave AudioBuffer");
			if (!AudioPacketBuffer)
				return NOS_RESULT_FAILED;
		}
		
		int32_t* audioSamples = reinterpret_cast<int32_t*>(nosVulkan->Map(AudioPacketBuffer));
		if (!audioSamples) {
			return NOS_RESULT_FAILED;
		}
		
		for (uint32_t i = 0; i < numSamples; ++i)
		{
			float sampleTime = waveFrequency / static_cast<float>(sampleRate) + LastSampleTime;
			sampleTime = std::fmod(sampleTime, 1.0f);
			LastSampleTime = sampleTime;
			float floatSample = waveAmplitude * std::sin(2.0f * static_cast<float>(M_PI) * sampleTime);
			
			int32_t sampleShifted = FloatToShiftedInt24(floatSample);
			for (auto channel = 0; channel < channelCount; ++channel)
			{
				audioSamples[i * channelCount + channel] = sampleShifted; // Store as 32-bit with 24-bit sample in MSB
			}
		}
		
		// Update current sample index for continuous playback
		CurrentSampleIndex += numSamples;

		AudioPacketDescriptor audioPacketDesc(
			sampleRate, numSamples, BitDepth::AUDIO_BIT_DEPTH_24_BIT, 4, channelCount);

		auto descObj = PrimitiveObjectRef::Create(
			NOS_NAME("nos.audio.AudioPacketDescriptor"),
			nos::Buffer::From(audioPacketDesc));

		std::unordered_map<nos::Name, nos::ObjectRef> audioPacketFields;
		audioPacketFields[NOS_NAME("desc")] = descObj.value_or(ObjectRef());
		audioPacketFields[NOS_NAME("buffer")] = AudioPacketBuffer;
		auto audioPacket = CompositeObjectRef::Create(NOS_NAME("nos.audio.AudioPacket"), audioPacketFields);
		if (!audioPacket)
			return NOS_RESULT_FAILED;

		SetPinObject(NOS_NAME("AudioPacket"), *audioPacket);
		return NOS_RESULT_SUCCESS;
	}

	TypedObjectRef<sys::vulkan::Buffer> AudioPacketBuffer;
	uint64_t CurrentSampleIndex;
	uint64_t TimeSoFar;
	uint64_t AccumulatedSampleNumerator; // Accumulates fractional samples as integer numerator
	float LastSampleTime = 0.0f;
};

nosResult RegisterSineWaveNode(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("SineWave"), SineWave, fn);
	return NOS_RESULT_SUCCESS;
}
} // namespace nos::audio

