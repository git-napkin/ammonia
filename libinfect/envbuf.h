#pragma once

int envbuf_len(const char *envp[]);
char **envbuf_mutcopy(const char *envp[]);
void envbuf_free(char *envp[]);

int envbuf_find(const char *envp[], const char *name);
const char *envbuf_getenv(const char *envp[], const char *name);

char **envbuf_setenv(char **envp, const char *name, const char *value);
char **envbuf_unsetenv(char **envp, const char *name);
