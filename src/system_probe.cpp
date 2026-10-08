// system_probe.cpp — see system_probe.h.

#include "system_probe.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace olas {

namespace {

std::string trim(std::string s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Value after the first ':' on a /proc line, or empty.
std::string after_colon(const std::string &line) {
    const size_t c = line.find(':');
    return c == std::string::npos ? std::string() : trim(line.substr(c + 1));
}

CpuInfo probe_cpu() {
    CpuInfo c;

    std::ifstream f("/proc/cpuinfo");
    std::string line;
    int n = 0;
    std::string phys, core;
    std::vector<std::string> pairs;

    while (std::getline(f, line)) {
        if (line.rfind("processor", 0) == 0) {
            ++n;
        } else if (line.rfind("model name", 0) == 0) {
            if (c.model.empty()) c.model = after_colon(line);
        } else if (line.rfind("physical id", 0) == 0) {
            phys = after_colon(line);
        } else if (line.rfind("core id", 0) == 0) {
            core = after_colon(line);
        } else if (line.empty() && !phys.empty() && !core.empty()) {
            // A new CPU block ended; record the (package, core) pair.
            const std::string key = phys + "/" + core;
            bool seen = false;
            for (const auto &p : pairs) if (p == key) { seen = true; break; }
            if (!seen) pairs.push_back(key);
            phys.clear();
            core.clear();
        }
    }

    c.logical_cores = n > 0 ? n : 1;
    if (c.logical_cores < 1) {
        const long sc = sysconf(_SC_NPROCESSORS_ONLN);
        c.logical_cores = sc > 0 ? static_cast<int>(sc) : 1;
    }
    c.physical_cores = pairs.empty() ? c.logical_cores
                                     : static_cast<int>(pairs.size());
    c.has_smt = c.logical_cores > c.physical_cores;
    return c;
}

} // namespace

SystemInfo probe_system() {
    SystemInfo s;
    s.cpu = probe_cpu();

    std::ifstream f("/proc/meminfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("MemTotal:", 0) == 0)
            s.mem_total_kb = std::strtoull(line.c_str() + line.find(':') + 1,
                                           nullptr, 10);
        else if (line.rfind("MemAvailable:", 0) == 0)
            s.mem_avail_kb = std::strtoull(line.c_str() + line.find(':') + 1,
                                           nullptr, 10);
    }
    return s;
}

} // namespace olas
