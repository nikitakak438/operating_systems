#include "daemon.h"

#include <filesystem>
#include <fstream>
#include <syslog.h>
#include <unistd.h>

Daemon::Daemon() : intervalSec_(20), running_(false) {}

Daemon& Daemon::instance()
{
    static Daemon inst;
    return inst;
}

void Daemon::init(const std::string& configPath)
{
    configPath_ = configPath;
    intervalSec_ = 20;
    running_ = true;
    loadConfig();
}

void Daemon::run()
{
    syslog(LOG_INFO, "Daemon: entering main loop");

    while (running_) {
        if (g_reloadRequested) {
            g_reloadRequested = 0;
            syslog(LOG_INFO, "Daemon: SIGHUP received, reloading config");
            loadConfig();
        }

        if (g_stopRequested) {
            g_stopRequested = 0;
            syslog(LOG_INFO, "Daemon: SIGTERM received, stopping");
            running_ = false;
            break;
        }

        doWork();

        int sleepLeft = intervalSec_;
        while (sleepLeft-- > 0 && running_) {
            if (g_reloadRequested || g_stopRequested)
                break;
            (void)::sleep(1);
        }
    }

    syslog(LOG_INFO, "Daemon: main loop finished");
}

std::string Daemon::trim(const std::string& s)
{
    const char* ws = " \t\r\n";
    std::size_t start = s.find_first_not_of(ws);
    if (start == std::string::npos) return "";
    std::size_t end = s.find_last_not_of(ws);
    return s.substr(start, end - start + 1);
}

void Daemon::loadConfig()
{
    if (configPath_.empty()) {
        syslog(LOG_ERR, "Daemon: config path is empty");
        return;
    }

    std::ifstream cfg(configPath_);
    if (!cfg.is_open()) {
        syslog(LOG_ERR, "Daemon: cannot open config file: %s", configPath_.c_str());
        return;
    }

    std::string line;
    std::string newDir1;
    std::string newDir2;
    int newInterval = intervalSec_;

    while (std::getline(cfg, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;

        std::size_t pos = line.find('=');
        if (pos == std::string::npos)
            continue;

        std::string key = trim(line.substr(0, pos));
        std::string val = trim(line.substr(pos + 1));

        if (key == "DIR1") {
            newDir1 = val;
        }
        else if (key == "DIR2") {
            newDir2 = val;
        }
        else if (key == "INTERVAL") {
            try {
                int v = std::stoi(val);
                if (v > 0) newInterval = v;
            }
            catch (...) {
                syslog(LOG_ERR, "Daemon: invalid INTERVAL: %s", val.c_str());
            }
        }
    }

    if (newDir1.empty()) {
        syslog(LOG_ERR, "Daemon: DIR1 is not set in config");
    }
    else {
        dir1_ = newDir1;
    }
    dir2_ = newDir2;
    intervalSec_ = newInterval;

    syslog(LOG_INFO, "Daemon: config loaded: DIR1=%s, DIR2=%s, INTERVAL=%d",
        dir1_.c_str(),
        dir2_.empty() ? "(none)" : dir2_.c_str(),
        intervalSec_);
}

void Daemon::cleanDirectory(const std::string& baseDir)
{
    if (baseDir.empty())
        return;

    namespace fs = std::filesystem;

    try {
        for (const auto& entry : fs::directory_iterator(baseDir)) {
            if (entry.is_directory()) {
                fs::remove_all(entry.path());
                syslog(LOG_INFO, "Daemon: removed directory %s",
                    entry.path().c_str());
            }
        }
    }
    catch (const std::exception& ex) {
        syslog(LOG_ERR, "Daemon: error cleaning %s: %s",
            baseDir.c_str(), ex.what());
    }
}

void Daemon::doWork()
{
    cleanDirectory(dir1_);
    if (!dir2_.empty()) {
        cleanDirectory(dir2_);
    }
}
