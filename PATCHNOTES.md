# OLAS — patch notes and research notes

Changelog and the measurements behind the version 1.1 decisions. For what the
program is and how to use it, see [README.md](README.md).

---

## 1.1

### Threading: let ONNX Runtime manage its own pool

`MOONSHINE_ORT_SINGLE_THREAD` is a **boolean, not a thread count**. Verified by
reading `/proc/<pid>/task` while running:

| value set | ORT worker threads |
| --------- | ------------------ |
| `1` | 1 |
| `2` | 1 |
| `4` | 1 |
| `8` | 1 |
| unset | 26 |

Any value forces exactly one thread; it cannot be used to request N threads.
The only real choice is *set* or *unset*, and the pool must then be bounded
with CPU affinity.

Measured on real speech (Medium Streaming, 44 s, three runs each, variance
under 2%):

| mode | RTF |
| ---- | --- |
| variable set (1 thread) | 0.733 / 0.724 / 0.718 → **0.725** |
| variable unset | 0.467 / 0.462 / 0.449 → **0.459** |

RTF is `decode_wall / audio_duration`; below 1.0 keeps up with a live
microphone. Unsetting the variable is **~37% faster**, so 1.1 leaves it unset
by default.

Affinity bounds the cost. Measured with the variable unset:

| cores granted | RTF |
| ------------- | --- |
| 2 | ~0.52 |
| **4** | **~0.50** |
| 6 | ~0.57 |

Four cores is the optimum; six is worse (oversubscription).

> An earlier note in this project concluded from the same runs that "1 thread
> is fastest and 2/4/8 are equal or slower". That was wrong: every one of those
> runs was single-threaded, so nothing was being compared.

### Per-model core split

The two models get separate core sets so their ORT pools do not contend.
Measured with Medium+Small over one shared 4-core mask, the slowest model sat
at RTF ~0.95 — close enough to 1.0 to be fragile. Split, it drops to ~0.81.

The split is applied at session creation, because that is when ONNX Runtime
sizes its pool, and the process widens back afterwards so capture and UI are
not confined to one model's cores.

Dual Small, 1+1 against 2+2, 44 s of speech, three runs each:

| split | mean RTF | worst |
| ----- | -------- | ----- |
| **1 + 1** (Potato) | **0.566** | **0.597** |
| 2 + 2 | 0.574 | 0.650 |

1+1 is marginally faster and noticeably steadier (the 2+2 pairs diverge, one
instance ~0.50 and the other ~0.65), for half the CPU. Hence Potato mode.

### The audio queue is now lossless and non-blocking

Two properties, pulling opposite ways:

- **Never block.** The original design blocked the producer once the queue
  filled. Because the capture thread pushes to every pane in turn, one slow
  model froze the capture thread and starved *both* panes; the backlog then
  grew without bound and the transcript drifted permanently behind.
- **Never discard.** Dropping the oldest chunk keeps a caption live but deletes
  speech, which is not acceptable here.

The 1.1 queue therefore: never blocks, never drops, warns once when the backlog
passes 10 s, and keeps a 60 s hard ceiling purely as a memory guard (any
discard is logged loudly and counted in `--stats`).

> **Known limitation:** a multi-minute lag could not be reproduced on the
> development machine (180 s of real speech held at 3.7–10 s of lag with zero
> loss). The threading change is believed to address the mechanism — a
> single-threaded Medium model at RTF ~0.72 is one competing process away from
> falling behind permanently — but this has not been observed directly.

### `transcription_interval` 0.5 → 1.0

The main CPU dial, and it does **not** change the transcript. Measured on real
speech with Medium, 44 s of audio: 69 s of wall clock at 0.5, 58 s at 2.0, for
identical output (13 lines, ~595 characters). Higher costs less CPU and updates
the screen less often.

### Model choice and modes

English ships two models, so the app has two modes:

| mode | English | Spanish | cores |
| ---- | ------- | ------- | ----- |
| Normal (default) | Medium Streaming | Small Streaming | 3 + 1 |
| Potato | Small Streaming | Small Streaming | 1 + 1 |

Both models are fetched by the build script so either mode works offline.

### Resource plan at startup

The detected core count decides the thread budget and the affinity mask, rather
than a fixed value. `--diagnose` prints the hardware probe and the resulting
plan without starting GTK or the audio server, so it works over SSH.

### GPU support removed

- `libmoonshine.so` parses only `cpu`, `coreml`, `nnapi`. Requesting `cuda`,
  `rocm`, `openvino`, `dml` or `tensorrt` is accepted at construction and then
  **silently ignored**.
- Tested through the real inference path on a host with no `/dev/dri`: every
  provider name produced **identical output and no error** — none of them ran
  on a GPU.
- The bundled `libonnxruntime.so.1` does contain CUDA/ROCm/OpenVINO/DML
  providers, but Moonshine's layer never requests them.
- Moonshine's own documentation agrees: *"Unset means CPU-only
  (recommended)."*

The GPU detection code was removed rather than left in place doing nothing.

### Context dictionaries removed

Earlier builds passed a domain term list to Moonshine as `keyterms` /
`context`. Removed after measurements on real Spanish speech (bad lines —
near-miss words and phantom fragments — across 3 runs of one clip):

| terms supplied | bad lines |
| -------------- | --------- |
| none | 0 |
| ~20 | 0 |
| ~50 | 3 |
| ~100 | 3 |
| ~200 | 3 |

Moonshine's documentation warns that large term lists produce phantom words,
and the measurements agreed. The usable window is around 20 terms, which was
too narrow to justify the feature as built. The `contexts/` directory and its
sample dictionaries were deleted so it can be redesigned cleanly; anything
rebuilt must stay inside that ~20-term budget.

### Benchmarking pitfalls hit along the way

Recorded so they are not repeated:

- **Synthetic tones prove nothing.** A tone-based benchmark reported RTF 0.17;
  it was also producing `lines=0`. The model decoded nothing. Always check that
  a benchmark actually produced a transcript.
- **Warm-up matters.** A cold ONNX Runtime session reported RTF 1.19 for a model
  that runs at 0.73 warm. Always discard the first pass.
- **Check the thread count, don't infer it.** The "8 threads is slower"
  conclusion came from a variable that ignores 8.

### Readability — unsolved

Each pane hears all audio, so the pane for the language nobody is speaking
still emits text. Two approaches were tried and both dropped:

- **Auto-muting the quiet pane.** Rejected before implementation: muting risks
  hiding real text, and the point of this program is not losing any.
- **Dimming the pane that is not producing.** Built, tested, and removed. The
  signal it relied on — that wrong-language output is short fragments with
  little text per line — does not hold. Spanish transcribing English produces
  normal-length, plausible-looking words ("la Clem", "Texas or Rollery"), so
  both panes look equally healthy to any measure of text *shape*.

Conclusion: **text statistics cannot separate a good transcript from a
fluent-looking bad one.** A real solution needs the language of the audio to be
known, which means language identification — a model in the middle of the
pipeline and the compute cost that comes with it. That trade-off has not been
made yet.

---

## 1.0

- Corrected the transcription parameters: `vad_threshold` 0.55 → 0.5,
  `vad_max_segment_duration` 6 → 12, `transcription_interval` 0.35 → 0.5.
- Removed the domain dictionary (see above).
- Configuration moved into `olas-1.0.conf`.
