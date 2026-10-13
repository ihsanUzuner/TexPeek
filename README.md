# TexPeek

[English](README.md) | [Türkçe](README-TR.md)

A small, fast texture viewer for game developers on Windows. Opens DDS first and foremost, and shows what is really inside the file: channels, mips, alpha, normals, HDR values.

**Supported formats:** DDS · TGA · HDR · EXR · PNG · JPG · BMP · TIF

## Features
- DDS: every DXGI format including BC1–BC7 and BC6H, with both legacy (DX9) and DX10 headers.
- Step through mip levels, cubemap faces, texture array layers and 3D texture slices.
- Channel view (RGBA / RGB / R / G / B / A) and the raw value of the pixel under the cursor.
- Exposure and ACES tone mapping for HDR images.
- Alpha threshold (cutout) preview, with the percentage that stays visible at each mip.
- Normal maps: reconstruct Z for two-channel data (BC5, DXT5nm), flip the green channel.
- Compare two images: split line, side by side, blend and difference modes.
- 360° view for panoramic (equirectangular) HDRIs, to see how they look as a skybox.
- 3x3 tiling preview to check the seams of tileable textures.
- Images larger than 16384 pixels (for example 24k HDRIs); big files load in the background.
- Export the displayed mip / layer as PNG, TGA or HDR.
- Optional Windows integration: open DDS files with a double-click and see their thumbnails in Explorer, including BC4 / BC5 / BC6H / BC7.
- Interface in English and Turkish.

## Install
Download from the **Releases** page:
- **`TexPeek-Setup-x.y.z.exe`:** Installer. Needs no admin rights and sets up the DDS double-click and Explorer thumbnails.
- **`TexPeek-x.y.z-portable.zip`:** Portable version. Unzip it anywhere and run `texpeek.exe`. If you want the double-click and thumbnails, use the **Windows > Install** button in the left panel.

**Requirements:** Windows 10/11 (64-bit). Nothing else to install.

> The installer is not digitally signed, so Windows SmartScreen may show a warning. Click "More info" > "Run anyway" to continue.

## Usage
- Double-click a DDS file, drag and drop a file onto the window, or use the **Open** button.
- The wheel zooms, dragging pans. Double-click fits the image to the window.
- To compare, drop two files together, or drop the second one while holding `Shift`.
- The language switch (`TR | EN`) is at the right end of the toolbar.

| Shortcut | Action |
|---|---|
| `R` `G` `B` `A` | Show a single channel (press again to return to RGBA) |
| `F` or double-click | Fit to window |
| `1` | Actual size (1:1) |
| `Space` | Jump between A and B in blend mode |
| `Ctrl + O` | Open a file |
| `Ctrl + E` | Export |

## Uninstall
Uninstall from Windows Settings > Apps. Everything is removed, settings included.
For the portable version, use **Windows > Remove** in the left panel and delete the folder.

Thumbnails that Explorer has already generated may stay visible for a while: Windows keeps them in its own thumbnail cache. Disk Cleanup > "Thumbnails" clears it.

## Build from source
You need [Build Tools for Visual Studio](https://visualstudio.microsoft.com/downloads/) (the "Desktop development with C++" workload, which includes CMake), Git, and Inno Setup for the installer:
```
winget install JRSoftware.InnoSetup
build-release.bat
```
The output goes to the `release\` folder. The libraries are downloaded automatically on the first build. The version number is kept in the `VERSION` file.

To build only the program:
```
cmake -B build
cmake --build build --config Release
```

## License
MIT, see [LICENSE](LICENSE). Third-party components: [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).
