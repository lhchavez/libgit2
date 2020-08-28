/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "mwindow.h"

#include "vector.h"
#include "futils.h"
#include "map.h"
#include "runtime.h"
#include "strmap.h"
#include "pack.h"

#define DEFAULT_WINDOW_SIZE \
	(sizeof(void*) >= 8 \
		? 1 * 1024 * 1024 * 1024 \
		: 32 * 1024 * 1024)

#define DEFAULT_MAPPED_LIMIT \
	((1024 * 1024) * (sizeof(void*) >= 8 ? 8192ULL : 256UL))

/* default is unlimited */
#define DEFAULT_FILE_LIMIT 0

size_t git_mwindow__window_size = DEFAULT_WINDOW_SIZE;
size_t git_mwindow__mapped_limit = DEFAULT_MAPPED_LIMIT;
size_t git_mwindow__file_limit = DEFAULT_FILE_LIMIT;

/* Mutex to control access */
git_mutex git__mwindow_mutex;

/* Whenever you want to read or modify this, grab git__mwindow_mutex */
git_mwindow_ctl git_mwindow__mem_ctl;

/* Global list of mwindow files, to open packs once across repos */
git_strmap *git__pack_cache = NULL;

static void git_mwindow_global_shutdown(void)
{
	git_strmap *tmp = git__pack_cache;

	git_mutex_free(&git__mwindow_mutex);

	git__pack_cache = NULL;
	git_strmap_free(tmp);
}

int git_mwindow_global_init(void)
{
	int error;

	GIT_ASSERT(!git__pack_cache);

	if ((error = git_mutex_init(&git__mwindow_mutex)) < 0 ||
	    (error = git_strmap_new(&git__pack_cache)) < 0)
	    return error;

	return git_runtime_shutdown_register(git_mwindow_global_shutdown);
}

int git_mwindow_get_pack(struct git_pack_file **out, const char *path)
{
	struct git_pack_file *pack;
	char *packname;
	int error;

	if ((error = git_packfile__name(&packname, path)) < 0)
		return error;

	if (git_mutex_lock(&git__mwindow_mutex) < 0) {
		git_error_set(GIT_ERROR_OS, "failed to lock mwindow mutex");
		return -1;
	}

	pack = git_strmap_get(git__pack_cache, packname);
	git__free(packname);

	if (pack != NULL) {
		git_atomic_inc(&pack->refcount);
		git_mutex_unlock(&git__mwindow_mutex);
		*out = pack;
		return 0;
	}

	/* If we didn't find it, we need to create it */
	if ((error = git_packfile_alloc(&pack, path)) < 0) {
		git_mutex_unlock(&git__mwindow_mutex);
		return error;
	}

	git_atomic_inc(&pack->refcount);

	error = git_strmap_set(git__pack_cache, pack->pack_name, pack);
	git_mutex_unlock(&git__mwindow_mutex);

	if (error < 0) {
		git_packfile_free(pack);
		return -1;
	}

	*out = pack;
	return 0;
}

int git_mwindow_put_pack(struct git_pack_file *pack)
{
	int count, error;

	if ((error = git_mutex_lock(&git__mwindow_mutex)) < 0)
		return error;

	/* put before get would be a corrupted state */
	GIT_ASSERT(git__pack_cache);

	/* if we cannot find it, the state is corrupted */
	GIT_ASSERT(git_strmap_exists(git__pack_cache, pack->pack_name));

	count = git_atomic_dec(&pack->refcount);
	if (count == 0) {
		git_strmap_delete(git__pack_cache, pack->pack_name);
		git_packfile_free(pack);
	}

	git_mutex_unlock(&git__mwindow_mutex);
	return 0;
}

int git_mwindow_free_all(git_mwindow_file *mwf)
{
	int error;

	if (git_mutex_lock(&git__mwindow_mutex)) {
		git_error_set(GIT_ERROR_THREAD, "unable to lock mwindow mutex");
		return -1;
	}

	error = git_mwindow_free_all_locked(mwf);

	git_mutex_unlock(&git__mwindow_mutex);

	return error;
}

/*
 * Free all the windows in a sequence, typically because we're done
 * with the file
 */
int git_mwindow_free_all_locked(git_mwindow_file *mwf)
{
	git_mwindow_ctl *ctl = &git_mwindow__mem_ctl;
	size_t i;

	/*
	 * Remove these windows from the global list
	 */
	for (i = 0; i < ctl->windowfiles.length; ++i){
		if (git_vector_get(&ctl->windowfiles, i) == mwf) {
			git_vector_remove(&ctl->windowfiles, i);
			break;
		}
	}

	if (ctl->windowfiles.length == 0) {
		git_vector_free(&ctl->windowfiles);
		ctl->windowfiles.contents = NULL;
	}

	while (mwf->windows) {
		git_mwindow *w = mwf->windows;
		GIT_ASSERT(w->inuse_cnt == 0);

		ctl->mapped -= w->window_map.len;
		ctl->open_windows--;

		git_futils_mmap_free(&w->window_map);

		mwf->windows = w->next;
		git__free(w);
	}

	return 0;
}

/*
 * Check if a window 'win' contains the address 'offset'
 */
int git_mwindow_contains(git_mwindow *win, off64_t offset)
{
	off64_t win_off = win->offset;
	return win_off <= offset
		&& offset <= (off64_t)(win_off + win->window_map.len);
}

#define GIT_MWINDOW__LRU -1
#define GIT_MWINDOW__MRU 1

/*
 * Find the least- or most-recently-used window in a file that is not currently
 * being used. The 'only_unused' flag controls whether the caller requires the
 * file to only have unused windows. If '*out_window' is non-null, it is used as
 * a starting point for the comparison.
 *
 * Returns whether such a window was found in the file.
 */
static bool git_mwindow_scan_recently_used(
		git_mwindow_file *mwf,
		git_mwindow **out_window,
		git_mwindow **out_last,
		bool only_unused,
		int comparison_sign)
{
	git_mwindow *w, *w_last;
	git_mwindow *lru_window = NULL, *lru_last = NULL;
	bool found = false;

	GIT_ASSERT_ARG(mwf);
	GIT_ASSERT_ARG(out_window);

	lru_window = *out_window;
	if (out_last)
		lru_last = *out_last;

	for (w_last = NULL, w = mwf->windows; w; w_last = w, w = w->next) {
		if (w->inuse_cnt) {
			if (only_unused)
				return false;
			/* This window is currently being used. Skip it. */
			continue;
		}

		/*
		 * If the current one is more (or less) recent than the last one,
		 * store it in the output parameter. If lru_window is NULL,
		 * it's the first loop, so store it as well.
		 */
		if (!lru_window ||
				(comparison_sign == GIT_MWINDOW__LRU && lru_window->last_used > w->last_used) ||
				(comparison_sign == GIT_MWINDOW__MRU && lru_window->last_used < w->last_used)) {
			lru_window = w;
			lru_last = w_last;
			found = true;
		}
	}

	if (!found)
		return false;

	*out_window = lru_window;
	if (out_last)
		*out_last = lru_last;
	return true;
}

/*
 * Close the least recently used window (that is currently not being used) out
 * of all the files. Called under lock from new_window.
 */
static int git_mwindow_close_lru_window(void)
{
	git_mwindow_ctl *ctl = &git_mwindow__mem_ctl;
	git_mwindow_file *cur;
	size_t i;
	git_mwindow *lru_window = NULL, *lru_last = NULL, **list = NULL;

	git_vector_foreach(&ctl->windowfiles, i, cur) {
		if (git_mwindow_scan_recently_used(
				cur, &lru_window, &lru_last, false, GIT_MWINDOW__LRU)) {
			list = &cur->windows;
		}
	}

	if (!lru_window) {
		git_error_set(GIT_ERROR_OS, "failed to close memory window; couldn't find LRU");
		return -1;
	}

	ctl->mapped -= lru_window->window_map.len;
	git_futils_mmap_free(&lru_window->window_map);

	if (lru_last)
		lru_last->next = lru_window->next;
	else if (list)
		*list = lru_window->next;

	git__free(lru_window);
	ctl->open_windows--;

	return 0;
}

/*
 * Close the file that does not have any open windows AND whose
 * most-recently-used window is the least-recently used one across all
 * currently open files.
 *
 * Called under lock from new_window.
 */
static int git_mwindow_close_lru_file(void)
{
	git_mwindow_ctl *ctl = &git_mwindow__mem_ctl;
	git_mwindow_file *lru_file = NULL, *current_file = NULL;
	git_mwindow *lru_window = NULL;
	size_t i;

	git_vector_foreach(&ctl->windowfiles, i, current_file) {
		git_mwindow *mru_window = NULL;
		if (!git_mwindow_scan_recently_used(
				current_file, &mru_window, NULL, true, GIT_MWINDOW__MRU)) {
			continue;
		}
		if (!lru_window || lru_window->last_used > mru_window->last_used)
			lru_file = current_file;
	}

	if (!lru_file) {
		git_error_set(GIT_ERROR_OS, "failed to close memory window file; couldn't find LRU");
		return -1;
	}

	git_mwindow_free_all_locked(lru_file);
	p_close(lru_file->fd);
	lru_file->fd = -1;

	return 0;
}

/* This gets called under lock from git_mwindow_open */
static git_mwindow *new_window(
	git_file fd,
	off64_t size,
	off64_t offset)
{
	git_mwindow_ctl *ctl = &git_mwindow__mem_ctl;
	size_t walign = git_mwindow__window_size / 2;
	off64_t len;
	git_mwindow *w;

	w = git__malloc(sizeof(*w));

	if (w == NULL)
		return NULL;

	memset(w, 0x0, sizeof(*w));
	w->offset = (offset / walign) * walign;

	len = size - w->offset;
	if (len > (off64_t)git_mwindow__window_size)
		len = (off64_t)git_mwindow__window_size;

	ctl->mapped += (size_t)len;

	while (git_mwindow__mapped_limit < ctl->mapped &&
			git_mwindow_close_lru_window() == 0) /* nop */;

	/*
	 * We treat `mapped_limit` as a soft limit. If we can't find a
	 * window to close and are above the limit, we still mmap the new
	 * window.
	 */

	if (git_futils_mmap_ro(&w->window_map, fd, w->offset, (size_t)len) < 0) {
		/*
		 * The first error might be down to memory fragmentation even if
		 * we're below our soft limits, so free up what we can and try again.
		 */

		while (git_mwindow_close_lru_window() == 0)
			/* nop */;

		if (git_futils_mmap_ro(&w->window_map, fd, w->offset, (size_t)len) < 0) {
			git__free(w);
			return NULL;
		}
	}

	ctl->mmap_calls++;
	ctl->open_windows++;

	if (ctl->mapped > ctl->peak_mapped)
		ctl->peak_mapped = ctl->mapped;

	if (ctl->open_windows > ctl->peak_open_windows)
		ctl->peak_open_windows = ctl->open_windows;

	return w;
}

/*
 * Open a new window, closing the least recenty used until we have
 * enough space. Don't forget to add it to your list
 */
unsigned char *git_mwindow_open(
	git_mwindow_file *mwf,
	git_mwindow **cursor,
	off64_t offset,
	size_t extra,
	unsigned int *left)
{
	git_mwindow_ctl *ctl = &git_mwindow__mem_ctl;
	git_mwindow *w = *cursor;

	if (git_mutex_lock(&git__mwindow_mutex)) {
		git_error_set(GIT_ERROR_THREAD, "unable to lock mwindow mutex");
		return NULL;
	}

	if (!w || !(git_mwindow_contains(w, offset) && git_mwindow_contains(w, offset + extra))) {
		if (w) {
			w->inuse_cnt--;
		}

		for (w = mwf->windows; w; w = w->next) {
			if (git_mwindow_contains(w, offset) &&
				git_mwindow_contains(w, offset + extra))
				break;
		}

		/*
		 * If there isn't a suitable window, we need to create a new
		 * one.
		 */
		if (!w) {
			w = new_window(mwf->fd, mwf->size, offset);
			if (w == NULL) {
				git_mutex_unlock(&git__mwindow_mutex);
				return NULL;
			}
			w->next = mwf->windows;
			mwf->windows = w;
		}
	}

	/* If we changed w, store it in the cursor */
	if (w != *cursor) {
		w->last_used = ctl->used_ctr++;
		w->inuse_cnt++;
		*cursor = w;
	}

	offset -= w->offset;

	if (left)
		*left = (unsigned int)(w->window_map.len - offset);

	git_mutex_unlock(&git__mwindow_mutex);
	return (unsigned char *) w->window_map.data + offset;
}

int git_mwindow_file_register(git_mwindow_file *mwf)
{
	git_mwindow_ctl *ctl = &git_mwindow__mem_ctl;
	int ret;

	if (git_mutex_lock(&git__mwindow_mutex)) {
		git_error_set(GIT_ERROR_THREAD, "unable to lock mwindow mutex");
		return -1;
	}

	if (ctl->windowfiles.length == 0 &&
	    git_vector_init(&ctl->windowfiles, 8, NULL) < 0) {
		git_mutex_unlock(&git__mwindow_mutex);
		return -1;
	}

	if (git_mwindow__file_limit) {
		while (git_mwindow__file_limit <= ctl->windowfiles.length &&
				git_mwindow_close_lru_file() == 0) /* nop */;
	}

	ret = git_vector_insert(&ctl->windowfiles, mwf);
	git_mutex_unlock(&git__mwindow_mutex);

	return ret;
}

void git_mwindow_file_deregister(git_mwindow_file *mwf)
{
	git_mwindow_ctl *ctl = &git_mwindow__mem_ctl;
	git_mwindow_file *cur;
	size_t i;

	if (git_mutex_lock(&git__mwindow_mutex))
		return;

	git_vector_foreach(&ctl->windowfiles, i, cur) {
		if (cur == mwf) {
			git_vector_remove(&ctl->windowfiles, i);
			git_mutex_unlock(&git__mwindow_mutex);
			return;
		}
	}
	git_mutex_unlock(&git__mwindow_mutex);
}

void git_mwindow_close(git_mwindow **window)
{
	git_mwindow *w = *window;
	if (w) {
		if (git_mutex_lock(&git__mwindow_mutex)) {
			git_error_set(GIT_ERROR_THREAD, "unable to lock mwindow mutex");
			return;
		}

		w->inuse_cnt--;
		git_mutex_unlock(&git__mwindow_mutex);
		*window = NULL;
	}
}
