// bench_tune.cpp — measure the effect of Moonshine transcriber options on RTF,
// so we can find a configuration that keeps Medium Streaming under real time
// without losing transcript content.
//
// Tests combinations of:
//   transcription_interval   (0.1..5.0; higher = fewer re-decodes)
//   vad_max_segment_duration (seconds; higher = fewer forced breaks)
//   decode_incomplete_lines  (false = don't re-decode a growing line)
//   use_speculative_decoding (false = no speculative re-decode)
//
// Usage: bench_tune <model_dir> <arch> <wav16k> <threads>

#include "moonshine-cpp.h"

#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

static constexpr int SAMPLE_RATE = 16000;
static constexpr int CHUNK_MS = 50;
static constexpr int CHUNK_SAMPLES = SAMPLE_RATE / 1000 * CHUNK_MS;

static std::vector<int16_t> read_wav16(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    if (raw.size() < 44) return {};
    auto rd32 = [&](size_t o) {
        return (uint32_t)(uint8_t)raw[o] | ((uint32_t)(uint8_t)raw[o+1] << 8) |
               ((uint32_t)(uint8_t)raw[o+2] << 16) |
               ((uint32_t)(uint8_t)raw[o+3] << 24);
    };
    auto rd16 = [&](size_t o) {
        return (uint16_t)((uint8_t)raw[o] | ((uint8_t)raw[o+1] << 8));
    };
    if (std::string(raw.data(), 4) != "RIFF") return {};
    size_t pos = 12; const char *dp = nullptr; size_t dl = 0;
    while (pos + 8 <= raw.size()) {
        const std::string id(raw.data() + pos, 4);
        const uint32_t sz = rd32(pos + 4);
        const size_t body = pos + 8;
        if (body + sz > raw.size()) break;
        if (id == "data") { dp = raw.data() + body; dl = sz; }
        if (id == "fmt ") {
            uint16_t ch = rd16(body + 2); uint32_t rt = rd32(body + 4);
            uint16_t b = rd16(body + 14);
            if (ch != 1 || rt != (uint32_t)SAMPLE_RATE || b != 16) {
                std::fprintf(stderr, "need 16k mono s16\n"); return {};
            }
        }
        pos = body + sz + (sz & 1);
    }
    if (!dp || !dl) return {};
    std::vector<int16_t> out(dl / 2);
    std::memcpy(out.data(), dp, (dl / 2) * 2);
    return out;
}

struct Collect : public moonshine::TranscriptEventListener {
    std::string text; int finals = 0;
    void onLineCompleted(const moonshine::LineCompleted &e) override {
        if (!text.empty()) text += ' ';
        text += e.line.text;
        ++finals;
    }
};

int main(int argc, char **argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s <model_dir> <arch> <wav16k> <threads>\n",
                     argv[0]);
        return 2;
    }
    const std::string model = argv[1];
    const int arch = std::atoi(argv[2]);
    const std::string wav = argv[3];
    const int threads = std::atoi(argv[4]);

    setenv("MOONSHINE_ORT_SINGLE_THREAD", std::to_string(threads).c_str(), 1);

    const moonshine::ModelArch ma =
        arch == 4 ? moonshine::ModelArch::SMALL_STREAMING
                  : moonshine::ModelArch::MEDIUM_STREAMING;

    auto pcm = read_wav16(wav);
    if (pcm.empty()) return 1;
    const double audio_s = (double)pcm.size() / SAMPLE_RATE;

    struct Cfg { const char *name; const char *interval; const char *maxseg;
                 const char *decode_incomplete; const char *spec; };
    const Cfg cfgs[] = {
        {"baseline(0.5,12,-,-)", "0.5", "12", "", ""},
        {"interval=1.0        ", "1.0", "12", "", ""},
        {"interval=2.0        ", "2.0", "12", "", ""},
        {"maxseg=30           ", "0.5", "30", "", ""},
        {"no_decode_incomplete", "0.5", "12", "false", ""},
        {"no_speculative      ", "0.5", "12", "", "false"},
        {"interval=1.0+no_dec_inc", "1.0", "12", "false", ""},
        {"interval=2.0+maxseg30", "2.0", "30", "", ""},
    };

    std::printf("%-24s  %6s  %6s  %5s  %s\n",
                "config", "wall", "RTF", "lines", "text");
    for (const auto &c : cfgs) {
        moonshine::Options opts = {
            {"transcription_interval", c.interval},
            {"return_audio_data",      "false"},
            {"vad_max_segment_duration", c.maxseg},
        };
        if (*c.decode_incomplete) opts.emplace_back("decode_incomplete_lines",
                                                     c.decode_incomplete);
        if (*c.spec) opts.emplace_back("use_speculative_decoding", c.spec);

        Collect col;
        std::unique_ptr<moonshine::Transcriber> t;
        try {
            t = std::make_unique<moonshine::Transcriber>(
                model, ma, std::atof(c.interval), "", opts);
            t->addListener(&col);
            t->start();
        } catch (const std::exception &e) {
            std::printf("%-24s  load failed: %s\n", c.name, e.what());
            continue;
        }

        // warm-up
        { std::vector<float> z(CHUNK_SAMPLES, 0.f); t->addAudio(z, SAMPLE_RATE); }

        const auto t0 = std::chrono::steady_clock::now();
        std::vector<float> fb(CHUNK_SAMPLES);
        for (size_t off = 0; off + CHUNK_SAMPLES <= pcm.size();
             off += CHUNK_SAMPLES) {
            for (int i = 0; i < CHUNK_SAMPLES; ++i)
                fb[i] = (float)pcm[off + i] / 32768.0f;
            t->addAudio(fb, SAMPLE_RATE);
        }
        try { t->updateTranscription(); } catch (...) {}
        const double wall = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        t->stop();

        std::string preview = col.text.substr(0, 70);
        std::printf("%-24s  %6.2f  %6.3f  %5d  %.70s\n",
                    c.name, wall, wall / audio_s, col.finals, preview.c_str());
    }
    return 0;
}
