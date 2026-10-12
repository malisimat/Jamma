---
name: jamma-tree
description: Create a Jamma feature worktree, copy repo-local configuration without overwriting existing files, then install x64-windows manifest dependencies into that worktree with isolated vcpkg staging paths.
---

# Jamma worktree bootstrap

Use this skill when the user wants a new feature worktree for Jamma.

Use the host harness's file inspection, search, and shell tools for the
operations below. The command examples use PowerShell because Jamma is a
Windows project; adapt the shell syntax only when the host does not provide
PowerShell.

- Infer a short feature slug from the user's prompt. Prefer kebab-case names such as `midi-war`, `hud-ui`, or `vst3-parity`.
- Work from the main Jamma repository, not the new worktree.
- Create the worktree, finish copying folder contents without replacing existing files, then install its manifest dependencies locally.

## Git commands

Use these commands from the main Jamma repository root:

```powershell
$sourceRoot = (Get-Location).Path
$featureSlug = "<inferred-feature-slug>"
$branchName = "feature/$featureSlug"
$worktreeRoot = Join-Path (Split-Path $sourceRoot -Parent) "Jamma.worktrees"
$worktreePath = Join-Path $worktreeRoot "Jamma-$featureSlug"
$currentBranch = git branch --show-current

git worktree add $worktreePath -b $branchName $currentBranch
```

## Copy commands

Copy folder contents from the source repo root into the new worktree root. Do not overwrite existing files.

```powershell
$sourceRoot = "<main-jamma-repo-root>"
$worktreePath = "<new-worktree-root>"

$foldersToCopy = @(
    ".vscode",
    ".agents"
)

foreach ($folder in $foldersToCopy) {
    $sourceFolder = Join-Path $sourceRoot $folder
    $targetFolder = Join-Path $worktreePath $folder

    if (Test-Path $sourceFolder) {
        New-Item -ItemType Directory -Force -Path $targetFolder | Out-Null
        robocopy $sourceFolder $targetFolder /E /XC /XN /XO /R:0 /W:0 | Out-Null
    }
}
```

## Install worktree-local dependencies

After all copying succeeds, run vcpkg in manifest mode for the new worktree.
Use `--triplet x64-windows` (not `--win-x64`). Keep installed dependencies,
build intermediates, and package staging inside the worktree so concurrent
sessions do not mutate the same directories. Reuse the available vcpkg tool;
do not run `vcpkg integrate install`, which changes user-wide integration.
Do not copy or link another worktree's `vcpkg_installed` directory.

```powershell
$worktreePath = (Resolve-Path -LiteralPath $worktreePath).Path
$installedPath = Join-Path $worktreePath "vcpkg_installed"
$vcpkg = (Get-Command vcpkg -CommandType Application -ErrorAction Stop).Source

Push-Location $worktreePath
try {
    & $vcpkg install --triplet x64-windows `
        "--x-manifest-root=$worktreePath" `
        "--x-install-root=$installedPath" `
        "--x-buildtrees-root=$installedPath/.buildtrees" `
        "--x-packages-root=$installedPath/.packages"
    if ($LASTEXITCODE -ne 0) {
        throw "Worktree dependency installation failed (exit $LASTEXITCODE)."
    }
} finally {
    Pop-Location
}
```

`Directory.Build.props` already selects the worktree's `vcpkg_installed`
directory; the staging folders above are also covered by its gitignore entry.
Check any existing `Directory.Build.local.props` for overrides pointing at
another repo's installed directory before declaring the worktree ready.
If vcpkg is unavailable or installation fails, report the incomplete setup
instead of claiming the worktree is ready to build.

Option reference: [vcpkg common command options](https://learn.microsoft.com/en-us/vcpkg/commands/common-options).

## Notes

- If the prompt already contains a usable feature name, reuse it directly; otherwise derive a slug by stripping stop words and punctuation.
- Copy folder contents instead of individual files so future local config can be added without changing the skill.
- Add more repo-local folders to the copy list as needed; keep the pattern generic.
- Declare the worktree ready for build only after copying and dependency installation both succeed.
