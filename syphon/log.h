#pragma once
#include <syslog.h>

#define log_info(fmt, ...)  syslog(LOG_INFO,  fmt, __VA_ARGS__)
#define log_error(fmt, ...) syslog(LOG_ERR, fmt, __VA_ARGS__)
