#include "daemon.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <syslog.h>
#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <time.h>

#include "options.h"


typedef enum EXIT_STATUS {
    EXIT_STATUS_SUCCESS,
    EXIT_STATUS_FAILURE,
} EXIT_STATUS;

typedef enum RUN_STATUS {
    RUN_STATUS_NORMAL,
    RUN_STATUS_EXIT_SUCCESS,
    RUN_STATUS_EXIT_FAILURE,
} RUN_STATUS;

typedef struct Daemon {
    pid_t pid, parent_pid, sid;
    mode_t file_mask;
    const char *identify_name, *run_user, *run_directory, *lock_file, *pid_file;
    Options *options;
} Daemon;

static int run_status = RUN_STATUS_NORMAL;

static void child_handler(int signal)
{
    switch (signal) {
        case SIGTERM:
        case SIGUSR1:
            run_status = RUN_STATUS_EXIT_SUCCESS;
            break;

        case SIGALRM:
        case SIGCHLD:
            run_status = RUN_STATUS_EXIT_FAILURE;
            break;

        default:
            break;
    }
}

static void init_log(const char *identify_name)
{
    openlog(identify_name, LOG_PID, LOG_LOCAL5);
}

static void close_log(void)
{
    closelog();
}

static void exit_graceful(EXIT_STATUS exit_status)
{
    syslog(LOG_NOTICE, "finished");
    close_log();

    exit(exit_status);
}

static int create_lock_file(const char *lock_file)
{
    int lock_file_descriptor = -1;

    // Create the lock file as the current user
    if (lock_file && lock_file[0]) {
        lock_file_descriptor = open(lock_file, O_RDWR | O_CREAT, 0640);

        if (lock_file_descriptor < 0) {
            syslog(LOG_ERR, "failed to create lock file %s, code=%d (%s)",
                   lock_file, errno, strerror(errno));

            return 0;
        }

        if (flock(lock_file_descriptor, LOCK_EX | LOCK_NB) == -1) {
            syslog(LOG_ERR, "failed to lock file %s, code=%d (%s)",
                   lock_file, errno, strerror(errno));

            return 0;
        }

        return 1;
    }

    return 0;
}

static int create_pid_file(const char *pid_file)
{
    FILE *file_handle;

    if ((file_handle = fopen (pid_file, "w")) == NULL) {
        syslog(LOG_ERR, "failed to create pid file %s, code=%d (%s)",
               pid_file, errno, strerror(errno));

        return 0;
    }

    pid_t pid = getpid();

    fprintf(file_handle, "%i\n", (int) pid);
    fclose(file_handle);

    return 1;
}

static int remove_file(const char *file)
{
    if ((unlink(file)) == -1) {
        syslog(LOG_ERR, "failed to remove file %s, code=%d (%s)",
               file, errno, strerror(errno));

        return 0;
    }

    return 1;
}

static int set_file_mask(mode_t file_mask)
{
    // Change the file mode mask
    umask(file_mask);

    return 1;
}

static int set_run_user(const char *run_user)
{
    // Drop user if there is one, and we were run as root
    if (getuid() == 0 && geteuid() == 0) {
        struct passwd *pw = getpwnam(run_user);
        if (pw) {
            syslog(LOG_NOTICE, "setting user to  %s", run_user);

            setuid(pw->pw_uid);

            return 1;
        }
    }

    syslog(LOG_ERR, "failed to set run user, calling user was not root");

    return 0;
}

static int set_run_directory(const char *run_directory)
{
    /* Change the current working directory.  This prevents the current
       directory from being locked; hence not being able to remove it. */
    if ((chdir(run_directory)) < 0) {
        syslog(LOG_ERR, "failed to change directory to %s, code %d (%s)",
                   run_directory, errno, strerror(errno));

        return 0;
    }

    return 1;
}

static int redirect_io(void)
{
    /* Redirect standard files to /dev/null */
    if (freopen("/dev/null", "r", stdin) == NULL) {
        syslog(LOG_ERR, "failed to redirect stdin, code %d (%s)",
               errno, strerror(errno));

        return 0;
    }

    if (freopen("/dev/null", "w", stdout) == NULL) {
        syslog(LOG_ERR, "failed to redirect stdout, code %d (%s)",
               errno, strerror(errno));

        return 0;
    }

    if (freopen("/dev/null", "w", stderr) == NULL) {
        syslog(LOG_ERR, "failed to redirect stderr, code %d (%s)",
               errno, strerror(errno));

        return 0;
    }

    return 1;
}

static int start_new_session(pid_t *sid)
{
    /* Create a new SID for the child process */
    *sid = setsid();

    if (*sid < 0) {
        syslog(LOG_ERR, "failed to create a new session, code %d (%s)",
                   errno, strerror(errno));

        return 0;
    }

    return 1;
}

static int fork_active_process(pid_t *parent_pid)
{
    pid_t pid = fork();

    if (pid < 0) {
        syslog(LOG_ERR, "failed to fork daemon, code=%d (%s)",
               errno, strerror(errno));

        return 0;
    }

    /* If we got a good PID, then we can exit the parent process. */
    if (pid > 0) {
        /* Wait for confirmation from the child via SIGTERM or SIGCHLD, or
           for two seconds to elapse (SIGALRM). pause() should not return. */
        alarm(2);
        pause();

        exit_graceful(EXIT_STATUS_FAILURE);
    }

    *parent_pid = getppid();

    return 1;
}

static int close_parent_process(pid_t parent_pid)
{
    if (kill(parent_pid, SIGUSR1) < 0) {
        syslog(LOG_ERR, "failed to close parent process, code=%d (%s)",
               errno, strerror(errno));

        return 0;
    }

    return 1;
}

static int set_trapped_signals(void)
{
    if (signal(SIGCHLD, child_handler) == SIG_ERR) {
        syslog(LOG_ERR, "failed to set signal handler for SIGCHLD, code=%d (%s)",
               errno, strerror(errno));

        return 0;
    }

    if (signal(SIGUSR1, child_handler) == SIG_ERR) {
        syslog(LOG_ERR, "failed to set signal handler for SIGUSR1, code=%d (%s)",
               errno, strerror(errno));

        return 0;
    }

    if (signal(SIGALRM, child_handler) == SIG_ERR) {
        syslog(LOG_ERR, "failed to set signal handler for SIGALRM, code=%d (%s)",
               errno, strerror(errno));

        return 0;
    }

    if (signal(SIGTERM, child_handler) == SIG_ERR) {
        syslog(LOG_ERR, "failed to set signal handler for SIGTERM, code=%d (%s)",
               errno, strerror(errno));

        return 0;
    }

    return 1;
}

static int set_ignored_signals(void)
{
    if (signal(SIGTSTP, SIG_IGN) == SIG_ERR) {
        syslog(LOG_ERR, "failed to set SIGTERM as ignored , code=%d (%s)",
               errno, strerror(errno));

        return 0;
    }

    if (signal(SIGTTOU, SIG_IGN) == SIG_ERR) {
        syslog(LOG_ERR, "failed to set SIGTTOU as ignored , code=%d (%s)",
               errno, strerror(errno));

        return 0;
    }

    if (signal(SIGTTIN, SIG_IGN) == SIG_ERR) {
        syslog(LOG_ERR, "failed to set SIGTTIN as ignored , code=%d (%s)",
               errno, strerror(errno));

        return 0;
    }

    if (signal(SIGHUP, SIG_IGN) == SIG_ERR) {
        syslog(LOG_ERR, "failed to set SIGHUP as ignored , code=%d (%s)",
               errno, strerror(errno));

        return 0;
    }

    return 1;
}

static void cleanup(Daemon *daemon)
{
    remove_file(daemon->pid_file);
    remove_file(daemon->lock_file);

    Daemon_destroy(daemon);
}

static void update(Daemon *daemon)
{
    switch (run_status) {
        case RUN_STATUS_EXIT_SUCCESS: {
            cleanup(daemon);
            exit_graceful(EXIT_STATUS_SUCCESS);
        }

        case RUN_STATUS_EXIT_FAILURE: {
            cleanup(daemon);
            exit_graceful(EXIT_STATUS_FAILURE);
        }

        default:
            break;
    }
}

static void sleep_ms(unsigned int milliseconds)
{
    struct timespec req = {0};

    time_t seconds = (int) (milliseconds / 1000);

    milliseconds = milliseconds - ((unsigned int) seconds * 1000);

    req.tv_sec = seconds;
    req.tv_nsec = milliseconds * 1000000L;

    while (nanosleep(&req, &req) == -1);
}

static Daemon *init(const char *identify_name,
                    const char *run_user,
                    const char *run_directory,
                    const char *lock_file,
                    const char *pid_file,
                    pid_t pid, pid_t parent_pid, pid_t sid,
                    mode_t file_mask)
{
    Daemon *daemon = malloc(sizeof(Daemon));

    daemon->identify_name = identify_name;
    daemon->run_user = run_user;
    daemon->run_directory = run_directory;
    daemon->lock_file = lock_file;
    daemon->pid_file = pid_file;

    daemon->pid = pid;
    daemon->parent_pid = parent_pid;
    daemon->sid = sid;

    daemon->file_mask = file_mask;

    daemon->options = NULL;

    return daemon;
}

void Daemon_init_options(Daemon *daemon, int option_count, char **option_values)
{
    daemon->options = Options_create(option_count, option_values);

    int i;
    for (i = 0; i < Options_get_count(daemon->options); i++) {
        syslog(LOG_NOTICE, "%d: %s", i, Options_get_value(daemon->options, i));
    }
}

void Daemon_destroy(Daemon *daemon)
{
    if (daemon != NULL) {
        Options_destroy(daemon->options);

        free(daemon);
        daemon = NULL;
    }
}

Daemon *Daemon_create(const char *identify_name,
                      const char *run_user,
                      const char *run_directory,
                      const char *lock_file,
                      const char *pid_file)
{
    // return if we're already daemonized
    if (getppid() == 1) {
        syslog(LOG_INFO, "instance is already daemonized");

        return NULL;
    }

    init_log(identify_name);
    syslog(LOG_INFO, "started");

    pid_t parent_pid, sid;
    mode_t file_mask = 0;

    if (create_lock_file(lock_file)
        && set_run_user(run_user)
        && set_trapped_signals()
        && fork_active_process(&parent_pid)
        && set_ignored_signals()
        && set_file_mask(file_mask)
        && start_new_session(&sid)
        && create_pid_file(pid_file)
        && set_run_directory(run_directory)
        && redirect_io()
        && close_parent_process(parent_pid)
    ) {
        pid_t pid = getpid();

        return init(identify_name, run_user, run_directory, lock_file, pid_file,
                    pid, parent_pid, sid,
                    file_mask);
    }

    return NULL;
}

void Daemon_run(Daemon *daemon)
{
    while (1) {
        sleep_ms(100);
        update(daemon);
    }

    exit_graceful(EXIT_STATUS_SUCCESS);
}
