---
name: jamma-tree
description: Create a Jamma worktree under ../Jamma.worktrees/Jamma-<feature-slug> on a branch named feature/<feature-slug>, then copy repo-local folder contents into the new worktree without overwriting existing files.
---

# Jamma worktree bootstrap

Use this skill when the user wants a new feature worktree for Jamma.

Use the host harness's file inspection, search, and shell tools for the
operations below. The command examples use PowerShell because Jamma is a
Windows project; adapt the shell syntax only when the host does not provide
PowerShell.

- Infer a short feature slug from the user's prompt. Prefer kebab-case names such as `midi-war`, `hud-ui`, or `vst3-parity`.
- Work from the main Jamma repository, not the new worktree.
- Keep the workflow simple: create the worktree, then copy folder contents into it without replacing anything that already exists.

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

## Notes

- If the prompt already contains a usable feature name, reuse it directly; otherwise derive a slug by stripping stop words and punctuation.
- Copy folder contents instead of individual files so future local config can be added without changing the skill.
- Add more repo-local folders to the copy list as needed; keep the pattern generic.
- After the copy, the worktree should be ready for build with the local repo config in place.
