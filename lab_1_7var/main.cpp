#include "daemon.h"
#include "pid_manager.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <limits.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

// Глобальные флаги для обработки сигналов
volatile sig_atomic_t g_reloadRequested = 0;
volatile sig_atomic_t g_stopRequested = 0;

extern "C" void signalHandler(int sig)
{
    if (sig == SIGHUP) {
        g_reloadRequested = 1;
    }
    else if (sig == SIGTERM) {
        g_stopRequested = 1;
    }
}

void daemonize()
{
    pid_t pid = ::fork();
    if (pid < 0) {
        std::exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        std::exit(EXIT_SUCCESS);
    }

    if (::setsid() < 0) {
        std::exit(EXIT_FAILURE);
    }

    ::signal(SIGCHLD, SIG_IGN);
    ::signal(SIGHUP, SIG_IGN);

    pid = ::fork();
    if (pid < 0) {
        std::exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        std::exit(EXIT_SUCCESS);
    }

    ::umask(0);

    if (::chdir("/") < 0) {
        std::exit(EXIT_FAILURE);
    }

    long maxfd = ::sysconf(_SC_OPEN_MAX);
    if (maxfd < 0) maxfd = 1024;
    for (long fd = 0; fd < maxfd; ++fd) {
        ::close(static_cast<int>(fd));
    }

    int fd0 = ::open("/dev/null", O_RDONLY);
    int fd1 = ::open("/dev/null", O_WRONLY);
    int fd2 = ::open("/dev/null", O_RDWR);
    (void)fd0; (void)fd1; (void)fd2;
}

void setupSignals()
{
    struct sigaction sa {};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGHUP, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

int main(int argc, char* argv[])
{
    // 1. Получаем путь к конфигу (относительный)
    std::string cfgPath = (argc > 1) ? argv[1] : "daemon.conf";

    // 2. Запоминаем абсолютный путь
    char resolved[PATH_MAX];
    if (!::realpath(cfgPath.c_str(), resolved)) {
        std::perror("realpath");
        return EXIT_FAILURE;
    }
    std::string absCfgPath = resolved;

    // 3. Проверяем уже работающий демон через PID-файл
    PidManager::handleExistingInstance();

    // 4. Демонизируемся
    daemonize();

    // 5. Открываем syslog
    openlog("mydaemon", LOG_PID | LOG_NDELAY, LOG_DAEMON);
    syslog(LOG_INFO, "Daemon: started");

    // 6. Записываем свой PID
    PidManager::writePidFile();

    // 7. Настраиваем обработчики сигналов
    setupSignals();

    // 8. Инициализируем и запускаем демон
    Daemon& d = Daemon::instance();
    d.init(absCfgPath);
    d.run();

    syslog(LOG_INFO, "Daemon: exiting");

    // 9. Убираем PID-файл
    PidManager::removePidFile();

    closelog();
    return EXIT_SUCCESS;
}
