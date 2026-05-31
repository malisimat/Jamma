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
- [ ] (to be filled by implementing agent)
