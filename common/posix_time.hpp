#pragma once
#include <ctime>

inline timespec make_abstimeout_sec(int seconds) {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += seconds;
    return ts;
}

