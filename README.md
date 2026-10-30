# tga-viewer

Windows Explorer thumbnails for TGA (Truevision TARGA) images.

Windows has no TGA decoder. tga-viewer adds a WIC codec with a complete, dependency-free TGA decoder:

- Thumbnails, preview pane and photo viewers.
- True-color (15/16/24/32-bit), grayscale and color-mapped images, raw or RLE, any origin.
- Alpha like image editors: unused all-zero alpha is shown opaque, TGA 2.0 alpha attributes are honored.

## Install

1. Download a release (or build it) and extract it.
2. Run `install.bat` as administrator.
3. If old icons remain, run `clear-thumbnails.bat`.

Uninstall with `uninstall.bat`. If Microsoft Defender ASR rules block the DLL, allow `C:\Program Files\TgaViewer\TgaWic.dll` with `Add-MpPreference -AttackSurfaceReductionOnlyExclusions`.

## Build

Needs CMake 3.20+ and Visual Studio 2022 (C++). Run `build.bat`: the package is created in `dist\`.

Tests: `python tools\make_testdata.py testdata` writes 32 TGA files with their expected images; decode them with `tga2png`, then check with `--check <folder>`.

## License

MIT.
