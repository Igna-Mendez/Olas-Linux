# OLAS — Open Linux Audio Scribe

Real-time local speech-to-text for Linux. Two language panes side by side, one
model each, running entirely on your machine. No cloud, no telemetry, no
account, no API keys, no network at runtime.

Built for meetings, interviews, lectures and interpretation — the situations
where a transcript needs to keep up with people talking, and where the audio
should never leave the machine.

**Accuracy is the design goal.** Where a trade-off exists between staying
current and keeping every word, this program keeps the word.

> **Note on how this was built.** OLAS was heavily vibe-coded: roughly 95% of
> the code was written by different agentic AI models, with a human directing
> the design, testing on real hardware and deciding what shipped. The
> measurements in [PATCHNOTES.md](PATCHNOTES.md) exist because the AI-written
> parts got things confidently wrong more than once, and only measurement
> caught it. Treat the code accordingly: it works, but it has not had a
> conventional human review.

---

## What it does

- **Two languages at once** — English and Spanish, each in its own pane with
  independent Start/Stop, collapse and detach-to-window controls
- **Two modes** — Normal (more accurate) and Potato (ultralight), chosen on
  first run and changeable from Options
- **Focus mode** — hides the toolbar and pane headers so the transcript fills
  the window
- **Live transcript file** — written line by line to `olas-moonshine-notes.txt`
- **Options toolbar** — capture device, zoom, timestamps, light/dark theme,
  auto-scroll
- **Diagnostics** — `-v` writes per-line latency to `olas-debug.log`,
  `--diagnose` prints the detected hardware and the inferred resource plan

## Modes

Which mode runs is decided by the **English** model. Spanish is always Small
Streaming: no Medium Spanish model exists.

| mode | English | Spanish | cores | character |
| ---- | ------- | ------- | ----- | --------- |
| **Normal** (default) | Medium Streaming | Small Streaming | 3 + 1 | more accurate |
| **Potato** | Small Streaming | Small Streaming | 1 + 1 | ultralight, lower CPU, slightly less accurate |

Normal is the default. Potato mode is for a busy machine or a weak laptop: one
core per language, two in total.

```sh
./gtk-launcher.sh                   # normal mode
OLAS_MODE=potato ./gtk-launcher.sh  # potato mode
```

## Quick start

```sh
./build-gtk.sh        # builds, and fetches the SDK and all three models
./gtk-launcher.sh     # run
```

The first build takes a few minutes: it creates a Python venv, installs the
Moonshine package, downloads the C++ SDK, fetches the models and compiles.

## Requirements

- Linux with a PulseAudio or PipeWire sound server
- `gtk4` and `libpulse` development headers:
  - Arch / CachyOS: `sudo pacman -S gtk4 pkgconf libpulse`
  - Debian / Ubuntu: `sudo apt install libgtk-4-dev pkg-config libpulse-dev`
  - Fedora: `sudo dnf install gtk4-devel pkgconf-pkg-config libpulse-devel`
- Python 3 (for the build only; not needed at runtime)

No GPU is used or required.

## How it works

```
Pulse/PipeWire monitor source
        |  16 kHz mono s16, 50 ms chunks
        v
capture thread
        |  AudioQueue per language (lossless, non-blocking)
        v
worker thread per language
        |  Moonshine Transcriber (streaming)
        v
listener -> GTK text buffer, and to olas-moonshine-notes.txt
```

Two models run in parallel, each processing the same audio stream. Both
transcribe everything; you read the pane for the language being spoken. This is
simpler and more robust than language detection, at the cost of the non-target
pane producing nonsense — a known limitation, discussed in
[PATCHNOTES.md](PATCHNOTES.md).

Each language runs on its own worker thread with its own Transcriber, on its
own CPU core set. The core budget and per-model split are derived from the
detected hardware at startup; `--diagnose` shows what was chosen.

### Source layout

```
src/
    olas-gtk-1.1.cpp     CLI, GTK UI, capture, worker wiring
    resource_plan.*      CPU detection, per-model core split, affinity
    system_probe.*       CPU and memory detection
tools/
    fetch-streaming-models.sh   model downloader
    thread_bench.cpp            RTF vs thread count
    pipeline_bench.cpp          end-to-end lag and audio loss
    bench_tune.cpp              transcriber option sweep
    config_test.cpp             config parser test
```

## Configuration

`olas-1.1.conf` holds the transcription parameters:

```ini
[general]
vad_threshold = 0.5             # speech/silence threshold
vad_max_segment_duration = 12   # longest single line, seconds
transcription_interval = 1.0    # how often the decoder re-runs
```

Delete it and built-in defaults apply. Bad values and unknown keys warn on
stderr; they never stop the program starting.

Threading and affinity are not configured here — they come from the hardware
probe. To override:

```sh
OLAS_CPU_CORES=2 ./gtk-launcher.sh      # shrink the core budget
OLAS_CPU_CORES=0 ./gtk-launcher.sh      # disable pinning
OLAS_THREADS=1   ./gtk-launcher.sh      # force single-threaded (lowest CPU)
OLAS_MOONSHINE_ARCHS=4,4 ./gtk-launcher.sh   # explicit arch per language
```

## Command line

```
-l, --language CODE[,CODE]   Language codes [default: en,es]
-m, --model PATH[,PATH]      Model directories
-a, --arch N[,N]             Architecture per language
-r, --rms THRESHOLD          Silence RMS threshold
-q, --chunk-ms MS            Capture chunk in ms [default: 50]
-c, --config PATH            Parameters file [default: olas-1.1.conf]
-v, --verbose                Per-line latency to olas-debug.log
    --diagnose               Print hardware probe and plan, then exit
    --stats                  Print diagnostics on exit
-h, --help                   Show help
```

Architecture numbers: `0`=Tiny, `1`=Base, `2`=Tiny Streaming,
`4`=Small Streaming, `5`=Medium Streaming. (`3`=Base Streaming is declared in
Moonshine's API but unsupported and rejected.)

## Built with

- **[Moonshine Voice](https://github.com/moonshine-ai/moonshine)** — the
  speech-to-text models and C++ runtime. Small and Medium Streaming
  architectures, MIT licensed.
- **GTK4** — the user interface.
- **PulseAudio / PipeWire** — audio capture from a monitor source, so the
  program transcribes what the machine is playing rather than a microphone.

## License

MIT. Moonshine and its models are MIT; see `license.txt` for the full text.
