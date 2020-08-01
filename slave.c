#include "slave.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <termios.h>

#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#define LOG_MODULE "slave"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "terminal.h"
#include "tokenize.h"

static bool
is_valid_shell(const char *shell)
{
    FILE *f = fopen("/etc/shells", "r");
    if (f == NULL)
        goto err;

    char *_line = NULL;
    size_t count = 0;

    while (true) {
        errno = 0;
        ssize_t ret = getline(&_line, &count, f);

        if (ret < 0) {
            free(_line);
            break;
        }

        char *line = _line;
        {
            while (isspace(*line))
                line++;
            if (line[0] != '\0') {
                char *end = line + strlen(line) - 1;
                while (isspace(*end))
                    end--;
                *(end + 1) = '\0';
            }
        }

        if (line[0] == '#')
            continue;

        if (strcmp(line, shell) == 0) {
            fclose(f);
            return true;
        }
    }

err:
    if (f != NULL)
        fclose(f);
    return false;
}

enum user_notification_ret_t {UN_OK, UN_NO_MORE, UN_FAIL};

static enum user_notification_ret_t
emit_one_notification(int fd, const struct user_notification *notif)
{
    const char *prefix = NULL;
    const char *postfix = "\e[m\n";

    switch (notif->kind) {
    case USER_NOTIFICATION_DEPRECATED:
        prefix = "\e[33;1mdeprecated\e[39;21m: ";
        break;

    case USER_NOTIFICATION_WARNING:
        prefix = "\e[33;1mwarning\e[39;21m: ";
        break;

    case USER_NOTIFICATION_ERROR:
        prefix = "\e[31;1merror\e[39;21m: ";
        break;
    }

    assert(prefix != NULL);

    if (write(fd, prefix, strlen(prefix)) < 0 ||
        write(fd, notif->text, strlen(notif->text)) < 0 ||
        write(fd, postfix, strlen(postfix)) < 0)
    {
        /*
         * The main process is blocking and waiting for us to close
         * the error pipe. Thus, pts data will *not* be processed
         * until we've exec:d. This means we cannot write anymore once
         * the kernel buffer is full. Don't treat this as a fatal
         * error.
         */
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            return UN_NO_MORE;
        else {
            LOG_ERRNO("failed to write user-notification");
            return UN_FAIL;
        }
    }

    return UN_OK;
}

static bool
emit_notifications_of_kind(int fd, const user_notifications_t *notifications,
                           enum user_notification_kind kind)
{
    tll_foreach(*notifications, it) {
        if (it->item.kind == kind) {
            switch (emit_one_notification(fd, &it->item)) {
            case UN_OK:
                break;
            case UN_NO_MORE:
                return true;
            case UN_FAIL:
                return false;
            }
        }
    }

    return true;
}

static bool
emit_notifications(int fd, const user_notifications_t *notifications)
{
    return
        emit_notifications_of_kind(fd, notifications, USER_NOTIFICATION_ERROR) &&
        emit_notifications_of_kind(fd, notifications, USER_NOTIFICATION_WARNING) &&
        emit_notifications_of_kind(fd, notifications, USER_NOTIFICATION_DEPRECATED);
}

static void
slave_exec(int ptmx, char *argv[], int err_fd, bool login_shell,
           const user_notifications_t *notifications)
{
    int pts = -1;
    const char *pts_name = ptsname(ptmx);

    if (grantpt(ptmx) == -1) {
        LOG_ERRNO("failed to grantpt()");
        goto err;
    }
    if (unlockpt(ptmx) == -1) {
        LOG_ERRNO("failed to unlockpt()");
        goto err;
    }

    close(ptmx);
    ptmx = -1;

    if (setsid() == -1) {
        LOG_ERRNO("failed to setsid()");
        goto err;
    }

    pts = open(pts_name, O_RDWR);
    if (pts == -1) {
        LOG_ERRNO("failed to open pseudo terminal slave device");
        goto err;
    }

    if (ioctl(pts, TIOCSCTTY, 0) < 0) {
        LOG_ERRNO("failed to configure controlling terminal");
        goto err;
    }

    {
        struct termios flags;
        if (tcgetattr(pts, &flags) < 0) {
            LOG_ERRNO("failed to get terminal attributes");
            goto err;
        }

        flags.c_iflag |= IUTF8;
        if (tcsetattr(pts, TCSANOW, &flags) < 0) {
            LOG_ERRNO("failed to set IUTF8 terminal attribute");
            goto err;
        }
    }

    if (tll_length(*notifications) > 0) {
        int flags = fcntl(pts, F_GETFL);
        if (flags < 0)
            goto err;
        if (fcntl(pts, F_SETFL, flags | O_NONBLOCK) < 0)
            goto err;

        if (!emit_notifications(pts, notifications))
            goto err;

        fcntl(pts, F_SETFL, flags);
    }

    if (dup2(pts, STDIN_FILENO) == -1 ||
        dup2(pts, STDOUT_FILENO) == -1 ||
        dup2(pts, STDERR_FILENO) == -1)
    {
        LOG_ERRNO("failed to dup stdin/stdout/stderr");
        goto err;
    }

    close(pts);
    pts = -1;

    const char *file;
    if (login_shell) {
        file = strdup(argv[0]);

        char *arg0 = malloc(strlen(argv[0]) + 1 + 1);
        arg0[0] = '-';
        arg0[1] = '\0';
        strcat(arg0, argv[0]);

        argv[0] = arg0;
    } else
        file = argv[0];

    execvp(file, argv);

err:
    (void)!write(err_fd, &errno, sizeof(errno));
    if (pts != -1)
        close(pts);
    if (ptmx != -1)
        close(ptmx);
    close(err_fd);
    _exit(errno);
}

pid_t
slave_spawn(int ptmx, int argc, const char *cwd, char *const *argv,
            const char *term_env, const char *conf_shell, bool login_shell,
            const user_notifications_t *notifications)
{
    int fork_pipe[2];
    if (pipe2(fork_pipe, O_CLOEXEC) < 0) {
        LOG_ERRNO("failed to create pipe");
        return -1;
    }

    pid_t pid = fork();
    switch (pid) {
    case -1:
        LOG_ERRNO("failed to fork");
        close(fork_pipe[0]);
        close(fork_pipe[1]);
        return -1;

    case 0:
        /* Child */
        close(fork_pipe[0]);  /* Close read end */

        if (chdir(cwd) < 0) {
            const int _errno = errno;
            LOG_ERRNO("failed to change working directory");
            (void)!write(fork_pipe[1], &_errno, sizeof(_errno));
            _exit(_errno);
        }

        /* Restore signals */
        sigset_t mask;
        sigemptyset(&mask);
        const struct sigaction sa = {.sa_handler = SIG_DFL};
        if (sigaction(SIGINT, &sa, NULL) < 0 ||
            sigaction(SIGTERM, &sa, NULL) < 0 ||
            sigaction(SIGHUP, &sa, NULL) < 0 ||
            sigprocmask(SIG_SETMASK, &mask, NULL) < 0)
        {
            const int _errno = errno;
            LOG_ERRNO_P("failed to restore signals", errno);
            (void)!write(fork_pipe[1], &_errno, sizeof(_errno));
            _exit(_errno);
        }

        setenv("TERM", term_env, 1);

        char **_shell_argv = NULL;
        char **shell_argv = NULL;

        if (argc == 0) {
            char *shell_copy = strdup(conf_shell);
            if (!tokenize_cmdline(shell_copy, &_shell_argv)) {
                free(shell_copy);
                (void)!write(fork_pipe[1], &errno, sizeof(errno));
                _exit(0);
            }
            shell_argv = _shell_argv;
        } else {
            size_t count = 0;
            for (; argv[count] != NULL; count++)
                ;
            shell_argv = malloc((count + 1) * sizeof(shell_argv[0]));
            for (size_t i = 0; i < count; i++)
                shell_argv[i] = argv[i];
            shell_argv[count] = NULL;
        }

        if (is_valid_shell(shell_argv[0]))
            setenv("SHELL", shell_argv[0], 1);

        slave_exec(ptmx, shell_argv, fork_pipe[1], login_shell, notifications);
        assert(false);
        break;

    default: {
        close(fork_pipe[1]); /* Close write end */
        LOG_DBG("slave has PID %d", pid);

        int _errno;
        static_assert(sizeof(errno) == sizeof(_errno), "errno size mismatch");

        ssize_t ret = read(fork_pipe[0], &_errno, sizeof(_errno));
        close(fork_pipe[0]);

        if (ret < 0) {
            LOG_ERRNO("failed to read from pipe");
            return -1;
        } else if (ret == sizeof(_errno)) {
            LOG_ERRNO_P(
                "%s: failed to execute", _errno, argc == 0 ? conf_shell : argv[0]);
            return -1;
        } else
            LOG_DBG("%s: successfully started", conf_shell);

        int fd_flags;
        if ((fd_flags = fcntl(ptmx, F_GETFD)) < 0 ||
            fcntl(ptmx, F_SETFD, fd_flags | FD_CLOEXEC) < 0)
        {
            LOG_ERRNO("failed to set FD_CLOEXEC on ptmx");
            return -1;
        }
        break;
    }
    }

    return pid;
}
