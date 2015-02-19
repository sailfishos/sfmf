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

#include "sfpf.h"
#include <arpa/inet.h>

int sfpf_fileheader_write(struct SFPF_FileHeader *header, FILE *fp)
{
    struct SFPF_FileHeader h;

    h.magic = htonl(header->magic);
    h.version = htonl(header->version);
    h.metadata_size = htonl(header->metadata_size);
    h.blobs_length = htonl(header->blobs_length);

    return fwrite(&h, sizeof(struct SFPF_FileHeader), 1, fp);
}

int sfpf_fileheader_read(struct SFPF_FileHeader *header, FILE *fp)
{
    struct SFPF_FileHeader h;

    int result = fread(&h, sizeof(struct SFPF_FileHeader), 1, fp);
    if (result == 1) {
        header->magic = ntohl(h.magic);
        header->version = ntohl(h.version);
        header->metadata_size = ntohl(h.metadata_size);
        header->blobs_length = ntohl(h.blobs_length);
    }

    return result;
}
