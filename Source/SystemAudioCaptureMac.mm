// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#ifdef __APPLE__

#include "SystemAudioCapture.h"

#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreAudio/CoreAudioTypes.h>
#import <AudioToolbox/AudioToolbox.h>

#include <Nodos/Plugin.hpp>

#include <dispatch/dispatch.h>

#include <atomic>
#include <functional>
#include <vector>

// ScreenCaptureKit is the only first-party macOS API for capturing the system
// audio output without installing a virtual device. It requires macOS 13+ and
// the Screen Recording TCC permission — macOS prompts the user automatically
// on the first SCShareableContent request. On denial or pre-13 hosts we fill
// LastError with a human-readable reason so the node surfaces it in the
// editor status area.
//
// Thread model: every public entry point (Initialize/Start/Stop) is invoked
// on an engine runner thread. Apple's ScreenCaptureKit docs DON'T formally
// require main thread, but in practice calling SCShareableContent / SCStream
// cold from a worker is known to hang or crash (FB12114396, FB15779754,
// and community report nonstrict-hq/SCShareableContent-hangs-sample). The
// root cause is that libxpc delivers replies from tccd / replayd / the
// WindowServer to dispatch_get_main_queue() by default, and CFRunLoop on
// main is the canonical place those Mach-port sources get serviced — plus
// Obj-C +initialize for these frameworks assumes the main runloop is live.
// Apple DTS has confirmed on-record that their sample code "just happened
// to be called on main" (developer.apple.com/forums/thread/735651). We
// funnel the Obj-C work through nosEngine.RunOnMainThread so we don't
// depend on that accident — the same pattern nos.display uses for AppKit.

namespace nos::audio
{
// Forward-declared here so the Obj-C delegate below can hold a raw pointer
// back to it. The anonymous-namespace pattern doesn't work — the Obj-C
// @property needs a named, externally-addressable type.
class ScreenCaptureKitCapture;
} // namespace nos::audio

// Obj-C adapter for SCStream callbacks. The SCStream delegate is a separate
// protocol from the SCStreamOutput sample handler, but ScreenCaptureKit lets
// us implement both on the same object, which keeps ownership simple.
API_AVAILABLE(macos(13.0))
@interface NosAudioStreamOutput : NSObject<SCStreamOutput, SCStreamDelegate>
@property (nonatomic, assign) nos::audio::ScreenCaptureKitCapture* backend;
@end

namespace nos::audio
{
class ScreenCaptureKitCapture : public SystemAudioCaptureBase
{
public:
	~ScreenCaptureKitCapture() override { Stop(); }

	bool Initialize(uint32_t sampleRate, uint8_t channelCount) override;
	bool Start() override;
	void Stop() override;

	// Invoked from the SCStreamOutput delegate on the capture queue.
	void OnAudioSampleBuffer(CMSampleBufferRef sampleBuffer);

private:
	// Wait this long for the async ScreenCaptureKit handshake / teardown
	// before giving up. Initialize / Start / Stop are called from the editor
	// execution thread, so we cap the wait to keep a hung system-service
	// from stalling the whole graph.
	static constexpr uint64_t ASYNC_TIMEOUT_SECONDS = 5;

	SCStream* Stream API_AVAILABLE(macos(13.0)) = nil;
	NosAudioStreamOutput* Delegate API_AVAILABLE(macos(13.0)) = nil;
	dispatch_queue_t AudioQueue = nullptr;
	std::atomic<bool> Running{false};

	// Reused each callback so we don't allocate in the capture hot path when
	// ScreenCaptureKit delivers planar (non-interleaved) float samples.
	std::vector<float> InterleaveScratch;
};

namespace
{
// Synchronously bounce fn onto the main thread via the engine's dispatcher.
// If the host engine predates plugin API 41.1 (no RunOnMainThread exposed),
// we log and fall through — running on the worker anyway is the best we can
// offer, and users on old engines will see the same intermittent crash they
// would have seen before this plugin existed.
void RunOnMainThreadSync(std::function<void()> fn)
{
	if (!fn)
		return;
	if ([NSThread isMainThread])
	{
		fn();
		return;
	}
	if (::nosEngine.RunOnMainThread)
	{
		::nosEngine.RunOnMainThread(
			[](void* p) { (*static_cast<std::function<void()>*>(p))(); },
			&fn,
			NOS_TRUE);
		return;
	}
	static bool warned = false;
	if (!warned)
	{
		::nosEngine.LogE("nos.audio: host engine has no RunOnMainThread; ScreenCaptureKit calls may crash.");
		warned = true;
	}
	fn();
}

// Pump the main runloop up to `timeoutSeconds` while `*done` is still false.
// Two reasons this is preferred over dispatch_semaphore_wait on main:
//   1. ScreenCaptureKit completion handlers are delivered through libxpc, and
//      libxpc replies default to dispatch_get_main_queue(). Blocking main
//      with a semaphore deadlocks: the main queue can't service the reply
//      because we're sitting on it.
//   2. We stay on a runloop pass cadence, so any plugin that queued work on
//      the main thread via MainThreadDispatcher (e.g. the display plugin)
//      doesn't starve while we wait.
// Returns true if `done` flipped before the deadline.
bool PumpMainRunLoopUntil(const bool& done, double timeoutSeconds)
{
	const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + timeoutSeconds;
	while (!done)
	{
		const CFAbsoluteTime remaining = deadline - CFAbsoluteTimeGetCurrent();
		if (remaining <= 0.0)
			return false;
		const CFTimeInterval step = std::min<CFTimeInterval>(0.05, remaining);
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, step, /*returnAfterSourceHandled*/ true);
	}
	return true;
}
} // namespace

bool ScreenCaptureKitCapture::Initialize(uint32_t sampleRate, uint8_t channelCount)
{
	if (@available(macOS 13.0, *))
	{
		TargetSampleRate = sampleRate;
		TargetChannelCount = channelCount;

		bool ok = false;
		RunOnMainThreadSync([&] {
			@autoreleasepool
			{
				// Everything that touches SCShareableContent / SCDisplay lives
				// inside the completion handler, where the framework guarantees
				// those objects are alive. Propagating them out through ARC +
				// __block + dispatch_semaphore_wait crashes intermittently on
				// macOS 26 (objc_msgSend on an apparently-valid SCShareableContent
				// in the caller's frame). Building the SCStream here avoids that.
				__block bool done = false;
				__block bool fetchOk = false;
				__block std::string fetchError;
				[SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent* content, NSError* error) {
					@autoreleasepool
					{
						if (error)
						{
							NSString* desc = error.localizedDescription;
							const char* utf8 = desc.UTF8String;
							fetchError = utf8 ? utf8 : "ScreenCaptureKit returned an unspecified error";
							done = true;
							return;
						}
						if (!content || content.displays.count == 0)
						{
							fetchError = "Screen Recording permission required. Enable Nodos in System Settings → "
										 "Privacy & Security → Screen & System Audio Recording, then restart the editor.";
							done = true;
							return;
						}

						SCDisplay* display = content.displays.firstObject;
						DeviceName = std::string("System Audio (Display ") + std::to_string(display.displayID) + ")";

						SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
						SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
						config.capturesAudio = YES;
						config.excludesCurrentProcessAudio = NO;
						config.sampleRate = (NSInteger)sampleRate;
						config.channelCount = (NSInteger)channelCount;
						// ScreenCaptureKit on macOS 13–14 still requires a video track
						// to be configured even for audio-only capture. A 2×2, 1 fps
						// track is the cheapest legal configuration and we never attach
						// a video output, so the frames are dropped by the framework.
						config.width = 2;
						config.height = 2;
						config.minimumFrameInterval = CMTimeMake(1, 1);
						config.queueDepth = 6;

						Delegate = [[NosAudioStreamOutput alloc] init];
						Delegate.backend = this;

						Stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:Delegate];

						AudioQueue = dispatch_queue_create("dev.nodos.audio.SystemAudioCapture", DISPATCH_QUEUE_SERIAL);

						NSError* attachError = nil;
						const BOOL attached = [Stream addStreamOutput:Delegate
																 type:SCStreamOutputTypeAudio
												   sampleHandlerQueue:AudioQueue
																error:&attachError];
						if (!attached)
						{
							const char* utf8 = attachError.localizedDescription.UTF8String;
							fetchError = utf8 ? utf8 : "Failed to attach audio stream output";
							Stream = nil;
							Delegate = nil;
							AudioQueue = nullptr;
							done = true;
							return;
						}

						fetchOk = true;
						done = true;
					}
				}];

				if (!PumpMainRunLoopUntil(done, 5.0))
				{
					LastError = "Timed out waiting for shareable content";
					return;
				}
				if (!fetchOk)
				{
					LastError = std::move(fetchError);
					return;
				}
				LastError.clear();
				ok = true;
			}
		});
		return ok;
	}

	LastError = "System audio capture requires macOS 13 (Ventura) or later";
	return false;
}

bool ScreenCaptureKitCapture::Start()
{
	if (Running)
		return true;
	if (@available(macOS 13.0, *))
	{
		if (!Stream)
			return false;

		bool ok = false;
		RunOnMainThreadSync([&] {
			@autoreleasepool
			{
				__block bool done = false;
				__block std::string startErrorMessage;
				__block bool hasStartError = false;
				[Stream startCaptureWithCompletionHandler:^(NSError* error) {
					if (error)
					{
						hasStartError = true;
						NSString* desc = error.localizedDescription;
						const char* utf8 = desc.UTF8String;
						startErrorMessage = utf8 ? utf8 : "ScreenCaptureKit returned an unspecified error";
					}
					done = true;
				}];
				if (!PumpMainRunLoopUntil(done, ASYNC_TIMEOUT_SECONDS))
				{
					LastError = "Timed out starting ScreenCaptureKit stream";
					return;
				}
				if (hasStartError)
				{
					LastError = std::move(startErrorMessage);
					return;
				}
				Running = true;
				ok = true;
			}
		});
		return ok;
	}
	return false;
}

void ScreenCaptureKitCapture::Stop()
{
	if (!Running && !Stream)
		return;

	if (@available(macOS 13.0, *))
	{
		RunOnMainThreadSync([&] {
			@autoreleasepool
			{
				if (Stream && Running)
				{
					__block bool done = false;
					[Stream stopCaptureWithCompletionHandler:^(NSError* /*error*/) {
						done = true;
					}];
					PumpMainRunLoopUntil(done, ASYNC_TIMEOUT_SECONDS);
				}
				if (Delegate)
					Delegate.backend = nullptr;
				Stream = nil;
				Delegate = nil;
			}
		});
	}

	AudioQueue = nullptr;
	Running = false;
	ResetBuffer();
}

void ScreenCaptureKitCapture::OnAudioSampleBuffer(CMSampleBufferRef sampleBuffer)
{
	if (!sampleBuffer || !CMSampleBufferIsValid(sampleBuffer) || !CMSampleBufferDataIsReady(sampleBuffer))
		return;

	CMFormatDescriptionRef desc = CMSampleBufferGetFormatDescription(sampleBuffer);
	if (!desc)
		return;
	const AudioStreamBasicDescription* asbd = CMAudioFormatDescriptionGetStreamBasicDescription(desc);
	if (!asbd || asbd->mBitsPerChannel != 32 || !(asbd->mFormatFlags & kAudioFormatFlagIsFloat))
	{
		// ScreenCaptureKit always delivers Float32 PCM per the docs; bail
		// safely if a future macOS changes that so we don't interpret the
		// bytes as the wrong type.
		return;
	}

	const uint32_t channels = asbd->mChannelsPerFrame;
	const uint32_t sourceRate = static_cast<uint32_t>(asbd->mSampleRate);
	if (channels == 0 || sourceRate == 0)
		return;

	const CMItemCount frameCount = CMSampleBufferGetNumSamples(sampleBuffer);
	if (frameCount == 0)
		return;

	// Two-phase pull: first call sizes the AudioBufferList, second copies.
	size_t bufferListSize = 0;
	OSStatus status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
		sampleBuffer,
		&bufferListSize,
		nullptr,
		0,
		kCFAllocatorSystemDefault,
		kCFAllocatorSystemDefault,
		kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
		nullptr);
	if (status != noErr || bufferListSize == 0)
		return;

	std::vector<uint8_t> storage(bufferListSize);
	auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
	CMBlockBufferRef blockBuffer = nullptr;
	status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
		sampleBuffer,
		nullptr,
		list,
		bufferListSize,
		kCFAllocatorSystemDefault,
		kCFAllocatorSystemDefault,
		kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
		&blockBuffer);
	if (status != noErr || !blockBuffer)
		return;

	const bool nonInterleaved = (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
	if (nonInterleaved && list->mNumberBuffers == channels)
	{
		InterleaveScratch.resize(static_cast<size_t>(frameCount) * channels);
		for (uint32_t ch = 0; ch < channels; ++ch)
		{
			const float* src = reinterpret_cast<const float*>(list->mBuffers[ch].mData);
			if (!src)
				continue;
			for (CMItemCount f = 0; f < frameCount; ++f)
				InterleaveScratch[static_cast<size_t>(f) * channels + ch] = src[f];
		}
		PushInterleavedSamples(InterleaveScratch.data(),
							   static_cast<uint32_t>(frameCount),
							   sourceRate,
							   static_cast<uint8_t>(channels));
	}
	else if (list->mNumberBuffers >= 1)
	{
		const float* src = reinterpret_cast<const float*>(list->mBuffers[0].mData);
		if (src)
			PushInterleavedSamples(src,
								   static_cast<uint32_t>(frameCount),
								   sourceRate,
								   static_cast<uint8_t>(channels));
	}

	CFRelease(blockBuffer);
}

std::unique_ptr<ISystemAudioCapture> ISystemAudioCapture::Create()
{
	return std::make_unique<ScreenCaptureKitCapture>();
}
} // namespace nos::audio

@implementation NosAudioStreamOutput
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type
{
	if (type != SCStreamOutputTypeAudio)
		return;
	auto* backend = self.backend;
	if (backend)
		backend->OnAudioSampleBuffer(sampleBuffer);
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error
{
	// Surface nothing here directly; the node already displays a warning when
	// ReadSamples returns silence. Recording the error on the backend would
	// race with Stop() tearing everything down, so we keep the hook empty.
}
@end

#endif // __APPLE__
