/**
 * [학습 안내] 원본 CSV를 성능 판단용 Markdown으로 요약
 * monotonic 시간 축의 준비 구간을 제외하고 이벤트/자원 데이터를 같은 구간에서 집계한다.
 * 평균, nearest-rank p95, 최대와 샘플 수를 기록한다. 부모 ID로 명령/버튼과 GPIO 호출을 연결한다.
 * GPIO 반환 시간은 실제 모터 도착이 아니다. 자료가 없는 지표는 만들지 않는다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {
/**
 * 따옴표와 escaped quote를 고려해 CSV를 필드로 나눈다. detail에 쉼표가 있어도 열이 밀리지 않게 한다.
 */
std::vector<std::string> csv(const std::string &line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
                field += '"';
                ++i;
            } else
                quoted = !quoted;
        } else if (c == ',' && !quoted) {
            fields.push_back(field);
            field.clear();
        } else
            field += c;
    }
    fields.push_back(field);
    return fields;
}
/**
 * 정렬한 샘플의 평균/nearest-rank p95/최대를 출력한다. 샘플 수를 함께 남겨 적은 시험을 과대 해석하지 않게 한다.
 */
void stats(const std::string &name, std::vector<double> values) {
    if (values.empty())
        return;
    std::sort(values.begin(), values.end());
    double total = 0;
    for (double value : values)
        total += value;
    const auto index = static_cast<std::size_t>(std::ceil(values.size() * .95)) - 1;
    std::cout << "| " << name << " | " << values.size() << " | " << total / values.size() << " | "
              << values[index] << " | " << values.back() << " |\n";
}
} // namespace
/**
 * 실행 인자와 자원을 준비한 뒤 위 함수들을 호출한다. 정상/오류 종료를 반환값으로 구분하고 보유 자원을 종료 경로에서 정리한다.
 */
int main(int argc, char **argv) {
    try {
        if (argc < 3 || argc > 4)
            throw std::runtime_error("Usage: summarize EVENTS.csv RESOURCES.csv [WARMUP_SECONDS]");
        const double warmup = argc == 4 ? std::stod(argv[3]) : 5;
        if (warmup < 0)
            throw std::runtime_error("Warmup must be >= 0");
        std::ifstream events(argv[1]), resources(argv[2]);
        if (!events || !resources)
            throw std::runtime_error("Cannot open input CSV");
        std::string line;
        std::getline(events, line);
        struct Event {
            double start, end, duration, v1, v2;
            std::string type, detail;
            unsigned long long id, parent;
        };
        std::vector<Event> rows;
        double begin = -1, finish = -1;
        while (std::getline(events, line)) {
            const auto f = csv(line);
            if (f.size() != 11)
                continue;
            Event e{std::stod(f[0]), std::stod(f[1]),   std::stod(f[7]),
                    std::stod(f[8]), std::stod(f[9]),   f[6],
                    f[10],           std::stoull(f[4]), std::stoull(f[5])};
            rows.push_back(e);
            if (begin < 0 || e.start < begin)
                begin = e.start;
            finish = std::max(finish, e.end);
            if (e.type == "trial_start")
                begin = e.start;
        }
        if (rows.empty())
            throw std::runtime_error("No events recorded");
        const double cutoff = begin + warmup * 1e9;
        const double duration = (finish - cutoff) / 1e9;
        if (duration <= 0)
            throw std::runtime_error("Run is shorter than warmup");
        std::map<std::string, std::vector<double>> timing;
        std::map<std::string, unsigned long long> counts;
        std::map<unsigned long long, Event> parents;
        for (const auto &e : rows)
            parents.emplace(e.id, e);
        for (const auto &e : rows) {
            if (e.start < cutoff)
                continue;
            ++counts[e.type];
            if (e.duration > 0)
                timing[e.type + " duration (ms)"].push_back(e.duration / 1000.0);
            if (e.type == "inference") {
                if (e.v1 >= 0)
                    timing["inference input age (ms)"].push_back(e.v1);
                if (e.v2 >= 0)
                    timing["inference result age (ms)"].push_back(e.v2);
            }
            if (e.type == "queue_state") {
                timing[e.detail + " depth (buffers)"].push_back(e.v1);
                timing[e.detail + " queued time (ms)"].push_back(e.v2);
            }
            if (e.type == "gpio_write" && parents.count(e.parent)) {
                const auto &parent = parents.at(e.parent);
                timing[parent.type + " start -> GPIO return (ms)"].push_back(
                    (e.end - parent.start) / 1e6);
            }
        }
        std::cout
            << std::fixed << std::setprecision(2)
            << "# Performance summary\n\nWarmup excluded: " << warmup
            << " s. Observed window: " << duration << " s.\n\n"
            << "| Metric | Samples | Mean | p95 | Max |\n| --- | ---: | ---: | ---: | ---: |\n";
        for (const auto &[name, values] : timing)
            stats(name, values);
        std::getline(resources, line);
        std::vector<double> cpu, rss, temp, threads;
        std::map<int, std::vector<double>> threadCpu;
        unsigned long long voluntary = 0, involuntary = 0;
        while (std::getline(resources, line)) {
            const auto f = csv(line);
            if (f.size() != 10)
                continue;
            const auto time = std::stod(f[0]);
            if (time < cutoff || time > finish)
                continue;
            const int tid = std::stoi(f[2]);
            const double usage = std::stod(f[3]);
            if (tid == 0) {
                if (usage >= 0)
                    cpu.push_back(usage);
                rss.push_back(std::stod(f[4]) / 1024.0);
                threads.push_back(std::stod(f[5]));
                if (std::stod(f[8]) >= 0)
                    temp.push_back(std::stod(f[8]) / 1000.0);
            } else {
                if (usage >= 0)
                    threadCpu[tid].push_back(usage);
                if (std::stoll(f[6]) >= 0)
                    voluntary += std::stoull(f[6]);
                if (std::stoll(f[7]) >= 0)
                    involuntary += std::stoull(f[7]);
            }
        }
        stats("process CPU (%; 100 = one core)", cpu);
        stats("process RSS (MiB)", rss);
        stats("CPU temperature (C)", temp);
        stats("OS thread count", threads);
        std::cout << "\n| Event | Count | Events/s |\n| --- | ---: | ---: |\n";
        for (const auto &[name, count] : counts)
            std::cout << "| " << name << " | " << count << " | " << count / duration << " |\n";
        std::cout << "\n| Thread ID | Samples | Mean CPU (%) | p95 | Max |\n| --- | ---: | ---: | "
                     "---: | ---: |\n";
        for (const auto &[tid, values] : threadCpu)
            stats(std::to_string(tid), values);
        std::cout << "\nObserved thread context-switch deltas: voluntary=" << voluntary
                  << ", involuntary=" << involuntary << ".\n\n"
                  << "These measurements exclude Qt playback and physical servo arrival.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
