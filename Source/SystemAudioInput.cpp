// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>

#include <nosSysVulkan/Helpers.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include "nosAudio/Audio_generated.h"
#include "nosAudio/AudioConversions.hpp"

#include "SystemAudioCapture.h"

namespace nos::audio
{

struct SystemAudioInputNode : NodeContext
{
	void SetNodeStatusMessageIfChanged(const std::string& message, fb::NodeStatusMessageType type)
	{
		if (LastStatusMessage != message)
		{
			SetNodeStatusMessage(message, type);
			LastStatusMessage = message;
		}
	}

	nosResult OnCreate(nosFbNodePtr) override
	{
		AddPinValueWatcher<bool>(NOS_NAME("Active"),
								 [this](const bool* newVal, std::optional<const bool*> /*oldVal*/) {
									 Active = *newVal;
									 // Status is owned by ExecuteNode so it stays consistent with the
									 // capture backend state. Writing here too races with the frame
									 // loop and flaps the node status between "active" and the
									 // capture-backend messages on every pin-value update (including
									 // the one that fires during graph load).
								 });

		AddPinValueWatcher<uint32_t>(NOS_NAME("SampleRate"),
									 [this](const uint32_t* newVal, std::optional<const uint32_t*> oldVal) {
										 if (!oldVal || *newVal != **oldVal)
											 NeedsReinitialize = true;
									 });

		AddPinValueWatcher<uint8_t>(NOS_NAME("ChannelCount"),
									[this](const uint8_t* newVal, std::optional<const uint8_t*> oldVal) {
										if (!oldVal || *newVal != **oldVal)
											NeedsReinitialize = true;
									});

		return NOS_RESULT_SUCCESS;
	}

	~SystemAudioInputNode() override
	{
		if (Capture)
			Capture->Stop();
	}

	void OnPathStart() override
	{
		AccumulatedSampleNumerator = 0;
		CurrentSampleIndex = 0;

		// Don't force a re-init here. If the engine restarts paths frequently
		// (e.g. downstream scheduler changes, other nodes calling SendPathRestart),
		// destroying and recreating Capture every round makes the node post the
		// same "Audio capture is ready …" status message over and over. The
		// SampleRate / ChannelCount pin watchers already set NeedsReinitialize
		// when the capture format actually changes; anything else just needs a
		// cheap Start() to resume a backend we paused in OnPathStop.
		if (Active && Capture)
			Capture->Start();

		// Don't clear LastStatusMessage either — keeping it means the guard in
		// SetNodeStatusMessageIfChanged suppresses a same-string repost from
		// the first post-restart frame, which is the source of the flap.
	}

	void OnPathStop() override
	{
		if (Capture)
			Capture->Stop();
		ClearNodeStatusMessages();
	}

	nosResult ExecuteNode(NodeExecuteParams const& pins) override
	{
		auto& sampleRate = *pins.GetPinValue<uint32_t>(NOS_NAME("SampleRate"));
		auto& channelCount = *pins.GetPinValue<uint8_t>(NOS_NAME("ChannelCount"));
		auto& gain = *pins.GetPinValue<float>(NOS_NAME("Gain"));

		if (pins.TimingMode != NOS_EXECUTION_TIMING_MODE_FIXED_STEP)
		{
			SetNodeStatusMessageIfChanged("Unsupported timing mode", fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_FAILED;
		}

		if (pins.FixedStepTiming.DeltaSeconds.y == 0)
		{
			SetNodeStatusMessageIfChanged("Invalid timing values", fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_FAILED;
		}

		// (Re)create the backend whenever Active flips on or the requested
		// format changes. A null Capture after this branch means the platform
		// has no backend compiled in — we emit silence + a status message.
		if (Active && (NeedsReinitialize || !Capture))
		{
			if (Capture)
			{
				Capture->Stop();
				Capture.reset();
			}

			Capture = ISystemAudioCapture::Create();
			if (!Capture)
			{
				SetNodeStatusMessageIfChanged("System audio input is not supported on this platform",
											  fb::NodeStatusMessageType::FAILURE);
				Active = false;
				return NOS_RESULT_FAILED;
			}

			if (!Capture->Initialize(sampleRate, channelCount))
			{
				const auto& err = Capture->GetLastError();
				SetNodeStatusMessageIfChanged(
					err.empty() ? std::string("Failed to initialize system audio capture")
								: "Failed to initialize system audio capture: " + err,
					fb::NodeStatusMessageType::FAILURE);
				Capture.reset();
				Active = false;
				return NOS_RESULT_FAILED;
			}

			if (!Capture->Start())
			{
				const auto& err = Capture->GetLastError();
				SetNodeStatusMessageIfChanged(
					err.empty() ? std::string("Failed to start system audio capture")
								: "Failed to start system audio capture: " + err,
					fb::NodeStatusMessageType::FAILURE);
				Capture.reset();
				Active = false;
				return NOS_RESULT_FAILED;
			}

			NeedsReinitialize = false;
			std::string deviceMsg = "Audio capture is ready";
			if (!Capture->GetDeviceName().empty())
				deviceMsg += " (" + Capture->GetDeviceName() + ")";
			SetNodeStatusMessageIfChanged(deviceMsg, fb::NodeStatusMessageType::INFO);
		}
		else if (!Active && Capture)
		{
			Capture->Stop();
			Capture.reset();
			SetNodeStatusMessageIfChanged("System audio input inactive", fb::NodeStatusMessageType::WARNING);
		}

		const uint64_t deltaNumerator = pins.FixedStepTiming.DeltaSeconds.x;
		const uint64_t deltaDenominator = pins.FixedStepTiming.DeltaSeconds.y;

		AccumulatedSampleNumerator += deltaNumerator * static_cast<uint64_t>(sampleRate);
		const uint32_t numSamples = static_cast<uint32_t>(AccumulatedSampleNumerator / deltaDenominator);
		AccumulatedSampleNumerator %= deltaDenominator;

		// Create or grow the audio buffer only when strictly necessary; 1.1x
		// headroom amortises reallocations across small timing fluctuations.
		const size_t requiredBufferSize = static_cast<size_t>(numSamples) * sizeof(uint32_t) * channelCount;
		size_t allocatedBufferSize = 0;
		if (AudioPacketBuffer)
		{
			if (auto bufferInfo = sys::vulkan::GetResourceInfo(AudioPacketBuffer))
				allocatedBufferSize = bufferInfo->Size;
			else
				NOS_SOFT_CHECK(false, "Failed to get buffer info for existing audio buffer");
		}

		if (!AudioPacketBuffer || requiredBufferSize > allocatedBufferSize)
		{
			AudioPacketBuffer = {};
			const size_t newBufferSize = static_cast<size_t>(requiredBufferSize * 1.1f);

			nosBufferInfo audioBufferDesc = {};
			audioBufferDesc.Size = static_cast<uint32_t>(newBufferSize);
			audioBufferDesc.Usage = nosBufferUsage(NOS_BUFFER_USAGE_STORAGE_BUFFER | NOS_BUFFER_USAGE_TRANSFER_DST |
												   NOS_BUFFER_USAGE_TRANSFER_SRC);
			audioBufferDesc.MemoryFlags =
				nosMemoryFlags(NOS_MEMORY_FLAGS_HOST_VISIBLE | NOS_MEMORY_FLAGS_FORCE_HOST_MEMORY);
			audioBufferDesc.ElementType = NOS_BUFFER_ELEMENT_TYPE_INT32;

			AudioPacketBuffer = sys::vulkan::CreateBuffer(audioBufferDesc, "SystemAudioInput AudioBuffer");
			if (!AudioPacketBuffer)
			{
				SetNodeStatusMessageIfChanged("Failed to create audio buffer", fb::NodeStatusMessageType::FAILURE);
				return NOS_RESULT_FAILED;
			}
		}

		auto* audioSamples = reinterpret_cast<int32_t*>(nosVulkan->Map(AudioPacketBuffer));
		if (!audioSamples)
		{
			SetNodeStatusMessageIfChanged("Failed to map audio buffer", fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_FAILED;
		}

		if (Active && Capture)
		{
			// Don't update the status message every frame based on whether
			// this single frame delivered audio — ReadSamples flips true/false
			// at the rate of buffer fills, which causes the editor's node
			// status area to spam updates. The "ready" message posted after
			// Initialize/Start stays put; transitions (inactive, failure) are
			// the only things that republish.
			Capture->ReadSamples(audioSamples, numSamples, channelCount, gain);
		}
		else
		{
			for (uint32_t i = 0; i < numSamples * channelCount; ++i)
				audioSamples[i] = 0;
		}

		CurrentSampleIndex += numSamples;

		AudioPacketDescriptor audioPacketDesc(
			sampleRate, numSamples, BitDepth::AUDIO_BIT_DEPTH_24_BIT, sizeof(int32_t), channelCount);

		ObjectRef outDesc{};
		nosEngine.ObjectAPI->CreatePrimitiveObject(NOS_NAME(AudioPacketDescriptor::GetFullyQualifiedName()),
												   nos::Buffer::From(audioPacketDesc),
												   &outDesc.GetStorage());

		ObjectRef out{};
		std::vector<nosCompositeObjectField> fields;
		fields.push_back(nosCompositeObjectField{
			.FieldName = NOS_NAME("desc"),
			.FieldObjectId = outDesc,
		});
		fields.push_back(nosCompositeObjectField{
			.FieldName = NOS_NAME("buffer"),
			.FieldObjectId = AudioPacketBuffer,
		});
		nosEngine.ObjectAPI->CreateCompositeObject(NOS_NAME(AudioPacket::GetFullyQualifiedName()),
												   fields.data(),
												   fields.size(),
												   &out.GetStorage());

		NOS_SOFT_CHECK(out, "Failed to create output AudioPacket object");
		SetPinObject(NOS_NAME("AudioPacket"), out);

		return NOS_RESULT_SUCCESS;
	}

	TypedObjectRef<sys::vulkan::Buffer> AudioPacketBuffer;
	uint64_t AccumulatedSampleNumerator = 0;
	uint64_t CurrentSampleIndex = 0;
	bool Active = false;
	bool NeedsReinitialize = false;
	std::string LastStatusMessage;
	std::unique_ptr<ISystemAudioCapture> Capture;
};

nosResult RegisterSystemAudioInputNode(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("SystemAudioInput"), SystemAudioInputNode, fn);
	return NOS_RESULT_SUCCESS;
}
} // namespace nos::audio
