#include "vajra/packages.h"
#include "vajra/storage.h"

/* The package bytes. greeter and calculator are the two real,
 * separately-compiled programs already embedded in the kernel image
 * (hal/x86_64/hello_blob.asm, calc_blob.asm); trojan is a deliberately
 * hostile package -- a "text editor" that carries the marker the
 * inspector looks for -- so the pipeline has something to catch. */
extern uint8_t hello_blob[];
extern uint8_t hello_blob_end[];
extern uint8_t calc_blob[];
extern uint8_t calc_blob_end[];

static const uint8_t trojan_bytes[] =
    "BADSTUFF: pretends to be a text editor, really rewrites your disk";

struct catalog_entry {
    const char *name;
    const uint8_t *bytes; /* NULL: use the extern blob pair below */
    uint32_t size;
};

static struct catalog_entry catalog[3];
static int catalog_ready;

static void catalog_init(void) {
    if (catalog_ready) {
        return;
    }
    catalog[0].name = "greeter";
    catalog[0].bytes = hello_blob;
    catalog[0].size = (uint32_t)(hello_blob_end - hello_blob);
    catalog[1].name = "calculator";
    catalog[1].bytes = calc_blob;
    catalog[1].size = (uint32_t)(calc_blob_end - calc_blob);
    catalog[2].name = "trojan";
    catalog[2].bytes = trojan_bytes;
    catalog[2].size = (uint32_t)(sizeof(trojan_bytes) - 1);
    catalog_ready = 1;
}

/* Objects packages_stage() created that have not had a verdict yet.
 * Stored with the object's generation so a deleted-and-reused id never
 * inherits the mark. */
#define STAGED_MAX 32
static int staged_gen[STAGED_MAX]; /* 0 = not staged */

int packages_count(void) {
    catalog_init();
    return 3;
}

int packages_info(int index, struct pkg_info *out) {
    catalog_init();
    if (index < 0 || index >= 3) {
        return -1;
    }
    int i = 0;
    for (; i < 15 && catalog[index].name[i]; i++) {
        out->name[i] = catalog[index].name[i];
    }
    out->name[i] = 0;
    out->size = catalog[index].size;
    out->state = PKG_NOT_INSTALLED;
    int id = storage_lookup_by_name(catalog[index].name);
    if (id >= 0) {
        int trust = storage_get_trust(id);
        if (trust == OBJ_TRUSTED) {
            out->state = PKG_INSTALLED;
        } else if (trust == OBJ_REJECTED) {
            out->state = PKG_REJECTED;
        } else {
            out->state = PKG_UNVETTED;
        }
    }
    return 0;
}

int packages_stage(int index) {
    catalog_init();
    if (index < 0 || index >= 3) {
        return -1;
    }
    if (storage_lookup_by_name(catalog[index].name) >= 0) {
        return -2;
    }
    int id = storage_create_named(catalog[index].name);
    if (id < 0) {
        return -1;
    }
    if (storage_write(id, catalog[index].bytes, catalog[index].size) < 0) {
        storage_delete(id);
        return -1;
    }
    if (id < STAGED_MAX) {
        staged_gen[id] = storage_object_generation(id);
    }
    return id;
}

int packages_verdict(int id, int pass) {
    if (id < 0 || id >= STAGED_MAX || staged_gen[id] == 0 ||
        staged_gen[id] != storage_object_generation(id)) {
        return -1; /* not something the installer staged, or not any more */
    }
    int trust = storage_get_trust(id);
    if (trust == OBJ_TRUSTED || trust == OBJ_REJECTED) {
        return -1;
    }
    staged_gen[id] = 0; /* one verdict per staged object */
    if (!pass) {
        storage_reject(id);
        return OBJ_REJECTED;
    }
    while (storage_get_trust(id) != OBJ_TRUSTED) {
        if (storage_promote(id) < 0) {
            break;
        }
    }
    return storage_get_trust(id);
}
