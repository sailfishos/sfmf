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

#ifndef SFMF_FILEENTRY_H
#define SFMF_FILEENTRY_H

#include "sfmf.h"

#define _XOPEN_SOURCE 500
#define __USE_XOPEN_EXTENDED

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

struct FileEntry {
    char *filename;
    struct stat st;
    uint32_t zsize;
    struct SFMF_FileHash hash;
    int duplicate; // set to 1 if we don't need to store this (hash match with another file)
    int hardlink_index; // if it's a duplicate, stores the index of the matching file (otherwise -1)
};

struct FileList {
    struct FileEntry *data;
    uint32_t length; // current length
    uint32_t size; // allocated size
};

/**
 * Return value: 1 to abort and return entry in filelist_foreach, 0 to continue
 **/
typedef int (*filelist_foreach_func_t)(struct FileEntry *entry, void *user_data);

enum FileListFlags {
    FILE_LIST_NONE = 0,
    FILE_LIST_CALCULATE_HASH,
};

struct FileList *filelist_new();
void filelist_append(struct FileList *list, const char *filename, enum FileListFlags flags);
void filelist_append_clone(struct FileList *list, struct FileEntry *source);
void filelist_free(struct FileList *list);

struct FileList *get_file_list(const char *root);
struct FileList *extend_file_list(struct FileList *list, const char *root, enum FileListFlags flags);
// Returns the first entry for which func returns 1, or NULL if none of them does
struct FileEntry *filelist_foreach(struct FileList *list, filelist_foreach_func_t func, void *user_data);

int32_t fileentry_get_min_size(struct FileEntry *entry);
void fileentry_calculate_zsize_hash(struct FileEntry *entry);

#endif /* SFMF_FILEENTRY_H */
