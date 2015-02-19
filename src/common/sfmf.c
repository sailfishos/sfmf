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

#include "logging.h"
#include "convert.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include <endian.h>

int sfmf_fileheader_write(struct SFMF_FileHeader *header, FILE *fp)
{
    struct SFMF_FileHeader h;

    h.magic = htonl(header->magic);
    h.version = htonl(header->version);
    h.metadata_size = htonl(header->metadata_size);
    h.filename_table_size = htonl(header->filename_table_size);
    h.entries_length = htonl(header->entries_length);
    h.packs_length = htonl(header->packs_length);
    h.blobs_length = htonl(header->blobs_length);

    return fwrite(&h, sizeof(struct SFMF_FileHeader), 1, fp);
}

int sfmf_fileheader_read(struct SFMF_FileHeader *header, FILE *fp)
{
    struct SFMF_FileHeader h;

    int res = fread(&h, sizeof(h), 1, fp);

    if (res == 1) {
        header->magic = ntohl(h.magic);
        header->version = ntohl(h.version);
        header->metadata_size = ntohl(h.metadata_size);
        header->filename_table_size = ntohl(h.filename_table_size);
        header->entries_length = ntohl(h.entries_length);
        header->packs_length = ntohl(h.packs_length);
        header->blobs_length = ntohl(h.blobs_length);
    }

    return res;
}

int sfmf_fileentry_write(struct SFMF_FileEntry *entry, FILE *fp)
{
    struct SFMF_FileEntry e;

    e.type = htonl(entry->type);
    e.mode = htonl(entry->mode);
    e.uid = htonl(entry->uid);
    e.gid = htonl(entry->gid);
    e.mtime = htobe64(entry->mtime);
    e.dev = htonl(entry->dev);
    e.zsize = htonl(entry->zsize);

    e.hash.size = htonl(entry->hash.size);
    e.hash.hashtype = htonl(entry->hash.hashtype);
    memcpy(&(e.hash.hash), &(entry->hash.hash), sizeof(e.hash.hash));

    e.filename_offset = htonl(entry->filename_offset);

    return fwrite(&e, sizeof(struct SFMF_FileEntry), 1, fp);
}

int sfmf_fileentry_read(struct SFMF_FileEntry *entry, FILE *fp)
{
    struct SFMF_FileEntry e;

    int res = fread(&e, sizeof(struct SFMF_FileEntry), 1, fp);

    if (res == 1) {
        entry->type = ntohl(e.type);
        entry->mode = ntohl(e.mode);
        entry->uid = ntohl(e.uid);
        entry->gid = ntohl(e.gid);
        entry->mtime = be64toh(e.mtime);
        entry->dev = ntohl(e.dev);
        entry->zsize = ntohl(e.zsize);

        entry->hash.size = ntohl(e.hash.size);
        entry->hash.hashtype = ntohl(e.hash.hashtype);
        memcpy(&(entry->hash.hash), &(e.hash.hash), sizeof(e.hash.hash));

        entry->filename_offset = ntohl(e.filename_offset);
    }

    return res;
}

int sfmf_filehash_write(struct SFMF_FileHash *hash, FILE *fp)
{
    struct SFMF_FileHash h;

    h.size = htonl(hash->size);
    h.hashtype = htonl(hash->hashtype);
    memcpy(&(h.hash), &(hash->hash), sizeof(h.hash));

    return fwrite(&h, sizeof(struct SFMF_FileHash), 1, fp);
}

int sfmf_filehash_read(struct SFMF_FileHash *hash, FILE *fp)
{
    struct SFMF_FileHash h;

    int res = fread(&h, sizeof(struct SFMF_FileHash), 1, fp);

    if (res == 1) {
        hash->size = ntohl(h.size);
        hash->hashtype = ntohl(h.hashtype);
        memcpy(&(hash->hash), &(h.hash), sizeof(h.hash));
    }

    return res;
}

int sfmf_packentry_write(struct SFMF_PackEntry *entry, FILE *fp)
{
    struct SFMF_PackEntry e;

    e.hash.size = htonl(entry->hash.size);
    e.hash.hashtype = htonl(entry->hash.hashtype);
    memcpy(&(e.hash.hash), &(entry->hash.hash), sizeof(e.hash.hash));

    e.offset = htonl(entry->offset);
    e.count = htonl(entry->count);

    return fwrite(&e, sizeof(struct SFMF_PackEntry), 1, fp);
}

int sfmf_packentry_read(struct SFMF_PackEntry *entry, FILE *fp)
{
    struct SFMF_PackEntry e;

    int res = fread(&e, sizeof(struct SFMF_PackEntry), 1,fp);

    if (res == 1) {
        entry->hash.size = ntohl(e.hash.size);
        entry->hash.hashtype = ntohl(e.hash.hashtype);

        memcpy(&(entry->hash.hash), &(e.hash.hash), sizeof(e.hash.hash));

        entry->offset = ntohl(e.offset);
        entry->count = ntohl(e.count);
    }

    return res;
}

int sfmf_blobentry_write(struct SFMF_BlobEntry *entry, FILE *fp)
{
    struct SFMF_BlobEntry e;

    e.hash.size = htonl(entry->hash.size);
    e.hash.hashtype = htonl(entry->hash.hashtype);
    memcpy(&(e.hash.hash), &(entry->hash.hash), sizeof(e.hash.hash));
    e.flags = htonl(entry->flags);
    e.offset = htonl(entry->offset);
    e.size = htonl(entry->size);

    return fwrite(&e, sizeof(struct SFMF_BlobEntry), 1, fp);
}

int sfmf_blobentry_read(struct SFMF_BlobEntry *entry, FILE *fp)
{
    struct SFMF_BlobEntry e;

    int res = fread(&e, sizeof(struct SFMF_BlobEntry), 1, fp);

    if (res == 1) {
        entry->hash.size = ntohl(e.hash.size);
        entry->hash.hashtype = ntohl(e.hash.hashtype);
        memcpy(&(entry->hash.hash), &(e.hash.hash), sizeof(e.hash.hash));
        entry->flags = ntohl(e.flags);
        entry->offset = ntohl(e.offset);
        entry->size = ntohl(e.size);
    }

    return res;
}

int sfmf_filehash_format(struct SFMF_FileHash *hash, char *buf, size_t len)
{
    assert(hash->hashtype == HASHTYPE_SHA1);

    if (len < 41 /* 20 bytes * 2 hex + 1 '\0' */) {
        // Cannot fit formatted hash into target buffer
        return 0;
    }

    for (int i=0; i<20; i++) {
        sprintf(buf + 2 * i, "%02x", hash->hash[i]);
    }

    return 1;
}

int sfmf_filehash_compare(struct SFMF_FileHash *a, struct SFMF_FileHash *b)
{
    assert(a->hashtype == HASHTYPE_SHA1 && b->hashtype == HASHTYPE_SHA1);

    if (a->size != b->size) {
        return a->size - b->size;
    }

    return memcmp(a->hash, b->hash, 20);
}

int sfmf_filehash_verify(struct SFMF_FileHash *expected, const char *filename, int zcompressed)
{
    struct SFMF_FileHash hash;
    memset(&hash, 0, sizeof(hash));
    int res = convert_file_hash(filename, &hash, zcompressed ? CONVERT_FLAG_ZUNCOMPRESS : CONVERT_FLAG_NONE);
    assert(res == 0);

    char tmp[100];
    res = sfmf_filehash_format(expected, tmp, sizeof(tmp));

    SFMF_DEBUG("Checking file hash of %s (expecting %s)\n", filename, tmp);
    if (sfmf_filehash_compare(&hash, expected) != 0) {
        res = sfmf_filehash_format(&hash, tmp, sizeof(tmp));
        assert(res);

        SFMF_WARN("File failed hash check: %s, got: %s\n", filename, tmp);
        return 1;
    } else {
        SFMF_DEBUG("File passed hash check: %s\n", filename);
        return 0;
    }
}
