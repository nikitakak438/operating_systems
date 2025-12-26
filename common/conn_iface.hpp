#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

enum : int32_t {
    GOAT_DEAD  = 0,
    GOAT_ALIVE = 1,
    GOAT_END   = 2
};

struct ConnConfig {
    bool is_host = false;     // true: host side, false: client side
    bool create  = false;     // true in host (creates IPC), false in client (opens existing)
    pid_t host_pid = 0;       // used for naming keys/resources
};

class IConn {
public:
    virtual ~IConn() = default;
    virtual bool Write(const void* buf, size_t count) = 0; // send to peer
    virtual bool Read(void* buf, size_t count) = 0;        // receive from peer
};

// Реализуется В КАЖДОМ conn_*.cpp
std::unique_ptr<IConn> CreateConn(const ConnConfig& cfg);

