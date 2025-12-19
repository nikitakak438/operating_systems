// main.cpp
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <cstdio>
#include <cerrno>

#include <sys/types.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <dirent.h>
#include <signal.h>

static volatile sig_atomic_t g_reloadRequested = 0;
static volatile sig_atomic_t g_stopRequested   = 0;

extern "C" void signalHandler(int sig)
{
    if (sig == SIGHUP) {
        g_reloadRequested = 1;
    } else if (sig == SIGTERM) {
        g_stopRequested = 1;
    }
}

const char* PID_FILE = "/tmp/mydaemon.pid";

// ---------- PID-файл и проверка уже запущенного демона ----------

static bool isProcessRunning(pid_t pid)
{
    if (pid <= 0) return false;
    std::string procPath = "/proc/" + std::to_string(pid);
    struct stat st{};
    return (::stat(procPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
}

static void handleExistingInstance()
{
    std::ifstream pidFile(PID_FILE);
    if (!pidFile.is_open())
        return;

    pid_t oldPid = 0;
    pidFile >> oldPid;

    if (oldPid > 0 && isProcessRunning(oldPid)) {
        // По заданию: отправить SIGTERM уже запущенному демону
        ::kill(oldPid, SIGTERM);
        // Немного подождём, чтобы он успел завершиться
        ::sleep(1);
    }
}

static void writePidFile()
{
    std::ofstream pidFile(PID_FILE, std::ios::trunc);
    if (!pidFile.is_open()) {
        syslog(LOG_ERR, "Daemon: cannot open PID file %s", PID_FILE);
        return;
    }
    pidFile << ::getpid() << std::endl;
}

// ---------- Демонизация процесса ----------

static void daemonize()
{
    pid_t pid = ::fork();
    if (pid < 0) {
        std::exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        // Родительский процесс просто выходит
        std::exit(EXIT_SUCCESS);
    }

    // Мы в дочернем процессе
    if (::setsid() < 0) {
        std::exit(EXIT_FAILURE);
    }

    // Игнорируем некоторые сигналы на время демонизации
    ::signal(SIGCHLD, SIG_IGN);
    ::signal(SIGHUP, SIG_IGN);

    pid = ::fork();
    if (pid < 0) {
        std::exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        std::exit(EXIT_SUCCESS);
    }

    // Новый процесс теперь точно не имеет управляющего терминала
    ::umask(0);

    if (::chdir("/") < 0) {
        std::exit(EXIT_FAILURE);
    }

    // Закрываем все открытые файловые дескрипторы
    long maxfd = ::sysconf(_SC_OPEN_MAX);
    if (maxfd < 0) maxfd = 1024;
    for (long fd = 0; fd < maxfd; ++fd) {
        ::close(static_cast<int>(fd));
    }

    // Перенаправляем stdin, stdout, stderr в /dev/null
    int fd0 = ::open("/dev/null", O_RDONLY);
    int fd1 = ::open("/dev/null", O_WRONLY);
    int fd2 = ::open("/dev/null", O_RDWR);
    (void)fd0;
    (void)fd1;
    (void)fd2;
}

// ---------- Установка обработчиков сигналов ----------

static void setupSignals()
{
    struct sigaction sa{};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGHUP, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

// ---------- Класс-одиночка демона ----------

class Daemon {
public:
    static Daemon& instance()
    {
        static Daemon inst;
        return inst;
    }

    void init(const std::string& configPath)
    {
        configPath_ = configPath;
        intervalSec_ = 20;      // значение по умолчанию
        running_ = true;
        loadConfig();
    }

    void run()
    {
        syslog(LOG_INFO, "Daemon: entering main loop");

        while (running_) {
            // Обработка сигналов
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

            // Основная работа
            doWork();

            // Спим по секундам, чтобы можно было прервать ожидание сигналом
            int sleepLeft = intervalSec_;
            while (sleepLeft-- > 0 && running_) {
                if (g_reloadRequested || g_stopRequested)
                    break;
                (void)::sleep(1);
            }
        }

        syslog(LOG_INFO, "Daemon: main loop finished");
    }

private:
    Daemon() : intervalSec_(20), running_(false) {}
    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;

    static std::string trim(const std::string& s)
    {
        const char* ws = " \t\r\n";
        std::size_t start = s.find_first_not_of(ws);
        if (start == std::string::npos) return "";
        std::size_t end = s.find_last_not_of(ws);
        return s.substr(start, end - start + 1);
    }

    void loadConfig()
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
            } else if (key == "DIR2") {
                newDir2 = val;
            } else if (key == "INTERVAL") {
                try {
                    int v = std::stoi(val);
                    if (v > 0) newInterval = v;
                } catch (...) {
                    syslog(LOG_ERR, "Daemon: invalid INTERVAL: %s", val.c_str());
                }
            }
        }

        if (newDir1.empty()) {
            syslog(LOG_ERR, "Daemon: DIR1 is not set in config");
        } else {
            dir1_ = newDir1;
        }
        dir2_ = newDir2;
        intervalSec_ = newInterval;

        syslog(LOG_INFO, "Daemon: config loaded: DIR1=%s, DIR2=%s, INTERVAL=%d",
               dir1_.c_str(),
               dir2_.empty() ? "(none)" : dir2_.c_str(),
               intervalSec_);
    }

    // Рекурсивное удаление файла/директории с содержимым
    void removeRecursive(const std::string& path)
    {
        struct stat st{};
        if (::lstat(path.c_str(), &st) == -1) {
            syslog(LOG_ERR, "Daemon: lstat failed for %s: %s",
                   path.c_str(), std::strerror(errno));
            return;
        }

        if (S_ISDIR(st.st_mode)) {
            DIR* dir = ::opendir(path.c_str());
            if (!dir) {
                syslog(LOG_ERR, "Daemon: opendir failed for %s: %s",
                       path.c_str(), std::strerror(errno));
                return;
            }

            struct dirent* entry;
            while ((entry = ::readdir(dir)) != nullptr) {
                // пропускаем . и ..
                if (std::strcmp(entry->d_name, ".") == 0 ||
                    std::strcmp(entry->d_name, "..") == 0) {
                    continue;
                }

                std::string child = path;
                child += "/";
                child += entry->d_name;

                removeRecursive(child);
            }

            ::closedir(dir);

            if (::rmdir(path.c_str()) == -1) {
                syslog(LOG_ERR, "Daemon: rmdir failed for %s: %s",
                       path.c_str(), std::strerror(errno));
            }
        } else {
            if (::unlink(path.c_str()) == -1) {
                syslog(LOG_ERR, "Daemon: unlink failed for %s: %s",
                       path.c_str(), std::strerror(errno));
            }
        }
    }

    // Удаление всех подпапок в baseDir (с их содержимым)
    void cleanDirectory(const std::string& baseDir)
    {
        if (baseDir.empty())
            return;

        DIR* dir = ::opendir(baseDir.c_str());
        if (!dir) {
            syslog(LOG_ERR, "Daemon: opendir failed for %s: %s",
                   baseDir.c_str(), std::strerror(errno));
            return;
        }

        struct dirent* entry;
        while ((entry = ::readdir(dir)) != nullptr) {
            if (std::strcmp(entry->d_name, ".") == 0 ||
                std::strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            std::string path = baseDir;
            path += "/";
            path += entry->d_name;

            struct stat st{};
            if (::lstat(path.c_str(), &st) == -1) {
                syslog(LOG_ERR, "Daemon: lstat failed for %s: %s",
                       path.c_str(), std::strerror(errno));
                continue;
            }

            if (S_ISDIR(st.st_mode)) {
                removeRecursive(path);
                syslog(LOG_INFO, "Daemon: removed directory %s", path.c_str());
            }
        }

        ::closedir(dir);
    }

    void doWork()
    {
        cleanDirectory(dir1_);
        if (!dir2_.empty()) {
            cleanDirectory(dir2_);
        }
    }

    std::string configPath_;
    std::string dir1_;
    std::string dir2_;
    int intervalSec_;
    bool running_;
};

// ---------- main ----------

int main(int argc, char* argv[])
{
    // 1. Путь к конфигурационному файлу (по умолчанию daemon.conf в текущей директории)
    std::string cfgPath = (argc > 1) ? argv[1] : "daemon.conf";

    // 2. Получаем абсолютный путь и "запоминаем" его для дальнейшего использования
    char resolved[PATH_MAX];
    if (!::realpath(cfgPath.c_str(), resolved)) {
        std::perror("realpath");
        return EXIT_FAILURE;
    }
    std::string absCfgPath = resolved;

    // 3. Проверяем уже работающий демон по PID-файлу и, если есть, шлём SIGTERM
    handleExistingInstance();

    // 4. Демонизируемся
    daemonize();

    // 5. Открываем системный журнал
    openlog("mydaemon", LOG_PID | LOG_NDELAY, LOG_DAEMON);
    syslog(LOG_INFO, "Daemon: started");

    // 6. Записываем свой PID
    writePidFile();

    // 7. Настраиваем обработчики сигналов
    setupSignals();

    // 8. Инициализируем и запускаем демон
    Daemon& d = Daemon::instance();
    d.init(absCfgPath);
    d.run();

    syslog(LOG_INFO, "Daemon: exiting");

    // 9. Удаляем PID-файл
    ::unlink(PID_FILE);

    // 10. Закрываем системный журнал
    closelog();

    return EXIT_SUCCESS;
}

