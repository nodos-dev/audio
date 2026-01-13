// Copyright MediaZ Teknoloji A.S. All Rights Reserved.


#define NOS_DISABLE_DEPRECATED 1

#include <Nodos/Plugin.hpp>

#include <nosSysVulkan/Helpers.hpp>
#include <cmath>
#include <chrono>
#include <sstream>

#include "nosAudio/Audio_generated.h"

// AudioFile.h defines it without checking if it was defined before.
#ifdef NOMINMAX
#undef NOMINMAX
#endif
#include <AudioFile.h>

namespace nos::audio
{

enum State
{
	Idle = 0,
	Loading = 1,
	Failed = 2,
};

struct ReadAudioFileNode : NodeContext
{
	decltype(std::chrono::high_resolution_clock::now()) TimeStarted = std::chrono::high_resolution_clock::now();

	void UpdateStatus(State newState, const char* path, const std::string& audioInfo = "")
	{
		switch(newState)
		{
		case State::Loading:
			TimeStarted = std::chrono::high_resolution_clock::now();
			SetNodeStatusMessage("Loading audio file: " + std::string(path), fb::NodeStatusMessageType::INFO);
			break;
		case State::Idle:
		{
			auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() -
																			TimeStarted)
						  .count();
			std::stringstream ss;
			std::filesystem::path fsPath = nos::Utf8ToPath(path);
			ss << "Audio file loaded: " << nos::PathToUtf8(fsPath.filename()) << " (" << dt << "ms)\n" << audioInfo;
			SetNodeStatusMessage(ss.str(), fb::NodeStatusMessageType::INFO);
			break;
		}
		case State::Failed:
		{
			SetNodeStatusMessage("Failed to load audio file: " + std::string(path), fb::NodeStatusMessageType::FAILURE);
			break;
		}
		}
	}

	nosResult ExecuteNode(NodeExecuteParams const& pins) override
	{
		AudioFile<int32_t> audioFile{};
		auto path = pins.GetPinData<const char*>(NOS_NAME("Path"));
		UpdateStatus(State::Loading, path);
		if (!audioFile.load(path))
		{
			nosEngine.LogE("Failed to load audio file: %s", path);
			UpdateStatus(State::Failed, path);
			return NOS_RESULT_FAILED;
		}
		
		std::stringstream audioInfoSS;
		audioInfoSS << "- Channel Count: " << audioFile.getNumChannels() << "\n"
					<< "- Sample Rate: " << audioFile.getSampleRate() << "\n"
					<< "- Bit Depth: " << audioFile.getBitDepth() << "\n"
					<< "- Duration (s): " << audioFile.getLengthInSeconds();
		std::string audioInfo = audioInfoSS.str();
		nosEngine.LogI("Audio file read from %s\n%s", path, audioInfo.c_str());

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
			UpdateStatus(State::Failed, path);
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

		AudioPacketDescriptor audioPacketDesc(audioFile.getSampleRate(),
											  audioFile.getNumSamplesPerChannel(),
											  BitDepth::AUDIO_BIT_DEPTH_24_BIT,
											  sizeof(int32_t),
											  channelCount);
		ObjectRef descObject{};
		nosEngine.ObjectAPI->CreatePrimitiveObject(NOS_NAME("nos.audio.AudioPacketDescriptor"), nos::Buffer::From(audioPacketDesc), &descObject.GetStorage());
		std::vector<nosCompositeObjectField> fields;
		fields.push_back(nosCompositeObjectField{
			.FieldName = NOS_NAME("desc"),
			.FieldObjectId = descObject,
		});
		fields.push_back(nosCompositeObjectField{
			.FieldName = NOS_NAME("buffer"),
			.FieldObjectId = bufferObject,
		});

		ObjectRef audioPacket{};
		nosEngine.ObjectAPI->CreateCompositeObject(NOS_NAME("nos.audio.AudioPacket"), fields.data(), fields.size(), &audioPacket.GetStorage());
		SetPinObject(NOS_NAME("FullAudio"), audioPacket);
		UpdateStatus(State::Idle, path, audioInfo);
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterReadAudioFileNode(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("ReadAudioFile"), ReadAudioFileNode, fn);
	return NOS_RESULT_SUCCESS;
}
} // namespace nos::audio
