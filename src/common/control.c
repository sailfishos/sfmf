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

#include "control.h"
#include "logging.h"

#include <stdlib.h>
#include <gio/gio.h>


static GDBusConnection *g_connection = NULL;
static guint g_own_name = 0;
static gboolean g_name_acquired = FALSE;
static guint g_object_registration = 0;
static struct SFMF_Control_Callbacks *g_callbacks = NULL;
static void *g_callbacks_user_data = NULL;

static void sfmf_control_bus_acquired_cb(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    SFMF_DEBUG("Bus acquired with name '%s'\n", name);
    g_object_ref(connection);
    g_connection = connection;
}

static void sfmf_control_name_acquired_cb(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    SFMF_DEBUG("Name acquired: '%s'\n", name);
    g_name_acquired = TRUE;
}

static void sfmf_control_name_lost_cb(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    if (!connection) {
        SFMF_FAIL("Could not establish D-Bus connection\n");
    } else {
        SFMF_FAIL("D-Bus name lost: '%s'\n", name);
    }

    g_name_acquired = FALSE;
}

static void sfmf_control_method_call_cb(GDBusConnection *connection, const gchar *sender,
        const gchar *object_path, const gchar *interface_name, const gchar *method_name,
        GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data)
{
    if (g_strcmp0(object_path, "/") == 0 && g_strcmp0(interface_name, "org.sailfishos.sfmf") == 0) {
        if (g_strcmp0(method_name, "Abort") == 0) {
            gboolean result = FALSE;
            if (g_callbacks && g_callbacks->abort) {
                result = g_callbacks->abort(g_callbacks_user_data);
            }
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", result));
            return;
        }
    }

    g_dbus_method_invocation_return_dbus_error(invocation, "org.sailfishos.sfmf.MethodCallError",
            "Invalid method call");
}

static const GDBusInterfaceVTable g_vtable = {
    sfmf_control_method_call_cb,
    NULL,
    NULL,
};

void sfmf_control_init(struct SFMF_Control_Callbacks *callbacks, void *user_data)
{
    g_callbacks = callbacks;
    g_callbacks_user_data = user_data;

    g_own_name = g_bus_own_name(G_BUS_TYPE_SYSTEM, "org.sailfishos.sfmf", G_BUS_NAME_OWNER_FLAGS_NONE,
            sfmf_control_bus_acquired_cb, sfmf_control_name_acquired_cb, sfmf_control_name_lost_cb,
            NULL, NULL);

    while (!g_name_acquired) {
        sfmf_control_process();
    }

    GError *error = NULL;
    GDBusNodeInfo *node_info = g_dbus_node_info_new_for_xml(""
        "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
        "  \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
        "<node name=\"/\">\n"
        "  <interface name=\"org.sailfishos.sfmf\">\n"
        "    <method name=\"Abort\">\n"
        "      <arg name=\"result\" type=\"b\" direction=\"out\" />\n"
        "    </method>\n"
        "    <signal name=\"Progress\">\n"
        "      <arg name=\"subvolume\" type=\"s\" />\n"
        "      <arg name=\"progress\" type=\"i\" />\n"
        "    </signal>\n"
        "  </interface>\n"
        "</node>\n"
    "", &error);
    if (!node_info) {
        SFMF_FAIL("Could not compile D-Bus XML: %s\n", error->message);
        g_error_free(error);
    }

    g_object_registration = g_dbus_connection_register_object(g_connection, "/",
            g_dbus_node_info_lookup_interface(node_info, "org.sailfishos.sfmf"),
            &g_vtable, NULL, NULL, &error);

    if (!g_object_registration) {
        SFMF_FAIL("Could not register object on D-Bus: %s\n", error->message);
        g_error_free(error);
    }

    g_dbus_node_info_unref(node_info);
}

void sfmf_control_process()
{
    while (g_main_context_iteration(NULL, FALSE)) {
        // pump the main loop
    }
}

void sfmf_control_set_progress(const char *target, int progress)
{
    GError *error = NULL;

    if (g_connection && !g_dbus_connection_emit_signal(g_connection, NULL,
                "/", "org.sailfishos.sfmf", "Progress",
                g_variant_new("(si)", target, progress), &error)) {
        SFMF_WARN("Could not send progress via D-Bus: %s\n", error->message);
        g_error_free(error);
    }

    sfmf_control_process();
}

void sfmf_control_close()
{
    while (g_main_context_iteration(NULL, FALSE)) {
        // empty loop body
    }

    if (g_object_registration != 0) {
        g_dbus_connection_unregister_object(g_connection, g_object_registration), g_object_registration = 0;
    }

    if (g_own_name != 0) {
        g_bus_unown_name(g_own_name), g_own_name = 0;
    }

    if (g_connection) {
        g_object_unref(g_connection), g_connection = NULL;
    }

    g_callbacks = NULL;
    g_callbacks_user_data = NULL;
}
