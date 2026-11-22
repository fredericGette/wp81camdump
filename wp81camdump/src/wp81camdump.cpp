// wp81camdump: takes a picture with the back camera of a Lumia 520 (Windows Phone 8.1)
// from a console application, and saves it as a JPEG file.
//
// The WinRT MediaCapture API is used without app identity: the activation factories are
// obtained directly from the DLLs implementing the runtime classes (DllGetActivationFactory),
// like in wp81powertool (battery.cpp).
// Analysis of WINDOWS.MEDIA.DLL: no package identity / capability check in the capture path,
// the camera resource is acquired through mfpalwp.dll (MFMCPalAcquireResources) during the
// initialization, and the photo is taken with IMFCaptureEngine::TakePhoto without checking
// that a preview is running.

#include "stdafx.h"

using Microsoft::WRL::ComPtr;
namespace WF = ABI::Windows::Foundation;
namespace WFC = ABI::Windows::Foundation::Collections;
namespace WSS = ABI::Windows::Storage::Streams;
namespace WM = ABI::Windows::Media;
namespace WMC = ABI::Windows::Media::Capture;
namespace WMD = ABI::Windows::Media::Devices;
namespace WMP = ABI::Windows::Media::MediaProperties;

#define WINDOWS_MEDIA_DLL "C:\\Windows\\System32\\Windows.Media.dll"
#define VIDEOSINK_RT_DLL "C:\\Windows\\System32\\VideoSink_RT.dll"
#define MFCAPTUREENGINE_DLL "C:\\Windows\\System32\\MFCaptureEngine.dll"
#define DEFAULT_OUTPUT "C:\\Data\\Users\\Public\\Documents\\camdump.jpg"
#define DEFAULT_RAW_OUTPUT "C:\\Data\\Users\\Public\\Documents\\camdump.raw"

#define ASYNC_TIMEOUT_MS 30000

static Win32Api win32Api;
static BOOL verbose = FALSE;

// RAII HSTRING
class HStr {
public:
	HSTRING h = nullptr;
	HStr() {}
	explicit HStr(const wchar_t *s) { WindowsCreateString(s, (UINT32)wcslen(s), &h); }
	~HStr() { if (h) WindowsDeleteString(h); }
	const wchar_t *c_str() const { return h ? WindowsGetStringRawBuffer(h, nullptr) : L""; }
	HSTRING *operator&() { return &h; }
};

#define CHECK(hr, what) \
	if (FAILED(hr)) { printf("%s failed 0x%08lX\n", what, (unsigned long)(hr)); return hr; } \
	else if (verbose) { printf("%s OK\n", what); }

static void PrintUsage()
{
	printf("Takes a picture with the back camera and saves it as a JPEG file.\n");
	printf("Usage: wp81camdump [options] [output.jpg]\n");
	printf("  -h         Show this help\n");
	printf("  -v         Verbose: print each step and the available photo resolutions\n");
	printf("  -n         No preview: take the picture directly (black picture on the Lumia 520)\n");
	printf("             By default a preview runs (to Windows.Phone.Media.Capture.MediaCapturePreviewSink)\n");
	printf("             before the picture, so the auto exposure/focus can settle\n");
	printf("  -d <ms>    Delay between the start of the preview and the picture (default 2000)\n");
	printf("  -s         Keep the default photo resolution (default: the largest one)\n");
	printf("  -a         Audible: keep the shutter sound (default: no sound)\n");
	printf("  -l         List the native media types of every camera stream, and exit\n");
	printf("  -r         Raw: save the still in a raw Bayer format of the sensor, as it is (+ a .txt with the format)\n");
	printf("             Needs the PT formats of the driver (ManufacturingOS ManufacturingMode = 1)\n");
	printf("  -c <n>     With -r: take <n> raw stills in the same session (<name>_1.raw, ...)\n");
	printf("  -g         With -r: first a throwaway still in the largest raw type, to make the driver reallocate\n");
	printf("             its raw buffer (only the first camera session after boot gets raw data otherwise)\n");
	printf("  -t <s>:<t> With -r: use the media type <t> of the stream <s> (indexes printed by -l)\n");
	printf("Default output: %s (%s with -r)\n", DEFAULT_OUTPUT, DEFAULT_RAW_OUTPUT);
}

// Gets an activation factory directly from the DLL implementing the runtime class.
static HRESULT GetFactory(const char *dllPath, const wchar_t *className, REFIID iid, void **ppv)
{
	*ppv = nullptr;
	HMODULE hModule = win32Api.LoadLibraryA(dllPath);
	if (hModule == NULL)
	{
		DWORD err = GetLastError();
		printf("LoadLibraryA(%s) failed 0x%08lX\n", dllPath, err);
		return HRESULT_FROM_WIN32(err);
	}

	typedef HRESULT(WINAPI *DllGetActivationFactoryFn)(HSTRING, IActivationFactory **);
	DllGetActivationFactoryFn fn = (DllGetActivationFactoryFn)GetProcAddress(hModule, "DllGetActivationFactory");
	if (fn == NULL)
	{
		DWORD err = GetLastError();
		printf("GetProcAddress(DllGetActivationFactory) in %s failed 0x%08lX\n", dllPath, err);
		return HRESULT_FROM_WIN32(err);
	}

	HStr name(className);
	ComPtr<IActivationFactory> factory;
	HRESULT hr = fn(name.h, &factory);
	if (FAILED(hr))
	{
		printf("DllGetActivationFactory(%ls) failed 0x%08lX\n", className, (unsigned long)hr);
		return hr;
	}
	hr = factory->QueryInterface(iid, ppv);
	if (FAILED(hr))
	{
		printf("QueryInterface on the factory of %ls failed 0x%08lX\n", className, (unsigned long)hr);
	}
	return hr;
}

// Creates an instance of a runtime class having a default constructor.
static HRESULT ActivateInstance(const char *dllPath, const wchar_t *className, REFIID iid, void **ppv)
{
	ComPtr<IActivationFactory> factory;
	HRESULT hr = GetFactory(dllPath, className, __uuidof(IActivationFactory), (void **)factory.GetAddressOf());
	if (FAILED(hr)) return hr;

	ComPtr<IInspectable> instance;
	hr = factory->ActivateInstance(&instance);
	if (FAILED(hr))
	{
		printf("ActivateInstance(%ls) failed 0x%08lX\n", className, (unsigned long)hr);
		return hr;
	}
	hr = instance->QueryInterface(iid, ppv);
	if (FAILED(hr))
	{
		printf("QueryInterface on %ls failed 0x%08lX\n", className, (unsigned long)hr);
	}
	return hr;
}

// Shutter sound.
// MediaCaptureServer::_Initialize (WINDOWS.MEDIA.DLL) creates the capture engine with
// IMFCaptureEngineClassFactory::CreateInstance and initializes it with an attribute store.
// For a camera device (no custom VideoSource) the store only gets
// MF_CAPTURE_ENGINE_CAMERA_REGIONAL_SOUND_LEVEL (HKLM\Software\Microsoft\Photos\OEM\RegionalSettings\Camera
// CameraSoundLevel) and the engine plays its shutter sound. With a custom VideoSource it sets
// MF_CAPTURE_ENGINE_CUSTOM_CAMERA_SOUND = 1 instead: the application plays its own sound, and
// the engine none. So IMFCaptureEngine::Initialize is hooked (vtable patch, shared by all the
// engine instances) to add MF_CAPTURE_ENGINE_CUSTOM_CAMERA_SOUND = 1.

// {efce38d3-8914-4674-a7df-ae1b3d654b8a} (same value used by WINDOWS.MEDIA.DLL for both CLSIDs)
static const GUID CLSID_MFCaptureEngineClassFactory_ =
	{ 0xefce38d3, 0x8914, 0x4674, { 0xa7, 0xdf, 0xae, 0x1b, 0x3d, 0x65, 0x4b, 0x8a } };
static const GUID CLSID_MFCaptureEngine_ =
	{ 0xefce38d3, 0x8914, 0x4674, { 0xa7, 0xdf, 0xae, 0x1b, 0x3d, 0x65, 0x4b, 0x8a } };
// IMFCaptureEngineClassFactory {8f02d140-56fc-4302-a705-3a97c78be779}
static const GUID IID_IMFCaptureEngineClassFactory_ =
	{ 0x8f02d140, 0x56fc, 0x4302, { 0xa7, 0x05, 0x3a, 0x97, 0xc7, 0x8b, 0xe7, 0x79 } };
// IMFCaptureEngine {a6bba433-176b-48b2-b375-53aa03473207}
static const GUID IID_IMFCaptureEngine_ =
	{ 0xa6bba433, 0x176b, 0x48b2, { 0xb3, 0x75, 0x53, 0xaa, 0x03, 0x47, 0x32, 0x07 } };
// MF_CAPTURE_ENGINE_CUSTOM_CAMERA_SOUND {e7e745df-6d9b-4cc3-a3b6-7dded4f61272}
static const GUID MF_CAPTURE_ENGINE_CUSTOM_CAMERA_SOUND_ =
	{ 0xe7e745df, 0x6d9b, 0x4cc3, { 0xa3, 0xb6, 0x7d, 0xde, 0xd4, 0xf6, 0x12, 0x72 } };

// IMFCaptureEngineClassFactory (not in the Windows Phone 8.1 SDK)
MIDL_INTERFACE("8f02d140-56fc-4302-a705-3a97c78be779")
IMFCaptureEngineClassFactory_ : public IUnknown
{
public:
	virtual HRESULT STDMETHODCALLTYPE CreateInstance(REFCLSID clsid, REFIID riid, LPVOID *ppvObject) = 0;
};

// Capture engine interfaces (mfcaptureengine.h isn't in the Windows Phone 8.1 SDK).
// Vtable order checked against MFCaptureEngine.dll of the phone (CCaptureEngine, CCaptureSource and
// CCapturePhotoSink vftables): same as Windows 8.1, plus Start/StopPhotoSequence at the end of IMFCaptureEngine.
enum { CAPTURE_SINK_TYPE_RECORD = 0, CAPTURE_SINK_TYPE_PREVIEW = 1, CAPTURE_SINK_TYPE_PHOTO = 2 };
enum { CAPTURE_DEVICE_TYPE_AUDIO = 0, CAPTURE_DEVICE_TYPE_VIDEO = 1 };

MIDL_INTERFACE("52150b82-ab39-4467-980f-e48bf0822ecd")
IMFCaptureEngineOnSampleCallback_ : public IUnknown
{
public:
	virtual HRESULT STDMETHODCALLTYPE OnSample(IMFSample *sample) = 0;
};

MIDL_INTERFACE("72d6135b-35e9-412c-b926-fd5265f2a885")
IMFCaptureSink_ : public IUnknown
{
public:
	virtual HRESULT STDMETHODCALLTYPE GetOutputMediaType(DWORD sinkStreamIndex, IMFMediaType **mediaType) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetService(DWORD sinkStreamIndex, REFGUID service, REFIID riid, IUnknown **unknown) = 0;
	virtual HRESULT STDMETHODCALLTYPE AddStream(DWORD sourceStreamIndex, IMFMediaType *mediaType, IMFAttributes *attributes, DWORD *sinkStreamIndex) = 0;
	virtual HRESULT STDMETHODCALLTYPE Prepare() = 0;
	virtual HRESULT STDMETHODCALLTYPE RemoveAllStreams() = 0;
};

MIDL_INTERFACE("d2d43cc8-48bb-4aa7-95db-10c06977e777")
IMFCapturePhotoSink_ : public IMFCaptureSink_
{
public:
	virtual HRESULT STDMETHODCALLTYPE SetOutputFileName(LPCWSTR fileName) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetSampleCallback(IMFCaptureEngineOnSampleCallback_ *callback) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetOutputByteStream(IUnknown *byteStream) = 0;
};

MIDL_INTERFACE("439a42a8-0d2c-4505-be83-f79b2a05d5c4")
IMFCaptureSource_ : public IUnknown
{
public:
	virtual HRESULT STDMETHODCALLTYPE GetCaptureDeviceSource(int deviceType, IUnknown **mediaSource) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetCaptureDeviceActivate(int deviceType, IUnknown **activate) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetService(REFIID service, REFIID riid, IUnknown **unknown) = 0;
	virtual HRESULT STDMETHODCALLTYPE AddEffect(DWORD sourceStreamIndex, IUnknown *effect) = 0;
	virtual HRESULT STDMETHODCALLTYPE RemoveEffect(DWORD sourceStreamIndex, IUnknown *effect) = 0;
	virtual HRESULT STDMETHODCALLTYPE RemoveAllEffects(DWORD sourceStreamIndex) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetAvailableDeviceMediaType(DWORD sourceStreamIndex, DWORD mediaTypeIndex, IMFMediaType **mediaType) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetCurrentDeviceMediaType(DWORD sourceStreamIndex, IMFMediaType *mediaType) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetCurrentDeviceMediaType(DWORD sourceStreamIndex, IMFMediaType **mediaType) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetDeviceStreamCount(DWORD *streamCount) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetDeviceStreamCategory(DWORD sourceStreamIndex, int *streamCategory) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetMirrorState(DWORD streamIndex, BOOL *mirrorState) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetMirrorState(DWORD streamIndex, BOOL mirrorState) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetStreamIndexFromFriendlyName(UINT32 friendlyName, DWORD *actualStreamIndex) = 0;
};

MIDL_INTERFACE("a6bba433-176b-48b2-b375-53aa03473207")
IMFCaptureEngine_ : public IUnknown
{
public:
	virtual HRESULT STDMETHODCALLTYPE Initialize(IUnknown *eventCallback, IMFAttributes *attributes, IUnknown *audioSource, IUnknown *videoSource) = 0;
	virtual HRESULT STDMETHODCALLTYPE StartPreview() = 0;
	virtual HRESULT STDMETHODCALLTYPE StopPreview() = 0;
	virtual HRESULT STDMETHODCALLTYPE StartRecord() = 0;
	virtual HRESULT STDMETHODCALLTYPE StopRecord(BOOL finalize, BOOL flushUnprocessedSamples) = 0;
	virtual HRESULT STDMETHODCALLTYPE TakePhoto() = 0;
	virtual HRESULT STDMETHODCALLTYPE GetSink(int sinkType, IMFCaptureSink_ **sink) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetSource(IMFCaptureSource_ **source) = 0;
};

// Media type attributes (values from mfapi.h, defined here to link only against mincore.lib)
static const GUID MF_MT_MAJOR_TYPE_ = { 0x48eba18e, 0xf8c9, 0x4687, { 0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f } };
static const GUID MF_MT_SUBTYPE_ = { 0xf7e34c9a, 0x42e8, 0x4714, { 0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5 } };
static const GUID MF_MT_FRAME_SIZE_ = { 0x1652c33d, 0xd6b2, 0x4012, { 0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d } };
static const GUID MF_MT_DEFAULT_STRIDE_ = { 0x644b4e48, 0x1e02, 0x4516, { 0xb0, 0xeb, 0xc0, 0x1c, 0xa9, 0xd4, 0x9a, 0xc6 } };
static const GUID MF_MT_SAMPLE_SIZE_ = { 0xdad3ab78, 0x1990, 0x408b, { 0xbc, 0xe2, 0xeb, 0xa6, 0x73, 0xda, 0xcc, 0x10 } };
static const GUID MF_MT_FRAME_RATE_ = { 0xc459a2e8, 0x3d2c, 0x4e44, { 0xb1, 0x32, 0xfe, 0xe5, 0x15, 0x6c, 0x7b, 0xb0 } };
static const GUID MFMediaType_Video_ = { 0x73646976, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID MFMediaType_Image_ = { 0x72178C23, 0xE45B, 0x11D5, { 0xBC, 0x2A, 0x00, 0xB0, 0xD0, 0xF3, 0xF4, 0xAB } };

// IMFCaptureEngine::Initialize(IMFCaptureEngineOnEventCallback *, IMFAttributes *, IUnknown *audioSource, IUnknown *videoSource)
// is the 4th entry of the vtable (after the IUnknown methods).
#define CAPTURE_ENGINE_INITIALIZE_INDEX 3
typedef HRESULT(STDMETHODCALLTYPE *CaptureEngineInitializeFn)(IUnknown *engine, IUnknown *eventCallback,
	IMFAttributes *attributes, IUnknown *audioSource, IUnknown *videoSource);
static CaptureEngineInitializeFn realCaptureEngineInitialize = nullptr;
static IUnknown *hookedEngine = nullptr; // Kept alive: pins the DLL holding the patched vtable
static BOOL muteShutter = TRUE;
// The engine created and initialized by WINDOWS.MEDIA.DLL (MediaCaptureServer), caught by the hook
static IMFCaptureEngine_ *capturedEngine = nullptr;

static void FormatSubtype(const GUID &g, char *out, size_t size);

// Events of the engine (GUIDs checked in MFCaptureEngine.dll)
static const GUID MF_CAPTURE_ENGINE_ERROR_ = { 0x46b89fc6, 0x33cc, 0x4399, { 0x9d, 0xad, 0x78, 0x4d, 0xe7, 0x7d, 0x58, 0x7c } };
static const GUID MF_CAPTURE_ENGINE_PHOTO_TAKEN_ = { 0x3c50c445, 0x7304, 0x48eb, { 0x86, 0x5d, 0xbb, 0xa1, 0x9b, 0xa3, 0xaf, 0x5c } };
static const GUID MF_CAPTURE_SOURCE_CURRENT_DEVICE_MEDIA_TYPE_SET_ = { 0xe7e75e4c, 0x039c, 0x4410, { 0x81, 0x5b, 0x87, 0x41, 0x30, 0x7b, 0x63, 0xaa } };
// IMFCaptureEngineOnEventCallback
static const GUID IID_IMFCaptureEngineOnEventCallback_ = { 0xaeda51c0, 0x9025, 0x4983, { 0x90, 0x12, 0xde, 0x59, 0x7b, 0x88, 0xb0, 0x89 } };

// Wraps the event callback that WINDOWS.MEDIA.DLL gives to IMFCaptureEngine::Initialize.
// The operations done directly on the engine (raw capture) send events that MediaCaptureServer doesn't expect:
// MF_CAPTURE_SOURCE_CURRENT_DEVICE_MEDIA_TYPE_SET goes to SetStreamSettingsOperationServer::_HandleAsyncCompletion
// without a pending operation, and crashes (NULL read in RTWorkQ). While intercepting, the events are kept here.
// No virtual destructor: OnEvent must be the 4th entry of the vtable
class EventCallbackFilter : public IUnknown
{
	LONG refCount = 1;
	IUnknown *inner;
public:
	volatile LONG intercept = 0;
	HANDLE mediaTypeSet = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
	HANDLE photoTaken = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
	HRESULT mediaTypeStatus = S_OK, photoStatus = S_OK;

	explicit EventCallbackFilter(IUnknown *innerCallback) : inner(innerCallback) { inner->AddRef(); }
	~EventCallbackFilter() { inner->Release(); CloseHandle(mediaTypeSet); CloseHandle(photoTaken); }

	STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
	{
		if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, IID_IMFCaptureEngineOnEventCallback_))
		{
			*ppv = this;
			AddRef();
			return S_OK;
		}
		return inner->QueryInterface(riid, ppv);
	}
	STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refCount); }
	STDMETHODIMP_(ULONG) Release() override
	{
		LONG r = InterlockedDecrement(&refCount);
		if (r == 0) delete this;
		return r;
	}
	// IMFCaptureEngineOnEventCallback::OnEvent (4th entry of the vtable)
	virtual HRESULT STDMETHODCALLTYPE OnEvent(IMFMediaEvent *event)
	{
		if (!intercept)
		{
			typedef HRESULT(STDMETHODCALLTYPE *OnEventFn)(IUnknown *, IMFMediaEvent *);
			return ((OnEventFn)(*(void ***)inner)[3])(inner, event);
		}
		GUID type = {};
		HRESULT status = S_OK;
		event->GetExtendedType(&type);
		event->GetStatus(&status);
		if (verbose)
		{
			char name[64];
			FormatSubtype(type, name, sizeof(name));
			printf("Engine event %s status 0x%08lX (kept from MediaCapture)\n", name, (unsigned long)status);
		}
		if (IsEqualGUID(type, MF_CAPTURE_SOURCE_CURRENT_DEVICE_MEDIA_TYPE_SET_))
		{
			mediaTypeStatus = status;
			SetEvent(mediaTypeSet);
		}
		else if (IsEqualGUID(type, MF_CAPTURE_ENGINE_PHOTO_TAKEN_))
		{
			photoStatus = status;
			SetEvent(photoTaken);
		}
		else if (IsEqualGUID(type, MF_CAPTURE_ENGINE_ERROR_))
		{
			mediaTypeStatus = photoStatus = FAILED(status) ? status : E_FAIL;
			SetEvent(mediaTypeSet);
			SetEvent(photoTaken);
		}
		return S_OK;
	}
};
static EventCallbackFilter *eventFilter = nullptr;

static HRESULT STDMETHODCALLTYPE HookCaptureEngineInitialize(IUnknown *engine, IUnknown *eventCallback,
	IMFAttributes *attributes, IUnknown *audioSource, IUnknown *videoSource)
{
	if (engine != hookedEngine && eventCallback != nullptr)
	{
		if (eventFilter) eventFilter->Release();
		eventFilter = new EventCallbackFilter(eventCallback);
		eventCallback = eventFilter; // the engine keeps its own reference
		if (verbose) printf("IMFCaptureEngine::Initialize: event callback wrapped\n");
	}
	if (engine != hookedEngine)
	{
		if (capturedEngine) capturedEngine->Release();
		capturedEngine = nullptr;
		engine->QueryInterface(__uuidof(IMFCaptureEngine_), (void **)&capturedEngine);
		if (verbose) printf("IMFCaptureEngine::Initialize: engine %p caught\n", (void *)capturedEngine);
	}
	if (!muteShutter)
	{
		// Shutter sound kept
	}
	else if (attributes != nullptr)
	{
		HRESULT hr = attributes->SetUINT32(MF_CAPTURE_ENGINE_CUSTOM_CAMERA_SOUND_, TRUE);
		if (FAILED(hr)) printf("Warning: cannot disable the shutter sound 0x%08lX\n", (unsigned long)hr);
		else if (verbose) printf("IMFCaptureEngine::Initialize: MF_CAPTURE_ENGINE_CUSTOM_CAMERA_SOUND set\n");
	}
	else
	{
		printf("Warning: IMFCaptureEngine::Initialize without attributes, the shutter sound stays enabled\n");
	}
	return realCaptureEngineInitialize(engine, eventCallback, attributes, audioSource, videoSource);
}

static BOOL IsBayerSubtype(const GUID &g);
static void FormatSubtype(const GUID &g, char *out, size_t size);

// Source reader of the engine.
// In PT mode the still pin of qccamavs8930.sys only offers raw Bayer types. CCaptureSource::ConfigureCurrentVideoMediaType
// (MFCaptureEngine.dll) then asks the source reader to convert the stream to NV12 (or another of its 6 candidate
// subtypes), there's no Bayer decoder, IMFSourceReader::SetCurrentMediaType fails with MF_E_TOPO_CODEC_NOT_FOUND
// and the whole initialization of MediaCapture fails. So SetCurrentMediaType is hooked: when it fails on a stream
// whose first native type is Bayer, the native type is set instead (no conversion).
#define SOURCE_READER_SET_CURRENT_MEDIA_TYPE_INDEX 7
typedef HRESULT(STDMETHODCALLTYPE *SetCurrentMediaTypeFn)(IMFSourceReader *reader, DWORD streamIndex,
	DWORD *reserved, IMFMediaType *mediaType);
static SetCurrentMediaTypeFn realSetCurrentMediaType = nullptr;
typedef HRESULT(STDAPICALLTYPE *CreateSourceReaderFromMediaSourceFn)(IUnknown *mediaSource, IMFAttributes *attributes,
	IMFSourceReader **sourceReader);
static CreateSourceReaderFromMediaSourceFn realCreateSourceReaderFromMediaSource = nullptr;
// Raw type being selected by CaptureRawPhoto (IMFCaptureSource::SetCurrentDeviceMediaType): it's the type
// the source reader must output without conversion, instead of the first native type
static ComPtr<IMFMediaType> pendingRawType;
static DWORD pendingRawStream = 0;

static HRESULT STDMETHODCALLTYPE HookSetCurrentMediaType(IMFSourceReader *reader, DWORD streamIndex,
	DWORD *reserved, IMFMediaType *mediaType)
{
	HRESULT hr = realSetCurrentMediaType(reader, streamIndex, reserved, mediaType);
	ComPtr<IMFMediaType> native;
	HRESULT hrGet = reader->GetNativeMediaType(streamIndex, 0, &native);
	if (verbose)
	{
		char requested[64] = "?", nativeName[64] = "?";
		GUID g = {};
		if (mediaType && SUCCEEDED(mediaType->GetGUID(MF_MT_SUBTYPE_, &g))) FormatSubtype(g, requested, sizeof(requested));
		if (SUCCEEDED(hrGet) && SUCCEEDED(native->GetGUID(MF_MT_SUBTYPE_, &g))) FormatSubtype(g, nativeName, sizeof(nativeName));
		printf("IMFSourceReader::SetCurrentMediaType(stream %lu, %s) 0x%08lX, native[0] %s\n", streamIndex, requested,
			(unsigned long)hr, nativeName);
	}
	if (SUCCEEDED(hr) || FAILED(hrGet)) return hr;
	GUID subtype = {};
	native->GetGUID(MF_MT_SUBTYPE_, &subtype);
	if (!IsBayerSubtype(subtype)) return hr;

	IMFMediaType *target = (pendingRawType && pendingRawStream == streamIndex) ? pendingRawType.Get() : native.Get();
	HRESULT hrNative = realSetCurrentMediaType(reader, streamIndex, reserved, target);
	if (verbose)
	{
		printf("IMFSourceReader::SetCurrentMediaType(stream %lu) failed 0x%08lX, native Bayer type set instead: 0x%08lX\n",
			streamIndex, (unsigned long)hr, (unsigned long)hrNative);
	}
	return SUCCEEDED(hrNative) ? hrNative : hr;
}

static HRESULT STDAPICALLTYPE HookCreateSourceReaderFromMediaSource(IUnknown *mediaSource, IMFAttributes *attributes,
	IMFSourceReader **sourceReader)
{
	HRESULT hr = realCreateSourceReaderFromMediaSource(mediaSource, attributes, sourceReader);
	if (SUCCEEDED(hr) && *sourceReader != nullptr && realSetCurrentMediaType == nullptr)
	{
		void **entry = &(*(void ***)*sourceReader)[SOURCE_READER_SET_CURRENT_MEDIA_TYPE_INDEX];
		DWORD oldProtect;
		if (VirtualProtect(entry, sizeof(void *), PAGE_READWRITE, &oldProtect))
		{
			realSetCurrentMediaType = (SetCurrentMediaTypeFn)*entry;
			*entry = (void *)&HookSetCurrentMediaType;
			VirtualProtect(entry, sizeof(void *), oldProtect, &oldProtect);
			if (verbose) printf("IMFSourceReader::SetCurrentMediaType hooked\n");
		}
		else
		{
			printf("VirtualProtect(IMFSourceReader vtable) failed 0x%08lX\n", GetLastError());
		}
	}
	return hr;
}

// Replaces the import importName (from a DLL whose name starts with dllPrefix) in the import table of hModule.
static void **FindImport(HMODULE hModule, const char *dllPrefix, const char *importName)
{
	BYTE *base = (BYTE *)hModule;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);

	auto searchThunks = [&](DWORD namesRva, DWORD iatRva) -> void ** {
		IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + namesRva);
		IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + iatRva);
		for (; names->u1.AddressOfData != 0; names++, iat++)
		{
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
			IMAGE_IMPORT_BY_NAME *byName = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
			if (strcmp((const char *)byName->Name, importName) == 0) return (void **)&iat->u1.Function;
		}
		return nullptr;
	};

	// Normal imports
	IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (dir.VirtualAddress != 0)
	{
		for (IMAGE_IMPORT_DESCRIPTOR *desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); desc->Name != 0; desc++)
		{
			if (_strnicmp((const char *)(base + desc->Name), dllPrefix, strlen(dllPrefix)) != 0) continue;
			void **entry = searchThunks(desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk, desc->FirstThunk);
			if (entry) return entry;
		}
	}

	// Delay-load imports (ImgDelayDescr with RVAs: attributes, DLL name, HMODULE, IAT, INT, ...)
	struct DelayDescr { DWORD attributes, dllNameRva, moduleHandleRva, iatRva, intRva, boundIatRva, unloadIatRva, timeStamp; };
	dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
	if (dir.VirtualAddress != 0)
	{
		for (DelayDescr *desc = (DelayDescr *)(base + dir.VirtualAddress); desc->dllNameRva != 0; desc++)
		{
			if (_strnicmp((const char *)(base + desc->dllNameRva), dllPrefix, strlen(dllPrefix)) != 0) continue;
			void **entry = searchThunks(desc->intRva, desc->iatRva);
			if (entry) return entry;
		}
	}
	return nullptr;
}

static void HookSourceReaderCreation(HMODULE hEngineModule)
{
	void **entry = FindImport(hEngineModule, "MFReadWrite", "MFCreateSourceReaderFromMediaSource");
	if (entry == nullptr)
	{
		printf("Warning: MFCreateSourceReaderFromMediaSource not found in the imports of MFCaptureEngine.dll\n");
		return;
	}
	if (*entry == (void *)&HookCreateSourceReaderFromMediaSource) return;
	// The entry may still point to the delay-load thunk: take the real function from MFReadWrite.dll
	HMODULE hReadWrite = win32Api.LoadLibraryA("C:\\Windows\\System32\\MFReadWrite.dll");
	CreateSourceReaderFromMediaSourceFn real = hReadWrite == NULL ? nullptr :
		(CreateSourceReaderFromMediaSourceFn)GetProcAddress(hReadWrite, "MFCreateSourceReaderFromMediaSource");
	if (real == nullptr)
	{
		printf("Warning: cannot find MFReadWrite.dll!MFCreateSourceReaderFromMediaSource 0x%08lX\n", GetLastError());
		return;
	}
	DWORD oldProtect;
	if (!VirtualProtect(entry, sizeof(void *), PAGE_READWRITE, &oldProtect))
	{
		printf("VirtualProtect(MFCaptureEngine.dll imports) failed 0x%08lX\n", GetLastError());
		return;
	}
	realCreateSourceReaderFromMediaSource = real;
	*entry = (void *)&HookCreateSourceReaderFromMediaSource;
	VirtualProtect(entry, sizeof(void *), oldProtect, &oldProtect);
	if (verbose) printf("MFCaptureEngine.dll!MFCreateSourceReaderFromMediaSource hooked\n");
}

// Hooks IMFCaptureEngine::Initialize, to mute the shutter sound and to catch the engine of MediaCapture.
static HRESULT HookCaptureEngine()
{
	// CoCreateInstance of the phone SDK fails with REGDB_E_CLASSNOTREG (no app identity): the class
	// object is obtained directly from the DLL. It's the DLL registered for the CLSID, so the module
	// (and the vtable patched below) is the one used by WINDOWS.MEDIA.DLL.
	HMODULE hModule = win32Api.LoadLibraryA(MFCAPTUREENGINE_DLL);
	if (hModule == NULL)
	{
		DWORD err = GetLastError();
		printf("LoadLibraryA(%s) failed 0x%08lX\n", MFCAPTUREENGINE_DLL, err);
		return HRESULT_FROM_WIN32(err);
	}
	HookSourceReaderCreation(hModule);

	typedef HRESULT(STDAPICALLTYPE *DllGetClassObjectFn)(REFCLSID, REFIID, LPVOID *);
	DllGetClassObjectFn getClassObject = (DllGetClassObjectFn)GetProcAddress(hModule, "DllGetClassObject");
	if (getClassObject == NULL)
	{
		DWORD err = GetLastError();
		printf("GetProcAddress(DllGetClassObject) in %s failed 0x%08lX\n", MFCAPTUREENGINE_DLL, err);
		return HRESULT_FROM_WIN32(err);
	}

	ComPtr<IClassFactory> classFactory;
	HRESULT hr = getClassObject(CLSID_MFCaptureEngineClassFactory_, __uuidof(IClassFactory),
		(void **)classFactory.GetAddressOf());
	CHECK(hr, "DllGetClassObject(MFCaptureEngineClassFactory)");

	ComPtr<IMFCaptureEngineClassFactory_> factory;
	hr = classFactory->CreateInstance(nullptr, IID_IMFCaptureEngineClassFactory_, (void **)factory.GetAddressOf());
	CHECK(hr, "IClassFactory::CreateInstance(MFCaptureEngineClassFactory)");

	ComPtr<IUnknown> engine;
	hr = factory->CreateInstance(CLSID_MFCaptureEngine_, IID_IMFCaptureEngine_, (void **)engine.GetAddressOf());
	CHECK(hr, "IMFCaptureEngineClassFactory::CreateInstance");

	void **vtable = *(void ***)engine.Get();
	void **entry = &vtable[CAPTURE_ENGINE_INITIALIZE_INDEX];
	if (*entry != (void *)&HookCaptureEngineInitialize)
	{
		DWORD oldProtect;
		if (!VirtualProtect(entry, sizeof(void *), PAGE_READWRITE, &oldProtect))
		{
			DWORD err = GetLastError();
			printf("VirtualProtect(IMFCaptureEngine vtable) failed 0x%08lX\n", err);
			return HRESULT_FROM_WIN32(err);
		}
		realCaptureEngineInitialize = (CaptureEngineInitializeFn)*entry;
		*entry = (void *)&HookCaptureEngineInitialize;
		VirtualProtect(entry, sizeof(void *), oldProtect, &oldProtect);
	}
	if (verbose) printf("IMFCaptureEngine::Initialize hooked\n");

	hookedEngine = engine.Detach();
	return S_OK;
}

// Prints a media subtype: a FourCC when it's built on MFVideoFormat_Base, otherwise the GUID.
static void FormatSubtype(const GUID &g, char *out, size_t size)
{
	static const BYTE baseData4[8] = { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
	if (g.Data2 == 0x0000 && g.Data3 == 0x0010 && memcmp(g.Data4, baseData4, 8) == 0)
	{
		char fcc[5];
		for (int i = 0; i < 4; i++)
		{
			char c = (char)((g.Data1 >> (8 * i)) & 0xFF);
			fcc[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
		}
		fcc[4] = 0;
		sprintf_s(out, size, "%s (0x%08lX)", fcc, (unsigned long)g.Data1);
	}
	else
	{
		sprintf_s(out, size, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", (unsigned long)g.Data1, g.Data2, g.Data3,
			g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
	}
}

static void PrintMediaType(const char *prefix, IMFMediaType *type)
{
	GUID major = {}, subtype = {};
	type->GetGUID(MF_MT_MAJOR_TYPE_, &major);
	type->GetGUID(MF_MT_SUBTYPE_, &subtype);
	char sub[64];
	FormatSubtype(subtype, sub, sizeof(sub));
	const char *majorName = IsEqualGUID(major, MFMediaType_Video_) ? "video" :
		IsEqualGUID(major, MFMediaType_Image_) ? "image" : "other";

	UINT64 frameSize = 0, frameRate = 0;
	type->GetUINT64(MF_MT_FRAME_SIZE_, &frameSize);
	type->GetUINT64(MF_MT_FRAME_RATE_, &frameRate);
	UINT32 stride = 0, sampleSize = 0;
	type->GetUINT32(MF_MT_DEFAULT_STRIDE_, &stride);
	type->GetUINT32(MF_MT_SAMPLE_SIZE_, &sampleSize);
	UINT32 rateDen = (UINT32)frameRate;
	printf("%s%s %s %ux%u stride %d sample %u fps %u/%u\n", prefix, majorName, sub,
		(UINT32)(frameSize >> 32), (UINT32)frameSize, (int)stride, sampleSize, (UINT32)(frameRate >> 32), rateDen);
}

// Lists the native media types of every stream of the camera (IMFCaptureSource of the engine of MediaCapture).
static HRESULT ListNativeFormats()
{
	if (capturedEngine == nullptr)
	{
		printf("The capture engine of MediaCapture wasn't caught\n");
		return E_FAIL;
	}
	ComPtr<IMFCaptureSource_> source;
	HRESULT hr = capturedEngine->GetSource(&source);
	CHECK(hr, "IMFCaptureEngine::GetSource");

	DWORD streamCount = 0;
	hr = source->GetDeviceStreamCount(&streamCount);
	CHECK(hr, "IMFCaptureSource::GetDeviceStreamCount");

	// MF_CAPTURE_ENGINE_STREAM_CATEGORY
	static const char *categories[] = { "video preview", "video capture", "photo independent", "photo dependent", "audio", "unsupported" };
	for (DWORD s = 0; s < streamCount; s++)
	{
		int category = -1;
		source->GetDeviceStreamCategory(s, &category);
		printf("Stream %lu: category %d (%s)\n", s, category,
			(category >= 0 && category < 6) ? categories[category] : "?");

		ComPtr<IMFMediaType> current;
		if (SUCCEEDED(source->GetCurrentDeviceMediaType(s, &current)))
		{
			PrintMediaType("  current: ", current.Get());
		}
		for (DWORD t = 0;; t++)
		{
			ComPtr<IMFMediaType> type;
			hr = source->GetAvailableDeviceMediaType(s, t, &type);
			if (FAILED(hr)) break;
			char prefix[32];
			sprintf_s(prefix, sizeof(prefix), "  [%lu] ", t);
			PrintMediaType(prefix, type.Get());
		}
	}
	return S_OK;
}

// Raw Bayer capture.
// qccamavs8930.sys only adds its raw Bayer data ranges (MIPI, QCOM, PL16, BA81, DPCM) to the still pin
// in "PT" (production test) mode: CaptureFilter13MP::CreateImagePinFactory picks its 10 PT ranges when
// HKLM\Software\Microsoft\ManufacturingOS ManufacturingMode = 1 (sub_401A78), otherwise a single NV12 range.
// In PT mode the still pin (stream 3) reports subtypes that aren't FourCCs, e.g. {009D4DF4-0A60-4020-A779-A67611E7684A}.
// So "raw" is: a known Bayer FourCC, or any subtype not built on MFVideoFormat_Base except the image formats.
static const GUID MFImageFormat_JPEG_ = { 0x19e4a5aa, 0x5662, 0x4fc5, { 0xa0, 0xc0, 0x17, 0x58, 0x02, 0x8e, 0x10, 0x57 } };
static BOOL IsBayerSubtype(const GUID &g)
{
	static const DWORD bayer[] = { 0x4950494D /* MIPI */, 0x4D4F4351 /* QCOM */, 0x36314C50 /* PL16 */,
		0x31384142 /* BA81 */, 0x4D435044 /* DPCM */ };
	static const BYTE baseData4[8] = { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
	if (g.Data2 != 0x0000 || g.Data3 != 0x0010 || memcmp(g.Data4, baseData4, 8) != 0)
	{
		return !IsEqualGUID(g, MFImageFormat_JPEG_) && !IsEqualGUID(g, GUID{}); // vendor raw GUID
	}
	for (DWORD f : bayer) if (g.Data1 == f) return TRUE;
	return FALSE;
}

// Receives the raw still from the photo sink.
class RawSampleCallback : public IMFCaptureEngineOnSampleCallback_
{
	LONG refCount = 1;
public:
	HANDLE done = CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS);
	ComPtr<IMFSample> sample;
	~RawSampleCallback() { if (done) CloseHandle(done); }
	STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
	{
		if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IMFCaptureEngineOnSampleCallback_)))
		{
			*ppv = static_cast<IMFCaptureEngineOnSampleCallback_ *>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refCount); }
	STDMETHODIMP_(ULONG) Release() override
	{
		LONG r = InterlockedDecrement(&refCount);
		if (r == 0) delete this;
		return r;
	}
	STDMETHODIMP OnSample(IMFSample *s) override
	{
		if (verbose) printf("IMFCaptureEngineOnSampleCallback::OnSample %p\n", (void *)s);
		if (s != nullptr && !sample) sample = s;
		SetEvent(done);
		return S_OK;
	}
};

static HRESULT WriteFileData(const wchar_t *path, const void *data, DWORD size)
{
	HANDLE hFile = CreateFile2(path, GENERIC_WRITE, 0, CREATE_ALWAYS, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		DWORD err = GetLastError();
		printf("CreateFile2(%ls) failed 0x%08lX\n", path, err);
		return HRESULT_FROM_WIN32(err);
	}
	DWORD written = 0;
	BOOL ok = WriteFile(hFile, data, size, &written, NULL);
	DWORD err = GetLastError();
	CloseHandle(hFile);
	if (!ok || written != size)
	{
		printf("WriteFile(%ls) failed 0x%08lX\n", path, err);
		return HRESULT_FROM_WIN32(err);
	}
	return S_OK;
}

// Sets the device media type of a stream and waits for MF_CAPTURE_SOURCE_CURRENT_DEVICE_MEDIA_TYPE_SET
// (the events are intercepted by eventFilter during the raw capture).
static HRESULT SetRawDeviceMediaType(IMFCaptureSource_ *source, DWORD stream, IMFMediaType *type)
{
	ResetEvent(eventFilter->mediaTypeSet);
	HRESULT hr = source->SetCurrentDeviceMediaType(stream, type);
	if (FAILED(hr))
	{
		printf("IMFCaptureSource::SetCurrentDeviceMediaType failed 0x%08lX\n", (unsigned long)hr);
		return hr;
	}
	if (WaitForSingleObjectEx(eventFilter->mediaTypeSet, 10000, FALSE) != WAIT_OBJECT_0)
	{
		printf("No MF_CAPTURE_SOURCE_CURRENT_DEVICE_MEDIA_TYPE_SET after 10 s\n");
		return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
	}
	if (FAILED(eventFilter->mediaTypeStatus))
	{
		printf("The device media type wasn't set 0x%08lX\n", (unsigned long)eventFilter->mediaTypeStatus);
		return eventFilter->mediaTypeStatus;
	}
	if (verbose) printf("Device media type of stream %lu set\n", stream);
	return S_OK;
}

// Puts back the media type the stream had before the raw capture, then gives the events back to MediaCapture.
static void RestoreDeviceMediaType(IMFCaptureSource_ *source, DWORD stream, IMFMediaType *previousType)
{
	if (previousType)
	{
		pendingRawType = previousType;
		SetRawDeviceMediaType(source, stream, previousType);
	}
	pendingRawType.Reset();
	InterlockedExchange(&eventFilter->intercept, 0);
}

// Takes a still in the first Bayer media type found (or streamIndex:typeIndex when given),
// through the photo sink of the engine of MediaCapture, and writes the buffer as it is.
// Finds the ideal-raw type (vendor subtype of the PT still pin) with the largest sample size. Used by -g:
// a still in that type makes MultiPass::CreateIdealRawHeap reallocate its heap (it reuses the heap kept from the
// previous camera session when it's big enough, and then the raw frames come back filled with zeros).
static BOOL FindLargestIdealRawType(int *streamOut, int *typeOut)
{
	static const GUID idealRaw = { 0x009D4DF4, 0x0A60, 0x4020, { 0xA7, 0x79, 0xA6, 0x76, 0x11, 0xE7, 0x68, 0x4A } };
	ComPtr<IMFCaptureSource_> source;
	if (capturedEngine == nullptr || FAILED(capturedEngine->GetSource(&source))) return FALSE;
	DWORD streamCount = 0;
	source->GetDeviceStreamCount(&streamCount);
	UINT32 best = 0;
	for (DWORD s = 0; s < streamCount; s++)
	{
		for (DWORD t = 0;; t++)
		{
			ComPtr<IMFMediaType> type;
			if (FAILED(source->GetAvailableDeviceMediaType(s, t, &type))) break;
			GUID subtype = {};
			type->GetGUID(MF_MT_SUBTYPE_, &subtype);
			UINT32 sampleSize = 0;
			type->GetUINT32(MF_MT_SAMPLE_SIZE_, &sampleSize);
			if (IsEqualGUID(subtype, idealRaw) && sampleSize > best)
			{
				best = sampleSize;
				*streamOut = (int)s;
				*typeOut = (int)t;
			}
		}
	}
	return best != 0;
}

static HRESULT CaptureRawPhoto(const wchar_t *outputPath, int forcedStream, int forcedType)
{
	if (capturedEngine == nullptr)
	{
		printf("The capture engine of MediaCapture wasn't caught\n");
		return E_FAIL;
	}
	ComPtr<IMFCaptureSource_> source;
	HRESULT hr = capturedEngine->GetSource(&source);
	CHECK(hr, "IMFCaptureEngine::GetSource");

	DWORD streamCount = 0;
	hr = source->GetDeviceStreamCount(&streamCount);
	CHECK(hr, "IMFCaptureSource::GetDeviceStreamCount");

	DWORD stream = 0;
	ComPtr<IMFMediaType> rawType;
	if (forcedStream >= 0)
	{
		stream = (DWORD)forcedStream;
		hr = source->GetAvailableDeviceMediaType(stream, (DWORD)forcedType, &rawType);
		CHECK(hr, "IMFCaptureSource::GetAvailableDeviceMediaType");
	}
	else
	{
		// The PT ranges are shared with other Nokia sensors (up to 41 MP): take the raw type closest to the
		// largest NV12 size of the camera (2592x1936 on the Lumia 520 → raw 2592x1944)
		UINT32 refWidth = 0, refHeight = 0;
		for (DWORD s = 0; s < streamCount; s++)
		{
			for (DWORD t = 0;; t++)
			{
				ComPtr<IMFMediaType> type;
				if (FAILED(source->GetAvailableDeviceMediaType(s, t, &type))) break;
				GUID subtype = {};
				type->GetGUID(MF_MT_SUBTYPE_, &subtype);
				if (IsBayerSubtype(subtype)) continue;
				UINT64 frameSize = 0;
				type->GetUINT64(MF_MT_FRAME_SIZE_, &frameSize);
				if ((frameSize >> 32) * (UINT32)frameSize > (UINT64)refWidth * refHeight)
				{
					refWidth = (UINT32)(frameSize >> 32);
					refHeight = (UINT32)frameSize;
				}
			}
		}
		UINT64 bestDistance = ~0ULL;
		for (DWORD s = 0; s < streamCount; s++)
		{
			for (DWORD t = 0;; t++)
			{
				ComPtr<IMFMediaType> type;
				if (FAILED(source->GetAvailableDeviceMediaType(s, t, &type))) break;
				GUID subtype = {};
				type->GetGUID(MF_MT_SUBTYPE_, &subtype);
				if (!IsBayerSubtype(subtype)) continue;
				UINT64 frameSize = 0;
				type->GetUINT64(MF_MT_FRAME_SIZE_, &frameSize);
				INT64 dw = (INT64)(frameSize >> 32) - refWidth, dh = (INT64)(UINT32)frameSize - refHeight;
				UINT64 distance = (UINT64)(dw * dw + dh * dh);
				if (distance < bestDistance)
				{
					bestDistance = distance;
					stream = s;
					rawType = type;
				}
			}
		}
		if (!rawType)
		{
			printf("No raw Bayer media type exposed by the camera (see -l).\n");
			printf("The driver only exposes them when HKLM\\Software\\Microsoft\\ManufacturingOS ManufacturingMode = 1\n");
			return MF_E_INVALIDMEDIATYPE;
		}
	}
	printf("Raw stream %lu: ", stream);
	PrintMediaType("", rawType.Get());

	if (eventFilter == nullptr)
	{
		printf("The event callback of the engine wasn't wrapped\n");
		return E_FAIL;
	}

	ComPtr<IMFMediaType> previousType;
	source->GetCurrentDeviceMediaType(stream, &previousType);
	pendingRawType = rawType;
	pendingRawStream = stream;
	// From here the events of the engine are kept from MediaCapture (see EventCallbackFilter)
	ResetEvent(eventFilter->mediaTypeSet);
	ResetEvent(eventFilter->photoTaken);
	InterlockedExchange(&eventFilter->intercept, 1);
	hr = SetRawDeviceMediaType(source.Get(), stream, rawType.Get());
	if (FAILED(hr))
	{
		RestoreDeviceMediaType(source.Get(), stream, previousType.Get());
		return hr;
	}

	ComPtr<IMFCaptureSink_> sink;
	ComPtr<IMFCapturePhotoSink_> photoSink;
	RawSampleCallback *callback = new RawSampleCallback();
	hr = capturedEngine->GetSink(CAPTURE_SINK_TYPE_PHOTO, &sink);
	if (FAILED(hr)) printf("IMFCaptureEngine::GetSink(photo) failed 0x%08lX\n", (unsigned long)hr);
	if (SUCCEEDED(hr))
	{
		hr = sink.As(&photoSink);
		if (FAILED(hr)) printf("QueryInterface(IMFCapturePhotoSink) failed 0x%08lX\n", (unsigned long)hr);
	}
	if (SUCCEEDED(hr))
	{
		hr = photoSink->RemoveAllStreams();
		if (FAILED(hr)) printf("IMFCapturePhotoSink::RemoveAllStreams failed 0x%08lX\n", (unsigned long)hr);
	}
	if (SUCCEEDED(hr))
	{
		DWORD sinkStream = 0;
		// Same type in and out: no converter nor encoder between the source and the callback
		hr = photoSink->AddStream(stream, rawType.Get(), nullptr, &sinkStream);
		if (FAILED(hr)) printf("IMFCapturePhotoSink::AddStream failed 0x%08lX\n", (unsigned long)hr);
	}
	if (SUCCEEDED(hr))
	{
		hr = photoSink->SetSampleCallback(callback);
		if (FAILED(hr)) printf("IMFCapturePhotoSink::SetSampleCallback failed 0x%08lX\n", (unsigned long)hr);
	}
	if (SUCCEEDED(hr))
	{
		hr = capturedEngine->TakePhoto();
		if (FAILED(hr)) printf("IMFCaptureEngine::TakePhoto failed 0x%08lX\n", (unsigned long)hr);
	}
	if (SUCCEEDED(hr))
	{
		if (verbose) printf("IMFCaptureEngine::TakePhoto OK\n");
		HANDLE handles[2] = { callback->done, eventFilter->photoTaken };
		DWORD wait = WaitForMultipleObjectsEx(2, handles, FALSE, ASYNC_TIMEOUT_MS, FALSE);
		if (wait == WAIT_OBJECT_0 + 1)
		{
			// PHOTO_TAKEN (or an error) before the sample
			if (FAILED(eventFilter->photoStatus))
			{
				printf("The photo failed 0x%08lX\n", (unsigned long)eventFilter->photoStatus);
				hr = eventFilter->photoStatus;
			}
			else
			{
				wait = WaitForSingleObjectEx(callback->done, 5000, FALSE);
			}
		}
		if (SUCCEEDED(hr) && wait == WAIT_TIMEOUT)
		{
			printf("No raw sample after %u ms\n", ASYNC_TIMEOUT_MS);
			hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
		}
		else if (SUCCEEDED(hr) && !callback->sample)
		{
			printf("The photo sink returned no sample\n");
			hr = E_FAIL;
		}
	}

	if (SUCCEEDED(hr))
	{
		ComPtr<IMFMediaBuffer> buffer;
		hr = callback->sample->ConvertToContiguousBuffer(&buffer);
		if (SUCCEEDED(hr))
		{
			BYTE *data = nullptr;
			DWORD length = 0;
			hr = buffer->Lock(&data, nullptr, &length);
			if (SUCCEEDED(hr))
			{
				hr = WriteFileData(outputPath, data, length);
				buffer->Unlock();
				if (SUCCEEDED(hr)) printf("Raw picture saved: %ls (%lu bytes)\n", outputPath, length);

				// Sidecar with the format, for the conversion on the PC
				GUID subtype = {};
				rawType->GetGUID(MF_MT_SUBTYPE_, &subtype);
				char sub[64];
				FormatSubtype(subtype, sub, sizeof(sub));
				UINT64 frameSize = 0;
				rawType->GetUINT64(MF_MT_FRAME_SIZE_, &frameSize);
				UINT32 stride = 0;
				rawType->GetUINT32(MF_MT_DEFAULT_STRIDE_, &stride);
				// format: the FourCC when there's one. The vendor subtype of the PT still pin is the VFE "ideal raw"
				// output (rawOutputFormat 21 in the driver trace): Qualcomm packed 10-bit (6 pixels in 8 bytes),
				// rows of ceil(width / 6) * 8 bytes, Bayer order GBRG (sensor GRBG + flip + mirror). Checked on
				// a 2592x1944 frame: 1948 rows of image, then other data.
				static const GUID idealRaw = { 0x009D4DF4, 0x0A60, 0x4020, { 0xA7, 0x79, 0xA6, 0x76, 0x11, 0xE7, 0x68, 0x4A } };
				char fourcc[5] = "";
				const char *order = "";
				UINT32 width = (UINT32)(frameSize >> 32);
				if (sub[0] != '{') { memcpy(fourcc, sub, 4); fourcc[4] = 0; }
				if (IsEqualGUID(subtype, idealRaw))
				{
					strcpy_s(fourcc, sizeof(fourcc), "QCOM");
					order = "GBRG";
					stride = (width + 5) / 6 * 8;
				}
				char text[320];
				int n = sprintf_s(text, sizeof(text), "format=%s\nsubtype=%s\nwidth=%u\nheight=%u\nstride=%d\norder=%s\nbytes=%lu\n",
					fourcc, sub, width, (UINT32)frameSize, (int)stride, order, length);
				wchar_t sidecar[MAX_PATH];
				swprintf_s(sidecar, MAX_PATH, L"%s.txt", outputPath);
				if (SUCCEEDED(hr) && n > 0) WriteFileData(sidecar, text, (DWORD)n);
			}
		}
		if (FAILED(hr)) printf("Cannot read the raw sample 0x%08lX\n", (unsigned long)hr);
	}

	if (photoSink)
	{
		photoSink->SetSampleCallback(nullptr);
		photoSink->RemoveAllStreams();
	}
	callback->Release();
	RestoreDeviceMediaType(source.Get(), stream, previousType.Get());
	return hr;
}

// Waits for the end of an asynchronous action (polling, the process has no message loop).
static HRESULT WaitAction(WF::IAsyncAction *action, const char *what)
{
	ComPtr<WF::IAsyncInfo> info;
	HRESULT hr = action->QueryInterface(__uuidof(WF::IAsyncInfo), (void **)info.GetAddressOf());
	if (FAILED(hr)) return hr;

	// AsyncStatus: 0 Started, 1 Completed, 2 Canceled, 3 Error
	DWORD elapsed = 0;
	for (;;)
	{
		AsyncStatus status;
		hr = info->get_Status(&status);
		if (FAILED(hr)) return hr;

		int s = (int)status;
		if (s == 1)
		{
			return action->GetResults();
		}
		if (s == 2)
		{
			printf("%s was canceled\n", what);
			return E_ABORT;
		}
		if (s == 3)
		{
			HRESULT errorCode = E_FAIL;
			info->get_ErrorCode(&errorCode);
			return errorCode;
		}
		if (elapsed >= ASYNC_TIMEOUT_MS)
		{
			printf("%s timed out after %lu ms\n", what, elapsed);
			info->Cancel();
			return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
		}
		Sleep(10);
		elapsed += 10;
	}
}

static void GetSize(WMP::IMediaEncodingProperties *props, UINT32 *width, UINT32 *height)
{
	*width = 0;
	*height = 0;
	ComPtr<WMP::IImageEncodingProperties> image;
	ComPtr<WMP::IVideoEncodingProperties> video;
	if (SUCCEEDED(props->QueryInterface(__uuidof(WMP::IImageEncodingProperties), (void **)image.GetAddressOf())))
	{
		image->get_Width(width);
		image->get_Height(height);
	}
	else if (SUCCEEDED(props->QueryInterface(__uuidof(WMP::IVideoEncodingProperties), (void **)video.GetAddressOf())))
	{
		video->get_Width(width);
		video->get_Height(height);
	}
}

static BOOL SameAspectRatio(UINT32 w1, UINT32 h1, UINT32 w2, UINT32 h2)
{
	// Tolerance of 2% (2592x1936 isn't exactly 4:3)
	UINT64 a = (UINT64)w1 * h2;
	UINT64 b = (UINT64)w2 * h1;
	UINT64 diff = a > b ? a - b : b - a;
	return diff * 50 <= (a > b ? a : b);
}

// Selects the largest resolution of a stream. When aspectWidth/aspectHeight aren't 0,
// only the resolutions with the same aspect ratio are considered.
static HRESULT SelectLargestResolution(WMD::IMediaDeviceController *controller, WMC::MediaStreamType streamType,
	const char *name, UINT32 aspectWidth, UINT32 aspectHeight, UINT32 *selectedWidth, UINT32 *selectedHeight)
{
	char what[128];
	ComPtr<WFC::IVectorView<WMP::IMediaEncodingProperties *>> list;
	HRESULT hr = controller->GetAvailableMediaStreamProperties(streamType, &list);
	sprintf_s(what, sizeof(what), "GetAvailableMediaStreamProperties(%s)", name);
	CHECK(hr, what);

	unsigned int count = 0;
	hr = list->get_Size(&count);
	CHECK(hr, "IVectorView::get_Size");

	ComPtr<WMP::IMediaEncodingProperties> best;
	UINT64 bestPixels = 0;
	for (unsigned int i = 0; i < count; i++)
	{
		ComPtr<WMP::IMediaEncodingProperties> props;
		if (FAILED(list->GetAt(i, &props))) continue;

		UINT32 width, height;
		GetSize(props.Get(), &width, &height);
		if (verbose)
		{
			HStr subtype;
			props->get_Subtype(&subtype);
			printf("  %s format %u: %ux%u %ls\n", name, i, width, height, subtype.c_str());
		}
		if (aspectWidth != 0 && aspectHeight != 0 && !SameAspectRatio(width, height, aspectWidth, aspectHeight))
		{
			continue;
		}
		if ((UINT64)width * height > bestPixels)
		{
			bestPixels = (UINT64)width * height;
			best = props;
		}
	}
	if (!best)
	{
		printf("No %s resolution available\n", name);
		return E_FAIL;
	}

	UINT32 width, height;
	GetSize(best.Get(), &width, &height);
	printf("%s resolution: %ux%u\n", name, width, height);
	if (selectedWidth) *selectedWidth = width;
	if (selectedHeight) *selectedHeight = height;

	ComPtr<WF::IAsyncAction> action;
	hr = controller->SetMediaStreamPropertiesAsync(streamType, best.Get(), &action);
	sprintf_s(what, sizeof(what), "SetMediaStreamPropertiesAsync(%s)", name);
	CHECK(hr, what);
	hr = WaitAction(action.Get(), what);
	CHECK(hr, "SetMediaStreamPropertiesAsync completion");
	return S_OK;
}

// Starts a preview to the preview sink of VideoSink_RT.dll, so the camera streams
// (and the 3A algorithms converge) without any UI.
static HRESULT StartPreview(WMC::IMediaCapture *capture, WMD::IMediaDeviceController *controller)
{
	// The sensor keeps the field of view of the preview for the photo: a 16:9 preview gives
	// a 16:9 picture letterboxed in a 4:3 photo. So the preview uses the aspect ratio of the photo.
	ComPtr<WMP::IMediaEncodingProperties> photoProps;
	HRESULT hr = controller->GetMediaStreamProperties(WMC::MediaStreamType_Photo, &photoProps);
	CHECK(hr, "GetMediaStreamProperties(Photo)");
	UINT32 photoWidth, photoHeight;
	GetSize(photoProps.Get(), &photoWidth, &photoHeight);
	if (FAILED(SelectLargestResolution(controller, WMC::MediaStreamType_VideoPreview, "Preview",
		photoWidth, photoHeight, nullptr, nullptr)))
	{
		printf("Warning: no preview resolution with the aspect ratio of the photo (%ux%u)\n", photoWidth, photoHeight);
	}

	ComPtr<WMP::IMediaEncodingProperties> previewProps;
	hr = controller->GetMediaStreamProperties(WMC::MediaStreamType_VideoPreview, &previewProps);
	CHECK(hr, "GetMediaStreamProperties(VideoPreview)");

	ComPtr<WMP::IVideoEncodingProperties> previewVideo;
	hr = previewProps.As(&previewVideo);
	CHECK(hr, "QueryInterface(IVideoEncodingProperties)");
	if (verbose)
	{
		UINT32 width, height;
		GetSize(previewProps.Get(), &width, &height);
		HStr subtype;
		previewProps->get_Subtype(&subtype);
		printf("  preview format: %ux%u %ls\n", width, height, subtype.c_str());
	}

	ComPtr<WMP::IMediaEncodingProfile> profile;
	hr = ActivateInstance(WINDOWS_MEDIA_DLL, L"Windows.Media.MediaProperties.MediaEncodingProfile",
		__uuidof(WMP::IMediaEncodingProfile), (void **)profile.GetAddressOf());
	CHECK(hr, "Activate MediaEncodingProfile");
	hr = profile->put_Video(previewVideo.Get());
	CHECK(hr, "MediaEncodingProfile::put_Video");

	ComPtr<WM::IMediaExtension> sink;
	hr = ActivateInstance(VIDEOSINK_RT_DLL, L"Windows.Phone.Media.Capture.MediaCapturePreviewSink",
		__uuidof(WM::IMediaExtension), (void **)sink.GetAddressOf());
	CHECK(hr, "Activate MediaCapturePreviewSink");

	ComPtr<WMC::IMediaCaptureVideoPreview> preview;
	hr = capture->QueryInterface(__uuidof(WMC::IMediaCaptureVideoPreview), (void **)preview.GetAddressOf());
	CHECK(hr, "QueryInterface(IMediaCaptureVideoPreview)");

	ComPtr<WF::IAsyncAction> action;
	hr = preview->StartPreviewToCustomSinkAsync(profile.Get(), sink.Get(), &action);
	CHECK(hr, "StartPreviewToCustomSinkAsync");
	hr = WaitAction(action.Get(), "StartPreviewToCustomSinkAsync");
	CHECK(hr, "StartPreviewToCustomSinkAsync completion");
	return S_OK;
}

static HRESULT StopPreview(WMC::IMediaCapture *capture)
{
	ComPtr<WMC::IMediaCaptureVideoPreview> preview;
	HRESULT hr = capture->QueryInterface(__uuidof(WMC::IMediaCaptureVideoPreview), (void **)preview.GetAddressOf());
	CHECK(hr, "QueryInterface(IMediaCaptureVideoPreview)");

	ComPtr<WF::IAsyncAction> action;
	hr = preview->StopPreviewAsync(&action);
	CHECK(hr, "StopPreviewAsync");
	hr = WaitAction(action.Get(), "StopPreviewAsync");
	CHECK(hr, "StopPreviewAsync completion");
	return S_OK;
}

// Captures a JPEG directly into a stream on the output file.
static HRESULT CapturePhoto(WMC::IMediaCapture *capture, const wchar_t *outputPath)
{
	ComPtr<WMP::IImageEncodingPropertiesStatics> imageStatics;
	HRESULT hr = GetFactory(WINDOWS_MEDIA_DLL, L"Windows.Media.MediaProperties.ImageEncodingProperties",
		__uuidof(WMP::IImageEncodingPropertiesStatics), (void **)imageStatics.GetAddressOf());
	CHECK(hr, "Get ImageEncodingProperties statics");

	ComPtr<WMP::IImageEncodingProperties> jpeg;
	hr = imageStatics->CreateJpeg(&jpeg);
	CHECK(hr, "ImageEncodingProperties::CreateJpeg");

	// CreateRandomAccessStreamOnFile only opens an existing file, and doesn't truncate it:
	// create (or truncate) the file first.
	HANDLE hFile = CreateFile2(outputPath, GENERIC_WRITE, 0, CREATE_ALWAYS, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		DWORD err = GetLastError();
		printf("CreateFile2(%ls) failed 0x%08lX\n", outputPath, err);
		return HRESULT_FROM_WIN32(err);
	}
	CloseHandle(hFile);

	// Resolved at runtime from SHCORE.DLL rather than through the api-ms-win-shcore-stream-winrt api set.
	HMODULE hShcore = win32Api.LoadLibraryA("C:\\Windows\\System32\\SHCORE.DLL");
	decltype(&::CreateRandomAccessStreamOnFile) pCreateRandomAccessStreamOnFile = hShcore == NULL ? nullptr :
		reinterpret_cast<decltype(&::CreateRandomAccessStreamOnFile)>(GetProcAddress(hShcore, "CreateRandomAccessStreamOnFile"));
	if (pCreateRandomAccessStreamOnFile == nullptr)
	{
		DWORD err = GetLastError();
		printf("Cannot find CreateRandomAccessStreamOnFile in SHCORE.DLL 0x%08lX\n", err);
		return HRESULT_FROM_WIN32(err);
	}

	ComPtr<WSS::IRandomAccessStream> stream;
	hr = pCreateRandomAccessStreamOnFile(outputPath, 1 /* FileAccessMode_ReadWrite */,
		__uuidof(WSS::IRandomAccessStream), (void **)stream.GetAddressOf());
	if (FAILED(hr))
	{
		printf("CreateRandomAccessStreamOnFile(%ls) failed 0x%08lX\n", outputPath, (unsigned long)hr);
		return hr;
	}
	if (verbose) printf("CreateRandomAccessStreamOnFile OK\n");

	ComPtr<WF::IAsyncAction> action;
	hr = capture->CapturePhotoToStreamAsync(jpeg.Get(), stream.Get(), &action);
	CHECK(hr, "CapturePhotoToStreamAsync");
	HRESULT captureHr = WaitAction(action.Get(), "CapturePhotoToStreamAsync");

	UINT64 size = 0;
	stream->get_Size(&size);

	ComPtr<WF::IClosable> closable;
	if (SUCCEEDED(stream.As(&closable)))
	{
		closable->Close();
	}

	CHECK(captureHr, "CapturePhotoToStreamAsync completion");
	if (size == 0)
	{
		printf("The picture is empty\n");
		return E_FAIL;
	}
	printf("Picture saved: %ls (%llu bytes)\n", outputPath, (unsigned long long)size);
	return S_OK;
}

static HRESULT TakePicture(const wchar_t *outputPath, BOOL usePreview, DWORD previewDelayMs, BOOL largestResolution,
	BOOL shutterSound, BOOL listFormats, BOOL raw, int rawStream, int rawType, int rawCount, BOOL growHeap)
{
	// Before the initialization of MediaCapture, which creates and initializes the capture engine
	muteShutter = !shutterSound;
	if (FAILED(HookCaptureEngine()))
	{
		if (muteShutter) printf("Warning: the shutter sound stays enabled\n");
		if (listFormats) return E_FAIL;
	}

	// MediaCapture
	ComPtr<WMC::IMediaCapture> capture;
	HRESULT hr = ActivateInstance(WINDOWS_MEDIA_DLL, RuntimeClass_Windows_Media_Capture_MediaCapture,
		__uuidof(WMC::IMediaCapture), (void **)capture.GetAddressOf());
	CHECK(hr, "Activate MediaCapture");

	// Video only: no microphone
	ComPtr<WMC::IMediaCaptureInitializationSettings> settings;
	hr = ActivateInstance(WINDOWS_MEDIA_DLL, RuntimeClass_Windows_Media_Capture_MediaCaptureInitializationSettings,
		__uuidof(WMC::IMediaCaptureInitializationSettings), (void **)settings.GetAddressOf());
	CHECK(hr, "Activate MediaCaptureInitializationSettings");
	hr = settings->put_StreamingCaptureMode(WMC::StreamingCaptureMode_Video);
	CHECK(hr, "put_StreamingCaptureMode(Video)");

	// The camera resource is acquired here (mfpalwp.dll MFMCPalAcquireResources)
	ComPtr<WF::IAsyncAction> action;
	hr = capture->InitializeWithSettingsAsync(settings.Get(), &action);
	CHECK(hr, "InitializeWithSettingsAsync");
	hr = WaitAction(action.Get(), "InitializeWithSettingsAsync");
	CHECK(hr, "InitializeWithSettingsAsync completion");

	if (verbose)
	{
		ComPtr<WMC::IMediaCaptureSettings> captureSettings;
		if (SUCCEEDED(capture->get_MediaCaptureSettings(&captureSettings)))
		{
			HStr deviceId;
			captureSettings->get_VideoDeviceId(&deviceId);
			printf("  video device: %ls\n", deviceId.c_str());
		}
	}

	if (listFormats)
	{
		hr = ListNativeFormats();
		ComPtr<WF::IClosable> closable;
		if (SUCCEEDED(capture.As(&closable))) closable->Close();
		return hr;
	}

	ComPtr<WMD::IVideoDeviceController> videoController;
	hr = capture->get_VideoDeviceController(&videoController);
	CHECK(hr, "get_VideoDeviceController");
	ComPtr<WMD::IMediaDeviceController> controller;
	hr = videoController.As(&controller);
	CHECK(hr, "QueryInterface(IMediaDeviceController)");

	if (largestResolution)
	{
		// Not fatal: the default resolution is used if it fails
		SelectLargestResolution(controller.Get(), WMC::MediaStreamType_Photo, "Photo", 0, 0, nullptr, nullptr);
	}

	BOOL previewStarted = FALSE;
	if (usePreview)
	{
		hr = StartPreview(capture.Get(), controller.Get());
		if (FAILED(hr)) return hr;
		previewStarted = TRUE;
		if (verbose) printf("Waiting %lu ms\n", previewDelayMs);
		Sleep(previewDelayMs);
	}

	if (raw && growHeap)
	{
		int growStream = 0, growType = 0;
		if (FindLargestIdealRawType(&growStream, &growType))
		{
			wchar_t growPath[MAX_PATH];
			swprintf_s(growPath, MAX_PATH, L"%s.grow", outputPath);
			printf("Throwaway still in raw type %d:%d to reallocate the ideal-raw heap of the driver\n", growStream, growType);
			CaptureRawPhoto(growPath, growStream, growType);
		}
		else
		{
			printf("Warning: no ideal-raw type to grow the heap with\n");
		}
	}
	if (raw && rawCount > 1)
	{
		// Several raw stills in the same session: <name>_1.raw, <name>_2.raw, ...
		hr = S_OK;
		for (int i = 1; i <= rawCount; i++)
		{
			wchar_t path[MAX_PATH];
			const wchar_t *dot = wcsrchr(outputPath, L'.');
			size_t baseLength = dot ? (size_t)(dot - outputPath) : wcslen(outputPath);
			swprintf_s(path, MAX_PATH, L"%.*s_%d%s", (int)baseLength, outputPath, i, dot ? dot : L"");
			HRESULT hrOne = CaptureRawPhoto(path, rawStream, rawType);
			if (FAILED(hrOne)) hr = hrOne;
		}
	}
	else
	{
		hr = raw ? CaptureRawPhoto(outputPath, rawStream, rawType) : CapturePhoto(capture.Get(), outputPath);
	}

	if (previewStarted)
	{
		StopPreview(capture.Get());
	}

	// Release the camera
	ComPtr<WF::IClosable> closable;
	if (SUCCEEDED(capture.As(&closable)))
	{
		closable->Close();
	}
	return hr;
}

// Prints the exceptions that usually kill the process (code, address, module + offset), to find a crash in IDA.
static LONG CALLBACK CrashReporter(EXCEPTION_POINTERS *info)
{
	DWORD code = info->ExceptionRecord->ExceptionCode;
	if (code == 0xE06D7363 /* C++ */ || code == 0x406D1388 /* thread name */ || code == 0x40010006 /* debug string */ ||
		code == 0x4001000A || code == 0x000006BA /* RPC */ || code == 0x80000003 /* breakpoint */)
	{
		return EXCEPTION_CONTINUE_SEARCH;
	}
	void *address = info->ExceptionRecord->ExceptionAddress;
	HMODULE module = NULL;
	char name[MAX_PATH] = "?";
	if (GetModuleHandleExW(4 /* FROM_ADDRESS */ | 2 /* UNCHANGED_REFCOUNT */, (LPCWSTR)address, &module))
	{
		GetModuleFileNameA(module, name, MAX_PATH);
	}
	printf("Exception 0x%08lX at %p (%s+0x%lX), thread %lu", code, address, name,
		(unsigned long)((BYTE *)address - (BYTE *)module), GetCurrentThreadId());
	if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
	{
		printf(", %s %p", info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
			(void *)info->ExceptionRecord->ExceptionInformation[1]);
	}
	printf("\n");
	// LR and the stack words that point into a module: probable callers
	DWORD_PTR candidates[1 + 96];
	int n = 0;
	candidates[n++] = info->ContextRecord->Lr;
	DWORD_PTR *sp = (DWORD_PTR *)info->ContextRecord->Sp;
	for (int i = 0; i < 96; i++) candidates[n++] = sp[i];
	int printed = 0;
	for (int i = 0; i < n && printed < 16; i++)
	{
		HMODULE m = NULL;
		if (!GetModuleHandleExW(4 | 2, (LPCWSTR)candidates[i], &m) || m == NULL) continue;
		char mn[MAX_PATH] = "?";
		GetModuleFileNameA(m, mn, MAX_PATH);
		const char *shortName = strrchr(mn, '\\');
		printf("  %s %p %s+0x%lX\n", i == 0 ? "LR" : "sp", (void *)candidates[i], shortName ? shortName + 1 : mn,
			(unsigned long)(candidates[i] - (DWORD_PTR)m));
		printed++;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char *argv[])
{
	AddVectoredExceptionHandler(1, CrashReporter);

	// Unbuffered: nothing lost when the output is a pipe (ssh) and the process dies
	setvbuf(stdout, NULL, _IONBF, 0);

	const char *output = nullptr;
	BOOL usePreview = TRUE;
	BOOL largestResolution = TRUE;
	DWORD previewDelayMs = 2000;
	BOOL shutterSound = FALSE;
	BOOL listFormats = FALSE;
	BOOL raw = FALSE;
	int rawStream = -1, rawType = -1;
	int rawCount = 1;
	BOOL growHeap = FALSE;

	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
		{
			PrintUsage();
			return EXIT_SUCCESS;
		}
		else if (strcmp(argv[i], "-v") == 0)
		{
			verbose = TRUE;
		}
		else if (strcmp(argv[i], "-n") == 0)
		{
			usePreview = FALSE;
		}
		else if (strcmp(argv[i], "-p") == 0)
		{
			usePreview = TRUE; // default, kept for compatibility
		}
		else if (strcmp(argv[i], "-s") == 0)
		{
			largestResolution = FALSE;
		}
		else if (strcmp(argv[i], "-a") == 0)
		{
			shutterSound = TRUE;
		}
		else if (strcmp(argv[i], "-l") == 0)
		{
			listFormats = TRUE;
		}
		else if (strcmp(argv[i], "-g") == 0)
		{
			growHeap = TRUE;
		}
		else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
		{
			rawCount = atoi(argv[++i]);
			if (rawCount < 1) rawCount = 1;
		}
		else if (strcmp(argv[i], "-r") == 0)
		{
			raw = TRUE;
		}
		else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
		{
			if (sscanf_s(argv[++i], "%d:%d", &rawStream, &rawType) != 2 || rawStream < 0 || rawType < 0)
			{
				printf("Invalid -t %s (expected <stream>:<type>)\n", argv[i]);
				return EXIT_FAILURE;
			}
		}
		else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
		{
			previewDelayMs = (DWORD)strtoul(argv[++i], NULL, 10);
		}
		else if (argv[i][0] == '-')
		{
			printf("Unknown option %s\n", argv[i]);
			PrintUsage();
			return EXIT_FAILURE;
		}
		else
		{
			output = argv[i];
		}
	}

	if (output == nullptr) output = raw ? DEFAULT_RAW_OUTPUT : DEFAULT_OUTPUT;

	wchar_t outputPath[MAX_PATH];
	if (MultiByteToWideChar(CP_ACP, 0, output, -1, outputPath, MAX_PATH) == 0)
	{
		printf("Invalid output path %s\n", output);
		return EXIT_FAILURE;
	}

	HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
	if (FAILED(hr))
	{
		printf("RoInitialize failed 0x%08lX\n", (unsigned long)hr);
		return EXIT_FAILURE;
	}

	hr = TakePicture(outputPath, usePreview, previewDelayMs, largestResolution, shutterSound, listFormats,
		raw, rawStream, rawType, rawCount, growHeap);
	pendingRawType.Reset();

	RoUninitialize();
	return SUCCEEDED(hr) ? EXIT_SUCCESS : EXIT_FAILURE;
}
