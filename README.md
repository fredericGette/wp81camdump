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
  -s <w>x<h> Picture resolution, e.g. 2048x1536 (default: the largest one)
             With -r: the raw resolution, e.g. 2600x1948 (default: the one of the sensor, 2592x1944)
  -a         Audible: keep the shutter sound (default: no sound)
  -l         List the picture resolutions (for -s), and exit. With -r: the raw resolutions
             With -v: also the media types of every stream of the driver (debugging)
  -r         Raw: save the still in a raw Bayer format of the sensor, as it is (+ a .txt with the format)
             Sets HKLM\Software\Microsoft\ManufacturingOS ManufacturingMode = 1 for the capture, then restores it
  -c <n>     With -r: take <n> raw stills in the same session (<name>_1.raw, ...)
  -g         With -r: first a throwaway still in the largest raw type, to make the driver reallocate
             its raw buffer (only the first camera session after boot gets raw data otherwise)
  -e <time>  Exposure time in seconds: 1/125, 0.5, ... up to 4, or auto (default: auto)
             Sets HKLM\Software\OEM\Nokia\Camera\RFC\Caps MaxET = 4000000 (the 1 s default limit -> 4 s)
  -i <iso>   ISO speed: 100, 800, ... or auto (default: auto)
             With -r, -e and -i are written to the sensor itself: exposure, and analog gain = ISO / 100
             (the .txt gives the values read back from the sensor)
```

Default output: `C:\Data\Users\Public\Documents\camdump.jpg`, or `camdump.raw` with `-r` (the shared folder, visible from the PC over USB).

## How it works

The application uses the WinRT `Windows.Media.Capture.MediaCapture` API without app identity. The activation factories are obtained directly from the DLLs implementing the runtime classes (`DllGetActivationFactory` of `Windows.Media.dll` and `VideoSink_RT.dll`) instead of the activation registry.

1. `MediaCapture` is initialized in video-only mode (no microphone). During the initialization, `Windows.Media.dll` acquires the camera resource through `mfpalwp.dll` (`MFMCPalAcquireResources`).
2. The largest photo resolution, or the one given with `-s`, is selected (`VideoDeviceController.GetAvailableMediaStreamProperties(Photo)`). On the Lumia 520: 2592x1936, 2048x1536, 640x480 (4:3), 2592x1456, 1280x720 (16:9).
3. A preview is started to the preview sink of [VideoSink_RT.dll](https://github.com/fredericGette/Lumia520/blob/main/content/libraries/videosink_rt.md), so that the camera streams and the 3A algorithms converge without any UI. Without it (`-n`) the picture is black. The preview uses the largest resolution with the aspect ratio of the photo (1024x768 for 2592x1936): the sensor keeps the field of view of the preview, so a 16:9 preview gives a 16:9 picture letterboxed in the 4:3 photo.
4. With `-e`/`-i`, the exposure time and the ISO speed are set through `IAdvancedVideoCaptureDeviceController2` (`ExposureControl`, `IsoSpeedControl`, with `IIsoSpeedControl2` for any value in the range when the driver has it, otherwise the ISO presets). It happens once the preview runs, so focus and white balance still settle during the delay. Values out of range are clamped, the applied values are read back and printed, and for `-r` they are also written to the `.txt` (`exposure_us`, `iso`; 0 = auto). After the capture both are set back to auto before the camera is released: otherwise the driver of the Lumia 520 delivers empty frames (green JPEGs) in every following camera session until a reboot. Ranges on the Lumia 520: 0.06 ms to 1 s, ISO 100 to 1600. These are defaults of `qccamavs8930.sys` (`CaptureFilter13MP`), which reads `MinET`/`MaxET` (µs) and `MinIso`/`MaxIso` from `HKLM\Software\OEM\Nokia\Camera\RFC\Caps` at each camera open. `MFCaptureEngine.dll` rejects a value outside the reported range (`E_INVALIDARG`), so a longer exposure needs a larger `MaxET`: with `MaxET` = 4000000, `-e 4` gives a real 4 s exposure. 4 s is the real limit: `NokiaElfsPlugIn::Init` in the driver caps the exposure to min(sensor maximum, 4000000 µs), and refuses anything longer with error `0xE801000E`, even when `MaxET` is larger. So with `-e`, the application sets `MaxET` = 4000000 before opening the camera (if it isn't already), and leaves it there.
5. `CapturePhotoToStreamAsync` writes the JPEG into a stream opened on the output file (`SHCORE!CreateRandomAccessStreamOnFile`).
6. The camera's JPEG has only a JFIF header, so the application replaces it with an EXIF segment: Make/Model (from `HKLM\System\Platform\DeviceTargetingInfo`), Software, date and time, 72 dpi, pixel dimensions, exposure program/mode (manual or auto), and, when set with `-e`/`-i`, ExposureTime and ISOSpeedRatings. In auto mode the driver doesn't report the exposure time and ISO actually used (it returns the minimum exposure and the last manual ISO), so those two tags are left out.

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

`-r` writes these values in the `.txt`, and `raw2png.py` reads them. Without `-s`, `-r` takes the raw type closest to the largest NV12 size (2592x1944); `-r -s <w>x<h>` takes another one from the list of `-r -l`. The raw types are a fixed table of the driver, shared with other Nokia sensors (up to 41 MP, 7712x5360). `-r -l` marks the ones larger than the Lumia 520's sensor as `larger than the sensor`. The driver accepts them, but the 5 MP sensor still sends only its 2592x1944 frame.

Exposure and gain of the raw still: the `ExposureControl`/`IsoSpeedControl` values (`-e`/`-i` of the JPEG path) have no effect on it, because the raw still keeps the sensor settings it had. In manufacturing mode the driver also has a diagnostic property set, `{2ED26BD2-811F-42E8-A81A-9199ACE19D36}` (`CameraDiagnostics::ControlCamera`). Its property 7, `GAIN_AND_EXPOSURE`, holds a ULONG exposure in µs and a USHORT analog gain in Q8 (256 = 1x). Setting it writes the sensor directly, without the 3A. With `-r`, `-e`/`-i` are sent there through the `IKsControl` of the camera's media source, just before `TakePhoto`, with gain = ISO / 100. The values read back go in the `.txt` (`exposure_us`, `iso`, `gain_q8`). Without `-e`/`-i`, the still is taken with whatever the 3A last set (66 ms, 8x in a dark room). Measured above black level: 0.1 s → 2.0, 1 s → 21.7, 4 s → 88.8 (linear). Going from 1x to 8x to 16x gain gives ×8 and ×2. The sensor accepted gains up to 16x. The 4 s cap of the 3A doesn't apply on this path. The limit is the sensor's: `qccamsensor8930.sys` converts the exposure into sensor lines and caps it to 0xFFFF (the coarse integration time register is 16-bit). That's 65535 × 98.09 µs = 6.43 s, so `-r -e 10` gives 6428 ms.

The driver trace that showed this: set `HKLM\System\CurrentControlSet\Services\qccamavs\Tracing` `TraceEnable` = 1 and `CoreDbgEnable` = 1, reboot, then record provider `{62774a98-5c63-43e1-8b13-3c5e8958e414}` with `tracelog -start … -f x.etl` and `tracelog -enableex … -level 5 -matchanykw 0xFFFFFFFFFFFFFFFF`. The events are ASCII strings (`tracerpt` shows them as `BinaryEventData`). Only the VFE core and the sensor log there, not the AVStream layer.

What it took to get that far in manufacturing mode:
- **Source reader.** In PT mode the still pin (stream 3) only offers raw types, with the vendor subtype `{009D4DF4-0A60-4020-A779-A67611E7684A}` (plus 2 `MIPI` types at 41 MP). `CCaptureSource::ConfigureCurrentVideoMediaType` asks the source reader for a YUY2/NV12/420O output, which fails with `MF_E_TOPO_CODEC_NOT_FOUND`, and then `MediaCapture` doesn't initialize. The app hooks `MFCreateSourceReaderFromMediaSource`, a delay-load import of `MFCaptureEngine.dll`, then `IMFSourceReader::SetCurrentMediaType`. When a conversion fails on a raw stream, the hook sets the native raw type instead.
- **Engine events.** `MF_CAPTURE_SOURCE_CURRENT_DEVICE_MEDIA_TYPE_SET` from the app's own `SetCurrentDeviceMediaType` goes to `SetStreamSettingsOperationServer::_HandleAsyncCompletion` in `Windows.Media.dll`, which has no pending operation and crashes (NULL read in `RTWorkQ.dll`). The event callback given to `IMFCaptureEngine::Initialize` is wrapped, so these events are kept from `MediaCapture` during the raw capture.
- **Driver cache.** `qccamavs8930.sys` keeps the last `ManufacturingMode` value it read. Deleting the key doesn't reset it: until a reboot, even the normal JPEG capture fails with `0xC00D5212`. So after the capture, `-r` writes 0, opens the camera once, and only then deletes the key (see below).

The JPEG has been through the whole ISP. `-r` saves the sensor's own pixels (10-bit Bayer) instead.

The hook on `IMFCaptureEngine::Initialize` also keeps the engine that `MediaCapture` creates. `-r -l` lists the raw types of its `IMFCaptureSource` (`-v -l` dumps the media types of every stream). `-r` selects the Bayer type closest to the sensor size (or the one given with `-s`), adds it to the engine's photo sink as both input and output (so no converter or encoder sits between the camera and the app), takes the photo with `IMFCaptureEngine::TakePhoto`, and writes the buffer unchanged. A `<file>.txt` next to it gives the format, width, height and stride. The vtables of `IMFCaptureSource` and `IMFCapturePhotoSink` were checked in the phone's `MFCaptureEngine.dll`: they are the same as on Windows 8.1, and `IMFCaptureEngine` adds `StartPhotoSequence`/`StopPhotoSequence` at the end.

By default the camera exposes only NV12 (still pin: 2592x1936, 2048x1536, 640x480, 2592x1456, 1280x720). `qccamavs8930.sys` does contain raw Bayer data ranges (FourCCs `MIPI`, `QCOM`, `PL16`, `BA81`, `DPCM`), but `CaptureFilter13MP::CreateImagePinFactory` only gives them to the still pin in "PT" (production test) mode. It picks its 10 PT ranges when `HKLM\Software\Microsoft\ManufacturingOS` `ManufacturingMode` = 1 (REG_DWORD), and a single NV12 range otherwise. The value is read each time the camera filter is created. Other drivers read it too, but only when they start: audio default device, ALS/proximity thresholds, Assertive Display, IP over USB policy, and vibrator voltage (that one checks only that the key exists). `-r` sets it only for the duration of the capture:

1. It saves the original state next to the value (`wp81camdump.KeyCreated`, `wp81camdump.OriginalValue`, `wp81camdump.RawModeActive`), then sets `ManufacturingMode` = 1.
2. After the capture, even a failed one, it writes back the original value, or 0 if there was none.
3. It opens the camera once, so that the driver reads that value. The driver keeps the last value it could read, so deleting the key alone would leave it in raw mode, and every capture (JPEG included) would fail with `0xC00D5212` until a reboot.
4. It checks that the still pin is back in its normal mode, then deletes the markers, and the value or key if it created them.

If a raw capture dies before the end, the next run of `wp81camdump` (any option) sees `wp81camdump.RawModeActive` and finishes the restore first ("Restoring the camera driver after a raw capture that didn't finish").

```
wp81camdump.exe -r
wp81camdump.exe -r -c 5
```

On the PC, `tools/raw2png.py` (Python 3 + numpy, Pillow optional) converts the file into a PNG: it unpacks the data, does a half-resolution demosaic, subtracts the black level, and applies gray-world white balance and gamma. The format and Bayer order come from the `.txt`. `--gray` shows the data without demosaic. `--planes` writes three PNGs, `<out>_R.png`, `<out>_G.png` and `<out>_B.png`, at the size of the raw frame: each PNG pixel is the raw pixel at the same position. Pixels of that channel are colored with the turbo colormap, and the other pixels are black. All three planes use the same scale: by default the 0.1 to 99.9 percentiles of the raw values, or `--range LO HI` in 10-bit units.

```
python tools/raw2png.py camdump.raw
python tools/raw2png.py camdump.raw --planes --range 64 400
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
