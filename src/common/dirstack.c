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

#include "dirstack.h"

#include "logging.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>


static struct DirStack *dirstack_resize(struct DirStack *stack, uint32_t size)
{
    assert(stack);
    assert(size >= stack->length);

    stack->size = size;
    stack->data = realloc(stack->data, stack->size * sizeof(struct DirStackEntry));

    return stack;
}

struct DirStack *dirstack_new(dirstack_pop_t pop_func)
{
    struct DirStack *stack = calloc(1, sizeof(struct DirStack));
    stack->pop_func = pop_func;
    return dirstack_resize(stack, 128);
}

static int dirstack_pop(struct DirStack *stack)
{
    assert(stack);

    if (stack->length > 0) {
        struct DirStackEntry *top = &(stack->data[stack->length-1]);

        //SFMF_DEBUG("DirStack POP: %s\n", top->path);

        stack->pop_func(top);
        free(top->path);
        stack->length--;

        memset(top, 0, sizeof(struct DirStackEntry));
    }

    return (stack->length > 0);
}

static int is_prefix_of(const char *prefix, const char *path)
{
    assert(prefix && path);

    // We assume that we never get into a situation where the same path is
    // pushed twice, so we never do a self-compare for the prefix check
    assert(strcmp(prefix, path) != 0);

    int prefix_len = strlen(prefix);
    int path_len = strlen(path);

    // This is a prefix:
    //     prefix = "/foo"
    //       path = "/foo/bar"
    // However, this one is not:
    //     prefix = "/foo"
    //       path = "/foobar"
    // This is also a prefix (note that path[prefix_len] != '/'):
    //     prefix = "out3/"
    //       path = "out3/usr"
    return (path_len > prefix_len &&
            strncmp(prefix, path, prefix_len) == 0 &&
            (prefix[prefix_len - 1] == '/' || path[prefix_len] == '/'));
}

struct DirStack *dirstack_push(struct DirStack *stack, const char *path, void *user_data)
{
    assert(stack != NULL && path != NULL);

    while (stack->length > 0) {
        struct DirStackEntry *top = &(stack->data[stack->length-1]);

        if (is_prefix_of(top->path, path)) {
            // Done, top-of-stack is prefix of current path
            break;
        } else {
            // Newly-added path is not "below" top of stack, need to pop the
            // top element (and retry if there are still entries on the stack)

            //SFMF_DEBUG("pop'ing non-prefix: %s (path = %s)\n", top->path, path);

            dirstack_pop(stack);
        }
    }

    if (stack->size < stack->length + 1) {
        stack = dirstack_resize(stack, stack->size + 128);
    }

    struct DirStackEntry *current = &(stack->data[stack->length++]);

    current->path = strdup(path);
    current->user_data = user_data;

    return stack;
}

void dirstack_free(struct DirStack *stack)
{
    assert(stack);

    // Pop all elements until stack is empty
    while (dirstack_pop(stack));
    assert(stack->length == 0);

    free(stack->data);
    free(stack);
}
