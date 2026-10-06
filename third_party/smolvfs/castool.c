/* castool.c : CLI tool for CAS-tree administration and debugging */
/* Copyright (c) 2026 Jon Mayo <jon@rm-f.net>
 * Licensed under BSD-2-Clause-Patent OR MIT */

#define _POSIX_C_SOURCE 200809L

#include "cas-tree.h"
#include "cas-topic.h"
#include "cas-pack.h"
#include "cas-codec.h"
#include "version.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *progname = "castool";

/****************************************************************
 * Argument expansion (@file)
 ****************************************************************/

struct arglist {
	char **args;
	int count;
	int cap;
};

static void
arglist_init(struct arglist *al)
{
	al->args = NULL;
	al->count = 0;
	al->cap = 0;
}

static void
arglist_free(struct arglist *al)
{
	for (int i = 0; i < al->count; i++)
		free(al->args[i]);
	free(al->args);
	al->args = NULL;
	al->count = 0;
	al->cap = 0;
}

static int
arglist_add(struct arglist *al, const char *s)
{
	if (al->count >= al->cap) {
		int newcap = al->cap ? al->cap * 2 : 16;
		char **p = realloc(al->args, (size_t)newcap * sizeof(*p));

		if (!p)
			return -1;
		al->args = p;
		al->cap = newcap;
	}
	al->args[al->count] = strdup(s);
	if (!al->args[al->count])
		return -1;
	al->count++;
	return 0;
}

static int
expand_at_file(struct arglist *al, const char *path)
{
	FILE *fp;
	int opened = 0;  /* we opened fp, so we have to close it */

	if (strcmp(path, "-") == 0) {
		fp = stdin;
	} else {
		fp = fopen(path, "r");
		opened = 1;
	}

	if (!fp) {
		fprintf(stderr, "%s: cannot open '@%s': %s\n",
		        progname, path, strerror(errno));
		return -1;
	}

	char line[4096];

	while (fgets(line, sizeof(line), fp)) {
		size_t len = strlen(line);

		if (len > 0 && line[len - 1] == '\n')
			line[--len] = '\0';
		if (len == 0)
			continue;
		if (arglist_add(al, line) != 0) {
			if (opened)
				fclose(fp);
			return -1;
		}
	}

	/* Keyed off the flag rather than an fp != stdin test.  The test says
	 * the same thing to a reader, but it leaves gcc's analyzer exploring
	 * a path where fopen returned stdin and the handle is never closed,
	 * which it reports as a leak. */
	if (opened)
		fclose(fp);
	return 0;
}

static int
expand_args(struct arglist *al, int argc, char **argv)
{
	for (int i = 0; i < argc; i++) {
		if (argv[i][0] == '@') {
			if (expand_at_file(al, argv[i] + 1) != 0)
				return -1;
		} else {
			if (arglist_add(al, argv[i]) != 0)
				return -1;
		}
	}
	return 0;
}

/****************************************************************
 * Helpers
 ****************************************************************/

static int
is_hex_hash(const char *s)
{
	if (strlen(s) != CAS_HASH_HEX)
		return 0;
	for (int i = 0; i < CAS_HASH_HEX; i++) {
		char c = s[i];

		if (!((c >= '0' && c <= '9') ||
		      (c >= 'a' && c <= 'f') ||
		      (c >= 'A' && c <= 'F')))
			return 0;
	}
	return 1;
}

static int
resolve_root(struct cas_tree *ct, const char *arg,
             char *hash_out)
{
	if (is_hex_hash(arg)) {
		memcpy(hash_out, arg, CAS_HASH_HEX + 1);
		return 0;
	}
	if (cas_tree_ref_read(ct, arg, hash_out) == CAS_OK)
		return 0;
	fprintf(stderr, "%s: cannot resolve '%s' as hash or ref\n",
	        progname, arg);
	return -1;
}

static unsigned char *
read_file(const char *path, size_t *out_len)
{
	FILE *fp = fopen(path, "rb");

	if (!fp) {
		fprintf(stderr, "%s: cannot open '%s': %s\n",
		        progname, path, strerror(errno));
		return NULL;
	}

	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);

	if (sz < 0) {
		fclose(fp);
		return NULL;
	}
	fseek(fp, 0, SEEK_SET);

	unsigned char *buf = malloc(sz > 0 ? (size_t)sz : 1);

	if (!buf) {
		fclose(fp);
		return NULL;
	}

	if (sz > 0 && fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
		free(buf);
		fclose(fp);
		return NULL;
	}

	fclose(fp);
	*out_len = (size_t)sz;
	return buf;
}

static unsigned char *
read_stdin(size_t *out_len)
{
	size_t cap = 4096;
	unsigned char *buf = malloc(cap);

	if (!buf)
		return NULL;

	size_t len = 0;
	size_t n;

	while ((n = fread(buf + len, 1, cap - len, stdin)) > 0) {
		len += n;
		if (len >= cap) {
			cap *= 2;
			unsigned char *p = realloc(buf, cap);

			if (!p) {
				free(buf);
				return NULL;
			}
			buf = p;
		}
	}

	*out_len = len;
	return buf;
}

static const char *
basename_of(const char *path)
{
	const char *p = strrchr(path, '/');

	return p ? p + 1 : path;
}

/****************************************************************
 * refs
 ****************************************************************/

static int
print_ref(const char *name, void *ctx)
{
	(void)ctx;
	printf("%s\n", name);
	return 0;
}

static int
cmd_refs(struct cas_tree *ct, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	cas_tree_ref_foreach(ct, print_ref, NULL);
	return 0;
}

/****************************************************************
 * log
 ****************************************************************/

static int
print_log_entry(const char *hash, int64_t time_s, int32_t time_ns,
                const char *comment, void *ctx)
{
	(void)time_ns;
	(void)ctx;

	struct tm tm;
	time_t t = (time_t)time_s;

	localtime_r(&t, &tm);

	char tbuf[32];

	strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
	if (comment[0])
		printf("%s  %s  %s\n", hash, tbuf, comment);
	else
		printf("%s  %s\n", hash, tbuf);
	return 0;
}

static int
cmd_log(struct cas_tree *ct, int argc, char **argv)
{
	if (argc < 1) {
		fprintf(stderr, "usage: %s log <ref>\n", progname);
		return 1;
	}

	int rc = cas_tree_log_read(ct, argv[0], print_log_entry, NULL);

	if (rc == CAS_ENOTFOUND) {
		fprintf(stderr, "%s: ref '%s' not found\n",
		        progname, argv[0]);
		return 1;
	}
	return rc != CAS_OK ? 1 : 0;
}

/****************************************************************
 * cat
 ****************************************************************/

static int
cmd_cat(struct cas_tree *ct, int argc, char **argv)
{
	if (argc < 1) {
		fprintf(stderr, "usage: %s cat <hash>\n", progname);
		return 1;
	}

	struct cas *store = cas_tree_cas(ct);
	struct cas_file cf;
	char type[CAS_TYPE_MAX + 1];

	int rc = cas_open_object(store, &cf, argv[0], type,
	                         sizeof(type));
	if (rc != CAS_OK) {
		fprintf(stderr, "%s: cannot open '%s': %s\n",
		        progname, argv[0], cas_strerror(rc));
		return 1;
	}

	if (cf.len > 0)
		fwrite(cf.data, 1, cf.len, stdout);
	cas_close(&cf);
	return 0;
}

/****************************************************************
 * ls
 ****************************************************************/

static int
cmd_ls(struct cas_tree *ct, int argc, char **argv)
{
	if (argc < 1) {
		fprintf(stderr, "usage: %s ls <ref-or-hash>\n", progname);
		return 1;
	}

	char hash[CAS_HASH_HEX + 1];

	if (resolve_root(ct, argv[0], hash) != 0)
		return 1;

	struct cas_tree_dir dir;
	int rc = cas_tree_load(ct, hash, &dir);

	if (rc != CAS_OK) {
		fprintf(stderr, "%s: cannot load tree '%s': %s\n",
		        progname, argv[0], cas_strerror(rc));
		return 1;
	}

	for (int i = 0; i < dir.count; i++) {
		struct cas_tree_entry *e = &dir.entries[i];
		int type = e->mode & CAS_TREE_S_IFMT;
		const char *suffix = (type == CAS_TREE_S_IFDIR) ? "/" : "";

		printf("%06o  %s  %s%s\n",
		       (unsigned)e->mode, e->hash, e->name, suffix);
	}

	cas_tree_dir_free(&dir);
	return 0;
}

/****************************************************************
 * tree (recursive listing)
 ****************************************************************/

static int
print_tree_r(struct cas_tree *ct, const char *hash,
             const char *prefix)
{
	struct cas_tree_dir dir;
	int rc = cas_tree_load(ct, hash, &dir);

	if (rc != CAS_OK) {
		fprintf(stderr, "%s: cannot load tree: %s\n",
		        progname, cas_strerror(rc));
		return 1;
	}

	for (int i = 0; i < dir.count; i++) {
		struct cas_tree_entry *e = &dir.entries[i];
		int type = e->mode & CAS_TREE_S_IFMT;
		const char *suffix = (type == CAS_TREE_S_IFDIR) ? "/" : "";

		printf("%06o  %s  %s%s%s\n", (unsigned)e->mode,
		       e->hash, prefix, e->name, suffix);

		if (type == CAS_TREE_S_IFDIR) {
			char newprefix[4096];

			snprintf(newprefix, sizeof(newprefix),
			         "%s%s/", prefix, e->name);
			if (print_tree_r(ct, e->hash, newprefix) != 0) {
				cas_tree_dir_free(&dir);
				return 1;
			}
		}
	}

	cas_tree_dir_free(&dir);
	return 0;
}

static int
cmd_tree(struct cas_tree *ct, int argc, char **argv)
{
	if (argc < 1) {
		fprintf(stderr, "usage: %s tree <ref-or-hash>\n", progname);
		return 1;
	}

	char hash[CAS_HASH_HEX + 1];

	if (resolve_root(ct, argv[0], hash) != 0)
		return 1;

	return print_tree_r(ct, hash, "");
}

/****************************************************************
 * import
 ****************************************************************/

static int
cmd_import(struct cas_tree *ct, int argc, char **argv)
{
	int policy = CAS_COMPRESS_NEVER;

	/* optional leading -z: compress text-like files */
	if (argc >= 1 && strcmp(argv[0], "-z") == 0) {
		policy = CAS_COMPRESS_GUESS;
		if (!cas_codec_can_encode(CAS_CODEC_DEFLATE))
			fprintf(stderr,
			        "%s: warning: no compression codec compiled "
			        "in (build with MINIZ=1); importing "
			        "uncompressed\n", progname);
		argc--;
		argv++;
	}

	if (argc < 2) {
		fprintf(stderr,
		        "usage: %s import [-z] <ref> <file>...\n", progname);
		return 1;
	}

	const char *refname = argv[0];
	struct arglist al;

	arglist_init(&al);
	if (expand_args(&al, argc - 1, argv + 1) != 0) {
		arglist_free(&al);
		return 1;
	}

	if (al.count == 0) {
		fprintf(stderr, "%s: no files to import\n", progname);
		arglist_free(&al);
		return 1;
	}

	struct cas_tree_dir dir;
	char hash[CAS_HASH_HEX + 1];

	if (cas_tree_ref_read(ct, refname, hash) == CAS_OK) {
		int rc = cas_tree_load(ct, hash, &dir);

		if (rc != CAS_OK) {
			fprintf(stderr,
			        "%s: cannot load current tree: %s\n",
			        progname, cas_strerror(rc));
			arglist_free(&al);
			return 1;
		}
	} else {
		cas_tree_dir_init(&dir);
	}

	struct cas *store = cas_tree_cas(ct);

	for (int i = 0; i < al.count; i++) {
		size_t flen;
		unsigned char *data = read_file(al.args[i], &flen);

		if (!data) {
			cas_tree_dir_free(&dir);
			arglist_free(&al);
			return 1;
		}

		char bhash[CAS_HASH_HEX + 1];
		int use = cas_codec_policy(policy, CAS_CODEC_DEFLATE,
		                           data, flen);
		int rc = cas_put_object_z(store, "blob", use, data, flen,
		                          bhash);

		free(data);
		if (rc != CAS_OK) {
			fprintf(stderr, "%s: cannot store '%s': %s\n",
			        progname, al.args[i], cas_strerror(rc));
			cas_tree_dir_free(&dir);
			arglist_free(&al);
			return 1;
		}

		const char *name = basename_of(al.args[i]);

		for (int j = 0; j < dir.count; j++) {
			if (strcmp(dir.entries[j].name, name) == 0) {
				dir.entries[j] = dir.entries[--dir.count];
				break;
			}
		}

		struct cas_tree_entry e = {
			.mode = CAS_TREE_S_IFREG | 0644,
		};
		struct stat st;

		if (stat(al.args[i], &st) == 0) {
			e.mode = CAS_TREE_S_IFREG | (st.st_mode & 0777);
			e.uid = (int)st.st_uid;
			e.gid = (int)st.st_gid;
			e.mtime_s = (int64_t)st.st_mtim.tv_sec;
			e.mtime_ns = (int32_t)st.st_mtim.tv_nsec;
		}

		memcpy(e.hash, bhash, CAS_HASH_HEX + 1);
		snprintf(e.name, sizeof(e.name), "%s", name);

		if (cas_tree_dir_add(&dir, &e) != CAS_OK) {
			fprintf(stderr, "%s: cannot add entry\n", progname);
			cas_tree_dir_free(&dir);
			arglist_free(&al);
			return 1;
		}

		fprintf(stderr, "%s  %s\n", bhash, name);
	}

	char root_hash[CAS_HASH_HEX + 1];
	int rc = cas_tree_store(ct, &dir, root_hash);

	cas_tree_dir_free(&dir);
	arglist_free(&al);

	if (rc != CAS_OK) {
		fprintf(stderr, "%s: cannot store tree: %s\n",
		        progname, cas_strerror(rc));
		return 1;
	}

	rc = cas_tree_ref_commit(ct, refname, root_hash, "import");
	if (rc != CAS_OK) {
		fprintf(stderr, "%s: cannot commit: %s\n",
		        progname, cas_strerror(rc));
		return 1;
	}

	printf("%s\n", root_hash);
	return 0;
}

/****************************************************************
 * export
 ****************************************************************/

static int
export_tree_r(struct cas_tree *ct, const char *hash,
              const char *destdir)
{
	struct cas_tree_dir dir;
	int rc = cas_tree_load(ct, hash, &dir);

	if (rc != CAS_OK) {
		fprintf(stderr, "%s: cannot load tree: %s\n",
		        progname, cas_strerror(rc));
		return 1;
	}

	for (int i = 0; i < dir.count; i++) {
		struct cas_tree_entry *e = &dir.entries[i];
		char path[4096];

		snprintf(path, sizeof(path), "%s/%s", destdir, e->name);

		int type = e->mode & CAS_TREE_S_IFMT;

		if (type == CAS_TREE_S_IFDIR) {
			if (mkdir(path, 0755) != 0 && errno != EEXIST) {
				fprintf(stderr,
				        "%s: cannot create '%s': %s\n",
				        progname, path, strerror(errno));
				cas_tree_dir_free(&dir);
				return 1;
			}
			if (export_tree_r(ct, e->hash, path) != 0) {
				cas_tree_dir_free(&dir);
				return 1;
			}
		} else {
			struct cas_file cf;

			rc = cas_open(cas_tree_cas(ct), &cf, e->hash);
			if (rc != CAS_OK) {
				fprintf(stderr,
				        "%s: cannot open blob '%s': %s\n",
				        progname, e->hash,
				        cas_strerror(rc));
				cas_tree_dir_free(&dir);
				return 1;
			}

			FILE *fp = fopen(path, "wb");

			if (!fp) {
				fprintf(stderr,
				        "%s: cannot create '%s': %s\n",
				        progname, path, strerror(errno));
				cas_close(&cf);
				cas_tree_dir_free(&dir);
				return 1;
			}
			if (cf.len > 0)
				fwrite(cf.data, 1, cf.len, fp);
			fclose(fp);
			cas_close(&cf);
			chmod(path, e->mode & 0777);
		}
	}

	cas_tree_dir_free(&dir);
	return 0;
}

static int
cmd_export(struct cas_tree *ct, int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr,
		        "usage: %s export <ref-or-hash> <destdir>\n",
		        progname);
		return 1;
	}

	char hash[CAS_HASH_HEX + 1];

	if (resolve_root(ct, argv[0], hash) != 0)
		return 1;

	if (mkdir(argv[1], 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "%s: cannot create '%s': %s\n",
		        progname, argv[1], strerror(errno));
		return 1;
	}

	return export_tree_r(ct, hash, argv[1]);
}

/****************************************************************
 * rm
 ****************************************************************/

static int
cmd_rm(struct cas_tree *ct, int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr,
		        "usage: %s rm <ref> <name>...\n", progname);
		return 1;
	}

	const char *refname = argv[0];
	struct arglist al;

	arglist_init(&al);
	if (expand_args(&al, argc - 1, argv + 1) != 0) {
		arglist_free(&al);
		return 1;
	}

	char hash[CAS_HASH_HEX + 1];

	if (cas_tree_ref_read(ct, refname, hash) != CAS_OK) {
		fprintf(stderr, "%s: ref '%s' not found\n",
		        progname, refname);
		arglist_free(&al);
		return 1;
	}

	struct cas_tree_dir dir;
	int rc = cas_tree_load(ct, hash, &dir);

	if (rc != CAS_OK) {
		fprintf(stderr, "%s: cannot load tree: %s\n",
		        progname, cas_strerror(rc));
		arglist_free(&al);
		return 1;
	}

	int removed = 0;

	for (int k = 0; k < al.count; k++) {
		int found = 0;

		for (int j = 0; j < dir.count; j++) {
			if (strcmp(dir.entries[j].name, al.args[k]) == 0) {
				dir.entries[j] = dir.entries[--dir.count];
				found = 1;
				removed++;
				break;
			}
		}
		if (!found)
			fprintf(stderr, "%s: '%s' not found in tree\n",
			        progname, al.args[k]);
	}

	arglist_free(&al);

	if (removed == 0) {
		cas_tree_dir_free(&dir);
		return 1;
	}

	char root_hash[CAS_HASH_HEX + 1];

	rc = cas_tree_store(ct, &dir, root_hash);
	cas_tree_dir_free(&dir);

	if (rc != CAS_OK) {
		fprintf(stderr, "%s: cannot store tree: %s\n",
		        progname, cas_strerror(rc));
		return 1;
	}

	rc = cas_tree_ref_commit(ct, refname, root_hash, "rm");
	if (rc != CAS_OK) {
		fprintf(stderr, "%s: cannot commit: %s\n",
		        progname, cas_strerror(rc));
		return 1;
	}

	printf("%s\n", root_hash);
	return 0;
}

/****************************************************************
 * hash
 ****************************************************************/

static int
cmd_hash(struct cas_tree *ct, int argc, char **argv)
{
	(void)ct;

	unsigned char *data;
	size_t len;

	if (argc >= 1)
		data = read_file(argv[0], &len);
	else
		data = read_stdin(&len);

	if (!data)
		return 1;

	char hash[CAS_HASH_HEX + 1];

	cas_hash_object("blob", data, len, hash);
	free(data);
	printf("%s\n", hash);
	return 0;
}

/****************************************************************
 * fsck
 ****************************************************************/

static int
fsck_reporter(const char *path, const char *hash, int status,
              void *ctx)
{
	(void)ctx;
	const char *msg;

	switch (status) {
	case CAS_TREE_FSCK_OK:
		return 0;
	case CAS_TREE_FSCK_MISSING:
		msg = "missing";
		break;
	case CAS_TREE_FSCK_CORRUPT:
		msg = "corrupt";
		break;
	case CAS_TREE_FSCK_BAD_TREE:
		msg = "bad tree";
		break;
	case CAS_TREE_FSCK_NOCODEC:
		msg = "skipped (no codec)";
		break;
	case CAS_TREE_FSCK_FOREIGN:
		msg = "skipped (not a directory)";
		break;
	default:
		msg = "unknown";
		break;
	}

	fprintf(stderr, "%s: %s %s\n", msg, hash, path);
	return 0;
}

static int
cmd_fsck(struct cas_tree *ct, int argc, char **argv)
{
	(void)argc;
	(void)argv;

	int rc = cas_tree_fsck(ct, fsck_reporter, NULL);

	if (rc == CAS_OK)
		fprintf(stderr, "fsck: clean\n");
	else
		fprintf(stderr, "fsck: errors found\n");
	return rc != CAS_OK ? 1 : 0;
}

/****************************************************************
 * gc
 ****************************************************************/

static int
gc_reporter(const char *hash, void *ctx)
{
	(void)ctx;
	fprintf(stderr, "removed %s\n", hash);
	return 0;
}

static int
cmd_gc(struct cas_tree *ct, int argc, char **argv)
{
	time_t grace = 3600;
	int do_pack = 0;
	int compress = 0;

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--now") == 0)
			grace = 0;
		else if (strcmp(argv[i], "--pack") == 0)
			do_pack = 1;
		else if (strcmp(argv[i], "-z") == 0)
			compress = 1;
		else {
			fprintf(stderr,
			        "usage: %s gc [--now] [--pack] [-z]\n",
			        progname);
			return 1;
		}
	}

	if (!do_pack && compress) {
		fprintf(stderr, "%s: -z only applies with --pack\n", progname);
		return 1;
	}

	int removed = 0;
	int rc;

	if (do_pack) {
		int did_compress = compress &&
		                   cas_codec_can_encode(CAS_CODEC_DEFLATE);

		if (compress && !did_compress)
			fprintf(stderr,
			        "%s: warning: no compression codec compiled "
			        "in (build with MINIZ=1); packing "
			        "uncompressed\n", progname);

		int policy = did_compress ? CAS_COMPRESS_GUESS
		                          : CAS_COMPRESS_NEVER;
		int codec = did_compress ? CAS_CODEC_DEFLATE : CAS_CODEC_NONE;
		uint64_t reclaimed = 0;

		rc = cas_tree_gc_pack(ct, grace, policy, codec, gc_reporter,
		                      NULL, &removed, &reclaimed);
		if (rc == CAS_OK)
			fprintf(stderr,
			        "packed, reclaimed %llu loose object%s\n",
			        (unsigned long long)reclaimed,
			        reclaimed == 1 ? "" : "s");
	} else {
		rc = cas_tree_gc(ct, grace, gc_reporter, NULL, &removed);
	}

	if (rc != CAS_OK) {
		fprintf(stderr, "%s: gc failed\n", progname);
		return 1;
	}
	fprintf(stderr, "removed %d object%s\n", removed,
	        removed == 1 ? "" : "s");
	return 0;
}

/****************************************************************
 * prune
 ****************************************************************/

static int
cmd_prune(struct cas_tree *ct, int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr,
		        "usage: %s prune <ref> <keep-count>\n", progname);
		return 1;
	}

	int keep = atoi(argv[1]);

	if (keep < 1) {
		fprintf(stderr, "%s: keep-count must be at least 1\n",
		        progname);
		return 1;
	}

	int removed = 0;
	int rc = cas_tree_log_truncate(ct, argv[0], keep, 0, &removed);

	if (rc == CAS_ENOTFOUND) {
		fprintf(stderr, "%s: ref '%s' has no log\n",
		        progname, argv[0]);
		return 1;
	}
	if (rc != CAS_OK) {
		fprintf(stderr, "%s: prune failed\n", progname);
		return 1;
	}
	fprintf(stderr, "pruned %d log entr%s\n", removed,
	        removed == 1 ? "y" : "ies");
	return 0;
}

/****************************************************************
 * pack
 ****************************************************************/

static int
cmd_pack(struct cas_tree *ct, int argc, char **argv)
{
	int compress = 0;
	int prune = 0;

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-z") == 0) {
			compress = 1;
		} else if (strcmp(argv[i], "--prune") == 0) {
			prune = 1;
		} else {
			fprintf(stderr, "usage: %s pack [-z] [--prune]\n",
			        progname);
			return 1;
		}
	}

	struct cas *store = cas_tree_cas(ct);
	const char *base = cas_basedir(store);
	char path[512];

	snprintf(path, sizeof(path), "%s/pack.dat", base);

	int did_compress = compress &&
	                   cas_codec_can_encode(CAS_CODEC_DEFLATE);
	int rc;

	if (compress && !did_compress)
		fprintf(stderr,
		        "%s: warning: no compression codec compiled in "
		        "(build with MINIZ=1); packing uncompressed\n",
		        progname);

	if (compress)
		rc = cas_pack_create_z(store, path, CAS_COMPRESS_GUESS,
		                       CAS_CODEC_DEFLATE);
	else
		rc = cas_pack_create(store, path);

	if (rc != CAS_OK) {
		fprintf(stderr, "%s: pack failed: %s\n",
		        progname, cas_strerror(rc));
		return 1;
	}

	fprintf(stderr, "pack: ok%s\n", did_compress ? " (compressed)" : "");

	if (prune) {
		uint64_t removed = 0;
		int prc = cas_pack_reclaim(store, path, &removed);

		if (prc != CAS_OK) {
			fprintf(stderr, "%s: prune failed: %s\n",
			        progname, cas_strerror(prc));
			return 1;
		}
		fprintf(stderr, "prune: reclaimed %llu loose object%s\n",
		        (unsigned long long)removed,
		        removed == 1 ? "" : "s");
	}
	return 0;
}

/****************************************************************
 * import-pack: merge a downloaded bundle into this depot
 ****************************************************************/

static int
cmd_import_pack(struct cas_tree *ct, int argc, char **argv)
{
	int policy = CAS_COMPRESS_NEVER;
	int i = 0;

	if (i < argc && strcmp(argv[i], "-z") == 0) {
		policy = CAS_COMPRESS_GUESS;
		i++;
	}

	const char *packfile = (i < argc) ? argv[i++] : NULL;
	const char *ref = NULL;
	const char *roothash = NULL;

	if (packfile && i + 2 == argc) {
		ref = argv[i++];
		roothash = argv[i++];
	}

	if (!packfile || i != argc) {
		fprintf(stderr,
		        "usage: %s import-pack [-z] <pack-file> "
		        "[<ref> <root-hash>]\n", progname);
		return 1;
	}

	struct cas *store = cas_tree_cas(ct);
	struct cas_pack *pack = cas_pack_open(packfile);

	if (!pack) {
		fprintf(stderr, "%s: cannot open bundle '%s'\n",
		        progname, packfile);
		return 1;
	}

	uint64_t total = 0, stored = 0;
	/* A bundle comes from outside, so every object is checked before
	 * it is admitted; cas_tree_check_object is what knows how to check
	 * an htree. */
	int rc = cas_pack_import(pack, store, policy, CAS_CODEC_DEFLATE,
	                         cas_tree_check_object, &total, &stored);

	cas_pack_close(pack);

	if (rc != CAS_OK) {
		fprintf(stderr, "%s: import failed: %s\n",
		        progname, cas_strerror(rc));
		return 1;
	}

	fprintf(stderr, "import-pack: %" PRIu64 " objects, %" PRIu64
	        " new\n", total, stored);

	if (ref) {
		if (!cas_exists(store, roothash)) {
			fprintf(stderr,
			        "%s: root hash '%s' not present in bundle\n",
			        progname, roothash);
			return 1;
		}
		rc = cas_tree_ref_commit(ct, ref, roothash,
		                         "imported bundle");
		if (rc != CAS_OK) {
			fprintf(stderr, "%s: ref commit failed: %s\n",
			        progname, cas_strerror(rc));
			return 1;
		}
		fprintf(stderr, "import-pack: ref '%s' -> %s\n",
		        ref, roothash);
	}

	return 0;
}


/****************************************************************
 * Signed topics
 ****************************************************************/

/* Every topic verb needs a signer, and a build without one must say so
 * plainly rather than fail somewhere further in with a vaguer message. */
static int
need_backend(void)
{
	if (cas_sign_available())
		return 0;
	fprintf(stderr,
	        "%s: built without a signing backend; "
	        "rebuild with MONOCYPHER=1\n", progname);
	return 1;
}

static void
print_topic_names(const unsigned char pk[CAS_SIGN_PUBKEY_LEN],
                  const char *label)
{
	unsigned char id[CAS_HASH_LEN];
	char idhex[CAS_HASH_HEX + 1], pkhex[CAS_HASH_HEX + 1];
	char refname[CAS_HASH_HEX + 2 + CAS_TOPIC_LABEL_MAX + 1];

	cas_sign_topic_id(id, pk);
	cas_hex_encode(id, CAS_HASH_LEN, idhex);
	cas_hex_encode(pk, CAS_SIGN_PUBKEY_LEN, pkhex);

	printf("topic-id   %s\n", idhex);
	printf("public-key %s\n", pkhex);
	if (cas_topic_ref_name(refname, sizeof(refname), id, label) == CAS_OK)
		printf("ref        %s\n", refname);
	else
		fprintf(stderr, "%s: label '%s' is not usable in a ref name\n",
		        progname, label ? label : "");
}

static int
cmd_keygen(struct cas_tree *ct, int argc, char **argv)
{
	(void)ct;
	if (argc < 1) {
		fprintf(stderr, "usage: %s keygen <keyfile>\n", progname);
		return 1;
	}
	if (need_backend())
		return 1;

	unsigned char pk[CAS_SIGN_PUBKEY_LEN];
	int rc = cas_sign_key_generate(argv[0], pk);

	if (rc != CAS_OK) {
		fprintf(stderr, "%s: keygen '%s': %s\n", progname, argv[0],
		        cas_sign_strerror(rc));
		return 1;
	}

	fprintf(stderr, "%s: wrote %s (mode 0600)\n", progname, argv[0]);
	print_topic_names(pk, NULL);
	return 0;
}

static int
cmd_keyid(struct cas_tree *ct, int argc, char **argv)
{
	(void)ct;
	if (argc < 1) {
		fprintf(stderr, "usage: %s keyid <keyfile> [label]\n",
		        progname);
		return 1;
	}
	if (need_backend())
		return 1;

	unsigned char sk[CAS_SIGN_SECKEY_LEN], pk[CAS_SIGN_PUBKEY_LEN];
	int rc = cas_sign_key_load(argv[0], sk, pk);

	if (rc != CAS_OK) {
		fprintf(stderr, "%s: %s: %s\n", progname, argv[0],
		        cas_sign_strerror(rc));
		return 1;
	}
	memset(sk, 0, sizeof(sk));
	print_topic_names(pk, argc > 1 ? argv[1] : NULL);
	return 0;
}

static int
cmd_publish(struct cas_tree *ct, int argc, char **argv)
{
	const char *label = NULL;

	while (argc > 0 && argv[0][0] == '-') {
		if (strcmp(argv[0], "-l") == 0 && argc >= 2) {
			label = argv[1];
			argc -= 2;
			argv += 2;
			continue;
		}
		fprintf(stderr, "%s: publish: unknown option '%s'\n",
		        progname, argv[0]);
		return 1;
	}

	if (argc < 2) {
		fprintf(stderr,
		        "usage: %s publish [-l label] <keyfile> <root-hash>\n",
		        progname);
		return 1;
	}
	if (need_backend())
		return 1;

	unsigned char sk[CAS_SIGN_SECKEY_LEN], pk[CAS_SIGN_PUBKEY_LEN];
	int rc = cas_sign_key_load(argv[0], sk, pk);

	if (rc != CAS_OK) {
		fprintf(stderr, "%s: %s: %s\n", progname, argv[0],
		        cas_sign_strerror(rc));
		return 1;
	}

	unsigned char id[CAS_HASH_LEN];
	char refname[CAS_HASH_HEX + 2 + CAS_TOPIC_LABEL_MAX + 1];

	cas_sign_topic_id(id, pk);
	if (cas_topic_ref_name(refname, sizeof(refname), id, label)
	    != CAS_OK) {
		fprintf(stderr, "%s: bad label\n", progname);
		memset(sk, 0, sizeof(sk));
		return 1;
	}

	/* seq and prev come from the topic's own head, so a publisher
	 * never has to track them and cannot accidentally fork its own
	 * chain by guessing. */
	struct cas_vrec head, next;
	char head_addr[CAS_HASH_HEX + 1];

	memset(&next, 0, sizeof(next));
	rc = cas_topic_head(ct, refname, &head, head_addr);
	if (rc == CAS_OK) {
		next.seq = head.seq + 1;
		snprintf(next.prev, sizeof(next.prev), "%s", head_addr);
	} else if (rc == CAS_ENOTFOUND) {
		next.seq = 1;
	} else {
		fprintf(stderr, "%s: head: %s\n", progname,
		        cas_sign_strerror(rc));
		memset(sk, 0, sizeof(sk));
		return 1;
	}

	/* A publisher signs what it has.  Signing a root this depot does
	 * not hold produces a topic that verifies perfectly and resolves
	 * to nothing, and the subscriber discovers it, not the publisher.
	 * The library allows it, because a record naming an object a node
	 * has not fetched is exactly what lazy fetch looks like; a person
	 * running publish almost never means it. */
	if (!cas_valid_hash(argv[1])) {
		fprintf(stderr, "%s: '%s' is not a hash\n", progname,
		        argv[1]);
		memset(sk, 0, sizeof(sk));
		return 1;
	}
	if (!cas_exists(cas_tree_cas(ct), argv[1])) {
		fprintf(stderr, "%s: root %s is not in this depot\n",
		        progname, argv[1]);
		memset(sk, 0, sizeof(sk));
		return 1;
	}

	memcpy(next.pubkey, pk, sizeof(pk));
	next.timestamp = (int64_t)time(NULL);
	snprintf(next.root, sizeof(next.root), "%s", argv[1]);

	unsigned char rec[CAS_VREC_LEN];

	rc = cas_vrec_encode(&next, sk, rec);
	memset(sk, 0, sizeof(sk));
	if (rc != CAS_OK) {
		fprintf(stderr, "%s: encode: %s\n", progname,
		        cas_sign_strerror(rc));
		return 1;
	}

	rc = cas_topic_publish(ct, refname, rec, "publish");
	if (rc != CAS_OK) {
		fprintf(stderr, "%s: publish: %s\n", progname,
		        cas_sign_strerror(rc));
		return 1;
	}

	char addr[CAS_HASH_HEX + 1];

	cas_vrec_address(rec, addr);
	printf("%s\n", addr);
	fprintf(stderr, "%s: %s seq %llu -> %s\n", progname, refname,
	        (unsigned long long)next.seq, next.root);
	return 0;
}

static void
print_vrec(const struct cas_vrec *v, const char *addr)
{
	char idhex[CAS_HASH_HEX + 1];

	cas_hex_encode(v->topic_id, CAS_HASH_LEN, idhex);
	printf("record   %s\n", addr);
	printf("topic-id %s\n", idhex);
	printf("seq      %llu\n", (unsigned long long)v->seq);
	printf("root     %s\n", v->root);
	printf("prev     %s\n", v->prev[0] ? v->prev : "(none)");
	printf("time     %lld\n", (long long)v->timestamp);
}

static int
cmd_topic(struct cas_tree *ct, int argc, char **argv)
{
	if (argc < 1) {
		fprintf(stderr, "usage: %s topic <ref>\n", progname);
		return 1;
	}
	if (need_backend())
		return 1;

	struct cas_vrec head;
	char addr[CAS_HASH_HEX + 1];
	int rc = cas_topic_head(ct, argv[0], &head, addr);

	if (rc != CAS_OK) {
		fprintf(stderr, "%s: %s: %s\n", progname, argv[0],
		        cas_sign_strerror(rc));
		return 1;
	}
	print_vrec(&head, addr);
	return 0;
}

static int
print_chain_entry(const struct cas_vrec *v, const char *addr, void *ctx)
{
	(void)ctx;
	printf("%llu %s %s\n", (unsigned long long)v->seq, addr, v->root);
	return 0;
}

static int
cmd_topic_log(struct cas_tree *ct, int argc, char **argv)
{
	if (argc < 1) {
		fprintf(stderr, "usage: %s topic-log <ref>\n", progname);
		return 1;
	}
	if (need_backend())
		return 1;

	struct cas_vrec head;
	char addr[CAS_HASH_HEX + 1];
	int rc = cas_topic_head(ct, argv[0], &head, addr);

	if (rc != CAS_OK) {
		fprintf(stderr, "%s: %s: %s\n", progname, argv[0],
		        cas_sign_strerror(rc));
		return 1;
	}

	rc = cas_vchain_walk(cas_tree_cas(ct), addr, 0, print_chain_entry,
	                     NULL);

	/* An incomplete chain is normal for a node holding part of a
	 * history, so it is worth saying and not worth failing over. */
	if (rc == CAS_SIGN_EINCOMPLETE) {
		fprintf(stderr, "%s: chain incomplete in this depot\n",
		        progname);
		return 0;
	}
	if (rc != CAS_OK) {
		fprintf(stderr, "%s: walk: %s\n", progname,
		        cas_sign_strerror(rc));
		return 1;
	}
	return 0;
}

/****************************************************************
 * Command dispatch
 ****************************************************************/

struct command {
	const char *name;
	int (*fn)(struct cas_tree *, int, char **);
	const char *usage;
};

static const struct command commands[] = {
	{ "refs",   cmd_refs,   "list all refs" },
	{ "log",    cmd_log,    "log <ref>" },
	{ "cat",    cmd_cat,    "cat <hash>" },
	{ "ls",     cmd_ls,     "ls <ref-or-hash>" },
	{ "tree",   cmd_tree,   "tree <ref-or-hash>" },
	{ "import", cmd_import, "import [-z] <ref> <file>..." },
	{ "export", cmd_export, "export <ref-or-hash> <destdir>" },
	{ "rm",     cmd_rm,     "rm <ref> <name>..." },
	{ "hash",   cmd_hash,   "hash [file]" },
	{ "fsck",   cmd_fsck,   "fsck" },
	{ "gc",     cmd_gc,     "gc [--now] [--pack] [-z]  "
	                        "(--pack compacts into the pack)" },
	{ "prune",  cmd_prune,  "prune <ref> <keep-count>" },
	{ "pack",   cmd_pack,   "pack [-z] [--prune] loose objects "
	                        "(-z compresses, --prune reclaims loose)" },
	{ "import-pack", cmd_import_pack,
	  "import-pack [-z] <pack-file> [<ref> <root-hash>]" },
	{ "keygen", cmd_keygen, "keygen <keyfile>" },
	{ "keyid",  cmd_keyid,  "keyid <keyfile> [label]" },
	{ "publish", cmd_publish,
	  "publish [-l label] <keyfile> <root-hash>" },
	{ "topic",  cmd_topic,  "topic <ref>  show the signed head" },
	{ "topic-log", cmd_topic_log, "topic-log <ref>  walk the chain" },
};

#define NCOMMANDS (sizeof(commands) / sizeof(commands[0]))

static void
usage(void)
{
	fprintf(stderr, "castool (smolvfs %s)\n\n", SMOLVFS_VERSION);
	fprintf(stderr,
	        "usage: %s [-d depot] <command> [args...]\n"
	        "       %s -V        print version\n\n",
	        progname, progname);
	fprintf(stderr, "commands:\n");
	for (size_t i = 0; i < NCOMMANDS; i++)
		fprintf(stderr, "  %-10s %s\n", commands[i].name,
		        commands[i].usage);
	fprintf(stderr,
	        "\narguments starting with @ expand to lines "
	        "from the named file (@- for stdin)\n");
}

int
main(int argc, char **argv)
{
	const char *depot = "depot";
	int opt;

	while ((opt = getopt(argc, argv, "d:hV")) != -1) {
		switch (opt) {
		case 'd':
			depot = optarg;
			break;
		case 'V':
			printf("%s\n", SMOLVFS_VERSION);
			return 0;
		case 'h':
			usage();
			return 0;
		default:
			usage();
			return 1;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		usage();
		return 1;
	}

	struct cas *store = cas_new(depot);

	if (!store) {
		fprintf(stderr, "%s: cannot create CAS store\n", progname);
		return 1;
	}

	struct cas_tree *ct = cas_tree_new(store);

	if (!ct) {
		cas_free(store);
		fprintf(stderr, "%s: cannot create CAS-tree\n", progname);
		return 1;
	}

	const char *cmdname = argv[0];
	int rc = 1;

	for (size_t i = 0; i < NCOMMANDS; i++) {
		if (strcmp(cmdname, commands[i].name) == 0) {
			rc = commands[i].fn(ct, argc - 1, argv + 1);
			goto done;
		}
	}

	fprintf(stderr, "%s: unknown command '%s'\n", progname, cmdname);
	usage();

done:
	cas_tree_free(ct);
	cas_free(store);
	return rc;
}
