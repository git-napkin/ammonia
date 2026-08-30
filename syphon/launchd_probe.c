#include <syslog.h>
#include <unistd.h>

__attribute__((constructor)) static void launchd_probe_init(void) {
    openlog("launchd_probe", LOG_PID | LOG_NDELAY, LOG_DAEMON);
    syslog(LOG_NOTICE, "launchd_probe: loaded in pid %d", (int)getpid());
}
