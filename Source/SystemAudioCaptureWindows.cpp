// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#ifdef _WIN32

#include "SystemAudioCapture.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <Windows.h>
#include <Audioclient.h>
#include <comdef.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

_COM_SMARTPTR_TYPEDEF(IMMDeviceEnumerator, __uuidof(IMMDeviceEnumerator));
_COM_SMARTPTR_TYPEDEF(IMMDevice, __uuidof(IMMDevice));
_COM_SMARTPTR_TYPEDEF(IAudioClient, __uuidof(IAudioClient));
_COM_SMARTPTR_TYPEDEF(IAudioCaptureClient, __uuidof(IAudioCaptureClient));

namespace nos::audio
{
namespace
{
class WASAPICapture : public SystemAudioCaptureBase
{
public:
	WASAPICapture() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
	~WASAPICapture() override
	{
		Stop();
		CoUninitialize();
	}

	bool Initialize(uint32_t sampleRate, uint8_t channelCount) override
	{
		IMMDeviceEnumeratorPtr enumerator;
		if (FAILED(enumerator.CreateInstance(__uuidof(MMDeviceEnumerator))))
		{
			LastError = "Failed to create MMDeviceEnumerator";
			return false;
		}

		IMMDevicePtr device;
		if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
		{
			LastError = "No default render endpoint";
			return false;
		}

		IPropertyStore* props = nullptr;
		if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)))
		{
			PROPVARIANT varName;
			PropVariantInit(&varName);
			if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName)))
			{
				DeviceName = _com_util::ConvertBSTRToString(varName.bstrVal);
				PropVariantClear(&varName);
			}
			props->Release();
		}

		if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&AudioClient)))
		{
			LastError = "Failed to activate audio client";
			return false;
		}

		WAVEFORMATEX* mixFormat = nullptr;
		if (FAILED(AudioClient->GetMixFormat(&mixFormat)))
		{
			LastError = "Failed to get mix format";
			return false;
		}

		const HRESULT initHr = AudioClient->Initialize(
			AUDCLNT_SHAREMODE_SHARED,
			AUDCLNT_STREAMFLAGS_LOOPBACK,
			10'000'000, // 1 second buffer in 100-ns units
			0,
			mixFormat,
			nullptr);

		// Snapshot the negotiated format before freeing it.
		const uint32_t negotiatedRate = mixFormat->nSamplesPerSec;
		const uint8_t negotiatedChannels = static_cast<uint8_t>(mixFormat->nChannels);
		CoTaskMemFree(mixFormat);

		if (FAILED(initHr))
		{
			LastError = "Failed to initialize loopback client";
			return false;
		}

		SourceSampleRate = negotiatedRate;
		SourceChannelCount = negotiatedChannels;
		TargetSampleRate = sampleRate;
		TargetChannelCount = channelCount;

		if (FAILED(AudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&CaptureClient)))
		{
			LastError = "Failed to get capture client";
			return false;
		}

		LastError.clear();
		return true;
	}

	bool Start() override
	{
		if (IsCapturing)
			return true;
		if (!AudioClient)
			return false;
		if (FAILED(AudioClient->Start()))
		{
			LastError = "Failed to start audio client";
			return false;
		}
		IsCapturing = true;
		ShouldStop = false;
		CaptureThread = std::thread(&WASAPICapture::CaptureThreadFunc, this);
		return true;
	}

	void Stop() override
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

private:
	void CaptureThreadFunc()
	{
		while (!ShouldStop)
		{
			if (!CaptureClient)
				break;

			UINT32 packetLength = 0;
			if (FAILED(CaptureClient->GetNextPacketSize(&packetLength)))
				break;

			while (packetLength > 0)
			{
				BYTE* data = nullptr;
				UINT32 numFramesAvailable = 0;
				DWORD flags = 0;
				if (FAILED(CaptureClient->GetBuffer(&data, &numFramesAvailable, &flags, nullptr, nullptr)))
					break;

				if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && numFramesAvailable > 0)
				{
					PushInterleavedSamples(
						reinterpret_cast<const float*>(data),
						numFramesAvailable,
						SourceSampleRate,
						SourceChannelCount);
				}

				CaptureClient->ReleaseBuffer(numFramesAvailable);

				if (FAILED(CaptureClient->GetNextPacketSize(&packetLength)))
					break;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}

	IAudioClientPtr AudioClient;
	IAudioCaptureClientPtr CaptureClient;
	std::thread CaptureThread;
	std::atomic<bool> IsCapturing{false};
	std::atomic<bool> ShouldStop{false};
};
} // namespace

std::unique_ptr<ISystemAudioCapture> ISystemAudioCapture::Create()
{
	return std::make_unique<WASAPICapture>();
}
} // namespace nos::audio

#endif // _WIN32
