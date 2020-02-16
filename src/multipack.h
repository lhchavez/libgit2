/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#ifndef INCLUDE_multipack_h__
#define INCLUDE_multipack_h__

#include "common.h"

#include <ctype.h>

#include "map.h"
#include "mwindow.h"
#include "odb.h"

/*
 * A multi-pack-index file.
 *
 * This file contains a merged index for multiple independent .pack files. This
 * can help speed up locating objects without requiring a garbage collection
 * cycle to create a single .pack file.
 *
 * Support for this feature was added in git 2.21, and requires the
 * `core.multiPackIndex` config option to be set.
 */
typedef struct git_multipack_index_file {
	git_map index_map;

	/* The table of Packfile Names. */
	git_vector packfile_names;

	/* The OID Fanout table. */
	const uint32_t *oid_fanout;
	/* The total number of objects in the index. */
	uint32_t num_objects;

	/* The OID Lookup table. */
	git_oid *oid_lookup;

	/* The Object Offsets table. Each entry has two 4-byte fields with the pack index and the offset. */
	const unsigned char *object_offsets;

	/* The Object Large Offsets table. */
	const unsigned char *object_large_offsets;
	/* The number of entries in the Object Large Offsets table. Each entry has an 8-byte with an offset */
	size_t num_object_large_offsets;

	/* The trailer of the file. Contains the SHA1-checksum of the whole file. */
	git_oid checksum;

	/* something like ".git/objects/pack/multi-pack-index". */
	git_buf filename;
} git_multipack_index_file;

/*
 * An entry in the multi-pack-index file. Similar in purpose to git_pack_entry.
 */
typedef struct git_multipack_entry {
	/* The index within idx->packfile_names where the packfile name can be found. */
	size_t pack_index;
	/* The offset within the .pack file where the requested object is found. */
	off64_t offset;
	/* The SHA-1 hash of the requested object. */
	git_oid sha1;
} git_multipack_entry;

int git_multipack_index_open(
		git_multipack_index_file **idx_out,
		const char *path);
bool git_multipack_index_needs_refresh(
		const git_multipack_index_file *idx,
		const char *path);
int git_multipack_index_entry_find(
		git_multipack_entry *e,
		git_multipack_index_file *idx,
		const git_oid *short_oid,
		size_t len);
int git_multipack_index_foreach_entry(
		git_multipack_index_file *idx,
		git_odb_foreach_cb cb,
		void *data);
void git_multipack_index_close(git_multipack_index_file *idx);
void git_multipack_index_free(git_multipack_index_file *idx);

/* This is exposed for use in the fuzzers. */
int git_multipack_index_parse(
		git_multipack_index_file *idx,
		const unsigned char *data,
		size_t size);

#endif
