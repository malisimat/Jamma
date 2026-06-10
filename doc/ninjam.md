# Ninjam Integration Guide

Jamma uses vendored Ninjam client files for networked collaborative jamming.

## Client Files Structure

Files are located under the `lib/` directory:

- Header include root: `lib\njclient\njclient.h`
- x64 Debug lib: `lib\njclient\x64\Debug\MD\njclient.lib`
- x64 Release lib: `lib\njclient\x64\Release\MD\njclient.lib`

Note: A local `Directory.Build.local.props` is no longer needed for Ninjam paths.

## Refreshing Vendored Ninjam Files (Only When Updating Them)

You only need this step when you intentionally update the vendored Ninjam artifacts.

1. Build Ninjam in the other repo for `x64` `Debug` and `Release` (`MD` runtime).
2. Copy updated headers/libs into this repo:

```powershell
$ninjam = "C:\Users\<you>\Source\Repos\NinjamLib\ninjam"

Copy-Item "$ninjam\ninjam\njclient.h" ".\lib\njclient\njclient.h" -Force

Copy-Item "$ninjam\bin\x64\Debug\MD\njclient.lib" ".\lib\njclient\x64\Debug\MD\njclient.lib" -Force
Copy-Item "$ninjam\bin\x64\Debug\MD\njclient.pdb" ".\lib\njclient\x64\Debug\MD\njclient.pdb" -Force
Copy-Item "$ninjam\bin\x64\Debug\MD\njclient.idb" ".\lib\njclient\x64\Debug\MD\njclient.idb" -Force
Copy-Item "$ninjam\bin\x64\Release\MD\njclient.lib" ".\lib\njclient\x64\Release\MD\njclient.lib" -Force
```

## Troubleshooting: Ninjam Include and Link Errors

If you see errors like:

- `Cannot open include file: 'njclient.h'`
- Linker errors for `njclient.lib`

Verify the vendored files above exist in `lib\njclient`.

### Required Ninjam Link Dependencies

To link successfully, the compilation requires:
- `njclient.lib` from the appropriate `lib\njclient\x64` configuration folder.
- `ogg.lib`, `vorbis.lib`, `vorbisenc.lib`, `vorbisfile.lib` (provided via `vcpkg`).
- `ws2_32.lib` (system library from Windows SDK).
