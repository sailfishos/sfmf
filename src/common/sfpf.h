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

#ifndef SAILFISH_SNAPSHOT_SFPF_H
#define SAILFISH_SNAPSHOT_SFPF_H

#include "sfmf.h"
#include <stdio.h>

// make sure stuff is 32-bit aligned in the structs
// all integer values are stored in network byte order

/* Magic number header of sfmf files */
#define SFPF_MAGIC_NUMBER (('S' << 24) | ('F' << 16) | ('P' << 8) | 'F')

/* File version - increment when it changes */
#define SFPF_CURRENT_VERSION 1

/**
 * Structure of a pack file:
 *
 *  - header
 *  - metadata
 *  - blob index
 *  - blobs
 **/

struct SFPF_FileHeader {
    uint32_t magic; // 'S' 'F' 'P' 'F'
    uint32_t version; // SFPF_CURRENT_VERSION
    uint32_t metadata_size;
    uint32_t blobs_length;

    // variable size '\0'-terminated metadata blob (<metadata_size> bytes)
    // variable size list of <blobs_length> x SFMF_BlobEntry structs
    // tightly packed blob payload
};

int sfpf_fileheader_write(struct SFPF_FileHeader *header, FILE *fp);
int sfpf_fileheader_read(struct SFPF_FileHeader *header, FILE *fp);

#endif /* SAILFISH_SNAPSHOT_SFPF_H */
