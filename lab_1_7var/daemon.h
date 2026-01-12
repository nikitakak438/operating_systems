#pragma once

#include <string>
#include <csignal>

// √лобальные флаги, которые выставл€ютс€ обработчиками сигналов
extern volatile sig_atomic_t g_reloadRequested;
extern volatile sig_atomic_t g_stopRequested;

class Daemon {
public:
    static Daemon& instance();

    // »нициализаци€ демона: путь к конфигу (абсолютный)
    void init(const std::string& configPath);

    // ќсновной цикл обработки
    void run();

private:
    Daemon();
    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;

    static std::string trim(const std::string& s);

    void loadConfig();
    void doWork();
    void cleanDirectory(const std::string& baseDir);

    std::string configPath_;
    std::string dir1_;
    std::string dir2_;
    int intervalSec_;
    bool running_;
};
