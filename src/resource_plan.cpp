// resource_plan.cpp — see resource_plan.h.

#include "resource_plan.h"

#include <algorithm>
#include <sstream>

namespace olas {

namespace {

// Mirrors the ARCH_* constants in olas-gtk-1.1.cpp. "Light" means Small
// Streaming or smaller (0=Tiny, 2=TinyStreaming, 4=SmallStreaming).
constexpr int ARCH_SMALL_STREAMING = 4;

// Total cores the process may use, by machine size. An affinity budget, not a
// thread count: ONNX Runtime sizes its own pool from the cores it can see.
// Four measured best on a 12-thread box; fewer on small machines so two models
// and the capture thread still fit.
int total_affinity_cores(int logical_cores) {
    if (logical_cores <= 2)  return 1;
    if (logical_cores <= 4)  return 2;
    if (logical_cores <= 8)  return 3;
    return 4;
}

// Split the core budget between the models so their ORT pools do not contend.
//
// Measured with Medium+Small over one shared 4-core mask, the slowest model sat
// at RTF ~0.95 — close enough to 1.0 to be fragile. Separate core sets drop it
// to ~0.81 on the same audio.
//
// Rules:
//   * A single model takes the whole budget.
//   * Every model Small or lighter: one core each. Small runs well single-
//     threaded (RTF ~0.46), so two of them stay in a deliberately minimal
//     footprint instead of claiming the machine's whole budget.
//   * Mixed: one core each to start, remainder to the heaviest arch.
void split_cores(int total, const std::vector<int> &archs,
                 std::vector<int> &out) {
    const int n = static_cast<int>(archs.size());
    out.assign(n, 0);
    if (n <= 0) return;

    if (n == 1) { out[0] = total; return; }

    bool all_light = true;
    for (int a : archs) if (a > ARCH_SMALL_STREAMING) all_light = false;

    if (all_light) {
        for (int i = 0; i < n; ++i) out[i] = 1;
        return;
    }

    if (total <= 1) {
        for (int i = 0; i < n; ++i) out[i] = 1;
        return;
    }

    int heaviest = 0;
    for (int i = 1; i < n; ++i)
        if (archs[i] > archs[heaviest]) heaviest = i;

    for (int i = 0; i < n; ++i) out[i] = 1;   // every model keeps at least one
    out[heaviest] += total - n;               // the rest goes to the slow one
}

} // namespace

ResourcePlan plan_resources(const SystemInfo &sys,
                            int models,
                            const std::vector<int> &archs,
                            int force_threads,
                            int force_cores) {
    ResourcePlan p;
    p.models = std::max(1, models);

    const int logical = std::max(1, sys.cpu.logical_cores);

    p.force_single_thread = (force_threads >= 1);
    p.affinity_cores = total_affinity_cores(logical);

    // All-Small: keep the whole process to one core per model rather than the
    // machine's full budget, so the light pair stays genuinely light.
    bool all_light = !archs.empty();
    for (int a : archs) if (a > ARCH_SMALL_STREAMING) all_light = false;
    if (all_light && p.models > 1)
        p.affinity_cores = std::min(p.affinity_cores, p.models);

    if (force_cores == 0) {
        p.pin_affinity = false;
        p.affinity_cores = 0;
    } else if (force_cores > 0) {
        p.affinity_cores = std::min(force_cores, logical);
    }

    if (p.pin_affinity && p.affinity_cores > 0)
        split_cores(p.affinity_cores, archs, p.model_cores);

    std::ostringstream os;
    os << logical << " logical/" << sys.cpu.physical_cores << " physical cores";
    if (sys.cpu.has_smt) os << " (SMT)";
    os << "; " << p.models << " model(s); "
       << (p.force_single_thread ? "single-threaded (OLAS_THREADS)"
                                 : "ORT-managed threads");
    if (p.pin_affinity && !p.model_cores.empty()) {
        os << "; cores";
        for (size_t i = 0; i < p.model_cores.size(); ++i)
            os << (i ? "+" : " ") << p.model_cores[i];
    } else if (p.pin_affinity) {
        os << "; affinity " << p.affinity_cores << " core(s)";
    } else {
        os << "; affinity disabled";
    }
    p.rationale = os.str();

    return p;
}

} // namespace olas
