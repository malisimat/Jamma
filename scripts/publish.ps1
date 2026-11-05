#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$MSBuild,
    # Optional external directory containing the x64 Microsoft.VC*.CRT DLLs.
    [string]$RuntimeDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path -Parent $PSScriptRoot
$tasksPath = Join-Path $repo '.vscode\tasks.json'
if (-not (Test-Path -LiteralPath $tasksPath)) { throw 'Read/configure .vscode/tasks.json before publishing.' }
$null = Get-Content -Raw -LiteralPath $tasksPath | ConvertFrom-Json
$MSBuild = (Resolve-Path -LiteralPath $MSBuild).Path
$visualStudio = Split-Path -Parent $MSBuild
while (-not (Test-Path -LiteralPath (Join-Path $visualStudio 'VC\Tools\MSVC') -PathType Container)) {
    $parent = Split-Path -Parent $visualStudio
    if (-not $parent -or $parent -eq $visualStudio) { throw "Cannot locate Visual Studio C++ tools above $MSBuild" }
    $visualStudio = $parent
}
$tools = @(Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC\Tools\MSVC') -Directory |
    Sort-Object { [version]$_.Name } -Descending)
$dumpbin = Join-Path $tools[0].FullName 'bin\Hostx64\x64\dumpbin.exe'
if (-not (Test-Path -LiteralPath $dumpbin)) { throw "Microsoft dependency checker missing: $dumpbin" }
if (-not $RuntimeDirectory) {
    $redists = @(Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC\Redist\MSVC') -Directory |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } | Sort-Object { [version]$_.Name } -Descending)
    foreach ($redist in $redists) {
        $crt = @(Get-ChildItem -Path (Join-Path $redist.FullName 'x64\Microsoft.VC*.CRT') -Directory)
        if ($crt.Count -eq 1) { $RuntimeDirectory = $crt[0].FullName; break }
    }
}
if (-not $RuntimeDirectory) { throw 'Supply -RuntimeDirectory pointing to the x64 Microsoft.VC*.CRT directory.' }
$RuntimeDirectory = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
foreach ($required in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
    if (-not (Test-Path -LiteralPath (Join-Path $RuntimeDirectory $required))) {
        throw "Required Microsoft runtime missing: $required in $RuntimeDirectory"
    }
}

$constants = Get-Content -Raw -LiteralPath (Join-Path $repo 'JammaLib\include\Constants.h')
$parts = foreach ($part in 'MAJOR', 'MINOR', 'PATCH') {
    $match = [regex]::Match($constants, "(?m)^#define\s+${part}_VERSION\s+(\d+)\s*$")
    if (-not $match.Success) { throw "Cannot read ${part}_VERSION" }
    $match.Groups[1].Value
}
$version = $parts -join '.'
$name = "Jamma-$version-windows-x64"
$publish = Join-Path $repo 'publish'
$destination = Join-Path $publish $name
$buildOutput = Join-Path $repo 'Jamma\bin\x64\Release'
$dependencies = Join-Path $repo 'vcpkg_installed\x64-windows\bin'
New-Item -ItemType Directory -Force -Path $publish | Out-Null
# Do not follow a junction/symlink into another tree when replacing artifacts.
if ((Get-Item -LiteralPath $publish).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw 'publish must be a real directory inside this worktree.'
}

# Same process-local workaround documented in doc/build.md.
$pathKeys = @([Environment]::GetEnvironmentVariables('Process').Keys | Where-Object { $_ -ieq 'PATH' })
if ($pathKeys.Count -gt 1) { $env:PATH = $null }
if ((@([Environment]::GetEnvironmentVariables('Process').Keys | Where-Object { $_ -ieq 'PATH' })).Count -ne 1) {
    throw 'Expected exactly one process Path variable before building.'
}
$buildLog = Join-Path $publish 'build.log'
$buildArguments = @((Join-Path $repo 'Jamma\Jamma.vcxproj'), '/m', '/t:Build', '/nologo', '/verbosity:minimal',
    '/p:Configuration=Release', '/p:Platform=x64', "/p:SolutionDir=$($repo.TrimEnd('\'))\",
    "/flp:logfile=$buildLog;verbosity=minimal")
& $MSBuild @buildArguments
if ($LASTEXITCODE -ne 0) { throw "Release build failed; see $buildLog" }
[ordered]@{
    script_version = '1.0.0'
    command = @($MSBuild) + $buildArguments
    cwd = $repo
    exit_code = 0
    log = $buildLog
    task = 'Publish Jamma (Release x64)'
    task_sha256 = (Get-FileHash -LiteralPath $tasksPath -Algorithm SHA256).Hash.ToLowerInvariant()
    binary = Join-Path $buildOutput 'Jamma.exe'
    binary_sha256 = (Get-FileHash -LiteralPath (Join-Path $buildOutput 'Jamma.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $publish 'build-record.json') -Encoding utf8

if (Test-Path -LiteralPath $destination) {
    $resolved = (Resolve-Path -LiteralPath $destination).Path
    if (-not [string]::Equals($resolved, [IO.Path]::GetFullPath($destination), [StringComparison]::OrdinalIgnoreCase) -or
        (Get-Item -LiteralPath $destination).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Refusing to replace an artifact outside the exact publish directory.'
    }
    $links = @(Get-ChildItem -LiteralPath $destination -Recurse -Force |
        Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint })
    if ($links.Count) { throw 'Refusing to recursively remove an artifact containing links.' }
    Remove-Item -LiteralPath $destination -Recurse -Force
}
New-Item -ItemType Directory -Path $destination | Out-Null
Copy-Item -LiteralPath (Join-Path $buildOutput 'Jamma.exe') -Destination $destination

$resourceRoot = Join-Path $repo 'Jamma\resources'
foreach ($file in Get-ChildItem -LiteralPath $resourceRoot -File -Recurse) {
    $relative = $file.FullName.Substring($resourceRoot.Length + 1)
    if ($relative -match '(^|[\\/])source[\\/]') { continue }
    $target = Join-Path $destination "resources\$relative"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $target
}
if (-not (Test-Path -LiteralPath (Join-Path $destination 'resources\ResourceList.txt'))) {
    throw 'ResourceList.txt missing from staged resources.'
}
Get-ChildItem -LiteralPath $RuntimeDirectory -Filter '*.dll' -File |
    Copy-Item -Destination $destination

# Follow actual PE imports, including delay imports, rather than copying a stale bin folder.
$queue = New-Object 'System.Collections.Generic.Queue[string]'
Get-ChildItem -LiteralPath $destination -File | ForEach-Object { $queue.Enqueue($_.FullName) }
$checked = @{}
while ($queue.Count) {
    $binary = $queue.Dequeue()
    if ($checked.ContainsKey($binary)) { continue }
    $headers = & $dumpbin /nologo /headers $binary
    if ($LASTEXITCODE -ne 0 -or -not ($headers -match '8664 machine')) { throw "Not an x64 PE binary: $binary" }
    $imports = & $dumpbin /nologo /dependents $binary
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect dependencies: $binary" }
    $checked[$binary] = $true
    foreach ($line in $imports) {
        if ($line -notmatch '^\s+([^\s]+\.dll)\s*$') { continue }
        $dll = $Matches[1]
        $local = Join-Path $destination $dll
        if (Test-Path -LiteralPath $local) { $queue.Enqueue($local); continue }
        $dependency = Join-Path $dependencies $dll
        if (Test-Path -LiteralPath $dependency) {
            Copy-Item -LiteralPath $dependency -Destination $local
            $queue.Enqueue($local)
        } elseif ($dll -match '^(api-ms-|ext-ms-)' -or
            (Test-Path -LiteralPath (Join-Path ([Environment]::SystemDirectory) $dll))) {
            # Windows 10/11 supply Windows DLLs and the Universal CRT.
            # Never accept an installed machine-wide VC runtime as a substitute.
            if ($dll -match '^(msvcp|vcruntime|concrt|vccorlib)\d') { throw "Runtime not bundled: $dll" }
        } else { throw "Unresolved dependency $dll imported by $binary" }
    }
}

$licenses = Join-Path $destination 'licenses'
New-Item -ItemType Directory -Path $licenses | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $destination
foreach ($port in 'kissfft', 'libogg', 'libvorbis', 'vst3sdk') {
    Copy-Item -LiteralPath (Join-Path $repo "vcpkg_installed\x64-windows\share\$port\copyright") -Destination (Join-Path $licenses "$port.txt")
}
$ninjam = Get-Content -Raw -LiteralPath (Join-Path $repo 'lib\njclient\njclient.h')
[regex]::Match($ninjam, '(?s)^/\*.*?\*/').Value | Set-Content -LiteralPath (Join-Path $licenses 'NINJAM.txt') -Encoding utf8
@"
Jamma $version - Windows x64

Extract the entire folder, then run Jamma.exe. Keep resources/ and every DLL
beside the executable. No separate Microsoft C++ runtime install is required
on Windows 10/11 x64. Your audio interface may need its manufacturer's driver.

Choose your audio devices in Jamma. Plugins are installed separately.
For an update, extract into a new folder; do not merge over an old release.
User configuration and recordings are not included in this distribution.

Project and dependency notices are in LICENSE and licenses/.
Microsoft runtime DLLs are redistributed from the Visual Studio redist folder.
"@ | Set-Content -LiteralPath (Join-Path $destination 'START-HERE.txt') -Encoding utf8
$commit = $null
$dirty = $null
if (Get-Command git -ErrorAction SilentlyContinue) {
    $commit = (git -C $repo rev-parse HEAD).Trim()
    $dirty = [bool](git -C $repo status --porcelain --untracked-files=normal)
}
[ordered]@{
    version = $version
    platform = 'windows-x64'
    built_utc = [DateTime]::UtcNow.ToString('o')
    commit = $commit
    working_tree_dirty = $dirty
    runtime_version = (Get-Item -LiteralPath (Join-Path $destination 'msvcp140.dll')).VersionInfo.FileVersion
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destination 'release.json') -Encoding utf8
$hashes = foreach ($file in Get-ChildItem -LiteralPath $destination -Recurse -File | Sort-Object FullName) {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant(),
        $file.FullName.Substring($destination.Length + 1).Replace('\', '/')
}
$hashes | Set-Content -LiteralPath (Join-Path $destination 'SHA256SUMS.txt') -Encoding ascii
Write-Host "`nReady to zip: $destination"
