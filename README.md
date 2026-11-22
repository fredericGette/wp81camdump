# wp81camdump

Takes a picture with the back camera of a Lumia 520 (Windows Phone 8.1) from the command line, and saves it as a JPEG file.

## Usage

```
wp81camdump [options] [output.jpg]
  -h         Show this help
  -v         Verbose: print each step and the available photo resolutions
  -n         No preview: take the picture directly (black picture on the Lumia 520)
             By default a preview runs (to Windows.Phone.Media.Capture.MediaCapturePreviewSink)
             before the picture, so the auto exposure/focus can settle
  -d <ms>    Delay between the start of the preview and the picture (default 2000)
  -s         Keep the default photo resolution (default: the largest one)
  -a         Audible: keep the shutter sound (default: no sound)
  -l         List the native media types of every camera stream, and exit
  -r         Raw: save the still in a raw Bayer format of the sensor, as it is (+ a .txt with the format)
             Needs the PT formats of the driver (ManufacturingOS ManufacturingMode = 1)
  -c <n>     With -r: take <n> raw stills in the same session (<name>_1.raw, ...)
  -g         With -r: first a throwaway still in the largest raw type, to make the driver reallocate
             its raw buffer (only the first camera session after boot gets raw data otherwise)
  -t <s>:<t> With -r: use the media type <t> of the stream <s> (indexes printed by -l)
```

Default output: `C:\Data\Users\Public\Documents\camdump.jpg`, or `camdump.raw` with `-r` (the shared folder, visible from the PC over USB).

## How it works

The application uses the WinRT `Windows.Media.Capture.MediaCapture` API without app identity. The activation factories are obtained directly from the DLLs implementing the runtime classes (`DllGetActivationFactory` of `Windows.Media.dll` and `VideoSink_RT.dll`) instead of the activation registry.

1. `MediaCapture` is initialized in video-only mode (no microphone). During the initialization, `Windows.Media.dll` acquires the camera resource through `mfpalwp.dll` (`MFMCPalAcquireResources`).
2. The largest photo resolution is selected (`VideoDeviceController.GetAvailableMediaStreamProperties(Photo)`).
3. A preview is started to the preview sink of [VideoSink_RT.dll](https://github.com/fredericGette/Lumia520/blob/main/content/libraries/videosink_rt.md), so that the camera streams and the 3A algorithms converge without any UI. Without it (`-n`) the picture is black. The preview uses the largest resolution with the aspect ratio of the photo (1024x768 for 2592x1936): the sensor keeps the field of view of the preview, so a 16:9 preview gives a 16:9 picture letterboxed in the 4:3 photo.
4. `CapturePhotoToStreamAsync` writes the JPEG into a stream opened on the output file (`SHCORE!CreateRandomAccessStreamOnFile`).

The shutter sound is disabled unless `-a` is given. `MediaCaptureServer::_Initialize` (`WINDOWS.MEDIA.DLL`) creates the `IMFCaptureEngine` and initializes it with an attribute store. For a camera device it only adds `MF_CAPTURE_ENGINE_CAMERA_REGIONAL_SOUND_LEVEL` (from `HKLM\Software\Microsoft\Photos\OEM\RegionalSettings\Camera\CameraSoundLevel`), and the engine plays its shutter sound. With a custom video source it sets `MF_CAPTURE_ENGINE_CUSTOM_CAMERA_SOUND` = 1 instead, meaning the application plays its own sound and the engine plays none. Before initializing `MediaCapture`, the application creates a capture engine itself. It gets the class factory from `DllGetClassObject` of `MFCaptureEngine.dll`, because `CoCreateInstance` fails with `REGDB_E_CLASSNOTREG` when there is no app identity. It then patches the `IMFCaptureEngine::Initialize` entry of the vtable, which all engine instances share, so that this attribute is added.

Static analysis of `WINDOWS.MEDIA.DLL`: the capture path checks no package identity or capability, and it calls `IMFCaptureEngine::TakePhoto` without first checking that a preview is running.

## Raw Bayer frames (`-r`)

> [!NOTE]
> On one boot, after the first camera session, every raw capture came back as a buffer of the right size filled with zeros, even though the driver trace showed the VFE writing the frame each time (same buffer, device address `0xf07000`). This happened after development runs that had crashed. On the 4 clean boots that followed, it never happened again, not even when that boot's exact sequence was replayed (raw → reset → JPEG → raw). The driver keeps its ideal-raw heap (`MultiPass::CreateIdealRawHeap`) from one session to the next: it's freed only when the cached size `+0x118` is 0. If the zeros come back, `-g` takes a throwaway still in the largest ideal-raw type (5376x3744) first. That makes the driver reallocate the heap, and it fixed the problem that time. Within one session, several stills (`-c`) all work.

The frame is the VFE **ideal raw** output: the sensor's Bayer mosaic after the VFE's first corrections (black level, lens shading, bad pixels), before demosaic, white balance, colour matrix, noise reduction and sharpening. It's not the untouched sensor data (that would be the RDI path). Format, from the driver trace (`rawOutputFormat` 21, `rowIncrement` 432, `linesPerImage` 1947) and checked on a real frame:
- Qualcomm packed 10-bit (`QCOM`): 6 pixels in a little-endian 64-bit word;
- rows of 3456 bytes for 2592 pixels;
- 1948 rows of image (the media type says 1944), followed by other data up to the 6,928,416 bytes of the buffer;
- black level 64;
- Bayer order **GBRG**: the sensor reports SMIA order GRBG, and the driver enables flip + mirror. Checked against a JPEG of the same scene;
- the picture is rotated 90°, as the sensor is mounted.

`-r` writes these values in the `.txt`, and `raw2png.py` reads them. Without `-t`, `-r` takes the raw type closest to the largest NV12 size (2592x1944).

The driver trace that showed this: set `HKLM\System\CurrentControlSet\Services\qccamavs\Tracing` `TraceEnable` = 1 and `CoreDbgEnable` = 1, reboot, then record provider `{62774a98-5c63-43e1-8b13-3c5e8958e414}` with `tracelog -start … -f x.etl` and `tracelog -enableex … -level 5 -matchanykw 0xFFFFFFFFFFFFFFFF`. The events are ASCII strings (`tracerpt` shows them as `BinaryEventData`). Only the VFE core and the sensor log there, not the AVStream layer.

What it took to get that far in manufacturing mode:
- **Source reader.** In PT mode the still pin (stream 3) only offers raw types, with the vendor subtype `{009D4DF4-0A60-4020-A779-A67611E7684A}` (plus 2 `MIPI` types at 41 MP). `CCaptureSource::ConfigureCurrentVideoMediaType` asks the source reader for a YUY2/NV12/420O output, which fails with `MF_E_TOPO_CODEC_NOT_FOUND`, and then `MediaCapture` doesn't initialize. The app hooks `MFCreateSourceReaderFromMediaSource`, a delay-load import of `MFCaptureEngine.dll`, then `IMFSourceReader::SetCurrentMediaType`. When a conversion fails on a raw stream, the hook sets the native raw type instead.
- **Engine events.** `MF_CAPTURE_SOURCE_CURRENT_DEVICE_MEDIA_TYPE_SET` from the app's own `SetCurrentDeviceMediaType` goes to `SetStreamSettingsOperationServer::_HandleAsyncCompletion` in `Windows.Media.dll`, which has no pending operation and crashes (NULL read in `RTWorkQ.dll`). The event callback given to `IMFCaptureEngine::Initialize` is wrapped, so these events are kept from `MediaCapture` during the raw capture.
- **Driver cache.** `qccamavs8930.sys` keeps the last `ManufacturingMode` value it read. Deleting the key doesn't reset it: until a reboot, even the normal JPEG capture fails with `0xC00D5212`. Set the value to 0 and open the camera once (e.g. `wp81camdump.exe -l`) before deleting the key.

The JPEG has been through the whole ISP. `-r` saves the sensor's own pixels (10-bit Bayer) instead.

The hook on `IMFCaptureEngine::Initialize` also keeps the engine that `MediaCapture` creates. `-l` lists the native media types of each stream through its `IMFCaptureSource`. `-r` selects the largest Bayer type, adds it to the engine's photo sink as both input and output (so no converter or encoder sits between the camera and the app), takes the photo with `IMFCaptureEngine::TakePhoto`, and writes the buffer unchanged. A `<file>.txt` next to it gives the format, width, height and stride. The vtables of `IMFCaptureSource` and `IMFCapturePhotoSink` were checked in the phone's `MFCaptureEngine.dll`: they are the same as on Windows 8.1, and `IMFCaptureEngine` adds `StartPhotoSequence`/`StopPhotoSequence` at the end.

By default the camera exposes only NV12 (still pin: 2592x1936, 2048x1536, 640x480, 2592x1456, 1280x720). `qccamavs8930.sys` does contain raw Bayer data ranges (FourCCs `MIPI`, `QCOM`, `PL16`, `BA81`, `DPCM`), but `CaptureFilter13MP::CreateImagePinFactory` only gives them to the still pin in "PT" (production test) mode. It picks its 10 PT ranges when `HKLM\Software\Microsoft\ManufacturingOS` `ManufacturingMode` = 1 (REG_DWORD), and a single NV12 range otherwise. The value is read each time the camera filter is created. Other drivers read it too, but only when they start: audio default device, ALS/proximity thresholds, Assertive Display, IP over USB policy, and vibrator voltage (that one checks only that the key exists). So set it just for the capture, and delete the key right after:

```
reg add HKLM\Software\Microsoft\ManufacturingOS /v ManufacturingMode /t REG_DWORD /d 1 /f
wp81camdump.exe -v -l
wp81camdump.exe -v -r
reg add HKLM\Software\Microsoft\ManufacturingOS /v ManufacturingMode /t REG_DWORD /d 0 /f
wp81camdump.exe -l
reg delete HKLM\Software\Microsoft\ManufacturingOS /f
```

On the PC, `tools/raw2png.py` (Python 3 + numpy, Pillow optional) converts the file into a PNG: it unpacks the data, does a half-resolution demosaic, subtracts the black level, and applies gray-world white balance and gamma. The format and Bayer order come from the `.txt`. `--gray` shows the data without demosaic.

```
python tools/raw2png.py camdump.raw
```

## Deployment

- [Install a telnet server on the phone](https://github.com/fredericGette/wp81documentation/tree/main/telnetOverUsb#readme), in order to run the application.
- Manually copy the executable from the root of this repository to the shared folder of the phone.
> [!NOTE]
> When you connect your phone with a USB cable, this folder is visible in the Explorer of your computer. And in the phone, this folder is mounted in `C:\Data\USERS\Public\Documents`

## Building

The tool is cross-compiled for ARM32 with clang-cl against the Windows Phone 8.1 SDK. It links only against `mincore.lib`. See [How to build with VSCode, LLVM, CMake and Ninja](https://github.com/fredericGette/wp81documentation/tree/main/ConsoleApplicationBuilding/vscode#readme).

```
cd wp81camdump
cmake --preset arm32-windows
cmake --build build
```
