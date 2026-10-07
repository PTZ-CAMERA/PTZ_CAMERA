#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>

namespace ptz::perf {
// Pi 안의 프로세스/스레드를 동일한 monotonic 시간 축으로 비교한다.
inline long long nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline std::atomic<unsigned long long> sequence{0};
inline thread_local unsigned long long parentEvent = 0;

class CsvLog {
public:
    static CsvLog& instance() { static CsvLog log; return log; }
    bool enabled() const { return enabled_; }
    void flush() {
        if (!enabled_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        file_.flush();
    }
    void write(const char* event, unsigned long long id, unsigned long long parent,
               long long start, long long end, long long value1 = -1,
               long long value2 = -1, const std::string& detail = {}) {
        if (!enabled_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        file_ << start << ',' << end << ',' << getpid() << ',' << syscall(SYS_gettid)
              << ',' << id << ',' << parent << ',' << event << ','
              << (end - start) / 1000.0 << ',' << value1 << ',' << value2 << ",\"";
        for (char c : detail) {
            if (c == '"') file_ << '"';
            file_ << (c == '\n' || c == '\r' ? ' ' : c);
        }
        file_ << "\"\n";
        // 매 행 flush하지 않는다. 로그 I/O 비용도 시험 결과에 포함된다.
        if (end - lastFlush_ >= 1000000000LL) { file_.flush(); lastFlush_ = end; }
    }
private:
    CsvLog() {
        const char* path = std::getenv("PTZ_PERF_LOG");
        if (!path || !*path) return;
        std::string filename(path);
        const auto marker = filename.find("%p");
        if (marker != std::string::npos) filename.replace(marker, 2, std::to_string(getpid()));
        file_.open(filename, std::ios::out | std::ios::trunc);
        enabled_ = file_.is_open();
        if (!enabled_) { std::cerr << "Cannot open performance log: " << filename << '\n'; return; }
        file_ << "start_ns,end_ns,pid,tid,event_id,parent_id,event,duration_us,value1,value2,detail\n";
        lastFlush_ = nowNs();
    }
    bool enabled_ = false;
    std::ofstream file_;
    std::mutex mutex_;
    long long lastFlush_ = 0;
};

inline bool enabled() { return CsvLog::instance().enabled(); }
inline void record(const char* event, long long start, long long end,
                   long long value1 = -1, long long value2 = -1,
                   const std::string& detail = {}) {
    if (enabled()) CsvLog::instance().write(event, ++sequence, parentEvent,
                                           start, end, value1, value2, detail);
}
inline void instant(const char* event, long long value1 = -1, long long value2 = -1,
                    const std::string& detail = {}) {
    if (!enabled()) return;
    const auto now = nowNs();
    record(event, now, now, value1, value2, detail);
}

// command -> gpio_write, button_callback -> gpio_write를 parent_id로 연결한다.
class Scope {
public:
    explicit Scope(const char* event, std::string detail = {})
        : event_(event), detail_(std::move(detail)), active_(enabled()) {
        if (active_) { start_ = nowNs(); parent_ = parentEvent; id_ = ++sequence; parentEvent = id_; }
    }
    ~Scope() {
        if (active_) {
            const auto end = nowNs();
            parentEvent = parent_;
            CsvLog::instance().write(event_, id_, parent_, start_, end, -1, -1, detail_);
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    const char* event_;
    std::string detail_;
    bool active_;
    long long start_ = 0;
    unsigned long long parent_ = 0, id_ = 0;
};
} // namespace ptz::perf
