#include "daemon.h"


int main(int argc, char **argv)
{
    Daemon *daemon = Daemon_create("mydaemon",
                                   "root",
                                   "/",
                                   "/var/lock",
                                   "/var/run");

    if (daemon) {
        Daemon_init_options(daemon, argc, argv);

        Daemon_run(daemon);

        Daemon_destroy(daemon);
    }

    return 0;
}
