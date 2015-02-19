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

#include "readpack.h"
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

char *get_blob_from_pack(const char *filename, struct SFMF_FileHash *hash, size_t *size, enum SFMF_BlobEntry_Flag *flags)
{
    char *result = NULL;

    struct SFPF_FileHeader header;

    FILE *fp = fopen(filename, "rb");
    assert(fp);
    int res = sfpf_fileheader_read(&header, fp);
    assert(res == 1);

    assert(header.magic == SFPF_MAGIC_NUMBER);
    assert(header.version == SFPF_CURRENT_VERSION);

#if 0
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
#endif

    char *metadata = malloc(header.metadata_size);
    res = fread(metadata, header.metadata_size, 1, fp);
    assert(res == 1);

#if 0
    SFMF_LOG("==== Metadata ====\n");
    SFMF_LOG("%s\n", metadata);
    SFMF_LOG("==== Metadata ====\n");
#endif
    free(metadata);

    struct SFMF_BlobEntry *entries;
    entries = calloc(sizeof(struct SFMF_BlobEntry), header.blobs_length);

    for (int i=0; i<header.blobs_length; i++) {
        sfmf_blobentry_read(&(entries[i]), fp);
    }

    for (int i=0; i<header.blobs_length; i++) {
        if (sfmf_filehash_compare(&(entries[i].hash), hash) == 0) {
            // Found match - read data into memory
            result = malloc(entries[i].size);
            res = fseek(fp, entries[i].offset, SEEK_SET);
            assert(res == 0);
            res = fread(result, entries[i].size, 1, fp);
            assert(res == 1);

            // These are only set when result is non-NULL
            *size = entries[i].size;
            *flags = entries[i].flags;
            break;
        }
    }

    free(entries);
    fclose(fp);

    return result;
}
