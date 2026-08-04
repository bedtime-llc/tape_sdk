#pragma once
/*
 * Minimal <stdio.h> for TAPP builds.
 *
 * snprintf is the ONLY formatting entry point the firmware exports.
 * printf / sprintf / vsnprintf / puts / fprintf / fopen are NOT exported — a tapp that calls them
 * links fine but fails to load on device, so they are deliberately not declared here.
 * (File I/O goes through the storage_* API in tapp_api.h.)
 */

#include <stdarg.h>
#include <stddef.h>

int snprintf(char* buf, size_t n, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
