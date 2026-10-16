# Single rig: migrate-rig.cmd -SourceFile "C:\path\rig.rig" -DestinationDirectory "C:\new-profile"
# Whole profile: run-all.cmd [-SourceDirectory "C:\old-profile"] [-DestinationDirectory "C:\new-profile"] [-DryRun]
# Distinct directories are required. Original source files and destination backups are retained.
[CmdletBinding()]
param(
    [string]$SourceDirectory = '',
    [string]$DestinationDirectory = '',
    [switch]$DryRun,
    [switch]$CopyProfile,
    [string]$SourceFile = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Compatible with Windows PowerShell 5.1. No application or Node installation required.
if (-not $DryRun -and (Get-Process -Name Jamma -ErrorAction SilentlyContinue)) { throw 'Close Jamma before migrating rig files.' }
if (-not [string]::IsNullOrEmpty($SourceFile)) {
    if ($CopyProfile) { throw '-SourceFile cannot be combined with -CopyProfile.' }
    $SourceFile = [IO.Path]::GetFullPath($SourceFile)
    if (-not [IO.File]::Exists($SourceFile) -or [IO.Path]::GetExtension($SourceFile) -ine '.rig') { throw '-SourceFile must name an existing .rig file.' }
    if (-not [string]::IsNullOrEmpty($SourceDirectory)) { throw 'Use either -SourceFile or -SourceDirectory.' }
    $SourceDirectory = [IO.Path]::GetDirectoryName($SourceFile)
}
if ([string]::IsNullOrEmpty($SourceDirectory) -or [string]::IsNullOrEmpty($DestinationDirectory)) {
    $profileDirectory = $env:APPDATA
    if ([string]::IsNullOrEmpty($profileDirectory)) { $profileDirectory = [Environment]::GetFolderPath([Environment+SpecialFolder]::ApplicationData) }
    if ([string]::IsNullOrEmpty($profileDirectory)) { throw 'Cannot locate Roaming AppData; provide -SourceDirectory and -DestinationDirectory.' }
    if ([string]::IsNullOrEmpty($SourceDirectory)) { $SourceDirectory = Join-Path $profileDirectory 'Jamma' }
    if ([string]::IsNullOrEmpty($DestinationDirectory)) { $DestinationDirectory = Join-Path $profileDirectory 'Jamma-rig-cleanup' }
}
$sourceRoot = [IO.Path]::GetFullPath($SourceDirectory).TrimEnd('\','/')
$destinationRoot = [IO.Path]::GetFullPath($DestinationDirectory).TrimEnd('\','/')
function Assert-NoReparseAncestor([string]$Path) {
    $probe=[IO.Path]::GetFullPath($Path)
    while($probe){
        if([IO.File]::Exists($probe) -or [IO.Directory]::Exists($probe)){
            if(((Get-Item -LiteralPath $probe -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw "Reparse-point path ancestor is unsupported: $probe"}
        }
        $probe=[IO.Path]::GetDirectoryName($probe)
    }
}
Assert-NoReparseAncestor $sourceRoot
Assert-NoReparseAncestor $destinationRoot
if (-not [IO.Directory]::Exists($sourceRoot)) { throw "Source directory does not exist: $sourceRoot" }
if ($sourceRoot.Equals($destinationRoot, [StringComparison]::OrdinalIgnoreCase) -or
    $destinationRoot.StartsWith($sourceRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or
    $sourceRoot.StartsWith($destinationRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Source and destination must be separate, non-overlapping directories.'
}
$utf8 = [Text.UTF8Encoding]::new($false, $true)
if (-not ('RigMigrationJson' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Collections.Specialized;
using System.Globalization;
using System.Text;
public sealed class RigMigrationJson {
    readonly string text; int position;
    RigMigrationJson(string text) { this.text = text; }
    void Space() { while(position < text.Length && " \t\r\n".IndexOf(text[position]) >= 0) ++position; }
    bool Eat(char c) { if(position < text.Length && text[position] == c) { ++position; return true; } return false; }
    Exception Bad(string message) { return new FormatException("JSON offset " + position + ": " + message); }
    string String() {
        if(!Eat('"')) throw Bad("Expected string"); var output = new StringBuilder();
        while(position < text.Length) {
            char c = text[position++]; if(c == '"') return output.ToString();
            if(c < 32) throw Bad("Unescaped control character");
            if(c != '\\') { if(Char.IsHighSurrogate(c)) { if(position >= text.Length || !Char.IsLowSurrogate(text[position])) throw Bad("Invalid surrogate"); output.Append(c); output.Append(text[position++]); }
                else { if(Char.IsLowSurrogate(c)) throw Bad("Invalid surrogate"); output.Append(c); } continue; }
            if(position == text.Length) throw Bad("Unterminated escape");
            switch(text[position++]) {
                case '"': output.Append('"'); break; case '\\': output.Append('\\'); break; case '/': output.Append('/'); break;
                case 'b': output.Append('\b'); break; case 'f': output.Append('\f'); break; case 'n': output.Append('\n'); break;
                case 'r': output.Append('\r'); break; case 't': output.Append('\t'); break;
                case 'u': char u = Hex(); if(Char.IsHighSurrogate(u)) { if(!Eat('\\') || !Eat('u')) throw Bad("Missing low surrogate"); char low = Hex(); if(!Char.IsLowSurrogate(low)) throw Bad("Invalid low surrogate"); output.Append(u); output.Append(low); }
                    else { if(Char.IsLowSurrogate(u)) throw Bad("Unpaired low surrogate"); output.Append(u); } break;
                default: throw Bad("Invalid escape");
            }
        } throw Bad("Unterminated string");
    }
    char Hex() { if(position + 4 > text.Length) throw Bad("Short unicode escape"); ushort n; if(!UInt16.TryParse(text.Substring(position,4), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture,out n)) throw Bad("Invalid unicode escape"); position += 4; return (char)n; }
    object Value(int depth) {
        if(depth > 64) throw Bad("Nesting exceeds 64"); Space(); if(position == text.Length) throw Bad("Missing value");
        if(text[position] == '"') return String();
        if(Eat('{')) { var result = new OrderedDictionary(StringComparer.Ordinal); Space(); if(Eat('}')) return result;
            do { Space(); string key = String(); Space(); if(!Eat(':')) throw Bad("Missing colon"); if(result.Contains(key)) throw Bad("Duplicate field " + key); result.Add(key,Value(depth+1)); Space(); if(Eat('}')) return result; } while(Eat(',')); throw Bad("Missing object separator"); }
        if(Eat('[')) { var result = new List<object>(); Space(); if(Eat(']')) return result.ToArray();
            do { result.Add(Value(depth+1)); Space(); if(Eat(']')) return result.ToArray(); } while(Eat(',')); throw Bad("Missing array separator"); }
        foreach(string literal in new [] {"true","false","null"}) if(position+literal.Length <= text.Length && text.Substring(position,literal.Length) == literal) { position += literal.Length; return literal == "null" ? null : (object)(literal == "true"); }
        int start = position; Eat('-'); if(!Eat('0')) { if(position >= text.Length || text[position] < '1' || text[position] > '9') throw Bad("Invalid number"); while(position < text.Length && text[position] >= '0' && text[position] <= '9') ++position; }
        bool floating = false; if(Eat('.')) { floating = true; int digits=position; while(position < text.Length && text[position] >= '0' && text[position] <= '9') ++position; if(digits == position) throw Bad("Missing fractional digits"); }
        if(Eat('e') || Eat('E')) { floating = true; if(!Eat('+')) Eat('-'); int digits=position; while(position < text.Length && text[position] >= '0' && text[position] <= '9') ++position; if(digits == position) throw Bad("Missing exponent digits"); }
        string number = text.Substring(start,position-start); if(number == "-0") throw Bad("Negative zero cannot represent an unsigned rig value"); if(!floating) { long integer; if(Int64.TryParse(number, NumberStyles.AllowLeadingSign,CultureInfo.InvariantCulture,out integer)) return integer; }
        double value; if(!Double.TryParse(number,NumberStyles.Float,CultureInfo.InvariantCulture,out value) || Double.IsInfinity(value) || Double.IsNaN(value)) throw Bad("Number out of range"); return value;
    }
    public static object Parse(string text) { var reader = new RigMigrationJson(text); object value = reader.Value(0); reader.Space(); if(reader.position != text.Length) throw reader.Bad("Trailing input"); return value; }
}
'@
}
function Read-Json([string]$Path) { return ,([RigMigrationJson]::Parse([IO.File]::ReadAllText($Path, $utf8))) }
function Require-Object($Value, [string]$Path) { if ($Value -isnot [Collections.IDictionary]) { throw "$Path : expected object" }; return ,$Value }
function Require-Array($Value, [string]$Path) { if ($Value -isnot [object[]]) { throw "$Path : expected array" }; return ,$Value }
function Field($Object, [string]$Key, [string]$Path) { if (-not $Object.Contains($Key)) { throw "$Path.$Key : required field missing" }; return ,$Object[$Key] }
function Check-Keys($Object, [string[]]$Allowed, [string]$Path) { $null = Require-Object $Object $Path; foreach ($key in $Object.Keys) { if ($Allowed -cnotcontains $key) { throw "$Path.$key : unknown field" } } }
function Text($Value, [string]$Path) { if ($Value -isnot [string] -or $Value.Length -eq 0 -or $Value.IndexOf([char]0) -ge 0) { throw "$Path : expected nonempty string" }; return $Value }
function Boolean($Value, [string]$Path) { if ($Value -isnot [bool]) { throw "$Path : expected boolean" }; return $Value }
function UInt($Value, [string]$Path, [uint64]$Maximum = 4294967295) { if (($Value -isnot [long] -and $Value -isnot [int] -and $Value -isnot [uint32]) -or $Value -lt 0 -or $Value -gt $Maximum) { throw "$Path : expected integer 0..$Maximum" }; return [uint32]$Value }
function Required-Text($Object, [string]$Key, [string]$Path) { return Text (Field $Object $Key $Path) "$Path.$Key" }
function Required-UInt($Object, [string]$Key, [string]$Path, [uint64]$Maximum = 4294967295) { return UInt (Field $Object $Key $Path) "$Path.$Key" $Maximum }
function Required-Bool($Object, [string]$Key, [string]$Path) { return Boolean (Field $Object $Key $Path) "$Path.$Key" }
function Set-OfStrings { return ,([Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)) }
function Hash-Bytes([byte[]]$Bytes) { $sha = [Security.Cryptography.SHA256]::Create(); try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-','').ToLowerInvariant() } finally { $sha.Dispose() } }
function File-Hash([string]$Path) { return Hash-Bytes ([IO.File]::ReadAllBytes($Path)) }
function Warning([string]$Message) { $script:notes.Add($Message); Write-Host "  WARNING $Message" }
function Midi-Edge($Spec, [string]$Path, [switch]$Legacy) {
    Check-Keys $Spec @('kind','id','channel') $Path
    $kind = Required-Text $Spec 'kind' $Path; $state = 1
    if ($Legacy -and @('noteon','note-on','note on') -ccontains $kind) { $kind = 'note' }
    if (@('noteoff','note-off','note off') -ccontains $kind) { $kind = 'note'; $state = 0 }
    if (@('note','cc') -cnotcontains $kind) { throw "$Path.kind : unsupported MIDI kind" }
    $id = Required-UInt $Spec 'id' $Path 127
    $channel = $null
    if ($Spec.Contains('channel') -and $null -ne $Spec['channel']) { $channel = Required-UInt $Spec 'channel' $Path 16; if ($channel -eq 0) { throw "$Path.channel : expected 1..16" } }
    return [ordered]@{ kind=$kind; id=$id; state=$state; channel=$channel }
}
function Midi-Binding([string]$Device, $Spec, [string]$Path) { $press = Midi-Edge $Spec $Path -Legacy; $release = [ordered]@{kind=$press.kind; id=$press.id; state=(1-$press.state); channel=$press.channel}; return [ordered]@{source='midi';device=$Device;press=$press;release=$release} }
function Button-Binding([string]$Source, [string]$Device, [uint32]$Down, [uint32]$Up) { return [ordered]@{source=$Source;device=$Device;press=[ordered]@{code=$Down;state=1};release=[ordered]@{code=$Up;state=0}} }
function Resolve-Midi($ByName, [string]$Name, [string]$Path) { if (-not $ByName.ContainsKey($Name)) { throw "$Path : MIDI hardware name has no registry entry" }; return $ByName[$Name] }
function Convert-Legacy($Rig) {
    Check-Keys $Rig @('version','name','user','triggers') '$'
    if ((Required-Text $Rig 'version' '$') -cne '1.0') { throw '$.version : unsupported version' }
    $name = Required-Text $Rig 'name' '$'
    $user = Require-Object (Field $Rig 'user' '$') '$.user'
    Check-Keys $user @('audio','loop','trigger','midi','serial') '$.user'
    $audio = Require-Object (Field $user 'audio' '$.user') '$.user.audio'
    $loop = Require-Object (Field $user 'loop' '$.user') '$.user.loop'
    $timing = Require-Object (Field $user 'trigger' '$.user') '$.user.trigger'
    Check-Keys $timing @('preDelay','debounceSamps','preDelaySamps','debounceMs') '$.user.trigger'
    if (($timing.Contains('preDelay') -and $timing.Contains('preDelaySamps')) -or ($timing.Contains('debounceSamps') -and $timing.Contains('debounceMs'))) { throw '$.user.trigger : ambiguous old and new timing fields' }
    $preKey = 'preDelay'; if ($timing.Contains('preDelaySamps')) { $preKey = 'preDelaySamps' }
    $debKey = 'debounceSamps'; if ($timing.Contains('debounceMs')) { $debKey = 'debounceMs' }
    $triggerSettings = [ordered]@{preDelaySamps=(Required-UInt $timing $preKey '$.user.trigger'); debounceMs=(Required-UInt $timing $debKey '$.user.trigger')}
    $midiConfig = Require-Object (Field $user 'midi' '$.user') '$.user.midi'
    Check-Keys $midiConfig @('devices','channelOverrideTriggers','channelOverrideLive') '$.user.midi'
    $midiSettings = [ordered]@{channelOverrideTriggers=(Required-Bool $midiConfig 'channelOverrideTriggers' '$.user.midi');channelOverrideLive=(Required-Bool $midiConfig 'channelOverrideLive' '$.user.midi')}
    $serialConfig = Require-Object (Field $user 'serial' '$.user') '$.user.serial'
    Check-Keys $serialConfig @('devices') '$.user.serial'
    $midi = [Collections.Generic.List[object]]::new(); $serial = [Collections.Generic.List[object]]::new()
    $byName = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::Ordinal)
    $usedIds = Set-OfStrings; $null = $usedIds.Add('keyboard')
    $serialDevices = Require-Array (Field $serialConfig 'devices' '$.user.serial') '$.user.serial.devices'
    for ($i=0;$i -lt $serialDevices.Length;$i++) {
        $device=$serialDevices[$i]; $path="$.user.serial.devices[$i]"; Check-Keys $device @('name','port','baudrate','enabled') $path
        $id=Required-Text $device 'name' $path; if (-not $usedIds.Add($id)) { throw "$path.name : duplicate/reserved device ID" }
        $serial.Add([ordered]@{id=$id;port=(Required-Text $device 'port' $path);baudrate=(Required-UInt $device 'baudrate' $path);enabled=(Required-Bool $device 'enabled' $path)})
    }
    $midiDevices = Require-Array (Field $midiConfig 'devices' '$.user.midi') '$.user.midi.devices'
    for ($i=0;$i -lt $midiDevices.Length;$i++) {
        $device=$midiDevices[$i]; $path="$.user.midi.devices[$i]"; Check-Keys $device @('name','enabled') $path
        $hardware=Required-Text $device 'name' $path
        if ($byName.ContainsKey($hardware)) { throw "$path.name : duplicate hardware name makes legacy references ambiguous" }
        $id='midi-'+($i+1); if (-not $usedIds.Add($id)) { throw "$path : generated registry ID conflicts with serial entry" }
        $byName.Add($hardware,$id); $midi.Add([ordered]@{id=$id;name=$hardware;enabled=(Required-Bool $device 'enabled' $path)})
    }
    $triggers=[Collections.Generic.List[object]]::new()
    $oldTriggers=Require-Array (Field $Rig 'triggers' '$') '$.triggers'
    for ($i=0;$i -lt $oldTriggers.Length;$i++) {
        $trigger=$oldTriggers[$i];$path="$.triggers[$i]"
        Check-Keys $trigger @('id','name','stationtype','stationtarget','midiinputmode','pairs','input','midiinputdevices','trigger') $path
        $id=Required-Text $trigger 'id' $path; $display=Required-Text $trigger 'name' $path
        $null=Required-UInt $trigger 'stationtype' $path
        $target=Field $trigger 'stationtarget' $path
        if ($target -isnot [string]) { throw "$path.stationtarget : expected explicit target string" }
        if ($target.Length -eq 0) { $target=$null }
        $activate=[Collections.Generic.List[object]]::new();$ditch=[Collections.Generic.List[object]]::new()
        $pairs=Require-Array (Field $trigger 'pairs' $path) "$path.pairs"
        for ($p=0;$p -lt $pairs.Length;$p++) {
            $pair=$pairs[$p];$pairPath="$path.pairs[$p]";Check-Keys $pair @('source','device','activatedown','activateup','ditchdown','ditchup') $pairPath
            $source='keyboard'; if ($pair.Contains('source')) { $source=Required-Text $pair 'source' $pairPath }
            if (@('keyboard','serial') -cnotcontains $source) { throw "$pairPath.source : unsupported source" }
            $device='keyboard'
            if ($pair.Contains('device') -and $pair['device'] -isnot [string]) { throw "$pairPath.device : expected string" }
            if ($source -ceq 'serial') { $device='default';if ($pair.Contains('device') -and $pair['device'] -is [string] -and $pair['device'].Length -gt 0) { $device=$pair['device'] } }
            elseif ($pair.Contains('device') -and ($pair['device'] -isnot [string] -or @('','keyboard') -cnotcontains $pair['device'])) { throw "$pairPath.device : unsupported keyboard device selector" }
            $activate.Add((Button-Binding $source $device (Required-UInt $pair 'activatedown' $pairPath) (Required-UInt $pair 'activateup' $pairPath)))
            $ditch.Add((Button-Binding $source $device (Required-UInt $pair 'ditchdown' $pairPath) (Required-UInt $pair 'ditchup' $pairPath)))
        }
        if ($trigger.Contains('trigger')) {
            $control=$trigger['trigger'];Check-Keys $control @('type','device','activate','ditch') "$path.trigger"
            if ((Required-Text $control 'type' "$path.trigger") -cne 'midi') { throw "$path.trigger.type : expected midi" }
            $device=Resolve-Midi $byName (Required-Text $control 'device' "$path.trigger") "$path.trigger.device"
            $activate.Add((Midi-Binding $device (Field $control 'activate' "$path.trigger") "$path.trigger.activate"))
            $ditch.Add((Midi-Binding $device (Field $control 'ditch' "$path.trigger") "$path.trigger.ditch"))
        }
        $audioCapture=Require-Array (Field $trigger 'input' $path) "$path.input"
        $midiCapture=Require-Array (Field $trigger 'midiinputdevices' $path) "$path.midiinputdevices"
        foreach($selector in $midiCapture){if($selector -isnot [string]){throw "$path.midiinputdevices : expected string entries"}}
        $mode=Required-Text $trigger 'midiinputmode' $path
        if (@('none','selected','any') -cnotcontains $mode) { throw "$path.midiinputmode : unsupported mode" }
        $capture=[Collections.Generic.List[object]]::new()
        if ($mode -ceq 'selected') { foreach($selector in $midiCapture) { $capture.Add((Resolve-Midi $byName (Text $selector "$path.midiinputdevices") "$path.midiinputdevices")) } }
        else {
            if ($mode -ceq 'none' -and $midiCapture.Length -gt 0) { throw "$path.midiinputdevices : none mode has nonempty selection" }
            if ($mode -ceq 'any') { Warning "${path}: legacy any/wildcard capture remains disabled" }
        }
        $triggers.Add([ordered]@{id=$id;name=$display;target=$target;controls=[ordered]@{activate=$activate.ToArray();ditch=$ditch.ToArray()};capture=[ordered]@{audio=$audioCapture;midi=$capture.ToArray()}})
    }
    return [ordered]@{version='2.0';name=$name;devices=[ordered]@{audio=$audio;midi=$midi.ToArray();serial=$serial.ToArray()};settings=[ordered]@{loop=$loop;trigger=$triggerSettings;midi=$midiSettings};triggers=$triggers.ToArray()}
}

function Canonical-Edge($Edge, [string]$Source, [string]$Path) {
    if ($Source -ceq 'midi') {
        Check-Keys $Edge @('kind','id','state','channel') $Path
        $kind=Required-Text $Edge 'kind' $Path; if (@('note','cc') -cnotcontains $kind) { throw "$Path.kind : expected note or cc" }
        $channel=Field $Edge 'channel' $Path
        if ($null -ne $channel) { $channel=UInt $channel "$Path.channel" 16; if ($channel -eq 0) { throw "$Path.channel : expected 1..16 or null" } }
        return [ordered]@{kind=$kind;id=(Required-UInt $Edge 'id' $Path 127);state=(Required-UInt $Edge 'state' $Path 1);channel=$channel}
    }
    Check-Keys $Edge @('code','state') $Path
    return [ordered]@{code=(Required-UInt $Edge 'code' $Path);state=(Required-UInt $Edge 'state' $Path 1)}
}
function Canonical-Binding($Binding, $Registry, [string]$Path) {
    Check-Keys $Binding @('source','device','press','release','button') $Path
    $source=Required-Text $Binding 'source' $Path;$device=Required-Text $Binding 'device' $Path
    if (@('keyboard','serial','midi') -cnotcontains $source) { throw "$Path.source : invalid source kind" }
    if (-not $Registry.ContainsKey($device) -or $Registry[$device] -cne $source) { throw "$Path.device : missing device registry reference or wrong kind" }
    $hasButton=$Binding.Contains('button');$hasExpanded=$Binding.Contains('press') -or $Binding.Contains('release')
    if ($hasButton -eq $hasExpanded) { throw "$Path : specify button or press/release" }
    if ($hasButton) {
        if ($source -ceq 'midi') {
            $spec=Require-Object $Binding['button'] "$Path.button"
            Check-Keys $spec @('kind','id','channel') "$Path.button"
            $kind=Required-Text $spec 'kind' "$Path.button"
            if (@('note','noteoff','cc') -cnotcontains $kind) { throw "$Path.button.kind : unsupported kind" }
            $null=Field $spec 'channel' "$Path.button"
            $press=Midi-Edge $spec "$Path.button"
            $release=[ordered]@{kind=$press.kind;id=$press.id;state=(1-$press.state);channel=$press.channel}
        }
        else { $code=Required-UInt $Binding 'button' $Path; $press=[ordered]@{code=$code;state=1};$release=[ordered]@{code=$code;state=0} }
    }
    else { $press=Canonical-Edge (Field $Binding 'press' $Path) $source "$Path.press";$release=Canonical-Edge (Field $Binding 'release' $Path) $source "$Path.release" }
    if ($source -ceq 'midi' -and $press.channel -ne $release.channel) { throw "$Path.release.channel : must match press channel selector" }
    if (Edge-Overlap $source $press $release) { throw "$Path.release : must match an event distinct from press" }
    return [ordered]@{source=$source;device=$device;press=$press;release=$release}
}
function Edge-Overlap([string]$Source,$First,$Second) {
    if ($First.state -ne $Second.state) { return $false }
    if ($Source -cne 'midi') { return $First.code -eq $Second.code }
    return $First.kind -ceq $Second.kind -and $First.id -eq $Second.id -and ($null -eq $First.channel -or $null -eq $Second.channel -or $First.channel -eq $Second.channel)
}
function Binding-Overlap($First,$Second) {
    if ($First.source -cne $Second.source -or $First.device -cne $Second.device) { return $false }
    foreach($a in @($First.press,$First.release)) { foreach($b in @($Second.press,$Second.release)) { if (Edge-Overlap $First.source $a $b) { return $true } } }
    return $false
}
function Validate-Canonical($Rig) {
    Check-Keys $Rig @('version','name','devices','settings','triggers') '$'
    if ((Required-Text $Rig 'version' '$') -cne '2.0') { throw '$.version : unsupported version' }
    $null=Required-Text $Rig 'name' '$'
    $devices=Require-Object (Field $Rig 'devices' '$') '$.devices';Check-Keys $devices @('audio','midi','serial') '$.devices'
    $settings=Require-Object (Field $Rig 'settings' '$') '$.settings';Check-Keys $settings @('loop','trigger','midi') '$.settings'
    $audio=Require-Object (Field $devices 'audio' '$.devices') '$.devices.audio'
    Check-Keys $audio @('name','samplerate','bufsize','inlatency','outlatency','numbuffers','numchannelsin','numchannelsout') '$.devices.audio'
    $null=Required-Text $audio 'name' '$.devices.audio'
    foreach($key in @('samplerate','bufsize','inlatency','outlatency','numbuffers','numchannelsin','numchannelsout')) { $null=Required-UInt $audio $key '$.devices.audio' }
    foreach($key in @('samplerate','bufsize','numbuffers')) { if ($audio[$key] -eq 0) { throw "$.devices.audio.$key : must be positive" } }
    if ($audio.numchannelsin -eq 0 -and $audio.numchannelsout -eq 0) { throw '$.devices.audio : at least one input or output channel is required' }
    if ($audio.numchannelsin -gt 1024 -or $audio.numchannelsout -gt 1024) { throw '$.devices.audio : channel capacity exceeds 1024' }
    $loop=Require-Object (Field $settings 'loop' '$.settings') '$.settings.loop'
    Check-Keys $loop @('fadeSamps','seedGrainMinMs','seedGrainTargetMaxMs','seedBpmMin','seedQuantisation') '$.settings.loop'
    foreach($key in @('fadeSamps','seedGrainMinMs','seedGrainTargetMaxMs','seedBpmMin')) { $null=Required-UInt $loop $key '$.settings.loop' }
    $quantisation=Required-Text $loop 'seedQuantisation' '$.settings.loop'
    if (@('power','multiple') -cnotcontains $quantisation -or $loop.seedGrainMinMs -eq 0 -or $loop.seedGrainTargetMaxMs -lt $loop.seedGrainMinMs -or $loop.seedBpmMin -eq 0) { throw '$.settings.loop : invalid seed timing policy' }
    $timing=Require-Object (Field $settings 'trigger' '$.settings') '$.settings.trigger';Check-Keys $timing @('preDelaySamps','debounceMs') '$.settings.trigger'
    $null=Required-UInt $timing 'preDelaySamps' '$.settings.trigger' 4294897295;$null=Required-UInt $timing 'debounceMs' '$.settings.trigger'
    $midiSettings=Require-Object (Field $settings 'midi' '$.settings') '$.settings.midi';Check-Keys $midiSettings @('channelOverrideTriggers','channelOverrideLive') '$.settings.midi'
    $null=Required-Bool $midiSettings 'channelOverrideTriggers' '$.settings.midi';$null=Required-Bool $midiSettings 'channelOverrideLive' '$.settings.midi'
    $registry=[Collections.Generic.Dictionary[string,string]]::new([StringComparer]::Ordinal);$registry.Add('keyboard','keyboard')
    foreach($kind in @('midi','serial')) {
        $list=Require-Array (Field $devices $kind '$.devices') "$.devices.$kind"
        for($i=0;$i -lt $list.Length;$i++) {
            $entry=$list[$i];$path="$.devices.$kind[$i]"
            if ($kind -ceq 'midi') { Check-Keys $entry @('id','name','enabled') $path;$null=Required-Text $entry 'name' $path }
            else { Check-Keys $entry @('id','port','baudrate','enabled') $path;$null=Required-Text $entry 'port' $path;$baud=Required-UInt $entry 'baudrate' $path;if($baud -eq 0){throw "$path.baudrate : must be positive"} }
            $id=Required-Text $entry 'id' $path;$null=Required-Bool $entry 'enabled' $path
            if($registry.ContainsKey($id)){throw "$path.id : duplicate/reserved ID"};$registry.Add($id,$kind)
        }
    }
    $triggers=Require-Array (Field $Rig 'triggers' '$') '$.triggers';if($triggers.Length -gt 256){throw '$.triggers : capacity exceeds 256'}
    $ids=Set-OfStrings;$names=Set-OfStrings
    for($i=0;$i -lt $triggers.Length;$i++) {
        $entry=$triggers[$i];$path="$.triggers[$i]";Check-Keys $entry @('id','name','target','controls','capture') $path
        $id=Required-Text $entry 'id' $path;$name=Required-Text $entry 'name' $path
        if(-not $ids.Add($id)){throw "$path.id : duplicate trigger identity"};if(-not $names.Add($name)){throw "$path.name : duplicate trigger name"}
        $target=Field $entry 'target' $path;if($null -ne $target){$null=Text $target "$path.target"}
        $controls=Require-Object (Field $entry 'controls' $path) "$path.controls";Check-Keys $controls @('activate','ditch') "$path.controls"
        $null=Field $controls 'activate' "$path.controls";$null=Field $controls 'ditch' "$path.controls"
        $prior=[Collections.Generic.List[object]]::new()
        foreach($group in @($controls.Keys)) {
            $list=Require-Array $controls[$group] "$path.controls.$group";$retained=[Collections.Generic.List[object]]::new();$compiled=0
            for($b=0;$b -lt $list.Length;$b++) {
                $binding=Canonical-Binding $list[$b] $registry "$path.controls.$group[$b]";$overlap=$false
                foreach($previous in $prior){if(Binding-Overlap $previous $binding){$overlap=$true;break}}
                if($overlap){Warning "$path.controls.$group[$b] : later overlapping binding removed; earliest definition wins";continue}
                $width=1;if($binding.source -ceq 'midi' -and $null -eq $binding.press.channel){$width=16};$compiled+=$width
                if($compiled -gt 64){throw "$path.controls.$group : compiled capacity exceeds 64"}
                $retained.Add($binding);$prior.Add($binding)
            }
            $controls[$group]=$retained.ToArray()
        }
        $capture=Require-Object (Field $entry 'capture' $path) "$path.capture";Check-Keys $capture @('audio','midi') "$path.capture"
        foreach($kind in @('audio','midi')) {
            $list=Require-Array (Field $capture $kind "$path.capture") "$path.capture.$kind";$seen=Set-OfStrings;$retained=[Collections.Generic.List[object]]::new()
            for($c=0;$c -lt $list.Length;$c++) {
                if($kind -ceq 'audio'){$selection=UInt $list[$c] "$path.capture.audio[$c]" 1023}
                else {$selection=Text $list[$c] "$path.capture.midi[$c]";if(-not $registry.ContainsKey($selection) -or $registry[$selection] -cne 'midi'){throw "$path.capture.midi[$c] : invalid MIDI registry reference"}}
                if($seen.Add([string]$selection)){$retained.Add($selection)}else{Warning "$path.capture.$kind[$c] : duplicate capture selection removed"}
            }
            $capture[$kind]=$retained.ToArray()
        }
    }
    return ,$Rig
}
# Ignore object-field order except controls, whose textual order determines overlap priority.
function Semantic($Value,[string]$Path='$') {
    if($null -eq $Value){return 'null'}
    if($Value -is [Collections.IDictionary]) {
        $keys=[string[]]@($Value.Keys)
        if(-not $Path.EndsWith('.controls',[StringComparison]::Ordinal)){[Array]::Sort($keys,[StringComparer]::Ordinal)}
        $parts=[Collections.Generic.List[string]]::new();foreach($key in $keys){$parts.Add(($key | ConvertTo-Json -Compress)+':'+(Semantic $Value[$key] "$Path.$key"))}
        return '{'+[string]::Join(',',$parts.ToArray())+'}'
    }
    if($Value -is [object[]]){$parts=[Collections.Generic.List[string]]::new();for($i=0;$i -lt $Value.Length;$i++){$parts.Add((Semantic $Value[$i] "$Path[$i]"))};return '['+[string]::Join(',',$parts.ToArray())+']'}
    return ($Value | ConvertTo-Json -Compress)
}

function Relative-Source([string]$Path) {
    $absolute=[IO.Path]::GetFullPath($Path)
    if(-not $absolute.StartsWith($sourceRoot+'\',[StringComparison]::OrdinalIgnoreCase)){throw "Source path escapes source directory: $Path"}
    return $absolute.Substring($sourceRoot.Length+1)
}
function Destination-Path([string]$Relative) {
    $path=[IO.Path]::GetFullPath((Join-Path $destinationRoot $Relative));Assert-Destination $path;return $path
}
function Assert-Destination([string]$Path) {
    $absolute=[IO.Path]::GetFullPath($Path)
    if(-not $absolute.StartsWith($destinationRoot+'\',[StringComparison]::OrdinalIgnoreCase)){throw "Destination path escapes destination directory: $Path"}
    Assert-NoReparseAncestor $absolute
}
function Compatible-Jam($Jam,[string]$Path) {
    $null=Require-Object $Jam $Path;$null=Required-Text $Jam 'name' $Path
    if($Jam.Contains('formatVersion')){
        $version=Required-Text $Jam 'formatVersion' $Path
        if($version -notmatch '^0\.[0-9]+\.[0-9]+$'){throw "$Path.formatVersion : unsupported session version"}
    }
    $stations=Require-Array (Field $Jam 'stations' $Path) "$Path.stations";$names=Set-OfStrings
    if($stations.Length -eq 0){throw "$Path.stations : no usable station"}
    for($i=0;$i -lt $stations.Length;$i++){
        $station=Require-Object $stations[$i] "$Path.stations[$i]";$name=Required-Text $station 'name' "$Path.stations[$i]"
        if(-not $names.Add($name)){throw "$Path.stations[$i].name : duplicate station name"}
        $takes=[object[]]@();if($station.Contains('takes')){$takes=Require-Array $station['takes'] "$Path.stations[$i].takes"}
        if($takes.Length -gt 0){throw "$Path.stations[$i].takes : recorded-session assets need separate copying; use the default rigs-only mode"}
    }
}
function Remap-DefaultPath([string]$Path,[string]$Kind,$Planned) {
    if(-not [IO.Path]::IsPathRooted($Path)){$absolute=[IO.Path]::GetFullPath((Join-Path $sourceRoot $Path))}else{$absolute=[IO.Path]::GetFullPath($Path)}
    if($absolute.StartsWith($sourceRoot+'\',[StringComparison]::OrdinalIgnoreCase)){
        $relative=Relative-Source $absolute
        if(-not $Planned.ContainsKey($relative) -or $Planned[$relative].Kind -cne $Kind){throw "defaults.$Kind : selected source-relative file was not successfully prepared: $relative"}
        return Destination-Path $relative
    }
    if(-not [IO.File]::Exists($absolute)){throw "defaults.$Kind : selected external file is missing: $absolute"}
    if($Kind -ceq 'rig'){$null=Validate-Canonical (Read-Json $absolute)}else{Compatible-Jam (Read-Json $absolute) "defaults.$Kind"}
    Warning "defaults.$Kind : external path retained unchanged: $Path"
    return $Path
}
function Rollback($Entries) {
    for($i=$Entries.Length-1;$i -ge 0;$i--){
        $entry=$Entries[$i];Assert-Destination $entry.Target;Assert-Destination $entry.Stage;Assert-Destination $entry.Backup
        if([IO.File]::Exists($entry.Backup)){
            if((File-Hash $entry.Backup) -cne $entry.BeforeHash){throw "Backup differs from original destination; preserve journal and backup: $($entry.Backup)"}
            if([IO.File]::Exists($entry.Target)){
                $currentHash=File-Hash $entry.Target
                if($currentHash -cne $entry.BeforeHash -and $currentHash -cne $entry.AfterHash){throw "Destination changed independently; refusing rollback overwrite: $($entry.Target)"}
                $discard=$entry.Target+'.rollback-discard-'+[Guid]::NewGuid().ToString('N');Assert-Destination $discard
                [IO.File]::Replace($entry.Backup,$entry.Target,$discard)
                [IO.File]::Delete($discard)
            }else{[IO.File]::Move($entry.Backup,$entry.Target)}
            if((File-Hash $entry.Target) -cne $entry.BeforeHash){throw "Rollback hash mismatch: $($entry.Target)"}
        }
        elseif(-not $entry.Existed -and -not [IO.File]::Exists($entry.Stage) -and [IO.File]::Exists($entry.Target)){
            if((File-Hash $entry.Target) -cne $entry.AfterHash){throw "Interrupted destination changed independently; refusing deletion: $($entry.Target)"}
            [IO.File]::Delete($entry.Target)
        }
        elseif($entry.Existed){
            # No backup means replacement has not started, or rollback already restored this entry.
            # Never discard the recovery journal when neither the original nor its backup remains.
            if(-not [IO.File]::Exists($entry.Target) -or (File-Hash $entry.Target) -cne $entry.BeforeHash){throw "Original destination and verified backup unavailable; preserve journal: $($entry.Target)"}
        }
        if([IO.File]::Exists($entry.Stage)){[IO.File]::Delete($entry.Stage)}
    }
}
$notes=[Collections.Generic.List[string]]::new();$failures=[Collections.Generic.List[string]]::new()
$journalPath=Join-Path $destinationRoot '.rig-migration-transaction.json'
if(-not $DryRun){
    $mutexName='Local\RigMigrator-'+(Hash-Bytes ($utf8.GetBytes($destinationRoot.ToLowerInvariant())))
    $migrationMutex=[Threading.Mutex]::new($false,$mutexName)
    try{$owned=$migrationMutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$owned=$true}
    if(-not $owned){$migrationMutex.Dispose();throw 'Another migrator is already writing this destination.'}
    # The standalone process releases the mutex on normal exit or interruption.
}
Write-Host "RigMigrator: source=$sourceRoot"
Write-Host "Destination=$destinationRoot"
if($CopyProfile){Write-Host 'Scope: rigs plus compatible empty-take sessions and defaults (defaults published last).'}else{Write-Host 'Scope: rigs only; destination sessions and defaults are preserved. Use -CopyProfile only for explicit profile copying.'}
if([IO.File]::Exists($journalPath)){
    if($DryRun){throw "Unfinished transaction exists at $journalPath; run without -DryRun to restore it before starting again."}
    $journal=Read-Json $journalPath
    $entries=Require-Array (Field $journal 'Entries' '$transaction') '$transaction.Entries'
    Write-Host 'Recovering unfinished transaction before migration.'
    Rollback $entries;[IO.File]::Delete($journalPath)
}
$planned=[Collections.Generic.Dictionary[string,object]]::new([StringComparer]::OrdinalIgnoreCase)
$inventory = if ([string]::IsNullOrEmpty($SourceFile)) { Get-ChildItem -LiteralPath $sourceRoot -File -Recurse -Force | Sort-Object FullName } else { Get-Item -LiteralPath $SourceFile }
foreach($file in $inventory){
    $relative=Relative-Source $file.FullName
    if($relative -match '(?i)(^|[\\/])backup[^\\/]*([\\/]|$)|\.backup|\.bak($|\.)|~$'){if($file.Extension -ieq '.rig'){Write-Host "SKIPPED backup $relative"};continue}
    if($file.Extension -ine '.rig' -and (-not $CopyProfile -or $file.Extension -ine '.jam')){if($CopyProfile -and $relative -ine 'defaults.json'){Write-Host "SKIPPED unsupported file $relative"};continue}
    if(($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){$failures.Add("$relative : reparse-point source unsupported");continue}
    try{
        Assert-NoReparseAncestor $file.FullName
        $bytes=[IO.File]::ReadAllBytes($file.FullName);$originalHash=Hash-Bytes $bytes
        $json=Read-Json $file.FullName;$target=Destination-Path $relative
        $kind='jam';$status='COPY';$semantic=$null
        if($file.Extension -ieq '.rig'){
            $kind='rig';$null=Require-Object $json '$';$version=Required-Text $json 'version' '$'
            if($version -ceq '1.0'){$json=Convert-Legacy $json;$status='CONVERT'}elseif($version -cne '2.0'){throw '$.version : unsupported rig version'}
            $before=Semantic $json;$json=Validate-Canonical $json;$semantic=Semantic $json
            if($version -ceq '1.0' -or $before -cne $semantic){$bytes=$utf8.GetBytes(($json | ConvertTo-Json -Depth 64 -Compress)+[Environment]::NewLine)}
            $null=Validate-Canonical ([RigMigrationJson]::Parse($utf8.GetString($bytes)))
            if([IO.File]::Exists($target)){
                try{$existing=Validate-Canonical (Read-Json $target);if((Semantic $existing) -ceq $semantic){$status='UNCHANGED'}}catch{Write-Host "  Existing destination requires replacement: $($_.Exception.Message)"}
            }
        }else{
            Compatible-Jam $json '$'
            if([IO.File]::Exists($target) -and (File-Hash $target) -ceq (Hash-Bytes $bytes)){$status='UNCHANGED'}
        }
        $planned.Add($relative,[pscustomobject]@{Kind=$kind;Source=$file.FullName;SourceHash=$originalHash;Target=$target;Bytes=$bytes;Status=$status;Semantic=$semantic})
        Write-Host "$status $relative"
    }catch{$message="$relative : $($_.Exception.Message)";$failures.Add($message);Write-Host "FAILED $message"}
}
if($CopyProfile){
    $defaultsPath=Join-Path $sourceRoot 'defaults.json'
    if([IO.File]::Exists($defaultsPath)){
        try{
            $bytes=[IO.File]::ReadAllBytes($defaultsPath);$originalHash=Hash-Bytes $bytes;$defaults=Require-Object (Read-Json $defaultsPath) 'defaults'
            $defaults['rig']=Remap-DefaultPath (Required-Text $defaults 'rig' 'defaults') 'rig' $planned
            $defaults['jam']=Remap-DefaultPath (Required-Text $defaults 'jam' 'defaults') 'jam' $planned
            $semantic=Semantic $defaults;$bytes=$utf8.GetBytes(($defaults | ConvertTo-Json -Depth 64 -Compress)+[Environment]::NewLine)
            $target=Destination-Path 'defaults.json';$status='MAP DEFAULTS'
            if([IO.File]::Exists($target) -and (Semantic (Read-Json $target)) -ceq $semantic){$status='UNCHANGED'}
            $planned.Add('defaults.json',[pscustomobject]@{Kind='defaults';Source=$defaultsPath;SourceHash=$originalHash;Target=$target;Bytes=$bytes;Status=$status;Semantic=$semantic});Write-Host "$status defaults.json"
        }catch{$message="defaults.json : $($_.Exception.Message)";$failures.Add($message);Write-Host "FAILED $message"}
    }else{Write-Host 'SKIPPED defaults.json: source file absent; no selection file will be published.'}
}
if($planned.Values.Where({$_.Kind -ceq 'rig'}).Count -eq 0){$failures.Add('No convertible or current rig files found.')}
if($failures.Count -gt 0){Write-Host "Migration blocked: $($failures.Count) failure(s); no destination files changed.";foreach($failure in $failures){Write-Host "  $failure"};exit 2}
$changes=@($planned.Values | Where-Object {$_.Status -cne 'UNCHANGED'} | Sort-Object @{Expression={if($_.Kind -ceq 'defaults'){1}else{0}}},Target)
if($DryRun){Write-Host "DRY RUN complete: $($changes.Count) destination change(s), $($planned.Count-$changes.Count) unchanged; source bytes retained.";exit 0}
if($changes.Count -eq 0){Write-Host 'Already equivalent: no destination files changed.';exit 0}
$transaction=[Guid]::NewGuid().ToString('N');$entries=[Collections.Generic.List[object]]::new()
$journalStage=$journalPath+'.stage-'+$transaction
try{
    foreach($change in $changes){
        Assert-Destination $change.Target
        if((File-Hash $change.Source) -cne $change.SourceHash){throw "Source changed during migration: $($change.Source)"}
        $parent=[IO.Path]::GetDirectoryName($change.Target);$null=[IO.Directory]::CreateDirectory($parent)
        $stage=$change.Target+'.migration-stage-'+$transaction;$backup=$change.Target+'.migration-backup-'+$transaction
        Assert-Destination $stage;Assert-Destination $backup
        $existed=[IO.File]::Exists($change.Target);$beforeHash=$null;if($existed){$beforeHash=File-Hash $change.Target}
        [IO.File]::WriteAllBytes($stage,$change.Bytes);$afterHash=Hash-Bytes $change.Bytes
        if((File-Hash $stage) -cne $afterHash){throw "Staged hash mismatch: $stage"}
        if($change.Kind -ceq 'rig'){$null=Validate-Canonical (Read-Json $stage)}
        $entries.Add([pscustomobject]@{Target=$change.Target;Stage=$stage;Backup=$backup;Existed=$existed;BeforeHash=$beforeHash;AfterHash=$afterHash})
    }
    $journal=[ordered]@{Transaction=$transaction;Entries=$entries.ToArray()}
    Assert-Destination $journalPath
    Assert-Destination $journalStage
    if([IO.File]::Exists($journalPath)){throw 'Another transaction appeared; refusing concurrent migration.'}
    # Publish the complete journal atomically before touching any destination original.
    # An interruption during serialization leaves only disposable stages, never a partial journal.
    [IO.File]::WriteAllText($journalStage,($journal | ConvertTo-Json -Depth 8 -Compress),$utf8)
    [IO.File]::Move($journalStage,$journalPath)
    foreach($entry in $entries){
        if($entry.Existed){
            if((File-Hash $entry.Target) -cne $entry.BeforeHash){throw "Destination changed before replacement: $($entry.Target)"}
            # A real backup path avoids Windows PowerShell binding null to an empty backup name.
            [IO.File]::Replace($entry.Stage,$entry.Target,$entry.Backup)
            if((File-Hash $entry.Backup) -cne $entry.BeforeHash){throw "Backup verification failed: $($entry.Backup)"}
            Write-Host "BACKUP $($entry.Backup)"
        }else{[IO.File]::Move($entry.Stage,$entry.Target)}
        if((File-Hash $entry.Target) -cne $entry.AfterHash){throw "Destination verification failed: $($entry.Target)"}
        Write-Host "PUBLISHED $($entry.Target)"
    }
    foreach($change in $changes){if((File-Hash $change.Source) -cne $change.SourceHash){throw "Original source changed during migration: $($change.Source)"}}
    [IO.File]::Delete($journalPath)
    Write-Host "Migration complete: $($entries.Count) published; original source SHA-256 checks verified. Unique destination backups retained."
}catch{
    $failure=$_.Exception.Message;Write-Host "Migration failed: $failure; rolling back destination transaction."
    if([IO.File]::Exists($journalStage)){[IO.File]::Delete($journalStage)}
    try{Rollback $entries.ToArray();if([IO.File]::Exists($journalPath)){[IO.File]::Delete($journalPath)};Write-Host 'Rollback complete; source originals untouched.'}
    catch{Write-Host "Rollback incomplete: $($_.Exception.Message). Preserve $journalPath and backups; rerun to recover."}
    exit 1
}
