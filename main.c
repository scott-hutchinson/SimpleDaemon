#include "daemon.h"


int main(int argc, char **argv)
{
    Daemon *daemon = Daemon_create("mydaemon",
                                   "scott",
                                   "/",
                                   "/var/lock/subsys/mydaemon");

    if (daemon) {
        Daemon_init_options(daemon, argc, argv);

        Daemon_run(daemon);

        Daemon_destroy(daemon);
    }

    return 0;
}
