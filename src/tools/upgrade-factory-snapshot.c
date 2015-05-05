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

#include "logging.h"
#include "cleanup.h"
#include "privileged.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <gio/gio.h>

#define SFMF_DEPLOY "/usr/bin/sfmf-deploy"
#define SAILFISH_SNAPSHOT "/usr/bin/sailfish-snapshot"

#define FACTORY_NAME "factory"
#define FACTORY_RENAME "factory-old"
#define SNAPSHOT_NAME "factory-slipstream-tmp"

#define UFS_DBUS_NAME "org.sailfishos.slipstream.upgrade"
#define UFS_DBUS_INTERFACE "org.sailfishos.slipstream.upgrade"
#define UFS_DBUS_PATH "/"

#define IDLE_TIMEOUT_SEC 60


struct DeployTask {
    gchar *name;
    gchar **cmd;
    int checked;
};

void deploy_task_handle_exit_status(struct DeployTask *task, int exit_status);


struct DeployTaskQueue {
    const char *name;

    struct DeployTask *tasks;
    int current;
    int total;
    int finished;

    void (*finished_callback)(struct DeployTaskQueue *, void *); // callback when everything is done
    void (*task_done_callback)(struct DeployTaskQueue *, void *); // callback when one task is done
    void *callback_user_data; // user_data pointer passed to finished_callback and task_done_callback
};

int deploy_task_queue_next(struct DeployTaskQueue *queue, int async);
void deploy_task_queue_abort(struct DeployTaskQueue *queue);
struct DeployTask *deploy_task_queue_get_current_task(struct DeployTaskQueue *queue);
void deploy_task_queue_run_sync(struct DeployTaskQueue *queue);
void deploy_task_queue_run_async(struct DeployTaskQueue *queue);

// Callbacks
void deploy_task_queue_on_subprocess_finished_cb(GPid pid, gint status, gpointer user_data);


struct UpgradeFactorySnapshot {
    GMainLoop *mainloop;
    GDBusConnection *system_bus;
    guint own_name;
    guint object_registration;
    guint idle_timer_source_id;

    gboolean running;

    struct DeployTaskQueue *deploy_queue;
    struct DeployTaskQueue *cleanup_queue;

    gchar **partitions;
    int partitions_length;

    struct {
        gchar *partition;
        int progress;
        gchar *message;
        int partition_current;
        int partition_total;
    } status;
};

void upgrade_factory_snapshot_init(struct UpgradeFactorySnapshot *ufs);
void upgrade_factory_snapshot_run(struct UpgradeFactorySnapshot *ufs);
void upgrade_factory_snapshot_quit(struct UpgradeFactorySnapshot *ufs);
void upgrade_factory_snapshot_broadcast_status(struct UpgradeFactorySnapshot *ufs,
        const char *partition, int progress, const char *message);
void upgrade_factory_snapshot_schedule_quit(struct UpgradeFactorySnapshot *ufs);
void upgrade_factory_snapshot_set_running(struct UpgradeFactorySnapshot *ufs, gboolean running);

// Callbacks
void upgrade_factory_snapshot_bus_acquired_cb(GDBusConnection *connection, const gchar *name, gpointer user_data);
void upgrade_factory_snapshot_name_acquired_cb(GDBusConnection *connection, const gchar *name, gpointer user_data);
void upgrade_factory_snapshot_name_lost_cb(GDBusConnection *connection, const gchar *name, gpointer user_data);
void upgrade_factory_snapshot_cleanup_cb(void *user_data);
gboolean upgrade_factory_snapshot_do_next_entry_cb(gpointer user_data);
gboolean upgrade_factory_snapshot_delayed_finish_cb(gpointer user_data);
void upgrade_factory_snapshot_finished_cb(struct DeployTaskQueue *queue, void *user_data);
void upgrade_factory_snapshot_task_done_cb(struct DeployTaskQueue *queue, void *user_data);
void upgrade_factory_snapshot_dbus_signal_cb(GDBusConnection *connection,
        const gchar *sender_name, const gchar *object_path, const gchar *interface_name,
        const gchar *signal_name, GVariant *parameters, gpointer user_data);
void upgrade_factory_snapshot_method_call_cb(GDBusConnection *connection, const gchar *sender,
        const gchar *object_path, const gchar *interface_name, const gchar *method_name,
        GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data);
GVariant *upgrade_factory_snapshot_get_property_cb(GDBusConnection *connection, const gchar *sender,
        const gchar *object_path, const gchar *interface_name, const gchar *property_name,
        GError **error, gpointer user_data);
gboolean upgrade_factory_snapshot_set_property_cb(GDBusConnection *connection, const gchar *sender,
        const gchar *object_path, const gchar *interface_name, const gchar *property_name,
        GVariant *value, GError **error, gpointer user_data);


static gchar *cmd_list[] = { SAILFISH_SNAPSHOT, "list", NULL };
static gchar *cmd_delete_snapshot[] = { SAILFISH_SNAPSHOT, "delete", SNAPSHOT_NAME, NULL };
static gchar *cmd_deploy[] = { SAILFISH_SNAPSHOT, "deploy", SFMF_DEPLOY, SNAPSHOT_NAME, NULL };
static gchar *cmd_delete_factory_rename[] = { SAILFISH_SNAPSHOT, "delete", FACTORY_RENAME, NULL };
static gchar *cmd_rename_factory[] = { SAILFISH_SNAPSHOT, "rename", FACTORY_NAME, FACTORY_RENAME, NULL };
static gchar *cmd_rename_snapshot[] = { SAILFISH_SNAPSHOT, "rename", SNAPSHOT_NAME, FACTORY_NAME, NULL };
static gchar *sbj_partitions[] = { "@", "@home", NULL };

static struct DeployTask g_deploy_tasks[] = {
    { "Checking for existing snapshots", cmd_list, 1 },
    { "Removing temporary snapshot", cmd_delete_snapshot, 0 },
    { "Deploying new factory snapshot", cmd_deploy, 1 },
    { "Removing renamed factory snapshot", cmd_delete_factory_rename, 0 },
    { "Renaming factory snapshot", cmd_rename_factory, 1 },
    { "Activating new factory snapshot", cmd_rename_snapshot, 1 },
    { "Removing renamed factory snapshot", cmd_delete_factory_rename, 0 },
    { "Enumerating snapshots", cmd_list, 0 },
    { NULL, NULL, 0 },
};

static struct DeployTaskQueue g_deploy_queue = {
    "deploy",

    g_deploy_tasks,
    -1,
    -1,
    0,

    upgrade_factory_snapshot_finished_cb,
    upgrade_factory_snapshot_task_done_cb,
    NULL,
};

static struct DeployTask g_cleanup_tasks[] = {
    { "Deleting temporary snapshot", cmd_delete_snapshot, 0 },
    { "Enumerating snapshots", cmd_list, 0 },
    { NULL, NULL, 0 },
};

static struct DeployTaskQueue g_cleanup_queue = {
    "cleanup",

    g_cleanup_tasks,
    -1,
    -1,
    0,

    NULL,
    NULL,
    NULL,
};

static struct UpgradeFactorySnapshot g_ufs = {
    NULL,
    NULL,
    0,
    0,
    0,

    FALSE,

    &g_deploy_queue,
    &g_cleanup_queue,

    sbj_partitions,
    -1,

    { NULL, 0, NULL, 0, 0 },
};

static const GDBusInterfaceVTable upgrade_factory_snapshot_vtable = {
    upgrade_factory_snapshot_method_call_cb,
    upgrade_factory_snapshot_get_property_cb,
    upgrade_factory_snapshot_set_property_cb,
};


void deploy_task_handle_exit_status(struct DeployTask *task, int exit_status)
{
    GError *error = NULL;
    if (!g_spawn_check_exit_status(exit_status, &error)) {
        if (task->checked) {
            SFMF_FAIL_AND_EXIT("Failed to run command: %s\n", error->message);
        } else {
            SFMF_WARN("Failure (ignored): %s\n", error->message);
        }
        g_error_free(error);
    }
}

struct DeployTask *deploy_task_queue_get_current_task(struct DeployTaskQueue *queue)
{
    if (queue->current >= 0) {
        return &(queue->tasks[queue->current]);
    }

    return NULL;
}

void deploy_task_queue_run_sync(struct DeployTaskQueue *queue)
{
    struct DeployTask *task = deploy_task_queue_get_current_task(queue);

    if (!task) {
        SFMF_FAIL_AND_EXIT("Current task is invalid\n");
    }

    gint exit_status = 0;
    GError *error = NULL;
    if (g_spawn_sync(NULL, task->cmd, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &exit_status, &error)) {
        deploy_task_handle_exit_status(task, exit_status);
    } else {
        SFMF_FAIL_AND_EXIT("Failed to run command: %s\n", error->message);
        g_error_free(error);
    }
}

void deploy_task_queue_on_subprocess_finished_cb(GPid pid, gint status, gpointer user_data)
{
    struct DeployTaskQueue *queue = user_data;
    struct DeployTask *task = deploy_task_queue_get_current_task(queue);

    if (!task) {
        SFMF_FAIL_AND_EXIT("Current task is invalid\n");
    }

    g_spawn_close_pid(pid);
    deploy_task_handle_exit_status(task, status);

    struct UpgradeFactorySnapshot *ufs = queue->callback_user_data;
    upgrade_factory_snapshot_broadcast_status(ufs, "", 100, "Finishing");

    if (queue->task_done_callback) {
        queue->task_done_callback(queue, queue->callback_user_data);
    }
}

void deploy_task_queue_run_async(struct DeployTaskQueue *queue)
{
    struct DeployTask *task = deploy_task_queue_get_current_task(queue);

    if (!task) {
        SFMF_FAIL_AND_EXIT("Current task is invalid\n");
    }

    struct UpgradeFactorySnapshot *ufs = queue->callback_user_data;
    upgrade_factory_snapshot_broadcast_status(ufs, "", 0, "Starting");

    GPid pid = 0;
    GError *error = NULL;
    if (!g_spawn_async(NULL, task->cmd, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, queue, &pid, &error)) {
        SFMF_FAIL_AND_EXIT("Failed to run command: %s\n", error->message);
        g_error_free(error);
    }

    g_child_watch_add(pid, deploy_task_queue_on_subprocess_finished_cb, queue);
}

void upgrade_factory_snapshot_cleanup_cb(void *user_data)
{
    struct UpgradeFactorySnapshot *ufs = user_data;

    SFMF_LOG("Running cleanup...\n");

    // Make sure the main tasks are not run anymore
    deploy_task_queue_abort(ufs->deploy_queue);

    // Run all steps synchronously until we're finished
    while (deploy_task_queue_next(ufs->cleanup_queue, 0));

    SFMF_LOG("Cleanup completed.\n");
}

int deploy_task_queue_next(struct DeployTaskQueue *queue, int async)
{
    int result = 0;

    if (!queue->finished) {
        queue->current++;

        struct DeployTask *task = deploy_task_queue_get_current_task(queue);

        if (!task) {
            SFMF_FAIL_AND_EXIT("Current task is invalid\n");
        }

        if (task->cmd) {
            gchar *cmds = g_strjoinv(" ", task->cmd);
            SFMF_DEBUG("Running '%s' (queue=%s, pos=%d/%d, checked=%d): '%s'\n",
                    task->name, queue->name, queue->current, queue->total, task->checked, cmds);
            g_free(cmds);

            if (async) {
                deploy_task_queue_run_async(queue);
            } else {
                deploy_task_queue_run_sync(queue);
            }

            result = 1;
        } else {
            if (queue->finished_callback) {
                queue->finished_callback(queue, queue->callback_user_data);
            }

            queue->finished = 1;
        }
    }

    return result;
}

void deploy_task_queue_abort(struct DeployTaskQueue *queue)
{
    SFMF_LOG("Aborting queue: %s\n", queue->name);
    queue->finished = 1;
}

gboolean upgrade_factory_snapshot_do_next_entry_cb(gpointer user_data)
{
    struct UpgradeFactorySnapshot *ufs = user_data;
    deploy_task_queue_next(ufs->deploy_queue, 1);
    return FALSE;
}

gboolean upgrade_factory_snapshot_delayed_finish_cb(gpointer user_data)
{
    struct UpgradeFactorySnapshot *ufs = user_data;
    upgrade_factory_snapshot_set_running(ufs, FALSE);
    upgrade_factory_snapshot_schedule_quit(ufs);
    return FALSE;
}

void upgrade_factory_snapshot_finished_cb(struct DeployTaskQueue *queue, void *user_data)
{
    struct UpgradeFactorySnapshot *ufs = user_data;

    // Reset queue in case we need to re-start it later
    queue->current = -1;
    g_timeout_add(1000, upgrade_factory_snapshot_delayed_finish_cb, ufs);
}

void upgrade_factory_snapshot_task_done_cb(struct DeployTaskQueue *queue, void *user_data)
{
    struct UpgradeFactorySnapshot *ufs = user_data;
    g_idle_add(upgrade_factory_snapshot_do_next_entry_cb, ufs);
}

void upgrade_factory_snapshot_broadcast_status(struct UpgradeFactorySnapshot *ufs,
        const char *partition, int progress, const char *message)
{
    struct DeployTaskQueue *queue = ufs->deploy_queue;
    struct DeployTask *task = deploy_task_queue_get_current_task(queue);

    if (!task) {
        SFMF_FAIL_AND_EXIT("Current task is invalid\n");
    }

    if (ufs->status.partition) {
        g_free(ufs->status.partition);
    }
    ufs->status.partition = g_strdup(partition);

    ufs->status.progress = progress;

    if (ufs->status.message) {
        g_free(ufs->status.message);
    }
    ufs->status.message = g_strdup(message);

    int partition_current = 0;
    int partition_total = ufs->partitions_length;
    while (partition_current < partition_total) {
        if (strcmp(partition, ufs->partitions[partition_current]) == 0) {
            break;
        }
        partition_current++;
    }

    if (partition_current == partition_total) {
        if (strcmp(partition, "") != 0) {
            SFMF_WARN("Unknown partition: %s\n", partition);
        }
        partition_current = 0;
        partition_total = 1;
    }

    ufs->status.partition_current = partition_current;
    ufs->status.partition_total = partition_total;

    SFMF_LOG("queue: %s, task='%s' (%d/%d), partition='%s' (%d/%d), message='%s' (%d%%)\n",
            queue->name, task->name, queue->current+1, queue->total,
            partition, partition_current+1, partition_total,
            message, progress);

    GError *error = NULL;
    if (!g_dbus_connection_emit_signal(ufs->system_bus, NULL, UFS_DBUS_PATH, UFS_DBUS_INTERFACE, "ProgressChanged",
                g_variant_new("()"), &error)) {
        SFMF_WARN("Could not forward progress via D-Bus: %s\n", error->message);
        g_error_free(error);
    }
}

gboolean upgrade_factory_snapshot_idle_timeout_cb(gpointer user_data)
{
    struct UpgradeFactorySnapshot *ufs = user_data;

    if (!ufs->running) {
        SFMF_DEBUG("Idle timeout reached, quitting\n");
        g_main_loop_quit(ufs->mainloop);
    }

    return FALSE;
}

void upgrade_factory_snapshot_schedule_quit(struct UpgradeFactorySnapshot *ufs)
{
    if (ufs->idle_timer_source_id) {
        g_source_remove(ufs->idle_timer_source_id);
        ufs->idle_timer_source_id = 0;
    }

    SFMF_DEBUG("(Re-)Starting idle timer (%d seconds)\n", IDLE_TIMEOUT_SEC);
    ufs->idle_timer_source_id = g_timeout_add_seconds(IDLE_TIMEOUT_SEC,
            upgrade_factory_snapshot_idle_timeout_cb, ufs);
}

void upgrade_factory_snapshot_set_running(struct UpgradeFactorySnapshot *ufs, gboolean running)
{
    if (ufs->running != running) {
        GError *error = NULL;

        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE_ARRAY);
        g_variant_builder_add(&builder, "{sv}", "running", g_variant_new_boolean(running));

        if (!g_dbus_connection_emit_signal(ufs->system_bus, NULL, UFS_DBUS_PATH,
                    "org.freedesktop.DBus.Properties", "PropertiesChanged",
                    g_variant_new("(sa{sv}as)", UFS_DBUS_INTERFACE, &builder, NULL), &error)) {
            SFMF_WARN("Could not emit properties changed signal: %s\n", error->message);
            g_error_free(error);
        }

        ufs->running = running;
    }
}

void upgrade_factory_snapshot_dbus_signal_cb(GDBusConnection *connection,
        const gchar *sender_name, const gchar *object_path, const gchar *interface_name,
        const gchar *signal_name, GVariant *parameters, gpointer user_data)
{
    struct UpgradeFactorySnapshot *ufs = user_data;

    if (strcmp(signal_name, "Progress") == 0) {
        gchar *partition = NULL;
        int progress = 0;
        gchar *message = NULL;
        g_variant_get(parameters, "(sis)", &partition, &progress, &message);

        upgrade_factory_snapshot_broadcast_status(ufs, partition ?: "", progress, message ?: "");

        g_free(partition);
        g_free(message);
    } else {
        SFMF_WARN("Unhandled D-Bus signal: '%s'\n", signal_name);
    }
}

void upgrade_factory_snapshot_method_call_cb(GDBusConnection *connection, const gchar *sender,
        const gchar *object_path, const gchar *interface_name, const gchar *method_name,
        GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data)
{
    struct UpgradeFactorySnapshot *ufs = user_data;
    GVariant *return_value = NULL;

    if (g_strcmp0(object_path, UFS_DBUS_PATH) == 0 &&
            g_strcmp0(interface_name, UFS_DBUS_INTERFACE) == 0 &&
            sfmf_dbus_is_privileged(connection, sender)) {
        if (g_strcmp0(method_name, "Start") == 0) {
            gboolean result = FALSE;

            if (!ufs->running) {
                gchar *release = 0;
                g_variant_get(parameters, "(s)", &release);
                if (release && strlen(release)) {
                    setenv("SSU_SLIPSTREAM_RELEASE", release, 1);
                }
                g_free(release);

                g_idle_add(upgrade_factory_snapshot_do_next_entry_cb, ufs);
                upgrade_factory_snapshot_set_running(ufs, TRUE);
                result = TRUE;
            }

            return_value = g_variant_new("(b)", result);
        } else if (g_strcmp0(method_name, "GetProgress") == 0) {
            struct DeployTaskQueue *queue = ufs->deploy_queue;
            struct DeployTask *task = deploy_task_queue_get_current_task(queue);

            return_value = g_variant_new("(ssiisiisi)", queue->name,
                    task ? task->name : "", queue->current+1, queue->total,
                    ufs->status.partition ?: "", ufs->status.partition_current+1, ufs->status.partition_total,
                    ufs->status.message ?: "", ufs->status.progress);
        }
    }

    if (return_value) {
        g_dbus_method_invocation_return_value(invocation, return_value);
    } else {
        g_dbus_method_invocation_return_dbus_error(invocation, UFS_DBUS_INTERFACE ".MethodCallError",
                "Invalid method call");
    }

    // After each D-Bus method call, reschedule the idle timer
    upgrade_factory_snapshot_schedule_quit(ufs);
}

GVariant *upgrade_factory_snapshot_get_property_cb(GDBusConnection *connection, const gchar *sender,
        const gchar *object_path, const gchar *interface_name, const gchar *property_name,
        GError **error, gpointer user_data)
{
    struct UpgradeFactorySnapshot *ufs = user_data;

    if (g_strcmp0(object_path, UFS_DBUS_PATH) == 0 &&
            g_strcmp0(interface_name, UFS_DBUS_INTERFACE) == 0 &&
            g_strcmp0(property_name, "running") == 0) {
        return g_variant_new("b", ufs->running);
    }

    // TODO: Use G_DBUS_ERROR_UNKNOWN_PROPERTY once we get glib >= 2.41.0
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Cannot get property '%s'", property_name);
    return NULL;
}

gboolean upgrade_factory_snapshot_set_property_cb(GDBusConnection *connection, const gchar *sender,
        const gchar *object_path, const gchar *interface_name, const gchar *property_name,
        GVariant *value, GError **error, gpointer user_data)
{
    struct UpgradeFactorySnapshot *ufs = user_data;

    // TODO: Use G_DBUS_ERROR_PROPERTY_READ_ONLY and G_DBUS_ERROR_UNKNOWN_PROPERTY once we
    // upgrade to glib >= 2.41.0 that has these new defines (added in glib commit 76d6fd0)
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Cannot set property '%s'", property_name);

    return FALSE;
}

void upgrade_factory_snapshot_bus_acquired_cb(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    struct UpgradeFactorySnapshot *ufs = user_data;

    ufs->system_bus = g_object_ref(connection);

    GError *error = NULL;
    GDBusNodeInfo *node_info = g_dbus_node_info_new_for_xml(""
        "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
        "  \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
        "<node name=\"" UFS_DBUS_PATH "\">\n"
        "  <interface name=\"" UFS_DBUS_INTERFACE "\">\n"
        "    <method name=\"Start\">\n"
        "      <arg name=\"release\" type=\"s\" direction=\"in\" />\n"
        "      <arg name=\"result\" type=\"b\" direction=\"out\" />\n"
        "    </method>\n"
        "    <method name=\"GetProgress\">\n"
        "      <arg name=\"queue_name\" type=\"s\" direction=\"out\" />\n"
        "      <arg name=\"task_name\" type=\"s\" direction=\"out\" />\n"
        "      <arg name=\"queue_pos\" type=\"i\" direction=\"out\" />\n"
        "      <arg name=\"queue_total\" type=\"i\" direction=\"out\" />\n"
        "      <arg name=\"partition_name\" type=\"s\" direction=\"out\" />\n"
        "      <arg name=\"partition_pos\" type=\"i\" direction=\"out\" />\n"
        "      <arg name=\"partition_total\" type=\"i\" direction=\"out\" />\n"
        "      <arg name=\"message\" type=\"s\" direction=\"out\" />\n"
        "      <arg name=\"progress\" type=\"i\" direction=\"out\" />\n"
        "    </method>\n"
        "    <signal name=\"ProgressChanged\">\n"
        "    </signal>\n"
        "    <property name=\"running\" type=\"b\" access=\"read\">\n"
        "    </property>\n"
        "  </interface>\n"
        "</node>\n"
    "", &error);

    if (!node_info) {
        SFMF_FAIL_AND_EXIT("Could not compile D-Bus XML: %s\n", error->message);
        g_error_free(error);
    }

    ufs->object_registration = g_dbus_connection_register_object(ufs->system_bus, UFS_DBUS_PATH,
            g_dbus_node_info_lookup_interface(node_info, UFS_DBUS_INTERFACE),
            &upgrade_factory_snapshot_vtable, ufs, NULL, &error);

    if (!ufs->object_registration) {
        SFMF_FAIL_AND_EXIT("Could not register object on D-Bus: %s\n", error->message);
        g_error_free(error);
    }

    g_dbus_node_info_unref(node_info);
}

void upgrade_factory_snapshot_name_acquired_cb(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    struct UpgradeFactorySnapshot *ufs = user_data;

    // Connect to D-Bus signals of the unpack utility
    g_dbus_connection_signal_subscribe(ufs->system_bus, "org.sailfishos.slipstream.unpack",
            "org.sailfishos.slipstream.unpack", NULL, "/", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
            upgrade_factory_snapshot_dbus_signal_cb, ufs, NULL);

    // Schedule idle timer if no calls come in
    upgrade_factory_snapshot_schedule_quit(ufs);
}

void upgrade_factory_snapshot_name_lost_cb(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    struct UpgradeFactorySnapshot *ufs = user_data;

    if (!connection) {
        SFMF_FAIL_AND_EXIT("Could not establish D-Bus connection\n");
    } else {
        SFMF_FAIL_AND_EXIT("D-Bus name lost: '%s'\n", name);
    }
}

void upgrade_factory_snapshot_init(struct UpgradeFactorySnapshot *ufs)
{
    sfmf_cleanup_register(upgrade_factory_snapshot_cleanup_cb, ufs);

    ufs->mainloop = g_main_loop_new(NULL, FALSE);
    ufs->deploy_queue->callback_user_data = ufs;
    ufs->cleanup_queue->callback_user_data = ufs;

    for (ufs->deploy_queue->total=0;
         ufs->deploy_queue->tasks[ufs->deploy_queue->total].cmd;
         ufs->deploy_queue->total++);

    for (ufs->cleanup_queue->total=0;
         ufs->cleanup_queue->tasks[ufs->cleanup_queue->total].cmd;
         ufs->cleanup_queue->total++);

    for (ufs->partitions_length=0;
         ufs->partitions[ufs->partitions_length];
         ufs->partitions_length++);

    SFMF_DEBUG("Deploy tasks: %d, cleanup tasks: %d, partitions: %d\n",
            ufs->deploy_queue->total, ufs->cleanup_queue->total, ufs->partitions_length);

    ufs->own_name = g_bus_own_name(G_BUS_TYPE_SYSTEM, UFS_DBUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
            upgrade_factory_snapshot_bus_acquired_cb,
            upgrade_factory_snapshot_name_acquired_cb,
            upgrade_factory_snapshot_name_lost_cb,
            ufs, NULL);
}

void upgrade_factory_snapshot_run(struct UpgradeFactorySnapshot *ufs)
{
    SFMF_LOG("Running mainloop...\n");
    g_main_loop_run(ufs->mainloop);
    SFMF_LOG("Main loop exited.\n");
}

void upgrade_factory_snapshot_quit(struct UpgradeFactorySnapshot *ufs)
{
    g_main_loop_unref(ufs->mainloop), ufs->mainloop = NULL;

    if (ufs->object_registration) {
        g_dbus_connection_unregister_object(ufs->system_bus, ufs->object_registration), ufs->object_registration = 0;
    }

    g_bus_unown_name(ufs->own_name);

    if (ufs->system_bus) {
        g_object_unref(ufs->system_bus), ufs->system_bus = NULL;
    }

    if (ufs->status.partition) {
        g_free(ufs->status.partition), ufs->status.partition = NULL;
    }

    if (ufs->status.message) {
        g_free(ufs->status.message), ufs->status.message = NULL;
    }
}


int main(int argc, char *argv[])
{
    struct UpgradeFactorySnapshot *ufs = &g_ufs;

    setenv("PATH", "/usr/bin:/usr/sbin:/bin:/sbin", 1);

    upgrade_factory_snapshot_init(ufs);
    upgrade_factory_snapshot_run(ufs);
    upgrade_factory_snapshot_quit(ufs);

    return 0;
}
