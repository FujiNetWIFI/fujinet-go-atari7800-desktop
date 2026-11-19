/* The key/gamepad bindings table (bindings.c). */
#ifndef A7800_BINDINGS_H
#define A7800_BINDINGS_H

#include "a7800session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reloads the table: every slot to its default, then whatever "bindings"
 * holds in s's settings store. Called once per a7800session_new. The table
 * is process-global (like gamepad_sdl.c's pad table). */
void bindings_init(struct a7800session *s);
/* The table must not keep saving into a session that has been freed. */
void bindings_shutdown(struct a7800session *s);

#ifdef __cplusplus
}
#endif

#endif
