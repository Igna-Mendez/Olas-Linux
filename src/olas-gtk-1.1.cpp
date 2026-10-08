// OLAS — Open Linux Audio Scribe (GTK4 build) — version 1.1
// Split-pane live captions via Moonshine Voice.
//
// Known limitation: each pane receives all audio, so the non-target pane
// produces cross-language nonsense; no language identification is used.
//
// Usage: ./gtk-launcher.sh   (resolves models and arch per language)


#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <gtk/gtk.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <getopt.h>
#include <pthread.h>
#include <sched.h>

#include "moonshine-cpp.h"

#include "resource_plan.h"
#include "system_probe.h"

// #define OLAS_MOONSHINE_EXTENDED_LANGUAGES 1

// ---------------------------------------------------------------------------
// Transcription parameters, loaded from olas-1.1.conf.
// ---------------------------------------------------------------------------

struct OlasConfig {
    double vad_threshold = 0.5;
    int    vad_max_segment_duration = 15;
    // How often the streaming decoders re-run: the dominant CPU lever, and it
    // does not change the transcript (measured: 44 s of audio took 69 s of
    // wall clock at 0.5 vs 58 s at 2.0, same 13 lines). Higher = less CPU,
    // slower on-screen updates.
    double transcription_interval = 1.0;

    static std::string default_path() {
        if (const char *e = std::getenv("OLAS_CONFIG")) return e;
        return "olas-1.1.conf";
    }

    // Tolerant parser: unknown keys and bad values produce warnings, never
    // a hard failure, so a typo cannot stop the app from starting.
    static OlasConfig load(const std::string &path,
                           std::vector<std::string> &warnings) {
        OlasConfig c;
        std::ifstream f(path);
        if (!f) return c;

        int lineno = 0;
        std::string line;

        auto trim = [](const std::string &s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) return std::string();
            size_t b = s.find_last_not_of(" \t\r\n");
            return s.substr(a, b - a + 1);
        };
        auto lower = [](std::string s) {
            for (char &ch : s) ch = (char)std::tolower((unsigned char)ch);
            return s;
        };

        while (std::getline(f, line)) {
            ++lineno;
            std::string l = trim(line);
            if (l.empty() || l[0] == '#' || l[0] == ';') continue;
            const size_t hash = l.find(" #");
            if (hash != std::string::npos) l = trim(l.substr(0, hash));

            if (l[0] == '[') {
                // Sections are accepted and ignored so older config files
                // don't produce spurious warnings.
                continue;
            }

            const size_t eq = l.find('=');
            if (eq == std::string::npos) {
                warnings.push_back("line " + std::to_string(lineno) +
                                   ": expected key = value");
                continue;
            }
            const std::string key = lower(trim(l.substr(0, eq)));
            const std::string val = trim(l.substr(eq + 1));

            auto to_d = [&](double &out) {
                try { size_t u = 0; out = std::stod(val, &u);
                      return u == val.size(); }
                catch (...) { return false; }
            };
            auto to_i = [&](int &out) {
                try { size_t u = 0; out = std::stoi(val, &u);
                      return u == val.size(); }
                catch (...) { return false; }
            };

            if (key == "vad_threshold") {
                double d = 0.0;
                if (to_d(d) && d >= 0.0 && d <= 1.0) c.vad_threshold = d;
                else warnings.push_back("line " + std::to_string(lineno) +
                                        ": vad_threshold must be 0.0..1.0");
            } else if (key == "vad_max_segment_duration" || key == "max_segment") {
                int i = 0;
                if (to_i(i) && i >= 1 && i <= 30) c.vad_max_segment_duration = i;
                else warnings.push_back("line " + std::to_string(lineno) +
                                        ": vad_max_segment_duration must be 1..30");
            } else if (key == "transcription_interval") {
                double d = 0.0;
                if (to_d(d) && d >= 0.1 && d <= 5.0) c.transcription_interval = d;
                else warnings.push_back("line " + std::to_string(lineno) +
                                        ": transcription_interval must be 0.1..5.0");
            } else {
                warnings.push_back("line " + std::to_string(lineno) +
                                   ": unknown key '" + key + "' (ignored)");
            }
        }
        return c;
    }
};

static OlasConfig g_ocfg;

// Detected hardware and the resource plan derived from it. Both are filled
// once in main() before any Transcriber is built.
static olas::SystemInfo  g_sys;
static olas::ResourcePlan g_plan;

// ---------- constants ----------

static constexpr int SAMPLE_RATE = 16000;
static constexpr int VAD_FRAME_MS = 20;
static constexpr int VAD_FRAME_SAMPLES = SAMPLE_RATE / 1000 * VAD_FRAME_MS;
static constexpr int VAD_HANGOVER_MS = 350;
static constexpr int VAD_PREROLL_MS  = 300;

static constexpr int PARTIAL_INTERVAL_MS = 500;
static constexpr int PARTIAL_INTERVAL_SAMPLES = SAMPLE_RATE / 1000 * PARTIAL_INTERVAL_MS;
static constexpr int MIN_PARTIAL_AUDIO_MS = 300;
static constexpr int MAX_PARTIAL_WINDOW_MS = 2500;
static constexpr int MAX_PARTIAL_WINDOW_SAMPLES = SAMPLE_RATE / 1000 * MAX_PARTIAL_WINDOW_MS;

static constexpr int MAX_SEGMENT_MS = 8000;

static constexpr int DEFAULT_CAPTURE_CHUNK_MS = 50;
static constexpr int MIN_CAPTURE_CHUNK_MS = 20;
static constexpr int MAX_CAPTURE_CHUNK_MS = 1000;

static constexpr double FOLLOW_RESUME_TOLERANCE_PX = 40.0;
static constexpr double FOLLOW_BREAK_TOLERANCE_PX  = 100.0;
static constexpr gint64 FOLLOW_COOLDOWN_US = 200000;   /* 200 ms */

static constexpr int MAX_LOG_LINES = 4000;
static constexpr int TRIM_TO_LINES = 2000;

// Audio backlog limits, not discard points (see AudioQueue): past the soft
// limit the backlog is reported but nothing is dropped.
static constexpr size_t SOFT_BACKLOG_WARN_CHUNKS = 200;   /* ~10 s */

// Hard ceiling: memory guard only, not a tuning value.
static constexpr size_t MAX_QUEUE_CHUNKS = 1200;          /* ~60 s */

static constexpr int ARCH_TINY = 0;
static constexpr int ARCH_BASE = 1;
static constexpr int ARCH_TINY_STREAMING = 2;
static constexpr int ARCH_BASE_STREAMING = 3;
static constexpr int ARCH_SMALL_STREAMING = 4;
static constexpr int ARCH_MEDIUM_STREAMING = 5;

static constexpr const char *DEF_MONITOR_SRC = "auto";
static constexpr const char *NOTES_FILE = "olas-moonshine-notes.txt";
static constexpr const char *VERBOSE_LOG_FILE = "olas-debug.log";
static constexpr double DEF_SILENCE_RMS = 100.0;

static constexpr int DEFAULT_BODY_PT = 13;
static constexpr int UI_CHROME_PT = 9;

// Resource sizing lives in resource_plan.cpp. The probe runs once in main()
// and the plan is applied by apply_resource_plan() below; a hard cap keeps a
// very wide machine from being swallowed whole.
static constexpr int MAX_AUTO_CORES = 6;

// ---------- globals ----------

struct LanguageConfig {
    std::string language;
    std::string model_path;
    int arch = ARCH_SMALL_STREAMING;
    bool spelling = false;
};

static std::vector<LanguageConfig> g_configs;
static std::string g_monitor_src = DEF_MONITOR_SRC;
static double g_silence_rms = DEF_SILENCE_RMS;
static int g_capture_chunk_ms = DEFAULT_CAPTURE_CHUNK_MS;
static bool g_show_stats = false;

static FILE *g_notes = nullptr;
static volatile sig_atomic_t g_running = 1;
static std::mutex g_out_mutex;

static bool  g_verbose     = false;
static FILE *g_verbose_log = nullptr;
static std::mutex g_log_mutex;

static void on_signal(int) { g_running = 0; }

static void print_error(const char *prefix, const char *detail) {
    std::lock_guard<std::mutex> lk(g_out_mutex);
    std::fprintf(stderr, "%s%s\n", prefix, detail ? detail : "unknown error");
}

static void verbose_logf(const char *fmt, ...) {
    if (!g_verbose || !g_verbose_log) return;
    std::lock_guard<std::mutex> lk(g_log_mutex);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_verbose_log, fmt, ap);
    va_end(ap);
    std::fflush(g_verbose_log);
}

// ---------- helpers ----------

static std::vector<std::string> split_csv(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

static bool parse_int(const char *s, int &out) {
    if (!s || !*s) return false;
    char *end = nullptr;
    errno = 0;
    long v = std::strtol(s, &end, 10);
    if (errno == ERANGE || end == s || *end != '\0' ||
        v < std::numeric_limits<int>::min() ||
        v > std::numeric_limits<int>::max()) return false;
    out = static_cast<int>(v);
    return true;
}

static bool parse_double(const char *s, double &out) {
    if (!s || !*s) return false;
    char *end = nullptr;
    errno = 0;
    double v = std::strtod(s, &end);
    if (errno == ERANGE || end == s || *end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

static bool is_streaming_arch(int a) {
    return a >= ARCH_TINY_STREAMING && a <= ARCH_MEDIUM_STREAMING;
}
static bool valid_arch(int a) {
    if (a == ARCH_BASE_STREAMING) return false;
    return a >= ARCH_TINY && a <= ARCH_MEDIUM_STREAMING;
}

static bool is_supported_language(const std::string &lang) {
#ifdef OLAS_MOONSHINE_EXTENDED_LANGUAGES
    static const std::vector<std::string> s = {
        "en","es","ar","ja","ko","zh","vi","uk"
    };
#else
    static const std::vector<std::string> s = { "en", "es" };
#endif
    for (const auto &x : s) if (x == lang) return true;
    return false;
}

static std::string default_model_dir(int arch, const std::string &lang) {
    switch (arch) {
        case ARCH_TINY_STREAMING:   return "models/tiny-streaming-"   + lang;
        case ARCH_SMALL_STREAMING:  return "models/small-streaming-"  + lang;
        case ARCH_MEDIUM_STREAMING: return "models/medium-streaming-" + lang;
        case ARCH_TINY:             return "models/tiny-" + lang;
        default:                    return "models/base-" + lang;
    }
}

static double frame_rms(const int16_t *p, int n) {
    if (!p || n <= 0) return 0.0;
    double acc = 0.0;
    for (int i = 0; i < n; ++i) {
        const double x = static_cast<double>(p[i]);
        acc += x * x;
    }
    return std::sqrt(acc / static_cast<double>(n));
}

// Restrict the calling thread to cores [from, to). Used to give each model its
// own cores while its ORT session is being created; worker threads the session
// spawns inherit the set. Failure is not fatal -- the caller carries on with
// whatever mask it has.
static void pin_current_thread(int from, int to) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = from; i < to; ++i) CPU_SET(i, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0)
        std::fprintf(stderr,
            "warning: per-model sched_setaffinity(%d..%d) failed: %s\n",
            from, to - 1, std::strerror(errno));
}

// ---------------------------------------------------------------------------
// Probe the machine and apply a resource plan, before any Transcriber exists
// (MOONSHINE_ORT_SINGLE_THREAD is read inside session creation, and the
// affinity mask must cover the worker threads that session spawns).
// ---------------------------------------------------------------------------
static void limit_inference_resources(const std::vector<int> &archs) {
    const int model_count = static_cast<int>(archs.size());
    g_sys = olas::probe_system();

    // Optional overrides, -1 = auto.
    int force_threads = -1;
    int force_cores   = -1;

    if (const char *env = std::getenv("OLAS_THREADS")) {
        char *end = nullptr;
        long n = std::strtol(env, &end, 10);
        if (end != env && *end == '\0' && n >= 1 && n <= 64)
            force_threads = static_cast<int>(n);
        else if (*env)
            std::fprintf(stderr,
                "warning: OLAS_THREADS='%s' invalid (1..64); using auto\n",
                env);
    }

    if (const char *env = std::getenv("OLAS_CPU_CORES")) {
        char *end = nullptr;
        long n = std::strtol(env, &end, 10);
        if (end != env && *end == '\0' && n >= 0 && n <= 4096)
            force_cores = static_cast<int>(n);
        else if (*env)
            std::fprintf(stderr,
                "warning: OLAS_CPU_CORES='%s' invalid; using auto\n", env);
    }

    g_plan = olas::plan_resources(g_sys, model_count, archs,
                                  force_threads, force_cores);

    std::fprintf(stderr, "system: %d logical / %d physical cores",
                 g_sys.cpu.logical_cores, g_sys.cpu.physical_cores);
    if (g_sys.cpu.has_smt) std::fprintf(stderr, " (SMT)");
    if (!g_sys.cpu.model.empty())
        std::fprintf(stderr, ", %s", g_sys.cpu.model.c_str());
    std::fprintf(stderr, "\n");
    if (g_sys.mem_total_kb)
        std::fprintf(stderr, "system: %llu MiB RAM (%llu MiB available)\n",
                     (unsigned long long)(g_sys.mem_total_kb / 1024),
                     (unsigned long long)(g_sys.mem_avail_kb / 1024));
    std::fprintf(stderr, "plan:   %s\n", g_plan.rationale.c_str());

    // Boolean switch, not a count: any value gives one ORT thread. Unset (the
    // default) lets ORT size its own pool, bounded by affinity below.
    if (g_plan.force_single_thread) {
        setenv("MOONSHINE_ORT_SINGLE_THREAD", "1", 1);
        std::fprintf(stderr, "threads: single (MOONSHINE_ORT_SINGLE_THREAD=1)\n");
    } else {
        unsetenv("MOONSHINE_ORT_SINGLE_THREAD");
        std::fprintf(stderr, "threads: ORT-managed pool, bounded by affinity\n");
    }

    if (!g_plan.pin_affinity || g_plan.affinity_cores <= 0) {
        std::fprintf(stderr, "CPU: unpinned (OLAS_CPU_CORES=0)\n");
        return;
    }

    pin_current_thread(0, g_plan.affinity_cores);
    std::fprintf(stderr, "CPU: %d-core budget of %d",
                 g_plan.affinity_cores, g_sys.cpu.logical_cores);
    if (!g_plan.model_cores.empty()) {
        std::fprintf(stderr, ", split");
        for (size_t i = 0; i < g_plan.model_cores.size(); ++i)
            std::fprintf(stderr, "%s%d", i ? "+" : " ", g_plan.model_cores[i]);
        std::fprintf(stderr, " per model");
    }
    std::fprintf(stderr, "\n");
}

// ---------- PulseAudio source discovery ----------

struct SourceListCtx {
    std::vector<std::string> monitors;
    std::string default_sink;
    bool sources_done = false;
    bool server_done = false;
};

static void source_info_cb(pa_context *, const pa_source_info *info, int eol, void *ud) {
    auto *ctx = static_cast<SourceListCtx *>(ud);
    if (eol != 0) { if (eol > 0) ctx->sources_done = true; return; }
    if (!info || !info->name) return;
    std::string name(info->name);
    if (name.empty()) return;
    if (info->monitor_of_sink != PA_INVALID_INDEX ||
        (name.size() >= 8 && name.compare(name.size() - 8, 8, ".monitor") == 0))
        ctx->monitors.push_back(std::move(name));
}

static void server_info_cb(pa_context *, const pa_server_info *info, void *ud) {
    auto *ctx = static_cast<SourceListCtx *>(ud);
    if (info && info->default_sink_name) ctx->default_sink = info->default_sink_name;
    ctx->server_done = true;
}

static void context_state_cb(pa_context *c, void *ud) {
    if (pa_context_get_state(c) == PA_CONTEXT_READY)
        *static_cast<bool *>(ud) = true;
}

static bool list_monitor_sources(std::vector<std::string> &out_monitors,
                                 std::string &out_default_sink) {
    pa_mainloop *ml = pa_mainloop_new();
    if (!ml) return false;
    pa_context *ctx = pa_context_new(pa_mainloop_get_api(ml), "olas-gtk-discover");
    if (!ctx) { pa_mainloop_free(ml); return false; }

    bool ready = false;
    pa_context_set_state_callback(ctx, context_state_cb, &ready);
    if (pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        pa_context_unref(ctx); pa_mainloop_free(ml); return false;
    }
    const auto rd = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!ready && pa_context_get_state(ctx) != PA_CONTEXT_FAILED &&
           pa_context_get_state(ctx) != PA_CONTEXT_TERMINATED &&
           std::chrono::steady_clock::now() < rd)
        pa_mainloop_iterate(ml, 0, nullptr);
    if (!ready) { pa_context_disconnect(ctx); pa_context_unref(ctx); pa_mainloop_free(ml); return false; }

    SourceListCtx sc;
    pa_operation *so = pa_context_get_source_info_list(ctx, source_info_cb, &sc);
    pa_operation *vo = pa_context_get_server_info(ctx, server_info_cb, &sc);
    if (!so || !vo) {
        if (so) pa_operation_unref(so);
        if (vo) pa_operation_unref(vo);
        pa_context_disconnect(ctx); pa_context_unref(ctx); pa_mainloop_free(ml);
        return false;
    }
    const auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!sc.sources_done || !sc.server_done) &&
           std::chrono::steady_clock::now() < dl)
        pa_mainloop_iterate(ml, 0, nullptr);

    pa_operation_unref(so); pa_operation_unref(vo);
    pa_context_disconnect(ctx); pa_context_unref(ctx); pa_mainloop_free(ml);

    if (!sc.sources_done) return false;
    out_monitors = std::move(sc.monitors);
    out_default_sink = std::move(sc.default_sink);
    return true;
}

static std::string choose_monitor_source(const std::vector<std::string> &monitors,
                                         const std::string &default_sink) {
    if (monitors.empty()) return {};
    if (!default_sink.empty()) {
        const std::string preferred = default_sink + ".monitor";
        for (const auto &m : monitors) if (m == preferred) return m;
    }
    return monitors.front();
}

// ---------- audio queue (lossless) ----------

// Lossless, non-blocking audio queue.
//
// push() must never block: the capture thread pushes to every pane in turn,
// so a blocking push lets one slow model stall PulseAudio for all of them.
// The pre-1.1 queue did exactly that at a 3-minute depth, and the transcript
// drifted further behind without ever recovering.
//
// push() must also never silently discard: accuracy is the point of this
// program, so a drop-oldest queue is not acceptable. Backlog past the soft
// limit is warned about and counted but every chunk is kept. The hard limit
// is only a memory guard; any discard there is loud and shows up in --stats.
//
// At the measured real-time factors (~0.42 Medium, ~0.15 Small) the backlog
// does not grow in normal use, so the guard should never fire.
class AudioQueue {
public:
    explicit AudioQueue(size_t hard_max = MAX_QUEUE_CHUNKS)
        : hard_max_(hard_max) {}

    bool push(std::shared_ptr<const std::vector<int16_t>> chunk) {
        if (!chunk || chunk->empty()) return true;

        std::lock_guard<std::mutex> lk(mutex_);
        if (closed_) return false;

        if (queue_.size() >= SOFT_BACKLOG_WARN_CHUNKS && !warned_) {
            warned_ = true;
            std::fprintf(stderr,
                "warning: audio backlog is %.1f s; inference is behind but "
                "nothing is being dropped\n",
                (double)queue_.size() * 0.050);
        } else if (queue_.size() < SOFT_BACKLOG_WARN_CHUNKS / 2) {
            warned_ = false;
        }

        // Last-resort memory guard. Never reached in normal operation.
        if (queue_.size() >= hard_max_) {
            queue_.pop_front();
            ++dropped_;
            std::fprintf(stderr,
                "error: audio backlog exceeded %.1f s; dropping oldest chunk "
                "(total dropped: %llu)\n",
                (double)hard_max_ * 0.050,
                (unsigned long long)dropped_);
        }

        queue_.push_back(std::move(chunk));
        cv_.notify_one();
        return true;
    }

    bool pop(std::shared_ptr<const std::vector<int16_t>> &out) {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait(lk, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    size_t depth() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return queue_.size();
    }

    uint64_t dropped() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return dropped_;
    }

    void close() {
        std::lock_guard<std::mutex> lk(mutex_);
        closed_ = true;
        cv_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<const std::vector<int16_t>>> queue_;
    size_t hard_max_;
    uint64_t dropped_ = 0;
    bool closed_ = false;
    bool warned_ = false;
};

// ---------- Pane ----------

struct Pane {
    GtkWidget *box = nullptr;
    GtkWidget *header = nullptr;
    GtkWidget *stop_btn = nullptr;
    GtkWidget *detach_btn = nullptr;
    GtkWidget *scrolled = nullptr;
    GtkWidget *view = nullptr;
    GtkTextBuffer *buffer = nullptr;
    GtkTextTag *stamp_tag = nullptr;
    GtkTextTag *error_tag = nullptr;
    GtkTextTag *body_tag = nullptr;
    GtkTextMark *mark = nullptr;

    GtkWidget *paned = nullptr;
    GtkWidget *float_window = nullptr;
    bool was_start_child = true;
    bool suppress_vadj = false;
    guint scroll_idle_id = 0;
    std::string language;
    std::string last_partial;

    gint64 last_content_change_us = 0;
};

static std::vector<Pane*> g_panes;
static GtkWidget *g_main_window = nullptr;
static GtkApplication *g_app = nullptr;
static int g_body_font_pt = DEFAULT_BODY_PT;
static GtkCssProvider *g_font_provider = nullptr;
static bool g_provider_attached = false;
static bool g_show_stamps = true;

static GtkWidget *g_stamps_btn = nullptr;
static GtkWidget *g_zoom_out_btn = nullptr;
static GtkWidget *g_zoom_in_btn = nullptr;
static GtkWidget *g_focus_btn = nullptr;
static GtkWidget *g_follow_btn = nullptr;
static bool g_focus_mode = false;

static bool g_follow_tail = true;
static bool g_suppress_follow_toggle = false;

struct UiUpdate {
    int slot;
    std::string prefix;
    std::string body;
    bool is_final;
    bool is_error;
};

static gboolean apply_update(gpointer data);

static void queue_update(int slot, std::string prefix, std::string body,
                         bool is_final, bool is_error) {
    UiUpdate *u = new UiUpdate{slot, std::move(prefix), std::move(body),
                               is_final, is_error};
    g_idle_add(apply_update, u);
}

// ---------- listener ----------

class PrintListener : public moonshine::TranscriptEventListener {
public:
    PrintListener(FILE *notes, std::string language, int slot_idx,
                  std::chrono::system_clock::time_point session_start)
        : notes_(notes), language_(std::move(language)),
          slot_idx_(slot_idx), session_start_(session_start) {}

    void set_stream_start(std::chrono::system_clock::time_point tp) {
        session_start_ = tp;
    }

    void emit_partial(const std::string &text) {
        if (text.empty()) return;
        auto [pre, body] = split(text, current_start_);
        queue_update(slot_idx_, std::move(pre), std::move(body), false, false);
    }

    void emit_final(const std::string &text, double rel_time) {
        if (text.empty()) return;
        auto [pre, body] = split(text, rel_time);
        std::string full = pre + body + "\n";
        if (notes_) {
            std::lock_guard<std::mutex> lk(g_out_mutex);
            std::fwrite(full.data(), 1, full.size(), notes_);
            std::fflush(notes_);
        }
        queue_update(slot_idx_, std::move(pre), std::move(body), true, false);
    }

    void set_segment_start(double rel_time) { current_start_ = rel_time; }
    void add_paused_ms(int64_t ms) { paused_ms_.fetch_add(ms); }

    void onLineTextChanged(const moonshine::LineTextChanged &e) override {
        emit_partial(e.line.text);
    }
    void onLineCompleted(const moonshine::LineCompleted &e) override {
        verbose_logf("[%s] latency=%d ms  text=\"%.80s\"\n",
                     language_.c_str(),
                     e.line.lastTranscriptionLatencyMs,
                     e.line.text.c_str());
        emit_final(e.line.text, e.line.startTime);
    }
    void onError(const moonshine::Error &e) override {
        queue_update(slot_idx_, "", e.errorMessage, true, true);
    }

private:
    std::pair<std::string, std::string> split(const std::string &text, double rel) {
        double sec = rel;
        if (!std::isfinite(sec) || sec < 0.0) sec = 0.0;
        sec += static_cast<double>(paused_ms_.load()) / 1000.0;
        auto tp = session_start_ +
                  std::chrono::milliseconds(static_cast<long long>(sec * 1000.0));
        std::time_t tt = std::chrono::system_clock::to_time_t(tp);
        struct tm tmv{};
        localtime_r(&tt, &tmv);
        char wall[16];
        std::strftime(wall, sizeof wall, "%H:%M:%S", &tmv);

        std::string pre;
        pre.reserve(32);
        pre += "[";
        pre += wall;
        pre += "] [";
        pre += language_;
        pre += "] ";
        return { std::move(pre), text };
    }

    FILE *notes_;
    std::string language_;
    int slot_idx_;
    std::chrono::system_clock::time_point session_start_;
    double current_start_ = 0.0;
    std::atomic<int64_t> paused_ms_{0};
};

// ---------- VAD ----------

class StatefulVad {
public:
    struct Segment { std::vector<int16_t> audio; uint64_t start_sample = 0; };

    StatefulVad(double rms_threshold, int hangover_ms, int preroll_ms, int max_segment_ms)
        : rms_threshold_(rms_threshold),
          hangover_frames_(std::max(1, hangover_ms / VAD_FRAME_MS)),
          preroll_frames_(std::max(0, preroll_ms / VAD_FRAME_MS)),
          max_segment_samples_(SAMPLE_RATE / 1000 * max_segment_ms) {}

    void reset() {
        state_ = State::Silence; silence_count_ = 0;
        preroll_.clear(); segment_.clear();
        preroll_start_sample_ = 0; segment_start_sample_ = 0;
    }

    bool feed(const int16_t *frame, int n, uint64_t frame_start_sample, Segment &out) {
        if (!frame || n != VAD_FRAME_SAMPLES) return false;
        const double r = frame_rms(frame, n);

        if (state_ == State::Silence) {
            preroll_.emplace_back(frame, frame + n);
            preroll_start_sample_ = frame_start_sample;
            while (static_cast<int>(preroll_.size()) > preroll_frames_) {
                preroll_.pop_front();
                preroll_start_sample_ += VAD_FRAME_SAMPLES;
            }
            if (r >= rms_threshold_) {
                state_ = State::Speech; silence_count_ = 0; segment_.clear();
                segment_start_sample_ =
                    preroll_.empty() ? frame_start_sample : preroll_start_sample_;
                if (preroll_.empty()) segment_.insert(segment_.end(), frame, frame + n);
                else for (const auto &f : preroll_)
                    segment_.insert(segment_.end(), f.begin(), f.end());
                preroll_.clear();
            }
            return false;
        }

        segment_.insert(segment_.end(), frame, frame + n);

        if (max_segment_samples_ > 0) {
            const uint64_t elapsed =
                (frame_start_sample + static_cast<uint64_t>(n)) - segment_start_sample_;
            if (elapsed >= static_cast<uint64_t>(max_segment_samples_)) {
                out.audio = std::move(segment_);
                out.start_sample = segment_start_sample_;
                segment_.clear(); preroll_.clear();
                state_ = State::Silence; silence_count_ = 0;
                return true;
            }
        }

        if (r < rms_threshold_) {
            ++silence_count_;
            if (silence_count_ >= hangover_frames_) {
                out.audio = std::move(segment_);
                out.start_sample = segment_start_sample_;
                segment_.clear(); preroll_.clear();
                state_ = State::Silence; silence_count_ = 0;
                return true;
            }
        } else silence_count_ = 0;
        return false;
    }

    bool flush(Segment &out) {
        if (state_ != State::Speech || segment_.empty()) return false;
        out.audio = std::move(segment_);
        out.start_sample = segment_start_sample_;
        segment_.clear(); preroll_.clear();
        state_ = State::Silence; silence_count_ = 0;
        return true;
    }

    bool in_speech() const { return state_ == State::Speech; }
    const std::vector<int16_t> &current_audio() const { return segment_; }
    uint64_t current_start_sample() const { return segment_start_sample_; }

private:
    enum class State { Silence, Speech };
    double rms_threshold_;
    int hangover_frames_, preroll_frames_;
    int max_segment_samples_ = 0;
    int silence_count_ = 0;
    State state_ = State::Silence;
    uint64_t preroll_start_sample_ = 0, segment_start_sample_ = 0;
    std::deque<std::vector<int16_t>> preroll_;
    std::vector<int16_t> segment_;
};

// ---------- streaming worker ----------

class StreamingWorker {
public:
    StreamingWorker(const std::string &model_path, int arch,
                    std::chrono::system_clock::time_point session_start,
                    FILE *notes, const std::string &language, int slot_idx,
                    bool spelling)
        : listener_(std::make_unique<PrintListener>(
              notes, language, slot_idx, session_start)),
          spelling_(spelling) {
        char interval_s[32], maxseg_s[32], vad_s[32];
        std::snprintf(interval_s, sizeof interval_s, "%.3f",
                      g_ocfg.transcription_interval);
        std::snprintf(maxseg_s, sizeof maxseg_s, "%d",
                      g_ocfg.vad_max_segment_duration);
        std::snprintf(vad_s, sizeof vad_s, "%.3f", g_ocfg.vad_threshold);

        moonshine::Options opts = {
            {"transcription_interval",   interval_s},
            {"return_audio_data",        "false"},
            {"vad_max_segment_duration", maxseg_s},
            {"vad_threshold",            vad_s},
        };

        if (spelling_) {
            opts.emplace_back("spelling_model_path",
                              model_path + "/spelling_cnn.ort");
        }

        transcriber_ = std::make_unique<moonshine::Transcriber>(
            model_path, to_model_arch(arch),
            g_ocfg.transcription_interval, "", opts);
        transcriber_->addListener(listener_.get());
        transcriber_->start();
    }
    ~StreamingWorker() {
        if (transcriber_) { try { transcriber_->stop(); } catch (...) {} }
    }

    void set_session_start(std::chrono::system_clock::time_point tp) {
        listener_->set_stream_start(tp);
    }
    void add_paused_ms(int64_t ms) { listener_->add_paused_ms(ms); }

    void feed(const std::vector<int16_t> &pcm) {
        if (!transcriber_ || pcm.empty()) return;
        fb_.resize(pcm.size());
        for (size_t i = 0; i < pcm.size(); ++i)
            fb_[i] = static_cast<float>(pcm[i]) / 32768.0f;
        transcriber_->addAudio(fb_, SAMPLE_RATE);
    }
    void flush() {
        if (!transcriber_) return;
        try {
            uint32_t flags = spelling_ ? MOONSHINE_FLAG_SPELLING_MODE : 0u;
            transcriber_->updateTranscription(flags);
        }
        catch (const moonshine::MoonshineException &e) {
            print_error("[stream flush failed] ", e.what());
        }
    }

private:
    static moonshine::ModelArch to_model_arch(int a) {
        switch (a) {
            case ARCH_TINY: return moonshine::ModelArch::TINY;
            case ARCH_BASE: return moonshine::ModelArch::BASE;
            case ARCH_TINY_STREAMING: return moonshine::ModelArch::TINY_STREAMING;
            case ARCH_BASE_STREAMING: return moonshine::ModelArch::BASE_STREAMING;
            case ARCH_SMALL_STREAMING: return moonshine::ModelArch::SMALL_STREAMING;
            case ARCH_MEDIUM_STREAMING: return moonshine::ModelArch::MEDIUM_STREAMING;
            default: throw std::invalid_argument("bad arch");
        }
    }
    std::unique_ptr<moonshine::Transcriber> transcriber_;
    std::unique_ptr<PrintListener> listener_;
    std::vector<float> fb_;
    bool spelling_ = false;
};

// ---------- non-streaming worker ----------

class NonStreamingWorker {
public:
    NonStreamingWorker(const std::string &model_path, int arch,
                       std::chrono::system_clock::time_point session_start,
                       FILE *notes, double silence_rms,
                       const std::string &language, int slot_idx,
                       bool spelling)
        : listener_(std::make_unique<PrintListener>(
              notes, language, slot_idx, session_start)),
          vad_(std::make_unique<StatefulVad>(
              silence_rms, VAD_HANGOVER_MS, VAD_PREROLL_MS, MAX_SEGMENT_MS)),
          spelling_(spelling) {
        const moonshine::ModelArch ma =
            (arch == ARCH_TINY) ? moonshine::ModelArch::TINY
                                : moonshine::ModelArch::BASE;
        moonshine::Options opts = {
            {"return_audio_data", "false"},
            {"vad_threshold",     "0.55"},
        };
        if (spelling_) {
            opts.emplace_back("spelling_model_path",
                              model_path + "/spelling_cnn.ort");
        }
        transcriber_ = std::make_unique<moonshine::Transcriber>(
            model_path, ma, 0.5, "", opts);
        inference_ = std::make_unique<InferenceThread>(
            transcriber_.get(), listener_.get(), &last_inference_ms_, spelling_);
    }

    void set_session_start(std::chrono::system_clock::time_point tp) {
        listener_->set_stream_start(tp);
    }
    void reset() {
        vad_->reset();
        pcm_rem_.clear();
        last_partial_at_ = 0;
        inference_->clear_pending_partial();
    }
    void add_paused_ms(int64_t ms) { listener_->add_paused_ms(ms); }

    void feed(const std::vector<int16_t> &pcm) {
        if (pcm.empty()) return;
        pcm_rem_.insert(pcm_rem_.end(), pcm.begin(), pcm.end());
        while (pcm_rem_.size() >= VAD_FRAME_SAMPLES) {
            const uint64_t frame_start = total_seen_;
            std::vector<int16_t> frame(pcm_rem_.begin(),
                                       pcm_rem_.begin() + VAD_FRAME_SAMPLES);
            pcm_rem_.erase(pcm_rem_.begin(),
                           pcm_rem_.begin() + VAD_FRAME_SAMPLES);
            total_seen_ += VAD_FRAME_SAMPLES;

            StatefulVad::Segment seg;
            if (vad_->feed(frame.data(), VAD_FRAME_SAMPLES, frame_start, seg)) {
                inference_->post_final(std::move(seg.audio), seg.start_sample);
                last_partial_at_ = 0;
                continue;
            }
            if (vad_->in_speech()) {
                const auto &audio = vad_->current_audio();
                const uint64_t start = vad_->current_start_sample();
                const uint64_t elapsed = total_seen_ - start;
                const uint64_t since = total_seen_ - last_partial_at_;
                const bool enough = elapsed >=
                    (uint64_t)(SAMPLE_RATE * MIN_PARTIAL_AUDIO_MS / 1000);
                const int64_t last_ms = last_inference_ms_.load();
                const uint64_t min_gap = std::max<uint64_t>(
                    PARTIAL_INTERVAL_SAMPLES,
                    last_ms > 0 ? (uint64_t)last_ms * 32u : 0u);
                const bool due = last_partial_at_ == 0 || since >= min_gap;
                if (enough && due) {
                    const size_t win = std::min(audio.size(),
                        static_cast<size_t>(MAX_PARTIAL_WINDOW_SAMPLES));
                    const uint64_t ws = start + (audio.size() - win);
                    std::vector<int16_t> w(audio.end() - win, audio.end());
                    inference_->post_partial(std::move(w), ws);
                    last_partial_at_ = total_seen_;
                }
            }
        }
    }

    void flush() {
        StatefulVad::Segment seg;
        if (vad_->flush(seg))
            inference_->post_final(std::move(seg.audio), seg.start_sample);
        inference_->drain();
    }

private:
    class InferenceThread {
    public:
        InferenceThread(moonshine::Transcriber *t, PrintListener *l,
                        std::atomic<int64_t> *ms_sink, bool spelling)
            : t_(t), l_(l), ms_sink_(ms_sink), spelling_(spelling) {
            th_ = std::thread([this] { run(); });
        }
        ~InferenceThread() {
            { std::lock_guard<std::mutex> lk(m_); closing_ = true; cv_.notify_all(); }
            if (th_.joinable()) th_.join();
        }

        void post_partial(std::vector<int16_t> &&a, uint64_t s) {
            std::lock_guard<std::mutex> lk(m_);
            pending_partial_ = Work{std::move(a), s};
            cv_.notify_one();
        }
        void post_final(std::vector<int16_t> &&a, uint64_t s) {
            std::lock_guard<std::mutex> lk(m_);
            finals_.push_back(Work{std::move(a), s});
            ++pending_finals_;
            cv_.notify_one();
        }
        void clear_pending_partial() {
            std::lock_guard<std::mutex> lk(m_);
            pending_partial_.reset();
        }
        void drain() {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] { return pending_finals_ == 0; });
        }

    private:
        struct Work { std::vector<int16_t> audio; uint64_t start_sample; };

        void run() {
            std::vector<float> fb;
            for (;;) {
                Work w; bool is_final;
                {
                    std::unique_lock<std::mutex> lk(m_);
                    cv_.wait(lk, [this] {
                        return closing_ || !finals_.empty() || pending_partial_.has_value();
                    });
                    if (closing_ && finals_.empty() && !pending_partial_.has_value()) return;
                    if (!finals_.empty()) {
                        w = std::move(finals_.front()); finals_.pop_front();
                        is_final = true;
                    } else {
                        w = std::move(*pending_partial_); pending_partial_.reset();
                        is_final = false;
                    }
                }
                if (is_final) {
                    transcribe(w, fb, true);
                    std::lock_guard<std::mutex> lk(m_);
                    --pending_finals_;
                    cv_.notify_all();
                } else transcribe(w, fb, false);
            }
        }

        void transcribe(const Work &w, std::vector<float> &fb, bool is_final) {
            if (w.audio.empty()) return;
            fb.resize(w.audio.size());
            for (size_t i = 0; i < w.audio.size(); ++i)
                fb[i] = static_cast<float>(w.audio[i]) / 32768.0f;

            const double rel = static_cast<double>(w.start_sample) / SAMPLE_RATE;
            l_->set_segment_start(rel);
            const auto t0 = std::chrono::steady_clock::now();
            try {
                uint32_t flags = spelling_ ? MOONSHINE_FLAG_SPELLING_MODE : 0u;
                const moonshine::Transcript t =
                    t_->transcribeWithoutStreaming(fb, SAMPLE_RATE, flags);

                std::string combined;
                double first_offset = 0.0;
                for (size_t i = 0; i < t.lines.size(); ++i) {
                    if (i == 0) first_offset = t.lines[i].startTime;
                    if (i) combined += ' ';
                    combined += t.lines[i].text;
                }
                const auto ms = std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
                if (ms_sink_) ms_sink_->store(ms);
                verbose_logf("[transcribe %s] audio=%.2fs  wall=%lldms  text=\"%.40s\"\n",
                             is_final ? "final" : "partial",
                             (double)w.audio.size() / SAMPLE_RATE,
                             (long long)ms,
                             combined.c_str());
                if (combined.empty()) return;

                if (is_final) l_->emit_final(combined, rel + first_offset);
                else          l_->emit_partial(combined);
            } catch (const moonshine::MoonshineException &e) {
                if (is_final) print_error("[transcription failed] ", e.what());
            }
        }

        moonshine::Transcriber *t_;
        PrintListener *l_;
        std::atomic<int64_t> *ms_sink_;
        std::thread th_;
        std::mutex m_;
        std::condition_variable cv_;
        std::deque<Work> finals_;
        int pending_finals_ = 0;
        std::optional<Work> pending_partial_;
        bool closing_ = false;
        bool spelling_ = false;
    };

    std::unique_ptr<moonshine::Transcriber> transcriber_;
    std::unique_ptr<PrintListener> listener_;
    std::unique_ptr<StatefulVad> vad_;
    std::unique_ptr<InferenceThread> inference_;
    std::atomic<int64_t> last_inference_ms_{0};
    uint64_t total_seen_ = 0, last_partial_at_ = 0;
    std::vector<int16_t> pcm_rem_;
    bool spelling_ = false;
};

// ---------- worker slot ----------

struct WorkerSlot {
    LanguageConfig config;
    std::unique_ptr<StreamingWorker> stream;
    std::unique_ptr<NonStreamingWorker> offline;
    std::unique_ptr<AudioQueue> queue;
    std::thread thread;

    std::shared_ptr<std::atomic<bool>> enabled;
    std::shared_ptr<std::atomic<bool>> need_reset;
    std::shared_ptr<std::atomic<int64_t>> disabled_at_ns;

    uint64_t chunks_pushed{0};
    uint64_t chunks_processed{0};
    uint64_t queue_depth{0};
    uint64_t queue_max_depth{0};
    uint64_t chunks_dropped{0};
    uint64_t inference_ns_total{0};
    uint64_t audio_frames_total{0};

    void feed(const std::vector<int16_t> &pcm) {
        if (!enabled || !enabled->load()) return;
        if (stream) stream->feed(pcm);
        else if (offline) offline->feed(pcm);
    }
    void set_session_start(std::chrono::system_clock::time_point tp) {
        if (stream) stream->set_session_start(tp);
        if (offline) offline->set_session_start(tp);
    }
    void flush() {
        if (stream) stream->flush();
        else if (offline) offline->flush();
    }
    void add_paused_ms(int64_t ms) {
        if (stream) stream->add_paused_ms(ms);
        if (offline) offline->add_paused_ms(ms);
    }

    void start() {
        queue = std::make_unique<AudioQueue>();
        AudioQueue *q = queue.get();
        thread = std::thread([this, q] {
            std::shared_ptr<const std::vector<int16_t>> chunk;
            while (q->pop(chunk)) {
                if (!chunk || chunk->empty()) continue;
                chunks_processed += 1;
                audio_frames_total += chunk->size();
                if (need_reset && need_reset->exchange(false)) {
                    if (offline) offline->reset();
                }
                try {
                    const auto t0 = std::chrono::steady_clock::now();
                    feed(*chunk);
                    const auto dt = std::chrono::steady_clock::now() - t0;
                    inference_ns_total +=
                        std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count();
                }
                catch (const moonshine::MoonshineException &e) {
                    print_error("[Moonshine processing failed] ", e.what());
                } catch (const std::exception &e) {
                    print_error("[processing failed] ", e.what());
                }
            }
        });
    }
    void stop() {
        if (queue) queue->close();
        if (thread.joinable()) thread.join();
    }
};

static std::vector<WorkerSlot> *g_slots_ptr = nullptr;

// ---------- pane build, detach, update ----------

// CSS must be attached after a real GDK display exists (i.e. after the
// window is built), otherwise no widget sees the rules.
static void apply_font_size(int pt) {
    g_body_font_pt = std::max(6, std::min(48, pt));

    if (!g_font_provider) {
        g_font_provider = gtk_css_provider_new();
    }

    GdkDisplay *disp = nullptr;
    if (g_main_window) disp = gtk_widget_get_display(g_main_window);
    if (!disp) disp = gdk_display_get_default();

    if (disp && !g_provider_attached) {
        gtk_style_context_add_provider_for_display(
            disp,
            GTK_STYLE_PROVIDER(g_font_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_provider_attached = true;
    }

    char css[512];
    std::snprintf(css, sizeof css,
        "textview { font-size: %dpt; }"
        "button, togglebutton {"
        "  font-size: %dpt;"
        "  padding: 2px 8px;"
        "  min-height: 0;"
        "  min-width: 0;"
        "}"
        "label { font-size: %dpt; }",
        g_body_font_pt, UI_CHROME_PT, UI_CHROME_PT);

    gtk_css_provider_load_from_string(g_font_provider, css);
}

static void on_zoom_in(GtkButton *, gpointer)  { apply_font_size(g_body_font_pt + 2); }
static void on_zoom_out(GtkButton *, gpointer) { apply_font_size(g_body_font_pt - 2); }

static void on_toggle_stamps(GtkToggleButton *btn, gpointer) {
    g_show_stamps = gtk_toggle_button_get_active(btn);
    for (Pane *p : g_panes) {
        if (p->stamp_tag)
            g_object_set(p->stamp_tag, "invisible", !g_show_stamps, nullptr);
    }
}

static void apply_focus_mode(void) {
    if (g_stamps_btn)   gtk_widget_set_visible(g_stamps_btn,   !g_focus_mode);
    if (g_zoom_out_btn) gtk_widget_set_visible(g_zoom_out_btn, !g_focus_mode);
    if (g_zoom_in_btn)  gtk_widget_set_visible(g_zoom_in_btn,  !g_focus_mode);
    if (g_follow_btn)   gtk_widget_set_visible(g_follow_btn,   !g_focus_mode);
    for (Pane *p : g_panes) {
        if (p->header && !p->float_window)
            gtk_widget_set_visible(p->header, !g_focus_mode);
    }
}

static void on_focus_toggled(GtkToggleButton *btn, gpointer) {
    g_focus_mode = gtk_toggle_button_get_active(btn);
    gtk_button_set_label(GTK_BUTTON(btn), g_focus_mode ? "Exit focus" : "Focus");
    apply_focus_mode();
}

static void toggle_pane(int idx);

static void on_stop_clicked(GtkButton *, gpointer data) {
    toggle_pane(GPOINTER_TO_INT(data));
}

static void detach_pane(Pane *p);
static void reattach_pane(Pane *p);

static void on_detach_clicked(GtkButton *, gpointer data) {
    Pane *p = static_cast<Pane *>(data);
    if (p->float_window) reattach_pane(p);
    else detach_pane(p);
}

static gboolean on_float_close(GtkWindow *, gpointer data) {
    reattach_pane(static_cast<Pane *>(data));
    return TRUE;
}

static void detach_pane(Pane *p) {
    if (p->float_window || !p->paned) return;

    GtkWidget *parent = p->paned;
    if (gtk_paned_get_start_child(GTK_PANED(parent)) == p->box)
        p->was_start_child = true;
    else if (gtk_paned_get_end_child(GTK_PANED(parent)) == p->box)
        p->was_start_child = false;
    else return;

    g_object_ref(p->box);
    if (p->was_start_child)
        gtk_paned_set_start_child(GTK_PANED(parent), nullptr);
    else
        gtk_paned_set_end_child(GTK_PANED(parent), nullptr);

    GtkWidget *w = gtk_window_new();
    if (g_app) gtk_application_add_window(g_app, GTK_WINDOW(w));
    std::string title = "OLAS — " + p->language;
    gtk_window_set_title(GTK_WINDOW(w), title.c_str());
    gtk_window_set_default_size(GTK_WINDOW(w), 640, 420);
    gtk_window_set_child(GTK_WINDOW(w), p->box);
    p->float_window = w;
    g_signal_connect(w, "close-request", G_CALLBACK(on_float_close), p);
    gtk_window_present(GTK_WINDOW(w));
    g_object_unref(p->box);

    if (p->header) gtk_widget_set_visible(p->header, TRUE);

    gtk_button_set_label(GTK_BUTTON(p->detach_btn), "Dock");
}

static void reattach_pane(Pane *p) {
    if (!p->float_window) return;
    GtkWidget *w = p->float_window;
    p->float_window = nullptr;

    g_object_ref(p->box);
    gtk_window_set_child(GTK_WINDOW(w), nullptr);
    gtk_window_destroy(GTK_WINDOW(w));

    if (p->paned) {
        if (p->was_start_child)
            gtk_paned_set_start_child(GTK_PANED(p->paned), p->box);
        else
            gtk_paned_set_end_child(GTK_PANED(p->paned), p->box);
    }
    g_object_unref(p->box);

    if (p->header) gtk_widget_set_visible(p->header, !g_focus_mode);

    gtk_button_set_label(GTK_BUTTON(p->detach_btn), "Detach");
}

static void update_follow_button(void) {
    if (!g_follow_btn) return;
    g_suppress_follow_toggle = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_follow_btn), g_follow_tail);
    g_suppress_follow_toggle = false;
}

static void scroll_pane_to_bottom(Pane *p) {
    if (!p->view || !p->mark) return;
    p->suppress_vadj = true;
    gtk_text_view_scroll_to_mark(
        GTK_TEXT_VIEW(p->view),
        p->mark,
        0.0,
        TRUE,
        0.0,
        1.0);
    p->suppress_vadj = false;
}

static void on_vadj_value_changed(GtkAdjustment *adj, gpointer data) {
    Pane *p = static_cast<Pane *>(data);
    if (p->suppress_vadj) return;

    const gint64 now = g_get_monotonic_time();
    if (now - p->last_content_change_us < FOLLOW_COOLDOWN_US) return;

    const double val   = gtk_adjustment_get_value(adj);
    const double size  = gtk_adjustment_get_page_size(adj);
    const double upper = gtk_adjustment_get_upper(adj);
    const double dist_from_bottom = upper - (val + size);

    if (g_follow_tail) {
        if (dist_from_bottom > FOLLOW_BREAK_TOLERANCE_PX) {
            g_follow_tail = false;
            update_follow_button();
        }
    } else {
        if (dist_from_bottom <= FOLLOW_RESUME_TOLERANCE_PX) {
            g_follow_tail = true;
            update_follow_button();
        }
    }
}

static void queue_scroll_to_bottom(Pane *p) {
    if (p->scroll_idle_id) return;
    // Needs to fire after GTK's layout pass; an idle would land one line short.
    p->scroll_idle_id = g_timeout_add_full(
        G_PRIORITY_DEFAULT,
        50,
        [](gpointer d) -> gboolean {
            Pane *pp = static_cast<Pane *>(d);
            pp->scroll_idle_id = 0;
            if (g_follow_tail) scroll_pane_to_bottom(pp);
            return G_SOURCE_REMOVE;
        },
        p, nullptr);
}

static void on_follow_toggled(GtkToggleButton *btn, gpointer) {
    if (g_suppress_follow_toggle) return;
    g_follow_tail = gtk_toggle_button_get_active(btn);
    if (g_follow_tail) {
        for (Pane *p : g_panes) scroll_pane_to_bottom(p);
    }
}

static void on_view_map(GtkWidget *, gpointer data) {
    Pane *p = static_cast<Pane *>(data);
    if (g_follow_tail) scroll_pane_to_bottom(p);
}

static Pane *build_pane(const std::string &language, int idx) {
    Pane *p = new Pane();
    p->language = language;

    p->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(p->box, TRUE);
    gtk_widget_set_vexpand(p->box, TRUE);

    p->header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(p->header, 4);
    gtk_widget_set_margin_end(p->header, 4);
    gtk_widget_set_margin_top(p->header, 2);
    gtk_widget_set_margin_bottom(p->header, 2);

    GtkWidget *lang_lbl = gtk_label_new(language.c_str());
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(lang_lbl), attrs);
    pango_attr_list_unref(attrs);
    gtk_box_append(GTK_BOX(p->header), lang_lbl);

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(p->header), spacer);

    p->stop_btn = gtk_button_new_with_label("Stop");
    g_signal_connect(p->stop_btn, "clicked",
                     G_CALLBACK(on_stop_clicked), GINT_TO_POINTER(idx));
    gtk_box_append(GTK_BOX(p->header), p->stop_btn);

    p->detach_btn = gtk_button_new_with_label("Detach");
    g_signal_connect(p->detach_btn, "clicked",
                     G_CALLBACK(on_detach_clicked), p);
    gtk_box_append(GTK_BOX(p->header), p->detach_btn);

    gtk_box_append(GTK_BOX(p->box), p->header);

    p->view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(p->view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(p->view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(p->view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(p->view), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(p->view), 6);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(p->view), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(p->view), 4);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(p->view), 4);

    p->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(p->view));

    p->stamp_tag = gtk_text_buffer_create_tag(p->buffer, nullptr,
                                              "scale", 0.5, "foreground", "#888888",
                                              "invisible", !g_show_stamps, nullptr);
    p->error_tag = gtk_text_buffer_create_tag(p->buffer, nullptr,
                                              "scale", 0.5, "foreground", "#888888", nullptr);

    p->body_tag = gtk_text_buffer_create_tag(p->buffer, nullptr, nullptr);

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(p->buffer, &end);
    p->mark = gtk_text_buffer_create_mark(p->buffer, nullptr, &end, TRUE);

    p->scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(p->scrolled), p->view);
    gtk_widget_set_vexpand(p->scrolled, TRUE);
    gtk_box_append(GTK_BOX(p->box), p->scrolled);

    GtkAdjustment *vadj =
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(p->scrolled));
    g_signal_connect(vadj, "value-changed",
                     G_CALLBACK(on_vadj_value_changed), p);
    g_signal_connect(p->view, "map",
                     G_CALLBACK(on_view_map), p);

    return p;
}

static void trim_buffer_if_needed(GtkTextBuffer *buf) {
    const int n_lines = gtk_text_buffer_get_line_count(buf);
    if (n_lines <= MAX_LOG_LINES) return;

    const int to_delete = n_lines - TRIM_TO_LINES;
    if (to_delete <= 0) return;

    GtkTextIter iter_start, iter_end;
    gtk_text_buffer_get_start_iter(buf, &iter_start);
    iter_end = iter_start;
    for (int i = 0; i < to_delete; ++i) {
        if (!gtk_text_iter_forward_line(&iter_end)) break;
    }
    gtk_text_buffer_delete(buf, &iter_start, &iter_end);
}

static gboolean apply_update(gpointer data) {
    UiUpdate *u = static_cast<UiUpdate *>(data);
    if (u->slot < 0 || u->slot >= static_cast<int>(g_panes.size())) {
        delete u;
        return G_SOURCE_REMOVE;
    }
    Pane *p = g_panes[u->slot];
    GtkTextBuffer *buf = p->buffer;

    p->last_content_change_us = g_get_monotonic_time();

    {
        GtkTextIter start, end;
        gtk_text_buffer_get_iter_at_mark(buf, &start, p->mark);
        gtk_text_buffer_get_end_iter(buf, &end);
        gtk_text_buffer_delete(buf, &start, &end);
    }

    if (u->is_error) {
        p->last_partial.clear();
        GtkTextIter pos;
        gtk_text_buffer_get_end_iter(buf, &pos);
        if (gtk_text_buffer_get_char_count(buf) > 0) {
            GtkTextIter prev = pos;
            gtk_text_iter_backward_char(&prev);
            if (gtk_text_iter_get_char(&prev) != '\n')
                gtk_text_buffer_insert(buf, &pos, "\n", -1);
        }
        std::string msg = "[error] " + u->body + "\n";
        gtk_text_buffer_insert_with_tags(buf, &pos, msg.c_str(), -1,
                                         p->error_tag, nullptr);
        GtkTextIter e2;
        gtk_text_buffer_get_end_iter(buf, &e2);
        gtk_text_buffer_move_mark(buf, p->mark, &e2);
        trim_buffer_if_needed(buf);
        if (g_follow_tail) queue_scroll_to_bottom(p);
        delete u;
        return G_SOURCE_REMOVE;
    }

    if (!u->is_final) {
        std::string cur = u->prefix + u->body;
        if (cur == p->last_partial) { delete u; return G_SOURCE_REMOVE; }
        p->last_partial = std::move(cur);
    } else {
        p->last_partial.clear();
    }

    GtkTextIter pos;
    gtk_text_buffer_get_end_iter(buf, &pos);

    if (u->is_final && gtk_text_buffer_get_char_count(buf) > 0) {
        GtkTextIter prev = pos;
        gtk_text_iter_backward_char(&prev);
        if (gtk_text_iter_get_char(&prev) != '\n')
            gtk_text_buffer_insert(buf, &pos, "\n", -1);
    }

    gtk_text_buffer_move_mark(buf, p->mark, &pos);

    gtk_text_buffer_insert_with_tags(buf, &pos, u->prefix.c_str(), -1,
                                     p->stamp_tag, nullptr);
    gtk_text_buffer_insert_with_tags(buf, &pos, u->body.c_str(), -1,
                                     p->body_tag, nullptr);

    if (u->is_final) {
        gtk_text_buffer_insert(buf, &pos, "\n", -1);
        GtkTextIter e2;
        gtk_text_buffer_get_end_iter(buf, &e2);
        gtk_text_buffer_move_mark(buf, p->mark, &e2);
        trim_buffer_if_needed(buf);
    }

    if (g_follow_tail) queue_scroll_to_bottom(p);

    delete u;
    return G_SOURCE_REMOVE;
}

static void toggle_pane(int idx) {
    if (!g_slots_ptr) return;
    if (idx < 0 || idx >= static_cast<int>(g_slots_ptr->size())) return;
    auto &s = (*g_slots_ptr)[idx];
    if (!s.enabled) return;

    bool now = !s.enabled->load();
    s.enabled->store(now);

    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    if (!now) {
        if (s.need_reset) s.need_reset->store(true);
        if (s.disabled_at_ns) s.disabled_at_ns->store(now_ns);
        if (idx < static_cast<int>(g_panes.size())) {
            Pane *p = g_panes[idx];
            GtkTextIter start, end;
            gtk_text_buffer_get_iter_at_mark(p->buffer, &start, p->mark);
            gtk_text_buffer_get_end_iter(p->buffer, &end);
            gtk_text_buffer_delete(p->buffer, &start, &end);
            p->last_partial.clear();
        }
    } else if (s.disabled_at_ns) {
        const int64_t was = s.disabled_at_ns->exchange(-1);
        if (was >= 0) {
            const int64_t ms = (now_ns - was) / 1000000;
            if (ms > 0) s.add_paused_ms(ms);
        }
    }
    if (idx < static_cast<int>(g_panes.size())) {
        gtk_button_set_label(GTK_BUTTON(g_panes[idx]->stop_btn),
                             now ? "Stop" : "Start");
    }
}

// ---------- capture ----------

struct CaptureArgs {
    pa_simple *pa = nullptr;
    int chunk_samples = 0;
    std::vector<WorkerSlot> *slots = nullptr;
};

static void *capture_thread(void *arg) {
    auto *a = static_cast<CaptureArgs *>(arg);

    while (g_running) {
        auto pcm = std::make_shared<std::vector<int16_t>>(a->chunk_samples);
        int pa_err = 0;
        const int rc = pa_simple_read(a->pa, pcm->data(),
                                      pcm->size() * sizeof(int16_t), &pa_err);
        if (rc < 0) {
            if (g_running)
                std::fprintf(stderr, "pa_simple_read failed: %s\n",
                             pa_strerror(pa_err));
            break;
        }
        auto shared = std::static_pointer_cast<const std::vector<int16_t>>(pcm);
        for (auto &s : *a->slots) {
            if (!s.queue) continue;
            if (s.enabled && !s.enabled->load()) continue;
            // push() never blocks: a pane that cannot keep up no longer stalls
            // the capture thread or the other pane.
            s.queue->push(shared);
            s.chunks_pushed += 1;
            s.chunks_dropped = s.queue->dropped();
            const size_t d = s.queue->depth();
            s.queue_depth = d;
            if (d > s.queue_max_depth) s.queue_max_depth = d;
        }
    }
    return nullptr;
}

// ---------- window & app ----------

static GtkWidget *build_main_window(GtkApplication *app) {
    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "OLAS — Open Linux Audio Scribe");
    gtk_window_set_default_size(GTK_WINDOW(win), 1280, 720);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(win), outer);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(bar, 4);
    gtk_widget_set_margin_end(bar, 4);
    gtk_widget_set_margin_top(bar, 4);
    gtk_widget_set_margin_bottom(bar, 4);

    g_stamps_btn = gtk_toggle_button_new_with_label("Stamps");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_stamps_btn), g_show_stamps);
    g_signal_connect(g_stamps_btn, "toggled",
                     G_CALLBACK(on_toggle_stamps), nullptr);
    gtk_box_append(GTK_BOX(bar), g_stamps_btn);

    g_zoom_out_btn = gtk_button_new_with_label("A−");
    g_zoom_in_btn  = gtk_button_new_with_label("A+");
    g_signal_connect(g_zoom_out_btn, "clicked", G_CALLBACK(on_zoom_out), nullptr);
    g_signal_connect(g_zoom_in_btn,  "clicked", G_CALLBACK(on_zoom_in),  nullptr);
    gtk_box_append(GTK_BOX(bar), g_zoom_out_btn);
    gtk_box_append(GTK_BOX(bar), g_zoom_in_btn);

    g_follow_btn = gtk_toggle_button_new_with_label("Follow");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_follow_btn), g_follow_tail);
    g_signal_connect(g_follow_btn, "toggled",
                     G_CALLBACK(on_follow_toggled), nullptr);
    gtk_box_append(GTK_BOX(bar), g_follow_btn);

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(bar), spacer);

    g_focus_btn = gtk_toggle_button_new_with_label("Focus");
    g_signal_connect(g_focus_btn, "toggled",
                     G_CALLBACK(on_focus_toggled), nullptr);
    gtk_box_append(GTK_BOX(bar), g_focus_btn);

    gtk_box_append(GTK_BOX(outer), bar);

    if (g_panes.size() == 1) {
        gtk_box_append(GTK_BOX(outer), g_panes[0]->box);
    } else {
        GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_widget_set_vexpand(paned, TRUE);
        gtk_paned_set_position(GTK_PANED(paned), 640);
        gtk_paned_set_start_child(GTK_PANED(paned), g_panes[0]->box);
        gtk_paned_set_end_child(GTK_PANED(paned), g_panes[1]->box);
        g_panes[0]->paned = paned;
        g_panes[1]->paned = paned;
        gtk_box_append(GTK_BOX(outer), paned);
    }

    return win;
}

static void on_app_activate(GtkApplication *app, gpointer) {
    g_app = app;
    if (g_main_window) {
        gtk_window_present(GTK_WINDOW(g_main_window));
        return;
    }
    if (g_panes.empty()) {
        for (size_t i = 0; i < g_configs.size(); ++i)
            g_panes.push_back(build_pane(g_configs[i].language, static_cast<int>(i)));
    }
    g_main_window = build_main_window(app);
    apply_font_size(g_body_font_pt);
    gtk_window_present(GTK_WINDOW(g_main_window));
}

// ---------- CLI ----------

static void usage(const char *prog) {
    std::fprintf(stderr,
        "olas-gtk — Open Linux Audio Scribe (GTK4 build)\n\n"
        "Usage: %s [options]\n\n"
        "  -m, --model PATH[,PATH]    Model directory per language\n"
        "  -s, --source NAME          Pulse/PipeWire monitor source [auto]\n"
        "  -a, --arch N[,N]           Architecture per language [4=SmallStreaming]\n"
        "  -l, --language CODE[,CODE] Comma-separated language codes [en,es]\n"
        "  -r, --rms THRESHOLD        Silence RMS threshold [%.0f]\n"
        "  -q, --chunk-ms MS          Capture chunk in ms [%d]\n"
        "      --spelling LANG[,LANG] Enable spelling mode for the listed languages\n"
        "  -v, --verbose              Write per-line latency to %s\n"
        "      --diagnose             Print hardware probe + resource plan, then exit\n"
        "  -c, --config PATH          Transcription parameters [olas-1.1.conf]\n"
        "      --stats                Print diagnostics on exit\n"
        "  -h, --help                 Show this help\n\n"
        "Architecture numbers:\n"
        "  0=Tiny  1=Base  2=TinyStreaming  4=SmallStreaming  5=MediumStreaming\n"
        "  (3=BaseStreaming is not supported by Moonshine and is rejected.)\n\n"
        "Resources are sized from the machine: cores and memory are probed at\n"
        "  startup and the thread budget is scaled to fit (max %d cores).\n"
        "  Override with OLAS_CPU_CORES=N (affinity budget; 0 disables pinning)\n"
        "  and OLAS_THREADS=N (force single-threaded inference).\n"
        "  Run with --diagnose to see what was detected.\n\n"
        "Defaults to Medium Streaming for English (5) and Small Streaming for\n"
        "  other languages (4). At most two languages in this build.\n",
        prog, DEF_SILENCE_RMS, DEFAULT_CAPTURE_CHUNK_MS,
        VERBOSE_LOG_FILE, MAX_AUTO_CORES);
}

int main(int argc, char *argv[]) {
    static struct option long_opts[] = {
        {"model",    required_argument, 0, 'm'},
        {"source",   required_argument, 0, 's'},
        {"arch",     required_argument, 0, 'a'},
        {"language", required_argument, 0, 'l'},
        {"rms",      required_argument, 0, 'r'},
        {"chunk-ms", required_argument, 0, 'q'},
        {"verbose",  no_argument,       0, 'v'},
        {"spelling", required_argument, 0, 1001},
        {"config",   required_argument, 0, 'c'},
        {"stats",    no_argument,       0, 1000},
        {"diagnose", no_argument,       0, 1002},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    std::vector<std::string> languages, models;
    std::vector<int> arches;
    std::vector<std::string> spelling_langs;
    bool diagnose_only = false;

    std::string config_path = OlasConfig::default_path();

    int opt;
    while ((opt = getopt_long(argc, argv, "m:s:a:l:r:q:c:vh",
                              long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'm': models = split_csv(optarg); break;
            case 's': g_monitor_src = optarg; break;
            case 'l': languages = split_csv(optarg); break;

            case 'a': {
                arches.clear();
                for (auto &tok : split_csv(optarg)) {
                    int v;
                    if (!parse_int(tok.c_str(), v) || !valid_arch(v)) {
                        std::fprintf(stderr, "invalid --arch: %s\n", tok.c_str());
                        return 1;
                    }
                    arches.push_back(v);
                }
                break;
            }
            case 'r': {
                double v;
                if (!parse_double(optarg, v) || v < 0.0) {
                    std::fprintf(stderr, "invalid --rms\n"); return 1;
                }
                g_silence_rms = v;
                break;
            }
            case 'q': {
                int v;
                if (!parse_int(optarg, v) ||
                    v < MIN_CAPTURE_CHUNK_MS || v > MAX_CAPTURE_CHUNK_MS) {
                    std::fprintf(stderr, "invalid --chunk-ms\n"); return 1;
                }
                g_capture_chunk_ms = v;
                break;
            }
            case 'v': g_verbose = true; break;
            case 'c': config_path = optarg; break;
            case 'h': usage(argv[0]); return 0;
            case 1000: g_show_stats = true; break;
            case 1001: spelling_langs = split_csv(optarg); break;
            case 1002: diagnose_only = true; break;
            default: usage(argv[0]); return 1;
        }
    }

    {
        std::vector<std::string> warnings;
        g_ocfg = OlasConfig::load(config_path, warnings);
        for (const auto &w : warnings)
            std::fprintf(stderr, "config: %s\n", w.c_str());
        std::fprintf(stderr,
            "config: %s  vad=%.2f max_seg=%ds interval=%.2fs\n",
            config_path.c_str(), g_ocfg.vad_threshold,
            g_ocfg.vad_max_segment_duration,
            g_ocfg.transcription_interval);
    }

    if (languages.empty()) { languages.push_back("en"); languages.push_back("es"); }

    for (const auto &l : languages) {
        if (!is_supported_language(l)) {
            std::fprintf(stderr, "unsupported language: %s\n", l.c_str());
            return 1;
        }
    }

    if (languages.size() > 2) {
        std::fprintf(stderr,
            "olas-gtk supports at most two languages (%zu requested).\n",
            languages.size());
        return 1;
    }

    if (arches.empty()) {
        // Per-language defaults, matching the launcher: English has a Medium
        // Streaming model and uses it; every other language falls back to
        // Small Streaming, the only streaming model most of them have.
        arches.reserve(languages.size());
        for (const auto &l : languages)
            arches.push_back(l == "en" ? ARCH_MEDIUM_STREAMING
                                       : ARCH_SMALL_STREAMING);
    } else if (arches.size() == 1 && languages.size() > 1) {
        arches.assign(languages.size(), arches.front());
    } else if (arches.size() != languages.size()) {
        std::fprintf(stderr, "--arch count mismatch\n"); return 1;
    }

    if (models.empty()) {
        models.reserve(languages.size());
        for (size_t i = 0; i < languages.size(); ++i)
            models.push_back(default_model_dir(arches[i], languages[i]));
    } else if (models.size() == 1 && languages.size() > 1) {
        models.assign(languages.size(), models.front());
    } else if (models.size() != languages.size()) {
        std::fprintf(stderr, "--model count mismatch\n"); return 1;
    }

    for (const auto &sl : spelling_langs) {
        if (std::find(languages.begin(), languages.end(), sl) == languages.end()) {
            std::fprintf(stderr, "warning: --spelling %s: language not active\n",
                         sl.c_str());
        }
    }

    // Size the thread pool and CPU affinity now that the models are known.
    // Must run before any Transcriber is constructed: MOONSHINE_ORT_SINGLE_THREAD
    // is read inside session creation, and the affinity mask applies to the
    // ORT worker threads that session spawns.
    limit_inference_resources(arches);

    // --diagnose prints the probe and plan and stops before touching GTK or
    // the sound server, so it works over SSH and in containers.
    if (diagnose_only) {
        std::fprintf(stderr, "\nresources: %s\n", g_plan.rationale.c_str());
        std::fprintf(stderr, "interval:  %.2fs  max_seg: %ds\n",
                     g_ocfg.transcription_interval,
                     g_ocfg.vad_max_segment_duration);
        return 0;
    }

    for (size_t i = 0; i < languages.size(); ++i) {
        LanguageConfig c;
        c.language = languages[i];
        c.model_path = models[i];
        c.arch = arches[i];
        c.spelling = std::find(spelling_langs.begin(), spelling_langs.end(),
                               languages[i]) != spelling_langs.end();
        g_configs.push_back(std::move(c));
    }

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    g_notes = std::fopen(NOTES_FILE, "w");
    if (!g_notes) {
        std::fprintf(stderr, "could not open %s: %s\n",
                     NOTES_FILE, std::strerror(errno));
        return 1;
    }

    if (g_verbose) {
        g_verbose_log = std::fopen(VERBOSE_LOG_FILE, "w");
        if (!g_verbose_log) {
            std::fprintf(stderr, "warning: could not open %s\n", VERBOSE_LOG_FILE);
        } else {
            std::fprintf(g_verbose_log, "OLAS verbose log\n");
            std::fprintf(g_verbose_log, "languages:");
            for (auto &l : languages) std::fprintf(g_verbose_log, " %s", l.c_str());
            std::fprintf(g_verbose_log, "\narches:");
            for (auto &a : arches) std::fprintf(g_verbose_log, " %d", a);
            std::fprintf(g_verbose_log, "\nmodels:");
            for (auto &m : models) std::fprintf(g_verbose_log, " %s", m.c_str());
            std::fprintf(g_verbose_log, "\nspelling:");
            for (auto &l : languages) {
                const bool on = std::find(spelling_langs.begin(),
                                          spelling_langs.end(), l) != spelling_langs.end();
                std::fprintf(g_verbose_log, " %s=%s", l.c_str(), on ? "on" : "off");
            }
            std::fprintf(g_verbose_log, "\n\n");
            std::fflush(g_verbose_log);
        }
    }

    std::vector<std::string> monitors;
    std::string default_sink;
    list_monitor_sources(monitors, default_sink);

    std::string chosen = g_monitor_src;
    if (chosen == "auto") {
        chosen = choose_monitor_source(monitors, default_sink);
        if (chosen.empty()) chosen = "@DEFAULT_SINK@.monitor";
        std::fprintf(stderr, "monitor source: %s\n", chosen.c_str());
    }

    std::vector<WorkerSlot> slots;
    slots.resize(g_configs.size());
    const auto provisional_start = std::chrono::system_clock::now();
    try {
        int core_cursor = 0;
        for (size_t i = 0; i < g_configs.size(); ++i) {
            auto &s = slots[i];
            s.config = g_configs[i];
            s.enabled = std::make_shared<std::atomic<bool>>(true);
            s.need_reset = std::make_shared<std::atomic<bool>>(false);
            s.disabled_at_ns = std::make_shared<std::atomic<int64_t>>(-1);

            // Restrict this thread to the model's own cores before building the
            // transcriber, so its ORT session sizes its pool from that set and
            // does not contend with the other model.
            const int want = (i < g_plan.model_cores.size())
                                 ? g_plan.model_cores[i] : 0;
            const int restore_from = core_cursor;
            const int restore_to = core_cursor + want;
            if (want > 0) {
                pin_current_thread(restore_from, restore_to);
                core_cursor = restore_to;
            }

            if (is_streaming_arch(s.config.arch)) {
                s.stream = std::make_unique<StreamingWorker>(
                    s.config.model_path, s.config.arch, provisional_start,
                    g_notes, s.config.language, static_cast<int>(i),
                    s.config.spelling);
            } else {
                s.offline = std::make_unique<NonStreamingWorker>(
                    s.config.model_path, s.config.arch, provisional_start,
                    g_notes, g_silence_rms, s.config.language, static_cast<int>(i),
                    s.config.spelling);
            }
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "failed to initialise Moonshine: %s\n", e.what());
        std::fclose(g_notes); g_notes = nullptr;
        if (g_verbose_log) { std::fclose(g_verbose_log); g_verbose_log = nullptr; }
        return 1;
    }

    // The per-model pinning above left this thread on the last model's cores.
    // Widen back to the whole budget so the capture thread, GTK and the audio
    // callback are not confined to one model's share.
    if (g_plan.pin_affinity && g_plan.affinity_cores > 0)
        pin_current_thread(0, g_plan.affinity_cores);

    g_slots_ptr = &slots;

    pa_sample_spec spec{};
    spec.format = PA_SAMPLE_S16LE;
    spec.rate = SAMPLE_RATE;
    spec.channels = 1;

    const int chunk_samples = SAMPLE_RATE * g_capture_chunk_ms / 1000;
    const size_t chunk_bytes = static_cast<size_t>(chunk_samples) * sizeof(int16_t);

    pa_buffer_attr attr{};
    attr.fragsize  = static_cast<uint32_t>(chunk_bytes);
    attr.maxlength = static_cast<uint32_t>(chunk_bytes * 4);
    attr.tlength   = static_cast<uint32_t>(-1);
    attr.prebuf    = static_cast<uint32_t>(-1);
    attr.minreq    = static_cast<uint32_t>(-1);

    int pa_err = 0;
    pa_simple *pa = pa_simple_new(
        nullptr, "olas-gtk", PA_STREAM_RECORD, chosen.c_str(),
        "capture", &spec, nullptr, &attr, &pa_err);
    if (!pa) {
        std::fprintf(stderr, "pa_simple_new failed: %s\n", pa_strerror(pa_err));
        std::fclose(g_notes); g_notes = nullptr;
        if (g_verbose_log) { std::fclose(g_verbose_log); g_verbose_log = nullptr; }
        return 1;
    }

    GtkApplication *app =
        gtk_application_new("ai.moonshine.olas", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), nullptr);

    const auto session_start = std::chrono::system_clock::now();
    for (auto &s : slots) s.set_session_start(session_start);
    for (auto &s : slots) s.start();

    CaptureArgs cargs;
    cargs.pa = pa;
    cargs.chunk_samples = chunk_samples;
    cargs.slots = &slots;

    pthread_t cap_tid{};
    pthread_create(&cap_tid, nullptr, capture_thread, &cargs);

    int rc = g_application_run(G_APPLICATION(app), 1, argv);

    g_running = 0;
    pthread_join(cap_tid, nullptr);
    for (auto &s : slots) s.stop();
    for (auto &s : slots) s.flush();

    if (g_show_stats) {
        for (size_t i = 0; i < slots.size() && i < languages.size(); ++i) {
            const auto &s = slots[i];
            std::fprintf(stderr, "\n=== Slot %zu (%s) ===\n", i, languages[i].c_str());
            std::fprintf(stderr, "  chunks_pushed      : %llu\n",
                         (unsigned long long)s.chunks_pushed);
            std::fprintf(stderr, "  chunks_processed   : %llu\n",
                         (unsigned long long)s.chunks_processed);
            std::fprintf(stderr, "  queue_depth        : %llu\n",
                         (unsigned long long)s.queue_depth);
            std::fprintf(stderr, "  queue_max_depth    : %llu\n",
                         (unsigned long long)s.queue_max_depth);
            const double audio_s = (double)s.audio_frames_total / SAMPLE_RATE;
            const double wall_s  = (double)s.inference_ns_total / 1e9;
            const double rtf = audio_s > 0.0 ? wall_s / audio_s : 0.0;
            std::fprintf(stderr, "  audio_seconds      : %.2f\n", audio_s);
            std::fprintf(stderr, "  inference_seconds  : %.2f\n", wall_s);
            std::fprintf(stderr, "  inference_rtf      : %.3f\n", rtf);
            std::fprintf(stderr, "  max_backlog_s      : %.1f\n",
                         (double)s.queue_max_depth * 0.050);
            std::fprintf(stderr, "  chunks_dropped     : %llu",
                         (unsigned long long)s.chunks_dropped);
            if (s.chunks_dropped == 0)
                std::fprintf(stderr, "  (nothing lost)");
            std::fprintf(stderr, "\n");
        }
    }

    slots.clear();
    g_slots_ptr = nullptr;

    pa_simple_flush(pa, nullptr);
    pa_simple_free(pa);
    if (g_notes) { std::fclose(g_notes); g_notes = nullptr; }
    if (g_verbose_log) { std::fclose(g_verbose_log); g_verbose_log = nullptr; }

    for (Pane *p : g_panes) {
        if (p->scroll_idle_id) g_source_remove(p->scroll_idle_id);
        delete p;
    }
    g_panes.clear();

    g_object_unref(app);
    std::fprintf(stderr, "done — transcript saved to %s\n", NOTES_FILE);
    return rc;
}