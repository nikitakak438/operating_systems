#include "pid_manager.h"

#include <fstream>
#include <string>

#include <signal.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

namespace {
    // ѕеременна€ с путЄм к PID-файлу Ч теперь тут, а не в main.cpp
    constexpr const char* PID_FILE = "/tmp/mydaemon.pid";
}

namespace PidManager {

    bool isProcessRunning(pid_t pid)
    {
        if (pid <= 0) return false;
        std::string procPath = std::string("/proc/") + std::to_string(pid);
        struct stat st {};
        return (::stat(procPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
    }

    void handleExistingInstance()
    {
        std::ifstream pidFile(PID_FILE);
        if (!pidFile.is_open())
            return;

        pid_t oldPid = 0;
        pidFile >> oldPid;

        if (oldPid > 0 && isProcessRunning(oldPid)) {
            ::kill(oldPid, SIGTERM);
            ::sleep(1);
        }
    }

    void writePidFile()
    {
        std::ofstream pidFile(PID_FILE, std::ios::trunc);
        if (!pidFile.is_open()) {
            syslog(LOG_ERR, "Daemon: cannot open PID file %s", PID_FILE);
            return;
        }
        pidFile << ::getpid() << std::endl;
    }

    void removePidFile()
    {
        ::unlink(PID_FILE);
    }

} // namespace PidManager
