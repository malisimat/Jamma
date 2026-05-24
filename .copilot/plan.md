# Feature: serial-input

## Summary
Support trigger input from serial device (e.g. COM3).  Allow rig to specify serial port and baud rate, and support this as trigger input (to trigger loop recording/ending) in addition to keyboard. This is so I can use my arduino-based pedalboard to control the app

## Goals & Acceptance Criteria
Successfully connects to a serial device and triggers respond. Rig files parsed correctly when specifying serial config.

## Scope

### In scope
Rig file JSON and parsing, trigger type (extending to serial, keeping existing keyboard input), up to as many pedals as supported by format/binary protocol

### Out of scope
saving, recording of serial input (we are only using it to trigger, not as a performance source)

## Proposed Approach
The arduino sketch is @C:\Users\matto\Source\Repos\Arduino\PedalBoard\PedalBoard.ino - use that to determine the binary protocal / format. Assume connected to COM3.  Add tests which do not rely on the presence of an actual device.

## Risks & Open Questions
Risk of code bloat - keep updates short and surgical.

## TODOs
- [x] Added rig-level serial config parsing (`user.serial`) with port, baud rate, and enable flag
- [x] Added per-trigger-pair source parsing so keyboard and serial bindings can coexist
- [x] Added serial packet decoder tests for the Arduino pedalboard protocol (`0xA5`, button index, pressed flag)
- [x] Added a serial device reader and scene ingress path that dispatches serial trigger events without touching the audio callback
- [x] Added/updated unit tests for serial parsing and source-aware trigger dispatch
- [x] Validated with targeted serial tests, the full native test suite, and a Debug x64 app build
