// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>
#include <nosVulkanSubsystem/Helpers.hpp>
#include <cmath>
#include <algorithm>
#include <complex>
#include <chrono>

#include "Audio_generated.h"
#include "AudioConversions.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace nos::audio
{

struct AudioOscilloscopeNode : NodeContext
{
	nosResult OnCreate(nosFbNodePtr) override
	{
		StartTime = std::chrono::high_resolution_clock::now();
		return NOS_RESULT_SUCCESS;
	}

	void OnPathStart() override
	{
		// Initialize state for audio processing
		StartTime = std::chrono::high_resolution_clock::now();
	}
	
	nosResult ExecuteNode(NodeExecuteParams const& pins) override
	{
		// Get inputs
		auto inputPacket = pins.GetPinObject<AudioPacket>(NOS_NAME("AudioPacket"));
		ObjectRef packetDescObj{}, inputAudioBuf{};
		nosEngine.ObjectAPI->GetField(inputPacket, NOS_NAME("desc"), &packetDescObj.GetStorage());
		nosEngine.ObjectAPI->GetField(inputPacket, NOS_NAME("buffer"), &inputAudioBuf.GetStorage());

		if (!packetDescObj || !inputAudioBuf)
			return NOS_RESULT_FAILURE;

		const nosBuffer* descBuf{};
		if (NOS_RESULT_SUCCESS != nosEngine.ObjectAPI->GetPrimitiveObjectDataView(packetDescObj, &descBuf))
			return NOS_RESULT_FAILURE;
		auto& inputPacketDesc = *static_cast<AudioPacketDescriptor*>(descBuf->Data);

		auto outputTexture = pins.GetPinObject<sys::vulkan::Texture>(NOS_NAME("Output"));

		auto& thickness = *pins.GetPinData<float>(NOS_NAME("Thickness"));
		auto& amplitude = *pins.GetPinData<float>(NOS_NAME("Amplitude"));
		auto& intensity = *pins.GetPinData<float>(NOS_NAME("Intensity"));
		auto& color = *pins.GetPinData<nos::fb::vec4>(NOS_NAME("Color"));
		auto& audioScale = *pins.GetPinData<float>(NOS_NAME("AudioScale"));
		auto& glowIntensity = *pins.GetPinData<float>(NOS_NAME("GlowIntensity"));
		auto& glowFalloff = *pins.GetPinData<float>(NOS_NAME("GlowFalloff"));
		auto& frameAverage = *pins.GetPinData<uint32_t>(NOS_NAME("FrameAverage"));

		// Map input audio buffer
		int32_t* inputAudioSamples = reinterpret_cast<int32_t*>(nosVulkan->Map(inputAudioBuf));
		if (!inputAudioSamples)
			return NOS_RESULT_FAILED;

		// Process audio to extract amplitude levels for oscilloscope display
		uint32_t numSamples = inputPacketDesc.num_samples();
		uint32_t channelCount = inputPacketDesc.channel_count();
		
		// Mix down to mono for analysis
		std::vector<float> monoAudio(numSamples);
		for (uint32_t i = 0; i < numSamples; ++i)
		{
			float sum = 0.0f;
			for (uint32_t ch = 0; ch < channelCount; ++ch)
			{
				int32_t sample = inputAudioSamples[i * channelCount + ch];
				sum += ShiftedInt24ToFloat(sample);
			}
			monoAudio[i] = sum / static_cast<float>(channelCount);
		}

		// Create oscilloscope trace texture from audio data (time-domain amplitude)
		const uint32_t scopeTexSize = 256;
		
		// Ensure frame history buffer is properly sized
		uint32_t maxFrames = std::max(1u, frameAverage);
		if (FrameHistory.size() != maxFrames) {
			FrameHistory.resize(maxFrames);
			for (auto& frame : FrameHistory) {
				frame.assign(scopeTexSize, 0.0f);
			}
			CurrentFrameIndex = 0;
		}
		if (!ScopeTexture || sys::vulkan::GetResourceInfo(ScopeTexture)->Width != scopeTexSize)
		{
			nosTextureInfo texInfo = {};
			texInfo.Width = scopeTexSize;
			texInfo.Height = 1;
			texInfo.Format = NOS_FORMAT_R32_SFLOAT;
			texInfo.Usage = nosImageUsage(NOS_IMAGE_USAGE_SAMPLED | NOS_IMAGE_USAGE_TRANSFER_DST);
			texInfo.FieldType = NOS_TEXTURE_FIELD_TYPE_PROGRESSIVE;
			
			ScopeTexture = sys::vulkan::CreateTexture(texInfo, "AudioOscilloscope TraceTexture");
			if (!ScopeTexture)
				return NOS_RESULT_FAILED;
		}

		std::vector<float> currentFrameData(scopeTexSize);
		uint32_t samplesPerBin = std::max(1u, numSamples / scopeTexSize);
		
		for (uint32_t bin = 0; bin < scopeTexSize; ++bin)
		{
			uint32_t startSample = bin * samplesPerBin;
			uint32_t endSample = std::min(startSample + samplesPerBin, numSamples);
			float binValue = 0.0f;
			for (uint32_t i = startSample; i < endSample; ++i)
			{
				binValue += monoAudio[i];
			}
			binValue /= (endSample - startSample); // Average the samples in this bin
			
			currentFrameData[bin] = binValue;
		}

		// Store current frame data and calculate moving average
		FrameHistory[CurrentFrameIndex] = currentFrameData;
		CurrentFrameIndex = (CurrentFrameIndex + 1) % FrameHistory.size();
		
		// Calculate moving average across all stored frames
		std::vector<float> scopeData(scopeTexSize, 0.0f);
		for (const auto& frame : FrameHistory)
		{
			for (uint32_t bin = 0; bin < scopeTexSize; ++bin)
			{
				scopeData[bin] += frame[bin];
			}
		}

		// Normalize by number of frames
		for (uint32_t bin = 0; bin < scopeTexSize; ++bin)
		{
			scopeData[bin] /= static_cast<float>(FrameHistory.size());
		}

		nosCmd cmd;
		nosCmdBeginParams beginParams{
			.Name = NOS_NAME("Audio Oscilloscope Upload"),
			.AssociatedNodeId = NodeId,
			.OutCmdHandle = &cmd,
			.PreferredQueueType = NOS_CMD_QUEUE_TYPE_MAIN
		};
		nosVulkan->Begin(&beginParams);

		nosVec2u extent = {scopeTexSize, 1};
		nosResult loadResult = nosVulkan->ImageLoad(cmd, scopeData.data(), extent, NOS_FORMAT_R32_SFLOAT, ScopeTexture.GetObjectId(), NOS_TEXTURE_FILTER_LINEAR);
		if (loadResult != NOS_RESULT_SUCCESS)
		{
			nosVulkan->End(cmd, nullptr);
			nosEngine.LogE("AudioOscilloscope: Failed to upload trace texture data");
			return NOS_RESULT_FAILED;
		}

		nosVulkan->End(cmd, nullptr);

		std::vector<nosShaderBinding> bindings = {
			sys::vulkan::ShaderTextureBinding(NOS_NAME("TraceData"), ScopeTexture, NOS_TEXTURE_FILTER_LINEAR),
			sys::vulkan::ShaderDataBinding(NOS_NAME("Thickness"), thickness),
			sys::vulkan::ShaderDataBinding(NOS_NAME("Amplitude"), amplitude),
			sys::vulkan::ShaderDataBinding(NOS_NAME("Intensity"), intensity),
			sys::vulkan::ShaderDataBinding(NOS_NAME("Color"), color),
			sys::vulkan::ShaderDataBinding(NOS_NAME("AudioScale"), audioScale),
			sys::vulkan::ShaderDataBinding(NOS_NAME("GlowIntensity"), glowIntensity),
			sys::vulkan::ShaderDataBinding(NOS_NAME("GlowFalloff"), glowFalloff)
		};

		nosRunPassParams passParams{
			.Key = NOS_NAME("AUDIO_OSCILLOSCOPE_PASS"),
			.Bindings = bindings.data(),
			.BindingCount = static_cast<uint32_t>(bindings.size()),
			.Output = outputTexture,
			.Wireframe = NOS_FALSE,
			.Benchmark = NOS_FALSE,
			.DoNotClear = NOS_FALSE,
			.ClearCol = {0.0f, 0.0f, 0.0f, 1.0f},
			.CullMode = NOS_CULL_MODE_BACK
		};

		nosCmdBeginParams visualBeginParams{
			.Name = NOS_NAME("Audio Oscilloscope"),
			.AssociatedNodeId = NodeId,
			.OutCmdHandle = &cmd,
			.PreferredQueueType = NOS_CMD_QUEUE_TYPE_MAIN
		};
		nosVulkan->Begin(&visualBeginParams);
		nosVulkan->RunPass(cmd, &passParams);
		nosVulkan->End(cmd, nullptr);

		return NOS_RESULT_SUCCESS;
	}

	TypedObjectRef<sys::vulkan::Texture> ScopeTexture;
	std::chrono::high_resolution_clock::time_point StartTime;
	
	// Moving average frame history
	std::vector<std::vector<float>> FrameHistory;
	uint32_t CurrentFrameIndex = 0;
};

nosResult RegisterAudioOscilloscopeNode(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("AudioOscilloscope"), AudioOscilloscopeNode, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::audio
