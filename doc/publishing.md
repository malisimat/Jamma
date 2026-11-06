# Portable Windows releases

In VS Code, run **Tasks: Run Task -> Publish Jamma (Release x64)**.
The task incrementally builds the app and its library in Release x64, then
creates **publish/Jamma-&lt;version&gt;-windows-x64/**. Zip that entire directory
and attach the ZIP to a GitHub Release with a matching `v<version>` tag.
Users extract it and launch `Jamma.exe`; no Jamma installer or separate
Microsoft C++ runtime installation is needed on Windows 10/11 x64.
Audio-interface drivers and third-party plugins are installed separately.

## Version and contents

The version comes from `JammaLib/include/Constants.h`, the same source as
the app's displayed version. Update it and the version in `readme.md` through
the usual release/version process before publishing a new version. Publishing
does not bump the version. Repeating the task replaces only that version's
generated folder; other version folders stay intact. `publish/` is gitignored.

The folder contains the executable, runtime resources, recursively required
vcpkg DLLs, the app-local x64 Microsoft C++ runtime, available dependency
notices, getting-started instructions, and `release.json` with version, UTC
build time, source commit and dirty-worktree status. `SHA256SUMS.txt` hashes
all other packaged files. The script uses fresh staging inputs so deleted
assets and obsolete DLLs cannot survive from a previous build folder.
Debug symbols, authoring sources, user settings, recordings, and test DLLs
are excluded. Keep the matching build PDB separately if needed for debugging.
The build log is `publish/build.log`, outside the distributable folder.

## One-time setup on another machine

Use the existing repository build prerequisites. Copy the task from
`doc/vscode-tasks.example.json` into local `.vscode/tasks.json` and set its
`-MSBuild` argument to that machine's explicit MSBuild executable.
The task uses Windows PowerShell 5.1 and Microsoft's installed build tools;
it needs no packaging utility, installer builder, or additional PowerShell module.
MSBuild builds only `Jamma/Jamma.vcxproj` and its project references.

The runtime is copied from the newest versioned `VC/Redist/MSVC` directory
under the same Visual Studio installation. To use files stored elsewhere,
add these two task arguments:

```json
"-RuntimeDirectory", "D:\\ReleaseDependencies\\Microsoft.VC145.CRT"
```

Supply the x64 CRT redistributable directory containing `msvcp140.dll`,
`vcruntime140.dll`, and the other Microsoft runtime DLLs, from a release
at least as recent as the build toolchain. These files stay outside Git.
The script checks x64 PE architecture and walks actual DLL imports using
Microsoft's `dumpbin.exe`, rejecting missing dependencies and unbundled VC
runtime imports even when this development machine has them installed.
Windows 10/11 provide the Universal CRT and normal Windows system DLLs.

Equivalent command from the repo root (use the path in your local task):

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\publish.ps1 -MSBuild 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
```

## Release checks

Run relevant native tests and smoke-test the staged app from another working
directory. Before a public release, also test the downloaded/extracted ZIP on
a clean supported Windows system, including recording/playback and intended
plugins. A successful developer-machine run cannot prove a clean-machine run.
Build from a committed release revision; `release.json` records local changes
but does not prevent publishing them.

The included `lib/njclient/njclient.h` declares NINJAM GPL v2-or-later terms;
its notice is copied into `licenses/NINJAM.txt`. The repository's MIT licence
alone does not describe every bundled component. Check distribution/source
requirements for linked dependencies and shipped fonts/assets before public
publication; staging notices is not a licence audit.
