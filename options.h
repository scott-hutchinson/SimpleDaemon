#ifndef OPTIONS_H
#define OPTIONS_H


typedef struct Options Options;

extern Options *Options_create(int, char **);
extern void Options_destroy(Options *);

extern int Options_get_count(Options *);
extern const char *Options_get_value(Options *, int);

#endif
