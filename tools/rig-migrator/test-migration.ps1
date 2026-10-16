[CmdletBinding()]
param([string]$TestDirectory = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $TestDirectory) { $TestDirectory = Join-Path $PSScriptRoot ('test-output-' + [Guid]::NewGuid().ToString('N')) }
$root = [IO.Path]::GetFullPath($TestDirectory)
$null = New-Item -ItemType Directory -Path $root
$utf8 = [Text.UTF8Encoding]::new($false)
$script:checks = 0
function Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
    Write-Host "PASS $Message"
}
function Write-Json([string]$Path, $Value) {
    $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path))
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 64), $utf8)
}
function Hash([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Run([string]$Source, [string]$Destination, [string[]]$Extra = @(), [int]$Expected = 0) {
    $ErrorActionPreference = 'Continue' # Capture expected child-process stderr as test evidence.
    $output = & powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'migrate-rig.ps1') -SourceDirectory $Source -DestinationDirectory $Destination @Extra 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    [IO.File]::AppendAllText((Join-Path $root 'runs.log'), (($output | Out-String) + "Exit=$code`r`n"), $utf8)
    Check ($code -eq $Expected) "converter exit $Expected ($([IO.Path]::GetFileName($Source)))"
    return ($output | Out-String)
}
function Legacy {
    return [ordered]@{
        version='1.0';name='Migration fixture';user=[ordered]@{
            audio=[ordered]@{name='default';samplerate=44100;bufsize=512;inlatency=4600;outlatency=6000;numbuffers=4;numchannelsin=2;numchannelsout=2}
            loop=[ordered]@{fadeSamps=800;seedGrainMinMs=400;seedGrainTargetMaxMs=3000;seedBpmMin=80;seedQuantisation='power'}
            trigger=[ordered]@{preDelay=17;debounceSamps=23}
            midi=[ordered]@{devices=@([ordered]@{name='Hardware Name';enabled=$true});channelOverrideTriggers=$false;channelOverrideLive=$true}
            serial=[ordered]@{devices=@([ordered]@{name='pedal';port='COM7';baudrate=115200;enabled=$true})}
        };triggers=@([ordered]@{
            id='one';name='One';stationtype=0;stationtarget='';midiinputmode='selected';midiinputdevices=@('Hardware Name');input=@(0,1)
            pairs=@([ordered]@{source='keyboard';device='keyboard';activatedown=49;activateup=49;ditchdown=50;ditchup=50})
            trigger=[ordered]@{type='midi';device='Hardware Name';activate=[ordered]@{kind='note';id=60;channel=1};ditch=[ordered]@{kind='cc';id=64;channel=1}}
        })
    }
}
$source = Join-Path $root 'legacy'
$destination = Join-Path $root 'canonical'
$sourceRig = Join-Path $source 'main.rig'
Write-Json $sourceRig (Legacy)
$sourceHash = Hash $sourceRig
$null = Run $source $destination @('-DryRun')
Check (-not (Test-Path -LiteralPath $destination)) 'dry-run creates no destination'
$null = Run $source $destination
$outputRig = Join-Path $destination 'main.rig'
$converted = Get-Content -Raw -LiteralPath $outputRig | ConvertFrom-Json
Check ($converted.version -ceq '2.0') 'legacy becomes canonical 2.0'
Check ($converted.settings.trigger.preDelaySamps -eq 17 -and $converted.settings.trigger.debounceMs -eq 23) 'timing numeric values retained'
Check ($converted.devices.midi[0].id -ceq 'midi-1' -and $converted.triggers[0].capture.midi[0] -ceq 'midi-1') 'hardware names become registry references'
Check ($null -eq $converted.triggers[0].target) 'empty legacy target becomes null'
Check ($sourceHash -ceq (Hash $sourceRig)) 'source original hash retained'
$outputHash = Hash $outputRig
$repeat = Run $source $destination
Check ($repeat.Contains('Already equivalent')) 'repeat migration is unchanged'
Check ($outputHash -ceq (Hash $outputRig)) 'repeat preserves output bytes'
Write-Json (Join-Path $destination 'defaults.json') ([ordered]@{rig='untouched';jam='untouched'})
[IO.File]::WriteAllText((Join-Path $destination 'recorded.jam'), 'recording sentinel', $utf8)
$defaultsHash = Hash (Join-Path $destination 'defaults.json')
$recordingHash = Hash (Join-Path $destination 'recorded.jam')
$collision = Legacy
$collision.name = 'Different source'
Write-Json $sourceRig $collision
$null = Run $source $destination
$backups = @(Get-ChildItem -LiteralPath $destination -File -Filter '*.migration-backup-*')
Check ($backups.Count -eq 1 -and (Hash $backups[0].FullName) -ceq $outputHash) 'collision retains exact unique destination backup'
Check ((Hash (Join-Path $destination 'defaults.json')) -ceq $defaultsHash -and (Hash (Join-Path $destination 'recorded.jam')) -ceq $recordingHash) 'rigs-only preserves defaults and recorded JAM'
$beforeBlocked = Hash $outputRig
Write-Json (Join-Path $source 'unsupported.rig') ([ordered]@{version='9.0';name='Unsupported'})
$null = Run $source $destination @() 2
Check ((Hash $outputRig) -ceq $beforeBlocked) 'unsupported rig blocks entire batch without publication'
Remove-Item -LiteralPath (Join-Path $source 'unsupported.rig')
Write-Json (Join-Path $source 'backup-archive/archival.rig') (Legacy)
$skip = Run $source $destination @('-DryRun')
Check ($skip.Contains('SKIPPED backup')) 'archival backup omission is explicit'
$null = Run $source $source @() 1
$junction = Join-Path $root 'source-junction'
$null = New-Item -ItemType Junction -Path $junction -Target $source
try {
    $null = Run $junction (Join-Path $root 'junction-out') @('-DryRun') 1
    Check (-not (Test-Path (Join-Path $root 'junction-out'))) 'source junction ancestor is rejected before planning'
    $null = Run $source (Join-Path $junction 'nested-output') @('-DryRun') 1
    Check (-not (Test-Path (Join-Path $source 'nested-output'))) 'destination junction ancestor cannot bypass directory separation'
} finally { [IO.Directory]::Delete($junction) }
$overlapSource = Join-Path $root 'overlap'
$overlap = Legacy
$overlap.triggers[0].pairs[0].ditchdown=49
$overlap.triggers[0].pairs[0].ditchup=49
Write-Json (Join-Path $overlapSource 'overlap.rig') $overlap
$warning = Run $overlapSource (Join-Path $root 'overlap-out')
Check ($warning.Contains('later overlapping binding removed')) 'removed overlap emits warning'
$anySource = Join-Path $root 'any'
$any = Legacy
$any.triggers[0].midiinputmode='any'
Write-Json (Join-Path $anySource 'any.rig') $any
$warning = Run $anySource (Join-Path $root 'any-out')
Check ($warning.Contains('legacy any/wildcard capture remains disabled')) 'legacy any capture emits warning'
$canonicalSource = Join-Path $root 'canonical-source'
$canonicalDestination = Join-Path $root 'canonical-out'
$canonical = Get-Content -Raw -LiteralPath $outputRig | ConvertFrom-Json
$canonical.triggers[0].controls = [ordered]@{
    ditch=@([ordered]@{source='midi';device='midi-1';button=[ordered]@{kind='noteoff';id=61;channel=$null}})
    activate=@([ordered]@{source='midi';device='midi-1';button=[ordered]@{kind='note';id=61;channel=1}})
}
$canonical.triggers[0].capture.audio=@(0,0,1)
$canonical.triggers[0].capture.midi=@('midi-1','midi-1')
Write-Json (Join-Path $canonicalSource 'ditch-first.rig') $canonical
$warning = Run $canonicalSource $canonicalDestination
Check ($warning.Contains('later overlapping binding removed') -and $warning.Contains('duplicate capture selection removed')) 'canonical shorthand wildcard and capture duplicates normalize with warnings'
$normalized = Get-Content -Raw -LiteralPath (Join-Path $canonicalDestination 'ditch-first.rig') | ConvertFrom-Json
Check ($normalized.triggers[0].controls.activate.Count -eq 0 -and $normalized.triggers[0].controls.ditch[0].press.state -eq 0) 'ditch-first noteoff wildcard wins overlap and preserves inverted state'
$inputOnlySource = Join-Path $root 'input-only'
$inputOnly = Legacy
$inputOnly.user.audio.numchannelsout=0
Write-Json (Join-Path $inputOnlySource 'input-only.rig') $inputOnly
$null = Run $inputOnlySource (Join-Path $root 'input-only-out')
$inputOnlyRig = Get-Content -Raw -LiteralPath (Join-Path $root 'input-only-out/input-only.rig') | ConvertFrom-Json
Check ($inputOnlyRig.devices.audio.numchannelsin -eq 2 -and $inputOnlyRig.devices.audio.numchannelsout -eq 0) 'input-only audio devices remain valid'
$inputOnly.user.audio.numchannelsin=0
Write-Json (Join-Path $inputOnlySource 'input-only.rig') $inputOnly
$null = Run $inputOnlySource (Join-Path $root 'no-channels-out') @() 2
Check (-not (Test-Path (Join-Path $root 'no-channels-out'))) 'zero input and output channels block publication'
$invalidSource = Join-Path $root 'malformed'
$null = [IO.Directory]::CreateDirectory($invalidSource)
[IO.File]::WriteAllText((Join-Path $invalidSource 'duplicate.rig'), '{"version":"2.0","version":"2.0"}', $utf8)
$null = Run $invalidSource (Join-Path $root 'malformed-out') @() 2
Check (-not (Test-Path (Join-Path $root 'malformed-out'))) 'malformed duplicate JSON fields create no destination'
$negativeZeroSource = Join-Path $root 'negative-zero'
$null = [IO.Directory]::CreateDirectory($negativeZeroSource)
$negativeZero = [IO.File]::ReadAllText($outputRig).Replace('"numchannelsin":2','"numchannelsin":-0')
Check ($negativeZero.Contains('"numchannelsin":-0')) 'negative-zero fixture includes signed zero'
[IO.File]::WriteAllText((Join-Path $negativeZeroSource 'negative-zero.rig'), $negativeZero, $utf8)
$null = Run $negativeZeroSource (Join-Path $root 'negative-zero-out') @() 2
Check (-not (Test-Path (Join-Path $root 'negative-zero-out'))) 'negative-zero unsigned field blocks publication like native parser'
$profileSource = Join-Path $root 'recorded-profile'
Write-Json (Join-Path $profileSource 'main.rig') (Legacy)
Write-Json (Join-Path $profileSource 'recorded.jam') ([ordered]@{name='Recorded';stations=@([ordered]@{name='One';takes=@([ordered]@{name='Take'})})})
$null = Run $profileSource (Join-Path $root 'recorded-out') @('-CopyProfile') 2
Check (-not (Test-Path (Join-Path $root 'recorded-out'))) 'CopyProfile refuses recorded sessions before rig publication'
# Construct an interrupted replacement exactly as recorded by the real transaction journal.
# An unsupported source forces recovery to stop before another publication can mask it.
$recoverySource = Join-Path $root 'recovery-source'
Write-Json (Join-Path $recoverySource 'bad.rig') ([ordered]@{version='9.0'})
$recovery = Join-Path $root 'recovery'
$target = Join-Path $recovery 'main.rig'
$backup = $target + '.migration-backup-test'
$stage = $target + '.migration-stage-test'
$null = [IO.Directory]::CreateDirectory($recovery)
[IO.File]::WriteAllText($target, 'original', $utf8)
$original = Hash $target
[IO.File]::Move($target,$backup)
[IO.File]::WriteAllText($target, 'published', $utf8)
$after = Hash $target
$entry = [ordered]@{Target=$target;Stage=$stage;Backup=$backup;Existed=$true;BeforeHash=$original;AfterHash=$after}
Write-Json (Join-Path $recovery '.rig-migration-transaction.json') ([ordered]@{Transaction='test';Entries=@($entry)})
$null = Run $recoverySource $recovery @('-DryRun') 1
Check ((Hash $target) -ceq $after) 'dry-run refuses interrupted transaction without mutation'
$recovered = Run $recoverySource $recovery @() 2
Check ((Hash $target) -ceq $original -and -not (Test-Path (Join-Path $recovery '.rig-migration-transaction.json'))) 'interrupted replacement rolls back exact original'
# New files from interrupted transactions are removed only if their content still matches.
[IO.File]::WriteAllText($target, 'published-new', $utf8)
$entry.Existed=$false;$entry.BeforeHash=$null;$entry.AfterHash=Hash $target
Write-Json (Join-Path $recovery '.rig-migration-transaction.json') ([ordered]@{Transaction='test-new';Entries=@($entry)})
$null = Run $recoverySource $recovery @() 2
Check (-not (Test-Path $target)) 'interrupted new destination is removed during rollback'
[IO.File]::WriteAllText($target, 'independent edit', $utf8)
Write-Json (Join-Path $recovery '.rig-migration-transaction.json') ([ordered]@{Transaction='test-edited';Entries=@($entry)})
$independentHash = Hash $target
$null = Run $recoverySource $recovery @() 1
Check ((Hash $target) -ceq $independentHash -and (Test-Path (Join-Path $recovery '.rig-migration-transaction.json'))) 'rollback refuses independent edits and preserves journal'
$entry.Existed=$true;$entry.BeforeHash=$original;$entry.AfterHash=$independentHash
Write-Json (Join-Path $recovery '.rig-migration-transaction.json') ([ordered]@{Transaction='test-missing-backup';Entries=@($entry)})
$null = Run $recoverySource $recovery @() 1
Check ((Hash $target) -ceq $independentHash -and (Test-Path (Join-Path $recovery '.rig-migration-transaction.json'))) 'missing original backup blocks recovery without discarding journal'
Write-Host "Migration regression checks: $script:checks passed. Retained fixtures and logs: $root"
# Expected converter failures must not leak their native exit code to CI.
exit 0
