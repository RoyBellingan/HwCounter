// perf_counters.hpp - minimal Linux perf_event self-monitoring counter group.
//
// Single header, no deps beyond libc + Linux headers. Counts hardware events
// for the *calling thread only*, in user mode, so it needs no root and no
// capabilities at kernel.perf_event_paranoid <= 2.
//
// Usage:
//     perf::counter_group g({PERF_COUNT_HW_CPU_CYCLES,
//                            PERF_COUNT_HW_INSTRUCTIONS});
//     g.reset(); g.enable();
//     work();
//     g.disable();
//     auto r = g.read();          // r.values[i], r.scale() for multiplexing

#pragma once

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

namespace perf {

inline long perf_event_open(perf_event_attr* attr, pid_t pid, int cpu,
                            int group_fd, unsigned long flags) noexcept {
    return ::syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

// Result of one read of the whole group.
struct reading {
    std::uint64_t time_enabled = 0;  // ns the group was enabled
    std::uint64_t time_running = 0;  // ns it was actually scheduled on the PMU
    std::vector<std::uint64_t> values;

    // The kernel multiplexes when more events are requested than the PMU has
    // physical slots. Raw counts are then under-reported; scale them back up.
    double scale() const noexcept {
        if (time_running == 0 || time_running == time_enabled) return 1.0;
        return double(time_enabled) / double(time_running);
    }
    double scaled(std::size_t i) const noexcept {
        return double(values[i]) * scale();
    }
    bool multiplexed() const noexcept {
        return time_running != time_enabled;
    }
};

class counter_group {
public:
    // Hardware events (PERF_TYPE_HARDWARE), first one becomes the group leader.
    explicit counter_group(std::initializer_list<perf_hw_id> events) {
        for (auto e : events)
            add(PERF_TYPE_HARDWARE, static_cast<std::uint64_t>(e));
    }

    // Generic form: any (type, config) pair, e.g. PERF_TYPE_HW_CACHE or
    // PERF_TYPE_RAW with a vendor-specific event select.
    counter_group() = default;

    counter_group(counter_group const&) = delete;
    counter_group& operator=(counter_group const&) = delete;

    ~counter_group() {
        for (auto it = fds_.rbegin(); it != fds_.rend(); ++it)
            if (*it != -1) ::close(*it);
    }

    void add(std::uint32_t type, std::uint64_t config) {
        perf_event_attr attr{};
        attr.size = sizeof(attr);
        attr.type = type;
        attr.config = config;
        attr.disabled = fds_.empty() ? 1 : 0;  // leader starts disabled
        attr.exclude_kernel = 1;   // required at paranoid == 2
        attr.exclude_hv = 1;
        attr.inherit = 0;          // this thread only; no children
        attr.read_format = PERF_FORMAT_GROUP |
                           PERF_FORMAT_TOTAL_TIME_ENABLED |
                           PERF_FORMAT_TOTAL_TIME_RUNNING;

        int leader = fds_.empty() ? -1 : fds_.front();
        // pid=0 -> calling thread, cpu=-1 -> whatever CPU it runs on.
        int fd = int(perf_event_open(&attr, 0, -1, leader, 0));
        if (fd == -1)
            throw std::runtime_error("perf_event_open(type=" +
                std::to_string(type) + ", config=0x" + hex(config) +
                ") failed: " + std::strerror(errno));
        fds_.push_back(fd);
    }

    std::size_t size() const noexcept { return fds_.size(); }

    void reset()   { ioctl_group(PERF_EVENT_IOC_RESET); }
    void enable()  { ioctl_group(PERF_EVENT_IOC_ENABLE); }
    void disable() { ioctl_group(PERF_EVENT_IOC_DISABLE); }

    reading read() const {
        if (fds_.empty()) throw std::runtime_error("read from empty perf group");
        // Group layout: nr, time_enabled, time_running, value[nr]
        std::vector<std::uint64_t> buf(3 + fds_.size());
        ssize_t n = ::read(fds_.front(), buf.data(), buf.size() * sizeof(buf[0]));
        if (n < 0)
            throw std::runtime_error(std::string("read from perf group failed: ") +
                                     std::strerror(errno));
        if (n != ssize_t(buf.size() * sizeof(buf[0])))
            throw std::runtime_error("short read from perf group");
        if (buf[0] != fds_.size())
            throw std::runtime_error("perf group returned " + std::to_string(buf[0]) +
                                     " values, expected " + std::to_string(fds_.size()));

        reading r;
        r.time_enabled = buf[1];
        r.time_running = buf[2];
        r.values.assign(buf.begin() + 3, buf.end());
        return r;
    }

private:
    // A failed ENABLE would otherwise read back as zeros, which a report
    // cannot tell from real data. Fail loudly instead.
    void ioctl_group(unsigned long req) {
        if (fds_.empty()) return;
        if (::ioctl(fds_.front(), req, PERF_IOC_FLAG_GROUP) == -1)
            throw std::runtime_error("perf ioctl(0x" + hex(req) + ") failed: " +
                                     std::strerror(errno));
    }
    static std::string hex(std::uint64_t v) {
        char b[32]; std::snprintf(b, sizeof b, "%llx", (unsigned long long)v);
        return b;
    }

    std::vector<int> fds_;
};

}  // namespace perf
