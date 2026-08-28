#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

bool depacify_file_in_place(const char *file_path);
bool strip_code_signature_file(const char *path);
bool sign_file(const char *path, void *entitlements_blob);
bool file_is_arm64e(const char *path);
