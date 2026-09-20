#ifndef VAJRA_PACKAGES_H
#define VAJRA_PACKAGES_H

#include <stdint.h>

/* Phase 20: package installation through the quarantine pipeline.
 *
 * The "repository" is a small built-in catalog (core/packages.c). An
 * install is NOT "copy and trust": packages_stage() creates an ordinary
 * UNTRUSTED object, an actor with CAP_INSTALL_PACKAGE has it inspected
 * by a sandboxed inspector, and only then does packages_verdict()
 * promote it (or reject it for good). Nothing loadable exists before the
 * verdict -- the loader refuses anything that isn't OBJ_TRUSTED. */

/* States reported by packages_info(). */
#define PKG_NOT_INSTALLED 0
#define PKG_INSTALLED     1 /* an object of that name exists and is OBJ_TRUSTED */
#define PKG_REJECTED      2 /* failed inspection -- permanently unusable */
#define PKG_UNVETTED      3 /* staged but no verdict yet */

/* Filled by SYS_PKG_LIST. name is always NUL-terminated. */
struct pkg_info {
    char name[16];
    uint32_t size;
    uint32_t state;
};

int packages_count(void);
int packages_info(int index, struct pkg_info *out);

/* Creates the object and writes the package bytes into it (it stays
 * OBJ_UNTRUSTED). Returns the object id, -1 (no space / bad index), or
 * -2 (an object with that name already exists). Remembers that this id
 * was staged by the installer -- see packages_verdict(). */
int packages_stage(int index);

/* The installer's ONLY promotion path: promotes (pass != 0) to
 * OBJ_TRUSTED or rejects (pass == 0) object `id`, but ONLY if `id` is an
 * object packages_stage() created and that has had no verdict yet.
 * Anything else is refused (-1), so the authority to install packages is
 * not, and cannot be used as, authority to promote arbitrary objects.
 * Returns the resulting trust level, or -1. */
int packages_verdict(int id, int pass);

#endif
