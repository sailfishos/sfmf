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


struct DeployTask {
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

    struct DeployTaskQueue *deploy_queue;
    struct DeployTaskQueue *cleanup_queue;

    gchar **partitions;
    int partitions_length;
};

void upgrade_factory_snapshot_init(struct UpgradeFactorySnapshot *ufs);
void upgrade_factory_snapshot_run(struct UpgradeFactorySnapshot *ufs);
void upgrade_factory_snapshot_quit(struct UpgradeFactorySnapshot *ufs);

// Callbacks
void upgrade_factory_snapshot_cleanup_cb(void *user_data);
gboolean upgrade_factory_snapshot_do_next_entry_cb(gpointer user_data);
void upgrade_factory_snapshot_finished_cb(struct DeployTaskQueue *queue, void *user_data);
void upgrade_factory_snapshot_task_done_cb(struct DeployTaskQueue *queue, void *user_data);
void upgrade_factory_snapshot_dbus_signal_cb(GDBusConnection *connection,
        const gchar *sender_name, const gchar *object_path, const gchar *interface_name,
        const gchar *signal_name, GVariant *parameters, gpointer user_data);


static gchar *cmd_list[] = { SAILFISH_SNAPSHOT, "list", NULL };
static gchar *cmd_delete_snapshot[] = { SAILFISH_SNAPSHOT, "delete", SNAPSHOT_NAME, NULL };
static gchar *cmd_deploy[] = { SAILFISH_SNAPSHOT, "deploy", SFMF_DEPLOY, SNAPSHOT_NAME, NULL };
static gchar *cmd_delete_factory_rename[] = { SAILFISH_SNAPSHOT, "delete", FACTORY_RENAME, NULL };
static gchar *cmd_rename_factory[] = { SAILFISH_SNAPSHOT, "rename", FACTORY_NAME, FACTORY_RENAME, NULL };
static gchar *cmd_rename_snapshot[] = { SAILFISH_SNAPSHOT, "rename", SNAPSHOT_NAME, FACTORY_NAME, NULL };
static gchar *sbj_partitions[] = { "@", "@home", NULL };

static struct DeployTask g_deploy_tasks[] = {
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
    { cmd_delete_snapshot, 0 },
    { cmd_list, 0 },
    { NULL, 0 },
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

    &g_deploy_queue,
    &g_cleanup_queue,

    sbj_partitions,
    -1,
};


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

struct DeployTask *deploy_task_queue_get_current_task(struct DeployTaskQueue *queue)
{
    return &(queue->tasks[queue->current]);
}

void deploy_task_queue_run_sync(struct DeployTaskQueue *queue)
{
    struct DeployTask *task = deploy_task_queue_get_current_task(queue);

    gint exit_status = 0;
    GError *error = NULL;
    if (g_spawn_sync(NULL, task->cmd, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &exit_status, &error)) {
        deploy_task_handle_exit_status(task, exit_status);
    } else {
        SFMF_FAIL("Failed to run command: %s\n", error->message);
        g_error_free(error);
    }
}

void deploy_task_queue_on_subprocess_finished_cb(GPid pid, gint status, gpointer user_data)
{
    struct DeployTaskQueue *queue = user_data;
    struct DeployTask *task = deploy_task_queue_get_current_task(queue);

    g_spawn_close_pid(pid);
    deploy_task_handle_exit_status(task, status);

    if (queue->task_done_callback) {
        queue->task_done_callback(queue, queue->callback_user_data);
    }
}

void deploy_task_queue_run_async(struct DeployTaskQueue *queue)
{
    struct DeployTask *task = deploy_task_queue_get_current_task(queue);

    GPid pid = 0;
    GError *error = NULL;
    if (!g_spawn_async(NULL, task->cmd, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, queue, &pid, &error)) {
        SFMF_FAIL("Failed to run command: %s\n", error->message);
        g_error_free(error);
    }

    g_child_watch_add(pid, deploy_task_queue_on_subprocess_finished_cb, queue);
}

void upgrade_factory_snapshot_cleanup_cb(void *user_data)
{
    struct UpgradeFactorySnapshot *ufs = user_data;

    // Make sure the main tasks are not run anymore
    deploy_task_queue_abort(ufs->deploy_queue);

    // Run all steps synchronously until we're finished
    while (deploy_task_queue_next(ufs->cleanup_queue, 0));
}

int deploy_task_queue_next(struct DeployTaskQueue *queue, int async)
{
    int result = 0;

    if (!queue->finished) {
        queue->current++;

        struct DeployTask *task = deploy_task_queue_get_current_task(queue);
        if (task->cmd) {
            gchar *cmds = g_strjoinv(" ", task->cmd);
            SFMF_DEBUG("Running command (queue=%s, pos=%d/%d, checked=%d): '%s'\n",
                    queue->name, queue->current, queue->total, task->checked, cmds);
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

void upgrade_factory_snapshot_finished_cb(struct DeployTaskQueue *queue, void *user_data)
{
    struct UpgradeFactorySnapshot *ufs = user_data;
    g_main_loop_quit(ufs->mainloop);
}

void upgrade_factory_snapshot_task_done_cb(struct DeployTaskQueue *queue, void *user_data)
{
    struct UpgradeFactorySnapshot *ufs = user_data;
    g_idle_add(upgrade_factory_snapshot_do_next_entry_cb, ufs);
}

void upgrade_factory_snapshot_dbus_signal_cb(GDBusConnection *connection,
        const gchar *sender_name, const gchar *object_path, const gchar *interface_name,
        const gchar *signal_name, GVariant *parameters, gpointer user_data)
{
    struct UpgradeFactorySnapshot *ufs = user_data;

    gchar *partition;
    int progress;
    gchar *message;
    g_variant_get(parameters, "(sis)", &partition, &progress, &message);
    gchar *args = g_variant_print(parameters, TRUE);

    int partition_pos = 0;
    while (partition_pos < ufs->partitions_length) {
        if (strcmp(partition, ufs->partitions[partition_pos]) == 0) {
            break;
        }
        partition_pos++;
    }

    if (partition_pos == ufs->partitions_length) {
        SFMF_WARN("Unknown partition: %s\n", partition);
        partition_pos = -1;
    } else {
        // index 0, 1 of length == 2 -> position 1, 2 of length == 2
        partition_pos++;
    }

    SFMF_LOG("dbus: sender=%s path=%s signal=%s.%s%s\n", sender_name, object_path,
            interface_name, signal_name, args);
    SFMF_LOG("queue: %s (%d/%d), partition='%s' (%d/%d), progress=%d, message='%s'\n",
            ufs->deploy_queue->name, ufs->deploy_queue->current, ufs->deploy_queue->total,
            partition, partition_pos, ufs->partitions_length,
            progress, message);

    g_free(args);
    g_free(partition);
    g_free(message);
}

void upgrade_factory_snapshot_init(struct UpgradeFactorySnapshot *ufs)
{
    sfmf_cleanup_register(upgrade_factory_snapshot_cleanup_cb, ufs);

    ufs->mainloop = g_main_loop_new(NULL, FALSE);
    ufs->deploy_queue->callback_user_data = ufs;
    ufs->cleanup_queue->callback_user_data = ufs;

    for (ufs->deploy_queue->total=0; ufs->deploy_queue->tasks[ufs->deploy_queue->total].cmd; ufs->deploy_queue->total++);
    for (ufs->cleanup_queue->total=0; ufs->cleanup_queue->tasks[ufs->cleanup_queue->total].cmd; ufs->cleanup_queue->total++);
    for (ufs->partitions_length=0; ufs->partitions[ufs->partitions_length]; ufs->partitions_length++);
    SFMF_DEBUG("Deploy tasks: %d, cleanup tasks: %d, partitions: %d\n",
            ufs->deploy_queue->total, ufs->cleanup_queue->total, ufs->partitions_length);

    GError *error = NULL;
    ufs->system_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!ufs->system_bus) {
        SFMF_FAIL("Could not connect to system bus: %s\n", error->message);
        g_error_free(error);
    }

    g_dbus_connection_signal_subscribe(ufs->system_bus, "org.sailfishos.sfmf.unpack",
            "org.sailfishos.sfmf.unpack", NULL, "/", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
            upgrade_factory_snapshot_dbus_signal_cb, ufs, NULL);
}

void upgrade_factory_snapshot_run(struct UpgradeFactorySnapshot *ufs)
{
    g_idle_add(upgrade_factory_snapshot_do_next_entry_cb, ufs);

    SFMF_LOG("Deploying snapshot...\n");
    g_main_loop_run(ufs->mainloop);
    SFMF_LOG("Done.\n");
}

void upgrade_factory_snapshot_quit(struct UpgradeFactorySnapshot *ufs)
{
    g_main_loop_unref(ufs->mainloop);

    if (ufs->system_bus) {
        g_object_unref(ufs->system_bus);
    }
}


int main(int argc, char *argv[])
{
    struct UpgradeFactorySnapshot *ufs = &g_ufs;

    upgrade_factory_snapshot_init(ufs);
    upgrade_factory_snapshot_run(ufs);
    upgrade_factory_snapshot_quit(ufs);

    return 0;
}
