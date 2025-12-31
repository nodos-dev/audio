// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>

#include <nosSysVulkan/Helpers.hpp>
#include <cmath>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

#include "Audio_generated.h"
#include "nosAudio/AudioConversions.hpp"

#ifdef _WIN32
#include <Windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <comdef.h>
#include <functiondiscoverykeys_devpkey.h>

// COM smart pointer helpers
_COM_SMARTPTR_TYPEDEF(IMMDeviceEnumerator, __uuidof(IMMDeviceEnumerator));
_COM_SMARTPTR_TYPEDEF(IMMDevice, __uuidof(IMMDevice));
_COM_SMARTPTR_TYPEDEF(IAudioClient, __uuidof(IAudioClient));
_COM_SMARTPTR_TYPEDEF(IAudioCaptureClient, __uuidof(IAudioCaptureClient));
#endif

namespace nos::audio
{

#ifdef _WIN32
class WASAPICapture
{
public:
	WASAPICapture() : IsCapturing(false), ShouldStop(false)
	{
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	}

	~WASAPICapture()
	{
		Stop();
		CoUninitialize();
	}

	bool Initialize(uint32_t sampleRate, uint8_t channelCount)
	{
		HRESULT hr;

		// Create device enumerator
		IMMDeviceEnumeratorPtr enumerator;
		hr = enumerator.CreateInstance(__uuidof(MMDeviceEnumerator));
		if (FAILED(hr))
			return false;

		// Get default audio endpoint (for loopback capture)
		IMMDevicePtr device;
		hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
		if (FAILED(hr))
			return false;

		// Get device name
		IPropertyStore* props = nullptr;
		hr = device->OpenPropertyStore(STGM_READ, &props);
		if (SUCCEEDED(hr))
		{
			PROPVARIANT varName;
			PropVariantInit(&varName);
			hr = props->GetValue(PKEY_Device_FriendlyName, &varName);
			if (SUCCEEDED(hr))
			{
				DeviceName = _com_util::ConvertBSTRToString(varName.bstrVal);
				PropVariantClear(&varName);
			}
			props->Release();
		}

		// Activate audio client
		hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&AudioClient);
		if (FAILED(hr))
			return false;

		// Get the mix format
		WAVEFORMATEX* mixFormat = nullptr;
		hr = AudioClient->GetMixFormat(&mixFormat);
		if (FAILED(hr))
			return false;

		// Initialize audio client for loopback capture
		hr = AudioClient->Initialize(
			AUDCLNT_SHAREMODE_SHARED,
			AUDCLNT_STREAMFLAGS_LOOPBACK,
			10000000, // 1 second buffer
			0,
			mixFormat,
			nullptr);

		if (FAILED(hr))
		{
			CoTaskMemFree(mixFormat);
			return false;
		}

		// Store format info
		SourceSampleRate = mixFormat->nSamplesPerSec;
		SourceChannelCount = mixFormat->nChannels;
		TargetSampleRate = sampleRate;
		TargetChannelCount = channelCount;

		CoTaskMemFree(mixFormat);

		// Get capture client
		hr = AudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&CaptureClient);
		if (FAILED(hr))
			return false;

		return true;
	}

	bool Start()
	{
		if (IsCapturing)
			return true;

		if (!AudioClient)
			return false;

		HRESULT hr = AudioClient->Start();
		if (FAILED(hr))
			return false;

		IsCapturing = true;
		ShouldStop = false;
		CaptureThread = std::thread(&WASAPICapture::CaptureThreadFunc, this);

		return true;
	}

	void Stop()
	{
		if (!IsCapturing)
			return;

		ShouldStop = true;
		if (CaptureThread.joinable())
			CaptureThread.join();

		if (AudioClient)
			AudioClient->Stop();

		IsCapturing = false;
	}

	bool ReadSamples(int32_t* outBuffer, uint32_t numSamples, uint8_t targetChannels, float gain)
	{
		std::lock_guard<std::mutex> lock(BufferMutex);

		// Calculate how many source samples we need
		float sampleRateRatio = static_cast<float>(SourceSampleRate) / static_cast<float>(TargetSampleRate);
		uint32_t sourceSamplesNeeded = static_cast<uint32_t>(numSamples * sampleRateRatio);

		// If we don't have enough samples, fill with silence
		if (CapturedSamples.size() < sourceSamplesNeeded * SourceChannelCount)
		{
			for (uint32_t i = 0; i < numSamples * targetChannels; ++i)
				outBuffer[i] = 0;
			return false; // No audio available
		}

		// Resample and convert
		for (uint32_t i = 0; i < numSamples; ++i)
		{
			float sourceIndex = i * sampleRateRatio;
			uint32_t sourceIndexInt = static_cast<uint32_t>(sourceIndex);
			float frac = sourceIndex - sourceIndexInt;

			for (uint8_t ch = 0; ch < targetChannels; ++ch)
			{
				// Map target channel to source channel (handle mono/stereo conversions)
				uint8_t sourceChannel = (ch < SourceChannelCount) ? ch : 0;

				// Get samples for interpolation
				uint32_t idx1 = sourceIndexInt * SourceChannelCount + sourceChannel;
				uint32_t idx2 = std::min(idx1 + SourceChannelCount, static_cast<uint32_t>(CapturedSamples.size() - 1));

				if (idx1 < CapturedSamples.size() && idx2 < CapturedSamples.size())
				{
					float sample1 = CapturedSamples[idx1];
					float sample2 = CapturedSamples[idx2];
					float interpolated = sample1 + (sample2 - sample1) * frac;
					
					// Apply gain and convert to shifted int24
					interpolated *= gain;
					outBuffer[i * targetChannels + ch] = FloatToShiftedInt24(interpolated);
				}
				else
				{
					outBuffer[i * targetChannels + ch] = 0;
				}
			}
		}

		// Remove consumed samples
		uint32_t samplesToRemove = sourceSamplesNeeded * SourceChannelCount;
		if (samplesToRemove < CapturedSamples.size())
			CapturedSamples.erase(CapturedSamples.begin(), CapturedSamples.begin() + samplesToRemove);

		return true; // Audio successfully read
	}

	const std::string& GetDeviceName() const { return DeviceName; }

private:
	void CaptureThreadFunc()
	{
		while (!ShouldStop)
		{
			if (!CaptureClient)
				break;

			UINT32 packetLength = 0;
			HRESULT hr = CaptureClient->GetNextPacketSize(&packetLength);
			if (FAILED(hr))
				break;

			while (packetLength > 0)
			{
				BYTE* data = nullptr;
				UINT32 numFramesAvailable = 0;
				DWORD flags = 0;

				hr = CaptureClient->GetBuffer(&data, &numFramesAvailable, &flags, nullptr, nullptr);
				if (FAILED(hr))
					break;

				// Convert samples to float and store
				if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
				{
					float* floatData = reinterpret_cast<float*>(data);
					std::lock_guard<std::mutex> lock(BufferMutex);
					
					for (UINT32 i = 0; i < numFramesAvailable * SourceChannelCount; ++i)
					{
						CapturedSamples.push_back(floatData[i]);
					}

					// Limit buffer size to prevent unbounded growth (keep max 5 seconds)
					size_t maxSamples = SourceSampleRate * SourceChannelCount * 5;
					if (CapturedSamples.size() > maxSamples)
					{
						CapturedSamples.erase(CapturedSamples.begin(), 
											   CapturedSamples.begin() + (CapturedSamples.size() - maxSamples));
					}
				}

				CaptureClient->ReleaseBuffer(numFramesAvailable);

				hr = CaptureClient->GetNextPacketSize(&packetLength);
				if (FAILED(hr))
					break;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}

	IAudioClientPtr AudioClient;
	IAudioCaptureClientPtr CaptureClient;
	std::thread CaptureThread;
	std::atomic<bool> IsCapturing;
	std::atomic<bool> ShouldStop;
	std::vector<float> CapturedSamples;
	std::mutex BufferMutex;
	uint32_t SourceSampleRate = 0;
	uint8_t SourceChannelCount = 0;
	uint32_t TargetSampleRate = 0;
	uint8_t TargetChannelCount = 0;
	std::string DeviceName;
};
#endif

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
								 [this](const bool* newVal, std::optional<const bool*> oldVal) {
									 Active = *newVal;
									 if (Active)
										 SetNodeStatusMessageIfChanged("System audio input active", fb::NodeStatusMessageType::INFO);
									 else
										 SetNodeStatusMessageIfChanged("System audio input inactive", fb::NodeStatusMessageType::WARNING);
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
#ifdef _WIN32
		if (Capture)
		{
			Capture->Stop();
			Capture.reset();
		}
#endif
	}

	void OnPathStart() override
	{
		ClearNodeStatusMessages();
		AccumulatedSampleNumerator = 0;
		CurrentSampleIndex = 0;
		NeedsReinitialize = true;
		
		if (Active)
			SetNodeStatusMessageIfChanged("System audio input active", fb::NodeStatusMessageType::INFO);
		else
			SetNodeStatusMessageIfChanged("System audio input inactive", fb::NodeStatusMessageType::WARNING);
	}

	void OnPathStop() override
	{
#ifdef _WIN32
		if (Capture)
		{
			Capture->Stop();
		}
#endif
		ClearNodeStatusMessages();
	}

	nosResult ExecuteNode(NodeExecuteParams const& pins) override
	{
		auto& sampleRate = *pins.GetPinData<uint32_t>(NOS_NAME("SampleRate"));
		auto& channelCount = *pins.GetPinData<uint8_t>(NOS_NAME("ChannelCount"));
		auto& gain = *pins.GetPinData<float>(NOS_NAME("Gain"));

		// Only support fixed step timing
		if (pins.TimingMode != NOS_EXECUTION_TIMING_MODE_FIXED_STEP)
		{
			SetNodeStatusMessageIfChanged("Unsupported timing mode", fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_FAILED;
		}

		// Check for invalid timing values
		if (pins.FixedStepTiming.DeltaSeconds.y == 0)
		{
			SetNodeStatusMessageIfChanged("Invalid timing values", fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_FAILED;
		}

#ifdef _WIN32
		// Initialize or reinitialize capture if needed
		if (Active && (NeedsReinitialize || !Capture))
		{
			if (Capture)
			{
				Capture->Stop();
				Capture.reset();
			}

			Capture = std::make_unique<WASAPICapture>();
			if (!Capture->Initialize(sampleRate, channelCount))
			{
				SetNodeStatusMessageIfChanged("Failed to initialize system audio capture", fb::NodeStatusMessageType::FAILURE);
				Capture.reset();
				Active = false;
				return NOS_RESULT_FAILED;
			}

			if (!Capture->Start())
			{
				SetNodeStatusMessageIfChanged("Failed to start system audio capture", fb::NodeStatusMessageType::FAILURE);
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
#else
		// System audio input is not supported on non-Windows platforms yet
		SetNodeStatusMessageIfChanged("System audio input is not supported on this platform yet", fb::NodeStatusMessageType::FAILURE);
		return NOS_RESULT_FAILED;
#endif

		uint64_t deltaNumerator = pins.FixedStepTiming.DeltaSeconds.x;
		uint64_t deltaDenominator = pins.FixedStepTiming.DeltaSeconds.y;

		AccumulatedSampleNumerator += deltaNumerator * static_cast<uint64_t>(sampleRate);

		uint32_t numSamples = static_cast<uint32_t>(AccumulatedSampleNumerator / deltaDenominator);
		AccumulatedSampleNumerator %= deltaDenominator; // Keep remainder for next frame

		// Create or resize audio buffer only if needed (with 1.1x headroom to avoid frequent reallocations)
		size_t requiredBufferSize = numSamples * sizeof(uint32_t) * channelCount;
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

			// Allocate 1.1x the required size to reduce frequency of reallocations
			size_t newBufferSize = requiredBufferSize * 1.1f;

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

		int32_t* audioSamples = reinterpret_cast<int32_t*>(nosVulkan->Map(AudioPacketBuffer));
		if (!audioSamples)
		{
			SetNodeStatusMessageIfChanged("Failed to map audio buffer", fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_FAILED;
		}

		if (Active)
		{
#ifdef _WIN32
			// Read captured system audio
			if (Capture)
			{
				bool hasAudio = Capture->ReadSamples(audioSamples, numSamples, channelCount, gain);
				std::string deviceName = Capture->GetDeviceName();
				std::string deviceSuffix = deviceName.empty() ? "" : " - " + deviceName;
				
				if (hasAudio)
					SetNodeStatusMessageIfChanged("Capturing audio" + deviceSuffix, fb::NodeStatusMessageType::INFO);
				else
					SetNodeStatusMessageIfChanged("Audio capture is ready" + deviceSuffix, fb::NodeStatusMessageType::INFO);
			}
			else
#endif
			{
				// Fill with silence if capture failed
				for (uint32_t i = 0; i < numSamples * channelCount; ++i)
				{
					audioSamples[i] = 0;
				}
			}
		}
		else
		{
			// Fill with silence when inactive
			for (uint32_t i = 0; i < numSamples * channelCount; ++i)
			{
				audioSamples[i] = 0;
			}
		}

		// Update current sample index
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

		// Set output pin values
		SetPinObject(NOS_NAME("AudioPacket"), out);

		return NOS_RESULT_SUCCESS;
	}

	TypedObjectRef<sys::vulkan::Buffer> AudioPacketBuffer;
	uint64_t AccumulatedSampleNumerator = 0;
	uint64_t CurrentSampleIndex = 0;
	bool Active = false;
	bool NeedsReinitialize = false;
	std::string LastStatusMessage;

#ifdef _WIN32
	std::unique_ptr<WASAPICapture> Capture;
#endif
};

nosResult RegisterSystemAudioInputNode(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("SystemAudioInput"), SystemAudioInputNode, fn);
	return NOS_RESULT_SUCCESS;
}
} // namespace nos::audio
