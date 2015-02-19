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
#include "sfpf.h"
#include "convert.h"
#include "fileentry.h"
#include "logging.h"

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

const char *progname = NULL;

static void usage()
{
    SFMF_LOG("Usage: %s <in-dir> <out-dir> <meta-file> <blob-upper> <pack-upper> <avg-pack>\n\n"
             "    <in-dir> ....... Path to source tree\n"
             "    <out-dir> ...... Output directory\n"
             "    <meta-file> .... Textfile with metadata\n"
             "    <blob-upper> ... Maximum total size for embedded blobs (in KiB)\n"
             "    <pack-upper> ... Maximum size for files to be packed (in KiB)\n"
             "    <avg-pack> ..... Average target size of pack files (in KiB)\n"
             "\n", progname);
}

struct PackOptions {
    const char *in_dir;
    const char *out_dir;
    const char *meta_file;
    uint32_t blob_upper_kb;
    uint32_t pack_upper_kb;
    uint32_t avg_pack_kb;

    char *metadata_bytes;
    size_t metadata_length;
};

void sfmf_print_hash(const char *filename, struct SFMF_FileHash *hash)
{
    char tmp[512];

    int res = sfmf_filehash_format(hash, tmp, sizeof(tmp));
    assert(res != 0);

    SFMF_LOG("%s %s\n", tmp, filename ?: "");
}

struct PackEntry {
    struct FileList *files;
    uint32_t size; // sum of entries' current minimum size

    uint32_t packfile_size; // size of written pack file
    struct SFMF_FileHash packfile_hash; // hash of packfile
};

struct PackList {
    struct PackEntry *data;
    uint32_t length; // current length
    uint32_t size; // allocated size

    uint32_t max_bin_size_bytes;
};

typedef int (*packlist_foreach_func_t)(struct PackEntry *entry, void *user_data);

struct PackList *packlist_resize(struct PackList *list, uint32_t size)
{
    assert(size >= list->length);
    //SFMF_DEBUG("Resizing %p: %d -> %d (%d items)\n", list, list->size, size, list->length);
    list->size = size;
    list->data = realloc(list->data, list->size * sizeof(struct PackEntry));
    return list;
}

struct PackList *packlist_new(uint32_t max_bin_size_bytes)
{
    struct PackList *list = packlist_resize(calloc(1, sizeof(struct PackList)), 16);
    list->max_bin_size_bytes = max_bin_size_bytes;
    return list;
}

int packlist_foreach(struct PackList *list, packlist_foreach_func_t func, void *user_data)
{
    assert(list);

    for (int i=0; i<list->length; i++) {
        if (func(list->data + i, user_data)) {
            return 1;
        }
    }

    return 0;
}

struct packlist_try_insert_t {
    struct PackList *list;
    struct FileEntry *source;
};

int packlist_try_insert(struct PackEntry *entry, void *user_data)
{
    struct packlist_try_insert_t *insert = user_data;

    int32_t new_size = fileentry_get_min_size(insert->source);

    if (entry->size + new_size <= insert->list->max_bin_size_bytes) {
        // Append new file entry to existing pack
        filelist_append_clone(entry->files, insert->source);
        entry->size += new_size;
        return 1;
    }

    return 0;
}

void packlist_insert(struct PackList *list, struct FileEntry *source)
{
    struct packlist_try_insert_t insert = { list, source };

    if (!packlist_foreach(list, packlist_try_insert, &insert)) {
        if (list->size < list->length + 1) {
            list = packlist_resize(list, list->size * 2);
        }

        // Could not insert into existing packs - create new one
        struct PackEntry *entry = &(list->data[list->length++]);
        memset(entry, 0, sizeof(*entry));

        int32_t new_size = fileentry_get_min_size(source);

        entry->files = filelist_new();
        filelist_append_clone(entry->files, source);
        entry->size = new_size;
    }
}

int packlist_free_entry(struct PackEntry *entry, void *user_data)
{
    assert(entry->files);
    filelist_free(entry->files);
    return 0;
}

void packlist_free(struct PackList *list)
{
    assert(list);
    assert(list->data);

    //SFMF_DEBUG("Freeing pack: %p\n", list);
    packlist_foreach(list, packlist_free_entry, NULL);

    free(list->data);
    free(list);
}




static int parse_int_into(const char *str, uint32_t *target)
{
    char *endptr = NULL;
    long int tmp = 0;

    tmp = strtol(str, &endptr, 10);

    if (tmp != LONG_MIN && tmp != LONG_MAX && *str != '\0' && *endptr == '\0') {
        // the entire string is valid, and no under/overflow occurred
        *target = tmp;
        return 1;
    }

    return 0;
}

static int parse_opts(int argc, char *argv[], struct PackOptions *opts)
{
    int expected_argc = 7;

    if (argc != expected_argc) {
        SFMF_WARN("Invalid number of arguments: %d (expected %d)\n",
                argc, expected_argc);
        return 0;
    }

    opts->in_dir = argv[1];
    opts->out_dir = argv[2];
    opts->meta_file = argv[3];

    if (!parse_int_into(argv[4], &opts->blob_upper_kb)) {
        SFMF_WARN("Not a valid size: '%s'\n", argv[4]);
        return 0;
    }

    if (!parse_int_into(argv[5], &opts->pack_upper_kb)) {
        SFMF_WARN("Not a valid size: '%s'\n", argv[5]);
        return 0;
    }

    if (!parse_int_into(argv[6], &opts->avg_pack_kb)) {
        SFMF_WARN("Not a valid size: '%s'\n", argv[6]);
        return 0;
    }

    if (opts->avg_pack_kb < opts->pack_upper_kb) {
        SFMF_WARN("Average pack size (%d) is smaller than upper pack limit (%d)\n",
                opts->avg_pack_kb, opts->pack_upper_kb);
        return 0;
    }

    return 1;
}

void mark_duplicates(struct FileList *files)
{
    uint32_t savings = 0;

    for (int i=0; i<files->length; i++) {
        struct FileEntry *a = &(files->data[i]);

        /**
         * We cannot skip duplicate (a->duplicate) files here, as we might
         * find that those are hardlinked together with other files later;
         * example:
         *
         *  File 1 (content "A") inode number 1              \
         *  File 2 (content "A") inode number 2 \_ hardlink --- duplicate
         *  File 3 (content "A") inode number 2 /            /
         *
         *  Step 1: Mark File 2 as duplicate (of File 1)
         *  Step 2: Mark File 3 as duplicate (of File 1)
         *
         *  Step 3: Mark File 3 as hardlink of File 2
         **/

        if (a->st.st_size == 0 || !(S_ISREG(a->st.st_mode) || S_ISLNK(a->st.st_mode))) {
            continue;
        }

        for (int j=i+1; j<files->length; j++) {
            struct FileEntry *b = &(files->data[j]);
            if (b->st.st_size == 0 || !(S_ISREG(b->st.st_mode) || S_ISLNK(b->st.st_mode))) {
                continue;
            }

            if (sfmf_filehash_compare(&(a->hash), &(b->hash)) == 0) {
                if (!b->duplicate) {
                    SFMF_LOG("Marking as dup: %s (%d bytes)\n",
                            b->filename, fileentry_get_min_size(b));
                    savings += b->st.st_size;
                    b->duplicate = 1;
                }

                if (a->st.st_ino == b->st.st_ino) {
                    SFMF_LOG("Found hard link: %s <-> %s (storing reference)\n",
                            a->filename, b->filename);

                    // Assume we can only have regular files as hardlinks
                    assert(S_ISREG(b->st.st_mode));

                    // Store index of file that is the source of the hardlink; the source of
                    // the hardlink will always be smaller than the current index, so that at
                    // extraction time, the source file already exists.
                    b->hardlink_index = i;
                } else {
                    // This is a file with duplicate contents, but not hardlinked with the
                    // file which we have found to have duplicate contents
                }
            }
        }
    }

    SFMF_LOG("Savings of dup elimination: %d bytes\n", savings);
}


static int get_cutoff_min_size(struct FileEntry *entry, void *user_data)
{
    uint32_t *min_size = user_data;

    uint32_t size = fileentry_get_min_size(entry);
    if (size < *min_size) {
        *min_size = size;
    }

    return 0;
}

static int get_cutoff_max_size(struct FileEntry *entry, void *user_data)
{
    uint32_t *max_size = user_data;

    if (entry->st.st_size > *max_size) {
        *max_size = entry->st.st_size;
    }

    if (entry->zsize > *max_size) {
        *max_size = entry->zsize;
    }

    return 0;
}

struct cutoff_search_t {
    uint32_t cutoff;
    uint32_t sum;
};

static int get_cutoff_sum(struct FileEntry *entry, void *user_data)
{
    struct cutoff_search_t *search = user_data;

    if (entry->zsize > 0 && entry->zsize < entry->st.st_size && entry->zsize < search->cutoff) {
        search->sum += entry->zsize;
    } else if (entry->st.st_size < search->cutoff) {
        search->sum += entry->st.st_size;
    }

    return 0;
}

uint32_t get_cutoff_size_bytes(struct FileList *files, uint32_t blob_upper_bytes)
{
    uint32_t min_size = UINT32_MAX;
    uint32_t max_size = 0;

    (void)filelist_foreach(files, get_cutoff_min_size, &min_size);
    (void)filelist_foreach(files, get_cutoff_max_size, &max_size);

    //SFMF_DEBUG("Cutoff size range: [%d..%d]\n", min_size, max_size);

    assert(min_size < max_size);
    assert(max_size > 0);
    assert(min_size >= 0);

    uint32_t center = (max_size + min_size) / 2;
    uint32_t width = (max_size - min_size) / 2;
    // Best fit is the maximum center value that fits into the requirements
    uint32_t best_fit = 0;

    while (width > 1) {
        struct cutoff_search_t search = { center, 0 };
        (void)filelist_foreach(files, get_cutoff_sum, &search);

        //SFMF_DEBUG("for cutoff %d bytes got size: %d bytes (want %d bytes)\n",
        //           search.cutoff, search.sum, blob_upper_bytes);

        width /= 2;
        if (search.sum > blob_upper_bytes) {
            center -= width;
        } else if (search.sum < blob_upper_bytes) {
            if (best_fit < center) {
                best_fit = center;
            }
            center += width;
        }
    }

    // 2. Determine blob cutoff size (in bytes) based on upper limit
    return best_fit;
}

struct bucketize_context_t {
    struct FileList *included_files;
    struct FileList *packed_files;
    struct FileList *unpacked_files;

    // Maximum number of bytes for file to be directly included
    uint32_t blob_cutoff_size_bytes;

    // Maximum number of bytes for file to be put into a pack
    uint32_t pack_upper_bytes;
};

static int bucketize_list_entry(struct FileEntry *entry, void *user_data)
{
    struct bucketize_context_t *context = user_data;

    // Minimum possible size of entry (either compressed or uncompressed)
    uint32_t size = fileentry_get_min_size(entry);

    if (entry->duplicate) {
        // Skip adding this, as it's a duplicate
        return 0;
    }

    if (size == 0) {
        // Skip adding empty files
        return 0;
    }

    if (!S_ISLNK(entry->st.st_mode) && !S_ISREG(entry->st.st_mode)) {
        // Skip files if they are not symlinks or regular files
        return 0;
    }

    if (S_ISLNK(entry->st.st_mode) || size < context->blob_cutoff_size_bytes) {
        // Small enough to be put into manifest directly
        // (symlinks will always have their contents stored directly)
        filelist_append_clone(context->included_files, entry);
    } else if (S_ISREG(entry->st.st_mode) && size < context->pack_upper_bytes) {
        // File is small enough to be put into a pack
        filelist_append_clone(context->packed_files, entry);
    } else if (S_ISREG(entry->st.st_mode)) {
        // File is big enough to not be packed and served directly
        filelist_append_clone(context->unpacked_files, entry);
    } else {
        // No need to pack - not a regular file (or symlink)
    }

    return 0;
}

void bucketize_file_list(struct FileList *files, uint32_t blob_cutoff_size_bytes,
        uint32_t pack_upper_bytes, struct FileList **included_files,
        struct FileList **packed_files, struct FileList **unpacked_files)
{
    if (pack_upper_bytes <= blob_cutoff_size_bytes) {
        pack_upper_bytes = blob_cutoff_size_bytes + 1;
        SFMF_LOG("Correcting pack upper bytes limit to %d KiB (blob cutoff size is %d KiB)\n",
                pack_upper_bytes / 1024, blob_cutoff_size_bytes / 1024);
    }

    SFMF_LOG("Bucketizing file list...\n");

    // 3. Sort file entries into three buckets
    *included_files = filelist_new();
    *packed_files = filelist_new();
    *unpacked_files = filelist_new();

    struct bucketize_context_t context = {
        *included_files,
        *packed_files,
        *unpacked_files,

        blob_cutoff_size_bytes,
        pack_upper_bytes,
    };

    (void)filelist_foreach(files, bucketize_list_entry, &context);
}

static int make_packs_insert(struct FileEntry *entry, void *user_data)
{
    struct PackList *list = user_data;
    packlist_insert(list, entry);

    return 0;
}

struct PackList *make_packs(struct FileList *packed_files, uint32_t avg_pack_bytes)
{
    // 4. Rucksack sorting of packed files into packs
    struct PackList *list = packlist_new(avg_pack_bytes);
    (void)filelist_foreach(packed_files, make_packs_insert, list);

    return list;
}

static int write_full_blob(struct FileEntry *entry, void *user_data)
{
    struct PackOptions *opts = user_data;

    //SFMF_DEBUG("Would write blob for: %s\n", entry->filename);
    // 5. Write out full blobs files

    char tmp[512];
    int res = sfmf_filehash_format(&(entry->hash), tmp, sizeof(tmp));
    assert(res != 0);

    char *filename;
    filename = malloc(strlen(opts->out_dir) + 1 /* '/' */ + strlen(tmp) + strlen(".blob") + 1 /* '\0' */);

    sprintf(filename, "%s/%s.blob", opts->out_dir, tmp);

    int32_t min_size = fileentry_get_min_size(entry);
    if (min_size == entry->st.st_size) {
        // Write uncompressed
        convert_file(entry->filename, filename, CONVERT_FLAG_NONE);
    } else {
        // Write compressed
        convert_file(entry->filename, filename, CONVERT_FLAG_ZCOMPRESS);
    }

    free(filename);

    return 0;
}

int write_pack(struct PackEntry *entry, void *user_data)
{
    struct PackOptions *opts = user_data;

    //SFMF_DEBUG("Would write pack with %d items (%d MiB)\n",
    //        entry->files->length, entry->size / (1024 * 1024));
    // 6. Write out packs files

    struct SFPF_FileHeader header = {
        .magic = SFPF_MAGIC_NUMBER,
        .version = SFPF_CURRENT_VERSION,
        .metadata_size = opts->metadata_length,
        .blobs_length = entry->files->length,
    };
    SFMF_LOG("Putting %d files into this pack\n", header.blobs_length);

    uint32_t blob_size = header.blobs_length * sizeof(struct SFMF_BlobEntry);
    uint32_t payload_size = entry->size;

    entry->packfile_size = sizeof(header) + header.metadata_size + blob_size + payload_size;

    char *tmp = malloc(strlen(opts->out_dir) + strlen("/pack.tmp") + 1 /* '\0' */);
    sprintf(tmp, "%s/pack.tmp", opts->out_dir);
    FILE *fp = fopen(tmp, "wb");
    assert(fp != NULL);

    // Write file header
    int res = sfpf_fileheader_write(&header, fp);
    assert(res == 1);

    // Write metadata blob
    res = fwrite(opts->metadata_bytes, opts->metadata_length, 1, fp);
    assert(res == 1);

    // Write blob entry index
    uint32_t blob_offset = sizeof(header) + header.metadata_size + blob_size;
    for (int i=0; i<header.blobs_length; i++) {
        struct FileEntry *fentry = &(entry->files->data[i]);
        assert(S_ISREG(fentry->st.st_mode));
        sfmf_print_hash(fentry->filename, &(fentry->hash));

        uint32_t item_payload = fileentry_get_min_size(fentry);

        struct SFMF_BlobEntry entry;
        memcpy(&(entry.hash), &(fentry->hash), sizeof(struct SFMF_FileHash));
        entry.flags = (fentry->zsize == item_payload) ? BLOB_FLAG_ZCOMPRESSED : 0;
        entry.offset = blob_offset;
        entry.size = item_payload;

        res = sfmf_blobentry_write(&entry, fp);
        assert(res == 1);

        blob_offset += item_payload;
    }

    // Write blobs
    for (int i=0; i<header.blobs_length; i++) {
        struct FileEntry *fentry = &(entry->files->data[i]);
        uint32_t item_payload = fileentry_get_min_size(fentry);

        int zcompress = (fentry->zsize == item_payload);

        SFMF_LOG("Packing file %s (zcompress=%d)\n",
                 fentry->filename, zcompress);

        FILE *infile = fopen(fentry->filename, "rb");
        assert(infile != NULL);
        convert_file_fp(infile, fp, zcompress ? CONVERT_FLAG_ZCOMPRESS : CONVERT_FLAG_NONE);
        fclose(infile);
    }

    fclose(fp);

    struct FileEntry e;
    memset(&e, 0, sizeof(e));
    e.filename = tmp;
    fileentry_calculate_zsize_hash(&e);
    e.hash.size = entry->packfile_size;

    sfmf_print_hash(tmp, &(e.hash));

    // Save file hash
    memcpy(&(entry->packfile_hash), &(e.hash), sizeof(e.hash));

    char tmp2[512];
    sfmf_filehash_format(&(entry->packfile_hash), tmp2, sizeof(tmp2));

    char *tmp3 = malloc(strlen(opts->out_dir) + 1 /* '/' */ + strlen(tmp2) + strlen(".pack") + 1 /* '\0' */);
    sprintf(tmp3, "%s/%s.pack", opts->out_dir, tmp2);
    SFMF_LOG("Renaming: %s -> %s\n", tmp, tmp3);
    rename(tmp, tmp3);

    free(tmp3);
    free(tmp);

    return 0;
}

static const char *get_file_basename(struct PackOptions *opts, const char *filename)
{
    const char *result = filename + strlen(opts->in_dir);

    if (strcmp(filename, opts->in_dir) == 0) {
        return "/";
    }

    assert(strlen(filename) > strlen(opts->in_dir));

    if (*result != '/') {
        result++;
    }

    return result;
}

void write_manifest(struct PackOptions *opts, struct FileList *files,
        struct PackList *pack_list, struct FileList *included_files)
{
    // 7. Write out manifest file
    SFMF_DEBUG("writing manifest with %d entries\n", files->length);
    SFMF_DEBUG("will attach %d entries to manifest directly\n", included_files->length);

    uint32_t filename_table_size = 0;

    // Calculate size of filename table
    for (int i=0; i<files->length; i++) {
        struct FileEntry *source = &(files->data[i]);
        const char *filename = get_file_basename(opts, source->filename);
        filename_table_size += strlen(filename) + 1;
    }

    struct SFMF_FileHeader header = {
        .magic = SFMF_MAGIC_NUMBER,
        .version = SFMF_CURRENT_VERSION,
        .metadata_size = opts->metadata_length,
        .filename_table_size = filename_table_size,
        .entries_length = files->length,
        .packs_length = pack_list->length,
        .blobs_length = included_files->length,
    };

    uint32_t entries_size = header.entries_length * sizeof(struct SFMF_FileEntry);
    uint32_t packs_size = header.packs_length * sizeof(struct SFMF_PackEntry);
    uint32_t blobs_size = header.blobs_length * sizeof(struct SFMF_BlobEntry);

    char *tmp = malloc(strlen(opts->out_dir) + strlen("/manifest.sfmf") + 1 /* '\0' */);
    sprintf(tmp, "%s/manifest.sfmf", opts->out_dir);
    FILE *fp = fopen(tmp, "wb");
    assert(fp != NULL);

    // Write file header
    int res = sfmf_fileheader_write(&header, fp);
    assert(res == 1);

    // Write metadata blob
    res = fwrite(opts->metadata_bytes, opts->metadata_length, 1, fp);
    assert(res == 1);

    // Write filename table
    for (int i=0; i<header.entries_length; i++) {
        struct FileEntry *source = &(files->data[i]);
        const char *filename = get_file_basename(opts, source->filename);
        res = fwrite(filename, strlen(filename) + 1, 1, fp);
        assert(res == 1);
    }

    uint32_t filename_offset = 0;

    // Write file entries
    for (int i=0; i<header.entries_length; i++) {
        struct FileEntry *source = &(files->data[i]);
        int is_hardlink = (source->duplicate && source->hardlink_index != -1);

        struct SFMF_FileEntry entry;

        if (S_ISREG(source->st.st_mode)) {
            if (is_hardlink) {
                entry.type = ENTRY_HARDLINK;
            } else {
                entry.type = ENTRY_FILE;
            }
        } else if (S_ISLNK(source->st.st_mode)) {
            entry.type = ENTRY_SYMLINK;
        } else if (S_ISDIR(source->st.st_mode)) {
            entry.type = ENTRY_DIRECTORY;
        } else if (S_ISCHR(source->st.st_mode)) {
            entry.type = ENTRY_CHARACTER;
        } else if (S_ISBLK(source->st.st_mode)) {
            entry.type = ENTRY_BLOCK;
        } else if (S_ISFIFO(source->st.st_mode)) {
            entry.type = ENTRY_FIFO;
        } else {
            assert(0 /* unsupported file type */);
        }

        entry.mode = source->st.st_mode;
        entry.uid = source->st.st_uid;
        entry.gid = source->st.st_gid;
        entry.mtime = source->st.st_mtime;
        if (is_hardlink) {
            // for ENTRY_HARDLINK entries, "dev" is the hardlink index
            entry.dev = source->hardlink_index;
        } else {
            entry.dev = source->st.st_rdev;
        }
        entry.zsize = source->zsize;
        memcpy(&(entry.hash), &(source->hash), sizeof(struct SFMF_FileHash));

        entry.filename_offset = filename_offset;
        const char *filename = get_file_basename(opts, source->filename);
        filename_offset += strlen(filename) + 1;

        res = sfmf_fileentry_write(&entry, fp);
        assert(res == 1);
    }

    uint32_t offset = sizeof(header) + header.metadata_size + header.filename_table_size +
        entries_size + packs_size + blobs_size;

    // Write pack entries
    for (int i=0; i<header.packs_length; i++) {
        struct PackEntry *source = &(pack_list->data[i]);

        struct SFMF_PackEntry entry;
        memcpy(&(entry.hash), &(source->packfile_hash), sizeof(struct SFMF_FileHash));
        entry.offset = offset;
        entry.count = source->files->length;

        res = sfmf_packentry_write(&entry, fp);
        assert(res == 1);

        offset += entry.count * sizeof(struct SFMF_FileHash);
    }

    // Write blob entries
    for (int i=0; i<header.blobs_length; i++) {
        struct FileEntry *source = &(included_files->data[i]);

        uint32_t item_payload = fileentry_get_min_size(source);

        struct SFMF_BlobEntry entry;
        memcpy(&(entry.hash), &(source->hash), sizeof(struct SFMF_FileHash));
        entry.flags = (source->zsize == item_payload) ? BLOB_FLAG_ZCOMPRESSED : 0;
        entry.offset = offset;
        entry.size = item_payload;

        res = sfmf_blobentry_write(&entry, fp);
        assert(res == 1);

        offset += item_payload;
    }

    // Write pack payloads (hashes)
    for (int i=0; i<header.packs_length; i++) {
        struct PackEntry *source = &(pack_list->data[i]);

        for (int j=0; j<source->files->length; j++) {
            struct SFMF_FileHash *hash = &(source->files->data[j].hash);
            res = sfmf_filehash_write(hash, fp);
            assert(res == 1);
        }
    }

    // Write blob payloads
    for (int i=0; i<header.blobs_length; i++) {
        struct FileEntry *source = &(included_files->data[i]);
        uint32_t item_payload = fileentry_get_min_size(source);

        int zcompress = (source->zsize == item_payload);

        if (S_ISLNK(source->st.st_mode)) {
            char buf[PATH_MAX];
            memset(buf, 0, sizeof(buf));
            ssize_t length = readlink(source->filename, buf, sizeof(buf));
            assert(length != -1);
            SFMF_DEBUG("Writing symlink: '%s'\n", buf);
            convert_buffer_fp(buf, length, fp, zcompress ? CONVERT_FLAG_ZCOMPRESS : CONVERT_FLAG_NONE);
        } else {
            assert(S_ISREG(source->st.st_mode));
            FILE *infile = fopen(source->filename, "rb");
            assert(infile != NULL);
            convert_file_fp(infile, fp, zcompress ? CONVERT_FLAG_ZCOMPRESS : CONVERT_FLAG_NONE);
            fclose(infile);
        }
    }

    fclose(fp);
    free(tmp);
}


int main(int argc, char *argv[])
{
    struct PackOptions opts;

    memset(&opts, 0, sizeof(opts));
    progname = argv[0];

    if (!parse_opts(argc, argv, &opts)) {
        usage();
        return 1;
    }

    SFMF_LOG("Configuration:\n"
             "   Input directory:   %s\n"
             "   Output directory:  %s\n"
             "   Metadata file:     %s\n"
             "   Total blob size:   %d KiB\n"
             "   Max pack size:     %d KiB\n"
             "   Average pack size: %d KiB\n",
             opts.in_dir, opts.out_dir, opts.meta_file,
             opts.blob_upper_kb, opts.pack_upper_kb, opts.avg_pack_kb);

    FILE *mfp = fopen(opts.meta_file, "rb");
    assert(mfp != NULL);
    fseek(mfp, 0, SEEK_END);
    opts.metadata_length = ftell(mfp);
    SFMF_LOG("Read metadata: %ld bytes\n", opts.metadata_length);
    fseek(mfp, 0, SEEK_SET);
    opts.metadata_length++;
    opts.metadata_bytes = malloc(opts.metadata_length);
    // Always zero-terminate the metadata
    opts.metadata_bytes[opts.metadata_length-1] = '\0';
    int res = fread(opts.metadata_bytes, opts.metadata_length - 1, 1, mfp);
    assert(res == 1);
    fclose(mfp);

    // 1. List all files, plus their zsize
    struct FileList *files = get_file_list(opts.in_dir);

    // Search for duplicates based on hash and mark those
    mark_duplicates(files);

    // yeah, we need at least one file, otherwise there's something wrong
    assert(files->length > 0);

    SFMF_LOG("%d entries to consider\n", files->length);

    // 2. Determine blob cutoff size based on upper limit
    uint32_t blob_cutoff_size_b = get_cutoff_size_bytes(files, opts.blob_upper_kb * 1024);
    SFMF_LOG("Will include files < %d KiB (%d bytes)\n", blob_cutoff_size_b / 1024,
            blob_cutoff_size_b);

    // 3. Sort file entries into three buckets
    struct FileList *included_files = NULL;
    struct FileList *packed_files = NULL;
    struct FileList *unpacked_files = NULL;
    bucketize_file_list(files, blob_cutoff_size_b, opts.pack_upper_kb * 1024,
            &included_files, &packed_files, &unpacked_files);

    SFMF_LOG("Stats: %d included, %d packed, %d unpacked\n",
            included_files->length, packed_files->length, unpacked_files->length);

    // TODO: Maybe have an educated guess which files we always need packed,
    // and put those ideally into the same packs before packing the rest

    // 4. Bin packing of packed files into packs
    struct PackList *pack_list = make_packs(packed_files, opts.avg_pack_kb * 1024);

    SFMF_LOG("Need %d packs a %d KiB\n", pack_list->length, opts.avg_pack_kb);

    // 5. Write out full blobs files
    (void)filelist_foreach(unpacked_files, write_full_blob, &opts);
    
    // 6. Write out packs files
    packlist_foreach(pack_list, write_pack, &opts);

    // 7. Write out manifest file
    write_manifest(&opts, files, pack_list, included_files);

    filelist_free(files);
    filelist_free(included_files);
    filelist_free(packed_files);
    filelist_free(unpacked_files);
    packlist_free(pack_list);

    free(opts.metadata_bytes);

    return 0;
}
