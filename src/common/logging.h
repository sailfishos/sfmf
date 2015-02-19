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

#ifndef SFMF_LOGGING_H
#define SFMF_LOGGING_H

#include <stdio.h>

#include "policy.h"

long logging_get_ticks();

#define SFMF_LOG(fmt, ...) fprintf(stderr, "[%6ld] " fmt, logging_get_ticks(), ##__VA_ARGS__)
#define SFMF_WARN(fmt, ...) fprintf(stderr, "[%6ld] [WARN] " fmt, logging_get_ticks(), ##__VA_ARGS__)
#define SFMF_DEBUG(fmt, ...) if (sfmf_policy_get_log_debug()) fprintf(stderr, "[%6ld] [DEBUG] " fmt, logging_get_ticks(), ##__VA_ARGS__)

#define SFMF_FAIL(fmt, ...) do { fprintf(stderr, "[%6ld] [ERROR] " fmt, logging_get_ticks(), ##__VA_ARGS__); exit(1); } while(0)

#endif /* SFMF_LOGGING_H */
