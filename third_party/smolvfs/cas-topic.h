/* cas-topic.h : signed topics over CAS refs */
/* PUBLIC DOMAIN (CC0-1.0) */

#ifndef CAS_TOPIC_H
#define CAS_TOPIC_H

#include "cas-sign.h"
#include "cas-tree.h"

/****************************************************************
 * What this is
 ****************************************************************
 *
 * A version record (cas-sign.h) says who published a root.  A topic is
 * the mutable name that tracks the newest such record, and this module
 * is the join: it keeps a topic's head in an ordinary local ref, so the
 * head pointer, its update log, and crash rollback all come from the
 * ref machinery already in cas-tree.
 *
 * The ref holds the address of the head *record*, not of the root.
 * Resolving a topic therefore always goes through a signature check,
 * and there is no path that reaches a root without one.
 */

/****************************************************************
 * Naming
 ****************************************************************
 *
 * SHOAL writes a topic as `server-id/topic`, which cannot be a ref name:
 * valid_ref_name rejects '/' and '\\' outright, refs are flat files
 * under refs/, and ref enumeration does not descend.
 *
 * Allowing a separator would mean relaxing the one validator whose rule
 * is currently "no separators, ever", teaching ref writes to create
 * directories, and making enumeration recursive, all to support a
 * hierarchy nothing here needs.  A path validator that says no to every
 * separator is worth more than the hierarchy is.
 *
 * So the two halves are encoded into one flat name instead:
 *
 *     <64 hex topic id>[-<label>]
 *
 * The topic id leads, so a local ref cannot confuse two publishers who
 * chose the same label.  The label is restricted rather than escaped,
 * because a charset a reader can check by eye is worth more than one
 * that round-trips every byte.
 */

#define CAS_TOPIC_LABEL_MAX 64

/** Build the local ref name for a topic.
 *
 *  `label` may be NULL or empty for a publisher's single topic.  When
 *  present it must be 1 to CAS_TOPIC_LABEL_MAX characters of
 *  [A-Za-z0-9._-], and must not start with '.' or '-'.
 *
 *  Returns CAS_OK, or CAS_ERR if the label is unacceptable or the
 *  buffer is too small.  Needs CAS_HASH_HEX + 2 + CAS_TOPIC_LABEL_MAX
 *  bytes at most.
 */
int
cas_topic_ref_name(char *out, size_t outsz,
                   const unsigned char topic_id[CAS_HASH_LEN],
                   const char *label);

/** Read a topic's current head.
 *
 *  Resolves the ref, loads the record it names, and verifies it.  A
 *  topic with no ref yet is CAS_ENOTFOUND, which is an ordinary state
 *  and not an error to report upward.
 *
 *  `addr_out` may be NULL; when given it needs CAS_HASH_HEX + 1 bytes
 *  and receives the head record's address.
 */
int
cas_topic_head(struct cas_tree *ct, const char *refname,
               struct cas_vrec *out, char *addr_out);

/** Publish a record as a topic's new head.
 *
 *  Verifies the record, checks it against the current head, admits it
 *  through the trust boundary, and only then moves the ref.  Nothing is
 *  stored and the ref does not move unless every step passes.
 *
 *  Returns CAS_OK, or the reason it was refused.  The succession codes
 *  come back unchanged, because the caller has to tell them apart:
 *  CAS_SIGN_EFORK means the topic's key signed two different records at
 *  one seq and something is badly wrong, while CAS_SIGN_EGAP only means
 *  intermediate records have not been seen and may be worth fetching.
 */
int
cas_topic_publish(struct cas_tree *ct, const char *refname,
                  const unsigned char rec[CAS_VREC_LEN],
                  const char *comment);

#endif /* CAS_TOPIC_H */
