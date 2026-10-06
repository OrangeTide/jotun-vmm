/* cas-topic.c : signed topics over CAS refs */
/* PUBLIC DOMAIN (CC0-1.0) */

#include "cas-topic.h"

#include <stdio.h>
#include <string.h>

static int
label_ok(const char *label, size_t len)
{
    if (len == 0 || len > CAS_TOPIC_LABEL_MAX)
        return 0;

    /* A leading '.' would make a hidden file of the ref, and a leading
     * '-' reads as an option to anything that passes a ref name to a
     * command line. */
    if (label[0] == '.' || label[0] == '-')
        return 0;

    for (size_t i = 0; i < len; i++) {
        char c = label[i];

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            continue;
        return 0;
    }
    return 1;
}

int
cas_topic_ref_name(char *out, size_t outsz,
                   const unsigned char topic_id[CAS_HASH_LEN],
                   const char *label)
{
    if (!out || !topic_id)
        return CAS_ERR;

    char hex[CAS_HASH_HEX + 1];

    cas_hex_encode(topic_id, CAS_HASH_LEN, hex);

    int n;

    if (!label || label[0] == '\0') {
        n = snprintf(out, outsz, "%s", hex);
    } else {
        if (!label_ok(label, strlen(label)))
            return CAS_ERR;
        n = snprintf(out, outsz, "%s-%s", hex, label);
    }

    if (n < 0 || (size_t)n >= outsz)
        return CAS_ERR;
    return CAS_OK;
}

/** Load and verify the record stored at `addr`. */
static int
load_record(struct cas_tree *ct, const char *addr, struct cas_vrec *out)
{
    struct cas_file cf;
    char type[CAS_TYPE_MAX + 1];
    int rc = cas_open_object(cas_tree_cas(ct), &cf, addr, type,
                             sizeof(type));

    if (rc != CAS_OK)
        return rc;

    if (strcmp(type, CAS_VREC_TYPE) != 0) {
        cas_close(&cf);
        return CAS_ETYPE;
    }

    rc = cas_vrec_decode(cf.data, cf.len, out);
    cas_close(&cf);
    return rc;
}

int
cas_topic_head(struct cas_tree *ct, const char *refname,
               struct cas_vrec *out, char *addr_out)
{
    if (!ct || !refname || !out)
        return CAS_ERR;

    char addr[CAS_HASH_HEX + 1];
    int rc = cas_tree_ref_read(ct, refname, addr);

    if (rc != CAS_OK)
        return rc;

    /* The ref names a record, never a root, so every resolution of a
     * topic passes through a signature check.
     *
     * This is a deliberate exception to ATOLL A4.0, which says an object
     * already in the depot is trusted and that re-checking on read buys
     * nothing against an adversary who never got past admission.  That
     * reasoning holds, and the check stays anyway: a head record's
     * entire value is its signature, it is read rarely, and one
     * verification is cheap.  It is defence in depth, not distrust of
     * the depot, and it does not replace what fsck answers. */
    rc = load_record(ct, addr, out);
    if (rc != CAS_OK)
        return rc;

    if (addr_out)
        memcpy(addr_out, addr, CAS_HASH_HEX + 1);
    return CAS_OK;
}

int
cas_topic_publish(struct cas_tree *ct, const char *refname,
                  const unsigned char rec[CAS_VREC_LEN],
                  const char *comment)
{
    if (!ct || !refname || !rec)
        return CAS_ERR;

    struct cas_vrec cand;
    int rc = cas_vrec_decode(rec, CAS_VREC_LEN, &cand);

    if (rc != CAS_OK)
        return rc;

    struct cas_vrec cur;
    char cur_addr[CAS_HASH_HEX + 1];
    int have_cur = 0;

    rc = cas_topic_head(ct, refname, &cur, cur_addr);
    if (rc == CAS_OK)
        have_cur = 1;
    else if (rc != CAS_ENOTFOUND)
        return rc;

    rc = cas_vrec_succeeds(have_cur ? &cur : NULL,
                           have_cur ? cur_addr : NULL, &cand);
    if (rc != CAS_OK)
        return rc;

    char addr[CAS_HASH_HEX + 1];

    rc = cas_vrec_address(rec, addr);
    if (rc != CAS_OK)
        return rc;

    /* Through the trust boundary rather than around it.  A record is
     * self-addressed, so this is the same hash check cas_tree_put_checked
     * applies to any raw object; going through it means one place
     * decides what enters a depot. */
    rc = cas_tree_put_checked(ct, CAS_VREC_TYPE, rec, CAS_VREC_LEN,
                              addr);
    if (rc != CAS_OK)
        return rc;

    /* The ref moves last.  If this fails the record is already stored,
     * which is harmless: an unreferenced record is collectable, whereas
     * a ref pointing at a record that was never written would not be. */
    return cas_tree_ref_commit(ct, refname, addr, comment);
}
