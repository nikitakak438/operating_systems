#pragma once

#include <sys/types.h>

namespace PidManager {

    // Проверка существования процесса с данным PID через /proc
    bool isProcessRunning(pid_t pid);

    // Обработка уже существующего экземпляра демона (чтение PID-файла и посылка SIGTERM)
    void handleExistingInstance();

    // Запись текущего PID в PID-файл (вызывать после демонизации и openlog)
    void writePidFile();

    // Удаление PID-файла при корректном завершении
    void removePidFile();

} // namespace PidManager
