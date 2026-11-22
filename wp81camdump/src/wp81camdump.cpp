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

// Timeout of the camera operations: 30 s, + 4 frames with a long manual exposure (-e)
static DWORD asyncTimeoutMs = 30000;

static Win32Api win32Api;
static BOOL verbose = FALSE;
static BOOL rawCaptureMode = FALSE; // -r
// Exposure time and ISO applied, read back from the controls (for the .txt of a raw still); 0 = auto/unknown
static INT64 appliedExposure100ns = 0;
static UINT32 appliedIso = 0;
// -r: exposure (µs) and analog gain (Q8, 256 = 1x) written to the sensor before the raw still; 0 = unchanged
static DWORD rawExposureUs = 0;
static DWORD rawGainQ8 = 0;
// -r: values read back from the sensor before the raw still (for the .txt); 0 = unknown
static DWORD sensorExposureUs = 0;
static DWORD sensorGainQ8 = 0;

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
	printf("  -s <w>x<h> Picture resolution, e.g. 2048x1536 (default: the largest one)\n");
	printf("             With -r: the raw resolution, e.g. 2600x1948 (default: the one of the sensor, 2592x1944)\n");
	printf("  -a         Audible: keep the shutter sound (default: no sound)\n");
	printf("  -l         List the picture resolutions (for -s), and exit. With -r: the raw resolutions\n");
	printf("             With -v: also the media types of every stream of the driver (debugging)\n");
	printf("  -r         Raw: save the still in a raw Bayer format of the sensor, as it is (+ a .txt with the format)\n");
	printf("             Sets HKLM\\Software\\Microsoft\\ManufacturingOS ManufacturingMode = 1 for the capture, then restores it\n");
	printf("  -c <n>     With -r: take <n> raw stills in the same session (<name>_1.raw, ...)\n");
	printf("  -g         With -r: first a throwaway still in the largest raw type, to make the driver reallocate\n");
	printf("             its raw buffer (only the first camera session after boot gets raw data otherwise)\n");
	printf("  -e <time>  Exposure time in seconds: 1/125, 0.5, ... up to 4, or auto (default: auto)\n");
	printf("             Sets HKLM\\Software\\OEM\\Nokia\\Camera\\RFC\\Caps MaxET = 4000000 (the 1 s default limit -> 4 s)\n");
	printf("  -i <iso>   ISO speed: 100, 800, ... or auto (default: auto)\n");
	printf("             With -r, -e and -i are written to the sensor itself: exposure, and analog gain = ISO / 100\n");
	printf("             (the .txt gives the values read back from the sensor)\n");
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

// IKsControl (ksproxy.h is desktop only)
struct KSPROPERTY_
{
	GUID Set;
	ULONG Id;
	ULONG Flags;
};
#define KSPROPERTY_TYPE_GET_ 0x00000001
#define KSPROPERTY_TYPE_SET_ 0x00000002

MIDL_INTERFACE("28F54685-06FD-11D2-B27A-00A0C9223196")
IKsControl_ : public IUnknown
{
public:
	virtual HRESULT STDMETHODCALLTYPE KsProperty(KSPROPERTY_ *property, ULONG propertyLength, void *data, ULONG dataLength,
		ULONG *bytesReturned) = 0;
	virtual HRESULT STDMETHODCALLTYPE KsMethod(void *method, ULONG methodLength, void *data, ULONG dataLength,
		ULONG *bytesReturned) = 0;
	virtual HRESULT STDMETHODCALLTYPE KsEvent(void *event, ULONG eventLength, void *data, ULONG dataLength,
		ULONG *bytesReturned) = 0;
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

static void PrintMediaType(const char *prefix, IMFMediaType *type, const char *suffix = "")
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
	printf("%s%s %s %ux%u stride %d sample %u fps %u/%u%s\n", prefix, majorName, sub,
		(UINT32)(frameSize >> 32), (UINT32)frameSize, (int)stride, sampleSize, (UINT32)(frameRate >> 32), rateDen, suffix);
}

// The size of the sensor, as far as the driver tells: its largest non-raw (NV12) size, 2592x1936 on the Lumia 520.
// The raw types of the PT still pin are a fixed table shared with other Nokia sensors (up to 41 MP), so most of them
// are larger than the sensor of the phone.
static void GetSensorSize(IMFCaptureSource_ *source, UINT32 *width, UINT32 *height)
{
	*width = 0;
	*height = 0;
	DWORD streamCount = 0;
	source->GetDeviceStreamCount(&streamCount);
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
			if ((frameSize >> 32) * (UINT32)frameSize > (UINT64)*width * *height)
			{
				*width = (UINT32)(frameSize >> 32);
				*height = (UINT32)frameSize;
			}
		}
	}
}

// The raw type used by -r without -s: the one closest to the sensor size (2592x1944 on the Lumia 520).
static BOOL FindDefaultRawType(IMFCaptureSource_ *source, DWORD *streamOut, DWORD *typeOut)
{
	UINT32 refWidth, refHeight;
	GetSensorSize(source, &refWidth, &refHeight);
	DWORD streamCount = 0;
	source->GetDeviceStreamCount(&streamCount);
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
				*streamOut = s;
				*typeOut = t;
			}
		}
	}
	return bestDistance != ~0ULL;
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
	printf("Media types of every stream of the driver (debugging):\n");
	int rawTypes = 0;
	UINT32 sensorWidth, sensorHeight;
	GetSensorSize(source.Get(), &sensorWidth, &sensorHeight);
	DWORD defaultStream = ~0u, defaultType = ~0u;
	FindDefaultRawType(source.Get(), &defaultStream, &defaultType);
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
			GUID subtype = {};
			type->GetGUID(MF_MT_SUBTYPE_, &subtype);
			BOOL raw = IsBayerSubtype(subtype);
			const char *note = "";
			if (raw)
			{
				rawTypes++;
				UINT64 frameSize = 0;
				type->GetUINT64(MF_MT_FRAME_SIZE_, &frameSize);
				UINT32 width = (UINT32)(frameSize >> 32), height = (UINT32)frameSize;
				// 3% margin: the raw frame has a few more rows/columns than the NV12 picture (2592x1944 for 2592x1936)
				if (s == defaultStream && t == defaultType) note = "  <- used by -r";
				else if (sensorWidth && ((UINT64)width * 100 > (UINT64)sensorWidth * 103 ||
					(UINT64)height * 100 > (UINT64)sensorHeight * 103))
				{
					note = "  (not this sensor: larger than its picture size)";
				}
			}
			char prefix[32];
			sprintf_s(prefix, sizeof(prefix), "  [%lu] %s", t, raw ? "RAW " : "");
			PrintMediaType(prefix, type.Get(), note);
		}
	}
	if (rawTypes == 0) printf("No RAW type: the driver only exposes them in manufacturing mode (-r)\n");
	return S_OK;
}

// Describes a raw subtype for the lists
static const char *RawFormatName(const GUID &subtype, char *buffer, size_t size)
{
	static const GUID idealRaw = { 0x009D4DF4, 0x0A60, 0x4020, { 0xA7, 0x79, 0xA6, 0x76, 0x11, 0xE7, 0x68, 0x4A } };
	if (IsEqualGUID(subtype, idealRaw)) return "10-bit Bayer GBRG, Qualcomm packed";
	if (subtype.Data1 == 0x4950494D /* MIPI */) return "Bayer, MIPI packed";
	FormatSubtype(subtype, buffer, size);
	return buffer;
}

// Finds the raw type with the given size (-r -s)
static BOOL FindRawTypeBySize(IMFCaptureSource_ *source, UINT32 width, UINT32 height, int *streamOut, int *typeOut)
{
	DWORD streamCount = 0;
	source->GetDeviceStreamCount(&streamCount);
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
			if ((UINT32)(frameSize >> 32) == width && (UINT32)frameSize == height)
			{
				*streamOut = (int)s;
				*typeOut = (int)t;
				return TRUE;
			}
		}
	}
	return FALSE;
}

// Lists the raw resolutions (-r -l), smallest first
static HRESULT ListRawResolutions()
{
	ComPtr<IMFCaptureSource_> source;
	if (capturedEngine == nullptr || FAILED(capturedEngine->GetSource(&source)))
	{
		printf("The capture engine of MediaCapture wasn't caught\n");
		return E_FAIL;
	}
	UINT32 sensorWidth, sensorHeight;
	GetSensorSize(source.Get(), &sensorWidth, &sensorHeight);
	DWORD defaultStream = ~0u, defaultType = ~0u;
	FindDefaultRawType(source.Get(), &defaultStream, &defaultType);

	struct RawEntry { UINT32 width, height; GUID subtype; BOOL isDefault; };
	RawEntry entries[32];
	int n = 0;
	DWORD streamCount = 0;
	source->GetDeviceStreamCount(&streamCount);
	for (DWORD s = 0; s < streamCount; s++)
	{
		for (DWORD t = 0; n < 32; t++)
		{
			ComPtr<IMFMediaType> type;
			if (FAILED(source->GetAvailableDeviceMediaType(s, t, &type))) break;
			GUID subtype = {};
			type->GetGUID(MF_MT_SUBTYPE_, &subtype);
			if (!IsBayerSubtype(subtype)) continue;
			UINT64 frameSize = 0;
			type->GetUINT64(MF_MT_FRAME_SIZE_, &frameSize);
			UINT32 width = (UINT32)(frameSize >> 32), height = (UINT32)frameSize;
			BOOL seen = FALSE;
			for (int j = 0; j < n; j++) seen |= entries[j].width == width && entries[j].height == height;
			if (seen) continue; // -s picks the first one
			entries[n].width = width;
			entries[n].height = height;
			entries[n].subtype = subtype;
			entries[n].isDefault = (s == defaultStream && t == defaultType);
			n++;
		}
	}
	if (n == 0)
	{
		printf("No raw resolution: the driver didn't switch to manufacturing mode\n");
		return MF_E_INVALIDMEDIATYPE;
	}
	for (int i = 1; i < n; i++)
	{
		for (int j = i; j > 0 && (UINT64)entries[j - 1].width * entries[j - 1].height > (UINT64)entries[j].width * entries[j].height; j--)
		{
			RawEntry e = entries[j];
			entries[j] = entries[j - 1];
			entries[j - 1] = e;
		}
	}
	printf("Raw resolutions (-r -s <width>x<height>):\n");
	for (int i = 0; i < n; i++)
	{
		char buffer[64], size[24];
		sprintf_s(size, sizeof(size), "%ux%u", entries[i].width, entries[i].height);
		// 3% margin: the raw frame has a few more rows/columns than the picture (2592x1944 for 2592x1936)
		BOOL larger = sensorWidth && ((UINT64)entries[i].width * 100 > (UINT64)sensorWidth * 103 ||
			(UINT64)entries[i].height * 100 > (UINT64)sensorHeight * 103);
		printf("  %-10s %s%s\n", size, RawFormatName(entries[i].subtype, buffer, sizeof(buffer)),
			entries[i].isDefault ? "  (default)" : larger ? "  (larger than the sensor)" : "");
	}
	printf("The driver has a fixed list of raw types, shared with other Nokia sensors (up to 41 MP). It accepts the\n");
	printf("larger ones, but the sensor still sends only its own frame (%ux%u + a few rows).\n", sensorWidth, sensorHeight);
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

// CameraDiagnostics property set of qccamavs8930.sys (CameraDiagnostics::ControlCamera), only in manufacturing mode:
// 5 = EXPOSURE_TIME (ULONG µs), 7 = GAIN_AND_EXPOSURE (ULONG µs + USHORT analog gain Q8). The setters write the sensor
// directly (sensor ioctl 0x43), without the 3A, then wait one frame interval (max 200 ms).
static const GUID PROPSETID_CAMERA_DIAGNOSTIC_ = { 0x2ed26bd2, 0x811f, 0x42e8, { 0xa8, 0x1a, 0x91, 0x99, 0xac, 0xe1, 0x9d, 0x36 } };
#define KSPROPERTY_CAMERA_DIAGNOSTIC_GAIN_AND_EXPOSURE_ 7

static HRESULT SensorGainAndExposure(BOOL set, DWORD *exposureUs, DWORD *gainQ8)
{
	ComPtr<IMFCaptureSource_> source;
	ComPtr<IUnknown> deviceSource;
	ComPtr<IKsControl_> ksControl;
	HRESULT hr = capturedEngine ? capturedEngine->GetSource(&source) : E_FAIL;
	if (SUCCEEDED(hr)) hr = source->GetCaptureDeviceSource(1 /* MF_CAPTURE_ENGINE_DEVICE_TYPE_VIDEO */, &deviceSource);
	if (SUCCEEDED(hr)) hr = deviceSource.As(&ksControl);
	if (FAILED(hr))
	{
		printf("Cannot get the IKsControl of the camera 0x%08lX\n", (unsigned long)hr);
		return hr;
	}
	KSPROPERTY_ property = { PROPSETID_CAMERA_DIAGNOSTIC_, KSPROPERTY_CAMERA_DIAGNOSTIC_GAIN_AND_EXPOSURE_,
		(ULONG)(set ? KSPROPERTY_TYPE_SET_ : KSPROPERTY_TYPE_GET_) };
	struct { DWORD exposureUs; WORD gainQ8; WORD reserved; } data = {};
	if (set)
	{
		data.exposureUs = *exposureUs;
		data.gainQ8 = (WORD)(*gainQ8 > 0xFFFF ? 0xFFFF : *gainQ8);
	}
	ULONG returned = 0;
	hr = ksControl->KsProperty(&property, sizeof(property), &data, sizeof(data), &returned);
	if (SUCCEEDED(hr) && !set)
	{
		*exposureUs = data.exposureUs;
		*gainQ8 = data.gainQ8;
	}
	return hr;
}

// -r: writes rawExposureUs/rawGainQ8 to the sensor (the -e/-i of the normal path don't reach it: the raw still is taken
// with the sensor settings of manufacturing mode), then reads the values back for the .txt.
static void ApplyRawExposure()
{
	DWORD exposureUs = 0, gainQ8 = 0;
	HRESULT hr = SensorGainAndExposure(FALSE, &exposureUs, &gainQ8);
	if (FAILED(hr))
	{
		printf("Warning: cannot read the sensor exposure 0x%08lX\n", (unsigned long)hr);
		return;
	}
	if (verbose) printf("  sensor before: exposure %.3f ms, analog gain %.2fx\n", exposureUs / 1000.0, gainQ8 / 256.0);
	if (rawExposureUs != 0 || rawGainQ8 != 0)
	{
		if (rawExposureUs != 0) exposureUs = rawExposureUs;
		if (rawGainQ8 != 0) gainQ8 = rawGainQ8;
		hr = SensorGainAndExposure(TRUE, &exposureUs, &gainQ8);
		if (FAILED(hr)) printf("Warning: cannot set the sensor exposure/gain 0x%08lX\n", (unsigned long)hr);
		// The frame being exposed still has the old values: wait for one with the new ones
		DWORD waitMs = exposureUs / 1000 * 2 + 200;
		if (waitMs > 10000) waitMs = 10000;
		Sleep(waitMs);
		asyncTimeoutMs = 30000 + exposureUs / 1000 * 4;
		if (FAILED(SensorGainAndExposure(FALSE, &exposureUs, &gainQ8))) exposureUs = gainQ8 = 0;
		if (rawExposureUs != 0 && exposureUs != 0 && exposureUs < rawExposureUs - rawExposureUs / 100)
		{
			// qccamsensor8930.sys converts the µs into sensor lines and caps them to 0xFFFF (16-bit coarse
			// integration register): 65535 lines of 98.09 µs = 6.43 s on the Lumia 520
			printf("Warning: the sensor limits the exposure to 65535 lines (%.3f ms here)\n", exposureUs / 1000.0);
		}
	}
	sensorExposureUs = exposureUs;
	sensorGainQ8 = gainQ8;
	printf("Sensor: exposure %.3f ms, analog gain %.2fx (ISO %lu if 1x = ISO 100)\n", exposureUs / 1000.0, gainQ8 / 256.0,
		(gainQ8 * 100 + 128) / 256);
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
		if (FAILED(hr))
		{
			printf("No media type %d:%d (0x%08lX)\n", forcedStream, forcedType, (unsigned long)hr);
			return hr;
		}
		GUID subtype = {};
		rawType->GetGUID(MF_MT_SUBTYPE_, &subtype);
		if (!IsBayerSubtype(subtype))
		{
			printf("Media type %d:%d isn't a raw type: ", forcedStream, forcedType);
			PrintMediaType("", rawType.Get());
			return MF_E_INVALIDMEDIATYPE;
		}
	}
	else
	{
		// The PT ranges are shared with other Nokia sensors (up to 41 MP): take the raw type closest to the
		// largest NV12 size of the camera (2592x1936 on the Lumia 520 → raw 2592x1944)
		DWORD defaultType = 0;
		if (FindDefaultRawType(source.Get(), &stream, &defaultType))
		{
			source->GetAvailableDeviceMediaType(stream, defaultType, &rawType);
		}
		if (!rawType)
		{
			printf("No raw Bayer media type exposed by the camera (see -r -l).\n");
			printf("The driver only exposes them when ManufacturingOS ManufacturingMode = 1 (set by -r)\n");
			return MF_E_INVALIDMEDIATYPE;
		}
	}
	if (verbose)
	{
		printf("Raw stream %lu: ", stream);
		PrintMediaType("", rawType.Get());
	}
	{
		UINT64 frameSize = 0;
		rawType->GetUINT64(MF_MT_FRAME_SIZE_, &frameSize);
		UINT32 width = (UINT32)(frameSize >> 32), height = (UINT32)frameSize;
		GUID subtype = {};
		rawType->GetGUID(MF_MT_SUBTYPE_, &subtype);
		static const GUID idealRaw = { 0x009D4DF4, 0x0A60, 0x4020, { 0xA7, 0x79, 0xA6, 0x76, 0x11, 0xE7, 0x68, 0x4A } };
		if (IsEqualGUID(subtype, idealRaw))
		{
			printf("Raw resolution: %ux%u, 10-bit Bayer GBRG, Qualcomm packed (6 pixels in 8 bytes, %u bytes per row)\n",
				width, height, (width + 5) / 6 * 8);
		}
		else
		{
			char sub[64];
			FormatSubtype(subtype, sub, sizeof(sub));
			printf("Raw resolution: %ux%u, format %s\n", width, height, sub);
		}
	}

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
		// After the raw media type is set (it may reconfigure the sensor), just before the still
		ApplyRawExposure();
		hr = capturedEngine->TakePhoto();
		if (FAILED(hr)) printf("IMFCaptureEngine::TakePhoto failed 0x%08lX\n", (unsigned long)hr);
	}
	if (SUCCEEDED(hr))
	{
		if (verbose) printf("IMFCaptureEngine::TakePhoto OK\n");
		HANDLE handles[2] = { callback->done, eventFilter->photoTaken };
		DWORD wait = WaitForMultipleObjectsEx(2, handles, FALSE, asyncTimeoutMs, FALSE);
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
			printf("No raw sample after %lu ms\n", asyncTimeoutMs);
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
				char text[400];
				// Read back from the sensor; iso: 100 per 1x of analog gain (assumption)
				int n = sprintf_s(text, sizeof(text),
					"format=%s\nsubtype=%s\nwidth=%u\nheight=%u\nstride=%d\norder=%s\nbytes=%lu\nexposure_us=%lu\niso=%lu\n"
					"gain_q8=%lu\n",
					fourcc, sub, width, (UINT32)frameSize, (int)stride, order, length, sensorExposureUs,
					(sensorGainQ8 * 100 + 128) / 256, sensorGainQ8);
				wchar_t sidecar[MAX_PATH];
				swprintf_s(sidecar, MAX_PATH, L"%s.txt", outputPath);
				if (SUCCEEDED(hr) && n > 0 && SUCCEEDED(WriteFileData(sidecar, text, (DWORD)n)))
				{
					printf("Raw format saved: %ls\n", sidecar);
				}
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
		if (elapsed >= asyncTimeoutMs)
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


// Lists the picture (JPEG) resolutions, for -s.
static void PrintPhotoResolutions(WMC::IMediaCapture *capture)
{
	ComPtr<WMD::IVideoDeviceController> videoController;
	ComPtr<WMD::IMediaDeviceController> controller;
	ComPtr<WFC::IVectorView<WMP::IMediaEncodingProperties *>> list;
	unsigned int count = 0;
	if (FAILED(capture->get_VideoDeviceController(&videoController)) || FAILED(videoController.As(&controller)) ||
		FAILED(controller->GetAvailableMediaStreamProperties(WMC::MediaStreamType_Photo, &list)) || FAILED(list->get_Size(&count)))
	{
		printf("Cannot list the picture resolutions\n");
		return;
	}
	UINT32 sizes[64][2];
	int n = 0;
	UINT64 largest = 0;
	for (unsigned int i = 0; i < count && n < 64; i++)
	{
		ComPtr<WMP::IMediaEncodingProperties> props;
		if (FAILED(list->GetAt(i, &props))) continue;
		UINT32 width, height;
		GetSize(props.Get(), &width, &height);
		BOOL seen = FALSE;
		for (int j = 0; j < n; j++) seen |= sizes[j][0] == width && sizes[j][1] == height;
		if (seen) continue;
		sizes[n][0] = width;
		sizes[n][1] = height;
		n++;
		if ((UINT64)width * height > largest) largest = (UINT64)width * height;
	}
	// Smallest first
	for (int i = 1; i < n; i++)
	{
		for (int j = i; j > 0 && (UINT64)sizes[j - 1][0] * sizes[j - 1][1] > (UINT64)sizes[j][0] * sizes[j][1]; j--)
		{
			UINT32 w = sizes[j][0], h = sizes[j][1];
			sizes[j][0] = sizes[j - 1][0];
			sizes[j][1] = sizes[j - 1][1];
			sizes[j - 1][0] = w;
			sizes[j - 1][1] = h;
		}
	}
	printf("Picture resolutions (-s <width>x<height>):\n");
	for (int j = 0; j < n; j++)
	{
		printf("  %ux%u%s\n", sizes[j][0], sizes[j][1], (UINT64)sizes[j][0] * sizes[j][1] == largest ? " (default)" : "");
	}
}

// Selects the largest resolution of a stream. When aspectWidth/aspectHeight aren't 0,
// only the resolutions with the same aspect ratio are considered.
// When requiredWidth/requiredHeight aren't 0, only that exact resolution is accepted.
static HRESULT SelectLargestResolution(WMD::IMediaDeviceController *controller, WMC::MediaStreamType streamType,
	const char *name, UINT32 aspectWidth, UINT32 aspectHeight, UINT32 *selectedWidth, UINT32 *selectedHeight,
	UINT32 requiredWidth = 0, UINT32 requiredHeight = 0)
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
		if (requiredWidth != 0 && (width != requiredWidth || height != requiredHeight))
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
		if (requiredWidth != 0) printf("No %s resolution %ux%u (wp81camdump -l lists them)\n", name, requiredWidth, requiredHeight);
		else printf("No %s resolution available\n", name);
		return E_FAIL;
	}

	UINT32 width, height;
	GetSize(best.Get(), &width, &height);
	// Verbose only: in raw mode the photo resolution (it only sets the aspect ratio of the preview), so that it can't
	// be taken for the raw resolution, and the preview (it only feeds the 3A, it isn't saved)
	BOOL isPreview = strcmp(name, "Preview") == 0;
	if (verbose || (!rawCaptureMode && !isPreview))
	{
		printf("%s resolution: %ux%u%s\n", name, width, height, isPreview ? " (live stream for exposure/focus, not saved)" : "");
	}
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

static const struct { UINT32 iso; WMD::IsoSpeedPreset preset; } isoPresets[] = {
	{ 50, WMD::IsoSpeedPreset_Iso50 }, { 80, WMD::IsoSpeedPreset_Iso80 }, { 100, WMD::IsoSpeedPreset_Iso100 },
	{ 200, WMD::IsoSpeedPreset_Iso200 }, { 400, WMD::IsoSpeedPreset_Iso400 }, { 800, WMD::IsoSpeedPreset_Iso800 },
	{ 1600, WMD::IsoSpeedPreset_Iso1600 }, { 3200, WMD::IsoSpeedPreset_Iso3200 }, { 6400, WMD::IsoSpeedPreset_Iso6400 },
	{ 12800, WMD::IsoSpeedPreset_Iso12800 }, { 25600, WMD::IsoSpeedPreset_Iso25600 },
};

static UINT32 IsoOfPreset(WMD::IsoSpeedPreset preset)
{
	for (size_t i = 0; i < ARRAYSIZE(isoPresets); i++)
	{
		if (isoPresets[i].preset == preset) return isoPresets[i].iso;
	}
	return 0; // Auto
}

static void PrintIsoPresets(WMD::IIsoSpeedControl *control)
{
	ComPtr<WFC::IVectorView<WMD::IsoSpeedPreset>> presets;
	UINT32 count = 0;
	if (FAILED(control->get_SupportedPresets(&presets)) || !presets || FAILED(presets->get_Size(&count))) return;
	printf("  ISO presets:");
	for (UINT32 i = 0; i < count; i++)
	{
		WMD::IsoSpeedPreset preset;
		if (FAILED(presets->GetAt(i, &preset))) continue;
		UINT32 iso = IsoOfPreset(preset);
		if (iso) printf(" %u", iso);
		else printf(" auto");
	}
	printf("\n");
}

// Sets the exposure time (100 ns units) and the ISO speed. -1 = keep as is, 0 = auto.
// Failures are warnings: the picture is still taken, and the applied values are printed.
static void ApplyManualExposure(WMD::IVideoDeviceController *videoController, int iso, INT64 exposure100ns,
	BOOL restoring = FALSE)
{
	if (iso < 0 && exposure100ns < 0) return;
	BOOL report = verbose || !restoring; // the read-back values

	ComPtr<WMD::IAdvancedVideoCaptureDeviceController2> advanced;
	HRESULT hr = videoController->QueryInterface(__uuidof(WMD::IAdvancedVideoCaptureDeviceController2),
		(void **)advanced.GetAddressOf());
	if (FAILED(hr))
	{
		printf("Warning: no IAdvancedVideoCaptureDeviceController2 (0x%08lX): exposure and ISO stay automatic\n",
			(unsigned long)hr);
		return;
	}

	if (exposure100ns >= 0)
	{
		ComPtr<WMD::IExposureControl> exposure;
		boolean supported = false;
		hr = advanced->get_ExposureControl(&exposure);
		if (SUCCEEDED(hr)) hr = exposure->get_Supported(&supported);
		if (FAILED(hr) || !supported)
		{
			printf("Warning: exposure control not supported (0x%08lX)\n", (unsigned long)hr);
		}
		else
		{
			WF::TimeSpan minimum = {}, maximum = {}, step = {};
			exposure->get_Min(&minimum);
			exposure->get_Max(&maximum);
			exposure->get_Step(&step);
			if (verbose)
			{
				printf("  exposure range: %.3f ms .. %.3f ms, step %.3f ms\n", minimum.Duration / 10000.0,
					maximum.Duration / 10000.0, step.Duration / 10000.0);
			}
			ComPtr<WF::IAsyncAction> action;
			if (exposure100ns == 0)
			{
				hr = exposure->SetAutoAsync(true, &action);
			}
			else
			{
				WF::TimeSpan value;
				value.Duration = exposure100ns;
				if (value.Duration < minimum.Duration) value.Duration = minimum.Duration;
				// MFCaptureEngine rejects a value above the maximum (E_INVALIDARG). The maximum is a default of the
				// driver (1 s), raised by HKLM\Software\OEM\Nokia\Camera\RFC\Caps MaxET (µs): 4 s works on the Lumia 520
				if (maximum.Duration > 0 && value.Duration > maximum.Duration) value.Duration = maximum.Duration;
				if (value.Duration != exposure100ns)
				{
					printf("Warning: exposure %.3f ms out of range, clamped to %.3f ms\n", exposure100ns / 10000.0,
						value.Duration / 10000.0);
					if (value.Duration == maximum.Duration)
					{
						printf("(4 s is the limit of the camera driver)\n");
					}
				}
				hr = exposure->SetValueAsync(value, &action);
				asyncTimeoutMs = 30000 + (DWORD)(value.Duration / 10000) * 4;
			}
			if (SUCCEEDED(hr)) hr = WaitAction(action.Get(), "ExposureControl");
			if (FAILED(hr)) printf("Warning: cannot set the exposure 0x%08lX\n", (unsigned long)hr);
			if (hr == (HRESULT)0xE801000E)
			{
				// NokiaElfsPlugIn::Init (qccamavs8930.sys) caps the exposure to min(sensor maximum, 4000000 µs)
				printf("(the 3A plug-in of the driver refuses exposures above 4 s)\n");
			}

			boolean isAuto = false;
			WF::TimeSpan current = {};
			exposure->get_Auto(&isAuto);
			exposure->get_Value(&current);
			if (!restoring) appliedExposure100ns = isAuto ? 0 : current.Duration;
			if (!report) {}
			else if (isAuto) printf("Exposure: auto\n");
			else if (current.Duration >= 10000000) printf("Exposure: %.3f ms (%.1f s)\n", current.Duration / 10000.0,
				current.Duration / 10000000.0);
			else printf("Exposure: %.3f ms (1/%.0f s)\n", current.Duration / 10000.0,
				current.Duration > 0 ? 10000000.0 / current.Duration : 0.0);
		}
	}

	if (iso >= 0)
	{
		ComPtr<WMD::IIsoSpeedControl> isoControl;
		boolean supported = false;
		hr = advanced->get_IsoSpeedControl(&isoControl);
		if (SUCCEEDED(hr)) hr = isoControl->get_Supported(&supported);
		if (FAILED(hr) || !supported)
		{
			printf("Warning: ISO control not supported (0x%08lX)\n", (unsigned long)hr);
			return;
		}
		if (verbose) PrintIsoPresets(isoControl.Get());

		// IIsoSpeedControl2 (any value in a range) if the driver has it, else the presets
		ComPtr<WMD::IIsoSpeedControl2> isoControl2;
		isoControl.As(&isoControl2);
		ComPtr<WF::IAsyncAction> action;
		if (isoControl2)
		{
			UINT32 minimum = 0, maximum = 0, step = 0;
			isoControl2->get_Min(&minimum);
			isoControl2->get_Max(&maximum);
			isoControl2->get_Step(&step);
			if (verbose) printf("  ISO range: %u .. %u, step %u\n", minimum, maximum, step);
			if (iso == 0)
			{
				hr = isoControl2->SetAutoAsync(&action);
			}
			else
			{
				UINT32 value = (UINT32)iso;
				if (value < minimum) value = minimum;
				if (maximum > 0 && value > maximum) value = maximum;
				if (value != (UINT32)iso) printf("Warning: ISO %d out of range, clamped to %u\n", iso, value);
				hr = isoControl2->SetValueAsync(value, &action);
			}
		}
		else
		{
			WMD::IsoSpeedPreset preset = WMD::IsoSpeedPreset_Auto;
			BOOL found = (iso == 0);
			for (size_t i = 0; i < ARRAYSIZE(isoPresets) && !found; i++)
			{
				if (isoPresets[i].iso == (UINT32)iso)
				{
					preset = isoPresets[i].preset;
					found = TRUE;
				}
			}
			if (!found)
			{
				printf("Warning: no ISO preset %d\n", iso);
				PrintIsoPresets(isoControl.Get());
				return;
			}
			hr = isoControl->SetPresetAsync(preset, &action);
		}
		if (SUCCEEDED(hr)) hr = WaitAction(action.Get(), "IsoSpeedControl");
		if (FAILED(hr))
		{
			printf("Warning: cannot set the ISO 0x%08lX\n", (unsigned long)hr);
			if (!verbose) PrintIsoPresets(isoControl.Get());
		}

		boolean isAuto = false;
		UINT32 current = 0;
		if (isoControl2)
		{
			isoControl2->get_Auto(&isAuto);
			isoControl2->get_Value(&current);
		}
		else
		{
			WMD::IsoSpeedPreset preset = WMD::IsoSpeedPreset_Auto;
			isoControl->get_Preset(&preset);
			current = IsoOfPreset(preset);
			isAuto = (current == 0);
		}
		if (!restoring) appliedIso = isAuto ? 0 : current;
		if (!report) {}
		else if (isAuto) printf("ISO: auto\n");
		else printf("ISO: %u\n", current);
	}
}

// Reads the current exposure time and ISO from the camera (for the EXIF of the JPEG).
// 0 when unknown: in auto mode the Lumia 520 reports the minimum exposure and the last manual ISO, not the ones in use.
static void ReadExposureState(WMC::IMediaCapture *capture, INT64 *exposure100ns, BOOL *exposureAuto, UINT32 *iso,
	BOOL *isoAuto)
{
	*exposure100ns = 0;
	*iso = 0;
	*exposureAuto = TRUE;
	*isoAuto = TRUE;
	ComPtr<WMD::IVideoDeviceController> videoController;
	ComPtr<WMD::IAdvancedVideoCaptureDeviceController2> advanced;
	if (FAILED(capture->get_VideoDeviceController(&videoController)) || FAILED(videoController.As(&advanced))) return;

	ComPtr<WMD::IExposureControl> exposure;
	boolean supported = false;
	if (SUCCEEDED(advanced->get_ExposureControl(&exposure)) && SUCCEEDED(exposure->get_Supported(&supported)) && supported)
	{
		boolean isAuto = true;
		WF::TimeSpan value = {}, minimum = {};
		exposure->get_Auto(&isAuto);
		exposure->get_Value(&value);
		exposure->get_Min(&minimum);
		*exposureAuto = isAuto;
		if (!isAuto) *exposure100ns = value.Duration;
		if (verbose) printf("  exposure read back: %s, %.3f ms\n", isAuto ? "auto" : "manual", value.Duration / 10000.0);
	}

	ComPtr<WMD::IIsoSpeedControl> isoControl;
	supported = false;
	if (SUCCEEDED(advanced->get_IsoSpeedControl(&isoControl)) && SUCCEEDED(isoControl->get_Supported(&supported)) && supported)
	{
		ComPtr<WMD::IIsoSpeedControl2> isoControl2;
		if (SUCCEEDED(isoControl.As(&isoControl2)))
		{
			boolean isAuto = true;
			UINT32 value = 0;
			isoControl2->get_Auto(&isAuto);
			isoControl2->get_Value(&value);
			*isoAuto = isAuto;
			// In auto mode the value is the last manual one (seen on the Lumia 520), not the one in use
			if (!isAuto) *iso = value;
			if (verbose) printf("  ISO read back: %s, %u\n", isAuto ? "auto" : "manual", value);
		}
		else
		{
			WMD::IsoSpeedPreset preset = WMD::IsoSpeedPreset_Auto;
			isoControl->get_Preset(&preset);
			*iso = IsoOfPreset(preset);
			*isoAuto = (*iso == 0);
		}
	}
}

// Minimal EXIF (TIFF little-endian) writer
enum { EXIF_SHORT = 3, EXIF_LONG = 4, EXIF_RATIONAL = 5, EXIF_ASCII = 2, EXIF_UNDEFINED = 7 };

struct ExifEntry
{
	WORD tag;
	WORD type;
	DWORD count;
	BYTE data[64];
	DWORD size;
};

struct ExifIfd
{
	ExifEntry entries[24];
	int count = 0;

	ExifEntry *Add(WORD tag, WORD type, DWORD count, const void *data, DWORD size)
	{
		ExifEntry *e = &entries[this->count++];
		e->tag = tag;
		e->type = type;
		e->count = count;
		e->size = size;
		memset(e->data, 0, sizeof(e->data));
		memcpy(e->data, data, size);
		return e;
	}
	void Short(WORD tag, WORD value) { Add(tag, EXIF_SHORT, 1, &value, 2); }
	void Long(WORD tag, DWORD value) { Add(tag, EXIF_LONG, 1, &value, 4); }
	void Rational(WORD tag, DWORD numerator, DWORD denominator)
	{
		DWORD value[2] = { numerator, denominator };
		Add(tag, EXIF_RATIONAL, 1, value, 8);
	}
	void Ascii(WORD tag, const char *text)
	{
		char buffer[64];
		strncpy_s(buffer, sizeof(buffer), text, _TRUNCATE);
		DWORD length = (DWORD)strlen(buffer) + 1;
		Add(tag, EXIF_ASCII, length, buffer, length);
	}
	void Undefined(WORD tag, const char *bytes4) { Add(tag, EXIF_UNDEFINED, 4, bytes4, 4); }

	// The entries must be sorted by tag
	void Sort()
	{
		for (int i = 1; i < count; i++)
		{
			for (int j = i; j > 0 && entries[j - 1].tag > entries[j].tag; j--)
			{
				ExifEntry t = entries[j];
				entries[j] = entries[j - 1];
				entries[j - 1] = t;
			}
		}
	}
	DWORD Size() const
	{
		DWORD size = 2 + 12 * count + 4;
		for (int i = 0; i < count; i++)
		{
			if (entries[i].size > 4) size += (entries[i].size + 1) & ~1u;
		}
		return size;
	}
	// Writes the IFD at the TIFF offset 'offset' (out points to the start of the TIFF header)
	void Write(BYTE *tiff, DWORD offset) const
	{
		BYTE *p = tiff + offset;
		DWORD dataOffset = offset + 2 + 12 * count + 4;
		*(WORD *)p = (WORD)count;
		p += 2;
		for (int i = 0; i < count; i++)
		{
			const ExifEntry &e = entries[i];
			*(WORD *)p = e.tag;
			*(WORD *)(p + 2) = e.type;
			*(DWORD *)(p + 4) = e.count;
			if (e.size <= 4)
			{
				memset(p + 8, 0, 4);
				memcpy(p + 8, e.data, e.size);
			}
			else
			{
				*(DWORD *)(p + 8) = dataOffset;
				memcpy(tiff + dataOffset, e.data, e.size);
				if (e.size & 1) tiff[dataOffset + e.size] = 0;
				dataOffset += (e.size + 1) & ~1u;
			}
			p += 12;
		}
		*(DWORD *)p = 0; // no next IFD
	}
};

static void ReadRegistryString(const wchar_t *key, const wchar_t *value, char *out, size_t outSize, const char *fallback)
{
	strcpy_s(out, outSize, fallback);
	HKEY hKey = NULL;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) return;
	wchar_t buffer[64] = {};
	DWORD type = 0, size = sizeof(buffer) - sizeof(wchar_t);
	if (RegQueryValueExW(hKey, value, nullptr, &type, (BYTE *)buffer, &size) == ERROR_SUCCESS && type == 1 /* REG_SZ */ &&
		buffer[0])
	{
		WideCharToMultiByte(CP_ACP, 0, buffer, -1, out, (int)outSize, nullptr, nullptr);
	}
	RegCloseKey(hKey);
}

// Builds the APP1 Exif segment (marker included) in 'segment', returns its size
static DWORD BuildExifSegment(BYTE *segment, DWORD capacity, UINT32 width, UINT32 height, INT64 exposure100ns,
	BOOL exposureAuto, UINT32 iso, BOOL isoAuto)
{
	char make[64], model[64], dateTime[20];
	ReadRegistryString(L"System\\Platform\\DeviceTargetingInfo", L"PhoneManufacturer", make, sizeof(make), "Unknown");
	ReadRegistryString(L"System\\Platform\\DeviceTargetingInfo", L"PhoneModelName", model, sizeof(model), "Windows Phone");
	SYSTEMTIME now;
	GetLocalTime(&now);
	sprintf_s(dateTime, sizeof(dateTime), "%04u:%02u:%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour,
		now.wMinute, now.wSecond);

	ExifIfd ifd0, exif;
	ifd0.Ascii(0x010F, make);
	ifd0.Ascii(0x0110, model);
	ifd0.Short(0x0112, 1);       // Orientation: top-left
	ifd0.Rational(0x011A, 72, 1); // XResolution
	ifd0.Rational(0x011B, 72, 1); // YResolution
	ifd0.Short(0x0128, 2);       // ResolutionUnit: inch
	ifd0.Ascii(0x0131, "wp81camdump");
	ifd0.Ascii(0x0132, dateTime);
	ifd0.Short(0x0213, 1);       // YCbCrPositioning: centered
	ExifEntry *exifPointer = ifd0.Add(0x8769, EXIF_LONG, 1, "\0\0\0\0", 4);

	if (exposure100ns > 0)
	{
		// 1/N when it is one (within 1%), else 100 ns units
		double n = 10000000.0 / (double)exposure100ns;
		DWORD rounded = (DWORD)(n + 0.5);
		if (n >= 1.0 && rounded > 0 && fabs(n - rounded) / n < 0.01) exif.Rational(0x829A, 1, rounded);
		else exif.Rational(0x829A, (DWORD)exposure100ns, 10000000);
	}
	BOOL manual = !exposureAuto || !isoAuto;
	exif.Short(0x8822, manual ? 1 : 2); // ExposureProgram: manual / normal program
	if (iso > 0)
	{
		exif.Short(0x8827, (WORD)(iso > 65535 ? 65535 : iso)); // ISOSpeedRatings
		exif.Short(0x8830, 3);                                 // SensitivityType: ISO speed
	}
	exif.Undefined(0x9000, "0230"); // ExifVersion
	exif.Ascii(0x9003, dateTime);   // DateTimeOriginal
	exif.Ascii(0x9004, dateTime);   // DateTimeDigitized
	exif.Undefined(0x9101, "\x01\x02\x03\x00"); // ComponentsConfiguration: YCbCr
	exif.Undefined(0xA000, "0100"); // FlashpixVersion
	exif.Short(0xA001, 1);          // ColorSpace: sRGB
	exif.Long(0xA002, width);       // PixelXDimension
	exif.Long(0xA003, height);      // PixelYDimension
	exif.Short(0xA402, manual ? 1 : 0); // ExposureMode: manual / auto
	ifd0.Sort();
	exif.Sort();

	DWORD ifd0Offset = 8;
	DWORD exifOffset = ifd0Offset + ifd0.Size();
	DWORD tiffSize = exifOffset + exif.Size();
	DWORD segmentSize = 4 + 6 + tiffSize; // FFE1 + length + "Exif\0\0" + TIFF
	if (segmentSize > capacity || segmentSize - 2 > 0xFFFF) return 0;
	for (int i = 0; i < ifd0.count; i++)
	{
		if (ifd0.entries[i].tag == 0x8769) memcpy(ifd0.entries[i].data, &exifOffset, 4);
	}
	(void)exifPointer;

	memset(segment, 0, segmentSize);
	segment[0] = 0xFF;
	segment[1] = 0xE1;
	segment[2] = (BYTE)((segmentSize - 2) >> 8);
	segment[3] = (BYTE)((segmentSize - 2) & 0xFF);
	memcpy(segment + 4, "Exif\0\0", 6);
	BYTE *tiff = segment + 10;
	memcpy(tiff, "II\x2A\x00", 4);
	*(DWORD *)(tiff + 4) = ifd0Offset;
	ifd0.Write(tiff, ifd0Offset);
	exif.Write(tiff, exifOffset);
	return segmentSize;
}

// Inserts an EXIF segment in the JPEG written by the camera (which has only a JFIF header):
// SOI, APP1 Exif, then the original segments without the JFIF APP0. Returns the new file size, 0 on failure.
static UINT64 AddExifToJpeg(const wchar_t *path, UINT64 fileSize, UINT32 width, UINT32 height, INT64 exposure100ns,
	BOOL exposureAuto, UINT32 iso, BOOL isoAuto)
{
	if (fileSize < 4 || fileSize > 64 * 1024 * 1024) return 0;
	BYTE *jpeg = (BYTE *)malloc((size_t)fileSize);
	if (!jpeg) return 0;
	UINT64 result = 0;
	HANDLE hFile = CreateFile2(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, NULL);
	DWORD read = 0;
	BOOL ok = hFile != INVALID_HANDLE_VALUE && ReadFile(hFile, jpeg, (DWORD)fileSize, &read, NULL) && read == fileSize;
	if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
	if (ok && jpeg[0] == 0xFF && jpeg[1] == 0xD8)
	{
		// Skip the JFIF APP0 and an existing Exif APP1 right after SOI
		DWORD rest = 2;
		while (rest + 4 <= fileSize && jpeg[rest] == 0xFF && (jpeg[rest + 1] == 0xE0 || jpeg[rest + 1] == 0xE1))
		{
			rest += 2 + ((DWORD)jpeg[rest + 2] << 8 | jpeg[rest + 3]);
		}
		BYTE segment[2048];
		DWORD segmentSize = BuildExifSegment(segment, sizeof(segment), width, height, exposure100ns, exposureAuto, iso, isoAuto);
		if (segmentSize > 0 && rest < fileSize)
		{
			DWORD newSize = 2 + segmentSize + (DWORD)(fileSize - rest);
			BYTE *out = (BYTE *)malloc(newSize);
			if (out)
			{
				out[0] = 0xFF;
				out[1] = 0xD8;
				memcpy(out + 2, segment, segmentSize);
				memcpy(out + 2 + segmentSize, jpeg + rest, (size_t)(fileSize - rest));
				if (SUCCEEDED(WriteFileData(path, out, newSize))) result = newSize;
				free(out);
			}
		}
	}
	free(jpeg);
	if (result == 0) printf("Warning: cannot add the EXIF information to %ls\n", path);
	return result;
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
	// The resolution of the saved picture: the one of the photo stream
	UINT32 width = 0, height = 0;
	ComPtr<WMD::IVideoDeviceController> videoController;
	ComPtr<WMD::IMediaDeviceController> controller;
	ComPtr<WMP::IMediaEncodingProperties> photoProps;
	if (SUCCEEDED(capture->get_VideoDeviceController(&videoController)) && SUCCEEDED(videoController.As(&controller)) &&
		SUCCEEDED(controller->GetMediaStreamProperties(WMC::MediaStreamType_Photo, &photoProps)))
	{
		GetSize(photoProps.Get(), &width, &height);
	}
	// EXIF: resolution, exposure time, ISO, date, camera
	INT64 exposure100ns = 0;
	UINT32 iso = 0;
	BOOL exposureAuto = TRUE, isoAuto = TRUE;
	ReadExposureState(capture, &exposure100ns, &exposureAuto, &iso, &isoAuto);
	UINT64 newSize = AddExifToJpeg(outputPath, size, width, height, exposure100ns, exposureAuto, iso, isoAuto);
	if (newSize) size = newSize;
	if (width && height) printf("Picture saved: %ls (%ux%u JPEG, %llu bytes)\n", outputPath, width, height, (unsigned long long)size);
	else printf("Picture saved: %ls (%llu bytes)\n", outputPath, (unsigned long long)size);
	return S_OK;
}

// Raw mode of the driver.
// qccamavs8930.sys gives its raw data ranges to the still pin only when ManufacturingOS ManufacturingMode = 1.
// It reads the value each time the camera filter is created, and keeps the last value it could read: when the
// value is missing it stays in raw mode, and then MediaCapture can't initialize (0xC00D5212) until a reboot.
// So -r sets the value, and afterwards writes back the original value (0 if there was none), opens the camera once
// so that the driver reads it, and only then deletes what it created. The original state is saved next to the value
// (wp81camdump.* values): if a run dies before the end, the next run of wp81camdump finishes the restore.
#define MMOS_KEY L"Software\\Microsoft\\ManufacturingOS"
#define MMOS_VALUE L"ManufacturingMode"
#define MARKER_KEY_CREATED L"wp81camdump.KeyCreated"      // 1: the key didn't exist
#define MARKER_ORIGINAL L"wp81camdump.OriginalValue"     // the original ManufacturingMode, absent if there was none
#define MARKER_ACTIVE L"wp81camdump.RawModeActive"       // 1: raw mode set by wp81camdump, not restored yet

static BOOL ReadDword(HKEY key, const wchar_t *name, DWORD *value)
{
	DWORD type = 0, size = sizeof(DWORD);
	return RegQueryValueExW(key, name, nullptr, &type, (BYTE *)value, &size) == ERROR_SUCCESS && type == REG_DWORD;
}

static LONG WriteDword(HKEY key, const wchar_t *name, DWORD value)
{
	return RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
}

// The maximum exposure the driver reports (and MFCaptureEngine accepts) is HKLM\...\RFC\Caps MaxET, in µs, read at each
// camera open, 1 s by default. The real limit is 4 s (NokiaElfsPlugIn::Init caps it): sets MaxET to 4 s, permanently.
#define CAMERA_CAPS_KEY L"Software\\OEM\\Nokia\\Camera\\RFC\\Caps"
#define MAX_EXPOSURE_US 4000000
static void EnsureMaxExposure()
{
	HKEY key = NULL;
	LONG err = RegCreateKeyExW(HKEY_LOCAL_MACHINE, CAMERA_CAPS_KEY, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr,
		&key, nullptr);
	if (err != ERROR_SUCCESS)
	{
		printf("Warning: cannot open HKLM\\%ls (error %ld): the maximum exposure stays at the driver default\n",
			CAMERA_CAPS_KEY, err);
		return;
	}
	DWORD current = 0;
	BOOL present = ReadDword(key, L"MaxET", &current);
	if (!present || current != MAX_EXPOSURE_US)
	{
		err = WriteDword(key, L"MaxET", MAX_EXPOSURE_US);
		if (err == ERROR_SUCCESS)
		{
			printf("Set HKLM\\%ls MaxET = %u (maximum exposure 4 s, was %s)\n", CAMERA_CAPS_KEY, MAX_EXPOSURE_US,
				present ? "another value" : "the 1 s default");
		}
		else
		{
			printf("Warning: cannot set HKLM\\%ls MaxET (error %ld)\n", CAMERA_CAPS_KEY, err);
		}
	}
	else if (verbose)
	{
		printf("HKLM\\%ls MaxET = %lu\n", CAMERA_CAPS_KEY, current);
	}
	RegCloseKey(key);
}

// Opens the camera once (filter created → the driver reads ManufacturingMode) and closes it.
// Returns whether the still pin offers raw types (TRUE = the driver is in raw mode).
static BOOL OpenCameraOnce(BOOL *rawExposed)
{
	*rawExposed = FALSE;
	HookCaptureEngine(); // to look at the native types through the engine
	ComPtr<WMC::IMediaCapture> capture;
	ComPtr<WMC::IMediaCaptureInitializationSettings> settings;
	if (FAILED(ActivateInstance(WINDOWS_MEDIA_DLL, RuntimeClass_Windows_Media_Capture_MediaCapture,
		__uuidof(WMC::IMediaCapture), (void **)capture.GetAddressOf()))) return FALSE;
	if (FAILED(ActivateInstance(WINDOWS_MEDIA_DLL, RuntimeClass_Windows_Media_Capture_MediaCaptureInitializationSettings,
		__uuidof(WMC::IMediaCaptureInitializationSettings), (void **)settings.GetAddressOf()))) return FALSE;
	settings->put_StreamingCaptureMode(WMC::StreamingCaptureMode_Video);
	ComPtr<WF::IAsyncAction> action;
	HRESULT hr = capture->InitializeWithSettingsAsync(settings.Get(), &action);
	if (SUCCEEDED(hr)) hr = WaitAction(action.Get(), "InitializeWithSettingsAsync");
	if (verbose) printf("Camera opened to reset the driver: 0x%08lX\n", (unsigned long)hr);
	if (SUCCEEDED(hr))
	{
		int s, t;
		*rawExposed = FindLargestIdealRawType(&s, &t);
	}
	ComPtr<WF::IClosable> closable;
	if (SUCCEEDED(capture.As(&closable))) closable->Close();
	return SUCCEEDED(hr);
}

// Sets ManufacturingMode = 1 for the raw capture, after saving the original state.
static HRESULT EnterRawMode()
{
	HKEY key = NULL;
	DWORD disposition = 0;
	LONG err = RegCreateKeyExW(HKEY_LOCAL_MACHINE, MMOS_KEY, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr,
		&key, &disposition);
	if (err != ERROR_SUCCESS)
	{
		printf("Cannot open HKLM\\%ls: error %ld\n", MMOS_KEY, err);
		return HRESULT_FROM_WIN32(err);
	}
	DWORD active = 0;
	if (!ReadDword(key, MARKER_ACTIVE, &active) || active != 1)
	{
		// Save the original state (not when a previous run wasn't restored: it's already saved)
		WriteDword(key, MARKER_KEY_CREATED, disposition == REG_CREATED_NEW_KEY ? 1 : 0);
		DWORD original = 0;
		if (ReadDword(key, MMOS_VALUE, &original)) WriteDword(key, MARKER_ORIGINAL, original);
		else RegDeleteValueW(key, MARKER_ORIGINAL);
		WriteDword(key, MARKER_ACTIVE, 1);
	}
	err = WriteDword(key, MMOS_VALUE, 1);
	RegCloseKey(key);
	if (err != ERROR_SUCCESS)
	{
		printf("Cannot set ManufacturingMode: error %ld\n", err);
		return HRESULT_FROM_WIN32(err);
	}
	if (verbose) printf("Raw mode: HKLM\\%ls %ls = 1\n", MMOS_KEY, MMOS_VALUE);
	return S_OK;
}

// Restores the original state saved by EnterRawMode, if raw mode is active. Called after the raw capture,
// and at the start of every run (cleanup after a run that died).
static void LeaveRawMode(BOOL atStart)
{
	HKEY key = NULL;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, MMOS_KEY, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
	DWORD active = 0;
	if (!ReadDword(key, MARKER_ACTIVE, &active) || active != 1)
	{
		RegCloseKey(key);
		return;
	}
	if (atStart) printf("Restoring the camera driver after a raw capture that didn't finish\n");

	DWORD keyCreated = 0, original = 0;
	ReadDword(key, MARKER_KEY_CREATED, &keyCreated);
	BOOL hadValue = ReadDword(key, MARKER_ORIGINAL, &original);

	// The driver must read a value: write the original one, or 0, then open the camera
	WriteDword(key, MMOS_VALUE, hadValue ? original : 0);
	BOOL rawExposed = FALSE;
	BOOL opened = FALSE;
	for (int attempt = 0; attempt < 5 && !opened; attempt++)
	{
		if (attempt > 0) Sleep(1000);
		opened = OpenCameraOnce(&rawExposed);
	}
	BOOL expectRaw = hadValue && original == 1;
	if (!opened || rawExposed != expectRaw)
	{
		// Keep the markers: the next run tries again
		printf("Warning: the camera driver may still be in raw mode (camera opened: %s). Run wp81camdump again, or reboot\n",
			opened ? "yes" : "no");
		RegCloseKey(key);
		return;
	}

	// The driver has read the original value: remove what was created
	RegDeleteValueW(key, MARKER_ACTIVE);
	RegDeleteValueW(key, MARKER_ORIGINAL);
	RegDeleteValueW(key, MARKER_KEY_CREATED);
	if (!hadValue) RegDeleteValueW(key, MMOS_VALUE);
	RegCloseKey(key);
	if (keyCreated) RegDeleteKeyExW(HKEY_LOCAL_MACHINE, MMOS_KEY, 0, 0); // fails if something else was added to it
	if (verbose) printf("Raw mode left, registry restored\n");
}

static HRESULT TakePicture(const wchar_t *outputPath, BOOL usePreview, DWORD previewDelayMs,
	BOOL shutterSound, BOOL listFormats, BOOL raw, int rawCount, BOOL growHeap,
	int iso, INT64 exposure100ns, UINT32 photoWidth, UINT32 photoHeight)
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
		if (raw) hr = ListRawResolutions();
		else PrintPhotoResolutions(capture.Get());
		if (verbose) ListNativeFormats();
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

	// With -r, -s is the raw resolution
	int rawStream = -1, rawType = -1;
	if (raw && photoWidth != 0)
	{
		ComPtr<IMFCaptureSource_> source;
		if (capturedEngine == nullptr || FAILED(capturedEngine->GetSource(&source)) ||
			!FindRawTypeBySize(source.Get(), photoWidth, photoHeight, &rawStream, &rawType))
		{
			printf("No raw resolution %ux%u (wp81camdump -r -l lists them)\n", photoWidth, photoHeight);
			ComPtr<WF::IClosable> closable;
			if (SUCCEEDED(capture.As(&closable))) closable->Close();
			return MF_E_INVALIDMEDIATYPE;
		}
	}

	if (photoWidth != 0 && !raw)
	{
		// Requested with -s: fatal if the camera doesn't have it
		hr = SelectLargestResolution(controller.Get(), WMC::MediaStreamType_Photo, "Photo", 0, 0, nullptr, nullptr,
			photoWidth, photoHeight);
		if (FAILED(hr))
		{
			ComPtr<WF::IClosable> closable;
			if (SUCCEEDED(capture.As(&closable))) closable->Close();
			return hr;
		}
	}
	else
	{
		// Not fatal: the default resolution is used if it fails
		SelectLargestResolution(controller.Get(), WMC::MediaStreamType_Photo, "Photo", 0, 0, nullptr, nullptr);
	}

	BOOL previewStarted = FALSE;
	// With -r, -e/-i go to the sensor itself (ApplyRawExposure), not to the 3A controls
	if (raw)
	{
		iso = -1;
		exposure100ns = -1;
	}
	if (!usePreview)
	{
		ApplyManualExposure(videoController.Get(), iso, exposure100ns);
	}
	else
	{
		hr = StartPreview(capture.Get(), controller.Get());
		if (FAILED(hr)) return hr;
		previewStarted = TRUE;
		// While the camera streams (the controls may be refused otherwise); the rest of 3A settles during the delay
		ApplyManualExposure(videoController.Get(), iso, exposure100ns);
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

	// Back to auto before the camera is released: the driver keeps the manual values in the next sessions
	if (iso > 0 || exposure100ns > 0)
	{
		if (verbose) printf("Restoring auto exposure/ISO\n");
		ApplyManualExposure(videoController.Get(), iso > 0 ? 0 : -1, exposure100ns > 0 ? 0 : -1, TRUE);
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

// -i: "auto" -> 0, or a positive integer
static BOOL ParseIso(const char *text, int *iso)
{
	if (_stricmp(text, "auto") == 0)
	{
		*iso = 0;
		return TRUE;
	}
	char *end = nullptr;
	long value = strtol(text, &end, 10);
	if (end == text || *end != '\0' || value <= 0 || value > 1000000) return FALSE;
	*iso = (int)value;
	return TRUE;
}

// -e: "auto" -> 0, "1/125" or "0.5" seconds -> 100 ns units
static BOOL ParseExposure(const char *text, INT64 *exposure100ns)
{
	if (_stricmp(text, "auto") == 0)
	{
		*exposure100ns = 0;
		return TRUE;
	}
	char *end = nullptr;
	double seconds = strtod(text, &end);
	if (end == text) return FALSE;
	if (*end == '/')
	{
		const char *denominatorText = end + 1;
		double denominator = strtod(denominatorText, &end);
		if (end == denominatorText || denominator <= 0) return FALSE;
		seconds /= denominator;
	}
	if (*end != '\0' || seconds <= 0 || seconds > 3600) return FALSE;
	*exposure100ns = (INT64)(seconds * 10000000.0 + 0.5);
	if (*exposure100ns < 1) *exposure100ns = 1;
	return TRUE;
}

int main(int argc, char *argv[])
{
	AddVectoredExceptionHandler(1, CrashReporter);

	// Unbuffered: nothing lost when the output is a pipe (ssh) and the process dies
	setvbuf(stdout, NULL, _IONBF, 0);

	const char *output = nullptr;
	BOOL usePreview = TRUE;
	DWORD previewDelayMs = 2000;
	BOOL shutterSound = FALSE;
	BOOL listFormats = FALSE;
	BOOL raw = FALSE;
	int rawCount = 1;
	BOOL growHeap = FALSE;
	int iso = -1;              // -1: unchanged, 0: auto
	INT64 exposure100ns = -1;  // -1: unchanged, 0: auto
	UINT32 photoWidth = 0, photoHeight = 0; // 0: the largest

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
			rawCaptureMode = TRUE;
		}
		else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
		{
			if (sscanf_s(argv[++i], "%ux%u", &photoWidth, &photoHeight) != 2 || photoWidth == 0 || photoHeight == 0)
			{
				printf("Invalid -s %s (expected <width>x<height>, e.g. 2048x1536)\n", argv[i]);
				return EXIT_FAILURE;
			}
		}
		else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
		{
			if (!ParseIso(argv[++i], &iso))
			{
				printf("Invalid -i %s (expected a number like 100, or auto)\n", argv[i]);
				return EXIT_FAILURE;
			}
		}
		else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc)
		{
			if (!ParseExposure(argv[++i], &exposure100ns))
			{
				printf("Invalid -e %s (expected seconds like 1/125 or 0.5, or auto)\n", argv[i]);
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

	// A previous raw capture that died left the driver in raw mode: restore it first
	LeaveRawMode(TRUE);

	// Manual exposure: before the camera opens (the driver reads MaxET then)
	if (exposure100ns > 0 && !raw) EnsureMaxExposure();
	if (raw)
	{
		if (exposure100ns > 0) rawExposureUs = (DWORD)((exposure100ns + 5) / 10);
		if (iso > 0) rawGainQ8 = (DWORD)(((UINT64)iso * 256 + 50) / 100);
	}

	if (raw)
	{
		hr = EnterRawMode();
		if (FAILED(hr))
		{
			RoUninitialize();
			return EXIT_FAILURE;
		}
	}
	hr = TakePicture(outputPath, usePreview, previewDelayMs, shutterSound, listFormats,
		raw, rawCount, growHeap, iso, exposure100ns, photoWidth, photoHeight);
	pendingRawType.Reset();
	if (raw) LeaveRawMode(FALSE);

	RoUninitialize();
	return SUCCEEDED(hr) ? EXIT_SUCCESS : EXIT_FAILURE;
}
