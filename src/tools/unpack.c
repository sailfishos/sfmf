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
#include "readpack.h"
#include "logging.h"
#include "dirstack.h"
#include "policy.h"
#include "cleanup.h"
#include "control.h"

#define _XOPEN_SOURCE 500
#define __USE_XOPEN_EXTENDED

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>
#include <getopt.h>
#include <argp.h>
#include <math.h>

#define FREE_VAR(x) free(x), (x) = 0


#define MAX_SOURCE_DIRS 64

const char *progname = NULL;

enum BlobResultType {
    BLOB_RESULT_INVALID = 0,
    BLOB_RESULT_INCLUDED,
    BLOB_RESULT_LOCAL,
    BLOB_RESULT_PACKED,
    BLOB_RESULT_FULL,
    BLOB_RESULT_EMPTY,
    BLOB_RESULT_HARDLINK,
};

struct BlobResult {
    union {
        enum BlobResultType type;
        struct {
            enum BlobResultType type;
            struct SFMF_BlobEntry *entry;
        } included;
        struct {
            enum BlobResultType type;
            struct FileEntry *entry;
        } local;
        struct {
            enum BlobResultType type;
            struct SFMF_PackEntry *entry;
        } packed;
    };
};

struct UnpackFileEntry {
    struct SFMF_FileEntry entry;
    struct BlobResult blob_result;
    char *target_filename;
};

struct UnpackOptions {
    // Command line options
    char *filename;
    char *outputdir;
    char *sourcedirs[MAX_SOURCE_DIRS];
    int n_sourcedirs;

    int verbose;
    int progress;
    int download_only;
    int offline_mode;

    struct {
        int current;
        int total;
    } steps;
    int abort;

    // Local cache directory for storing files
    int keep_cached_files;
    char *cachedir;
    struct FileList *cached_files;

    // Runtime context data
    FILE *fp;
    struct SFMF_FileHeader header;
    char *metadata;
    char *filename_table;
    struct UnpackFileEntry *fentries;
    struct SFMF_PackEntry *pentries;
    struct SFMF_BlobEntry *bentries;
    struct SFMF_FileHash **pack_hashes;
    struct FileList *local_files;
    struct DirStack *dir_stack;
    char *manifest_local_filename;
    char *temporary_download;
    int success;
};

const char *argp_program_version = "sfmf-unpack " VERSION;
const char *argp_program_bug_address = "info@sailfishos.org";

// Forward declarations
static char *get_filename_in_cache(struct UnpackOptions *opts, const char *filename);
static int file_exists(const char *filename);

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    struct UnpackOptions *opts = state->input;

    switch (key) {
        case 'v':
            opts->verbose++;
            break;
        case 'd':
            opts->download_only = 1;
            break;
        case 'D':
            opts->offline_mode = 1;
            break;
        case 'p':
            opts->progress = 1;
            break;
        case 'C':
            opts->cachedir = strdup(arg);
            // FIXME: Create parent directory, error checking, etc..
            mkdir(opts->cachedir, 0755);
            opts->keep_cached_files = 1;
            break;
        case ARGP_KEY_ARG:
            if (state->arg_num == 0) {
                opts->filename = strdup(arg);
            } else if (state->arg_num == 1) {
                opts->outputdir = strdup(arg);
            } else if (opts->n_sourcedirs < MAX_SOURCE_DIRS) {
                opts->sourcedirs[opts->n_sourcedirs++] = strdup(arg);
            } else {
                argp_usage(state);
            }
            break;
        case ARGP_KEY_END:
            if (opts->filename == NULL) {
                if (opts->cachedir) {
                    // If we have a cache directory, assume there is a file
                    // "manifest.sfmf" in it if not provided
                    opts->filename = get_filename_in_cache(opts, "manifest.sfmf");
                    if (file_exists(opts->filename)) {
                        SFMF_LOG("Using %s as manifest file\n", opts->filename);
                    } else {
                        SFMF_FAIL("File not found: %s\n", opts->filename);
                    }
                } else {
                    // We do need a manifest filename
                    argp_usage(state);
                }
            }

            if (opts->outputdir == NULL) {
                if (opts->download_only) {
                    // Use the current directory, as we are not going to write
                    // the data (download-only mode)
                    opts->outputdir = strdup(".");
                } else {
                    // We do need a output directory
                    argp_usage(state);
                }
            }
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }

    return 0;
}

static void parse_opts(int argc, char *argv[], struct UnpackOptions *opts)
{
    struct argp_option options[] = {
        // Controlling the output
        { "verbose", 'v', 0, 0, "Verbose output" },
        { "progress", 'p', 0, 0, "Show progress meter" },

        // Download and cache directory controlling
        { "download", 'd', 0, 0, "Download only, do not unpack" },
        { "offline", 'D', 0, 0, "Do not try to download anything" },
        { "cache", 'C', "DIR", 0, "Use DIR as persistent local cache" },

        // Standard options for input and output selection
        { "<manifestfile>", 0, 0, OPTION_DOC, "SFMF file to unpack" },
        { "<outputdir>", 0, 0, OPTION_DOC, "Output directory" },
        { "<localsrc> ...", 0, 0, OPTION_DOC, "Local directories for sourcing blobs (optional)" },
        { 0 }
    };

    struct argp argp = {
        options,
        parse_opt,
        "<manifestfile> <outputdir> [<localsrc> ...]",
        "\nManifest downloading, unpacking and verifying tool."
    };

    argp_parse(&argp, argc, argv, 0, 0, opts);
}

char *get_blob_data(struct UnpackOptions *opts, struct SFMF_FileEntry *entry, struct BlobResult *blob, size_t *size)
{
    // For now, we just assume included blobs (check for uncompressed outside)
    assert(blob->type == BLOB_RESULT_INCLUDED);

    // need to have size point to something
    assert(size);

    size_t offset = blob->included.entry->offset;
    *size = blob->included.entry->size;
    char *result = malloc(*size + 1);
    memset(result, 0, *size + 1);

    int res = fseek(opts->fp, offset, SEEK_SET);
    assert(res == 0);

    size_t read = fread(result, *size, 1, opts->fp);
    assert(read == 1);

    return result;
}

static char *make_pack_filename(struct SFMF_FileHash *hash)
{
    char tmp[128];

    int res = sfmf_filehash_format(hash, tmp, sizeof(tmp));
    assert(res);

    strncat(tmp, ".pack", sizeof(tmp));

    return strdup(tmp);
}

static char *make_blob_filename(struct SFMF_FileHash *hash)
{
    char tmp[128];

    int res = sfmf_filehash_format(hash, tmp, sizeof(tmp));
    assert(res);

    strncat(tmp, ".blob", sizeof(tmp));

    return strdup(tmp);
}

static int file_exists(const char *filename)
{
    struct stat st;
    return (stat(filename, &st) == 0);
}

static char *get_filename_in_source(struct UnpackOptions *opts, const char *filename)
{
    char tmp[PATH_MAX];

    char *d = strdup(opts->filename);
    char *source_dir = strdup(dirname(d));
    free(d);

    sprintf(tmp, "%s/%s", source_dir, filename);
    char *source_file = strdup(tmp);
    free(source_dir);

    return source_file;
}

static char *get_filename_in_cache(struct UnpackOptions *opts, const char *filename)
{
    char tmp[PATH_MAX];
    sprintf(tmp, "%s/%s", opts->cachedir, filename);
    return strdup(tmp);
}

static int filelist_contains_filename(struct FileEntry *entry, void *user_data)
{
    char *filename = user_data;
    return (strcmp(filename, entry->filename) == 0);
}

static char *download_payload_file(struct UnpackOptions *opts, const char *filename,
        struct SFMF_FileHash *expected_hash, int is_compressed)
{
    char *source_file = get_filename_in_source(opts, filename);
    char *dest_file = get_filename_in_cache(opts, filename);

    if (file_exists(dest_file) && expected_hash) {
        if (filelist_foreach(opts->cached_files, filelist_contains_filename, dest_file) != NULL) {
            // Already verified this file before
        } else if (sfmf_filehash_verify(expected_hash, dest_file, is_compressed) == 0) {
            // The file was already in the cache directory, and it verifies,
            // but it's not in opts->cached_files, so add it now
            filelist_append(opts->cached_files, dest_file, FILE_LIST_NONE);
        } else {
            SFMF_WARN("Deleting %s, as checksum does not match.\n", dest_file);
            unlink(dest_file);
        }
    }

    if (!file_exists(dest_file)) {
        if (opts->offline_mode) {
            SFMF_FAIL("Need to download %s, but offline mode requested.\n", source_file);
        }

        SFMF_LOG("Downloading: %s\n", source_file);

        // Remember this file, as we need to clean it up if interrupted
        opts->temporary_download = strdup(dest_file);

        if (strncmp(source_file, "http://", 7) == 0 || strncmp(source_file, "https://", 8) == 0) {
#if defined(USE_LIBCURL)
            FILE *fp = fopen(dest_file, "w");
            if (fp == NULL) {
                SFMF_FAIL("Failed to create '%s'\n", dest_file);
            }
            int res = convert_url_fp(source_file, fp, CONVERT_FLAG_NONE);
            assert(res == 0);
            fclose(fp);
#else
            pid_t pid = fork();
            if (pid == 0) {
                char * const args[] = { "curl", "-o", dest_file, source_file, NULL };
                if (execvp("curl", args) == -1) {
                    SFMF_FAIL("Could not execute curl: %s\n", strerror(errno));
                }
            }

            int res = 0;
            if (waitpid(pid, &res, 0) != pid) {
                SFMF_FAIL("Could not wait for curl exit status: %s\n", strerror(errno));
            }
            if (WIFEXITED(res) && WEXITSTATUS(res) != 0) {
                SFMF_FAIL("curl exited with non-zero exit status: %d\n", res);
            }
#endif
        } else {
            // Looks like a local file - just copy it over
            int res = convert_file(source_file, dest_file, CONVERT_FLAG_NONE);
            assert(res == 0);
        }

        if (expected_hash) {
            if (sfmf_filehash_verify(expected_hash, dest_file, is_compressed) == 0) {
                filelist_append(opts->cached_files, dest_file, FILE_LIST_NONE);
            } else {
                // TODO: Retry download?
                SFMF_WARN("Deleting %s as hash does not match (corrupt file?).\n", dest_file);
                unlink(dest_file);
                // return NULL below, because we don't have the file
                FREE_VAR(dest_file);
            }
        } else {
            SFMF_WARN("Unchecked file: %s (no expected_hash available)\n", dest_file);
            filelist_append(opts->cached_files, dest_file, FILE_LIST_NONE);
        }

        FREE_VAR(opts->temporary_download);
    }

    free(source_file);

    return dest_file;
}

static int write_file_from_pack(FILE *fp, const char *filename, struct SFMF_FileHash *hash)
{
    size_t size = 0;
    enum SFMF_BlobEntry_Flag flags = 0;

    char *data = get_blob_from_pack(filename, hash, &size, &flags);

    if (data) {
        enum ConvertFlags cflags = CONVERT_FLAG_NONE;
        if ((flags & BLOB_FLAG_ZCOMPRESSED) != 0) {
            cflags = CONVERT_FLAG_ZUNCOMPRESS;
        }

        int res = convert_buffer_fp(data, size, fp, cflags);
        assert(res == 0);

        free(data);
        return 1;
    }

    return 0;
}

void write_blob_data(struct UnpackOptions *opts, struct SFMF_FileEntry *entry, struct BlobResult *blob, const char *filename)
{
    FILE *fp = fopen(filename, "wb");
    if (fp == NULL) {
        SFMF_FAIL("Failed to create '%s'\n", filename);
    }

    switch (blob->type) {
        case BLOB_RESULT_INCLUDED:
            {
                // Blob is included in the manifest, can seek and write data directly,
                // might need to uncompress data using the converter functions
                size_t size = 0;
                char *data = get_blob_data(opts, entry, blob, &size);

                enum ConvertFlags flags = CONVERT_FLAG_NONE;
                if ((blob->included.entry->flags & BLOB_FLAG_ZCOMPRESSED) != 0) {
                    flags = CONVERT_FLAG_ZUNCOMPRESS;
                }

                int res = convert_buffer_fp(data, size, fp, flags);
                assert(res == 0);

                free(data);
            }
            break;
        case BLOB_RESULT_LOCAL:
            {
                SFMF_DEBUG("Copying: %s -> %s\n", blob->local.entry->filename, filename);
                FILE *in = fopen(blob->local.entry->filename, "rb");
                assert(in != NULL);
                int res = convert_file_fp(in, fp, CONVERT_FLAG_NONE);
                assert(res == 0);
                fclose(in);
            }
            break;
        case BLOB_RESULT_PACKED:
            {
                // Determine pack filename
                char *pack_filename = make_pack_filename(&(blob->packed.entry->hash));
                assert(pack_filename);

                // File must have been downloaded already
                char *pack_local_filename = get_filename_in_cache(opts, pack_filename);
                assert(pack_local_filename && file_exists(pack_local_filename));

                // Use pack functions to extract blob from pack
                int res = write_file_from_pack(fp, pack_local_filename, &(entry->hash));
                assert(res);

                free(pack_local_filename);
                free(pack_filename);
            }
            break;
        case BLOB_RESULT_FULL:
            {
                // Determine blob filename
                char *blob_filename = make_blob_filename(&(entry->hash));
                assert(blob_filename);

                // Download blob file (if not exists)
                char *blob_local_filename = get_filename_in_cache(opts, blob_filename);
                assert(blob_local_filename && file_exists(blob_local_filename));

                // Use convert functions to cross-write blob from file
                FILE *in = fopen(blob_local_filename, "rb");
                enum ConvertFlags flags = CONVERT_FLAG_NONE;
                if (entry->zsize < entry->hash.size) {
                    // Compressed size is not equal to uncompressed size, so decompress
                    flags = CONVERT_FLAG_ZUNCOMPRESS;
                }
                int res = convert_file_fp(in, fp, flags);
                assert(res == 0);

                fclose(in);

                free(blob_local_filename);
                free(blob_filename);
            }
            break;
        case BLOB_RESULT_EMPTY:
            // all good, we need to write an empty file
            break;
        default:
            assert(0);
            break;
    }

    fclose(fp);

    if (blob->type != BLOB_RESULT_EMPTY) {
        // Verify if the written blob matches the expected hash in the manifest

        struct SFMF_FileHash hash;
        memset(&hash, 0, sizeof(hash));
        int res = convert_file_zsize_hash(filename, &hash, NULL);
        assert(res == 0);

        if (sfmf_filehash_compare(&hash, &(entry->hash)) != 0) {
            char tmp[100];
            int res = sfmf_filehash_format(&hash, tmp, sizeof(tmp));
            assert(res);

            SFMF_FAIL("File failed hash check: %s, got: %s\n",
                    filename, tmp);
        } else {
            //SFMF_LOG("File passed hash check: %s\n", filename);
        }
    }
}

struct FileListSearchContext {
    struct UnpackOptions *opts;
    struct SFMF_FileHash *hash;
    struct BlobResult *result;
};

static int filelist_download_summary(struct FileEntry *entry, void *user_data)
{
    size_t *total = user_data;

    int32_t size = entry->hash.size;
    SFMF_LOG(" %10d KiB  %s\n", size / 1024, entry->filename);

    *total += size;

    return 0;
}

static int filelist_remove_file(struct FileEntry *entry, void *user_data)
{
    int res = remove(entry->filename);
    if (res != 0) {
        SFMF_WARN("Cannot remove cached file %s: %s\n",
                  entry->filename, strerror(errno));
    }

    return 0;
}

static int filelist_search_blob_hash(struct FileEntry *entry, void *user_data)
{
    struct FileListSearchContext *ctx = user_data;

    if (entry->hash.size != ctx->hash->size) {
        // Can skip entries that don't have a matching file size
        return 0;
    }

    if (entry->hash.hashtype == HASHTYPE_LAZY) {
        // Lazily calculate hash if size matches
        SFMF_DEBUG("Lazily calculating file hash: %s\n", entry->filename);
        fileentry_calculate_zsize_hash(entry);
    }

    // At this point, we must have a sha1 hash of the file (or we skipped it)
    assert(entry->hash.hashtype == HASHTYPE_SHA1);

    if (sfmf_filehash_compare(ctx->hash, &(entry->hash)) == 0) {
        //SFMF_DEBUG("Found matching hash: %s\n", entry->filename);
        return 1;
    } else {
        //SFMF_DEBUG("No match: %s\n", entry->filename);
    }

    return 0;
}

static void search_blob_hash(struct UnpackOptions *opts, struct SFMF_FileHash *hash,
        struct BlobResult *result)
{
    result->type = BLOB_RESULT_INVALID;

    // 1. Search in included blobs
    for (int i=0; i<opts->header.blobs_length; i++) {
        if (sfmf_filehash_compare(hash, &(opts->bentries[i].hash)) == 0) {
            result->type = BLOB_RESULT_INCLUDED;
            result->included.entry = &(opts->bentries[i]);
            return;
        }
    }

    // 2. Search in local files
    struct FileListSearchContext search_ctx = {
        opts,
        hash,
        result,
    };

    // TODO: Keep list of (device, inode) pairs that have already been visited
    // to avoid hashing all hardlinks (e.g. happened with 1.1.1.27 on top of 1.1.3.91)

    struct FileEntry *entry = filelist_foreach(opts->local_files, filelist_search_blob_hash, &search_ctx);

    if (entry) {
        result->type = BLOB_RESULT_LOCAL;
        result->local.entry = entry;
        return;
    }

    // 3. Search in packed files
    for (int i=0; i<opts->header.packs_length; i++) {
        struct SFMF_PackEntry *entry = &(opts->pentries[i]);

        for (int j=0; j<entry->count; j++) {
            struct SFMF_FileHash *fhash = &(opts->pack_hashes[i][j]);

            if (sfmf_filehash_compare(hash, fhash) == 0) {
                result->type = BLOB_RESULT_PACKED;
                result->packed.entry = entry;
                return;
            }
        }
    }

    // 4. Search in / fallback to full blob downloads
    result->type = BLOB_RESULT_FULL;
}

static void unpack_dirstack_entry_pop(struct DirStackEntry *entry)
{
    struct SFMF_FileEntry *user_data = entry->user_data;

    //SFMF_DEBUG("Setting mtime of directory %s to %ld\n",
    //        entry->path, user_data->mtime);

    // Set timestamp
    struct timeval tv[2];
    tv[0].tv_sec = tv[1].tv_sec = user_data->mtime;
    tv[0].tv_usec = tv[1].tv_usec = 0;
    int res = lutimes(entry->path, tv);
    if (res != 0) {
        SFMF_FAIL("Failed to set mtime of '%s' to %ld: %s\n",
                entry->path, user_data->mtime, strerror(errno));
    }
}

static void unpack_classify_entry(struct UnpackOptions *opts, struct UnpackFileEntry *e)
{
    const char *filename = opts->filename_table + e->entry.filename_offset;
    e->target_filename = malloc(strlen(opts->outputdir) + strlen(filename) + 1);
    sprintf(e->target_filename, "%s%s", opts->outputdir, filename);

    char filetype = '?';
    switch (e->entry.type) {
        case ENTRY_DIRECTORY: filetype = 'd'; break;
        case ENTRY_FILE: filetype = 'f'; break;
        case ENTRY_SYMLINK: filetype = 's'; break;
        case ENTRY_CHARACTER: filetype = 'c'; break;
        case ENTRY_FIFO: filetype = 'p'; break;
        case ENTRY_HARDLINK: filetype = 'h'; break;
        case ENTRY_BLOCK: filetype = 'b'; break;
        default: filetype = '!'; break;
    }

    char tmp[512];
    if (e->entry.hash.size > 0) {
        sfmf_filehash_format(&(e->entry.hash), tmp, sizeof(tmp));
    } else {
        strcpy(tmp, "----------------------------------------");
    }

    const char *info = "";

    if (e->entry.type == ENTRY_HARDLINK) {
        info = "HARDLINK";
        e->blob_result.type = BLOB_RESULT_HARDLINK;
    } else if (e->entry.hash.size > 0) {
        search_blob_hash(opts, &(e->entry.hash), &(e->blob_result));
        switch (e->blob_result.type) {
            case BLOB_RESULT_INCLUDED:
                info = "INCLUDED";
                break;
            case BLOB_RESULT_LOCAL:
                info = "FILECOPY";
                break;
            case BLOB_RESULT_PACKED:
                info = "DOWNPACK";
                break;
            case BLOB_RESULT_FULL:
                info = "DOWNBLOB";
                break;
            default:
                assert(0);
                break;
        }
    } else {
        info = "ZEROBYTE";
        e->blob_result.type = BLOB_RESULT_EMPTY;
    }

    // Shorten blob display
    tmp[10] = '\0';

    SFMF_DEBUG("[%c] %06o %6d:%6d (%s, %s) (%9d b, %9d z) %s\n",
               filetype, e->entry.mode, e->entry.uid, e->entry.gid, tmp,
               info, e->entry.hash.size, e->entry.zsize, e->target_filename);
}

static void unpack_download_requirements(struct UnpackOptions *opts, struct UnpackFileEntry *e)
{
    if (e->entry.type == ENTRY_FILE) {
        switch (e->blob_result.type) {
            case BLOB_RESULT_PACKED:
                {
                    struct SFMF_FileHash *expected_hash = &(e->blob_result.packed.entry->hash);
                    char *pack_filename = make_pack_filename(expected_hash);
                    assert(pack_filename);

                    char *pack_local_filename = download_payload_file(opts, pack_filename, expected_hash, 0);
                    assert(pack_local_filename);

                    free(pack_local_filename);
                    free(pack_filename);
                }
                break;
            case BLOB_RESULT_FULL:
                {
                    struct SFMF_FileHash *expected_hash = &(e->entry.hash);
                    char *blob_filename = make_blob_filename(expected_hash);
                    assert(blob_filename);

                    int is_compressed = (e->entry.zsize < e->entry.hash.size);
                    char *blob_local_filename = download_payload_file(opts, blob_filename, expected_hash, is_compressed);
                    assert(blob_local_filename);

                    free(blob_local_filename);
                    free(blob_filename);
                }
                break;
            default:
                // Nothing to download
                break;
        }
    }
}

static void unpack_write_entry(struct UnpackOptions *opts, struct UnpackFileEntry *e)
{
    int res = 0;

    switch (e->entry.type) {
        case ENTRY_DIRECTORY:
            res = mkdir(e->target_filename, 0755);
            if (res != 0 && !(strcmp(opts->filename_table + e->entry.filename_offset, "/") == 0 && errno == EEXIST)) {
                SFMF_FAIL("Failed to create '%s': %s\n", e->target_filename, strerror(errno));
            }
            break;
        case ENTRY_FILE:
            write_blob_data(opts, &(e->entry), &(e->blob_result), e->target_filename);
            break;
        case ENTRY_SYMLINK:
            {
                // For symlinks, we assume the packing tool has included all symlink contents
                // into the file, and that the contents are uncompressed, so we can read them
                // directly (it usually doesn't make sense to compress symlink data too much)
                assert(e->blob_result.type == BLOB_RESULT_INCLUDED);
                assert((e->blob_result.included.entry->flags & BLOB_FLAG_ZCOMPRESSED) == 0);

                size_t size = 0;
                char *symlink_target = get_blob_data(opts, &(e->entry), &(e->blob_result), &size);
                assert(symlink_target != NULL);
                //SFMF_LOG("Symlink: '%s' -> '%s'\n", fn, symlink_target);
                res = symlink(symlink_target, e->target_filename);
                free(symlink_target);
                if (res != 0) {
                    SFMF_FAIL("Failed to create '%s': %s\n", e->target_filename, strerror(errno));
                }
            }
            break;
        case ENTRY_CHARACTER:
        case ENTRY_BLOCK:
            res = mknod(e->target_filename, e->entry.mode, e->entry.dev);
            if (res != 0) {
                SFMF_FAIL("Failed to create '%s': %s\n", e->target_filename, strerror(errno));
            }
            break;
        case ENTRY_FIFO:
            res = mkfifo(e->target_filename, 0644);
            if (res != 0) {
                SFMF_FAIL("Failed to create '%s': %s\n", e->target_filename, strerror(errno));
            }
            break;
        case ENTRY_HARDLINK:
            {
                assert(e->blob_result.type == BLOB_RESULT_HARDLINK);
                //assert(e->entry.dev >= 0 && entry->dev < opts->header.entries_length && entry->dev < i);
                assert(e->entry.dev >= 0 && e->entry.dev < opts->header.entries_length); // FIXME: entry->dev < i)

                struct SFMF_FileEntry *hentry = &(opts->fentries[e->entry.dev].entry);
                const char *hfilename = opts->filename_table + hentry->filename_offset;
                char *hfn = malloc(strlen(opts->outputdir) + strlen(hfilename) + 1);
                sprintf(hfn, "%s%s", opts->outputdir, hfilename);
                int res = link(hfn, e->target_filename);
                if (res != 0) {
                    SFMF_FAIL("Failed to create '%s' (from '%s'): %s\n", e->target_filename,
                            hfn, strerror(errno));
                }
                free(hfn);
            }
            break;
        default:
            assert(0);
            break;
    }
}

static void unpack_set_permissions(struct UnpackOptions *opts, struct UnpackFileEntry *e)
{
    // Set numeric owner/group, also for symlinks (set the link, not the
    // pointed-to filesystem entry instead)
    int res = lchown(e->target_filename, e->entry.uid, e->entry.gid);
    if (res != 0) {
        SFMF_FAIL("Could not change owner/group of '%s' to %d/%d: %s\n",
                e->target_filename, e->entry.uid, e->entry.gid, strerror(errno));
    }

    // Important: Need to set the permissions after setting owner/group, as
    // otherwise suid/sgid is dropped if the owner of the file is changed.

    // Set permissions, but only if it's not a symlink, as permissions on
    // symlinks are not really used (http://superuser.com/a/303063/228762)
    if (e->entry.type != ENTRY_SYMLINK) {
        res = chmod(e->target_filename, e->entry.mode);
        if (res != 0) {
            SFMF_FAIL("Could not change permission of '%s' to %o: %s\n",
                    e->target_filename, e->entry.mode, strerror(errno));
        }
    }

    if(e->entry.type == ENTRY_DIRECTORY) {
        // Timestamps of directories should be set only after all its
        // children have been written (otherwise the timestamp would be
        // updated when a new child is written), so we push it on a stack
        // and the stack takes care of calling our pop function (given to
        // dirstack_new(), in our case unpack_dirstack_entry_pop) which
        // will then update the timestamp of the directory.
        dirstack_push(opts->dir_stack, e->target_filename, &(e->entry));
    } else {
        // Can set timestamp immediately, as this is not a directory
        struct timeval tv[2];
        tv[0].tv_sec = tv[1].tv_sec = e->entry.mtime;
        tv[0].tv_usec = tv[1].tv_usec = 0;
        res = lutimes(e->target_filename, tv);
        if (res != 0) {
            SFMF_FAIL("Failed to set mtime of '%s' to %ld: %s\n",
                    e->target_filename, e->entry.mtime, strerror(errno));
        }
    }
}


void unpack_cleanup(void *user_data)
{
    struct UnpackOptions *opts = user_data;

    if (opts->dir_stack) {
        dirstack_free(opts->dir_stack);
        opts->dir_stack = 0;
    }

    if (!opts->success) {
        // TODO: Also cleanup partially unpacked files, as we didn't arrive at
        // the end (although probably the caller does this for us, too)
    }

    if (opts->temporary_download) {
        int res = remove(opts->temporary_download);
        if (res != 0) {
            SFMF_WARN("Cannot remove temporary download %s: %s\n",
                      opts->temporary_download, strerror(errno));
        }
        FREE_VAR(opts->temporary_download);
    }

    if (!opts->keep_cached_files) {
        if (opts->cached_files) {
            (void)filelist_foreach(opts->cached_files, filelist_remove_file, NULL);
            filelist_free(opts->cached_files);
            opts->cached_files = 0;
        }

        if (opts->cachedir) {
            rmdir(opts->cachedir);
            FREE_VAR(opts->cachedir);
        }
    }

    if (opts->pack_hashes) {
        for (int i=0; i<opts->header.packs_length; i++) {
            FREE_VAR(opts->pack_hashes[i]);
        }

        FREE_VAR(opts->pack_hashes);
    }

    if (opts->fentries) {
        for (int i=0; i<opts->header.entries_length; i++) {
            FREE_VAR(opts->fentries[i].target_filename);
        }
    }

    FREE_VAR(opts->bentries);
    FREE_VAR(opts->pentries);
    FREE_VAR(opts->metadata);
    FREE_VAR(opts->fentries);
    FREE_VAR(opts->filename_table);

    if (opts->fp) {
        fclose(opts->fp);
        opts->fp = 0;
    }

    FREE_VAR(opts->manifest_local_filename);

    FREE_VAR(opts->filename);
    FREE_VAR(opts->outputdir);
    for (int i=0; i<opts->n_sourcedirs; i++) {
        FREE_VAR(opts->sourcedirs[i]);
    }

    sfmf_control_close();
}

static void draw_progress(struct UnpackOptions *opts, int i, const char *message)
{
    static float last_progress = -1.f;

    float partial = (opts->header.entries_length && i >= 0)
        ? fminf(1.f, (float)(i) / (float)(opts->header.entries_length))
        : (i == -1 ? 0.f : 1.f);

    float progress = fminf(1.f, ((float)(opts->steps.current) + partial) / (float)(opts->steps.total));

    if (i >= 0 && progress - last_progress < 0.005f) {
        // Avoid excessive status updates
        return;
    }

    if (opts->progress) {
        if (i == -1) {
            SFMF_LOG("%c[K%.1f%% %s\n", 27, 100.f * progress, message);
        } else {
            SFMF_LOG("%c[K%.1f%% %s \r", 27, 100.f * progress, message);
        }
    }

    sfmf_control_set_progress(getenv("SFMF_TARGET") ?: "-",
        100 * progress,
        (i == -1) ? message : NULL);

    last_progress = progress;
}

static void next_step(struct UnpackOptions *opts, const char *message)
{
    opts->steps.current++;
    draw_progress(opts, -1, message);
}

static void foreach_unpack_entry(struct UnpackOptions *opts, void (*f)(struct UnpackOptions *opts, struct UnpackFileEntry *e))
{
    for (int i=0; i<opts->header.entries_length; i++) {
        struct UnpackFileEntry *e = &(opts->fentries[i]);

        if (opts->abort) {
            SFMF_FAIL("Operation aborted via D-Bus\n");
        }

        const char *filename = opts->filename_table + e->entry.filename_offset;
        draw_progress(opts, i, filename);

        f(opts, e);

        sfmf_control_process();
    }

    draw_progress(opts, -2, "DONE");
}

static int control_abort_cb(void *user_data)
{
    struct UnpackOptions *opts = user_data;
    opts->abort = 1;
    return 1;
}

static struct SFMF_Control_Callbacks control_callbacks = {
    control_abort_cb,
};

int main(int argc, char *argv[])
{
    struct UnpackOptions *opts = calloc(1, sizeof(struct UnpackOptions));
    progname = argv[0];

    sfmf_cleanup_register(unpack_cleanup, opts);

    // So we don't hog the CPU
    nice(5);

    parse_opts(argc, argv, opts);

    opts->steps.current = -1;
    opts->steps.total = 8;

    sfmf_control_init(&control_callbacks, opts);

    if (opts->offline_mode) {
        opts->steps.total -= 1;
    }

    if (opts->download_only) {
        opts->steps.total -= 2;
    }

    sfmf_policy_set_log_debug(opts->verbose);

    // Initialize local file cache
    if (!opts->cachedir) {
        opts->cachedir = strdup("sfmf-cache-XXXXXX");
        opts->cachedir = mkdtemp(opts->cachedir);
    }
    assert(opts->cachedir != NULL);
    opts->cached_files = filelist_new();

    next_step(opts, "Downloading manifest file");

    // TODO: Have an expected hash for the manifest file
    opts->manifest_local_filename = download_payload_file(opts, "manifest.sfmf", NULL, 0);
    assert(opts->manifest_local_filename);

    // TODO: We could also have a known file hash for the manifest file, so
    // that the download of the manifest file could also be verified.

    opts->fp = fopen(opts->manifest_local_filename, "rb");
    assert(opts->fp);
    int res = sfmf_fileheader_read(&(opts->header), opts->fp);
    assert(res == 1);

    assert(opts->header.magic == SFMF_MAGIC_NUMBER);
    assert(opts->header.version == SFMF_CURRENT_VERSION);

    next_step(opts, "Indexing local files");

    // Index all local files
    SFMF_LOG("==== Local Files ====\n");
    opts->local_files = filelist_new();
    sfmf_policy_set_ignore_unsupported(1);
    for (int i=0; i<opts->n_sourcedirs; i++) {
        opts->local_files = extend_file_list(opts->local_files, opts->sourcedirs[i], FILE_LIST_NONE);
    }
    SFMF_LOG("Got local files: %d\n", opts->local_files->length);
    sfmf_policy_set_ignore_unsupported(0);
    SFMF_LOG("==== Local Files ====\n");

    SFMF_LOG("File header:\n"
             " Magic: %x (%c%c%c%c)\n"
             " Version: %d\n"
             " Metadata size: %d bytes\n"
             " Filename table size: %d bytes\n"
             " Entries: %d\n"
             " Packs: %d\n"
             " Blobs: %d\n\n",
             opts->header.magic,
             (opts->header.magic >> 24) & 0xFF,
             (opts->header.magic >> 16) & 0xFF,
             (opts->header.magic >> 8) & 0xFF,
             (opts->header.magic >> 0) & 0xff,
             opts->header.version,
             opts->header.metadata_size,
             opts->header.filename_table_size,
             opts->header.entries_length,
             opts->header.packs_length,
             opts->header.blobs_length);

    opts->metadata = malloc(opts->header.metadata_size);
    res = fread(opts->metadata, opts->header.metadata_size, 1, opts->fp);
    assert(res == 1);

    SFMF_LOG("==== Metadata ====\n");
    SFMF_LOG("%s\n", opts->metadata);
    SFMF_LOG("==== Metadata ====\n");

    next_step(opts, "Parsing manifest file");

    opts->filename_table = malloc(opts->header.filename_table_size);
    res = fread(opts->filename_table, opts->header.filename_table_size, 1, opts->fp);
    assert(res == 1);

    opts->fentries = calloc(sizeof(struct UnpackFileEntry), opts->header.entries_length);

    for (int i=0; i<opts->header.entries_length; i++) {
        sfmf_fileentry_read(&(opts->fentries[i].entry), opts->fp);
    }

    opts->pentries = calloc(sizeof(struct SFMF_PackEntry), opts->header.packs_length);

    for (int i=0; i<opts->header.packs_length; i++) {
        sfmf_packentry_read(&(opts->pentries[i]), opts->fp);
    }

    opts->bentries = calloc(sizeof(struct SFMF_BlobEntry), opts->header.blobs_length);

    for (int i=0; i<opts->header.blobs_length; i++) {
        sfmf_blobentry_read(&(opts->bentries[i]), opts->fp);
    }

    opts->pack_hashes = calloc(sizeof(struct SFMF_FileHash *), opts->header.packs_length);

    for (int i=0; i<opts->header.packs_length; i++) {
        struct SFMF_PackEntry *entry = &(opts->pentries[i]);

        opts->pack_hashes[i] = calloc(sizeof(struct SFMF_FileHash), entry->count);

        fseek(opts->fp, entry->offset, SEEK_SET);
        for (int j=0; j<entry->count; j++) {
            int res = sfmf_filehash_read(&(opts->pack_hashes[i][j]), opts->fp);
            assert(res == 1);
        }
    }

    next_step(opts, "Classifying entries");
    foreach_unpack_entry(opts, unpack_classify_entry);

    if (!opts->offline_mode) {
        next_step(opts, "Downloading requirements");
        foreach_unpack_entry(opts, unpack_download_requirements);
    }

    if (!opts->download_only) {
        next_step(opts, "Writing files");
        foreach_unpack_entry(opts, unpack_write_entry);

        next_step(opts, "Setting permissions");
        {
            // Dir stack for setting the right mtimes on directories
            opts->dir_stack = dirstack_new(unpack_dirstack_entry_pop);

            foreach_unpack_entry(opts, unpack_set_permissions);

            // Write outstanding (queued) directory permissions
            dirstack_free(opts->dir_stack);
            opts->dir_stack = 0;
        }
    }

    next_step(opts, "Verifying entries");

    // TODO: Verify entries

    // Always send the 100% signal
    sfmf_control_set_progress(getenv("SFMF_TARGET") ?: "-", 100, "FINISHED");

    SFMF_LOG("==== Download Summary ====\n");
    size_t total = 0;
    (void)filelist_foreach(opts->cached_files, filelist_download_summary, &total);
    SFMF_LOG("==== Download Summary ====\n");
    SFMF_LOG("TOTAL DOWNLOAD: %ld KiB\n", total / 1024);
    SFMF_LOG("==== Download Summary ====\n");

    // If we arrive here, we have successfully unpacked everything
    opts->success = 1;
    return 0;
}
