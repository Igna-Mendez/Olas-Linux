// config_test.cpp — tests the OlasConfig parser shipped inside the main source.
//
// The parser lives inline in src/olas-gtk-1.1.cpp so OLAS keeps a one-file
// build recipe. Rather than duplicate it here (which would let the copy drift
// from what ships), build-gtk.sh extracts the exact config block into a temp
// header and passes it as CFG_BLOCK, so this test compiles the shipped code.
//
// Run with:  ./build-gtk.sh cfgtest && ./tools/config_test <dir>
#include CFG_BLOCK

#include <cstdio>
#include <fstream>

static int fails = 0;
static void check(bool ok, const char *what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

int main(int argc, char **argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";

    // 1. The shipped config must load cleanly, with no warnings.
    std::vector<std::string> warn;
    OlasConfig c = OlasConfig::load(dir + "/olas-1.1.conf", warn);

    std::printf("shipped olas-1.1.conf: warnings=%zu\n", warn.size());
    for (const auto &w : warn) std::printf("  - %s\n", w.c_str());

    std::printf("\nchecks:\n");
    check(warn.empty(), "shipped config loads with no warnings");
    check(c.vad_threshold == 0.5, "vad_threshold");
    check(c.vad_max_segment_duration == 12, "vad_max_segment_duration");
    check(c.transcription_interval == 1.0, "transcription_interval");

    // 2. The removed domain-dictionary keys must warn, not be silently
    //    accepted, so an old config cannot look like it is still working.
    //    Sections are accepted and ignored, so a [bias.*] header alone is
    //    not a warning -- its keys are.
    {
        const char *path = "/tmp/olas_removed.conf";
        {
            std::ofstream f(path);
            f << "vad_threshold = 0.6\n"
                 "max_keyterms = 40\n"
                 "include = dict-en.conf\n"
                 "keyterms = Acme Corp\n"
                 "\n"
                 "[bias.en.medical]\n"
                 "keyterms = anamnesis\n"
                 "boost = 2.5\n";
        }
        std::vector<std::string> w2;
        OlasConfig c2 = OlasConfig::load(path, w2);

        std::printf("\nlegacy config: warnings=%zu\n", w2.size());
        for (const auto &w : w2) std::printf("  - %s\n", w.c_str());

        auto warned = [&](const char *needle) {
            for (const auto &w : w2)
                if (w.find(needle) != std::string::npos) return true;
            return false;
        };
        check(c2.vad_threshold == 0.6, "surviving key still parses");
        check(warned("max_keyterms"), "max_keyterms warns (removed)");
        check(warned("include"), "include warns (removed)");
        check(warned("keyterms"), "keyterms warns (removed)");
        check(warned("boost"), "boost warns (removed)");
    }

    // 3. Out-of-range values must be rejected with a warning, leaving the
    //    default in place, rather than silently clamped.
    {
        const char *path = "/tmp/olas_bad.conf";
        {
            std::ofstream f(path);
            f << "vad_threshold = 9.0\n"
                 "vad_max_segment_duration = 0\n"
                 "transcription_interval = 99\n";
        }
        std::vector<std::string> w3;
        OlasConfig c3 = OlasConfig::load(path, w3);
        check(c3.vad_threshold == 0.5, "bad vad_threshold keeps default");
        check(c3.vad_max_segment_duration == 15,
              "bad vad_max_segment_duration keeps default");
        check(c3.transcription_interval == 1.0,
              "bad transcription_interval keeps default");
        check(w3.size() == 3, "each bad value warns");
    }

    std::printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", fails);
    return fails ? 1 : 0;
}
