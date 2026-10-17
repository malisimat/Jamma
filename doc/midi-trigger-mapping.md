# Multi-Device MIDI Trigger Rig Configuration

Rig files support multiple configured MIDI input devices. MIDI activation and ditch bindings select their own devices, while MIDI loop recording can subscribe to one or more devices independently.

## Example Rig JSON

```json
{
  "version": "2.0",
  "name": "rig",
  "devices": {
    "audio": { "name": "HDMI", "samplerate": 44100, "bufsize": 255, "inlatency": 414, "outlatency": 414, "numbuffers": 4, "numchannelsin": 0, "numchannelsout": 10 },
    "midi": [
      { "id": "trigger-pad", "name": "TriggerPad", "enabled": true },
      { "id": "keys-a", "name": "Keys A", "enabled": true },
      { "id": "keys-b", "name": "Keys B", "enabled": true }
    ],
    "serial": []
  },
  "settings": {
    "loop": { "fadeSamps": 800, "seedGrainMinMs": 400, "seedGrainTargetMaxMs": 3000, "seedBpmMin": 80, "seedQuantisation": "power" },
    "trigger": { "debounceMs": 280 },
    "midi": { "channelOverrideTriggers": false, "channelOverrideLive": true }
  },
  "triggers": [
    {
      "id": "trigger-1",
      "name": "Trig1",
      "target": "Keys",
      "controls": {
        "activate": [
          { "source": "midi", "device": "trigger-pad", "button": { "kind": "note", "channel": 1, "id": 60 } }
        ],
        "ditch": [
          { "source": "midi", "device": "trigger-pad", "button": { "kind": "cc", "channel": 1, "id": 64 } }
        ]
      },
      "capture": { "audio": [], "midi": ["keys-a", "keys-b"] }
    }
  ]
}
```

## Behavior and Configuration Reference

- **`devices.midi`**: The supported registry of MIDI device IDs, hardware names and enabled states. The application accepts only canonical rig version `2.0`.
- **Enabled Devices**: Each enabled device gets its own MIDI callback endpoint and ingress queue.
- **`controls.activate[].device` / `controls.ditch[].device`**: Select the registry ID of the input device for each binding.
- **MIDI Channel Selection**: Recording channels follow each station's active MIDI channel toggles (`allowedmidichannels` in jam state).
- **`capture.midi`**: Selects the MIDI registry IDs recorded into MIDI loops. An empty list captures no live MIDI; the field must be present.
- **Device Separation**: The same MIDI channel from different devices is recorded into distinct MIDI loop streams when both devices are listed.
- **`channel`**: Values in MIDI binding edges or button shorthand are one-based. Set `channel` to `null` to match any channel; the field must be present.
- **Button `kind`**: Supports `note`, `noteoff`, and `cc` shorthand; saved bindings use explicit `press` / `release` edges with `note` or `cc` kinds and state `1` / `0`.
  - `note` uses Note On as press and Note Off (or Note On with velocity 0) as release.
  - `noteoff` triggers on Note Off and releases on Note On—the inverse of `note`.
  - `cc` uses CC values greater than 0 as press and `0` as release.
  - Velocity is ignored for trigger purposes.
- **Matching**: Hardware names are matched exactly against MIDI input names; `default` selects the first unclaimed input. Startup logs report unavailable devices while retaining their configured selections.
- **Overlap**: Controls and capture may use the same registry ID. Shared bindings may reach multiple triggers; within one trigger, the earliest defined binding wins and a later conflicting binding is discarded with a warning.

## Trigger predelay

Each trigger may specify signed fractional `preDelayMs` in `[-200, 200]`;
missing values mean zero. Positive values put content zero earlier than the
admitted trigger edge, so a coincident onset is that distance into the loop.
Negative values wrap that onset into the loop tail. The active audio stream
sample rate determines the rounded sample count, which is frozen at START and
retained through STOP. Trigger-to-trigger duration and live MIDI timestamps do
not change. Keyboard and serial edges without a coherent clock anchor use their
admitting callback boundary; their unknown physical press time is not sample exact.

A fixed 200 ms history reserve supplies earlier input independently of each
trigger's setting. It is not a rig field or the default content adjustment.
The final loop must be longer than the absolute adjustment and its selected
history window; too-short captures fail rather than silently clamp timing.
A positive adjustment admits up to 10 ms of additional earlier seam history,
bounded by the same 200 ms reserve. This guard changes source admission, not
content position or playback phase. Timing edits replace idle trigger instances
through the existing rig transition; already active takes keep frozen timing.

Legacy `settings.trigger.preDelaySamps: 0` is accepted with a deprecation warning
and omitted on save. Nonzero legacy values require explicit per-trigger
`preDelayMs`. Mixing that deprecated global field with per-trigger declarations
is rejected. MIDI device timestamp-offset configuration is unsupported.
