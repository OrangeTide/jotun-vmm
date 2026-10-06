/* jimage.h : the jotun guest-image manifest, a small text object stored in the
 * CAS that names a root tree per architecture. The object's own hash is the
 * distributable image identity. The host reads only the format version and the
 * arch rows; every other field is informational. */
#ifndef JIMAGE_H
#define JIMAGE_H

#include <stddef.h>

#define JIMAGE_MAGIC   "jimage"
#define JIMAGE_TYPE    "jimage"     /* the CAS object type */
#define JIMAGE_VERSION 1            /* the format version this host understands */

#define JIMAGE_MAXARCH 8
#define JIMAGE_ARCHLEN 16
#define JIMAGE_HASHLEN 65           /* 64 hex digits + NUL */

struct jimage_arch {
    char arch[JIMAGE_ARCHLEN];
    char root[JIMAGE_HASHLEN];
};

struct jimage {
    int version;                            /* from the "jimage <n>" magic line */
    char name[64];                          /* the "name" field, or "" */
    char ver[64];                           /* the "version" field, or "" */
    struct jimage_arch archs[JIMAGE_MAXARCH];
    int narch;
};

/* Parse a jimage text object. Returns 0 on success, -1 if the magic line is
 * missing or names a version this host does not understand. Fields are
 * "key value" lines; a line beginning with whitespace folds onto the previous
 * field's value (RFC 822 / deb822 style) and a blank line is ignored. Unknown
 * keys are ignored so the format can grow. */
int jimage_parse(const char *text, size_t len, struct jimage *im);

/* Return the root tree hash for arch, or NULL if the image has no such arch. */
const char *jimage_root_for(const struct jimage *im, const char *arch);

#endif /* JIMAGE_H */
