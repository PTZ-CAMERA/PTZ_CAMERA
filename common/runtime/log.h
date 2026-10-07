#pragma once
#include <chrono>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>
namespace edge {
// 프로세스 안의 여러 thread가 한 로그 줄을 섞지 않게 한다. frame마다 호출하지 않는다.
inline void log(const char* module,const std::string& message) {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    const auto now=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm time{}; localtime_r(&now,&time);
    char stamp[16]; std::strftime(stamp,sizeof(stamp),"%H:%M:%S",&time);
    std::cout << '[' << stamp << "][" << module << "] " << message << '\n' << std::flush;
}
} // namespace edge
