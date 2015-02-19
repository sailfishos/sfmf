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

#include "cleanup.h"

#include <signal.h>
#include <assert.h>
#include <stdlib.h>

#include "logging.h"


static cleanup_func_t g_cleanup_func = 0;
static void *g_cleanup_func_user_data = 0;

static void (*g_sig_action_term)(int) = SIG_DFL;
static void (*g_sig_action_int)(int) = SIG_DFL;
static void (*g_sig_action_hup)(int) = SIG_DFL;


void on_signal(int sig)
{
    SFMF_WARN("Signal %d received, running cleanup\n", sig);

    sfmf_cleanup_run();

    // Re-raise the signal
    switch (sig) {
        case SIGTERM:
            signal(sig, g_sig_action_term);
            break;
        case SIGINT:
            signal(sig, g_sig_action_int);
            break;
        case SIGHUP:
            signal(sig, g_sig_action_hup);
            break;
        default:
            signal(sig, SIG_DFL);
            break;
    }
    raise(sig);
}

void sfmf_cleanup_register(cleanup_func_t cleanup_func, void *user_data)
{
    assert(g_cleanup_func == 0);
    assert(g_cleanup_func_user_data == 0);

    g_cleanup_func = cleanup_func;
    g_cleanup_func_user_data = user_data;

    g_sig_action_term = signal(SIGTERM, on_signal);
    g_sig_action_term = signal(SIGINT, on_signal);
    g_sig_action_term = signal(SIGHUP, on_signal);

    atexit(sfmf_cleanup_run);
}

void sfmf_cleanup_run()
{
    if (g_cleanup_func) {
        // Set g_cleanup_func to 0 before calling it so we don't call it twice
        // if a signal happens while running g_cleanup_func
        cleanup_func_t f = g_cleanup_func;
        g_cleanup_func = 0;
        f(g_cleanup_func_user_data);

        g_cleanup_func_user_data = 0;
    }
}
