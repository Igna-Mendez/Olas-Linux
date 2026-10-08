// system_probe.h — CPU and memory detection for OLAS 1.1.
//
// Read-only kernel queries; nothing here links a vendor SDK or opens a device
// node. There is no GPU path: Moonshine's shipped library exposes no usable
// non-CPU execution provider (see resource_plan.h).

#ifndef OLAS_SYSTEM_PROBE_H
#define OLAS_SYSTEM_PROBE_H

#include <cstdint>
#include <string>

namespace olas {

struct CpuInfo {
    int logical_cores  = 1;
    int physical_cores = 1;
    bool has_smt       = false;
    std::string model;           // from /proc/cpuinfo, may be empty
};

struct SystemInfo {
    CpuInfo cpu;
    uint64_t mem_total_kb = 0;
    uint64_t mem_avail_kb = 0;
};

// Reads everything in one shot. Never throws.
SystemInfo probe_system();

} // namespace olas

#endif // OLAS_SYSTEM_PROBE_H
