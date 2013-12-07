#include "options.h"

#include <stdlib.h>
#include <stdio.h>


typedef struct Options {
    int count;
    char **values;
} Options;

Options *Options_create(int argument_count, char **argument_values)
{
    Options *options = malloc(sizeof(Options));

    // only count options, don't count the command itself
    options->count = argument_count - 1;

    // increment to the first option, to avoid saving the command itself
    argument_values++;

    options->values = argument_values;

    return options;
}

void Options_destroy(Options *options)
{
    if (options != NULL) {
        free(options);

        options = NULL;
    }
}

int Options_get_count(Options *options)
{
    return options->count;
}

const char *Options_get_value(Options *options, int index)
{
    if (options->values[index] != NULL) {
        return options->values[index];
    }

    return NULL;
}
