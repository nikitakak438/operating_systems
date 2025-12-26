#include "common/conn_iface.hpp"
#include "common/log.hpp"
#include "common/posix_time.hpp"

#include <cerrno>
#include <cstring>
#include <mqueue.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr int TIMEOUT_SEC = 5;

std::string mq_name_c2h(pid_t host_pid) { return "/lab_mq_" + std::to_string(host_pid) + "_c2h"; }
std::string mq_name_h2c(pid_t host_pid) { return "/lab_mq_" + std::to_string(host_pid) + "_h2c"; }

std::string sem_name_c2h(pid_t host_pid) { return "/lab_mq_sem_" + std::to_string(host_pid) + "_c2h"; }
std::string sem_name_h2c(pid_t host_pid) { return "/lab_mq_sem_" + std::to_string(host_pid) + "_h2c"; }

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

mqd_t mq_create_or_open(const std::string& name, bool create, const std::string& who) {
    mq_attr attr{};
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(int32_t);

    if (create) {
        mq_unlink(name.c_str());
        mqd_t q = mq_open(name.c_str(), O_CREAT | O_RDWR, 0666, &attr);
        if (q != (mqd_t)-1) return q;
        LOGE(who, "mq_open(create) failed: " + name + " err=" + std::string(strerror(errno)));
        return (mqd_t)-1;
    }

    mqd_t q = mq_open(name.c_str(), O_RDWR);
    if (q == (mqd_t)-1) {
        LOGE(who, "mq_open(open) failed: " + name + " err=" + std::string(strerror(errno)));
    }
    return q;
}

bool mq_send_5s(mqd_t q, const std::string& who, const void* data, size_t sz) {
    timespec ts = make_abstimeout_sec(TIMEOUT_SEC);
    if (mq_timedsend(q, (const char*)data, sz, 0, &ts) == 0) return true;
    if (errno == ETIMEDOUT) LOGW(who, "mq_timedsend timeout (5s) -> connection break");
    else LOGE(who, "mq_timedsend failed: err=" + std::string(strerror(errno)));
    return false;
}

bool mq_recv_5s(mqd_t q, const std::string& who, void* data, size_t sz) {
    timespec ts = make_abstimeout_sec(TIMEOUT_SEC);
    unsigned prio = 0;
    ssize_t r = mq_timedreceive(q, (char*)data, sz, &prio, &ts);
    if (r == (ssize_t)sz) return true;
    if (r < 0 && errno == ETIMEDOUT) LOGW(who, "mq_timedreceive timeout (5s) -> connection break");
    else LOGE(who, "mq_timedreceive failed: err=" + std::string(strerror(errno)));
    return false;
}
} // namespace

class ConnMq final : public IConn {
public:
    explicit ConnMq(const ConnConfig& cfg)
        : cfg_(cfg),
          who_(cfg.is_host ? "HOST(mq)" : "CLIENT(mq)") {

        // in = where I read from, out = where I write to
        std::string in_name  = cfg_.is_host ? mq_name_c2h(cfg_.host_pid) : mq_name_h2c(cfg_.host_pid);
        std::string out_name = cfg_.is_host ? mq_name_h2c(cfg_.host_pid) : mq_name_c2h(cfg_.host_pid);

        q_in_  = mq_create_or_open(in_name,  cfg_.create, who_);
        q_out_ = mq_create_or_open(out_name, cfg_.create, who_);
        if (q_in_ == (mqd_t)-1 || q_out_ == (mqd_t)-1) { ok_ = false; return; }

        sem_in_  = sem_create_or_open(cfg_.is_host ? sem_name_c2h(cfg_.host_pid) : sem_name_h2c(cfg_.host_pid),
                                      0, cfg_.create, who_);
        sem_out_ = sem_create_or_open(cfg_.is_host ? sem_name_h2c(cfg_.host_pid) : sem_name_c2h(cfg_.host_pid),
                                      0, cfg_.create, who_);
        if (sem_in_ == SEM_FAILED || sem_out_ == SEM_FAILED) { ok_ = false; return; }

        LOGI(who_, "ready");
    }

    bool Write(const void* buf, size_t count) override {
        if (!ok_ || count != sizeof(int32_t)) return false;

        // Send message (5s timeout)
        if (!mq_send_5s(q_out_, who_, buf, sizeof(int32_t))) return false;

        // Signal peer that message exists (global semaphore)
        if (sem_post(sem_out_) != 0) {
            LOGE(who_, "sem_post failed: err=" + std::string(strerror(errno)));
            return false;
        }
        return true;
    }

    bool Read(void* buf, size_t count) override {
        if (!ok_ || count != sizeof(int32_t)) return false;

        // Wait order semaphore (5s)
        if (!sem_wait_5s(sem_in_, who_, "Read()")) return false;

        // Receive message (5s timeout)
        return mq_recv_5s(q_in_, who_, buf, sizeof(int32_t));
    }

    ~ConnMq() override {
        if (q_in_  != (mqd_t)-1) mq_close(q_in_);
        if (q_out_ != (mqd_t)-1) mq_close(q_out_);

        if (sem_in_  && sem_in_  != SEM_FAILED) sem_close(sem_in_);
        if (sem_out_ && sem_out_ != SEM_FAILED) sem_close(sem_out_);

        if (cfg_.create) {
            mq_unlink(mq_name_c2h(cfg_.host_pid).c_str());
            mq_unlink(mq_name_h2c(cfg_.host_pid).c_str());
            sem_unlink(sem_name_c2h(cfg_.host_pid).c_str());
            sem_unlink(sem_name_h2c(cfg_.host_pid).c_str());
        }
        LOGI(who_, "closed");
    }

private:
    ConnConfig cfg_;
    std::string who_;
    bool ok_ = true;

    mqd_t q_in_  = (mqd_t)-1;
    mqd_t q_out_ = (mqd_t)-1;

    sem_t* sem_in_  = SEM_FAILED;
    sem_t* sem_out_ = SEM_FAILED;
};

std::unique_ptr<IConn> CreateConn(const ConnConfig& cfg) {
    return std::make_unique<ConnMq>(cfg);
}

