/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "multipack.h"

#include "array.h"
#include "futils.h"
#include "hash.h"
#include "odb.h"
#include "pack.h"
#include "path.h"

#define GIT_MULTIPACK_FILE_MODE 0444

#define MULTIPACK_INDEX_SIGNATURE 0x4d494458 /* "MIDX" */
#define MULTIPACK_INDEX_VERSION 1
#define MULTIPACK_INDEX_OBJECT_ID_VERSION 1
struct git_multipack_index_header {
	uint32_t signature;
	uint8_t version;
	uint8_t object_id_version;
	uint8_t chunks;
	uint8_t base_midx_files;
	uint32_t packfiles;
};

#define MULTIPACK_INDEX_PACKFILE_NAMES_ID 0x504e414d	   /* "PNAM" */
#define MULTIPACK_INDEX_OID_FANOUT_ID 0x4f494446	   /* "OIDF" */
#define MULTIPACK_INDEX_OID_LOOKUP_ID 0x4f49444c	   /* "OIDL" */
#define MULTIPACK_INDEX_OBJECT_OFFSETS_ID 0x4f4f4646	   /* "OOFF" */
#define MULTIPACK_INDEX_OBJECT_LARGE_OFFSETS_ID 0x4c4f4646 /* "LOFF" */

struct git_multipack_index_chunk {
	off64_t offset;
	size_t length;
};

static int multipack_error(const char *message)
{
	git_error_set(GIT_ERROR_ODB, "invalid multi-pack-index file - %s", message);
	return -1;
}

static int multipack_index_parse_packfile_names(
		git_multipack_index_file *idx,
		const unsigned char *data,
		uint32_t packfiles,
		struct git_multipack_index_chunk *chunk)
{
	int error;
	uint32_t i;
	char *packfile_name = (char *)(data + chunk->offset);
	size_t chunk_size = chunk->length, len;
	if (chunk->offset == 0)
		return multipack_error("missing Packfile Names chunk");
	if (chunk->length == 0)
		return multipack_error("empty Packfile Names chunk");
	if ((error = git_vector_init(&idx->packfile_names, packfiles, git__strcmp_cb)) < 0)
		return error;
	for (i = 0; i < packfiles; ++i) {
		len = p_strnlen(packfile_name, chunk_size);
		if (len == 0)
			return multipack_error("empty packfile name");
		if (len + 1 > chunk_size)
			return multipack_error("unterminated packfile name");
		git_vector_insert(&idx->packfile_names, packfile_name);
		if (i && strcmp(git_vector_get(&idx->packfile_names, i - 1), packfile_name) >= 0)
			return multipack_error("packfile names are not sorted");
		if (strlen(packfile_name) <= strlen(".idx") || git__suffixcmp(packfile_name, ".idx") != 0)
			return multipack_error("non-.idx packfile name");
		if (strchr(packfile_name, '/') != NULL || strchr(packfile_name, '\\') != NULL)
			return multipack_error("non-local packfile");
		packfile_name += len + 1;
		chunk_size -= len + 1;
	}
	return 0;
}

static int multipack_index_parse_oid_fanout(
		git_multipack_index_file *idx,
		const unsigned char *data,
		struct git_multipack_index_chunk *chunk_oid_fanout)
{
	uint32_t i, nr;
	if (chunk_oid_fanout->offset == 0)
		return multipack_error("missing OID Fanout chunk");
	if (chunk_oid_fanout->length == 0)
		return multipack_error("empty OID Fanout chunk");
	if (chunk_oid_fanout->length != 256 * 4)
		return multipack_error("OID Fanout chunk has wrong length");

	idx->oid_fanout = (const uint32_t *)(data + chunk_oid_fanout->offset);
	nr = 0;
	for (i = 0; i < 256; ++i) {
		uint32_t n = ntohl(idx->oid_fanout[i]);
		if (n < nr)
			return multipack_error("index is non-monotonic");
		nr = n;
	}
	idx->num_objects = nr;
	return 0;
}

static int multipack_index_parse_oid_lookup(
		git_multipack_index_file *idx,
		const unsigned char *data,
		struct git_multipack_index_chunk *chunk_oid_lookup)
{
	uint32_t i;
	git_oid *oid, *prev_oid, zero_oid = {{0}};

	if (chunk_oid_lookup->offset == 0)
		return multipack_error("missing OID Lookup chunk");
	if (chunk_oid_lookup->length == 0)
		return multipack_error("empty OID Lookup chunk");
	if (chunk_oid_lookup->length != idx->num_objects * 20)
		return multipack_error("OID Lookup chunk has wrong length");

	idx->oid_lookup = oid = (git_oid *)(data + chunk_oid_lookup->offset);
	prev_oid = &zero_oid;
	for (i = 0; i < idx->num_objects; ++i, ++oid) {
		if (git_oid_cmp(prev_oid, oid) >= 0)
			return multipack_error("OID Lookup index is non-monotonic");
		prev_oid = oid;
	}

	return 0;
}

static int multipack_index_parse_object_offsets(
		git_multipack_index_file *idx,
		const unsigned char *data,
		struct git_multipack_index_chunk *chunk_object_offsets)
{
	if (chunk_object_offsets->offset == 0)
		return multipack_error("missing Object Offsets chunk");
	if (chunk_object_offsets->length == 0)
		return multipack_error("empty Object Offsets chunk");
	if (chunk_object_offsets->length != idx->num_objects * 8)
		return multipack_error("Object Offsets chunk has wrong length");

	idx->object_offsets = data + chunk_object_offsets->offset;

	return 0;
}

static int multipack_index_parse_object_large_offsets(
		git_multipack_index_file *idx,
		const unsigned char *data,
		struct git_multipack_index_chunk *chunk_object_large_offsets)
{
	if (chunk_object_large_offsets->length == 0)
		return 0;
	if (chunk_object_large_offsets->length % 8 != 0)
		return multipack_error("malformed Object Large Offsets chunk");

	idx->object_large_offsets = data + chunk_object_large_offsets->offset;
	idx->num_object_large_offsets = chunk_object_large_offsets->length / 8;

	return 0;
}

int git_multipack_index_parse(
		git_multipack_index_file *idx,
		const unsigned char *data,
		size_t size)
{
	struct git_multipack_index_header *hdr;
	const unsigned char *chunk_hdr;
	struct git_multipack_index_chunk *last_chunk;
	uint32_t i;
	off64_t last_chunk_offset, chunk_offset, trailer_offset;
	git_oid idx_checksum = {{0}};
	int error;
	struct git_multipack_index_chunk chunk_packfile_names = {0},
					 chunk_oid_fanout = {0},
					 chunk_oid_lookup = {0},
					 chunk_object_offsets = {0},
					 chunk_object_large_offsets = {0};

	assert(idx);

	if (size < sizeof(struct git_multipack_index_header) + 20)
		return multipack_error("multi-pack index is too short");

	hdr = ((struct git_multipack_index_header *)data);

	if (hdr->signature != htonl(MULTIPACK_INDEX_SIGNATURE) ||
	    hdr->version != MULTIPACK_INDEX_VERSION ||
	    hdr->object_id_version != MULTIPACK_INDEX_OBJECT_ID_VERSION) {
		return multipack_error("unsupported multi-pack index version");
	}
	if (hdr->chunks == 0)
		return multipack_error("no chunks in multi-pack index");

	/*
	 * The very first chunk's offset should be after the header, all the chunk
	 * headers, and a special zero chunk.
	 */
	last_chunk_offset =
			sizeof(struct git_multipack_index_header) +
			(1 + hdr->chunks) * 12;
	trailer_offset = size - 20;
	if (trailer_offset < last_chunk_offset)
		return multipack_error("wrong index size");
	git_oid_cpy(&idx->checksum, (git_oid *)(data + trailer_offset));

	if (git_hash_buf(&idx_checksum, data, (size_t)trailer_offset) < 0)
		return multipack_error("could not calculate signature");
	if (!git_oid_equal(&idx_checksum, &idx->checksum))
		return multipack_error("index signature mismatch");

	chunk_hdr = data + sizeof(struct git_multipack_index_header);
	last_chunk = NULL;
	for (i = 0; i < hdr->chunks; ++i, chunk_hdr += 12) {
		chunk_offset = ((off64_t)ntohl(*((uint32_t *)(chunk_hdr + 4)))) << 32 |
				((off64_t)ntohl(*((uint32_t *)(chunk_hdr + 8))));
		if (chunk_offset < last_chunk_offset)
			return multipack_error("chunks are non-monotonic");
		if (chunk_offset >= trailer_offset)
			return multipack_error("chunks extend beyond the trailer");
		if (last_chunk != NULL)
			last_chunk->length = (size_t)(chunk_offset - last_chunk_offset);
		last_chunk_offset = chunk_offset;

		switch (ntohl(*((uint32_t *)(chunk_hdr + 0)))) {
		case MULTIPACK_INDEX_PACKFILE_NAMES_ID:
			chunk_packfile_names.offset = last_chunk_offset;
			last_chunk = &chunk_packfile_names;
			break;

		case MULTIPACK_INDEX_OID_FANOUT_ID:
			chunk_oid_fanout.offset = last_chunk_offset;
			last_chunk = &chunk_oid_fanout;
			break;

		case MULTIPACK_INDEX_OID_LOOKUP_ID:
			chunk_oid_lookup.offset = last_chunk_offset;
			last_chunk = &chunk_oid_lookup;
			break;

		case MULTIPACK_INDEX_OBJECT_OFFSETS_ID:
			chunk_object_offsets.offset = last_chunk_offset;
			last_chunk = &chunk_object_offsets;
			break;

		case MULTIPACK_INDEX_OBJECT_LARGE_OFFSETS_ID:
			chunk_object_large_offsets.offset = last_chunk_offset;
			last_chunk = &chunk_object_large_offsets;
			break;

		default:
			return multipack_error("unrecognized chunk ID");
		}
	}
	last_chunk->length = (size_t)(trailer_offset - last_chunk_offset);

	error = multipack_index_parse_packfile_names(
			idx, data, ntohl(hdr->packfiles), &chunk_packfile_names);
	if (error < 0)
		return error;
	error = multipack_index_parse_oid_fanout(idx, data, &chunk_oid_fanout);
	if (error < 0)
		return error;
	error = multipack_index_parse_oid_lookup(idx, data, &chunk_oid_lookup);
	if (error < 0)
		return error;
	error = multipack_index_parse_object_offsets(idx, data, &chunk_object_offsets);
	if (error < 0)
		return error;
	error = multipack_index_parse_object_large_offsets(idx, data, &chunk_object_large_offsets);
	if (error < 0)
		return error;

	return 0;
}

int git_multipack_index_open(
		git_multipack_index_file **idx_out,
		const char *path)
{
	git_multipack_index_file *idx;
	git_file fd = -1;
	size_t idx_size;
	struct stat st;
	int error;

	/* TODO: properly open the file without access time using O_NOATIME */
	fd = git_futils_open_ro(path);
	if (fd < 0)
		return fd;

	if (p_fstat(fd, &st) < 0) {
		p_close(fd);
		git_error_set(GIT_ERROR_ODB, "multi-pack-index file not found - '%s'", path);
		return -1;
	}

	if (!S_ISREG(st.st_mode) || !git__is_sizet(st.st_size)) {
		p_close(fd);
		git_error_set(GIT_ERROR_ODB, "invalid pack index '%s'", path);
		return -1;
	}
	idx_size = (size_t)st.st_size;

	idx = git__calloc(1, sizeof(git_multipack_index_file));
	GIT_ERROR_CHECK_ALLOC(idx);

	error = git_buf_sets(&idx->filename, path);
	if (error < 0)
		return error;

	error = git_futils_mmap_ro(&idx->index_map, fd, 0, idx_size);
	p_close(fd);
	if (error < 0) {
		git_multipack_index_free(idx);
		return error;
	}

	if ((error = git_multipack_index_parse(idx, idx->index_map.data, idx_size)) < 0) {
		git_multipack_index_free(idx);
		return error;
	}

	*idx_out = idx;
	return 0;
}

bool git_multipack_index_needs_refresh(
		const git_multipack_index_file *idx,
		const char *path)
{
	git_file fd = -1;
	struct stat st;
	ssize_t bytes_read;
	git_oid idx_checksum = {{0}};

	/* TODO: properly open the file without access time using O_NOATIME */
	fd = git_futils_open_ro(path);
	if (fd < 0)
		return true;

	if (p_fstat(fd, &st) < 0) {
		p_close(fd);
		return true;
	}

	if (!S_ISREG(st.st_mode) ||
	    !git__is_sizet(st.st_size) ||
	    (size_t)st.st_size != idx->index_map.len) {
		p_close(fd);
		return true;
	}

	if (p_lseek(fd, -20, SEEK_END) < 0) {
		p_close(fd);
		return true;
	}

	bytes_read = p_read(fd, &idx_checksum, GIT_OID_RAWSZ);
	p_close(fd);

	if (bytes_read < 0)
		return true;

	return git_oid_cmp(&idx_checksum, &idx->checksum) == 0;
}

int git_multipack_index_entry_find(
		git_multipack_entry *e,
		git_multipack_index_file *idx,
		const git_oid *short_oid,
		size_t len)
{
	int pos, found = 0;
	size_t pack_index;
	uint32_t hi, lo;
	const git_oid *current = NULL;
	const unsigned char *object_offset;
	off64_t offset;

	assert(idx);

	hi = ntohl(idx->oid_fanout[(int)short_oid->id[0]]);
	lo = ((short_oid->id[0] == 0x0) ? 0 : ntohl(idx->oid_fanout[(int)short_oid->id[0] - 1]));

	pos = git_pack__lookup_sha1(idx->oid_lookup, 20, lo, hi, short_oid->id);

	if (pos >= 0) {
		/* An object matching exactly the oid was found */
		found = 1;
		current = idx->oid_lookup + pos;
	} else {
		/* No object was found */
		/* pos refers to the object with the "closest" oid to short_oid */
		pos = -1 - pos;
		if (pos < (int)idx->num_objects) {
			current = idx->oid_lookup + pos;

			if (!git_oid_ncmp(short_oid, current, len))
				found = 1;
		}
	}

	if (found && len != GIT_OID_HEXSZ && pos + 1 < (int)idx->num_objects) {
		/* Check for ambiguousity */
		const git_oid *next = current + 1;

		if (!git_oid_ncmp(short_oid, next, len)) {
			found = 2;
		}
	}

	if (!found)
		return git_odb__error_notfound("failed to find offset for multi-pack index entry", short_oid, len);
	if (found > 1)
		return git_odb__error_ambiguous("found multiple offsets for multi-pack index entry");

	object_offset = idx->object_offsets + pos * 8;
	offset = ntohl(*((uint32_t *)(object_offset + 4)));
	if (offset & 0x80000000) {
		uint32_t object_large_offsets_pos = offset & 0x7fffffff;
		const unsigned char *object_large_offsets_index = idx->object_large_offsets;

		/* Make sure we're not being sent out of bounds */
		if (object_large_offsets_pos >= idx->num_object_large_offsets)
			return git_odb__error_notfound("invalid index into the object large offsets table", short_oid, len);

		object_large_offsets_index += 8 * object_large_offsets_pos;

		offset = (((uint64_t)ntohl(*((uint32_t *)(object_large_offsets_index + 0)))) << 32) |
				ntohl(*((uint32_t *)(object_large_offsets_index + 4)));
	}
	pack_index = ntohl(*((uint32_t *)(object_offset + 0)));
	if (pack_index >= git_vector_length(&idx->packfile_names))
		return multipack_error("invalid index into the packfile names table");
	e->pack_index = pack_index;
	e->offset = offset;
	git_oid_cpy(&e->sha1, current);
	return 0;
}

int git_multipack_index_foreach_entry(
		git_multipack_index_file *idx,
		git_odb_foreach_cb cb,
		void *data)
{
	size_t i;
	int error;

	assert(idx);

	for (i = 0; i < idx->num_objects; ++i)
		if ((error = cb(&idx->oid_lookup[i], data)) != 0)
			return git_error_set_after_callback(error);

	return error;
}

void git_multipack_index_close(git_multipack_index_file *idx)
{
	assert(idx);

	if (idx->index_map.data)
		git_futils_mmap_free(&idx->index_map);
	git_vector_free(&idx->packfile_names);
}

void git_multipack_index_free(git_multipack_index_file *idx)
{
	if (!idx)
		return;

	git_buf_dispose(&idx->filename);
	git_multipack_index_close(idx);
	git__free(idx);
}

static int packfile__cmp(const void *a_, const void *b_)
{
	const struct git_pack_file *a = a_;
	const struct git_pack_file *b = b_;

	return strcmp(a->pack_name, b->pack_name);
}

int git_multipack_index_writer_new(
		git_multipack_index_writer **out,
		const char *pack_dir)
{
	git_multipack_index_writer *w = git__calloc(1, sizeof(git_multipack_index_writer));
	GIT_ERROR_CHECK_ALLOC(w);

	if (git_buf_sets(&w->pack_dir, pack_dir) < 0) {
		git__free(w);
		return -1;
	}
	git_path_squash_slashes(&w->pack_dir);

	if (git_vector_init(&w->packs, 0, packfile__cmp) < 0) {
		git_buf_dispose(&w->pack_dir);
		git__free(w);
		return -1;
	}

	*out = w;
	return 0;
}

void git_multipack_index_writer_free(git_multipack_index_writer *w)
{
	struct git_pack_file *p;
	size_t i;

	if (!w)
		return;

	git_vector_foreach (&w->packs, i, p)
		git_mwindow_put_pack(p);
	git_vector_free(&w->packs);
	git_buf_dispose(&w->pack_dir);
	git__free(w);
}

int git_multipack_index_writer_add(
		git_multipack_index_writer *w,
		const char *idx_path)
{
	git_buf idx_path_buf = GIT_BUF_INIT;
	int error;
	struct git_pack_file *p;

	error = git_path_prettify(&idx_path_buf, idx_path, git_buf_cstr(&w->pack_dir));
	if (error < 0)
		return error;

	error = git_mwindow_get_pack(&p, git_buf_cstr(&idx_path_buf));
	git_buf_dispose(&idx_path_buf);
	if (error < 0)
		return error;

	error = git_vector_insert(&w->packs, p);
	if (error < 0) {
		git_mwindow_put_pack(p);
		return error;
	}

	return 0;
}

int git_multipack_index_writer_commit(
		git_multipack_index_writer *w)
{
	int error;
	git_buf midx = GIT_BUF_INIT, midx_path = GIT_BUF_INIT;

	error = git_buf_joinpath(&midx_path, git_buf_cstr(&w->pack_dir), "multi-pack-index");
	if (error < 0)
		return error;

	error = git_multipack_index_writer_dump(&midx, w);
	if (error < 0) {
		git_buf_dispose(&midx);
		git_buf_dispose(&midx_path);
		return error;
	}

	error = git_futils_writebuffer(&midx, git_buf_cstr(&midx_path), 0, 0644);
	git_buf_dispose(&midx);
	git_buf_dispose(&midx_path);
	if (error < 0)
		return error;

	return 0;
}

typedef git_array_t(git_multipack_entry) object_entry_array_t;

struct object_entry_cb_state {
	uint32_t pack_index;
	object_entry_array_t *object_entries_array;
};

static int object_entry__cb(const git_oid *oid, off64_t offset, void *data)
{
	struct object_entry_cb_state *state = (struct object_entry_cb_state *)data;

	git_multipack_entry *entry = git_array_alloc(*state->object_entries_array);
	GIT_ERROR_CHECK_ALLOC(entry);

	git_oid_cpy(&entry->sha1, oid);
	entry->offset = offset;
	entry->pack_index = state->pack_index;

	return 0;
}

static int object_entry__cmp(const void *a_, const void *b_)
{
	const git_multipack_entry *a = (const git_multipack_entry *)a_;
	const git_multipack_entry *b = (const git_multipack_entry *)b_;

	return git_oid_cmp(&a->sha1, &b->sha1);
}

static int write_offset(git_buf *midx, off64_t offset)
{
	int error;
	uint32_t word;

	word = htonl((uint32_t)((offset >> 32) & 0xffffffffu));
	error = git_buf_put(midx, (const char *)&word, sizeof(word));
	if (error < 0)
		return error;
	word = htonl((uint32_t)((offset >> 0) & 0xffffffffu));
	error = git_buf_put(midx, (const char *)&word, sizeof(word));
	if (error < 0)
		return error;

	return 0;
}

static int write_chunk_header(git_buf *midx, int chunk_id, off64_t offset)
{
	uint32_t word = htonl(chunk_id);
	int error = git_buf_put(midx, (const char *)&word, sizeof(word));
	if (error < 0)
		return error;
	return write_offset(midx, offset);

	return 0;
}

int git_multipack_index_writer_dump(
		git_buf *midx,
		git_multipack_index_writer *w)
{
	int error = 0;
	size_t i;
	struct git_pack_file *p;
	struct git_multipack_index_header hdr = {
			.signature = htonl(MULTIPACK_INDEX_SIGNATURE),
			.version = MULTIPACK_INDEX_VERSION,
			.object_id_version = MULTIPACK_INDEX_OBJECT_ID_VERSION,
			.base_midx_files = 0,
	};
	uint32_t oid_fanout_count;
	uint32_t object_large_offsets_count;
	uint32_t oid_fanout[256];
	off64_t offset;
	git_buf packfile_names = GIT_BUF_INIT,
		oid_lookup = GIT_BUF_INIT,
		object_offsets = GIT_BUF_INIT,
		object_large_offsets = GIT_BUF_INIT;
	git_oid idx_checksum = {{0}};
	git_multipack_entry *entry;
	object_entry_array_t object_entries_array = GIT_ARRAY_INIT;
	git_vector object_entries = GIT_VECTOR_INIT;

	git_vector_sort(&w->packs);
	git_vector_foreach (&w->packs, i, p) {
		git_buf relative_index = GIT_BUF_INIT;
		struct object_entry_cb_state state = {
				.pack_index = (uint32_t)i,
				.object_entries_array = &object_entries_array,
		};
		size_t path_len;

		error = git_buf_sets(&relative_index, p->pack_name);
		if (error < 0)
			goto cleanup;
		error = git_path_make_relative(&relative_index, git_buf_cstr(&w->pack_dir));
		if (error < 0) {
			git_buf_dispose(&relative_index);
			goto cleanup;
		}
		path_len = git_buf_len(&relative_index);
		if (path_len <= strlen(".pack") || git__suffixcmp(git_buf_cstr(&relative_index), ".pack") != 0) {
			git_buf_dispose(&relative_index);
			goto cleanup;
		}
		path_len -= strlen(".pack");

		git_buf_put(&packfile_names, git_buf_cstr(&relative_index), path_len);
		git_buf_puts(&packfile_names, ".idx");
		git_buf_putc(&packfile_names, '\0');
		git_buf_dispose(&relative_index);

		error = git_pack_foreach_entry_offset(p, object_entry__cb, &state);
		if (error < 0)
			goto cleanup;
	}

	/* Sort the object entries. */
	error = git_vector_init(&object_entries, git_array_size(object_entries_array), object_entry__cmp);
	if (error < 0)
		goto cleanup;
	git_array_foreach (object_entries_array, i, entry)
		error = git_vector_set(NULL, &object_entries, i, entry);
	git_vector_set_sorted(&object_entries, 0);
	git_vector_sort(&object_entries);
	git_vector_uniq(&object_entries, NULL);

	/* Pad the packfile names so it is a multiple of four. */
	while (git_buf_len(&packfile_names) & 3)
		git_buf_putc(&packfile_names, '\0');

	/* Fill the OID Fanout table. */
	oid_fanout_count = 0;
	for (i = 0; i < 256; i++) {
		while (oid_fanout_count < git_vector_length(&object_entries) &&
		       ((const git_multipack_entry *)git_vector_get(&object_entries, oid_fanout_count))->sha1.id[0] <= i)
			++oid_fanout_count;
		oid_fanout[i] = htonl(oid_fanout_count);
	}

	/* Fill the OID Lookup table. */
	git_vector_foreach (&object_entries, i, entry) {
		error = git_buf_put(&oid_lookup, (const char *)&entry->sha1, sizeof(entry->sha1));
		if (error < 0)
			goto cleanup;
	}

	/* Fill the Object Offsets and Object Large Offsets tables. */
	object_large_offsets_count = 0;
	git_vector_foreach (&object_entries, i, entry) {
		uint32_t word;

		word = htonl((uint32_t)entry->pack_index);
		error = git_buf_put(&object_offsets, (const char *)&word, sizeof(word));
		if (error < 0)
			goto cleanup;
		if (entry->offset >= 0x80000000l) {
			word = htonl(0x80000000u | object_large_offsets_count++);
			error = write_offset(&object_large_offsets, entry->offset);
		} else {
			word = htonl((uint32_t)entry->offset & 0x7fffffffu);
		}
		error = git_buf_put(&object_offsets, (const char *)&word, sizeof(word));
		if (error < 0)
			goto cleanup;
	}

	/* Write the header. */
	hdr.packfiles = htonl((uint32_t)git_vector_length(&w->packs));
	hdr.chunks = 4;
	if (git_buf_len(&object_large_offsets) > 0)
		hdr.chunks++;
	git_buf_put(midx, (const char *)&hdr, sizeof(hdr));

	/* Write the chunk headers. */
	offset = sizeof(hdr) + (hdr.chunks + 1) * 12;
	error = write_chunk_header(midx, MULTIPACK_INDEX_PACKFILE_NAMES_ID, offset);
	if (error < 0)
		goto cleanup;
	offset += git_buf_len(&packfile_names);
	error = write_chunk_header(midx, MULTIPACK_INDEX_OID_FANOUT_ID, offset);
	if (error < 0)
		goto cleanup;
	offset += sizeof(oid_fanout);
	error = write_chunk_header(midx, MULTIPACK_INDEX_OID_LOOKUP_ID, offset);
	if (error < 0)
		goto cleanup;
	offset += git_buf_len(&oid_lookup);
	error = write_chunk_header(midx, MULTIPACK_INDEX_OBJECT_OFFSETS_ID, offset);
	if (error < 0)
		goto cleanup;
	offset += git_buf_len(&object_offsets);
	if (git_buf_len(&object_large_offsets) > 0) {
		error = write_chunk_header(midx, MULTIPACK_INDEX_OBJECT_LARGE_OFFSETS_ID, offset);
		if (error < 0)
			goto cleanup;
		offset += git_buf_len(&object_large_offsets);
	}
	error = write_chunk_header(midx, 0, offset);
	if (error < 0)
		goto cleanup;

	/* Write all the chunks. */
	error = git_buf_put(midx, git_buf_cstr(&packfile_names), git_buf_len(&packfile_names));
	if (error < 0)
		goto cleanup;
	error = git_buf_put(midx, (const char *)oid_fanout, sizeof(oid_fanout));
	if (error < 0)
		goto cleanup;
	error = git_buf_put(midx, git_buf_cstr(&oid_lookup), git_buf_len(&oid_lookup));
	if (error < 0)
		goto cleanup;
	error = git_buf_put(midx, git_buf_cstr(&object_offsets), git_buf_len(&object_offsets));
	if (error < 0)
		goto cleanup;
	error = git_buf_put(midx, git_buf_cstr(&object_large_offsets), git_buf_len(&object_large_offsets));
	if (error < 0)
		goto cleanup;

	/* Compute the checksum and write the trailer. */
	error = git_hash_buf(&idx_checksum, git_buf_cstr(midx), git_buf_len(midx));
	if (error < 0)
		goto cleanup;
	error = git_buf_put(midx, (const char *)&idx_checksum, sizeof(idx_checksum));
	if (error < 0)
		goto cleanup;

cleanup:
	git_array_clear(object_entries_array);
	git_vector_free(&object_entries);
	git_buf_dispose(&packfile_names);
	git_buf_dispose(&oid_lookup);
	git_buf_dispose(&object_offsets);
	git_buf_dispose(&object_large_offsets);
	return error;
}
