# NINJAM integration task log

## Milestones

- [x] Added build wiring for NINJAM include/lib integration.
- [x] Added `io::NinjamConnection` wrapper around `NJClient`.
- [x] Added `engine::StationRemote` and `engine::LoopRemote`.
- [x] Integrated NINJAM pump/snapshot/audio flow into `engine::Scene`.
- [x] Added native tests for `StationRemote`/`LoopRemote`.

## Notes

- NINJAM connection is configured through the jam file `ninjam` block:
  - `host`
  - `user`
  - `pass`
  - `workdir` (optional)
- The default jam now seeds `host` with `ninjam.com:2049`.
- One remote user is mapped to one `StationRemote`, with one assigned stereo output pair in NINJAM.
