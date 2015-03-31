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

#include <unistd.h>
#include <stdlib.h>

#include <glib.h>
#include <gio/gio.h>

#define SFMF_DEPLOY "/usr/bin/sfmf-deploy"
#define SAILFISH_SNAPSHOT "/usr/bin/sailfish-snapshot"

#define FACTORY_NAME "factory"
#define FACTORY_RENAME "factory-old"
#define SNAPSHOT_NAME "factory-slipstream-tmp"


static gchar *cmd_list[] = { SAILFISH_SNAPSHOT, "list", NULL };
static gchar *cmd_delete_snapshot[] = { SAILFISH_SNAPSHOT, "delete", SNAPSHOT_NAME, NULL };
static gchar *cmd_deploy[] = { SAILFISH_SNAPSHOT, "deploy", SFMF_DEPLOY, SNAPSHOT_NAME, NULL };
static gchar *cmd_delete_factory_rename[] = { SAILFISH_SNAPSHOT, "delete", FACTORY_RENAME, NULL };
static gchar *cmd_rename_factory[] = { SAILFISH_SNAPSHOT, "rename", FACTORY_NAME, FACTORY_RENAME, NULL };
static gchar *cmd_rename_snapshot[] = { SAILFISH_SNAPSHOT, "rename", SNAPSHOT_NAME, FACTORY_NAME, NULL };


struct DeployTask {
    gchar **cmd;
    int checked;
};

void deploy_task_handle_exit_status(struct DeployTask *task, int exit_status);


struct DeployTaskList {
    const char *name;
    struct DeployTask *tasks;
    int current;
    int aborted;
    void (*finished_callback)(struct DeployTaskList *); // callback when everything is done
    void (*task_done_callback)(struct DeployTaskList *); // if not-NULL, use async spawn; called after every task
    GMainLoop *mainloop;
};

int deploy_task_list_next(struct DeployTaskList *queue);
void deploy_task_list_done(struct DeployTaskList *queue);
void deploy_task_list_abort(struct DeployTaskList *queue);

void deploy_task_handle_exit_status(struct DeployTask *task, int exit_status)
{
    GError *error = NULL;
    if (g_spawn_check_exit_status(exit_status, &error)) {
        SFMF_DEBUG("Success\n");
    } else {
        if (task->checked) {
            SFMF_FAIL("Failed to run command: %s\n", error->message);
        } else {
            SFMF_WARN("Failure (ignored): %s\n", error->message);
        }
        g_error_free(error);
    }
}

static void run_sync(struct DeployTaskList *queue)
{
    struct DeployTask *task = &(queue->tasks[queue->current]);

    gint exit_status = 0;
    GError *error = NULL;
    if (g_spawn_sync(NULL, task->cmd, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &exit_status, &error)) {
        deploy_task_handle_exit_status(task, exit_status);
    } else {
        SFMF_FAIL("Failed to run command: %s\n", error->message);
        g_error_free(error);
    }
}

static void on_subprocess_finished(GPid pid, gint status, gpointer user_data)
{
    struct DeployTaskList *queue = user_data;
    struct DeployTask *task = &(queue->tasks[queue->current]);

    g_spawn_close_pid(pid);
    deploy_task_handle_exit_status(task, status);

    queue->task_done_callback(queue);
}

static void run_async(struct DeployTaskList *queue)
{
    struct DeployTask *task = &(queue->tasks[queue->current]);

    GPid pid = 0;
    GError *error = NULL;
    if (!g_spawn_async(NULL, task->cmd, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, queue, &pid, &error)) {
        SFMF_FAIL("Failed to run command: %s\n", error->message);
        g_error_free(error);
    }

    g_child_watch_add(pid, on_subprocess_finished, queue);
}

void upgrade_factory_snapshot_cleanup(void *user_data)
{
    struct DeployTaskList *queue = (struct DeployTaskList *)user_data;
    // Make sure the main tasks are not run anymore
    deploy_task_list_abort(queue);

    static struct DeployTask cleanup_tasks[] = {
        { cmd_delete_snapshot, 0 },
        { cmd_list, 0 },
        { NULL, 0 },
    };
    static struct DeployTaskList cleanup_queue = { "cleanup", cleanup_tasks, -1, 0, NULL, NULL, NULL };

    // Run all steps until we're finished
    while (deploy_task_list_next(&cleanup_queue));
}

int deploy_task_list_next(struct DeployTaskList *queue)
{
    int result = 0;
    if (!queue->aborted) {
        queue->current++;

        SFMF_DEBUG("NEXT: %s (#%d)\n", queue->name, queue->current);

        struct DeployTask *task = &(queue->tasks[queue->current]);
        if (task->cmd) {
            gchar *cmds = g_strjoinv(" ", task->cmd);
            SFMF_DEBUG("Running command (checked=%d): '%s'\n", task->checked, cmds);
            g_free(cmds);

            if (queue->task_done_callback) {
                // Can run this asynchronously
                run_async(queue);
            } else {
                // Must run this synchronously
                run_sync(queue);
            }

            result = 1;
        } else {
            deploy_task_list_done(queue);
        }
    } else {
        deploy_task_list_done(queue);
    }

    return result;
}

void deploy_task_list_done(struct DeployTaskList *queue)
{
    SFMF_LOG("Queue done: %s\n", queue->name);

    if (queue->finished_callback && !queue->aborted) {
        queue->finished_callback(queue);
    }
}

void deploy_task_list_abort(struct DeployTaskList *queue)
{
    SFMF_LOG("Aborting queue: %s\n", queue->name);
    queue->aborted = 1;
}

gboolean do_next_entry(gpointer user_data)
{
    struct DeployTaskList *queue = user_data;
    deploy_task_list_next(queue);
    return FALSE;
}

gboolean every_second(gpointer user_data)
{
    struct DeployTaskList *queue = user_data;
    printf("Watch, queue position = %d, aborted = %d\n", queue->current, queue->current);
    return TRUE;
}

void on_finished_main(struct DeployTaskList *queue)
{
    if (queue->mainloop) {
        g_main_loop_quit(queue->mainloop);
    }
}

void on_task_done(struct DeployTaskList *queue)
{
    g_idle_add(do_next_entry, queue);
}

void on_dbus_signal(GDBusConnection *connection, const gchar *sender_name,
        const gchar *object_path, const gchar *interface_name, const gchar *signal_name,
        GVariant *parameters, gpointer user_data)
{
    gchar *args = g_variant_print(parameters, TRUE);
    SFMF_LOG("on_dbus_signal: sender=%s path=%s signal=%s.%s%s\n", sender_name, object_path,
            interface_name, signal_name, args);
    g_free(args);
}

int main(int argc, char *argv[])
{
    GMainLoop *mainloop = g_main_loop_new(NULL, FALSE);

    static struct DeployTask deploy_tasks[] = {
        { cmd_list, 1 },
        { cmd_delete_snapshot, 0 },
        { cmd_deploy, 1 },
        { cmd_delete_factory_rename, 0 },
        { cmd_rename_factory, 1 },
        { cmd_rename_snapshot, 1 },
        { cmd_delete_factory_rename, 0 },
        { cmd_list, 0 },
        { NULL, 0 },
    };
    static struct DeployTaskList deploy_queue = { "deploy", deploy_tasks, -1, 0, on_finished_main, on_task_done, NULL };

    deploy_queue.mainloop = mainloop;

    sfmf_cleanup_register(upgrade_factory_snapshot_cleanup, &deploy_queue);

    g_idle_add(do_next_entry, &deploy_queue);

    //g_timeout_add(1000, every_second, &deploy_queue);

    GError *error = NULL;
    GDBusConnection *system_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!system_bus) {
        SFMF_FAIL("Could not connect to system bus: %s\n", error->message);
        g_error_free(error);
    }

    g_dbus_connection_signal_subscribe(system_bus, "org.sailfishos.sfmf.unpack",
            "org.sailfishos.sfmf.unpack", NULL, "/", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
            on_dbus_signal, &deploy_queue, NULL);

    SFMF_LOG("Deploying snapshot...\n");
    g_main_loop_run(mainloop);
    g_main_loop_unref(mainloop);
    SFMF_LOG("Done.\n");

    g_object_unref(system_bus);

    return 0;
}
