#ifndef VAJRA_USERLAND_RUNTIME_H
#define VAJRA_USERLAND_RUNTIME_H

/* ------------------------------------------------------------------
 * Roadmap Phase 16: the minimal userland runtime -- the real,
 * documented ABI a genuinely separately-compiled program links
 * against, replacing the ad hoc user_write()/user_spawn()-style
 * wrappers hand-written per demo actor in core/main.c. Every program
 * under src/userland/ compiles and links against this the same way;
 * core/main.c's own wrappers stay exactly as they are (the built-in
 * demo actors are still ordinary kernel-linked functions, not loaded
 * programs -- this doesn't replace that, it's the NEW path for code
 * that didn't exist when the kernel was compiled).
 *
 * Deliberately tiny, matching this milestone's own minimal scope: just
 * enough for a genuine "hello world" to prove the loader works end to
 * end. Grows as later programs need more of hal.h's syscall surface.
 * ---------------------------------------------------------------- */

void user_write(const char *str);
void user_write_int(long long value);
void user_exit(void);

#endif
