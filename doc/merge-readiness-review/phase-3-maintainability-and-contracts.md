# Phase 3 — Maintainability, Tests, Documentation, and Product Contracts

## Goal and human gate

Reduce complexity without losing the branch’s consolidated timing model, and make tests and documentation truthful. Human gate: approve each deletion, simplification, test rewrite/removal, and documentation correction before it is implemented.

## Outputs

- Maintain a ranked simplification map: code size, coupling, behavioural risk, and protected concepts retained.
- Add test-quality, docs/comments, compatibility, observability, and security/robustness findings to `findings.md`.
- Record the minimum regression suite and manual acceptance cases in `verification-matrix.md`.

## Independent review stages

13. **Simplification and code size** (`stage-reports/13-simplification.md`). **Primary ownership:** live abstractions, adapters, indirections, duplication, and conditionals that can be removed or merged while keeping all protected timing distinctions. Consume stage 6's dead-code list and do not re-report it. **Required handoff:** ranked proposals with estimated deletion, coupling reduction, behavioural risk, dependencies, and an equivalence argument.
14. **Unit-test quality** (`stage-reports/14-tests.md`). **Primary ownership:** whether new/changed native tests express meaningful deterministic contracts with realistic boundaries, strong assertions, and useful failure diagnosis; identify missing regression contracts as well as low-value duplication. **Excludes:** judging production implementation except where a test cannot observe its contract. **Required handoff:** test-to-contract map and proposed minimum regression suite.
15. **Docs and comments** (`stage-reports/15-docs.md`). **Primary ownership:** truthfulness and glossary consistency of changed docs, comments, UI text, log text, and test descriptions. **Excludes:** runtime log cost/placement (stages 8/17) and naming production symbols (stage 4). **Required handoff:** statement-level correction table tied to current behaviour or an approved decision.
16. **Compatibility and persistence** (`stage-reports/16-compatibility.md`). **Primary ownership:** `.jam`, rig/config, JSON, VST state, MIDI mappings, resource paths, project/build metadata, defaults, versions, migration, and fallback. **Excludes:** hostile input handling (stage 18). **Required handoff:** compatibility matrix by artifact version/producer/consumer and explicit intentional breaks.
17. **Observability and diagnosability** (`stage-reports/17-observability.md`). **Primary ownership:** whether assertions, state exposure, errors, and non-hot-path logs can diagnose failures with appropriate identifiers, levels, and volume. Consume stage 8's hot-path log findings and do not duplicate them. **Required handoff:** symptom-to-signal map plus stale-debug cleanup candidates.
18. **Security and input robustness** (`stage-reports/18-input-robustness.md`). **Primary ownership:** trust-boundary validation for changed parsing, loading, network/MIDI/VST input, paths, lengths, and serialization, including malformed data, bounds, denial of service, and sensitive logging. **Excludes:** backward compatibility for well-formed data (stage 16). **Required handoff:** input-boundary table with validation, failure behavior, and resource limits. This is not a broad security certification.

## Human review packet

The integrator writes `phase-packets/phase-3.md`, reconciling simplifications with Phase 1 boundaries and Phase 2 correctness invariants. It presents the top simplifications by deleted-line potential and risk, contract/documentation corrections, compatibility and robustness risks, coverage gaps, and the proposed lean regression suite. The human reviewer decides dispositions; batching remains Phase 4 work so separate findings are not prematurely coupled.
