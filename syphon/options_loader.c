#include "options_loader.h"
#include "tweak_utils.h"
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#define OPTIONS_PATH "/opt/pluginplayground/current.options"

FangsOptions fangs_load_options(void) {
    FangsOptions opts = {false, false, false};
    CFDictionaryRef dict = fangs_read_plist_dictionary(OPTIONS_PATH);
    if (!dict)
        return opts;

    CFBooleanRef val;
    val = (CFBooleanRef)CFDictionaryGetValue(dict, CFSTR("disablePAC"));
    if (val && CFGetTypeID(val) == CFBooleanGetTypeID())
        opts.disablePAC = (bool)CFBooleanGetValue(val);

    val = (CFBooleanRef)CFDictionaryGetValue(dict, CFSTR("useLegacyAmmonia"));
    if (val && CFGetTypeID(val) == CFBooleanGetTypeID())
        opts.useLegacyAmmonia = (bool)CFBooleanGetValue(val);

    val = (CFBooleanRef)CFDictionaryGetValue(dict, CFSTR("pauseInjection"));
    if (val && CFGetTypeID(val) == CFBooleanGetTypeID())
        opts.pauseInjection = (bool)CFBooleanGetValue(val);

    CFRelease(dict);
    return opts;
}

static void (*g_watch_cb)(void);
static dispatch_source_t g_watcher;
static atomic_uint_fast64_t g_watch_gen;

static void watch_start(void);

static void watch_retry(void) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC),
                   dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0),
                   ^{ watch_start(); });
}

static void watch_start(void) {
    if (g_watcher) {
        dispatch_source_cancel(g_watcher);
        g_watcher = NULL;
    }

    int fd = open(OPTIONS_PATH, O_EVTONLY);
    if (fd < 0) {
        syslog(LOG_WARNING, "options: cannot watch %s: %s", OPTIONS_PATH,
               strerror(errno));
        watch_retry();
        return;
    }

    dispatch_source_t source = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_VNODE, (uintptr_t)fd,
        DISPATCH_VNODE_WRITE | DISPATCH_VNODE_EXTEND | DISPATCH_VNODE_DELETE |
            DISPATCH_VNODE_RENAME | DISPATCH_VNODE_ATTRIB,
        dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0));
    if (!source) {
        close(fd);
        watch_retry();
        return;
    }

    dispatch_source_set_event_handler(source, ^{
      unsigned long flags = dispatch_source_get_data(source);
      uint64_t gen = atomic_fetch_add(&g_watch_gen, 1) + 1;
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),
                     dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0),
                     ^{
                       if (atomic_load(&g_watch_gen) == gen && g_watch_cb)
                           g_watch_cb();
                     });
      if (flags & (DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME)) {
          dispatch_source_cancel(source);
          g_watcher = NULL;
          watch_start();
      }
    });
    dispatch_source_set_cancel_handler(source, ^{ close(fd); });
    dispatch_resume(source);
    g_watcher = source;
}

void fangs_watch_options(void (*on_change)(void)) {
    g_watch_cb = on_change;
    watch_start();
}
