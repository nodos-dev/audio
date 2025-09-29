// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>

#include <nosVulkanSubsystem/Helpers.hpp>
#include <cmath>

#include "Audio_generated.h"

// AudioFile.h defines it without checking if it was defined before.
#ifdef NOMINMAX
#undef NOMINMAX
#endif
#include <AudioFile.h>

namespace nos::audio
{
struct ReadAudioFileNode : NodeContext
{
	nosResult ExecuteNode(NodeExecuteParams const& pins) override
	{
		AudioFile<int32_t> audioFile{};
		auto path = pins.GetPinData<const char*>(NOS_NAME("Path"));
		if (!audioFile.load(path))
		{
			nosEngine.LogE("Failed to load audio file: %s", path);
			return NOS_RESULT_FAILED;
		}
		std::stringstream ss;
		ss << "Audio file read from " << path << std::endl
		   << "\t|======================================| " << std::endl
		   << "\t| Num Channels: " << audioFile.getNumChannels() << std::endl
		   << "\t| Num Samples Per Channel: " << audioFile.getNumSamplesPerChannel() << std::endl
		   << "\t| Sample Rate: " << audioFile.getSampleRate() << std::endl
		   << "\t| Bit Depth: " << audioFile.getBitDepth() << std::endl
		   << "\t| Length in Seconds: " << audioFile.getLengthInSeconds() << std::endl
		   << "\t|======================================|" << std::endl;
		nosEngine.LogI("%s", ss.str().c_str());

		auto channelCount = *pins.GetPinData<uint32_t>(NOS_NAME("ChannelCount"));

		auto bufferObject = sys::vulkan::CreateBuffer(
			nosBufferInfo{
				.Size = uint32_t(audioFile.getNumSamplesPerChannel() * channelCount * sizeof(int32_t)),
				.Usage = NOS_BUFFER_USAGE_TRANSFER_SRC,
				.MemoryFlags = nosMemoryFlags(NOS_MEMORY_FLAGS_HOST_VISIBLE | NOS_MEMORY_FLAGS_FORCE_HOST_MEMORY),
			},
			"Audio File Buffer");
		if (!bufferObject)
		{
			nosEngine.LogE("Failed to create buffer for audio file: %s",
						   pins.GetPinData<const char*>(NOS_NAME("Path")));
			return NOS_RESULT_FAILED;
		}
		int32_t* data = reinterpret_cast<int32_t*>(nosVulkan->Map(bufferObject));
		for (size_t sample = 0; sample < audioFile.getNumSamplesPerChannel(); ++sample)
		{
			for (size_t channel = 0; channel < audioFile.getNumChannels(); ++channel)
			{
				int32_t value = audioFile.samples[channel][sample];
				data[sample * channelCount + channel] = value << 8;
			}
		}

		SetPinObject(NOS_NAME("Out"), bufferObject);
		AudioPacketDescriptor audioPacketDesc(audioFile.getSampleRate(),
											  audioFile.getNumSamplesPerChannel(),
											  BitDepth::AUDIO_BIT_DEPTH_24_BIT,
											  sizeof(int32_t),
											  channelCount);
		SetPinValue(NOS_NAME("OutAudioPacketDescriptor"), audioPacketDesc);
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterReadAudioFileNode(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("ReadAudioFile"), ReadAudioFileNode, fn);
	return NOS_RESULT_SUCCESS;
}
} // namespace nos::audio
