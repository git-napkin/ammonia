#pragma once
#include <stdbool.h>

typedef struct {
    bool disablePAC;
    bool useLegacyAmmonia;
    bool pauseInjection;
} FangsOptions;

FangsOptions fangs_load_options(void);
void fangs_watch_options(void (*on_change)(void));
