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
    SFMF_LOG("Usage: %s <packfile>\n\n"
             "    <packfile> ..... Name of SFPF file to dump\n"
             "\n", progname);
}

struct DumpPackOptions {
    const char *filename;
};

static int parse_opts(int argc, char *argv[], struct DumpPackOptions *opts)
{
    int expected_argc = 2;

    if (argc != expected_argc) {
        SFMF_LOG("Invalid number of arguments: %d (expected %d)\n", argc, expected_argc);
        return 0;
    }

    opts->filename = argv[1];

    return 1;
}


int main(int argc, char *argv[])
{
    struct DumpPackOptions opts;

    memset(&opts, 0, sizeof(opts));
    progname = argv[0];

    if (!parse_opts(argc, argv, &opts)) {
        usage();
        return 1;
    }

    struct SFPF_FileHeader header;

    FILE *fp = fopen(opts.filename, "rb");
    assert(fp);
    int res = sfpf_fileheader_read(&header, fp);
    assert(res == 1);

    assert(header.magic == SFPF_MAGIC_NUMBER);
    assert(header.version == SFPF_CURRENT_VERSION);

    SFMF_LOG("File header:\n"
           " Magic: %x (%c%c%c%c)\n"
           " Version: %d\n"
           " Metadata size: %d bytes\n"
           " Packed items: %d\n\n",
           header.magic,
           (header.magic >> 24) & 0xFF,
           (header.magic >> 16) & 0xFF,
           (header.magic >> 8) & 0xFF,
           (header.magic >> 0) & 0xff,
           header.version,
           header.metadata_size,
           header.blobs_length);

    char *metadata = malloc(header.metadata_size);
    res = fread(metadata, header.metadata_size, 1, fp);
    assert(res == 1);
    SFMF_LOG("==== Metadata ====\n");
    SFMF_LOG("%s\n", metadata);
    SFMF_LOG("==== Metadata ====\n");
    free(metadata);

    struct SFMF_BlobEntry *entries;
    entries = calloc(sizeof(struct SFMF_BlobEntry), header.blobs_length);

    for (int i=0; i<header.blobs_length; i++) {
        sfmf_blobentry_read(&(entries[i]), fp);
    }

    for (int i=0; i<header.blobs_length; i++) {
        SFMF_LOG(" == Item %d ==\n", i);
        char tmp[512];
        sfmf_filehash_format(&(entries[i].hash), tmp, sizeof(tmp));
        SFMF_LOG("  Hash: %s\n", tmp);
        if (entries[i].flags & BLOB_FLAG_ZCOMPRESSED) {
            SFMF_LOG("  Flags: zcompressed\n");
        } else {
            SFMF_LOG("  Flags: -\n");
        }
        SFMF_LOG("  Offset: %d\n", entries[i].offset);
        SFMF_LOG("  Size: %d (%d uncompressed)\n", entries[i].size,
                entries[i].hash.size);
    }

    free(entries);
    fclose(fp);

    return 0;
}
