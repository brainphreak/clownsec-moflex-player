/* The 3D video store, entered from OPEN VIDEO ("Walk the Aisle").
 * Builds the shop from the library's moviedata, runs until the user walks out (START) or picks a
 * case up and presses Y. resolve(key) maps a case's moviedata key (filename without extension)
 * to the movie's path. Returns 1 with out[] set, 0 when they walked out, -1 if the app is closing.
 * Owns citro3d for its duration and releases all of it before returning. */
#ifndef STORE_H
#define STORE_H
#include <stddef.h>
int store_run(int (*resolve)(const char *key, char *out, size_t cap), char *out, size_t cap);
#endif
