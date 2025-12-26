#include "common/conn_iface.hpp"
#include "common/log.hpp"
#include "common/posix_time.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr int TIMEOUT_SEC = 5;

struct SharedSeg {
    int32_t goat_number;
    int32_t goat_status;
};

std::string keyfile_path(pid_t host_pid) {
    return "/tmp/lab_seg_key_" + std::to_string(host_pid);
}

std::string sem_name_c2h(pid_t host_pid) { return "/lab_seg_" + std::to_string(host_pid) + "_c2h"; }
std::string sem_name_h2c(pid_t host_pid) { return "/lab_seg_" + std::to_string(host_pid) + "_h2c"; }

bool ensure_keyfile(pid_t host_pid, const std::string& who) {
    std::string p = keyfile_path(host_pid);
    int fd = ::open(p.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        LOGE(who, "open keyfile failed: " + p + " err=" + std::string(strerror(errno)));
        return false;
    }
    ::close(fd);
    return true;
}

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
    if (errno == ETIMEDOUT) {
        LOGW(who, what + " timeout (5s) -> connection break");
    } else {
        LOGE(who, what + " sem_timedwait failed: err=" + std::string(strerror(errno)));
    }
    return false;
}
} // namespace

class ConnSeg final : public IConn {
public:
    explicit ConnSeg(const ConnConfig& cfg)
        : cfg_(cfg),
          who_(cfg.is_host ? "HOST(seg)" : "CLIENT(seg)") {

        if (!ensure_keyfile(cfg_.host_pid, who_)) ok_ = false;

        key_t key = ftok(keyfile_path(cfg_.host_pid).c_str(), 'S');
        if (key == -1) {
            LOGE(who_, "ftok failed: err=" + std::string(strerror(errno)));
            ok_ = false;
            return;
        }

        int shmflg = 0666;
        if (cfg_.create) shmflg |= IPC_CREAT;

        shmid_ = shmget(key, sizeof(SharedSeg), shmflg);
        if (shmid_ < 0) {
            LOGE(who_, "shmget failed: err=" + std::string(strerror(errno)));
            ok_ = false;
            return;
        }

        void* addr = shmat(shmid_, nullptr, 0);
        if (addr == (void*)-1) {
            LOGE(who_, "shmat failed: err=" + std::string(strerror(errno)));
            ok_ = false;
            return;
        }
        seg_ = reinterpret_cast<SharedSeg*>(addr);

        // Semaphores: order client->host then host->client
        sem_in_  = sem_create_or_open(cfg_.is_host ? sem_name_c2h(cfg_.host_pid) : sem_name_h2c(cfg_.host_pid),
                                      0, cfg_.create, who_);
        sem_out_ = sem_create_or_open(cfg_.is_host ? sem_name_h2c(cfg_.host_pid) : sem_name_c2h(cfg_.host_pid),
                                      0, cfg_.create, who_);

        if (sem_in_ == SEM_FAILED || sem_out_ == SEM_FAILED) {
            ok_ = false;
            return;
        }

        LOGI(who_, std::string("ready; shmid=") + std::to_string(shmid_));
    }

    bool Write(const void* buf, size_t count) override {
        if (!ok_ || count != sizeof(int32_t)) return false;
        int32_t v{};
        std::memcpy(&v, buf, sizeof(v));

        if (cfg_.is_host) seg_->goat_status = v;
        else             seg_->goat_number = v;

        if (sem_post(sem_out_) != 0) {
            LOGE(who_, "sem_post failed: err=" + std::string(strerror(errno)));
            return false;
        }
        return true;
    }

    bool Read(void* buf, size_t count) override {
        if (!ok_ || count != sizeof(int32_t)) return false;
        if (!sem_wait_5s(sem_in_, who_, "Read()")) return false;

        int32_t v = cfg_.is_host ? seg_->goat_number : seg_->goat_status;
        std::memcpy(buf, &v, sizeof(v));
        return true;
    }

    ~ConnSeg() override {
        if (seg_) shmdt(seg_);
        if (cfg_.create && shmid_ >= 0) shmctl(shmid_, IPC_RMID, nullptr);

        if (sem_in_  && sem_in_  != SEM_FAILED) sem_close(sem_in_);
        if (sem_out_ && sem_out_ != SEM_FAILED) sem_close(sem_out_);

        if (cfg_.create) {
            sem_unlink(sem_name_c2h(cfg_.host_pid).c_str());
            sem_unlink(sem_name_h2c(cfg_.host_pid).c_str());
            ::unlink(keyfile_path(cfg_.host_pid).c_str());
        }
        LOGI(who_, "closed");
    }

private:
    ConnConfig cfg_;
    std::string who_;
    bool ok_ = true;

    int shmid_ = -1;
    SharedSeg* seg_ = nullptr;

    sem_t* sem_in_ = SEM_FAILED;
    sem_t* sem_out_ = SEM_FAILED;
};

std::unique_ptr<IConn> CreateConn(const ConnConfig& cfg) {
    return std::make_unique<ConnSeg>(cfg);
}

