// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>

#include <nosSysVulkan/Helpers.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nosAudio/Audio_generated.h"
#include "nosAudio/AudioConversions.hpp"

#include "SystemAudioCapture.h"

namespace nos::audio
{
// The node captures at the device's native format and passes it straight
// through, so there are no rate/channel input pins and it requests no specific
// format from the backend. These defaults only label the empty packet emitted
// before the first captured frame reveals the real device format.
constexpr uint32_t DEFAULT_SAMPLE_RATE = 48000;
constexpr uint8_t DEFAULT_CHANNEL_COUNT = 2;

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

	// Post several status lines at once (the editor renders each as its own
	// row). The change guard keys off the newline-joined text so a steady
	// stream of identical multi-line statuses doesn't spam updates.
	void SetNodeStatusMessagesIfChanged(const std::vector<fb::TNodeStatusMessage>& messages)
	{
		std::string key;
		for (const auto& m : messages)
		{
			key += m.text;
			key += '\n';
		}
		if (LastStatusMessage != key)
		{
			SetNodeStatusMessages(messages);
			LastStatusMessage = key;
		}
	}

	~SystemAudioInputNode() override
	{
		if (Capture)
			Capture->Stop();
	}

	void OnPathStart() override
	{
		// Drop whatever queued up between Capture->Start() and this first tick
		// so the first packet after a restart carries one tick's worth of audio
		// rather than the whole accumulated backlog as a single burst.
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
		const float gain = *pins.GetPinValue<float>(NOS_NAME("Gain"));
		// No fixed-step timing is needed: this node ships exactly what the device
		// produced this tick, at the device's own format, so it is agnostic to
		// the graph's timing mode.

		// Create the backend when Active flips on. A null Capture after this
		// branch means the platform has no backend compiled in.
		if (active && !Capture)
		{
			Capture = ISystemAudioCapture::Create();
			if (!Capture)
			{
				SetNodeStatusMessageIfChanged("System audio input is not supported on this platform",
											  fb::NodeStatusMessageType::FAILURE);
				SetPinValue(NOS_NAME("Active"), false);
				return NOS_RESULT_FAILED;
			}

			if (!Capture->Initialize())
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
		}
		else if (!active && Capture)
		{
			Capture->Stop();
			Capture.reset();
		}

		// Pull everything captured since the last tick. deviceRate / deviceChannels
		// report the format the OS actually delivered (0 until the first frame
		// arrives). DrainScratch is reused across ticks so the steady state does
		// not allocate.
		uint32_t deviceRate = 0;
		uint8_t deviceChannels = 0;
		if (active && Capture)
			Capture->DrainSamples(DrainScratch, deviceRate, deviceChannels);
		else
			DrainScratch.clear();

		// Status. Re-posted every tick while live (and on inactivity) so it
		// survives path restarts that clear the node status; the change guard
		// suppresses spam when the strings haven't moved.
		if (active && Capture)
		{
			std::vector<fb::TNodeStatusMessage> messages;
			messages.push_back({{}, "Capturing system audio", fb::NodeStatusMessageType::INFO});
			if (!Capture->GetDeviceName().empty())
				messages.push_back({{}, Capture->GetDeviceName(), fb::NodeStatusMessageType::INFO});
			if (deviceRate != 0)
				messages.push_back({{},
									std::to_string(deviceRate) + " Hz, " + std::to_string(deviceChannels) + " ch",
									fb::NodeStatusMessageType::INFO});
			SetNodeStatusMessagesIfChanged(messages);
		}
		else
		{
			SetNodeStatusMessageIfChanged("System audio input inactive", fb::NodeStatusMessageType::WARNING);
		}

		// Output format mirrors the device. Before the first frame arrives the
		// format is unknown; fall back to the defaults so the (empty) packet is
		// still well-formed.
		const uint32_t outRate = deviceRate != 0 ? deviceRate : DEFAULT_SAMPLE_RATE;
		const uint8_t outChannels = deviceChannels != 0 ? deviceChannels : DEFAULT_CHANNEL_COUNT;
		const uint32_t numSamples = outChannels != 0 ? static_cast<uint32_t>(DrainScratch.size() / outChannels) : 0;

		// Create or grow the audio buffer only when strictly necessary; 1.1x
		// headroom amortises reallocations across packet-size fluctuations. Keep
		// room for at least one frame so a zero-sample tick still has a valid
		// buffer object to attach to the packet.
		const size_t requiredBufferSize =
			std::max<size_t>(static_cast<size_t>(numSamples), 1) * sizeof(uint32_t) * outChannels;
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

		// Apply gain and pack the device's Float32 samples into shifted int24,
		// straight through at the device's own rate and channel layout.
		const uint32_t totalSamples = numSamples * outChannels;
		for (uint32_t i = 0; i < totalSamples; ++i)
			audioSamples[i] = FloatToShiftedInt24(DrainScratch[i] * gain);

		AudioPacketDescriptor audioPacketDesc(
			outRate, numSamples, BitDepth::AUDIO_BIT_DEPTH_24_BIT, sizeof(int32_t), outChannels);

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
	std::vector<float> DrainScratch;
	std::string LastStatusMessage;
	std::unique_ptr<ISystemAudioCapture> Capture;
};

nosResult RegisterSystemAudioInputNode(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("SystemAudioInput"), SystemAudioInputNode, fn);
	return NOS_RESULT_SUCCESS;
}
} // namespace nos::audio
