# Feature: fix-scene-reset

## Summary
Address issue #97 and ensure the scene is reset after the last loop is ditched, because that flow is currently not working properly.

## Goals & Acceptance Criteria
evidence from TDD - a test was able to be written that failed, demonstrating we can reproduce the issue(s).  Then evidence of this test passing after surgical fix(es).

## Scope

### In scope
reproducing variety of scene resets in tests (ditching whilst recording, ditch trigger in debounce mode, ditch in overdub) and reliably fixing scene reset code

### Out of scope
focus only on fixing test

## Proposed Approach
Check https://github.com/malisimat/Jamma/issues/97 and ensure issue is reproducible before implementing fix.  Investigate variety of possible root causes

## Risks & Open Questions
risk of breaking existing functionality

## TODOs
- [x] Reproduce root causes via failing tests (TDD red phase)
  - Root cause 1: `Scene::_DispatchMidiTriggerEvent` never called `Reset()` on ditch-to-zero
  - Root cause 2: `Scene::_PumpSerial` discarded `OnTriggerEvent` return value, never called `Reset()` on ditch-to-zero
- [x] Added `IsSceneResetForTest()` accessor to `TestScene` in `Trigger_Tests.cpp`
- [x] Added 5 `SceneReset.*` tests covering key (direct, debounced, overdub), MIDI, and serial trigger paths
- [x] Fixed `Scene::_DispatchMidiTriggerEvent` — on `ACTIONRESULT_DITCH`, counts total takes across all stations; if 0, calls `Reset()` immediately
- [x] Fixed `Scene::_PumpSerial` — captures `OnTriggerEvent` return value; on `ACTIONRESULT_DITCH`, counts total takes; if 0, calls `Reset()` immediately
- [x] All 5 `SceneReset.*` tests pass; full suite: 433/434 passed (1 pre-existing skip), 0 regressions
