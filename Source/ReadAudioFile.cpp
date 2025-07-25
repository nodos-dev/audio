// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>

#include <nosVulkanSubsystem/Helpers.hpp>
#include <cmath>

#include "Audio_generated.h"

#include <AudioFile.h>

namespace nos::audio
{
struct ReadAudioFileNode : NodeContext
{
	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		auto pins = nos::NodeExecuteParams(params);
		AudioFile<int32_t> audioFile{};
		if (!audioFile.load(pins.GetPinData<const char*>(NOS_NAME("Path"))))
		{
			nosEngine.LogE("Failed to load audio file: %s", pins.GetPinData<const char*>(NOS_NAME("Path")));
			return NOS_RESULT_FAILED;
		}
		std::stringstream ss;
		ss << "|======================================|" << std::endl
		   << "Num Channels: " << audioFile.getNumChannels() << std::endl
		   << "Num Samples Per Channel: " << audioFile.getNumSamplesPerChannel() << std::endl
		   << "Sample Rate: " << audioFile.getSampleRate() << std::endl
		   << "Bit Depth: " << audioFile.getBitDepth() << std::endl
		   << "Length in Seconds: " << audioFile.getLengthInSeconds() << std::endl
		   << "|======================================|" << std::endl;
		nosEngine.LogI("%s", ss.str().c_str());

		auto channelCount = *pins.GetPinData<uint32_t>(NOS_NAME("ChannelCount"));

		auto bufOpt = vkss::Resource::Create(
			nosBufferInfo{
				.Size = audioFile.getNumSamplesPerChannel() * channelCount * sizeof(int32_t),
				.Usage = NOS_BUFFER_USAGE_TRANSFER_SRC,
				.MemoryFlags = nosMemoryFlags(NOS_MEMORY_FLAGS_HOST_VISIBLE | NOS_MEMORY_FLAGS_FORCE_HOST_MEMORY),
			},
			"Audio File Buffer");
		if (!bufOpt)
		{
			nosEngine.LogE("Failed to create buffer for audio file: %s",
						   pins.GetPinData<const char*>(NOS_NAME("Path")));
			return NOS_RESULT_FAILED;
		}
		auto& buf = *bufOpt;
		int32_t* data = reinterpret_cast<int32_t*>(nosVulkan->Map(&buf));
		for (size_t sample = 0; sample < audioFile.getNumSamplesPerChannel(); ++sample)
		{
			for (size_t channel = 0; channel < audioFile.getNumChannels(); ++channel)
			{
				int32_t value = audioFile.samples[channel][sample];
				data[sample * channelCount + channel] = value << 8;
			}
		}

		SetPinValue(NOS_NAME("Out"), buf.ToPinData());
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
