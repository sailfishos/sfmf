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

#ifndef SAILFISH_SNAPSHOT_SFMF_H
#define SAILFISH_SNAPSHOT_SFMF_H

#include <stdint.h>
#include <stdio.h>

// make sure stuff is 32-bit aligned in the structs
// all integer values are stored in network byte order

/* Magic number header of sfmf files */
#define SFMF_MAGIC_NUMBER (('S' << 24) | ('F' << 16) | ('M' << 8) | 'F')

/* Maximum hash length (in bytes) */
#define SFMF_MAX_HASHSIZE 20

/* File version - increment when it changes */
#define SFMF_CURRENT_VERSION 1

/**
 * Structure of a manifest file:
 *
 *  - header
 *  - metadata
 *  - filename table
 *  - entries list
 *  - blobs index
 *  - packs index
 *  - blobs
 *  - packs
 **/

struct SFMF_FileHeader {
    uint32_t magic; // 'S' 'F' 'M' 'F'
    uint32_t version; // SFMF_CURRENT_VERSION
    uint32_t metadata_size;
    uint32_t filename_table_size;
    uint32_t entries_length;
    uint32_t packs_length;
    uint32_t blobs_length;

    // variable size '\0'-terminated metadata blob (<metadata_size> bytes)
    // variable size filename table (<filename_size> bytes)
    // variable size list of <entries_length> x SFMF_FileEntry structs
    // variable size list of <packs_length> x SFMF_PackEntry structs
    // variable size list of <blobs_length> x SFMF_BlobEntry structs
    // tightly packed pack payload
    // tightly packed blob payload
};

enum SFMF_FileEntry_Type {
    ENTRY_UNKNOWN = 0, // invalid
    ENTRY_DIRECTORY = 1,
    ENTRY_FILE = 2,
    ENTRY_SYMLINK = 3,
    ENTRY_CHARACTER = 4,
    ENTRY_FIFO = 5,
    ENTRY_HARDLINK = 6,
    ENTRY_BLOCK = 7,
    /* ... */
};

enum SFMF_FileEntry_HashType {
    HASHTYPE_UNKNOWN = 0, // invalid
    HASHTYPE_SHA1 = 1,
    HASHTYPE_LAZY = 2, // only used at runtime; for on-demand hash calculation
    /* ... */
};

struct SFMF_FileHash {
    uint32_t size; // file size in bytes (not hash size)
    uint32_t hashtype; // SFMF_FileEntry_HashType
    unsigned char hash[SFMF_MAX_HASHSIZE];
};

struct SFMF_FileEntry {
    uint32_t type; // SFMF_FileEntry_Type
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t mtime; // mtime as unix timestamp
    uint32_t dev; // for ENTRY_CHARACTER or ENTRY_BLOCK, the device node value
    uint32_t zsize; // compressed file size in bytes

    struct SFMF_FileHash hash; // includes file size and hash value
    uint32_t filename_offset; // offset into filename table
};

struct SFMF_PackEntry {
    struct SFMF_FileHash hash; // hash of the pack (to be used to look up, size = download size in bytes)
    uint32_t offset; // absolute file offset of first SFMF_FileHash for this pack
    uint32_t count; // number of file hashes contained in this pack
};

enum SFMF_BlobEntry_Flag {
    BLOB_FLAG_NONE = 0,
    BLOB_FLAG_ZCOMPRESSED = 1 << 0,
    /* ... */
};

// some blobs might not be z-compressed if their uncompressed size is smaller
struct SFMF_BlobEntry {
    struct SFMF_FileHash hash; // hash of the blob and uncompressed file size)
    uint32_t flags; // OR-ed field of SFMF_BlobEntry_Flag values
    uint32_t offset; // absolute file offset of start of blob data
    uint32_t size; // number of bytes for this blob (in the file)
};

int sfmf_fileheader_write(struct SFMF_FileHeader *header, FILE *fp);
int sfmf_fileheader_read(struct SFMF_FileHeader *header, FILE *fp);

int sfmf_fileentry_write(struct SFMF_FileEntry *entry, FILE *fp);
int sfmf_fileentry_read(struct SFMF_FileEntry *entry, FILE *fp);

int sfmf_filehash_write(struct SFMF_FileHash *hash, FILE *fp);
int sfmf_filehash_read(struct SFMF_FileHash *hash, FILE *fp);

int sfmf_packentry_write(struct SFMF_PackEntry *entry, FILE *fp);
int sfmf_packentry_read(struct SFMF_PackEntry *entry, FILE *fp);

int sfmf_blobentry_write(struct SFMF_BlobEntry *entry, FILE *fp);
int sfmf_blobentry_read(struct SFMF_BlobEntry *entry, FILE *fp);

int sfmf_filehash_format(struct SFMF_FileHash *hash, char *buf, size_t len);
int sfmf_filehash_compare(struct SFMF_FileHash *a, struct SFMF_FileHash *b);
int sfmf_filehash_verify(struct SFMF_FileHash *expected, const char *filename, int zcompressed);

#endif /* SAILFISH_SNAPSHOT_SFMF_H */
