// pipeline_bench.cpp — reproduce the OLAS capture->queue->transcriber pipeline
// and measure END-TO-END LAG and AUDIO LOSS for a simulated fluent lecture.
//
// This is not a micro-benchmark of the model. It runs the same structure the
// app uses:
//
//    producer thread  --(50 ms chunks)-->  AudioQueue  -->  worker thread
//                                                              |
//                                                              v
//                                                    moonshine::Transcriber
//                                                       (streaming)
//
// We timestamp every chunk as it enters the queue and every line as it is
// emitted, so we can compute how far behind the transcript is at any moment,
// and we count how many input samples never reached a decoded line.
//
// Modes:
//   --queue=block   : current OLAS v1.1 behaviour (block producer at 3600)
//   --queue=drop    : bounded drop-oldest (Windows behaviour)
//   --queue=<N>     : bounded cap of N chunks, drop-oldest
//
// Usage:
//   pipeline_bench <model_dir> <arch> <threads> <seconds> <queue-mode>

#include "moonshine-cpp.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static constexpr int SAMPLE_RATE = 16000;
static constexpr int CHUNK_MS = 50;
static constexpr int CHUNK_SAMPLES = SAMPLE_RATE / 1000 * CHUNK_MS;

using Clock = std::chrono::steady_clock;
using Chunk = std::vector<int16_t>;

// Read a 16 kHz mono s16 WAV. Only the canonical 44-byte-header PCM format is
// needed here (ffmpeg produces it). Returns empty on any problem.
static Chunk read_wav(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 44) { std::fclose(f); return {}; }

    Chunk out;
    out.resize((size_t)((sz - 44) / 2));
    std::fseek(f, 44, SEEK_SET);
    const size_t got =
        std::fread(out.data(), sizeof(int16_t), out.size(), f);
    std::fclose(f);
    out.resize(got);
    return out;
}

// ---------------------------------------------------------------------------
// Queue: two behaviours selected at runtime.
// ---------------------------------------------------------------------------
class Queue {
public:
    enum class Mode { Block, Drop };
    Queue(Mode m, size_t cap) : mode_(m), cap_(cap) {}

    // Returns false if a chunk had to be dropped.
    bool push(Chunk &&c, Clock::time_point tstamp) {
        std::unique_lock<std::mutex> lk(m_);
        if (closed_) return false;
        if (mode_ == Mode::Block) {
            cv_space_.wait(lk, [this] {
                return closed_ || q_.size() < cap_;
            });
            if (closed_) return false;
            q_.push_back({std::move(c), tstamp});
        } else {
            if (q_.size() >= cap_) {
                q_.pop_front();
                ++dropped_;
            }
            q_.push_back({std::move(c), tstamp});
        }
        cv_.notify_one();
        return true;
    }

    bool pop(Chunk &out, Clock::time_point &tstamp) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return closed_ || !q_.empty(); });
        if (q_.empty()) return false;
        out = std::move(q_.front().c);
        tstamp = q_.front().t;
        q_.pop_front();
        cv_space_.notify_one();
        return true;
    }

    size_t depth() { std::lock_guard<std::mutex> lk(m_); return q_.size(); }
    size_t dropped() { std::lock_guard<std::mutex> lk(m_); return dropped_; }
    void close() {
        std::lock_guard<std::mutex> lk(m_);
        closed_ = true; cv_.notify_all(); cv_space_.notify_all();
    }

private:
    struct Item { Chunk c; Clock::time_point t; };
    Mode mode_; size_t cap_;
    std::deque<Item> q_;
    std::mutex m_;
    std::condition_variable cv_, cv_space_;
    bool closed_ = false;
    size_t dropped_ = 0;
};

// ---------------------------------------------------------------------------
// Listener: records when each line was emitted, and how much audio it covered.
// ---------------------------------------------------------------------------
class BenchListener : public moonshine::TranscriptEventListener {
public:
    struct Line { double start_time; double duration; };
    std::mutex m;
    std::vector<Line> lines;
    size_t chars = 0;

    void onLineCompleted(const moonshine::LineCompleted &e) override {
        std::lock_guard<std::mutex> lk(m);
        Line l; l.start_time = e.line.startTime; l.duration = e.line.duration;
        lines.push_back(l);
        chars += e.line.text.size();
    }
};

int main(int argc, char **argv) {
    if (argc < 6) {
        std::fprintf(stderr,
            "usage: %s <model_dir> <arch> <threads> <file.wav> <queue-mode>\n"
            "  queue-mode: block | drop | <N>   (N = cap in chunks)\n", argv[0]);
        return 2;
    }
    const std::string model = argv[1];
    const int arch = std::atoi(argv[2]);
    const int threads = std::atoi(argv[3]);
    const std::string qmode = argv[5];
    // argv[4] is either a number of seconds or a path to a 16 kHz mono WAV.
    const std::string arg4 = argv[4];
    const bool from_file = arg4.find(".wav") != std::string::npos;
    double seconds = from_file ? 0.0 : std::atof(arg4.c_str());

    setenv("MOONSHINE_ORT_SINGLE_THREAD", std::to_string(threads).c_str(), 1);

    moonshine::ModelArch ma =
        arch == 5 ? moonshine::ModelArch::MEDIUM_STREAMING
                  : moonshine::ModelArch::SMALL_STREAMING;

    Queue::Mode mode = Queue::Mode::Drop;
    size_t cap = 120;
    if (qmode == "block") { mode = Queue::Mode::Block; cap = 3600; }
    else if (qmode != "drop") cap = (size_t)std::atol(qmode.c_str());

    Chunk pcm;
    if (from_file) {
        pcm = read_wav(arg4);
        if (pcm.empty()) {
            std::fprintf(stderr, "could not read wav: %s\n", arg4.c_str());
            return 1;
        }
        seconds = (double)pcm.size() / SAMPLE_RATE;
    } else {
        std::fprintf(stderr, "need a .wav path for the lecture test\n");
        return 1;
    }

    auto listener = std::make_unique<BenchListener>();
    std::unique_ptr<moonshine::Transcriber> t;
    try {
        moonshine::Options opts = {
            {"transcription_interval", "0.5"},
            {"return_audio_data",      "false"},
        };
        t = std::make_unique<moonshine::Transcriber>(model, ma, 0.5, "", opts);
        t->addListener(listener.get());
        t->start();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "load failed: %s\n", e.what());
        return 1;
    }

    Queue q(mode, cap);
    std::atomic<bool> done{false};
    std::atomic<size_t> consumed{0};
    std::atomic<double> max_lag{0.0};

    // Worker: pop and feed the transcriber, exactly like the app.
    std::thread worker([&] {
        Chunk c; Clock::time_point ts;
        std::vector<float> fb;
        while (q.pop(c, ts)) {
            fb.resize(c.size());
            for (size_t i = 0; i < c.size(); ++i)
                fb[i] = (float)c[i] / 32768.0f;
            t->addAudio(fb, SAMPLE_RATE);
            consumed += c.size();

            const double lag =
                std::chrono::duration<double>(Clock::now() - ts).count();
            double cur = max_lag.load();
            while (lag > cur && !max_lag.compare_exchange_weak(cur, lag)) {}
        }
    });

    // Producer: emit 50 ms chunks at real-time pace.
    const auto t_start = Clock::now();
    for (size_t off = 0; off + CHUNK_SAMPLES <= pcm.size();
         off += CHUNK_SAMPLES) {
        const auto due = t_start + std::chrono::milliseconds(
            (long long)((off / CHUNK_SAMPLES) * CHUNK_MS));
        std::this_thread::sleep_until(due);
        Chunk c(pcm.begin() + off, pcm.begin() + off + CHUNK_SAMPLES);
        q.push(std::move(c), Clock::now());
    }

    // Drain: give the worker time to chew through whatever backlog remains,
    // then force a final pass so trailing audio is decoded.
    {
        const auto drain_deadline =
            Clock::now() + std::chrono::seconds(120);
        while (q.depth() > 0 && Clock::now() < drain_deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    try { t->updateTranscription(); } catch (...) {}
    std::this_thread::sleep_for(std::chrono::seconds(3));
    q.close();
    worker.join();
    t->stop();

    const double total = seconds;
    const double consumed_s = (double)consumed.load() / SAMPLE_RATE;
    const double lost_s = total - consumed_s;

    size_t nlines = 0, nchars = 0;
    { std::lock_guard<std::mutex> lk(listener->m);
      nlines = listener->lines.size(); nchars = listener->chars; }

    std::printf(
        "qmode=%-6s arch=%d threads=%d audio=%.0fs "
        "consumed=%.1fs lost=%.1fs(%.1f%%) lines=%zu chars=%zu maxlag=%.1fs\n",
        qmode.c_str(), arch, threads, total,
        consumed_s, lost_s, 100.0 * lost_s / total,
        nlines, nchars, max_lag.load());
    return 0;
}
