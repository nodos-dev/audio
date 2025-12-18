// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>

#include <nosVulkanSubsystem/Helpers.hpp>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "Audio_generated.h"
#include "AudioConversions.hpp"

namespace nos::audio
{
struct UnpackAudioBuffer : NodeContext
{
	nosResult OnCreate(nosFbNodePtr) override
	{
		return NOS_RESULT_SUCCESS;
	}

	void OnPathStart() override
	{
	}

	nosResult ExecuteNode(NodeExecuteParams const& pins) override
	{
		auto inputBuf = pins.GetPinObject<sys::vulkan::Buffer>(NOS_NAME("PrefixedAudioBuffer"));

		// Wait GPU
		nosCmd cmd{};
		nosCmdBeginParams beginParams{
			.Name = NOS_NAME("Wait Audio Buffer"),
			.AssociatedNodeId = NodeId,
			.OutCmdHandle = &cmd,
		};
		nosVulkan->Begin(&beginParams);

		nosGPUEvent waitHandle{};
		nosCmdEndParams endParams{
			.ForceSubmit = NOS_TRUE,
			.OutGPUEventHandle = &waitHandle
		};
		nosVulkan->End(cmd, &endParams);

		nosVulkan->WaitGpuEvent(&waitHandle, UINT64_MAX);

		// Map the input buffer to read the prefixed data
		uint8_t* inputData = nosVulkan->Map(inputBuf);
		if (!inputData)
			return NOS_RESULT_SUCCESS;

		// Read the prefix data: sampleRate (4 bytes), numSamples (4 bytes), channelCount (4 bytes)
		int32_t sampleRate = *reinterpret_cast<int32_t*>(inputData);
		int32_t numSamples = *reinterpret_cast<int32_t*>(inputData + 4);
		int32_t channelCount = *reinterpret_cast<int32_t*>(inputData + 8);

		if (numSamples > 100000)
			return NOS_RESULT_SUCCESS;
		
		// Point to the float audio data after the 12-byte prefix
		float* inputAudioSamples = reinterpret_cast<float*>(inputData + 12);
		
		// Create or resize audio buffer only if needed (with 1.1x headroom to avoid frequent reallocations)
		size_t requiredBufferSize = std::max(size_t(numSamples * sizeof(uint32_t) * channelCount), size_t(1000));
		size_t allocatedBufferSize = OutputAudioBuffer ? sys::vulkan::GetResourceInfo(OutputAudioBuffer)->Size : 0;
		
		if (!OutputAudioBuffer || requiredBufferSize > allocatedBufferSize)
		{
			OutputAudioBuffer = {};
			
			// Allocate 1.1x the required size to reduce frequency of reallocations
			size_t newBufferSize = requiredBufferSize * 1.1f;
			
			nosBufferInfo audioBufferDesc = {};
			audioBufferDesc.Size = static_cast<uint32_t>(newBufferSize);
			audioBufferDesc.Usage = nosBufferUsage(NOS_BUFFER_USAGE_STORAGE_BUFFER | NOS_BUFFER_USAGE_TRANSFER_DST | NOS_BUFFER_USAGE_TRANSFER_SRC);
			audioBufferDesc.MemoryFlags = nosMemoryFlags(NOS_MEMORY_FLAGS_HOST_VISIBLE);
			audioBufferDesc.ElementType = NOS_BUFFER_ELEMENT_TYPE_INT32;
			
			OutputAudioBuffer = sys::vulkan::CreateBuffer(audioBufferDesc, "Unpacked Audio Buffer");
			if (!OutputAudioBuffer)
				return NOS_RESULT_SUCCESS;
		}
		
		int32_t* outputAudioSamples = reinterpret_cast<int32_t*>(nosVulkan->Map(OutputAudioBuffer));
		if (!outputAudioSamples)
			return NOS_RESULT_SUCCESS;
		
		// Convert float samples to 24-bit MSB int32 format
		for (uint32_t i = 0; i < numSamples * channelCount; ++i)
		{
			// Clamp float sample to [-1.0, 1.0] range
			float floatSample = std::max(-1.0f, std::min(1.0f, inputAudioSamples[i]));
			
			// Store as 32-bit with 24-bit sample in MSB (shift left by 8 bits)
			outputAudioSamples[i] = FloatToShiftedInt24(floatSample);
		}
		
		AudioPacketDescriptor audioPacketDesc(
			sampleRate, numSamples, BitDepth::AUDIO_BIT_DEPTH_24_BIT, 4, channelCount);

		auto newDescObj = PrimitiveObjectRef::Create(NOS_NAME("nos.audio.AudioPacketDescriptor"), nos::Buffer::From(audioPacketDesc));

		std::unordered_map<nos::Name, nos::ObjectRef> audioPacketFields;
		audioPacketFields[NOS_NAME("desc")] = newDescObj.value_or(ObjectRef());
		audioPacketFields[NOS_NAME("buffer")] = OutputAudioBuffer;
		auto audioPacket = CompositeObjectRef::Create(NOS_NAME("nos.audio.AudioPacket"), audioPacketFields);
		if (!audioPacket)
			return NOS_RESULT_FAILED;

		SetPinObject(NOS_NAME("Audio"), *audioPacket);

		return NOS_RESULT_SUCCESS;
	}

	TypedObjectRef<sys::vulkan::Buffer> OutputAudioBuffer;
};

nosResult RegisterUnpackAudioBufferNode(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("UnpackAudioBuffer"), UnpackAudioBuffer, fn);
	return NOS_RESULT_SUCCESS;
}
} // namespace nos::audio

