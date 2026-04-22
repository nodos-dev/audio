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
		// Drop any audio that queued up between Capture->Start() and this
		// first consumer tick. Without this, the consumer would forever play
		// from the back of a full ring buffer, running the apparent latency
		// ceiling (~100ms after the in-read trim) instead of the floor.
		if (Capture)
			Capture->DiscardBufferedSamples();
	}

	// Deliberately no OnPathStop override: ScreenCaptureKit's Stop() is a full
	// stream teardown (not a pause), so any Stop here would leave the stream
	// dead across the routine OnPathStop → OnPathStart cycles that happen on
	// graph load and downstream reconfiguration. The backend stays running
	// until Active flips off or the node is destroyed, and the status message
	// is kept in sync by ExecuteNode below rather than being cleared here —
	// clearing with ClearNodeStatusMessages without also resetting the
	// LastStatusMessage mirror used to leave the node with no visible status
	// after a path restart.

	nosResult ExecuteNode(NodeExecuteParams const& pins) override
	{
		// Read Active straight from the pin rather than relying on a watcher-
		// backed mirror: on graph load the first ExecuteNode can fire before
		// the watcher has propagated the saved `true`, which left the node
		// inert until the user toggled the pin.
		const bool active = *pins.GetPinValue<bool>(NOS_NAME("Active"));
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
		if (active && (NeedsReinitialize || !Capture))
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
				SetPinValue(NOS_NAME("Active"), false);
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
				SetPinValue(NOS_NAME("Active"), false);
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
				SetPinValue(NOS_NAME("Active"), false);
				return NOS_RESULT_FAILED;
			}

			NeedsReinitialize = false;
		}
		else if (!active && Capture)
		{
			Capture->Stop();
			Capture.reset();
			SetNodeStatusMessageIfChanged("System audio input inactive", fb::NodeStatusMessageType::WARNING);
		}

		// Steady-state status, re-posted every frame while capture is live.
		// Posting here (instead of once inside the init branch) means the
		// message survives path restarts: if OnPathStop or an external clear
		// wipes the node status, the very next ExecuteNode repaints it, and
		// the SetNodeStatusMessageIfChanged guard suppresses spam in the
		// common case where the string hasn't changed.
		if (active && Capture)
		{
			std::string deviceMsg = "Capturing system audio";
			if (!Capture->GetDeviceName().empty())
				deviceMsg += " (" + Capture->GetDeviceName() + ")";
			SetNodeStatusMessageIfChanged(deviceMsg, fb::NodeStatusMessageType::INFO);
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
			// DOWNLOAD flips VMA from HOST_ACCESS_SEQUENTIAL_WRITE (which lets
			// it pick write-combined memory) to HOST_ACCESS_RANDOM (cached
			// memory). The engine already requests VK_MEMORY_PROPERTY_HOST_-
			// COHERENT_BIT in either case, so host↔device coherence is fine
			// without this flag — but the buffer is ALSO read by a consumer
			// node (AudioOscilloscope) running on a different engine runner
			// thread. Write-combined memory doesn't participate in normal
			// CPU cache coherence between cores, so the consumer's reads
			// could miss the producer's writes until some unrelated sync
			// event flushed things. Cached memory fixes this, at the cost
			// of slightly slower sequential writes (unmeasurable at audio
			// sample volumes).
			audioBufferDesc.MemoryFlags = nosMemoryFlags(NOS_MEMORY_FLAGS_HOST_VISIBLE |
														 NOS_MEMORY_FLAGS_DOWNLOAD |
														 NOS_MEMORY_FLAGS_FORCE_HOST_MEMORY);
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

		if (active && Capture)
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
