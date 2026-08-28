#pragma once
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>

extern char **environ;

static inline bool runArgv(const char *path, const char *const argv[]) {
    pid_t pid;
    if (posix_spawn(&pid, path, nullptr, nullptr,
                    (char *const *)argv, environ) != 0)
        return false;
    int status;
    if (waitpid(pid, &status, 0) < 0)
        return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static inline bool runPrivilegedScript(const char *script) {
    const char *args[] = {"/usr/bin/osascript", "-e", script, nullptr};
    return runArgv("/usr/bin/osascript", args);
}

static inline std::string runCapture(const char *path, const char *const argv[],
                                     int *exit_status = nullptr) {
    int pipefd[2];
    if (pipe(pipefd) < 0)
        return {};
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    if (pipefd[1] != STDOUT_FILENO)
        posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    pid_t pid;
    int r = posix_spawn(&pid, path, &actions, nullptr,
                        (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);

    if (r != 0) {
        close(pipefd[0]);
        if (exit_status)
            *exit_status = -1;
        return {};
    }

    std::string result;
    char buffer[256];
    ssize_t n;
    while ((n = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0) {
        buffer[n] = '\0';
        result += buffer;
    }
    close(pipefd[0]);

    int status;
    if (waitpid(pid, &status, 0) < 0) {
        if (exit_status)
            *exit_status = -1;
        return result;
    }
    if (exit_status)
        *exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}
