#include "common/conn_iface.hpp"
#include "common/log.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <random>
#include <string>
#include <unistd.h>
#include <cerrno>
#include <cstring>


static std::atomic<bool> g_stop{false};
static void on_sigint(int) { g_stop = true; }

int main(int argc, char** argv) {
    std::signal(SIGINT, on_sigint);

    const std::string who = "CLIENT";
    if (argc < 2) {
        LOGE(who, "usage: client_<type> <host_pid>");
        return 2;
    }

    pid_t host_pid = (pid_t)std::strtol(argv[1], nullptr, 10);
    if (host_pid <= 1) {
        LOGE(who, "bad host_pid");
        return 2;
    }

    // handshake: send SIGUSR1 to host pid
    if (kill(host_pid, SIGUSR1) != 0) {
        LOGE(who, "handshake kill(SIGUSR1) failed: err=" + std::string(strerror(errno)));
        return 2;
    }
    LOGI(who, "sent SIGUSR1 to host pid=" + std::to_string(host_pid));

    ConnConfig cfg;
    cfg.is_host = false;
    cfg.create = false;
    cfg.host_pid = host_pid;

    auto conn = CreateConn(cfg);

    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<int> alive_dist(1, 100);
    std::uniform_int_distribution<int> dead_dist(1, 50);

    bool goat_alive = true;
    int round = 1;

    while (!g_stop.load()) {
        int32_t goat_num = goat_alive ? (int32_t)alive_dist(rng) : (int32_t)dead_dist(rng);

        if (!conn->Write(&goat_num, sizeof(goat_num))) {
            LOGE(who, "write failed -> exit");
            break;
        }

        int32_t status = GOAT_DEAD;
        if (!conn->Read(&status, sizeof(status))) {
            LOGE(who, "read status failed -> exit");
            break;
        }

        if (status == GOAT_END) {
            LOGI(who, "received GOAT_END -> exit");
            break;
        }

        goat_alive = (status == GOAT_ALIVE);
        LOGI(who,
             "ROUND " + std::to_string(round) +
             " sent=" + std::to_string(goat_num) +
             " received_status=" + std::string(goat_alive ? "ALIVE" : "DEAD"));

        round++;
    }

    LOGI(who, "finished");
    return 0;
}

