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

const char *progname = NULL;

static void usage()
{
    SFMF_LOG("Usage: %s <manifestfile>\n\n"
             "    <manifestfile> . Name of SFMF file to dump\n"
             "\n", progname);
}

struct DumpManifestOptions {
    const char *filename;
};

static int parse_opts(int argc, char *argv[], struct DumpManifestOptions *opts)
{
    int expected_argc = 2;

    if (argc != expected_argc) {
        SFMF_LOG("Invalid number of arguments: %d (expected %d)\n",
                argc, expected_argc);
        return 0;
    }

    opts->filename = argv[1];

    return 1;
}


int main(int argc, char *argv[])
{
    struct DumpManifestOptions opts;

    memset(&opts, 0, sizeof(opts));
    progname = argv[0];

    if (!parse_opts(argc, argv, &opts)) {
        usage();
        return 1;
    }

    struct SFMF_FileHeader header;

    FILE *fp = fopen(opts.filename, "rb");
    assert(fp);
    int res = sfmf_fileheader_read(&header, fp);
    assert(res == 1);

    assert(header.magic == SFMF_MAGIC_NUMBER);
    assert(header.version == SFMF_CURRENT_VERSION);

    SFMF_LOG("File header:\n"
           " Magic: %x (%c%c%c%c)\n"
           " Version: %d\n"
           " Metadata size: %d bytes\n"
           " Filename table size: %d bytes\n"
           " Entries: %d\n"
           " Packs: %d\n"
           " Blobs: %d\n\n",
           header.magic,
           (header.magic >> 24) & 0xFF,
           (header.magic >> 16) & 0xFF,
           (header.magic >> 8) & 0xFF,
           (header.magic >> 0) & 0xff,
           header.version,
           header.metadata_size,
           header.filename_table_size,
           header.entries_length,
           header.packs_length,
           header.blobs_length);


    char *metadata = malloc(header.metadata_size);
    res = fread(metadata, header.metadata_size, 1, fp);
    assert(res == 1);

    SFMF_LOG("==== Metadata ====\n");
    SFMF_LOG("%s\n", metadata);
    SFMF_LOG("==== Metadata ====\n");
    free(metadata);

    char *filename_table = malloc(header.filename_table_size);
    res = fread(filename_table, header.filename_table_size, 1, fp);
    assert(res == 1);

#if 0
    SFMF_LOG("==== Filename table ====\n");
    for (int i=0; i<header.filename_table_size; i++) {
        SFMF_LOG("%s\n", filename_table + i);
        i += strlen(filename_table + i) + 1;
    }
    SFMF_LOG("==== Filename table ====\n");
#endif

    struct SFMF_FileEntry *fentries;
    fentries = calloc(sizeof(struct SFMF_FileEntry), header.entries_length);

    for (int i=0; i<header.entries_length; i++) {
        sfmf_fileentry_read(&(fentries[i]), fp);
    }

    SFMF_LOG("==== Entries ====\n");
    for (int i=0; i<header.entries_length; i++) {
        struct SFMF_FileEntry *entry = &(fentries[i]);
        char filetype = '?';
        switch (entry->type) {
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
        if (entry->hash.size > 0) {
            sfmf_filehash_format(&(entry->hash), tmp, sizeof(tmp));
        } else {
            strcpy(tmp, "-");
        }

        SFMF_LOG("[%c] %06o %5d:%5d (%s) %s (%d bytes / %d zbytes)\n",
                filetype, entry->mode, entry->uid, entry->gid,
                tmp, filename_table + entry->filename_offset,
                entry->hash.size, entry->zsize);
    }
    SFMF_LOG("==== Entries ====\n");

    free(fentries);

    struct SFMF_PackEntry *pentries;

    pentries = calloc(sizeof(struct SFMF_PackEntry), header.packs_length);

    for (int i=0; i<header.packs_length; i++) {
        sfmf_packentry_read(&(pentries[i]), fp);
    }

    SFMF_LOG("==== Pack entries ====\n");
    for (int i=0; i<header.packs_length; i++) {
        struct SFMF_PackEntry *entry = &(pentries[i]);

        char tmp[512];
        sfmf_filehash_format(&(entry->hash), tmp, sizeof(tmp));
        SFMF_LOG("Pack %d (%s), %d bytes: %d entries @ offset %d\n",
                i, tmp, entry->hash.size, entry->count, entry->offset);
    }
    SFMF_LOG("==== Pack entries ====\n");

    struct SFMF_BlobEntry *bentries;
    bentries = calloc(sizeof(struct SFMF_BlobEntry), header.blobs_length);

    for (int i=0; i<header.blobs_length; i++) {
        sfmf_blobentry_read(&(bentries[i]), fp);
    }

    for (int i=0; i<header.blobs_length; i++) {
        SFMF_LOG(" == Item %d ==\n", i);
        char tmp[512];
        sfmf_filehash_format(&(bentries[i].hash), tmp, sizeof(tmp));
        SFMF_LOG("  Hash: %s\n", tmp);
        if (bentries[i].flags & BLOB_FLAG_ZCOMPRESSED) {
            SFMF_LOG("  Flags: zcompressed\n");
        } else {
            SFMF_LOG("  Flags: -\n");
        }
        SFMF_LOG("  Offset: %d\n", bentries[i].offset);
        SFMF_LOG("  Size: %d (%d uncompressed)\n", bentries[i].size,
                bentries[i].hash.size);
    }

    SFMF_LOG("==== Pack Contents ====\n");
    for (int i=0; i<header.packs_length; i++) {
        struct SFMF_PackEntry *entry = &(pentries[i]);

        char tmp[512];
        sfmf_filehash_format(&(entry->hash), tmp, sizeof(tmp));
        SFMF_LOG("Pack %d (%s):\n", i, tmp);
        fseek(fp, entry->offset, SEEK_SET);

        for (int j=0; j<entry->count; j++) {
            struct SFMF_FileHash hash;
            res = sfmf_filehash_read(&hash, fp);
            assert(res == 1);

            sfmf_filehash_format(&hash, tmp, sizeof(tmp));
            SFMF_LOG("  #%4d: %s (%d bytes)\n", j, tmp, hash.size);
        }
    }
    SFMF_LOG("==== Pack Contents ====\n");


    free(pentries);
    free(bentries);

    free(filename_table);
    fclose(fp);

    return 0;
}
