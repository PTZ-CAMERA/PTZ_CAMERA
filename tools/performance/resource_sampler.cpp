/**
 * [학습 안내] Linux 프로세스/스레드별 자원 수집
 * /proc stat/status의 누적 카운터 차이로 약 1초 구간 CPU와 문맥 전환을 구한다.
 * CPU 100%는 코어 하나다. RSS는 프로세스만 기록해 스레드별 중복 합산을 피한다.
 * 첫 샘플과 읽을 수 없는 항목은 -1이다. 순간 피크/전체 실행 누적값과 구별한다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {
/**
 * steady_clock 시간을 ns로 표현해 프로세스 이벤트와 자원 CSV를 같은 시간 축에서 비교한다.
 */
long long nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
struct Sample {
    unsigned long long ticks = 0, voluntary = 0, involuntary = 0;
    long long rssKb = -1;
};
/**
 * /proc stat의 괄호 뒤 필드와 status의 문맥 전환 카운터를 읽는다. task 이름 안의 공백 때문에 단순 전체 split을 하지 않는다.
 */
bool readSample(const std::string &path, Sample &sample) {
    std::ifstream file(path + "/stat");
    std::string line;
    if (!std::getline(file, line))
        return false;
    const auto end = line.rfind(')');
    if (end == std::string::npos)
        return false;
    std::istringstream input(line.substr(end + 2));
    std::string field;
    std::vector<std::string> fields;
    while (input >> field)
        fields.push_back(field);
    if (fields.size() < 22)
        return false;
    sample.ticks = std::stoull(fields[11]) + std::stoull(fields[12]);
    sample.rssKb = std::stoll(fields[21]) * sysconf(_SC_PAGESIZE) / 1024;
    std::ifstream status(path + "/status");
    while (std::getline(status, line)) {
        if (line.rfind("voluntary_ctxt_switches:", 0) == 0)
            sample.voluntary = std::stoull(line.substr(line.find(':') + 1));
        if (line.rfind("nonvoluntary_ctxt_switches:", 0) == 0)
            sample.involuntary = std::stoull(line.substr(line.find(':') + 1));
    }
    return true;
}
/**
 * 온도/클럭 sysfs 값을 읽는다. 경로가 없거나 읽지 못하면 -1을 유지한다.
 */
long long readNumber(const char *path) {
    std::ifstream file(path);
    long long value = -1;
    file >> value;
    return value;
}
struct Previous {
    Sample sample;
    long long time = 0;
};
} // namespace
/**
 * 실행 인자와 자원을 준비한 뒤 위 함수들을 호출한다. 정상/오류 종료를 반환값으로 구분하고 보유 자원을 종료 경로에서 정리한다.
 */
int main(int argc, char **argv) {
    try {
        int pid = 0, seconds = 30;
        std::string output;
        for (int i = 1; i < argc; i += 2) {
            if (i + 1 == argc)
                throw std::runtime_error("Every option needs a value");
            const std::string key = argv[i], value = argv[i + 1];
            if (key == "--pid")
                pid = std::stoi(value);
            else if (key == "--seconds")
                seconds = std::stoi(value);
            else if (key == "--output")
                output = value;
            else
                throw std::runtime_error("Unknown option: " + key);
        }
        if (pid <= 0 || seconds <= 0 || output.empty())
            throw std::runtime_error("Usage: --pid PID --seconds N --output CSV");
        std::ofstream file(output);
        if (!file)
            throw std::runtime_error("Cannot open resource CSV");
        file << "monotonic_ns,pid,tid,cpu_percent,rss_kb,threads,voluntary_delta,involuntary_delta,"
                "temp_mC,cpu0_kHz\n";
        std::map<int, Previous> previous;
        const auto begin = nowNs();
        while (nowNs() - begin < seconds * 1000000000LL) {
            const auto now = nowNs();
            const std::string proc = "/proc/" + std::to_string(pid);
            Sample process;
            if (!readSample(proc, process))
                break;
            std::vector<int> tids;
            std::error_code error;
            for (const auto &entry : std::filesystem::directory_iterator(proc + "/task", error))
                tids.push_back(std::stoi(entry.path().filename().string()));
            const auto temperature = readNumber("/sys/class/thermal/thermal_zone0/temp");
            const auto frequency =
                readNumber("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
            auto write = [&](int tid, const Sample &sample) {
                const auto old = previous.find(tid);
                double cpu = -1;
                long long voluntary = -1, involuntary = -1;
                if (old != previous.end()) {
                    const auto &last = old->second;
                    const double elapsed = (now - last.time) / 1e9;
                    if (elapsed > 0 && sample.ticks >= last.sample.ticks)
                        cpu = 100.0 * (sample.ticks - last.sample.ticks) / sysconf(_SC_CLK_TCK) /
                              elapsed;
                    if (sample.voluntary >= last.sample.voluntary)
                        voluntary = sample.voluntary - last.sample.voluntary;
                    if (sample.involuntary >= last.sample.involuntary)
                        involuntary = sample.involuntary - last.sample.involuntary;
                }
                if (tid == 0) {
                    voluntary = -1;
                    involuntary = -1;
                }
                file << now << ',' << pid << ',' << tid << ',' << cpu << ','
                     << (tid == 0 ? sample.rssKb : -1) << ',' << tids.size() << ',' << voluntary
                     << ',' << involuntary << ',' << temperature << ',' << frequency << '\n';
                previous[tid] = {sample, now};
            };
            // tid=0은 프로세스 전체 CPU/RSS. 스레드 RSS는 중복 합산을 막기 위해 -1.
            // 프로세스 status의 문맥 전환은 리더 스레드 값이므로 여기서는 -1로 기록한다.
            write(0, process);
            for (const auto tid : tids) {
                Sample thread;
                if (readSample(proc + "/task/" + std::to_string(tid), thread))
                    write(tid, thread);
            }
            // 프로세스 행의 문맥 전환은 해석하지 않고 각 스레드 행을 사용한다.
            file.flush();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
