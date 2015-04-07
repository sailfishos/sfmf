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

#include "privileged.h"

#include "logging.h"

#include <glib.h>
#include <gio/gio.h>


gboolean sfmf_dbus_is_privileged(GDBusConnection *connection, const gchar *sender)
{
    gboolean result = FALSE;

    GVariantType *rtype = g_variant_type_new("(u)");
    GError *error = NULL;
    GVariant *boxedpid = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus",
            "/", "org.freedesktop.DBus", "GetConnectionUnixProcessID",
            g_variant_new("(s)", sender), rtype, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    g_variant_type_free(rtype);

    if (!boxedpid) {
        SFMF_WARN("Could not check caller privileges: '%s' (%s)\n", sender, error->message);
        g_error_free(error);
    } else {
        guint32 pid = 0;
        g_variant_get(boxedpid, "(u)", &pid);
        char *path = g_strdup_printf("/proc/%u", pid);
        GFile *proc = g_file_new_for_path(path);
        GFileInfo *info = g_file_query_info(proc, G_FILE_ATTRIBUTE_OWNER_USER ","
                G_FILE_ATTRIBUTE_OWNER_GROUP, G_FILE_QUERY_INFO_NONE, NULL, &error);
        if (!info) {
            SFMF_WARN("Could not get owner of '%s': %s\n", path, error->message);
            g_error_free(error);
        } else {
            const char *effective_user = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_OWNER_USER);
            const char *effective_group = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_OWNER_GROUP);
            if (g_strcmp0(effective_user, "root") == 0 || g_strcmp0(effective_group, "privileged") == 0) {
                result = TRUE;
            }
            SFMF_DEBUG("Method call: pid=%u, user=%s, group=%s, decision=%s\n",
                    pid, effective_user, effective_group, result ? "allow" : "deny");
            g_object_unref(info);
        }
        g_object_unref(proc);
        g_free(path);
        g_variant_unref(boxedpid);
    }

    return result;
}
