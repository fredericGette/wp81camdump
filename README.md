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
  -e <time>  Exposure time in seconds: 1/125, 0.5, ... or auto (default: auto)
             Up to 4 s (4000 ms), or up to 6.428 s (6428 ms) with -r
             Sets HKLM\Software\OEM\Nokia\Camera\RFC\Caps MaxET = 4000000 (the 1 s default limit -> 4 s)
  -i <iso>   ISO speed: 100, 800, ... or auto (default: auto)
             With -r, -e and -i are written to the sensor itself: exposure, and analog gain = ISO / 100
             (the .txt gives the values read back from the sensor)
```

Default output: `C:\Data\Users\Public\Documents\camdump.jpg`, or `camdump.raw` with `-r` (the shared folder, visible from the PC over USB).

The application uses the WinRT `MediaCapture` API without app identity. A preview runs before the picture so that the auto exposure and focus can settle. The shutter sound is muted, and an EXIF header is added to the JPEG. The details are in the comments of `wp81camdump/src/wp81camdump.cpp`.

## Raw Bayer frames (`-r`)

`-r` saves the sensor's 10-bit Bayer data instead of the JPEG. The camera driver offers raw formats only in manufacturing mode, so `-r` sets `HKLM\Software\Microsoft\ManufacturingOS` `ManufacturingMode` = 1 for the capture and restores it afterwards. If a run dies before the restore, the next run of `wp81camdump` finishes it.

The frame is the VFE "ideal raw" output: after black level, lens shading and bad-pixel corrections, before demosaic and the rest of the ISP. On the Lumia 520 it is Qualcomm packed 10-bit (`QCOM`), 2592x1944, rows of 3456 bytes, black level 64, Bayer order GBRG, rotated 90°. A `.txt` file next to the `.raw` gives these values.

With `-r`, `-e` and `-i` are written to the sensor directly. The exposure can then go up to 6428 ms (the sensor's 16-bit line counter), instead of the 4000 ms limit of the JPEG path.

If raw stills come back filled with zeros, add `-g`.

```
wp81camdump.exe -r
wp81camdump.exe -r -c 5 -e 2 -i 400
```

On the PC, `tools/raw2png.py` (Python 3 + numpy, Pillow optional) converts the file into a PNG. It reads the format from the `.txt`. `--gray` shows the data without demosaic. `--planes` writes one full-size PNG per channel (`_R`, `_G`, `_B`), colored with the turbo colormap.

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
