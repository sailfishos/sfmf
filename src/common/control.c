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
#include "privileged.h"

#include <stdlib.h>
#include <gio/gio.h>


#define SFMF_DBUS_NAME "org.sailfishos.slipstream.unpack"
#define SFMF_DBUS_INTERFACE "org.sailfishos.slipstream.unpack"
#define SFMF_DBUS_PATH "/"


// Private global variables
static struct SFMF_Control_Private {
    GDBusConnection *connection;
    guint own_name;
    gboolean name_acquired;
    guint object_registration;
    struct SFMF_Control_Callbacks *callbacks;
    void *callbacks_user_data;
    struct {
        char *target;
        int progress;
        char *phase;
    } progress;
} g;


static void sfmf_control_bus_acquired_cb(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    struct SFMF_Control_Private *priv = user_data;

    SFMF_DEBUG("Bus acquired with name '%s'\n", name);
    priv->connection = g_object_ref(connection);
}

static void sfmf_control_name_acquired_cb(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    struct SFMF_Control_Private *priv = user_data;

    SFMF_DEBUG("Name acquired: '%s'\n", name);
    priv->name_acquired = TRUE;
}

static void sfmf_control_name_lost_cb(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    struct SFMF_Control_Private *priv = user_data;

    if (!connection) {
        SFMF_FAIL("Could not establish D-Bus connection\n");
    } else {
        SFMF_FAIL("D-Bus name lost: '%s'\n", name);
    }

    priv->name_acquired = FALSE;
}

static void sfmf_control_method_call_cb(GDBusConnection *connection, const gchar *sender,
        const gchar *object_path, const gchar *interface_name, const gchar *method_name,
        GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data)
{
    struct SFMF_Control_Private *priv = user_data;

    if ((g_strcmp0(object_path, SFMF_DBUS_PATH) == 0 &&
         g_strcmp0(interface_name, SFMF_DBUS_INTERFACE) == 0 &&
         sfmf_dbus_is_privileged(connection, sender))) {
        if (g_strcmp0(method_name, "Abort") == 0) {
            gboolean result = FALSE;
            if (priv->callbacks && priv->callbacks->abort) {
                result = priv->callbacks->abort(priv->callbacks_user_data);
            }
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", result));
            return;
        }

        if (g_strcmp0(method_name, "GetProgress") == 0) {
            g_dbus_method_invocation_return_value(invocation,
                    g_variant_new("(sis)", g.progress.target ?: "", g.progress.progress,
                        g.progress.phase ?: ""));
            return;
        }
    }

    g_dbus_method_invocation_return_dbus_error(invocation, SFMF_DBUS_INTERFACE ".MethodCallError",
            "Invalid method call");
}

static const GDBusInterfaceVTable sfmf_control_vtable = {
    sfmf_control_method_call_cb,
    NULL,
    NULL,
};

void sfmf_control_init(struct SFMF_Control_Callbacks *callbacks, void *user_data)
{
    g.callbacks = callbacks;
    g.callbacks_user_data = user_data;

    g.own_name = g_bus_own_name(G_BUS_TYPE_SYSTEM, SFMF_DBUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
            sfmf_control_bus_acquired_cb, sfmf_control_name_acquired_cb, sfmf_control_name_lost_cb,
            &g, NULL);

    while (!g.name_acquired) {
        sfmf_control_process();
    }

    GError *error = NULL;
    GDBusNodeInfo *node_info = g_dbus_node_info_new_for_xml(""
        "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
        "  \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
        "<node name=\"" SFMF_DBUS_PATH "\">\n"
        "  <interface name=\"" SFMF_DBUS_INTERFACE "\">\n"
        "    <method name=\"Abort\">\n"
        "      <arg name=\"result\" type=\"b\" direction=\"out\" />\n"
        "    </method>\n"
        "    <method name=\"GetProgress\">\n"
        "      <arg name=\"subvolume\" type=\"s\" direction=\"out\" />\n"
        "      <arg name=\"progress\" type=\"i\" direction=\"out\" />\n"
        "      <arg name=\"phase\" type=\"s\" direction=\"out\" />\n"
        "    </method>\n"
        "    <signal name=\"Progress\">\n"
        "      <arg name=\"subvolume\" type=\"s\" />\n"
        "      <arg name=\"progress\" type=\"i\" />\n"
        "      <arg name=\"phase\" type=\"s\" />\n"
        "    </signal>\n"
        "  </interface>\n"
        "</node>\n"
    "", &error);
    if (!node_info) {
        SFMF_FAIL("Could not compile D-Bus XML: %s\n", error->message);
        g_error_free(error);
    } else {
        g.object_registration = g_dbus_connection_register_object(g.connection, "/",
                g_dbus_node_info_lookup_interface(node_info, SFMF_DBUS_INTERFACE),
                &sfmf_control_vtable, &g, NULL, &error);
    }

    if (!g.object_registration) {
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

void sfmf_control_set_progress(const char *target, int progress, const char *phase)
{
    GError *error = NULL;

    if (g.progress.target) {
        g_free(g.progress.target), g.progress.target = NULL;
    }
    if (target) {
        g.progress.target = g_strdup(target);
    }

    g.progress.progress = progress;

    if (phase && g.progress.phase) {
        g_free(g.progress.phase), g.progress.phase = NULL;
    }
    if (phase) {
        g.progress.phase = g_strdup(phase);
    }

    if (g.connection && !g_dbus_connection_emit_signal(g.connection, NULL,
          SFMF_DBUS_PATH, SFMF_DBUS_INTERFACE, "Progress",
                g_variant_new("(sis)", g.progress.target ?: "", g.progress.progress,
                    g.progress.phase ?: ""), &error)) {
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

    if (g.object_registration != 0) {
        g_dbus_connection_unregister_object(g.connection, g.object_registration), g.object_registration = 0;
    }

    if (g.own_name != 0) {
        g_bus_unown_name(g.own_name), g.own_name = 0;
    }

    if (g.connection) {
        g_object_unref(g.connection), g.connection = NULL;
    }

    g.callbacks = NULL;
    g.callbacks_user_data = NULL;

    if (g.progress.target) {
        g_free(g.progress.target), g.progress.target = NULL;
    }
    g.progress.progress = 0;
    if (g.progress.phase) {
        g_free(g.progress.phase), g.progress.phase = NULL;
    }
}
