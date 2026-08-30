#pragma once
#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/syslimits.h>

bool path_ends_with(const char *path, const char *name);
bool ammonia_bootargs_has_safe_mode(const char *args);
bool ammonia_in_safe_boot(void);
bool path_matches_entry(const char *path, const char *entry);
bool is_safe_filename(const char *name);
bool check_file_read(FILE *f, void *buf, size_t len);
uint32_t swap32_if(uint32_t val, bool swap);
bool macho_has_framework(const char *base, size_t size, const char *framework);
bool exe_links_to_framework(const char *exe_path, const char *framework);
bool process_has_framework(const char *framework);
bool check_dylib_options(const char *dir, const char *name, const char *exe);
bool check_list_match(const char *path, const char *exe);
bool is_tweak_stat_safe(const struct stat *st);
bool is_tweak_safe(const char *full_path);
bool should_load_tweak(const char *dir, const char *name, const char *exe);
char *get_exe_path(void);
bool is_tweak_enabled(const char *name);
void clear_tweak_enabled_cache(void);
CFDictionaryRef fangs_read_plist_dictionary(const char *path);
