#include "envbuf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int envbuf_len(const char *envp[]) {
    if (envp == NULL)
        return 0;

    int k = 0;
    while (envp[k] != NULL) {
        k++;
    }
    return k + 1;
}

char **envbuf_mutcopy(const char *envp[]) {
    if (envp == NULL)
        return NULL;

    int len = envbuf_len(envp);
    char **envcopy = malloc(len * sizeof(char *));

    for (int i = 0; i < len - 1; i++) {
        envcopy[i] = strdup(envp[i]);
    }
    envcopy[len - 1] = NULL;

    return envcopy;
}

void envbuf_free(char *envp[]) {
    if (envp == NULL)
        return;

    int len = envbuf_len((const char **)envp);
    for (int i = 0; i < len - 1; i++) {
        free(envp[i]);
    }
    free(envp);
}

int envbuf_find(const char *envp[], const char *name) {
    if (envp) {
        size_t nameLen = strlen(name);
        int k = 0;
        const char *env = envp[k++];
        while (env != NULL) {
            size_t envLen = strlen(env);
            if (envLen > nameLen) {
                if (!strncmp(env, name, nameLen)) {
                    if (env[nameLen] == '=') {
                        return k - 1;
                    }
                }
            }
            env = envp[k++];
        }
    }
    return -1;
}

const char *envbuf_getenv(const char *envp[], const char *name) {
    if (envp) {
        size_t nameLen = strlen(name);
        int envIndex = envbuf_find(envp, name);
        if (envIndex >= 0) {
            return &envp[envIndex][nameLen + 1];
        }
    }
    return NULL;
}

char **envbuf_setenv(char **envp, const char *name, const char *value) {
    if (!name || !value)
        return envp;

    if (!envp) {
        envp = malloc(sizeof(char *));
        if (!envp)
            return NULL;
        envp[0] = NULL;
    }

    char *envToSet = malloc(strlen(name) + strlen(value) + 2);
    if (!envToSet)
        return envp;
    sprintf(envToSet, "%s=%s", name, value);

    int existingEnvIndex = envbuf_find((const char **)envp, name);
    if (existingEnvIndex >= 0) {
        free(envp[existingEnvIndex]);
        envp[existingEnvIndex] = envToSet;
        return envp;
    }

    int prevLen = envbuf_len((const char **)envp);
    char **tmp = realloc(envp, (prevLen + 1) * sizeof(char *));
    if (!tmp) {
        free(envToSet);
        return envp;
    }
    envp = tmp;
    envp[prevLen - 1] = envToSet;
    envp[prevLen] = NULL;
    return envp;
}

char **envbuf_unsetenv(char **envp, const char *name) {
    if (!envp || !name)
        return envp;

    int existingEnvIndex = envbuf_find((const char **)envp, name);
    if (existingEnvIndex < 0)
        return envp;

    free(envp[existingEnvIndex]);
    int prevLen = envbuf_len((const char **)envp);
    for (int i = existingEnvIndex; i < (prevLen - 1); i++) {
        envp[i] = envp[i + 1];
    }
    char **tmp = realloc(envp, (prevLen - 1) * sizeof(char *));
    return tmp ? tmp : envp;
}
