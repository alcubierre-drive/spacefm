/**
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <string>
#include <string_view>

#include <filesystem>

#include <map>

#include <iostream>
#include <fstream>

#include <cassert>

#include <fmt/format.h>

#include <glibmm.h>
#include <glibmm/convert.h>

#include <fcntl.h>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include <glibmm.h>

#include <ztd/ztd.hxx>
#include <ztd/ztd_logger.hxx>

#include <magic_enum.hpp>

#include "write.hxx"
#include "utils.hxx"

#include "vfs/vfs-volume.hxx"
#include "vfs/vfs-thumbnail-loader.hxx"

#include "vfs/vfs-user-dir.hxx"
#include "vfs/vfs-dir.hxx"

static void vfs_dir_class_init(VFSDirClass* klass);
static void vfs_dir_init(VFSDir* dir);
static void vfs_dir_finalize(GObject* obj);
static void vfs_dir_set_property(GObject* obj, unsigned int prop_id, const GValue* value,
                                 GParamSpec* pspec);
static void vfs_dir_get_property(GObject* obj, unsigned int prop_id, GValue* value,
                                 GParamSpec* pspec);

static const std::string gethidden(std::string_view path);
static bool ishidden(std::string_view hidden, std::string_view file_name);

/* constructor is private */
static VFSDir* vfs_dir_new(const char* path);

static void vfs_dir_load(VFSDir* dir);
static void* vfs_dir_load_thread(VFSAsyncTask* task, VFSDir* dir);

static void vfs_dir_monitor_callback(VFSFileMonitor* monitor, VFSFileMonitorEvent event,
                                     const char* file_name, void* user_data);

static void on_mime_type_reload(void* user_data);

static bool notify_file_change(void* user_data);
static bool update_file_info(VFSDir* dir, VFSFileInfo* file);

static void on_list_task_finished(VFSAsyncTask* task, bool is_cancelled, VFSDir* dir);

enum VFSDirSignal
{
    FILE_CREATED_SIGNAL,
    FILE_DELETED_SIGNAL,
    FILE_CHANGED_SIGNAL,
    THUMBNAIL_LOADED_SIGNAL,
    FILE_LISTED_SIGNAL,
};

static unsigned int signals[magic_enum::enum_count<VFSDirSignal>()] = {0};
static GObjectClass* parent_class = nullptr;

static std::map<const char*, VFSDir*> dir_map;

static GList* mime_cb = nullptr;
static unsigned int change_notify_timeout = 0;

GType
vfs_dir_get_type()
{
    static GType type = G_TYPE_INVALID;
    if (type == G_TYPE_INVALID)
    {
        static const GTypeInfo info = {
            sizeof(VFSDirClass),
            nullptr,
            nullptr,
            (GClassInitFunc)vfs_dir_class_init,
            nullptr,
            nullptr,
            sizeof(VFSDir),
            0,
            (GInstanceInitFunc)vfs_dir_init,
            nullptr,
        };
        type = g_type_register_static(G_TYPE_OBJECT, "VFSDir", &info, GTypeFlags::G_TYPE_FLAG_NONE);
    }
    return type;
}

static void
vfs_dir_class_init(VFSDirClass* klass)
{
    GObjectClass* object_class;

    object_class = (GObjectClass*)klass;
    parent_class = (GObjectClass*)g_type_class_peek_parent(klass);

    object_class->set_property = vfs_dir_set_property;
    object_class->get_property = vfs_dir_get_property;
    object_class->finalize = vfs_dir_finalize;

    /*
     * file-created is emitted when there is a new file created in the dir.
     * The param is VFSFileInfo of the newly created file.
     */
    signals[VFSDirSignal::FILE_CREATED_SIGNAL] =
        g_signal_new("file-created",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     G_STRUCT_OFFSET(VFSDirClass, file_created),
                     nullptr,
                     nullptr,
                     g_cclosure_marshal_VOID__POINTER,
                     G_TYPE_NONE,
                     1,
                     G_TYPE_POINTER);

    /*
     * file-deleted is emitted when there is a file deleted in the dir.
     * The param is VFSFileInfo of the newly created file.
     */
    signals[VFSDirSignal::FILE_DELETED_SIGNAL] =
        g_signal_new("file-deleted",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     G_STRUCT_OFFSET(VFSDirClass, file_deleted),
                     nullptr,
                     nullptr,
                     g_cclosure_marshal_VOID__POINTER,
                     G_TYPE_NONE,
                     1,
                     G_TYPE_POINTER);

    /*
     * file-changed is emitted when there is a file changed in the dir.
     * The param is VFSFileInfo of the newly created file.
     */
    signals[VFSDirSignal::FILE_CHANGED_SIGNAL] =
        g_signal_new("file-changed",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     G_STRUCT_OFFSET(VFSDirClass, file_changed),
                     nullptr,
                     nullptr,
                     g_cclosure_marshal_VOID__POINTER,
                     G_TYPE_NONE,
                     1,
                     G_TYPE_POINTER);

    signals[VFSDirSignal::THUMBNAIL_LOADED_SIGNAL] =
        g_signal_new("thumbnail-loaded",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     G_STRUCT_OFFSET(VFSDirClass, thumbnail_loaded),
                     nullptr,
                     nullptr,
                     g_cclosure_marshal_VOID__POINTER,
                     G_TYPE_NONE,
                     1,
                     G_TYPE_POINTER);

    signals[VFSDirSignal::FILE_LISTED_SIGNAL] =
        g_signal_new("file-listed",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     G_STRUCT_OFFSET(VFSDirClass, file_listed),
                     nullptr,
                     nullptr,
                     g_cclosure_marshal_VOID__BOOLEAN,
                     G_TYPE_NONE,
                     1,
                     G_TYPE_BOOLEAN);
}

/* constructor */
static void
vfs_dir_init(VFSDir* dir)
{
    dir->mutex = (GMutex*)g_malloc(sizeof(GMutex));
    g_mutex_init(dir->mutex);
}

void
vfs_dir_lock(VFSDir* dir)
{
    g_mutex_lock(dir->mutex);
}

void
vfs_dir_unlock(VFSDir* dir)
{
    g_mutex_unlock(dir->mutex);
}

static void
vfs_dir_clear(VFSDir* dir)
{
    g_mutex_clear(dir->mutex);
    free(dir->mutex);
}

/* destructor */

static void
vfs_dir_finalize(GObject* obj)
{
    VFSDir* dir = VFS_DIR_REINTERPRET(obj);
    // LOG_INFO("vfs_dir_finalize  {}", dir->path);
    do
    {
    } while (g_source_remove_by_user_data(dir));

    if (dir->task)
    {
        g_signal_handlers_disconnect_by_func(dir->task, (void*)on_list_task_finished, dir);
        /* FIXME: should we generate a "file-list" signal to indicate the dir loading was cancelled?
         */
        // LOG_INFO("vfs_dir_finalize -> vfs_async_task_cancel");
        vfs_async_task_cancel(dir->task);
        g_object_unref(dir->task);
        dir->task = nullptr;
    }
    if (dir->monitor)
    {
        vfs_file_monitor_remove(dir->monitor, vfs_dir_monitor_callback, dir);
    }
    if (dir->path)
    {
        dir_map.erase(dir->path);

        /* There is no VFSDir instance */
        if (dir_map.size() == 0)
        {
            vfs_mime_type_remove_reload_cb(mime_cb);
            mime_cb = nullptr;

            if (change_notify_timeout)
            {
                g_source_remove(change_notify_timeout);
                change_notify_timeout = 0;
            }
        }

        free(dir->path);
        free(dir->disp_path);
        dir->path = dir->disp_path = nullptr;
    }
    // LOG_DEBUG("dir->thumbnail_loader: {:p}", dir->thumbnail_loader);
    if (dir->thumbnail_loader)
    {
        // LOG_DEBUG("FREE THUMBNAIL LOADER IN VFSDIR");
        vfs_thumbnail_loader_free(dir->thumbnail_loader);
        dir->thumbnail_loader = nullptr;
    }

    if (!dir->file_list.empty())
    {
        vfs_file_info_list_free(dir->file_list);
        dir->file_list.clear();
    }

    if (!dir->changed_files.empty())
    {
        vfs_file_info_list_free(dir->changed_files);
        dir->changed_files.clear();
    }

    if (dir->created_files)
    {
        g_slist_foreach(dir->created_files, (GFunc)free, nullptr);
        g_slist_free(dir->created_files);
        dir->created_files = nullptr;
    }

    vfs_dir_clear(dir);
    G_OBJECT_CLASS(parent_class)->finalize(obj);
}

static void
vfs_dir_get_property(GObject* obj, unsigned int prop_id, GValue* value, GParamSpec* pspec)
{
    (void)obj;
    (void)prop_id;
    (void)value;
    (void)pspec;
}

static void
vfs_dir_set_property(GObject* obj, unsigned int prop_id, const GValue* value, GParamSpec* pspec)
{
    (void)obj;
    (void)prop_id;
    (void)value;
    (void)pspec;
}

static VFSFileInfo*
vfs_dir_find_file(VFSDir* dir, const char* file_name, VFSFileInfo* file)
{
    for (VFSFileInfo* file2: dir->file_list)
    {
        if (file == file2)
            return file2;
        if (ztd::same(file2->name, file_name))
            return file2;
    }
    return nullptr;
}

/* signal handlers */
void
vfs_dir_emit_file_created(VFSDir* dir, const char* file_name, bool force)
{
    (void)force;
    // Ignore avoid_changes for creation of files
    // if ( !force && dir->avoid_changes )
    //    return;

    if (!strcmp(file_name, dir->path))
    {
        // Special Case: The directory itself was created?
        return;
    }

    dir->created_files = g_slist_append(dir->created_files, ztd::strdup(file_name));
    if (change_notify_timeout == 0)
    {
        change_notify_timeout = g_timeout_add_full(G_PRIORITY_LOW,
                                                   200,
                                                   (GSourceFunc)notify_file_change,
                                                   nullptr,
                                                   nullptr);
    }
}

void
vfs_dir_emit_file_deleted(VFSDir* dir, const char* file_name, VFSFileInfo* file)
{
    if (!strcmp(file_name, dir->path))
    {
        /* Special Case: The directory itself was deleted... */
        file = nullptr;
        /* clear the whole list */
        vfs_dir_lock(dir);
        vfs_file_info_list_free(dir->file_list);
        dir->file_list.clear();
        vfs_dir_unlock(dir);

        g_signal_emit(dir, signals[VFSDirSignal::FILE_DELETED_SIGNAL], 0, file);
        return;
    }

    VFSFileInfo* file_found = vfs_dir_find_file(dir, file_name, file);
    if (file_found)
    {
        file_found = vfs_file_info_ref(file_found);
        if (!ztd::contains(dir->changed_files, file_found))
        {
            dir->changed_files.push_back(file_found);
            if (change_notify_timeout == 0)
            {
                change_notify_timeout = g_timeout_add_full(G_PRIORITY_LOW,
                                                           200,
                                                           (GSourceFunc)notify_file_change,
                                                           nullptr,
                                                           nullptr);
            }
        }
        else
        {
            vfs_file_info_unref(file_found);
        }
    }
}

void
vfs_dir_emit_file_changed(VFSDir* dir, const char* file_name, VFSFileInfo* file, bool force)
{
    // LOG_INFO("vfs_dir_emit_file_changed dir={} file_name={} avoid={}", dir->path, file_name,
    // dir->avoid_changes ? "true" : "false");

    if (!force && dir->avoid_changes)
        return;

    if (!strcmp(file_name, dir->path))
    {
        // Special Case: The directory itself was changed
        g_signal_emit(dir, signals[VFSDirSignal::FILE_CHANGED_SIGNAL], 0, nullptr);
        return;
    }

    vfs_dir_lock(dir);

    VFSFileInfo* file_found = vfs_dir_find_file(dir, file_name, file);
    if (file_found)
    {
        file_found = vfs_file_info_ref(file_found);

        if (!ztd::contains(dir->changed_files, file_found))
        {
            if (force)
            {
                dir->changed_files.push_back(file_found);
                if (change_notify_timeout == 0)
                {
                    change_notify_timeout = g_timeout_add_full(G_PRIORITY_LOW,
                                                               100,
                                                               (GSourceFunc)notify_file_change,
                                                               nullptr,
                                                               nullptr);
                }
            }
            else if (update_file_info(dir, file_found)) // update file info the first time
            {
                dir->changed_files.push_back(file_found);
                if (change_notify_timeout == 0)
                {
                    change_notify_timeout = g_timeout_add_full(G_PRIORITY_LOW,
                                                               500,
                                                               (GSourceFunc)notify_file_change,
                                                               nullptr,
                                                               nullptr);
                }
                g_signal_emit(dir, signals[VFSDirSignal::FILE_CHANGED_SIGNAL], 0, file_found);
            }
        }
        else
        {
            vfs_file_info_unref(file_found);
        }
    }

    vfs_dir_unlock(dir);
}

void
vfs_dir_emit_thumbnail_loaded(VFSDir* dir, VFSFileInfo* file)
{
    vfs_dir_lock(dir);

    VFSFileInfo* file_found = vfs_dir_find_file(dir, file->name.c_str(), file);
    if (file_found)
    {
        assert(file == file_found);
        file = vfs_file_info_ref(file_found);
    }
    else
    {
        file = nullptr;
    }

    vfs_dir_unlock(dir);

    if (file)
    {
        g_signal_emit(dir, signals[VFSDirSignal::THUMBNAIL_LOADED_SIGNAL], 0, file);
        vfs_file_info_unref(file);
    }
}

/* methods */

static VFSDir*
vfs_dir_new(const char* path)
{
    VFSDir* dir = VFS_DIR(g_object_new(VFS_TYPE_DIR, nullptr));
    dir->path = ztd::strdup(path);

    dir->avoid_changes = vfs_volume_dir_avoid_changes(path);
    // LOG_INFO("vfs_dir_new {}  avoid_changes={}", dir->path, dir->avoid_changes ? "true" :
    // "false");
    return dir;
}

void
on_list_task_finished(VFSAsyncTask* task, bool is_cancelled, VFSDir* dir)
{
    (void)task;
    g_object_unref(dir->task);
    dir->task = nullptr;
    g_signal_emit(dir, signals[VFSDirSignal::FILE_LISTED_SIGNAL], 0, is_cancelled);
    dir->file_listed = true;
    dir->load_complete = true;
}

static const std::string
gethidden(std::string_view path)
{
    std::string hidden;

    // Read .hidden into string
    const std::string hidden_path = Glib::build_filename(path.data(), ".hidden");

    // test access first because open() on missing file may cause
    // long delay on nfs
    if (!have_rw_access(hidden_path))
        return hidden;

    std::string line;
    std::ifstream file(hidden_path);
    if (file.is_open())
    {
        while (std::getline(file, line))
        {
            hidden.append(line + '\n');
        }
    }
    file.close();

    return hidden;
}

static bool
ishidden(std::string_view hidden, std::string_view file_name)
{
    if (ztd::contains(hidden, file_name))
        return true;
    return false;
}

bool
vfs_dir_add_hidden(std::string_view path, std::string_view file_name)
{
    const std::string file_path = Glib::build_filename(path.data(), ".hidden");
    const std::string data = fmt::format("{}\n", file_name);

    const bool result = write_file(file_path, data);
    if (!result)
        return false;
    return true;
}

static void
vfs_dir_load(VFSDir* dir)
{
    if (dir->path)
    {
        dir->disp_path = ztd::strdup(Glib::filename_display_name(dir->path));
        dir->task = vfs_async_task_new((VFSAsyncFunc)vfs_dir_load_thread, dir);
        g_signal_connect(dir->task, "finish", G_CALLBACK(on_list_task_finished), dir);
        vfs_async_task_execute(dir->task);
    }
}

static void*
vfs_dir_load_thread(VFSAsyncTask* task, VFSDir* dir)
{
    (void)task;

    dir->file_listed = false;
    dir->load_complete = false;
    dir->xhidden_count = 0; // MOD
    if (dir->path)
    {
        /* Install file alteration monitor */
        dir->monitor = vfs_file_monitor_add(dir->path, vfs_dir_monitor_callback, dir);

        // MOD  dir contains .hidden file?
        const std::string hidden = gethidden(dir->path);

        for (const auto& file: std::filesystem::directory_iterator(dir->path))
        {
            if (vfs_async_task_is_cancelled(dir->task))
                break;

            const std::string file_name = std::filesystem::path(file).filename();
            const std::string full_path = Glib::build_filename(dir->path, file_name);

            // MOD ignore if in .hidden
            if (ishidden(hidden, file_name))
            {
                dir->xhidden_count++;
                continue;
            }

            VFSFileInfo* fi = vfs_file_info_new();
            if (vfs_file_info_get(fi, full_path))
            {
                vfs_dir_lock(dir);

                /* Special processing for desktop directory */
                vfs_file_info_load_special_info(fi, full_path.c_str());

                dir->file_list.push_back(fi);

                vfs_dir_unlock(dir);
            }
            else
            {
                vfs_file_info_unref(fi);
            }
        }
    }
    return nullptr;
}

bool
vfs_dir_is_file_listed(VFSDir* dir)
{
    return dir->file_listed;
}

static bool
update_file_info(VFSDir* dir, VFSFileInfo* file)
{
    bool ret = false;

    /* FIXME: Dirty hack: steal the string to prevent memory allocation */
    const std::string file_name = file->name;
    if (ztd::same(file->name, file->disp_name))
        file->disp_name.clear();
    file->name.clear();

    const std::string full_path = Glib::build_filename(dir->path, file_name);

    if (vfs_file_info_get(file, full_path))
    {
        ret = true;
        vfs_file_info_load_special_info(file, full_path.c_str());
    }
    else /* The file does not exist */
    {
        if (ztd::contains(dir->file_list, file))
        {
            ztd::remove(dir->file_list, file);
            if (file)
            {
                g_signal_emit(dir, signals[VFSDirSignal::FILE_DELETED_SIGNAL], 0, file);
                vfs_file_info_unref(file);
            }
        }
        ret = false;
    }

    return ret;
}

static void
update_changed_files(const char* key, VFSDir* dir)
{
    (void)key;

    if (dir->changed_files.empty())
        return;

    vfs_dir_lock(dir);
    for (VFSFileInfo* file: dir->changed_files)
    {
        if (update_file_info(dir, file))
        {
            g_signal_emit(dir, signals[VFSDirSignal::FILE_CHANGED_SIGNAL], 0, file);
            vfs_file_info_unref(file);
        }
        // else was deleted, signaled, and unrefed in update_file_info
    }
    dir->changed_files.clear();
    vfs_dir_unlock(dir);
}

static void
update_created_files(const char* key, VFSDir* dir)
{
    (void)key;

    if (dir->created_files)
    {
        vfs_dir_lock(dir);
        for (GSList* l = dir->created_files; l; l = l->next)
        {
            VFSFileInfo* file;
            VFSFileInfo* file_found = vfs_dir_find_file(dir, (char*)l->data, nullptr);
            if (!file_found)
            {
                // file is not in dir file_list
                const std::string full_path = Glib::build_filename(dir->path, (char*)l->data);
                file = vfs_file_info_new();
                if (vfs_file_info_get(file, full_path))
                {
                    // add new file to dir file_list
                    vfs_file_info_load_special_info(file, full_path.c_str());
                    dir->file_list.push_back(vfs_file_info_ref(file));
                    g_signal_emit(dir, signals[VFSDirSignal::FILE_CREATED_SIGNAL], 0, file);
                }
                // else file does not exist in filesystem
                vfs_file_info_unref(file);
            }
            else
            {
                // file already exists in dir file_list
                file = vfs_file_info_ref(file_found);
                if (update_file_info(dir, file))
                {
                    g_signal_emit(dir, signals[VFSDirSignal::FILE_CHANGED_SIGNAL], 0, file);
                    vfs_file_info_unref(file);
                }
                // else was deleted, signaled, and unrefed in update_file_info
            }
            free((char*)l->data); // free file_name string
        }
        g_slist_free(dir->created_files);
        dir->created_files = nullptr;
        vfs_dir_unlock(dir);
    }
}

static bool
notify_file_change(void* user_data)
{
    (void)user_data;

    for (auto it = dir_map.begin(); it != dir_map.end(); ++it)
    {
        update_changed_files(it->first, it->second);
        update_created_files(it->first, it->second);
    }
    /* remove the timeout */
    change_notify_timeout = 0;
    return false;
}

void
vfs_dir_flush_notify_cache()
{
    if (change_notify_timeout)
        g_source_remove(change_notify_timeout);
    change_notify_timeout = 0;

    for (auto it = dir_map.begin(); it != dir_map.end(); ++it)
    {
        update_changed_files(it->first, it->second);
        update_created_files(it->first, it->second);
    }
}

/* Callback function which will be called when monitored events happen */
static void
vfs_dir_monitor_callback(VFSFileMonitor* monitor, VFSFileMonitorEvent event, const char* file_name,
                         void* user_data)
{
    (void)monitor;
    VFSDir* dir = VFS_DIR(user_data);

    switch (event)
    {
        case VFSFileMonitorEvent::VFS_FILE_MONITOR_CREATE:
            vfs_dir_emit_file_created(dir, file_name, false);
            break;
        case VFSFileMonitorEvent::VFS_FILE_MONITOR_DELETE:
            vfs_dir_emit_file_deleted(dir, file_name, nullptr);
            break;
        case VFSFileMonitorEvent::VFS_FILE_MONITOR_CHANGE:
            vfs_dir_emit_file_changed(dir, file_name, nullptr, false);
            break;
        default:
            LOG_WARN("Error: unrecognized file monitor signal!");
    }
}

VFSDir*
vfs_dir_get_by_path_soft(const char* path)
{
    if (!path)
        return nullptr;

    VFSDir* dir = nullptr;

    try
    {
        dir = dir_map.at(path);
    }
    catch (std::out_of_range)
    {
        dir = nullptr;
    }

    if (dir)
        g_object_ref(dir);
    return dir;
}

VFSDir*
vfs_dir_get_by_path(const char* path)
{
    if (!path)
        return nullptr;

    VFSDir* dir = nullptr;

    if (!dir_map.empty())
    {
        try
        {
            dir = dir_map.at(path);
        }
        catch (std::out_of_range)
        {
            dir = nullptr;
        }
    }

    if (!mime_cb)
        mime_cb = vfs_mime_type_add_reload_cb(on_mime_type_reload, nullptr);

    if (dir)
    {
        g_object_ref(dir);
    }
    else
    {
        dir = vfs_dir_new(path);
        vfs_dir_load(dir); /* asynchronous operation */
        dir_map.insert({dir->path, dir});
    }
    return dir;
}

static void
reload_mime_type(const char* key, VFSDir* dir)
{
    (void)key;

    if (!dir || dir->file_list.empty())
        return;

    vfs_dir_lock(dir);

    for (VFSFileInfo* file: dir->file_list)
    {
        const std::string full_path = Glib::build_filename(dir->path, vfs_file_info_get_name(file));
        vfs_file_info_reload_mime_type(file, full_path.c_str());
        // LOG_DEBUG("reload {}", full_path);
    }

    for (VFSFileInfo* file: dir->file_list)
    {
        g_signal_emit(dir, signals[VFSDirSignal::FILE_CHANGED_SIGNAL], 0, file);
    }

    vfs_dir_unlock(dir);
}

static void
on_mime_type_reload(void* user_data)
{
    (void)user_data;
    // LOG_DEBUG("reload mime-type");
    for (auto it = dir_map.begin(); it != dir_map.end(); ++it)
    {
        reload_mime_type(it->first, it->second);
    }
}

void
vfs_dir_foreach(VFSDirForeachFunc func, void* user_data)
{
    // LOG_DEBUG("reload mime-type");
    for (auto it = dir_map.begin(); it != dir_map.end(); ++it)
    {
        func(it->first, it->second, user_data);
    }
}

void
vfs_dir_unload_thumbnails(VFSDir* dir, bool is_big)
{
    vfs_dir_lock(dir);
    for (VFSFileInfo* file: dir->file_list)
    {
        if (is_big)
        {
            if (file->big_thumbnail)
            {
                g_object_unref(file->big_thumbnail);
                file->big_thumbnail = nullptr;
            }
        }
        else
        {
            if (file->small_thumbnail)
            {
                g_object_unref(file->small_thumbnail);
                file->small_thumbnail = nullptr;
            }
        }

        /* This is a desktop entry file, so the icon needs reload
             FIXME: This is not a good way to do things, but there is no better way now.  */
        if (file->flags & VFSFileInfoFlag::VFS_FILE_INFO_DESKTOP_ENTRY)
        {
            const std::string file_path = Glib::build_filename(dir->path, file->name);
            vfs_file_info_load_special_info(file, file_path.c_str());
        }
    }
    vfs_dir_unlock(dir);

    /* Ensuring free space at the end of the heap is freed to the OS,
     * mainly to deal with the possibility thousands of large thumbnails
     * have been freed but the memory not actually released by SpaceFM */
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
}

// sfm added mime change timer
static unsigned int mime_change_timer = 0;
static VFSDir* mime_dir = nullptr;

static bool
on_mime_change_timer(void* user_data)
{
    (void)user_data;

    // LOG_INFO("MIME-UPDATE on_timer");
    const std::string mime_command =
        fmt::format("update-mime-database {}/mime", vfs_user_data_dir());
    print_command(mime_command);
    Glib::spawn_command_line_async(mime_command);

    const std::string desk_command =
        fmt::format("update-desktop-database {}/applications", vfs_user_data_dir());
    print_command(desk_command);
    Glib::spawn_command_line_async(desk_command);

    g_source_remove(mime_change_timer);
    mime_change_timer = 0;
    return false;
}

static void
mime_change(void* user_data)
{
    (void)user_data;
    if (mime_change_timer)
    {
        // timer is already running, so ignore request
        // LOG_INFO("MIME-UPDATE already set");
        return;
    }
    if (mime_dir)
    {
        // update mime database in 2 seconds
        mime_change_timer = g_timeout_add_seconds(2, (GSourceFunc)on_mime_change_timer, nullptr);
        // LOG_INFO("MIME-UPDATE timer started");
    }
}

void
vfs_dir_monitor_mime()
{
    // start watching for changes
    if (mime_dir)
        return;
    const std::string path = Glib::build_filename(vfs_user_data_dir(), "mime/packages", nullptr);
    if (std::filesystem::is_directory(path))
    {
        mime_dir = vfs_dir_get_by_path(path.c_str());
        if (mime_dir)
        {
            g_signal_connect(mime_dir, "file-listed", G_CALLBACK(mime_change), nullptr);
            g_signal_connect(mime_dir, "file-created", G_CALLBACK(mime_change), nullptr);
            g_signal_connect(mime_dir, "file-deleted", G_CALLBACK(mime_change), nullptr);
            g_signal_connect(mime_dir, "file-changed", G_CALLBACK(mime_change), nullptr);
        }
        // LOG_INFO("MIME-UPDATE watch started");
    }
}
