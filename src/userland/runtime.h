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

/* Phase 19 additions -- see runtime.c. */
#include <stdint.h>
struct message;
struct object_info;
struct actor_info;
void user_receive(struct message *out);
int user_lookup_name(const char *name);
int user_list_objects(int index, struct object_info *out);
int user_object_read(int id, void *buf, uint32_t len);
int user_object_write(int id, const void *buf, uint32_t len);
int user_create_name(const char *name);
int user_rename_object(int id, const char *new_name);
int user_delete_name(int id);
int user_key_read(void);
void user_sleep(uint64_t ticks);
int user_actor_info(int slot, struct actor_info *out);
int user_object_read_at(int id, uint32_t off, void *buf, uint32_t len);
int user_object_write_at(int id, uint32_t off, const void *buf, uint32_t len);
int user_object_protect(int id, int flags);
void *user_heap_grow(int pages);
struct rtc_time;
struct kernel_stats;
struct core_info;
int user_terminate(int slot);
void user_rtc_read(struct rtc_time *out);
int user_kernel_stats(struct kernel_stats *out);
int user_core_info(int count, struct core_info *out);

#endif
