// thread_bench.cpp — measure real decode throughput of the Moonshine streaming
// models on real speech, as a function of MOONSHINE_ORT_SINGLE_THREAD.
//
// Unlike pipeline_bench this does NOT pace to real time. It feeds a WAV as
// fast as the model will accept it and measures how long the decode takes.
//
//   RTF = decode_wall / audio_duration
//
// RTF < 1 means the model can keep up with a live microphone on this machine;
// RTF > 1 means it cannot and any live use will fall behind.
//
// Usage: thread_bench <model_dir> <arch> <threads> <file.wav>

#include "moonshine-cpp.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

static constexpr int SAMPLE_RATE = 16000;
static constexpr int CHUNK_MS = 50;
static constexpr int CHUNK_SAMPLES = SAMPLE_RATE / 1000 * CHUNK_MS;

static std::vector<int16_t> read_wav(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 44) { std::fclose(f); return {}; }
    std::vector<int16_t> out((size_t)((sz - 44) / 2));
    std::fseek(f, 44, SEEK_SET);
    const size_t got = std::fread(out.data(), sizeof(int16_t), out.size(), f);
    std::fclose(f);
    out.resize(got);
    return out;
}

class Counter : public moonshine::TranscriptEventListener {
public:
    size_t lines = 0, chars = 0;
    void onLineCompleted(const moonshine::LineCompleted &e) override {
        ++lines; chars += e.line.text.size();
    }
};

int main(int argc, char **argv) {
    if (argc < 5) {
        std::fprintf(stderr,
            "usage: %s <model_dir> <arch> <threads> <file.wav>\n", argv[0]);
        return 2;
    }
    const std::string model = argv[1];
    const int arch = std::atoi(argv[2]);
    const int threads = std::atoi(argv[3]);

    setenv("MOONSHINE_ORT_SINGLE_THREAD", std::to_string(threads).c_str(), 1);

    moonshine::ModelArch ma =
        arch == 5 ? moonshine::ModelArch::MEDIUM_STREAMING
                  : moonshine::ModelArch::SMALL_STREAMING;

    std::vector<int16_t> pcm = read_wav(argv[4]);
    if (pcm.empty()) { std::fprintf(stderr, "cannot read %s\n", argv[4]); return 1; }
    const double audio_s = (double)pcm.size() / SAMPLE_RATE;

    Counter counter;
    std::unique_ptr<moonshine::Transcriber> t;
    try {
        moonshine::Options opts = {
            {"transcription_interval", "0.5"},
            {"return_audio_data",      "false"},
        };
        t = std::make_unique<moonshine::Transcriber>(model, ma, 0.5, "", opts);
        t->addListener(&counter);
        t->start();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "load failed: %s\n", e.what());
        return 1;
    }

    // Feed everything as fast as possible, then drain.
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<float> fb(CHUNK_SAMPLES);
    for (size_t off = 0; off + CHUNK_SAMPLES <= pcm.size();
         off += CHUNK_SAMPLES) {
        for (int i = 0; i < CHUNK_SAMPLES; ++i)
            fb[i] = (float)pcm[off + i] / 32768.0f;
        t->addAudio(fb, SAMPLE_RATE);
    }
    try { t->updateTranscription(); } catch (...) {}
    t->stop();
    const double wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();

    std::printf(
        "arch=%d threads=%d audio=%.1fs decode=%.2fs RTF=%.3f lines=%zu chars=%zu %s\n",
        arch, threads, audio_s, wall, wall / audio_s,
        counter.lines, counter.chars,
        (wall / audio_s) < 1.0 ? "REALTIME-OK" : "TOO-SLOW");
    return 0;
}
