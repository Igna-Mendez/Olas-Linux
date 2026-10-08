# OLAS — Open Linux Audio Scribe (GTK4) — version 1.1

Real-time local speech-to-text via Moonshine Voice. Two language panes side by
side, one model each, running entirely on your machine. No cloud, no telemetry,
no network at runtime.

Defaults to **Medium Streaming English** and **Small Streaming Spanish**. The
build ships all three models (Medium English, Small English, Small Spanish)
and the launcher selects between them.

**Accuracy is the design goal.** Where a trade-off exists between staying
current and keeping every word, this program keeps the word.

---

## Research and development notes

Everything below was measured on this project, not assumed. Where an earlier
conclusion was wrong, it is recorded as wrong — the corrections matter more
than the original guess.

### 1. `MOONSHINE_ORT_SINGLE_THREAD` is a boolean, not a thread count

The obvious-looking knob does not do what its name suggests. Verified by
reading `/proc/<pid>/task` while running:

| value set | ORT worker threads |
| --------- | ------------------ |
| `1` | 1 |
| `2` | 1 |
| `4` | 1 |
| `8` | 1 |
| unset | 26 |

Any value forces exactly one thread. It **cannot** be used to request N
threads. The only real choice is *set* (one thread) or *unset* (ONNX Runtime
sizes its own pool), and the pool must then be bounded with CPU affinity.

*Earlier in this project it was concluded from these same runs that "1 thread
is fastest and 2/4/8 are equal or slower". That was wrong: every one of those
runs was single-threaded, so nothing was being compared.*

### 2. Multi-threaded inference is meaningfully faster

Medium Streaming, real speech (44 s), three runs each, variance under 2%:

| mode | RTF |
| ---- | --- |
| `MOONSHINE_ORT_SINGLE_THREAD=1` | 0.733 / 0.724 / 0.718 → **0.725** |
| variable unset | 0.467 / 0.462 / 0.449 → **0.459** |

RTF is `decode_wall / audio_duration`; below 1.0 keeps up with a live
microphone. Unsetting the variable is **~37% faster**.

Affinity then bounds the cost. Measured with the variable unset:

| cores granted | RTF |
| ------------- | --- |
| 2 | ~0.52 |
| **4** | **~0.50** |
| 6 | ~0.57 |

Four cores is the optimum; six is worse (oversubscription).

### 3. There is no usable GPU path

- `libmoonshine.so` parses only `cpu`, `coreml`, `nnapi`. Requesting `cuda`,
  `rocm`, `openvino`, `dml` or `tensorrt` is accepted at construction and then
  **silently ignored**.
- Tested through the real inference path on a host with no `/dev/dri`: every
  provider name produced **identical output and no error** — none of them ran
  on a GPU.
- The bundled `libonnxruntime.so.1` does contain CUDA/ROCm/OpenVINO/DML
  providers, but Moonshine's layer never requests them.

GPU support has been **removed** from the code. `--diagnose` still reports the
detected GPU so it is clear what the machine has.

### 4. The audio queue must be lossless and must never block

Two properties, pulling opposite ways:

- **Never block.** The original design blocked the producer once the queue
  filled. Because the capture thread pushes to every pane in turn, one slow
  model froze the capture thread and starved *both* panes; the backlog then
  grew without bound and the transcript drifted permanently behind.
- **Never discard.** The Windows port drops the oldest chunk to stay live.
  That is right for live captions and wrong here, because it deletes speech.

The 1.1 queue therefore: never blocks, never drops, warns once when the
backlog passes 10 s, and keeps a 60 s hard ceiling purely as a memory guard
(any discard is logged loudly and counted in `--stats`).

**Known limitation:** a multi-minute lag could not be reproduced on the
development machine (180 s of real speech held at 3.7–10 s of lag with zero
loss). The threading fix above is believed to address the mechanism — a
single-threaded Medium model at RTF ~0.72 is one competing process away from
falling behind permanently — but this has not been observed directly.

### 5. `transcription_interval` is the main CPU dial, and it is free

It does **not** change the transcript. Measured on real speech with Medium,
44 s of audio: 69 s of wall clock at 0.5, 58 s at 2.0, for identical output
(13 lines, ~595 characters). Higher costs less CPU and updates the screen less
often. Default is 1.0 — roughly a third less work than 0.5 while still
refreshing twice a second.

### 6. Benchmarking pitfalls hit along the way

Recorded so they are not repeated:

- **Synthetic tones prove nothing.** A tone-based benchmark reported RTF 0.17;
  it was also producing `lines=0`. The model decoded nothing. Always check that
  a benchmark actually produced a transcript.
- **Warm-up matters.** A cold ONNX Runtime session reported RTF 1.19 for a
  model that runs at 0.73 warm. Always discard the first pass.
- **Check the thread count, don't infer it.** The "8 threads is slower"
  conclusion came from a variable that ignores 8.

### 7. Context dictionaries — removed, pending redesign

The earlier builds passed a domain term list to Moonshine as `keyterms` /
`context`. That was removed after measurements on real Spanish speech (bad
lines — near-miss words and phantom fragments — across 3 runs of one clip):

| terms supplied | bad lines |
| -------------- | --------- |
| none | 0 |
| ~20 | 0 |
| ~50 | 3 |
| ~100 | 3 |
| ~200 | 3 |

Moonshine's documentation warns that large term lists produce phantom words,
and the measurements agreed. The usable window is around 20 terms, which was
too narrow to justify the feature as it was built.

The `contexts/` directory and its sample dictionaries were deleted so the
feature can be redesigned cleanly. Anything rebuilt here must stay inside that
~20-term budget, or the accuracy loss outweighs the gain.

---

## What 1.1 changes from 1.0

- **Threading:** ONNX Runtime manages its own pool, bounded by CPU affinity
  (4 cores, or 2 on a small machine). ~37% faster than the previous
  single-threaded arrangement.
- **Queue:** lossless and non-blocking. See section 4.
- **Model default:** Medium Streaming English + Small Streaming Spanish.
- **Multi-model:** all three models ship with the build, and the launcher
  selects which to use without re-downloading.
- **`--diagnose`:** prints the detected hardware and the derived plan without
  starting GTK or the audio server, so it works over SSH.
- **Resource plan:** derived from the detected core count at startup.

---

## Quick start

```sh
./build-gtk.sh                 # build (also fetches models on first run)
./gtk-launcher.sh              # run
```

`build-gtk.sh` downloads the Medium Streaming English model, the Small
Streaming Spanish model, and the Small Streaming English model, so every
configuration is available offline.

## Choosing a model

The default is **Medium Streaming for English** (the most complete English
model) and **Small Streaming for Spanish** (there is no Medium Spanish model).
The resource planner sizes the thread budget so the pair runs in real time.

Either can be changed at launch, without re-downloading anything:

```sh
OLAS_MOONSHINE_ARCHS=5,4 ./gtk-launcher.sh    # en=Medium, es=Small (the default)
OLAS_MOONSHINE_ARCHS=4,4 ./gtk-launcher.sh    # both Small (lowest CPU)
OLAS_MOONSHINE_LANGS=en ./gtk-launcher.sh     # English only
```

Architecture numbers: `0`=Tiny, `1`=Base, `2`=Tiny Streaming,
`4`=Small Streaming, `5`=Medium Streaming. (`3`=Base Streaming is declared in
Moonshine's API but unsupported and rejected.)

## Command line

```
-m, --model PATH[,PATH]      Model directory per language
-s, --source NAME            Pulse/PipeWire monitor source [auto]
-a, --arch N[,N]             Architecture per language
-l, --language CODE[,CODE]   Language codes [default: en,es]
-r, --rms THRESHOLD          Silence RMS threshold
-q, --chunk-ms MS            Capture chunk in ms [default: 50]
-c, --config PATH            Parameters file [default: olas-1.1.conf]
-v, --verbose                Write per-line latency to olas-debug.log
    --diagnose               Print hardware probe and plan, then exit
    --stats                  Print diagnostics on exit
-h, --help                   Show help
```

## Readability: pane colouring

Both panes hear all audio, so the pane for the language nobody is speaking
emits either almost nothing or a scatter of short fragments. Rather than mute
it —
muting risks hiding real text if the guess is wrong — a pane that has gone
quiet while another is producing is shown **dimmed**: same text, same position,
still selectable, just a lighter colour.

Each pane also gets its own accent colour (blue for the first, amber for the
second) on the body text and the language label, so the two transcripts are
told apart at a glance even when one is dimmed.

The judgement is deliberately cheap — two integers per pane updated per
completed line, no text analysis and no extra model:

* A pane counts as producing when its recent lines average at least 8
  characters. Fragments fall well below that; real sentences are far above.
* Nothing is dimmed until at least one pane is clearly producing.
* The counters halve every ~12 lines, so a change of speaker flips the
  emphasis back within a few seconds.

### Dual Small: 1+1 measured against 2+2

Two Small models run concurrently, 44 s of speech, three runs each:

| split | RTF per run | mean | worst |
| ----- | ----------- | ---- | ----- |
| **1 + 1** | 0.539/0.597, 0.556/0.578, 0.554/0.571 | **0.566** | **0.597** |
| 2 + 2 | 0.503/0.648, 0.500/0.645, 0.497/0.650 | 0.574 | 0.650 |

1+1 is marginally faster on average and noticeably steadier (the 2+2 pairs
diverge, one instance ~0.50 and the other ~0.65, because the two ORT pools
contend). Both keep up easily, so 1+1 wins on using half the CPU for the same
or better result.

## Resource tuning

### Per-model core split

The two models do not share one thread pool. Each transcriber is built while
its thread is confined to its own cores, so the ONNX Runtime pools cannot
contend with each other. Measured with Medium+Small over one shared 4-core
mask, the slowest model sat at RTF ~0.95; split 3+1 it drops to ~0.81.

The rule depends on what is loaded:

| models | split | why |
| ------ | ----- | --- |
| Medium + Small (default) | **3 + 1** | the extra core is worth more to the slower model |
| Small + Small | **1 + 1** | Small runs well single-threaded, so the pair stays genuinely light — 2 cores for the whole process |
| one model | 4 | nothing to share with |

After both transcribers are built the process widens back to the whole budget,
so capture, GTK and the audio callback are not confined to one model's cores.

### Overrides

```sh
OLAS_THREADS=1      ./gtk-launcher.sh   # force single-threaded (lowest CPU)
OLAS_CPU_CORES=2    ./gtk-launcher.sh   # shrink the total budget
OLAS_CPU_CORES=0    ./gtk-launcher.sh   # disable pinning entirely
```

`--diagnose` prints the detected hardware and the chosen split without starting
the UI. `--stats` on exit reports real-time factor, maximum backlog and any
dropped chunks per pane, which is the fastest way to tell whether a machine is
keeping up.

## Building the benchmarks

The measurement tools live in `tools/`:

```sh
# needs the Moonshine SDK (run build-gtk.sh first)
g++ tools/thread_bench.cpp    -O2 -std=c++17 -Imoonshine-voice/include \
    -Lmoonshine-voice/lib -lmoonshine -Wl,-rpath,"$PWD/moonshine-voice/lib" \
    -lpthread -o thread_bench

./thread_bench <model_dir> <arch> <threads> <file.wav>
./pipeline_bench <model_dir> <arch> <threads> <file.wav> <queue-mode>
```

Use **real speech** (16 kHz mono WAV), never synthetic tones.

## Output

- `olas-moonshine-notes.txt` — live transcript, overwritten each session
- `olas-debug.log` — per-line latency, with `-v`

## Requirements

GTK4 and libpulse development headers:

- Arch / CachyOS: `sudo pacman -S gtk4 pkgconf libpulse`
- Debian / Ubuntu: `sudo apt install libgtk-4-dev pkg-config libpulse-dev`
- Fedora: `sudo dnf install gtk4-devel pkgconf-pkg-config libpulse-devel`

## License

MIT. Moonshine is MIT; see `license.txt`.
