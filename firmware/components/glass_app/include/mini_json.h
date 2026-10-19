/**
 * Minimal JSON reader for the Open-Meteo response.
 *
 * A full DOM parser is not worth the flash and the heap when we need a dozen
 * numbers: this walks the document, matches keys exactly, and pulls scalars or
 * parallel arrays. It is deliberately strict about string boundaries so a
 * value inside a string ("temperature_2m" appearing in a units block) cannot
 * be mistaken for the real key.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start of the object or array value stored under `key`, or nullptr. */
const char* json_find(const char* json, size_t len, const char* key, size_t* out_len);

/** Number under `key` within a scope; returns false when absent or not numeric. */
bool json_number(const char* scope, size_t scope_len, const char* key, double* out);

/** Integer under `key`. */
bool json_int(const char* scope, size_t scope_len, const char* key, long* out);

/**
 * Copy the string under `key` into `out`. Returns false when absent; always
 * NUL-terminates when out_size > 0.
 */
bool json_string(const char* scope, size_t scope_len, const char* key, char* out,
                 size_t out_size);

/**
 * Read up to `max` numbers from the array stored under `key`. Returns how many
 * were read.
 */
int json_array_numbers(const char* scope, size_t scope_len, const char* key,
                       double* out, int max);

/** Read up to `max` strings from the array under `key`. */
int json_array_strings(const char* scope, size_t scope_len, const char* key,
                       char out[][32], int max);

#ifdef __cplusplus
}
#endif