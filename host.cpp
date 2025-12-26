#include "common/conn_iface.hpp"
#include "common/log.hpp"

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstring>
#include <poll.h>
#include <random>
#include <string>
#include <unistd.h>

static std::atomic<bool> g_stop{false};

static void on_sigint(int) { g_stop = true; }

static bool wait_sigusr1(pid_t& out_client_pid) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);

    // block SIGUSR1 so sigtimedwait works
    pthread_sigmask(SIG_BLOCK, &set, nullptr);

    timespec ts{};
    ts.tv_sec = 5;
    ts.tv_nsec = 0;

    siginfo_t info{};
    int sig = sigtimedwait(&set, &info, &ts);
    if (sig == SIGUSR1) {
        out_client_pid = info.si_pid;
        return true;
    }
    return false; // timeout or error -> caller can decide (we just loop)
}

static int wolf_roll_with_3s_input(std::mt19937& rng) {
    // 3 sec input window; if no input -> random
    pollfd p{};
    p.fd = STDIN_FILENO;
    p.events = POLLIN;

    int r = poll(&p, 1, 3000);
    std::uniform_int_distribution<int> dist(1, 100);

    if (r == 1) {
        std::string s;
        std::getline(std::cin, s);
        try {
            int v = std::stoi(s);
            if (v >= 1 && v <= 100) return v;
        } catch (...) {}
        return dist(rng);
    }
    return dist(rng);
}

int main() {
    std::signal(SIGINT, on_sigint);

    const std::string who = "HOST";
    pid_t host_pid = getpid();
    LOGI(who, "started; pid=" + std::to_string(host_pid));
    LOGI(who, "run client as: ./bin/client_<type> " + std::to_string(host_pid));

    ConnConfig cfg;
    cfg.is_host = true;
    cfg.create = true;
    cfg.host_pid = host_pid;

    auto conn = CreateConn(cfg);

    // Wait for handshake
    pid_t client_pid = 0;
    while (!g_stop.load()) {
        if (wait_sigusr1(client_pid)) {
            LOGI(who, "handshake: got SIGUSR1 from client pid=" + std::to_string(client_pid));
            break;
        }
        LOGI(who, "waiting for client handshake (SIGUSR1) ...");
    }
    if (g_stop.load()) {
        LOGW(who, "stopped before session start");
        return 0;
    }

    // Game: n=1 (one goat)
    std::random_device rd;
    std::mt19937 rng(rd());

    bool goat_alive = true;
    int consecutive_all_dead_rounds = 0;
    int round = 1;

    while (!g_stop.load()) {
        int wolf = wolf_roll_with_3s_input(rng);

        int32_t goat_num = 0;
        if (!conn->Read(&goat_num, sizeof(goat_num))) {
            LOGE(who, "failed to read goat number -> end");
            break;
        }

        int diff = std::abs((int)goat_num - wolf);

        int hidden = 0, caught = 0, dead_count = 0, resurrected = 0;

        if (goat_alive) {
            // hide if abs <= 70/n ; n=1 => 70
            if (diff <= 70) {
                hidden = 1;
            } else {
                goat_alive = false;
                caught = 1;
            }
        } else {
            // dead rolls 1..50 on client; resurrect if abs <= 20/n ; n=1 => 20
            if (diff <= 20) {
                goat_alive = true;
                resurrected = 1;
            }
        }

        dead_count = goat_alive ? 0 : 1;

        int32_t status = goat_alive ? GOAT_ALIVE : GOAT_DEAD;
        if (!conn->Write(&status, sizeof(status))) {
            LOGE(who, "failed to send status -> end");
            break;
        }

        // end condition: 2 rounds подряд все козлята мертвы (n=1 => goat_dead)
        if (!goat_alive) consecutive_all_dead_rounds++;
        else consecutive_all_dead_rounds = 0;

        LOGI(who,
             "ROUND " + std::to_string(round) +
             " | wolf=" + std::to_string(wolf) +
             " goat=" + std::to_string(goat_num) +
             " diff=" + std::to_string(diff) +
             " | hidden=" + std::to_string(hidden) +
             " caught=" + std::to_string(caught) +
             " dead=" + std::to_string(dead_count) +
             " resurrected=" + std::to_string(resurrected) +
             " | goat_state=" + std::string(goat_alive ? "ALIVE" : "DEAD"));

        if (consecutive_all_dead_rounds >= 2) {
            LOGI(who, "game over: goat dead for 2 consecutive rounds");
            int32_t end = GOAT_END;
            // попытка корректно завершить клиента
            (void)conn->Write(&end, sizeof(end));
            break;
        }

        round++;
    }

    LOGI(who, "finished");
    return 0;
}

