// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>

#include <nosVulkanSubsystem/Helpers.hpp>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
	
	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		auto pins = nos::NodeExecuteParams(params);
		auto& waveFrequency = *pins.GetPinData<float>(NOS_NAME("WaveFrequency"));
		auto& waveAmplitude = *pins.GetPinData<float>(NOS_NAME("WaveAmplitude"));
		auto& sampleRate = *pins.GetPinData<uint32_t>(NOS_NAME("SampleRate"));
		
		// Only support fixed step timing
		if (pins.TimingMode != NOS_EXECUTION_TIMING_MODE_FIXED_STEP) {
			return NOS_RESULT_FAILED;
		}
		
		// Check for invalid timing values
		if (pins.FixedStepTiming.DeltaSeconds.y == 0) {
			return NOS_RESULT_FAILED;
		}
		
		TimeSoFar += pins.FixedStepTiming.DeltaSeconds.x;
		
		uint64_t deltaNumerator = pins.FixedStepTiming.DeltaSeconds.x;
		uint64_t deltaDenominator = pins.FixedStepTiming.DeltaSeconds.y;
		
		// Accumulate samples using fixed-point arithmetic: (delta * sampleRate) / denominator
		AccumulatedSampleNumerator += deltaNumerator * static_cast<uint64_t>(sampleRate);
		
		uint32_t numSamples = static_cast<uint32_t>(AccumulatedSampleNumerator / deltaDenominator);
		AccumulatedSampleNumerator %= deltaDenominator; // Keep remainder for next frame
		
		// Create or resize audio buffer only if needed (with 1.5x headroom to avoid frequent reallocations)
		// AJA expects 32-bit words (24-bit samples in MSB)
		size_t requiredBufferSize = numSamples * sizeof(uint32_t);
		size_t allocatedBufferSize = AudioPacket ? AudioPacket->Info.Buffer.Size : 0;
		
		if (!AudioPacket || requiredBufferSize > allocatedBufferSize)
		{
			AudioPacket = std::nullopt;
			
			// Allocate 1.5x the required size to reduce frequency of reallocations
			size_t newBufferSize = static_cast<size_t>(requiredBufferSize * 1.5f);
			
			nosBufferInfo audioBufferDesc = {};
			audioBufferDesc.Size = static_cast<uint32_t>(newBufferSize);
			audioBufferDesc.Usage = nosBufferUsage(NOS_BUFFER_USAGE_STORAGE_BUFFER | NOS_BUFFER_USAGE_TRANSFER_DST | NOS_BUFFER_USAGE_TRANSFER_SRC);
			audioBufferDesc.MemoryFlags = NOS_MEMORY_FLAGS_HOST_VISIBLE;
			audioBufferDesc.ElementType = NOS_BUFFER_ELEMENT_TYPE_UINT32;
			audioBufferDesc.FieldType = NOS_TEXTURE_FIELD_TYPE_PROGRESSIVE;
			
			AudioPacket = vkss::Resource::Create(audioBufferDesc, "SineWave AudioBuffer");
			if (!AudioPacket)
				return NOS_RESULT_FAILED;

			nos::Buffer audioPacketPinData = AudioPacket->ToPinData();
			SetPinValue(NOS_NAME("AudioPacket"), audioPacketPinData);
		}
		
		// Generate sine wave samples and convert to 24-bit format for AJA
		nosResourceShareInfo& audioBufDesc = *AudioPacket;
		uint32_t* audioSamples = reinterpret_cast<uint32_t*>(nosVulkan->Map(&audioBufDesc));
		if (!audioSamples) {
			return NOS_RESULT_FAILED;
		}
		
		for (uint32_t i = 0; i < numSamples; ++i) {
			float sampleTime = static_cast<float>(CurrentSampleIndex + i) / static_cast<float>(sampleRate);
			float phase = 2.0f * static_cast<float>(M_PI) * waveFrequency * sampleTime;
			float floatSample = waveAmplitude * std::sin(phase);
			
			// Convert float (-1.0 to 1.0) to 24-bit integer in MSB of 32-bit word
			// 24-bit range: -8,388,608 to 8,388,607
			int32_t sample24bit = static_cast<int32_t>(floatSample * 8388607.0f);
			
			// Clamp to 24-bit range
			sample24bit = std::max(-8388608, std::min(8388607, sample24bit));
			
			// Pack 24-bit sample into MSB of 32-bit word (shift left 8 bits)
			audioSamples[i] = static_cast<uint32_t>(sample24bit << 8);
		}
		
		// Update current sample index for continuous playback
		CurrentSampleIndex += numSamples;
		
		// Set output pin values
		SetPinValue(NOS_NAME("NumSamples"), numSamples);
		
		return NOS_RESULT_SUCCESS;
	}

	std::optional<vkss::Resource> AudioPacket = std::nullopt;
	uint64_t CurrentSampleIndex;
	uint64_t TimeSoFar;
	uint64_t AccumulatedSampleNumerator; // Accumulates fractional samples as integer numerator
};

nosResult RegisterSineWaveNode(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("SineWave"), SineWave, fn);
	return NOS_RESULT_SUCCESS;
}
} // namespace nos::audio

