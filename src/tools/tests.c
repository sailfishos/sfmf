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
#include "convert.h"

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>


int main(int argc, char *argv[])
{
    char buf[1024*1024*2];
    memset(buf, 0, sizeof(buf));
    FILE *fp = fopen("/dev/urandom", "r");
    fread(buf+1024*1024, sizeof(buf)-1024*1024, 1, fp);
    fclose(fp);

    FILE *uncompressed = fopen("uncompressed", "w");
    convert_buffer_fp(buf, sizeof(buf), uncompressed, CONVERT_FLAG_NONE);
    fclose(uncompressed);

    FILE *zcompressed = fopen("zcompressed", "w");
    convert_buffer_fp(buf, sizeof(buf), zcompressed, CONVERT_FLAG_ZCOMPRESS);
    fclose(zcompressed);

    struct SFMF_FileHash a_hash;
    char a[100];
    memset(&a_hash, 0, sizeof(a_hash));
    convert_file_hash("uncompressed", &a_hash, CONVERT_FLAG_NONE);
    sfmf_filehash_format(&a_hash, a, sizeof(a));
    printf("Got uncompressed hash: %s (%d)\n", a, a_hash.size);

    struct SFMF_FileHash b_hash;
    char b[100];
    memset(&b_hash, 0, sizeof(b_hash));
    convert_file_hash("zcompressed", &b_hash, CONVERT_FLAG_ZUNCOMPRESS);
    sfmf_filehash_format(&b_hash, b, sizeof(b));
    printf("Got zcompressed hash: %s (%d)\n", a, b_hash.size);

    unlink("uncompressed");
    unlink("zcompressed");

    assert(sfmf_filehash_compare(&a_hash, &b_hash) == 0);

    return 0;
}
