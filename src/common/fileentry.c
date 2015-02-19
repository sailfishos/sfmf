/**
 * Sailfish OS Factory Snapshot Update
 * Copyright (C) 2015 Jolla Ltd.
 * Contact: Thomas Perl <thomas.perl@jolla.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 **/

#include "sfmf.h"
#include "convert.h"
#include "fileentry.h"
#include "logging.h"
#include "policy.h"

#include "sha1.h"

#define _XOPEN_SOURCE 500
#define __USE_XOPEN_EXTENDED

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <ftw.h>


static struct FileList *filelist_resize(struct FileList *list, uint32_t size)
{
    assert(size >= list->length);
    //SFMF_DEBUG("Resizing %p: %d -> %d (%d items)\n", list, list->size, size, list->length);
    list->size = size;
    list->data = realloc(list->data, list->size * sizeof(struct FileEntry));
    return list;
}

struct FileList *filelist_new()
{
    return filelist_resize(calloc(1, sizeof(struct FileList)), 128);
}

int32_t fileentry_get_min_size(struct FileEntry *entry)
{
    // Minimum possible size of entry (either compressed or uncompressed)
    uint32_t size = entry->st.st_size;
    if (entry->zsize > 0 && entry->zsize < size) {
        size = entry->zsize;
    }
    return size;
}

struct FileEntry *filelist_foreach(struct FileList *list, filelist_foreach_func_t func, void *user_data)
{
    assert(list);

    for (int i=0; i<list->length; i++) {
        struct FileEntry *entry = list->data + i;
        if (func(entry, user_data)) {
            return entry;
        }
    }

    return NULL;
}

void fileentry_calculate_zsize_hash(struct FileEntry *entry)
{
    convert_file_zsize_hash(entry->filename, &(entry->hash), &(entry->zsize));
}

static void SHA1(const unsigned char *buf, size_t length, unsigned char *hash)
{
    SHA1_CTX ctx;
    memset(&ctx, 0, sizeof(ctx));
    SHA1_Init(&ctx);
    SHA1_Update(&ctx, buf, length);
    SHA1_Final(&ctx, hash);
}

void filelist_append(struct FileList *list, const char *filename, enum FileListFlags flags)
{
    if (list->size < list->length + 1) {
        list = filelist_resize(list, list->size * 2);
    }

    struct FileEntry *entry = &(list->data[list->length++]);
    memset(entry, 0, sizeof(*entry));

    entry->filename = strdup(filename);
    if (lstat(entry->filename, &entry->st) != 0) {
        SFMF_FAIL("Can't stat %s: %s\n", entry->filename, strerror(errno));
    }

    if (S_ISLNK(entry->st.st_mode)) {
        //SFMF_DEBUG("symlink %s\n", filename);
    } else if (S_ISREG(entry->st.st_mode)) {
        //SFMF_DEBUG("file %s\n", filename);
    } else if (S_ISDIR(entry->st.st_mode)) {
        //SFMF_DEBUG("directory %s\n", filename);
    } else if (S_ISCHR(entry->st.st_mode)) {
        //SFMF_DEBUG("character device %s\n", filename);
    } else if (S_ISBLK(entry->st.st_mode)) {
        //SFMF_DEBUG("block device %s\n", filename);
    } else if (S_ISFIFO(entry->st.st_mode)) {
        //SFMF_DEBUG("fifo %s\n", filename);
    } else if (S_ISSOCK(entry->st.st_mode)) {
        SFMF_WARN("socket %s (ignoring)\n", filename);
        memset(entry, 0, sizeof(*entry));
        list->length--;
        return;
    } else {
        if (sfmf_policy_get_ignore_unsupported()) {
            SFMF_WARN("Unsupported type for %s\n", filename);
            memset(entry, 0, sizeof(*entry));
            list->length--;
            return;
        } else {
            SFMF_FAIL("Unsupported type for %s\n", filename);
        }
    }

    if (S_ISREG(entry->st.st_mode) && entry->st.st_size > 0) {
        entry->hash.size = entry->st.st_size;

        if ((flags & FILE_LIST_CALCULATE_HASH) != 0) {
            // If it's a nonempty regular file or symlink, check how well it compresses
            //SFMF_DEBUG("Calculating size and hash: %s\n", entry->filename);
            fileentry_calculate_zsize_hash(entry);
            //sfmf_print_hash(entry->filename, &(entry->hash));
        } else {
            //SFMF_DEBUG("Not calculating hash of file: %s\n", entry->filename);
            entry->hash.hashtype = HASHTYPE_LAZY;
        }
    } else if (S_ISLNK(entry->st.st_mode)) {
        // We never try to compress symlink contents
        entry->zsize = 0;
        char buf[PATH_MAX];
        memset(buf, 0, sizeof(buf));
        ssize_t length = readlink(entry->filename, buf, sizeof(buf));
        assert(length != -1);
        entry->hash.size = length;
        entry->hash.hashtype = HASHTYPE_SHA1;

        SHA1((unsigned char *)buf, length, (unsigned char *)&(entry->hash.hash));
        //sfmf_print_hash(entry->filename, &(entry->hash));
    }

    // Reset duplicate and hardlink indicators
    entry->duplicate = 0;
    entry->hardlink_index = -1;
}

void filelist_append_clone(struct FileList *list, struct FileEntry *source)
{
    if (list->size < list->length + 1) {
        list = filelist_resize(list, list->size * 2);
    }

    struct FileEntry *entry = &(list->data[list->length++]);

    // Most fields can be copied as-is (they're just data)
    memcpy(entry, source, sizeof(struct FileEntry));
    // Need to duplicate the filename, as it's a pointer
    entry->filename = strdup(entry->filename);
}

static int filelist_free_entry(struct FileEntry *entry, void *user_data)
{
    if (entry->filename) {
        //SFMF_DEBUG("Freeing entry: %p\n", entry);
        free(entry->filename);
    }

    return 0;
}

void filelist_free(struct FileList *list)
{
    assert(list);
    assert(list->data);

    //SFMF_DEBUG("Freeing list: %p\n", list);
    (void)filelist_foreach(list, filelist_free_entry, NULL);
    free(list->data);
    free(list);
}

struct VisitDirectoryContext {
    struct FileList *list;
    enum FileListFlags flags;
};

static struct VisitDirectoryContext *visit_directory_context = NULL;

static int visit_directory(const char *fpath, const struct stat *sb,
        int typeflag, struct FTW *ftwbuf)
{
    // Add this entry to the list
    filelist_append(visit_directory_context->list, fpath, visit_directory_context->flags);
    return 0;
}

struct FileList *get_file_list(const char *root)
{
    return extend_file_list(NULL, root, FILE_LIST_CALCULATE_HASH);
}

struct FileList *extend_file_list(struct FileList *list, const char *root, enum FileListFlags flags)
{
    if (list == NULL) {
        list = filelist_new();
    }

    struct VisitDirectoryContext ctx = {
        list,
        flags,
    };
    visit_directory_context = &ctx;

    nftw(root, visit_directory, 0, FTW_PHYS);

    return list;
}
