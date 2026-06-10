# Multi-Device MIDI Trigger Rig Configuration

Rig files support multiple configured MIDI input devices. MIDI trigger activation uses one device per trigger, while MIDI loop recording can subscribe to one or more devices independently.

## Example Rig JSON

```json
{
  "name": "rig",
  "user": {
    "audio": { "name": "HDMI", "bufsize": 255, "inlatency": 414, "outlatency": 414, "numchannelsin": 0, "numchannelsout": 10 },
    "midi": {
      "devices": [
        { "name": "TriggerPad", "enabled": true },
        { "name": "Keys A", "enabled": true },
        { "name": "Keys B", "enabled": true }
      ]
    }
  },
  "triggers": [
    {
      "name": "Trig1",
      "stationtype": 0,
      "midiinput": [1],
      "midiinputdevices": ["Keys A", "Keys B"],
      "trigger": {
        "type": "midi",
        "device": "TriggerPad",
        "activate": { "kind": "note", "channel": 1, "id": 60 },
        "ditch": { "kind": "cc", "channel": 1, "id": 64 }
      }
    }
  ]
}
```

## Behavior and Configuration Reference

- **`user.midi.devices`**: The supported list of MIDI device names and their enabled state. The old single-device `user.midi.name` / `enabled` shape is rejected.
- **Enabled Devices**: Each enabled device gets its own MIDI callback endpoint and ingress queue.
- **`trigger.device`**: Selects the input device that can activate or ditch that station trigger.
- **`midiinput`**: Selects the one-based MIDI channels recorded into MIDI loops.
- **`midiinputdevices`**: Selects the MIDI input devices recorded into MIDI loops. If omitted, MIDI loop recording keeps the legacy channel-only behavior.
- **Device Separation**: The same MIDI channel from different devices is recorded into distinct MIDI loop streams when both devices are listed.
- **`channel`**: Values in trigger bindings are one-based. Omitting `channel` makes the binding match any channel.
- **Trigger `kind`**: Supports `note`, `noteoff`, and `cc`.
  - `note` (aliases: `noteon`, `note-on`, `note on`) uses Note On as press and Note Off (or Note On with velocity 0) as release.
  - `noteoff` (aliases: `note-off`, `note off`) triggers on Note Off and releases on Note On—the inverse of `note`.
  - `cc` uses CC values greater than 0 as press and `0` as release.
  - Velocity is ignored for trigger purposes.
- **Matching**: Device names are matched exactly against configured active MIDI input names. Startup logs report unresolved trigger devices and unresolved loop-record devices.
- **Overlap**: `device` may match one of the names in `user.midi.devices` if the same controller should both play/record MIDI loops and trigger recording.
