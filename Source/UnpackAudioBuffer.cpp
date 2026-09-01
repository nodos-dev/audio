// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>

#include <nosSysVulkan/Helpers.hpp>
#include <cmath>
#include <cstring>
#include <string>

#include "NodeErrors.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "nosAudio/Audio_generated.h"
#include "nosAudio/AudioConversions.hpp"

namespace nos::audio
{
// Sample rate, sample count and channel count sit in front of the samples.
constexpr uint64_t PREFIX_SIZE = 3 * sizeof(int32_t);

// Roughly two seconds at 60 fps. A producer that has nothing to send writes an
// empty packet, so waiting this long keeps the odd gap between packets from
// flickering a warning on and off.
constexpr uint32_t SILENT_EXECUTIONS_BEFORE_WARNING = 120;

struct UnpackAudioBuffer : NodeContext
{
	// What the node is complaining about, so each problem can be raised and
	// cleared on its own.
	enum class ErrorType
	{
		Input,
		Header,
		Output,
	};

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

		// No waiting on the GPU here. The command such a wait submits joins the
		// batch that already carries this frame's wait on the app's semaphore, and
		// the app only signals that once the path has run, so waiting for it here
		// deadlocks the two processes against each other. Every other node in this
		// plugin maps its input straight away and lets the engine order the copy.

		// Everything below is read out of a buffer another process owns, so it
		// has to be checked against the bytes that are really there. A buffer
		// nobody has written yet reads as garbage.
		auto inputInfo = sys::vulkan::GetResourceInfo(inputBuf);
		if (!inputInfo)
		{
			Errors.Set(ErrorType::Input, fb::NodeStatusMessageType::FAILURE, "No audio buffer to unpack");
			PublishSilence();
			return NOS_RESULT_SUCCESS;
		}

		const uint64_t inputSize = inputInfo->Size;
		if (inputSize < PREFIX_SIZE)
		{
			Errors.Set(ErrorType::Input, fb::NodeStatusMessageType::FAILURE, "Audio buffer is too small to hold a packet header",
				"It holds " + std::to_string(inputSize) + " bytes, the header alone needs " + std::to_string(PREFIX_SIZE) + ".");
			PublishSilence();
			return NOS_RESULT_SUCCESS;
		}

		// Map the input buffer to read the prefixed data
		uint8_t* inputData = nosVulkan->Map(inputBuf);
		if (!inputData)
		{
			Errors.Set(ErrorType::Input, fb::NodeStatusMessageType::FAILURE, "Audio buffer cannot be read",
				"The buffer has to be host visible for this node to unpack it.");
			PublishSilence();
			return NOS_RESULT_SUCCESS;
		}
		Errors.Clear(ErrorType::Input);

		// Read the prefix data: sampleRate (4 bytes), numSamples (4 bytes), channelCount (4 bytes)
		int32_t sampleRate = *reinterpret_cast<int32_t*>(inputData);
		int32_t numSamples = *reinterpret_cast<int32_t*>(inputData + 4);
		int32_t channelCount = *reinterpret_cast<int32_t*>(inputData + 8);

		// An empty packet is what a producer with nothing to send writes, and what
		// a buffer nobody has written yet reads as. Say so once it lasts.
		if (sampleRate == 0 && numSamples == 0 && channelCount == 0)
		{
			if (++SilentExecutions >= SILENT_EXECUTIONS_BEFORE_WARNING)
				Errors.Set(ErrorType::Header, fb::NodeStatusMessageType::WARNING, "No audio is arriving",
					"The buffer holds an empty packet. Whatever feeds it has not written audio for a while.");
			PublishSilence();
			return NOS_RESULT_SUCCESS;
		}
		SilentExecutions = 0;

		if (sampleRate <= 0 || numSamples <= 0 || channelCount <= 0)
		{
			Errors.Set(ErrorType::Header, fb::NodeStatusMessageType::WARNING, "Audio packet header does not make sense",
				DescribeHeader(sampleRate, numSamples, channelCount));
			PublishSilence();
			return NOS_RESULT_SUCCESS;
		}

		const uint64_t sampleCount = uint64_t(numSamples) * uint64_t(channelCount);
		const uint64_t neededSize = PREFIX_SIZE + sampleCount * sizeof(float);
		if (neededSize > inputSize)
		{
			Errors.Set(ErrorType::Header, fb::NodeStatusMessageType::WARNING, "Audio packet claims more samples than the buffer holds",
				DescribeHeader(sampleRate, numSamples, channelCount) + " That needs " + std::to_string(neededSize) +
					" bytes, the buffer holds " + std::to_string(inputSize) + ".");
			PublishSilence();
			return NOS_RESULT_SUCCESS;
		}
		Errors.Clear(ErrorType::Header);

		// Kept so silence can be published in the same shape when the next packet
		// turns out to be unreadable.
		LastSampleRate = sampleRate;
		LastNumSamples = numSamples;
		LastChannelCount = channelCount;

		// Point to the float audio data after the prefix
		float* inputAudioSamples = reinterpret_cast<float*>(inputData + PREFIX_SIZE);
		Publish(sampleRate, numSamples, channelCount, inputAudioSamples);

		return NOS_RESULT_SUCCESS;
	}

	// Writes one packet to the Audio pin: the samples converted, or silence when
	// there are none to convert. Everything that reaches the pin goes through here.
	bool Publish(int32_t sampleRate, int32_t numSamples, int32_t channelCount, float const* samples)
	{
		const uint64_t sampleCount = uint64_t(numSamples) * uint64_t(channelCount);

		// Create or resize audio buffer only if needed (with 1.1x headroom to avoid frequent reallocations)
		size_t requiredBufferSize = std::max(size_t(sampleCount * sizeof(uint32_t)), size_t(1000));
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
			{
				Errors.Set(ErrorType::Output, fb::NodeStatusMessageType::FAILURE, "Cannot create the unpacked audio buffer",
					"Asked for " + std::to_string(audioBufferDesc.Size) + " bytes.");
				return false;
			}
		}

		int32_t* outputAudioSamples = reinterpret_cast<int32_t*>(nosVulkan->Map(OutputAudioBuffer));
		if (!outputAudioSamples)
		{
			Errors.Set(ErrorType::Output, fb::NodeStatusMessageType::FAILURE, "Cannot write the unpacked audio buffer");
			return false;
		}
		Errors.Clear(ErrorType::Output);

		if (samples)
		{
			// Convert float samples to 24-bit MSB int32 format
			for (uint64_t i = 0; i < sampleCount; ++i)
			{
				// Clamp float sample to [-1.0, 1.0] range
				float floatSample = std::max(-1.0f, std::min(1.0f, samples[i]));

				// Store as 32-bit with 24-bit sample in MSB (shift left by 8 bits)
				outputAudioSamples[i] = FloatToShiftedInt24(floatSample);
			}
		}
		else
			std::memset(outputAudioSamples, 0, sampleCount * sizeof(int32_t));

		AudioPacketDescriptor audioPacketDesc(
			sampleRate, numSamples, BitDepth::AUDIO_BIT_DEPTH_24_BIT, 4, channelCount);

		auto newDescObj = PrimitiveObjectRef::Create(NOS_NAME("nos.audio.AudioPacketDescriptor"), nos::Buffer::From(audioPacketDesc));

		std::unordered_map<nos::Name, nos::ObjectRef> audioPacketFields;
		audioPacketFields[NOS_NAME("desc")] = newDescObj.value_or(ObjectRef());
		audioPacketFields[NOS_NAME("buffer")] = OutputAudioBuffer;
		auto audioPacket = CompositeObjectRef::Create(NOS_NAME("nos.audio.AudioPacket"), audioPacketFields);
		if (!audioPacket)
			return false;

		SetPinObject(NOS_NAME("Audio"), *audioPacket);
		return true;
	}

	// The shape of the last packet that made sense, with nothing in it. Leaving the
	// pin alone instead hands the reader the packet before this one, and it plays
	// that on a loop for as long as the producer stays quiet. Before any packet has
	// arrived there is no shape to be silent in, so the pin waits for a first one.
	void PublishSilence()
	{
		if (LastSampleRate > 0)
			Publish(LastSampleRate, LastNumSamples, LastChannelCount, nullptr);
	}

	static std::string DescribeHeader(int32_t sampleRate, int32_t numSamples, int32_t channelCount)
	{
		return "Sample rate " + std::to_string(sampleRate) + ", " + std::to_string(numSamples) + " samples, " +
			std::to_string(channelCount) + " channels.";
	}

	NodeErrors<ErrorType> Errors{*this};
	uint32_t SilentExecutions = 0;
	int32_t LastSampleRate = 0;
	int32_t LastNumSamples = 0;
	int32_t LastChannelCount = 0;
	TypedObjectRef<sys::vulkan::Buffer> OutputAudioBuffer;
};

nosResult RegisterUnpackAudioBufferNode(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("UnpackAudioBuffer"), UnpackAudioBuffer, fn);
	return NOS_RESULT_SUCCESS;
}
} // namespace nos::audio
