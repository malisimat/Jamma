# Audio spectral source acquisition contract

Status: implemented and covered by native spectral/controller/publication tests. Live GPU/input/listening verification remains pending; persistence is deferred. See [current validation and handover](audio-spectral-editor-handover.md). Older validation-history paragraphs below describe the preceding session.

## Performance priority and ownership

Punch-in is performance-critical and must not be deferred for source acquisition, analysis or reconstruction. Temporary older display/playback revisions are acceptable; accepted completed content must eventually match the actual punched recording. Audio edits do not change MIDI ownership or punch scheduling.

Loop owns its recording storage, source identity/generation and bounded source copy. LoopTake retains capture/punch coordination and membership. AudioSpectralEditor, in the neutral editor directory, requests acquisition, retains the previous complete source for display, and retries after source invalidation. Scene and LoopGridEditor only provide the shared UI lifecycle and existing membership validation.

A nonzero logical length, STATE_PLAYING or LoopTake::IsCompletedRecording is not a memory-access fence. EndRecording can change state while a callback that observed its previous state is still writing. No off-audio reader is permitted to copy mutable BufferBank PCM based on state/generation checks alone.

## Exact sample layout

Let F = constants::MaxLoopFadeSamps, L = LoopLength(), P = PhysicalLoopLength(), and f = Hanning::Size(). BufferBank.Length() is F + P.

| Storage interval | Meaning | Native source preservation |
| --- | --- | --- |
| [0, F) | Captured prefix before body sample zero; load currently recreates a zero prefix | Preserve every original float |
| [F, F + L) | Logical Loop body, with source-time zero at F | Preserve every original float |
| [F + L, F + P) | Additional recorded physical tail after a rounded logical boundary | Preserve every original float |

L must be positive and no larger than P. Bounds must fit MaxLoopBufferSize; sample rate must be positive and finite. Preserve effective fade size separately. ExportSamples(false) currently returns only L body samples; ExportSamples(true) returns P samples beginning at F. Neither accessor preserves the prefix and neither is a safe source-acquisition operation while writers can resume.

The existing seam at index k, 0 <= k < f, mixes body-end sample F + L - f + k with prefix sample F - f + k. It does not read body-start sample F + k. Source storage coordinates remain distinct from play cursor, master transport, phase offsets and timing anchors. No source capture changes any of those transport values.

AudioSpectralSourceLayout::WrappedBodyIndex computes (storageIndex - F) modulo L, including negative prefix coordinates. This is a storage-to-body coordinate helper. The seam prefix receives the same wrapped source-time gain as its body-end partner, as confirmed below.

## Acquisition and invalidation protocol

1. The UI requests a copy only for eligible, completed local audio. Live remote ring buffers are explicitly excluded. Check layout and source-copy memory limits, then create a small request. The dedicated AudioSpectralWorker allocates and initializes destination PCM; neither the UI nor Scene's MIDI/serial trigger-service job thread performs that bulk allocation.
2. Publish one request through PublishedAudioSnapshot. Its layout, identity, generation and request topology remain fixed. Worker-owned destination topology becomes fixed before release-publishing Pending; the callback accesses no PCM while Preparing. Publication/collection are serialized by Loop's off-audio publication mutex, with no nested lock ordering. The callback never takes this mutex or touches shared_ptr reference counts.
3. The worker holds at most one active and one latest queued preparation. Closing cancels without joining; only editor-owner destruction joins the worker off audio. Allocation failure becomes an unavailable reason rather than a callback exception. Loop::EndMultiPlay borrows the request after that block's capture/bounce writes. This hook is called for muted Loops too and in both output/no-output callback branches. A failed bounded borrow skips that block and retries later.
4. Copy at most 4096 floats per visit, splitting at bank boundaries, on the same callback thread that owns sample writes. The request's progress and destination PCM are callback-owned. No worker/UI reads an incomplete destination. This avoids concurrent reads/writes without delaying punch or allocating a replacement recording bank.
5. A generation change, ineligible state or changed layout cancels an incomplete request. Punch entry invalidates immediately; every actual non-monitor PCM write also invalidates. Reset, logical-length changes, load, sample-rate changes and exclusive moves invalidate sources too. Generation changes are lock-free scalar operations.
6. Release-publish Complete only after every stored sample has been copied. Acquire-reading Complete permits const ownership to escape to analysis/export. After Complete, the callback never changes that source. Cancellation can race a bounded copy but cannot expose incomplete PCM; the callback lease retains storage until that copy returns.
7. The UI checks owner identity, generation, layout, eligibility and target membership before accepting source content. A changed source is reacquired after punch completes; previous immutable content can remain visible but must not authorize edits/publication against the new source. Completed original PCM can never be modified by later punch writes.
8. Closing cancels the pending request. The UI commit pump keeps collecting outstanding retirements until callback leases end, even if no further source is published. Source data and identity tokens are destroyed off audio.

Load, storage moves and owner destruction remain exclusive operations requiring stopped audio readers. Generation invalidation does not make concurrent destruction/load safe. The only discovered application Load call constructs a new Loop before publication. Sample-rate configuration already happens with the device callback stopped.

## Shared state review

| State | Writer | Reader | Synchronization and teardown |
| --- | --- | --- | --- |
| Loop source generation | Callback PCM writer and existing control/configuration paths | Callback capture, UI and later workers | Lock-free uint64 atomic; invalidated before source changes |
| Sample-rate metadata | Device configuration with callback stopped | Callback and UI source checks | Lock-free float atomic; changes invalidate generation |
| Owner identity token | Constructed with Loop; never reassigned | Captures and source validation | Immutable shared ownership off audio; callback only observes pointers |
| Capture publication and retirements | Non-audio lifecycle owners | Audio callback | PublishedAudioSnapshot borrow; off-audio publication mutex; commit collection |
| Capture destination topology | Dedicated worker only, before Pending | Callback after Pending | Release/acquire preparation state; fixed allocation during callback copy |
| Capture progress and destination samples | Audio callback only after Pending | No readers until Complete | Release/acquire terminal state; no mutation after completion |
| Latest preparation request | UI controller | Dedicated worker | Off-audio queue mutex/CV; one latest slot; joins only at owner teardown |
| Capture cancellation state | UI or callback | UI and callback | Lock-free atomic; cancelled destinations never escape |
| Editor current/previous source | UI controller | UI; later immutable worker inputs | Const shared ownership; release on UI close/replacement |

The initial copy budget is a tuning constant, not a measured deadline guarantee. Acquisition duration grows with recording length and available callback visits. Benchmark callback overhead and acquisition latency before claiming performance figures. Broader analysis/history/playback memory budgeting belongs to subsequent plan steps.

## Confirmed seam-envelope rule

The user confirmed that silencing the end crossfade band must also silence the captured prefix partner, so it cannot leak into the Hanning mix. Each contributor uses its own storage-to-body mapping; because the two raw indices differ by L, they receive the same wrapped body-time envelope value. Prefix context is not reassigned to beginning envelope time k.

This preserves existing sample layout and seam timing. Applying gain per contributor before the mix makes the contract explicit; an equivalent common scalar can mathematically factor through the linear mix. The original plan's claim that a post-mix scalar would necessarily misalign these particular contributors was incorrect.

## Validation performed

Read-only tracing of Loop/LoopTake/Station/AudioHost, source capture ownership review, project/filter registration inspection and git diff whitespace checks. The threading-review script is file-wide: off-audio publication mutex additions are expected to be flagged; manually verify those methods are not reachable from callback copy, ReadBlock, WriteBlock, EndMultiPlay, OnBlockWrite or punch paths. No native fixtures/tests added, no native tests or builds run. DSP, processed playback, rendering, gestures/Undo and persistence are not yet implemented.

## Live resource and overview handoffs

The editor owns one explicit 512 MiB session resource budget. Before source allocation, Loop reserves the conservative complete working-memory estimate; the captured immutable source pins that reservation through jobs, playback and history. Admissions fail with a useful reason while preserving existing edits. Reservations include six full gain revisions, dense envelope scratch/retention, processed playback handles, display and history allowances. They remain conservative full reservations while a source is retained, including after a live draft is cancelled back to unity.

Playback snapshot preparation decimates composed immutable PCM into a fixed 2048-segment overview off audio. Both the open editor and Loop's UI commit pump consume it; the commit pump also handles final results after close and Undo of closed Loops. Snapshot identity and radius key the waveform cache. This path keeps the acquired snapshot and never falls back to mutable recording-bank reads if the source changes while refreshing.
