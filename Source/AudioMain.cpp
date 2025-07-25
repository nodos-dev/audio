// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>
#include <nosVulkanSubsystem/nosVulkanSubsystem.h>

NOS_INIT_WITH_MIN_REQUIRED_MINOR(9)
NOS_VULKAN_INIT()

NOS_BEGIN_IMPORT_DEPS()
	NOS_VULKAN_IMPORT()
NOS_END_IMPORT_DEPS()

namespace nos::audio
{
enum class Nodes : int
{
	SineWave,
	ReadAudioFile,
	AudioPlayer,
	Count
};

nosResult RegisterSineWaveNode(nosNodeFunctions*);
nosResult RegisterReadAudioFileNode(nosNodeFunctions*);
nosResult RegisterAudioPlayerNode(nosNodeFunctions*);

struct AudioPluginFunctions : nos::PluginFunctions
{
	using PluginFunctions::PluginFunctions;
	nosResult ExportNodeFunctions(size_t& outSize, nosNodeFunctions** outList) override
	{
		outSize = static_cast<size_t>(Nodes::Count);
		if (!outList)
			return NOS_RESULT_SUCCESS;

		NOS_RETURN_ON_FAILURE(RegisterSineWaveNode(outList[(int)Nodes::SineWave]))
		NOS_RETURN_ON_FAILURE(RegisterReadAudioFileNode(outList[(int)Nodes::ReadAudioFile]))
		NOS_RETURN_ON_FAILURE(RegisterAudioPlayerNode(outList[(int)Nodes::AudioPlayer]))
		return NOS_RESULT_SUCCESS;
	}
};
NOS_EXPORT_PLUGIN_FUNCTIONS(AudioPluginFunctions)
} // namespace nos::audio
