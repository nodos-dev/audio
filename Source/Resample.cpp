// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>

#include <nosSysVulkan/Helpers.hpp>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "nosAudio/Audio_generated.h"
#include "AudioConversions.hpp"

namespace nos::audio
{
struct ResampleNode : NodeContext
{
	nosResult OnCreate(nosFbNodePtr) override
	{
		return NOS_RESULT_SUCCESS;
	}

	void OnPathStart() override
	{
		// Reset any state if needed
	}
	
	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		auto pins = nos::NodeExecuteParams(params);
		auto inputBuf = pins.GetPinData<vkss::BufferPinData>(NOS_NAME("InputAudio"));

		auto& inputPacketDesc = *pins.GetPinData<AudioPacketDescriptor>(NOS_NAME("InputAudioPacketDescriptor"));
		auto& outputSampleRate = *pins.GetPinData<uint32_t>(NOS_NAME("OutputSampleRate"));
		auto& outputChannelCount = *pins.GetPinData<uint32_t>(NOS_NAME("OutputChannelCount"));

		// Calculate the number of output samples based on the input duration and output sample rate
		float inputDurationSeconds = static_cast<float>(inputPacketDesc.num_samples()) / static_cast<float>(inputPacketDesc.sample_rate());
		uint32_t outputNumSamples = static_cast<uint32_t>(inputDurationSeconds * static_cast<float>(outputSampleRate));

		// Create or resize output audio buffer only if needed
		size_t requiredBufferSize = outputNumSamples * sizeof(uint32_t) * outputChannelCount;
		size_t allocatedBufferSize = OutputAudio ? OutputAudio->Info.Buffer.Size : 0;
		
		if (!OutputAudio || requiredBufferSize > allocatedBufferSize)
		{
			OutputAudio = std::nullopt;
			
			// Allocate 1.1x the required size to reduce frequency of reallocations
			size_t newBufferSize = static_cast<size_t>(requiredBufferSize * 1.1f);
			
			nosBufferInfo audioBufferDesc = {};
			audioBufferDesc.Size = static_cast<uint32_t>(newBufferSize);
			audioBufferDesc.Usage = nosBufferUsage(NOS_BUFFER_USAGE_STORAGE_BUFFER | NOS_BUFFER_USAGE_TRANSFER_DST | NOS_BUFFER_USAGE_TRANSFER_SRC);
			audioBufferDesc.MemoryFlags = nosMemoryFlags(NOS_MEMORY_FLAGS_HOST_VISIBLE | NOS_MEMORY_FLAGS_FORCE_HOST_MEMORY);
			audioBufferDesc.ElementType = NOS_BUFFER_ELEMENT_TYPE_INT32;
			audioBufferDesc.FieldType = NOS_TEXTURE_FIELD_TYPE_PROGRESSIVE;
			
			OutputAudio = vkss::Resource::Create(audioBufferDesc, "Resample AudioBuffer");
			if (!OutputAudio)
				return NOS_RESULT_SUCCESS;

			nos::Buffer audioPacketPinData = OutputAudio->ToPinData();
			SetPinValue(NOS_NAME("OutputAudio"), audioPacketPinData);
		}
		
		nosResourceShareInfo& outputBufDesc = *OutputAudio;
		int32_t* outputAudioSamples = reinterpret_cast<int32_t*>(nosVulkan->Map(&outputBufDesc));
		int32_t* inputAudioSamples = reinterpret_cast<int32_t*>(nosVulkan->Map(&inputBuf));
		
		if (!outputAudioSamples || !inputAudioSamples)
		{
			return NOS_RESULT_SUCCESS;
		}

		// Sample rate conversion ratio
		float sampleRateRatio = static_cast<float>(inputPacketDesc.sample_rate()) / static_cast<float>(outputSampleRate);
		
		for (uint32_t outputSample = 0; outputSample < outputNumSamples; ++outputSample)
		{
			// Calculate the corresponding input sample position (with fractional part for interpolation)
			float inputSamplePos = static_cast<float>(outputSample) * sampleRateRatio;
			uint32_t inputSampleIndex = static_cast<uint32_t>(inputSamplePos);
			float fractionalPart = inputSamplePos - static_cast<float>(inputSampleIndex);
			
			// Ensure we don't go out of bounds
			if (inputSampleIndex >= inputPacketDesc.num_samples())
			{
				inputSampleIndex = inputPacketDesc.num_samples() - 1;
				fractionalPart = 0.0f;
			}
			
			for (uint32_t outputChannel = 0; outputChannel < outputChannelCount; ++outputChannel)
			{
				float outputSampleValue = 0.0f;
				
				if (outputChannel < inputPacketDesc.channel_count())
				{
					int32_t currentSample = inputAudioSamples[inputSampleIndex * inputPacketDesc.channel_count() + outputChannel];
					float currentValue = ShiftedInt24ToFloat(currentSample);
					
					if (inputSampleIndex + 1 < inputPacketDesc.num_samples() && fractionalPart > 0.0f)
					{
						int32_t nextSample = inputAudioSamples[(inputSampleIndex + 1) * inputPacketDesc.channel_count() + outputChannel];
						float nextValue = ShiftedInt24ToFloat(nextSample);
						
						// TODO: Add different interpolation methods
						outputSampleValue = currentValue + (nextValue - currentValue) * fractionalPart;
					}
					else
					{
						outputSampleValue = currentValue;
					}
				}
				else
				{
					outputSampleValue = 0.0f;
				}
				
				// Convert back to 24-bit shifted format and store
				outputAudioSamples[outputSample * outputChannelCount + outputChannel] = FloatToShiftedInt24(outputSampleValue);
			}
		}
		
		// Create output audio packet descriptor
		AudioPacketDescriptor outputPacketDesc(
			outputSampleRate, outputNumSamples, BitDepth::AUDIO_BIT_DEPTH_24_BIT, sizeof(int32_t), outputChannelCount);
		
		// Set output pin values
		SetPinValue(NOS_NAME("OutputAudioPacketDescriptor"), outputPacketDesc);
		
		return NOS_RESULT_SUCCESS;
	}

	std::optional<vkss::Resource> OutputAudio = std::nullopt;
};

nosResult RegisterResampleNode(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("Resample"), ResampleNode, fn);
	return NOS_RESULT_SUCCESS;
}
} // namespace nos::audio
