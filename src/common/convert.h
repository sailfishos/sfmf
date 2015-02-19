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

#ifndef SAILFISH_SNAPSHOT_CONVERT_H
#define SAILFISH_SNAPSHOT_CONVERT_H

#include <stdio.h>
#include "sfmf.h"

#define DEFAULT_BUFFER_SIZE (64 * 1024)

enum ConvertFlags {
    CONVERT_FLAG_NONE = 0,
    CONVERT_FLAG_ZCOMPRESS = 1,
    CONVERT_FLAG_ZUNCOMPRESS = 2,
};

/**
 * convert a file (infile) to another file (outfile),
 * optionally with zlib compression (deflate)
 **/
#if defined(USE_LIBCURL)
int convert_url_fp(const char *url, FILE *outfile, enum ConvertFlags flags);
#endif /* USE_LIBCURL */
int convert_file(const char *infile, const char *outfile, enum ConvertFlags flags);
int convert_file_fp(FILE *infile, FILE *outfile, enum ConvertFlags flags);
int convert_buffer_fp(char *buf, size_t len, FILE *outfile, enum ConvertFlags flags);

// Passing in NULL for zsize (if not required) will just calculate the hash of the file;
// this is faster than also calculating the zsize (which compresses all input data). In
// case zsize == NULL, the total size of the file will be stored in hash->size, which
// is useful for getting a hash object for a given file to be compared later.
int convert_file_zsize_hash(const char *filename, struct SFMF_FileHash *hash, uint32_t *zsize);
int convert_file_hash(const char *filename, struct SFMF_FileHash *hash, enum ConvertFlags flags);

#endif /* SAILFISH_SNAPSHOT_CONVERT_H */
