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

#include "convert.h"
#include "logging.h"
#include "control.h"

#include <stdio.h>
#include <assert.h>
#include <zlib.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <errno.h>

#include <sha1.h>

#if defined(USE_LIBCURL)
#include <curl/curl.h>
#endif /* USE_LIBCURL */


struct ConvertIO {
    ssize_t (*transfer)(char *buffer, size_t len, void *user_data);
    void *user_data;
    size_t total;
};

struct ConvertContext {
    struct ConvertIO *read;
    struct ConvertIO *write;
};

struct DuplicateConvertIOContext {
    struct ConvertIO *master; // original to be read from/written to
    struct ConvertIO *slave; // gets all (read/written) data written to
};

struct BufferConvertContextSource {
    char *buf;
    size_t len;
    size_t pos;
};

// Number of blocks transferred between mainloop pumps
#define PUMP_MAINLOOP_EVERY_X_BLOCKS 300

static ssize_t convert_io_transfer(struct ConvertIO *io, char *buffer, size_t len)
{
    static int iterations = 0;
    if (++iterations >= PUMP_MAINLOOP_EVERY_X_BLOCKS) {
        // Pump the mainloop after every X blocks transferred; should give
        // good responsiveness while not slowing down data transfer
        sfmf_control_process();
        iterations = 0;
    }

    ssize_t res = io->transfer(buffer, len, io->user_data);
    if (res >= 0) {
        io->total += res;
    }
    return res;
}

static ssize_t convert_io_read(struct ConvertContext *ctx, char *buffer, size_t len)
{
    return convert_io_transfer(ctx->read, buffer, len);
}

static ssize_t convert_io_write(struct ConvertContext *ctx, char *buffer, size_t len)
{
    return convert_io_transfer(ctx->write, buffer, len);
}

static ssize_t duplicate_convert_io_read(char *buffer, size_t len, void *user_data)
{
    struct DuplicateConvertIOContext *ctx = user_data;

    ssize_t res = convert_io_transfer(ctx->master, buffer, len); // Read from master
    // Consumer MUST NOT modify the buffer it is given
    ssize_t res2 = convert_io_transfer(ctx->slave, buffer, res); // Write to slave (but only as much bytes as we read before)

    assert(res == res2);

    return res;
}

void do_convert_uncompressed(struct ConvertContext *ctx)
{
    char buf[DEFAULT_BUFFER_SIZE];
    ssize_t len;

    while ((len = convert_io_read(ctx, buf, sizeof(buf)))) {
        ssize_t res = convert_io_write(ctx, buf, len);
        assert(res == len);
    }
}

void do_convert_zcompressed(struct ConvertContext *ctx)
{
    const uint32_t buffer_size = DEFAULT_BUFFER_SIZE;
    char tmp_in[buffer_size];
    char tmp_out[buffer_size];

    z_stream stream;
    memset(&stream, 0, sizeof(stream));

    int res = deflateInit(&stream, Z_DEFAULT_COMPRESSION);
    assert(res == Z_OK);

    ssize_t read_bytes = 0;
    while ((read_bytes = convert_io_read(ctx, tmp_in, buffer_size)) != 0) {
        stream.next_in = (unsigned char *)tmp_in;
        stream.avail_in = read_bytes;

        // Let deflate() consume the whole input buffer
        do {
            stream.next_out = (unsigned char *)tmp_out;
            stream.avail_out = buffer_size;
            res = deflate(&stream, 0);
            assert(res == Z_OK);
            ssize_t len = (buffer_size - stream.avail_out);
            ssize_t res = convert_io_write(ctx, tmp_out, len);
            assert(res == len);
        } while (stream.avail_in > 0);
    }

    // Read output buffer empty
    do {
        stream.next_out = (unsigned char *)tmp_out;
        stream.avail_out = buffer_size;
        res = deflate(&stream, Z_FINISH);
        assert(res == Z_OK || res == Z_STREAM_END);
        ssize_t len = (buffer_size - stream.avail_out);
        ssize_t res = convert_io_write(ctx, tmp_out, len);
        assert(res == len);
    } while (res != Z_STREAM_END);

    deflateEnd(&stream);
}

void do_convert_zdecompress(struct ConvertContext *ctx)
{
    const uint32_t buffer_size = DEFAULT_BUFFER_SIZE;
    char tmp_in[buffer_size];
    char tmp_out[buffer_size];

    z_stream stream;
    memset(&stream, 0, sizeof(stream));

    int res = inflateInit(&stream);
    assert(res == Z_OK);

    ssize_t read_bytes = 0;
    while ((read_bytes = convert_io_read(ctx, tmp_in, buffer_size)) != 0) {
        stream.next_in = (unsigned char *)tmp_in;
        stream.avail_in = read_bytes;

        // Let inflate() consume the whole input buffer
        do {
            stream.next_out = (unsigned char *)tmp_out;
            stream.avail_out = buffer_size;
            res = inflate(&stream, 0);
            assert(res == Z_OK || res == Z_STREAM_END);
            ssize_t len = (buffer_size - stream.avail_out);
            ssize_t res = convert_io_write(ctx, tmp_out, len);
            assert(res == len);
        } while (stream.avail_in > 0);
    }

    // Read output buffer empty
    do {
        stream.next_out = (unsigned char *)tmp_out;
        stream.avail_out = buffer_size;
        res = inflate(&stream, Z_FINISH);
        assert(res == Z_OK || res == Z_STREAM_END);
        ssize_t len = (buffer_size - stream.avail_out);
        ssize_t res = convert_io_write(ctx, tmp_out, len);
        assert(res == len);
    } while (res != Z_STREAM_END);

    inflateEnd(&stream);
}

ssize_t file_convert_context_read(char *buffer, size_t len, void *user_data)
{
    FILE *fp = user_data;
    return fread(buffer, 1, len, fp);
}

ssize_t buffer_convert_context_read(char *buffer, size_t len, void *user_data)
{
    struct BufferConvertContextSource *source = user_data;

    size_t remaining = (source->len - source->pos);
    if (len > remaining) {
        len = remaining;
    }

    if (len) {
        memcpy(buffer, source->buf + source->pos, len);
        source->pos += len;
    }

    return len;
}

ssize_t file_convert_context_write(char *buffer, size_t len, void *user_data)
{
    FILE *fp = user_data;
    return fwrite(buffer, 1, len, fp);
}

static const char *get_compression_method(enum ConvertFlags flags)
{
    switch (flags) {
        case CONVERT_FLAG_NONE:
            return "copy";
        case CONVERT_FLAG_ZCOMPRESS:
            return "compress";
        case CONVERT_FLAG_ZUNCOMPRESS:
            return "decompress";
        default:
            // fallthrough
            break;
    }

    return "???";
}

#if defined(USE_LIBCURL)
int convert_url_fp(const char *url, FILE *out, enum ConvertFlags flags)
{
    if (flags != CONVERT_FLAG_NONE) {
        SFMF_WARN("Compression on URLs not supported\n");
        return 1;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        SFMF_FAIL("Could not init cURL\n");
    }

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        SFMF_FAIL("Could not init cURL-easy\n");
    }

    SFMF_DEBUG("Download %s\n", url);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "sfmf/" VERSION " (+https://sailfishos.org/)");

    /* Error handling */
    //curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L * 20L /* seconds */);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 4196L /* bytes/second */);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 10L /* seconds */);

    /* Authentication */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    /* Redirection */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        SFMF_FAIL("Could not download %s: %s\n", url, curl_easy_strerror(res));
    }

    curl_easy_cleanup(curl);
    curl_global_cleanup();

    return 0;
}
#endif /* USE_LIBCURL */

int convert_file(const char *infile, const char *outfile, enum ConvertFlags flags)
{
    SFMF_DEBUG("Convert %s -> %s (%s)\n", infile, outfile, get_compression_method(flags));

    FILE *ifp = fopen(infile, "rb");
    assert(ifp != NULL);

    FILE *ofp = fopen(outfile, "wb");
    assert(ofp != NULL);

    convert_file_fp(ifp, ofp, flags);

    fclose(ifp);
    fclose(ofp);

    return 0;
}

static int run_conversion(struct ConvertIO *read_io, struct ConvertIO *write_io, enum ConvertFlags flags)
{
    struct ConvertContext ctx = {
        read_io,
        write_io,
    };

    switch (flags) {
        case CONVERT_FLAG_NONE:
            do_convert_uncompressed(&ctx);
            break;
        case CONVERT_FLAG_ZCOMPRESS:
            do_convert_zcompressed(&ctx);
            break;
        case CONVERT_FLAG_ZUNCOMPRESS:
            do_convert_zdecompress(&ctx);
            break;
        default:
            assert(0);
            break;
    }

    return 0;
}

// Taken from GNU coreutils' src/copy.c
static inline int
clone_file (int dest_fd, int src_fd)
{
# undef BTRFS_IOCTL_MAGIC
# define BTRFS_IOCTL_MAGIC 0x94
# undef BTRFS_IOC_CLONE
# define BTRFS_IOC_CLONE _IOW (BTRFS_IOCTL_MAGIC, 9, int)
  return ioctl (dest_fd, BTRFS_IOC_CLONE, src_fd);
}

int convert_file_fp(FILE *infile, FILE *outfile, enum ConvertFlags flags)
{
    struct ConvertIO read_io = {
        file_convert_context_read,
        infile,
        0,
    };

    struct ConvertIO write_io = {
        file_convert_context_write,
        outfile,
        0,
    };

    /**
     * Optimization shortcut on btrfs: If we copy from one file to another,
     * we can create a reflink copy, so that the files share the same data
     * blocks (uses copy-on-write when one file is modified).
     **/
    if (flags == CONVERT_FLAG_NONE) {
        int src_fd = fileno(infile);
        int dest_fd = fileno(outfile);

        if (src_fd != -1 && dest_fd != -1) {
            if (clone_file(dest_fd, src_fd) == 0) {
                fprintf(stderr, "BTRFS: Successfully reflinked file (CoW)\n");
                return 0;
            } else {
                //fprintf(stderr, "BTRFS: Could not reflink file (%s)\n", strerror(errno));
            }
        }
    }

    return run_conversion(&read_io, &write_io, flags);
}

int convert_buffer_fp(char *buf, size_t len, FILE *outfile, enum ConvertFlags flags)
{
    struct BufferConvertContextSource source = { buf, len, 0 };

    struct ConvertIO read_io = {
        buffer_convert_context_read,
        &source,
        0,
    };

    struct ConvertIO write_io = {
        file_convert_context_write,
        outfile,
        0,
    };

    return run_conversion(&read_io, &write_io, flags);
}

static ssize_t sha1_convert_context_write(char *buf, size_t len, void *user_data)
{
    SHA1_CTX *ctx = user_data;

    // When we don't have SHA1HANDSOFF defined, SHA1_Update will modify the contents
    // of the buffer passed to it, which we can't allow, so we use a local one here.
    unsigned char *tmp = malloc(len);
    memcpy(tmp, buf, len);
    SHA1_Update(ctx, tmp, len);
    free(tmp);

    return len;
}

static ssize_t null_convert_context_write(char *buf, size_t len, void *user_data)
{
    // Not doing any actual writing here (we just count the zbytes)
    return len;
}

int convert_file_zsize_hash(const char *filename, struct SFMF_FileHash *hash, uint32_t *zsize)
{
    FILE *infile = fopen(filename, "rb");
    assert(infile != NULL);

    // Pipeline goes like this:
    //
    //  (o) -- (sha1) --> (zcompress) --> (o)
    //   ^       ^ calculate sha1sum       ^
    //   |                                 |
    //   |                 discard output, but remember
    //   |                 total bytes written (=zsize)
    //   file source
    //
    // Or with the structs from below:
    //
    // file_read_io -> dup_ctx -> sha1_write_io
    //                    |
    //                    +-> dup_read_io -> (zcompress) -> null_write_io

    SHA1_CTX sha1ctx;
    SHA1_Init(&sha1ctx);

    struct ConvertIO file_read_io = {
        file_convert_context_read,
        infile,
        0,
    };

    struct ConvertIO sha1_write_io = {
        sha1_convert_context_write,
        &sha1ctx,
        0,
    };

    struct DuplicateConvertIOContext dup_ctx = {
        &file_read_io,
        &sha1_write_io,
    };

    struct ConvertIO dup_read_io = {
        duplicate_convert_io_read,
        &dup_ctx,
        0,
    };

    struct ConvertIO null_write_io = {
        null_convert_context_write,
        NULL,
        0,
    };

    // Only compress if the caller is interested in the zsize
    int res = run_conversion(&dup_read_io, &null_write_io,
            zsize ? CONVERT_FLAG_ZCOMPRESS : CONVERT_FLAG_NONE);

    hash->hashtype = HASHTYPE_SHA1;
    SHA1_Final(&sha1ctx, (unsigned char *)&(hash->hash));

    if (zsize) {
        *zsize = null_write_io.total;
    } else {
        // If zsize == NULL, the uncompressed size will be stored
        // in the hash structure (to get a hash object that can be
        // compared to an expected hash object)
        hash->size = null_write_io.total;
    }

    fclose(infile);

    return res;
}

int convert_file_hash(const char *filename, struct SFMF_FileHash *hash, enum ConvertFlags flags)
{
    FILE *infile = fopen(filename, "rb");
    assert(infile != NULL);

    SHA1_CTX sha1ctx;
    SHA1_Init(&sha1ctx);

    struct ConvertIO file_read_io = {
        file_convert_context_read,
        infile,
        0,
    };

    struct ConvertIO sha1_write_io = {
        sha1_convert_context_write,
        &sha1ctx,
        0,
    };

    int res = run_conversion(&file_read_io, &sha1_write_io, flags);

    hash->hashtype = HASHTYPE_SHA1;
    SHA1_Final(&sha1ctx, (unsigned char *)&(hash->hash));
    hash->size = sha1_write_io.total;

    fclose(infile);

    return res;
}
