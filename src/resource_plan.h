// resource_plan.h — turn a SystemInfo into an inference plan.
//
// Records the measurements behind the defaults so they are not re-derived:
//
//   Medium Streaming, MOONSHINE_ORT_SINGLE_THREAD set -> RTF 0.72
//   Medium Streaming, variable unset (ORT sizes pool) -> RTF 0.42
//
// RTF is decode_wall / audio_duration (below 1.0 keeps up with a live mic).
// The variable is a boolean, not a count: setting it to 1, 2, 4 or 8 all
// yield exactly one worker thread, so it cannot request N threads. The only
// real choice is set (one thread, lowest CPU) or unset (ORT sizes its own
// pool), and affinity is what bounds the pool. Four cores measured fastest;
// six was worse.
//
// There is no GPU path: libmoonshine.so parses only cpu, coreml and nnapi,
// and on Linux none of those engage a GPU. See README for the A/B runs.

#ifndef OLAS_RESOURCE_PLAN_H
#define OLAS_RESOURCE_PLAN_H

#include "system_probe.h"

#include <string>
#include <vector>

namespace olas {

struct ResourcePlan {
    int  models         = 1;    // concurrent Transcribers
    int  affinity_cores = 0;    // 0 = do not pin
    bool pin_affinity   = true;

    // True: export MOONSHINE_ORT_SINGLE_THREAD=1 (lowest CPU, slower decode).
    // False (default): leave it unset and bound ORT with affinity_cores.
    bool force_single_thread = false;

    // Per-model core budget. Each transcriber's ORT session is created while
    // the calling thread is restricted to its own core set, so the models do
    // not contend for one shared pool. Measured with Medium+Small concurrently
    // on 4 cores: shared mask -> slowest model RTF ~0.95; split 3+1 -> ~0.81.
    // Empty means every model shares all of affinity_cores.
    std::vector<int> model_cores;

    std::string rationale;      // one-line summary for the startup banner
};

// `archs` is the model architecture number per transcriber, used to give the
// heavier model the larger share of cores. `force_threads` / `force_cores`
// come from OLAS_THREADS / OLAS_CPU_CORES, or -1 for auto.
ResourcePlan plan_resources(const SystemInfo &sys,
                            int models,
                            const std::vector<int> &archs,
                            int force_threads,
                            int force_cores);

} // namespace olas

#endif // OLAS_RESOURCE_PLAN_H
