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

#ifndef SFMF_DIRSTACK_H
#define SFMF_DIRSTACK_H

#include <stdint.h>

struct DirStackEntry {
    char *path;
    void *user_data;
};

typedef void (*dirstack_pop_t)(struct DirStackEntry *entry);

struct DirStack {
    struct DirStackEntry *data;
    uint32_t length; // current length
    uint32_t size; // allocated size

    // Callbacks
    dirstack_pop_t pop_func;
};

struct DirStack *dirstack_new(dirstack_pop_t pop_func);
struct DirStack *dirstack_push(struct DirStack *stack, const char *path, void *user_data);
void dirstack_free(struct DirStack *stack);

#endif /* SFMF_DIRSTACK_H */
