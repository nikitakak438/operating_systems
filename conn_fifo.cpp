#include "common/conn_iface.hpp"
#include "common/log.hpp"
#include "common/posix_time.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr int TIMEOUT_SEC = 5;

std::string fifo_path(pid_t host_pid) { return "/tmp/lab_fifo_" + std::to_string(host_pid); }
std::string sem_name_c2h(pid_t host_pid) { return "/lab_fifo_sem_" + std::to_string(host_pid) + "_c2h"; }
std::string sem_name_h2c(pid_t host_pid) { return "/lab_fifo_sem_" + std::to_string(host_pid) + "_h2c"; }

sem_t* sem_create_or_open(const std::string& name, unsigned init, bool create, const std::string& who) {
    if (create) {
        sem_unlink(name.c_str());
        sem_t* s = sem_open(name.c_str(), O_CREAT | O_EXCL, 0666, init);
        if (s != SEM_FAILED) return s;
        if (errno != EEXIST) {
            LOGE(who, "sem_open(create) failed: " + name + " err=" + std::string(strerror(errno)));
            return SEM_FAILED;
        }
    }
    sem_t* s = sem_open(name.c_str(), 0);
    if (s == SEM_FAILED) {
        LOGE(who, "sem_open(open) failed: " + name + " err=" + std::string(strerror(errno)));
    }
    return s;
}

bool sem_wait_5s(sem_t* s, const std::string& who, const std::string& what) {
    timespec ts = make_abstimeout_sec(TIMEOUT_SEC);
    if (sem_timedwait(s, &ts) == 0) return true;
    if (errno == ETIMEDOUT) LOGW(who, what + " timeout (5s) -> connection break");
    else LOGE(who, what + " sem_timedwait failed: err=" + std::string(strerror(errno)));
    return false;
}

bool poll_fd(int fd, short events, int timeout_ms, const std::string& who, const std::string& what) {
    pollfd p{};
    p.fd = fd;
    p.events = events;
    int r = poll(&p, 1, timeout_ms);
    if (r == 1) return true;
    if (r == 0) {
        LOGW(who, what + " poll timeout -> connection break");
    } else {
        LOGE(who, what + " poll failed: err=" + std::string(strerror(errno)));
    }
    return false;
}

bool write_full(int fd, const void* buf, size_t n, const std::string& who) {
    const char* p = (const char*)buf;
    size_t left = n;
    while (left > 0) {
        ssize_t w = ::write(fd, p, left);
        if (w > 0) { p += w; left -= (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        LOGE(who, "write failed: err=" + std::string(strerror(errno)));
        return false;
    }
    return true;
}

bool read_full(int fd, void* buf, size_t n, const std::string& who) {
    char* p = (char*)buf;
    size_t left = n;
    while (left > 0) {
        ssize_t r = ::read(fd, p, left);
        if (r > 0) { p += r; left -= (size_t)r; continue; }
        if (r == 0) {
            LOGW(who, "read EOF -> peer closed");
            return false;
        }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return false;
        LOGE(who, "read failed: err=" + std::string(strerror(errno)));
        return false;
    }
    return true;
}

int open_write_retry(const std::string& path, const std::string& who) {
    // open FIFO for write without blocking forever
    // if no reader -> ENXIO, retry in loop
    for (;;) {
        int fd = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
        if (fd >= 0) return fd;
        if (errno != ENXIO && errno != ENOENT) {
            LOGE(who, "open write failed: err=" + std::string(strerror(errno)));
            return -1;
        }
        // wait up to 5s per attempt, but repeat (host can wait client start)
        if (!poll_fd(STDIN_FILENO, 0, 5000, who, "open_write_retry wait")) {
            // poll on fd 0 with 0 events just to sleep-ish, but poll() with 0 events is not portable.
            // So: simple usleep instead:
        }
        usleep(100 * 1000);
    }
}
} // namespace

class ConnFifo final : public IConn {
public:
    explicit ConnFifo(const ConnConfig& cfg)
        : cfg_(cfg),
          who_(cfg.is_host ? "HOST(fifo)" : "CLIENT(fifo)") {

        path_ = fifo_path(cfg_.host_pid);

        if (cfg_.create) {
            ::unlink(path_.c_str());
            if (mkfifo(path_.c_str(), 0666) != 0) {
                LOGE(who_, "mkfifo failed: " + path_ + " err=" + std::string(strerror(errno)));
                ok_ = false;
                return;
            }
        }

        // Open read end first, nonblocking
        fd_r_ = ::open(path_.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd_r_ < 0) {
            LOGE(who_, "open read failed: " + path_ + " err=" + std::string(strerror(errno)));
            ok_ = false;
            return;
        }

        // Open write end with retry (wait until peer opens read end)
        fd_w_ = open_write_retry(path_, who_);
        if (fd_w_ < 0) {
            ok_ = false;
            return;
        }

        sem_in_  = sem_create_or_open(cfg_.is_host ? sem_name_c2h(cfg_.host_pid) : sem_name_h2c(cfg_.host_pid),
                                      0, cfg_.create, who_);
        sem_out_ = sem_create_or_open(cfg_.is_host ? sem_name_h2c(cfg_.host_pid) : sem_name_c2h(cfg_.host_pid),
                                      0, cfg_.create, who_);
        if (sem_in_ == SEM_FAILED || sem_out_ == SEM_FAILED) { ok_ = false; return; }

        LOGI(who_, "ready; fifo=" + path_);
    }

    bool Write(const void* buf, size_t count) override {
        if (!ok_ || count != sizeof(int32_t)) return false;

        // poll write ready 5s
        if (!poll_fd(fd_w_, POLLOUT, TIMEOUT_SEC * 1000, who_, "Write()")) return false;

        if (!write_full(fd_w_, buf, sizeof(int32_t), who_)) return false;

        if (sem_post(sem_out_) != 0) {
            LOGE(who_, "sem_post failed: err=" + std::string(strerror(errno)));
            return false;
        }
        return true;
    }

    bool Read(void* buf, size_t count) override {
        if (!ok_ || count != sizeof(int32_t)) return false;

        if (!sem_wait_5s(sem_in_, who_, "Read()")) return false;

        // poll read ready 5s
        if (!poll_fd(fd_r_, POLLIN, TIMEOUT_SEC * 1000, who_, "Read()")) return false;

        // read full int32
        if (!read_full(fd_r_, buf, sizeof(int32_t), who_)) return false;
        return true;
    }

    ~ConnFifo() override {
        if (fd_r_ >= 0) ::close(fd_r_);
        if (fd_w_ >= 0) ::close(fd_w_);

        if (sem_in_  && sem_in_  != SEM_FAILED) sem_close(sem_in_);
        if (sem_out_ && sem_out_ != SEM_FAILED) sem_close(sem_out_);

        if (cfg_.create) {
            sem_unlink(sem_name_c2h(cfg_.host_pid).c_str());
            sem_unlink(sem_name_h2c(cfg_.host_pid).c_str());
            ::unlink(path_.c_str()); // обязательное удаление FIFO файла
        }
        LOGI(who_, "closed");
    }

private:
    ConnConfig cfg_;
    std::string who_;
    bool ok_ = true;

    std::string path_;
    int fd_r_ = -1;
    int fd_w_ = -1;

    sem_t* sem_in_  = SEM_FAILED;
    sem_t* sem_out_ = SEM_FAILED;
};

std::unique_ptr<IConn> CreateConn(const ConnConfig& cfg) {
    return std::make_unique<ConnFifo>(cfg);
}

