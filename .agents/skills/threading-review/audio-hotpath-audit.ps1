[CmdletBinding()]
param(
    [string]$Base,
    [string]$Head,
    [switch]$WorkingTree,
    [ValidateSet('text', 'json')][string]$Format = 'text'
)

$ErrorActionPreference = 'Stop'
if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    throw 'Python 3 is required by the shared Jamma review checker.'
}
if (-not $Base) {
    if ($Head -or $WorkingTree) { throw 'Specify -Base for an explicit review scope.' }
    Write-Warning 'Local-edit smoke check only (HEAD versus working tree); committed branch changes are excluded. Use -Base and -Head for branch review.'
    $Base = 'HEAD'
    $WorkingTree = $true
}
if ($Head -and $WorkingTree) { throw '-Head and -WorkingTree are mutually exclusive.' }
$checker = Join-Path $PSScriptRoot '../jamma-review/scripts/review_changes.py'
$reviewArgs = @($checker, '--base', $Base, '--format', $Format)
if ($WorkingTree) { $reviewArgs += '--working-tree' }
elseif ($Head) { $reviewArgs += @('--head', $Head) }
else { $reviewArgs += @('--head', 'HEAD') }
& python @reviewArgs
exit $LASTEXITCODE
