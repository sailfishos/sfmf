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

#include "policy.h"


static int g_ignore_unsupported = 0;
static int g_log_debug = 1;

void
sfmf_policy_set_ignore_unsupported(int ignore_unsupported)
{
    g_ignore_unsupported = ignore_unsupported;
}

int
sfmf_policy_get_ignore_unsupported()
{
    return g_ignore_unsupported;
}

void
sfmf_policy_set_log_debug(int log_debug)
{
    g_log_debug = log_debug;
}

int
sfmf_policy_get_log_debug()
{
    return g_log_debug;
}
