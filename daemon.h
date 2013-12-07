#ifndef DAEMON_H
#define DAEMON_H


typedef enum EXIT_STATUS EXIT_STATUS;

typedef enum RUN_STATUS RUN_STATUS;

typedef struct Daemon Daemon;

extern Daemon *Daemon_create(const char *, const char *, const char *, const char *);
extern void Daemon_destroy(Daemon *);

extern void Daemon_init_options(Daemon *, int, char **);
extern void Daemon_run(Daemon *);

#endif
