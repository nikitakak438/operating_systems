#pragma once
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

inline std::string now_ts() {
    using namespace std::chrono;
    auto tp = system_clock::now();
    std::time_t t = system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%F %T");
    return oss.str();
}

inline void log_msg(const std::string& level, const std::string& who, const std::string& msg) {
    std::cerr << "[" << now_ts() << "]"
              << " pid=" << getpid()
              << " " << who
              << " [" << level << "] "
              << msg << "\n";
}

#define LOGI(who, msg) log_msg("INFO",  who, msg)
#define LOGW(who, msg) log_msg("WARN",  who, msg)
#define LOGE(who, msg) log_msg("ERROR", who, msg)

