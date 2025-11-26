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
struct AudioPlayerNode : NodeContext
{
	nosResult OnCreate(nosFbNodePtr) override { return NOS_RESULT_SUCCESS; }

	void OnPathStart() override
	{
		AccumulatedSampleNumerator = 0;
		LastSampleTime = 0;
		LastSampleTimeFract = 0.0f;
	}

	nosResult ExecuteNode(NodeExecuteParams const& pins) override
	{
		auto fullAudio = pins.GetPinObject(NOS_NAME("FullAudio"));
		
		ObjectRef desc{}, buf{};
		nosEngine.ObjectAPI->GetField(fullAudio, NOS_NAME("desc"), &desc.GetStorage());
		nosEngine.ObjectAPI->GetField(fullAudio, NOS_NAME("buffer"), &buf.GetStorage());

		if (!desc || !buf)
			return NOS_RESULT_FAILURE;

		nosImmutableBuffer descBuf{};
		if (NOS_RESULT_SUCCESS != nosEngine.ObjectAPI->GetObjectDataView(desc, &descBuf))
			return NOS_RESULT_FAILURE;

		auto& inputPacketDesc = *static_cast<const AudioPacketDescriptor*>(descBuf.Data);
		auto& soundBoost = *pins.GetPinData<float>(NOS_NAME("SoundBoost"));
		auto& targetSampleRate = *pins.GetPinData<uint32_t>(NOS_NAME("TargetSampleRate"));
		auto inputSampleRate = inputPacketDesc.sample_rate();
		// Only support fixed step timing
		if (pins.TimingMode != NOS_EXECUTION_TIMING_MODE_FIXED_STEP)
		{
			return NOS_RESULT_FAILED;
		}

		// Check for invalid timing values
		if (pins.FixedStepTiming.DeltaSeconds.y == 0)
		{
			return NOS_RESULT_FAILED;
		}

		uint64_t deltaNumerator = pins.FixedStepTiming.DeltaSeconds.x;
		uint64_t deltaDenominator = pins.FixedStepTiming.DeltaSeconds.y;

		AccumulatedSampleNumerator += deltaNumerator * static_cast<uint64_t>(targetSampleRate);

		uint32_t numSamples = static_cast<uint32_t>(AccumulatedSampleNumerator / deltaDenominator);
		AccumulatedSampleNumerator %= deltaDenominator; // Keep remainder for next frame

		// Create or resize audio buffer only if needed (with 1.1x headroom to avoid frequent reallocations)
		size_t requiredBufferSize = numSamples * sizeof(uint32_t) * inputPacketDesc.channel_count();
		size_t allocatedBufferSize = 0;
		if (OutputAudio)
		{
			if (auto bufferInfo = sys::vulkan::GetResourceInfo(OutputAudio))
				allocatedBufferSize = bufferInfo->Size;
			else
				NOS_SOFT_CHECK(false, "Failed to get buffer info for existing audio buffer");
		}

		if (!OutputAudio || requiredBufferSize > allocatedBufferSize)
		{
			OutputAudio = {};

			// Allocate 1.1x the required size to reduce frequency of reallocations
			size_t newBufferSize = requiredBufferSize * 1.1f;

			nosBufferInfo audioBufferDesc = {};
			audioBufferDesc.Size = static_cast<uint32_t>(newBufferSize);
			audioBufferDesc.Usage = nosBufferUsage(NOS_BUFFER_USAGE_STORAGE_BUFFER | NOS_BUFFER_USAGE_TRANSFER_DST |
												   NOS_BUFFER_USAGE_TRANSFER_SRC);
			audioBufferDesc.MemoryFlags =
				nosMemoryFlags(NOS_MEMORY_FLAGS_HOST_VISIBLE | NOS_MEMORY_FLAGS_FORCE_HOST_MEMORY);
			audioBufferDesc.ElementType = NOS_BUFFER_ELEMENT_TYPE_INT32;
			audioBufferDesc.FieldType = NOS_TEXTURE_FIELD_TYPE_PROGRESSIVE;

			OutputAudio = sys::vulkan::CreateBuffer(audioBufferDesc, "AudioPlayer AudioBuffer");
			if (!OutputAudio)
				return NOS_RESULT_FAILED;
		}

		int32_t* outAudioSamples = reinterpret_cast<int32_t*>(nosVulkan->Map(OutputAudio));
		int32_t* inputAudioSamples = reinterpret_cast<int32_t*>(nosVulkan->Map(buf));
		if (!outAudioSamples)
		{
			return NOS_RESULT_FAILED;
		}

		for (uint32_t i = 0; i < numSamples; ++i)
		{
			float targetSampleTimeFract = 1.0f / static_cast<float>(targetSampleRate) + LastSampleTimeFract;
			uint64_t targetSampleTime = LastSampleTime + static_cast<uint64_t>(targetSampleTimeFract);
			targetSampleTimeFract = std::fmod(targetSampleTimeFract, 1.0f);
			LastSampleTime = targetSampleTime;
			LastSampleTimeFract = targetSampleTimeFract;

			float sourceSampleIndexFract = targetSampleTimeFract * inputSampleRate;
			uint64_t sourceSampleIndex = targetSampleTime * inputSampleRate + static_cast<uint64_t>(sourceSampleIndexFract);
			sourceSampleIndex %= inputPacketDesc.num_samples();
			// This will be used to interpolate the sample
			sourceSampleIndexFract = std::fmod(sourceSampleIndexFract, 1.0f);

			for (auto channel = 0; channel < inputPacketDesc.channel_count(); ++channel)
			{
				int32_t sample1 = inputAudioSamples[sourceSampleIndex * inputPacketDesc.channel_count() + channel];
				uint64_t nextSampleIndex = (sourceSampleIndex + 1) % inputPacketDesc.num_samples();
				int32_t sample2 = inputAudioSamples[nextSampleIndex * inputPacketDesc.channel_count()  + channel];
				float sample1Shifted = ShiftedInt24ToFloat(sample1);
				float sample2Shifted = ShiftedInt24ToFloat(sample2);
				float interpolated = std::lerp(sample1Shifted, sample2Shifted, sourceSampleIndexFract) * soundBoost;

				int32_t sampleShifted = FloatToShiftedInt24(interpolated);

				outAudioSamples[i * inputPacketDesc.channel_count() + channel] = sampleShifted;
			}
		}

		AudioPacketDescriptor audioPacketDesc(
			targetSampleRate, numSamples, BitDepth::AUDIO_BIT_DEPTH_24_BIT, sizeof(int32_t), inputPacketDesc.channel_count());

		ObjectRef outDesc{};
		nosEngine.ObjectAPI->CreatePrimitiveObject(NOS_NAME(AudioPacketDescriptor::GetFullyQualifiedName()), nos::Buffer::From(audioPacketDesc), &outDesc.GetStorage());
		
		ObjectRef out{};
		std::vector<nosCompositeObjectField> fields;
		fields.push_back(nosCompositeObjectField{
			.FieldName = NOS_NAME("desc"),
			.FieldObjectId = outDesc,
		});
		fields.push_back(nosCompositeObjectField{
			.FieldName = NOS_NAME("buffer"),
			.FieldObjectId = OutputAudio,
		});
		nosEngine.ObjectAPI->CreateCompositeObject(NOS_NAME(AudioPacket::GetFullyQualifiedName()), fields.data(), fields.size(), &out.GetStorage());
		
		NOS_SOFT_CHECK(out, "Failed to create output AudioPacket object");

		// Set output pin values
		SetPinObject(NOS_NAME("AudioPacket"), out);

		return NOS_RESULT_SUCCESS;
	}

	TypedObjectRef<sys::vulkan::Buffer> OutputAudio;
	uint64_t AccumulatedSampleNumerator; // Accumulates fractional samples as integer numerator
	uint64_t LastSampleTime = 0;
	float LastSampleTimeFract = 0.0f;
};

nosResult RegisterAudioPlayerNode(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("AudioPlayer"), AudioPlayerNode, fn);
	return NOS_RESULT_SUCCESS;
}
} // namespace nos::audio
