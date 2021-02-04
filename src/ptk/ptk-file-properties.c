/*
 *  C Implementation: file_properties
 *
 * Description:
 *
 *
 * Copyright: See COPYING file that comes with this distribution
 *
 */

#include <stdbool.h>

#include <libintl.h>

#include "private.h"

#include <gtk/gtk.h>

#include "ptk-file-properties.h"

#include "../mime-type/mime-type.h"

#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <string.h>

#include "ptk-file-task.h"
#include "ptk-utils.h"

#include "../vfs/vfs-app-desktop.h"

#include "ptk-app-chooser.h"
#include "utils.h"

static const char* chmod_names[] = {"owner_r",
                                    "owner_w",
                                    "owner_x",
                                    "group_r",
                                    "group_w",
                                    "group_x",
                                    "others_r",
                                    "others_w",
                                    "others_x",
                                    "set_uid",
                                    "set_gid",
                                    "sticky"};

typedef struct FilePropertiesDialogData
{
    char* dir_path;
    GList* file_list;
    GtkWidget* dlg;

    GtkEntry* owner;
    GtkEntry* group;
    char* owner_name;
    char* group_name;

    GtkEntry* mtime;
    char* orig_mtime;
    GtkEntry* atime;
    char* orig_atime;

    GtkToggleButton* chmod_btns[N_CHMOD_ACTIONS];
    unsigned char chmod_states[N_CHMOD_ACTIONS];

    GtkLabel* total_size_label;
    GtkLabel* size_on_disk_label;
    GtkLabel* count_label;
    off_t total_size;
    off_t size_on_disk;
    unsigned int total_count;
    unsigned int total_count_dir;
    bool cancel;
    bool done;
    GThread* calc_size_thread;
    unsigned int update_label_timer;
    GtkWidget* recurse;
} FilePropertiesDialogData;

static void on_dlg_response(GtkDialog* dialog, int response_id, void* user_data);

/*
 * void get_total_size_of_dir( const char* path, off_t* size )
 * Recursively count total size of all files in the specified directory.
 * If the path specified is a file, the size of the file is directly returned.
 * cancel is used to cancel the operation. This function will check the value
 * pointed by cancel in every iteration. If cancel is set to TRUE, the
 * calculation is cancelled.
 * NOTE: path is encoded in on-disk encoding and not necessarily UTF-8.
 */
static void calc_total_size_of_files(const char* path, FilePropertiesDialogData* data)
{
    if (data->cancel)
        return;

    struct stat file_stat;
    if (lstat(path, &file_stat))
        return;

    data->total_size += file_stat.st_size;
    data->size_on_disk += (file_stat.st_blocks << 9); /* block x 512 */

    GDir* dir = g_dir_open(path, 0, NULL);
    if (dir)
    {
        const char* name;
        ++data->total_count_dir;
        while (!data->cancel && (name = g_dir_read_name(dir)))
        {
            char* full_path = g_build_filename(path, name, NULL);
            lstat(full_path, &file_stat);
            if (S_ISDIR(file_stat.st_mode))
            {
                calc_total_size_of_files(full_path, data);
            }
            else
            {
                data->total_size += file_stat.st_size;
                data->size_on_disk += (file_stat.st_blocks << 9);
                ++data->total_count;
            }
            g_free(full_path);
        }
        g_dir_close(dir);
    }
    else
        ++data->total_count;
}

static void* calc_size(void* user_data)
{
    FilePropertiesDialogData* data = (FilePropertiesDialogData*)user_data;
    GList* l;
    for (l = data->file_list; l; l = l->next)
    {
        if (data->cancel)
            break;
        VFSFileInfo* file = (VFSFileInfo*)l->data;
        char* path = g_build_filename(data->dir_path, vfs_file_info_get_name(file), NULL);
        if (path)
        {
            calc_total_size_of_files(path, data);
            g_free(path);
        }
    }
    data->done = TRUE;
    return NULL;
}

static bool on_update_labels(FilePropertiesDialogData* data)
{
    char buf[64];
    char buf2[32];

    gdk_threads_enter();

    vfs_file_size_to_string_format(buf2, data->total_size, "%.1f %s");
    sprintf(buf, _("%s ( %lu bytes )"), buf2, (uint64_t)data->total_size);
    gtk_label_set_text(data->total_size_label, buf);

    vfs_file_size_to_string_format(buf2, data->size_on_disk, "%.1f %s");
    sprintf(buf, _("%s ( %lu bytes )"), buf2, (uint64_t)data->size_on_disk);
    gtk_label_set_text(data->size_on_disk_label, buf);

    char* count;
    char* count_dir;
    if (data->total_count_dir)
    {
        count_dir =
            g_strdup_printf(ngettext("%d directory", "%d directories", data->total_count_dir),
                            data->total_count_dir);
        count = g_strdup_printf(ngettext("%d file, %s", "%d files, %s", data->total_count),
                                data->total_count,
                                count_dir);
        g_free(count_dir);
    }
    else
        count =
            g_strdup_printf(ngettext("%d files", "%d files", data->total_count), data->total_count);

    gtk_label_set_text(data->count_label, count);
    g_free(count);

    gdk_threads_leave();

    if (data->done)
        data->update_label_timer = 0;
    return !data->done;
}

static void on_chmod_btn_toggled(GtkToggleButton* btn, FilePropertiesDialogData* data)
{
    /* Bypass the default handler */
    g_signal_stop_emission_by_name(btn, "toggled");
    /* Block this handler while we are changing the state of buttons,
      or this handler will be called recursively. */
    g_signal_handlers_block_matched(btn,
                                    G_SIGNAL_MATCH_FUNC,
                                    0,
                                    0,
                                    NULL,
                                    on_chmod_btn_toggled,
                                    NULL);

    if (gtk_toggle_button_get_inconsistent(btn))
    {
        gtk_toggle_button_set_inconsistent(btn, FALSE);
        gtk_toggle_button_set_active(btn, FALSE);
    }
    else if (!gtk_toggle_button_get_active(btn))
    {
        gtk_toggle_button_set_inconsistent(btn, TRUE);
    }

    g_signal_handlers_unblock_matched(btn,
                                      G_SIGNAL_MATCH_FUNC,
                                      0,
                                      0,
                                      NULL,
                                      on_chmod_btn_toggled,
                                      NULL);
}

static bool combo_sep(GtkTreeModel* model, GtkTreeIter* it, void* user_data)
{
    int i;
    for (i = 2; i > 0; --i)
    {
        char* tmp;
        gtk_tree_model_get(model, it, i, &tmp, -1);
        if (tmp)
        {
            g_free(tmp);
            return FALSE;
        }
    }
    return TRUE;
}

static void on_combo_change(GtkComboBox* combo, void* user_data)
{
    GtkTreeIter it;
    if (gtk_combo_box_get_active_iter(combo, &it))
    {
        char* action;
        GtkTreeModel* model = gtk_combo_box_get_model(combo);
        gtk_tree_model_get(model, &it, 2, &action, -1);
        if (!action)
        {
            VFSMimeType* mime = (VFSMimeType*)user_data;
            GtkWidget* parent = gtk_widget_get_toplevel(GTK_WIDGET(combo));
            action = (char*)
                ptk_choose_app_for_mime_type(GTK_WINDOW(parent), mime, FALSE, TRUE, TRUE, TRUE);
            if (action)
            {
                bool exist = FALSE;
                /* check if the action is already in the list */
                if (gtk_tree_model_get_iter_first(model, &it))
                {
                    do
                    {
                        char* tmp;
                        gtk_tree_model_get(model, &it, 2, &tmp, -1);
                        if (!tmp)
                            continue;
                        if (!strcmp(tmp, action))
                        {
                            exist = TRUE;
                            g_free(tmp);
                            break;
                        }
                        g_free(tmp);
                    } while (gtk_tree_model_iter_next(model, &it));
                }

                if (!exist) /* It didn't exist */
                {
                    VFSAppDesktop* app = vfs_app_desktop_new(action);
                    if (app)
                    {
                        GdkPixbuf* icon;
                        icon = vfs_app_desktop_get_icon(app, 20, TRUE);
                        gtk_list_store_insert_with_values(GTK_LIST_STORE(model),
                                                          &it,
                                                          0,
                                                          0,
                                                          icon,
                                                          1,
                                                          vfs_app_desktop_get_disp_name(app),
                                                          2,
                                                          action,
                                                          -1);
                        if (icon)
                            g_object_unref(icon);
                        vfs_app_desktop_unref(app);
                        exist = TRUE;
                    }
                }

                if (exist)
                    gtk_combo_box_set_active_iter(combo, &it);
                g_free(action);
            }
            else
            {
                int prev_sel;
                prev_sel = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(combo), "prev_sel"));
                gtk_combo_box_set_active(combo, prev_sel);
            }
        }
        else
        {
            int prev_sel = gtk_combo_box_get_active(combo);
            g_object_set_data(G_OBJECT(combo), "prev_sel", GINT_TO_POINTER(prev_sel));
        }
    }
    else
    {
        g_object_set_data(G_OBJECT(combo), "prev_sel", GINT_TO_POINTER(-1));
    }
}

GtkWidget* file_properties_dlg_new(GtkWindow* parent, const char* dir_path, GList* sel_files,
                                   int page)
{
#if (GTK_MAJOR_VERSION == 3)
    const char* file_properties_ui = g_strdup("/file_properties3.ui");
#elif (GTK_MAJOR_VERSION == 2)
    const char* file_properties_ui = g_strdup("/file_properties2.ui");
#endif
    GtkBuilder* builder = _gtk_builder_new_from_file(PACKAGE_UI_DIR, file_properties_ui, NULL);
    GtkWidget* dlg = (GtkWidget*)gtk_builder_get_object(builder, "dlg");
    GtkNotebook* notebook = (GtkNotebook*)gtk_builder_get_object(builder, "notebook");
    xset_set_window_icon(GTK_WINDOW(dlg));

    FilePropertiesDialogData* data;
    bool need_calc_size = TRUE;

    VFSFileInfo* file;
    VFSMimeType* mime;

    const char* multiple_files = _("( multiple files )");
    const char* calculating;
    GtkWidget* name = (GtkWidget*)gtk_builder_get_object(builder, "file_name");
    GtkWidget* label_name = (GtkWidget*)gtk_builder_get_object(builder, "label_filename");
    GtkWidget* location = (GtkWidget*)gtk_builder_get_object(builder, "location");
    gtk_editable_set_editable(GTK_EDITABLE(location), FALSE);
    GtkWidget* target = (GtkWidget*)gtk_builder_get_object(builder, "target");
    GtkWidget* label_target = (GtkWidget*)gtk_builder_get_object(builder, "label_target");
    gtk_editable_set_editable(GTK_EDITABLE(target), FALSE);
    GtkWidget* mime_type = (GtkWidget*)gtk_builder_get_object(builder, "mime_type");
    GtkWidget* open_with = (GtkWidget*)gtk_builder_get_object(builder, "open_with");

    char buf[64];
    char buf2[32];
    const char* time_format = g_strdup("%Y-%m-%d %H:%M:%S");

    char* disp_path;
    char* file_type;

    int i;
    GList* l;
    bool same_type = TRUE;
    bool is_dirs = FALSE;
    char* owner_group;
    char* tmp;

    int width = xset_get_int("app_dlg", "s");
    int height = xset_get_int("app_dlg", "z");
    if (width && height)
        gtk_window_set_default_size(GTK_WINDOW(dlg), width, -1);

    data = g_slice_new0(FilePropertiesDialogData);
    data->update_label_timer = 0;
    /* FIXME: When will the data be freed??? */
    g_object_set_data(G_OBJECT(dlg), "DialogData", data);
    data->file_list = sel_files;
    data->dlg = dlg;

    data->dir_path = g_strdup(dir_path);
    disp_path = g_filename_display_name(dir_path);
    // gtk_label_set_text( GTK_LABEL( location ), disp_path );
    gtk_entry_set_text(GTK_ENTRY(location), disp_path);
    g_free(disp_path);

    data->total_size_label = GTK_LABEL((GtkWidget*)gtk_builder_get_object(builder, "total_size"));
    data->size_on_disk_label =
        GTK_LABEL((GtkWidget*)gtk_builder_get_object(builder, "size_on_disk"));
    data->count_label = GTK_LABEL((GtkWidget*)gtk_builder_get_object(builder, "count"));
    data->owner = GTK_ENTRY((GtkWidget*)gtk_builder_get_object(builder, "owner"));
    data->group = GTK_ENTRY((GtkWidget*)gtk_builder_get_object(builder, "group"));
    data->mtime = GTK_ENTRY((GtkWidget*)gtk_builder_get_object(builder, "mtime"));
    data->atime = GTK_ENTRY((GtkWidget*)gtk_builder_get_object(builder, "atime"));

    for (i = 0; i < N_CHMOD_ACTIONS; ++i)
    {
        data->chmod_btns[i] =
            GTK_TOGGLE_BUTTON((GtkWidget*)gtk_builder_get_object(builder, chmod_names[i]));
    }

    // MOD
    VFSMimeType* type;
    VFSMimeType* type2 = NULL;
    for (l = sel_files; l; l = l->next)
    {
        file = (VFSFileInfo*)l->data;
        type = vfs_file_info_get_mime_type(file);
        if (!type2)
            type2 = vfs_file_info_get_mime_type(file);
        if (vfs_file_info_is_dir(file))
            is_dirs = TRUE;
        if (type != type2)
            same_type = FALSE;
        vfs_mime_type_unref(type);
        if (is_dirs && !same_type)
            break;
    }
    if (type2)
        vfs_mime_type_unref(type2);

    data->recurse = (GtkWidget*)gtk_builder_get_object(builder, "recursive");
    gtk_widget_set_sensitive(data->recurse, is_dirs);

    file = (VFSFileInfo*)sel_files->data;
    if (same_type)
    {
        mime = vfs_file_info_get_mime_type(file);
        file_type = g_strdup_printf("%s\n%s",
                                    vfs_mime_type_get_description(mime),
                                    vfs_mime_type_get_type(mime));
        gtk_label_set_text(GTK_LABEL(mime_type), file_type);
        g_free(file_type);
        vfs_mime_type_unref(mime);
    }
    else
    {
        gtk_label_set_text(GTK_LABEL(mime_type), _("( multiple types )"));
    }

    /* Open with...
     * Don't show this option menu if files of different types are selected,
     * ,the selected file is a directory, or its type is unknown.
     */
    if (!same_type || vfs_file_info_is_desktop_entry(file) ||
        vfs_file_info_is_executable(file, NULL))
    {
        /* if open with shouldn't show, destroy it. */
        gtk_widget_destroy(open_with);
        open_with = NULL;
        gtk_widget_destroy((GtkWidget*)gtk_builder_get_object(builder, "open_with_label"));
    }
    else /* Add available actions to the option menu */
    {
        GtkTreeIter it;
        char** action;
        char** actions;

        mime = vfs_file_info_get_mime_type(file);
        actions = vfs_mime_type_get_actions(mime);
        GtkCellRenderer* renderer;
        GtkListStore* model;
        gtk_cell_layout_clear(GTK_CELL_LAYOUT(open_with));
        renderer = gtk_cell_renderer_pixbuf_new();
        gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(open_with), renderer, FALSE);
        gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(open_with), renderer, "pixbuf", 0, NULL);
        renderer = gtk_cell_renderer_text_new();
        gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(open_with), renderer, TRUE);
        gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(open_with), renderer, "text", 1, NULL);
        model = gtk_list_store_new(3, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
        if (actions)
        {
            for (action = actions; *action; ++action)
            {
                VFSAppDesktop* desktop;
                GdkPixbuf* icon;
                desktop = vfs_app_desktop_new(*action);
                gtk_list_store_append(model, &it);
                icon = vfs_app_desktop_get_icon(desktop, 20, TRUE);
                gtk_list_store_set(model,
                                   &it,
                                   0,
                                   icon,
                                   1,
                                   vfs_app_desktop_get_disp_name(desktop),
                                   2,
                                   *action,
                                   -1);
                if (icon)
                    g_object_unref(icon);
                vfs_app_desktop_unref(desktop);
            }
        }
        else
        {
            g_object_set_data(G_OBJECT(open_with), "prev_sel", GINT_TO_POINTER(-1));
        }

        /* separator */
        gtk_list_store_append(model, &it);

        gtk_list_store_append(model, &it);
        gtk_list_store_set(model, &it, 0, NULL, 1, _("Choose..."), -1);
        gtk_combo_box_set_model(GTK_COMBO_BOX(open_with), GTK_TREE_MODEL(model));
        // gtk_combo_box_set_model adds a ref
        g_object_unref(model);
        gtk_combo_box_set_row_separator_func(GTK_COMBO_BOX(open_with), combo_sep, NULL, NULL);
        gtk_combo_box_set_active(GTK_COMBO_BOX(open_with), 0);
        g_signal_connect(open_with, "changed", G_CALLBACK(on_combo_change), mime);

        /* vfs_mime_type_unref( mime ); */
        /* We can unref mime when combo box gets destroyed */
        g_object_weak_ref(G_OBJECT(open_with), (GWeakNotify)vfs_mime_type_unref, mime);
    }
    g_object_set_data(G_OBJECT(dlg), "open_with", open_with);

    /* Multiple files are selected */
    if (sel_files && sel_files->next)
    {
        gtk_widget_set_sensitive(name, FALSE);
        gtk_entry_set_text(GTK_ENTRY(name), multiple_files);

        data->orig_mtime = NULL;
        data->orig_atime = NULL;

        for (i = 0; i < N_CHMOD_ACTIONS; ++i)
        {
            gtk_toggle_button_set_inconsistent(data->chmod_btns[i], TRUE);
            data->chmod_states[i] = 2; /* Don't touch this bit */
            g_signal_connect(G_OBJECT(data->chmod_btns[i]),
                             "toggled",
                             G_CALLBACK(on_chmod_btn_toggled),
                             data);
        }
    }
    else
    {
        /* special processing for files with special display names */
        if (vfs_file_info_is_desktop_entry(file))
        {
            char* disp_name = g_filename_display_name(file->name);
            gtk_entry_set_text(GTK_ENTRY(name), disp_name);
            g_free(disp_name);
        }
        else
        {
            if (vfs_file_info_is_dir(file) && !vfs_file_info_is_symlink(file))
                gtk_label_set_markup_with_mnemonic(GTK_LABEL(label_name),
                                                   _("<b>Directory _Name:</b>"));
            gtk_entry_set_text(GTK_ENTRY(name), vfs_file_info_get_disp_name(file));
        }

        gtk_editable_set_editable(GTK_EDITABLE(name), FALSE);

        if (!vfs_file_info_is_dir(file))
        {
            /* Only single "file" is selected, so we don't need to
                caculate total file size */
            need_calc_size = FALSE;

            sprintf(buf,
                    _("%s  ( %lu bytes )"),
                    vfs_file_info_get_disp_size(file),
                    (uint64_t)vfs_file_info_get_size(file));
            gtk_label_set_text(data->total_size_label, buf);

            vfs_file_size_to_string_format(buf2, vfs_file_info_get_blocks(file) * 512, "%.1f %s");
            sprintf(buf,
                    _("%s  ( %lu bytes )"),
                    buf2,
                    (uint64_t)vfs_file_info_get_blocks(file) * 512);
            gtk_label_set_text(data->size_on_disk_label, buf);

            gtk_label_set_text(data->count_label, _("1 file"));
        }

        // Modified / Accessed
        // gtk_entry_set_text( GTK_ENTRY( mtime ),
        //                    vfs_file_info_get_disp_mtime( file ) );
        strftime(buf, sizeof(buf), time_format, localtime(vfs_file_info_get_mtime(file)));
        gtk_entry_set_text(GTK_ENTRY(data->mtime), buf);
        data->orig_mtime = g_strdup(buf);

        strftime(buf, sizeof(buf), time_format, localtime(vfs_file_info_get_atime(file)));
        gtk_entry_set_text(GTK_ENTRY(data->atime), buf);
        data->orig_atime = g_strdup(buf);

        // Permissions
        owner_group = (char*)vfs_file_info_get_disp_owner(file);
        tmp = strchr(owner_group, ':');
        data->owner_name = g_strndup(owner_group, tmp - owner_group);
        gtk_entry_set_text(GTK_ENTRY(data->owner), data->owner_name);
        data->group_name = g_strdup(tmp + 1);
        gtk_entry_set_text(GTK_ENTRY(data->group), data->group_name);

        for (i = 0; i < N_CHMOD_ACTIONS; ++i)
        {
            if (data->chmod_states[i] != 2) /* allow to touch this bit */
            {
                data->chmod_states[i] = (vfs_file_info_get_mode(file) & chmod_flags[i] ? 1 : 0);
                gtk_toggle_button_set_active(data->chmod_btns[i], data->chmod_states[i]);
            }
        }

        // target
        if (vfs_file_info_is_symlink(file))
        {
            gtk_label_set_markup_with_mnemonic(GTK_LABEL(label_name), _("<b>Link _Name:</b>"));
            disp_path = g_build_filename(dir_path, file->name, NULL);
            char* target_path = g_file_read_link(disp_path, NULL);
            if (target_path)
            {
                gtk_entry_set_text(GTK_ENTRY(target), target_path);
                if (target_path[0] && target_path[0] != '/')
                {
                    // relative link to absolute
                    char* str = target_path;
                    target_path = g_build_filename(dir_path, str, NULL);
                    g_free(str);
                }
                if (!g_file_test(target_path, G_FILE_TEST_EXISTS))
                    gtk_label_set_text(GTK_LABEL(mime_type), _("( broken link )"));
                g_free(target_path);
            }
            else
                gtk_entry_set_text(GTK_ENTRY(target), _("( read link error )"));
            g_free(disp_path);
            gtk_widget_show(target);
            gtk_widget_show(label_target);
        }
    }

    if (need_calc_size)
    {
        /* The total file size displayed in "File Properties" is not
           completely calculated yet. So "Calculating..." is displayed. */
        calculating = _("Calculating...");
        gtk_label_set_text(data->total_size_label, calculating);
        gtk_label_set_text(data->size_on_disk_label, calculating);

        g_object_set_data(G_OBJECT(dlg), "calc_size", data);
        data->calc_size_thread = g_thread_new("calc_size", calc_size, data);
        data->update_label_timer = g_timeout_add(250, (GSourceFunc)on_update_labels, data);
    }

    g_signal_connect(dlg, "response", G_CALLBACK(on_dlg_response), dlg);
    g_signal_connect_swapped(gtk_builder_get_object(builder, "ok_button"),
                             "clicked",
                             G_CALLBACK(gtk_widget_destroy),
                             dlg);
    g_signal_connect_swapped(gtk_builder_get_object(builder, "cancel_button"),
                             "clicked",
                             G_CALLBACK(gtk_widget_destroy),
                             dlg);

    g_object_unref(builder);

    gtk_notebook_set_current_page(notebook, page);

    if (parent)
        gtk_window_set_transient_for(GTK_WINDOW(dlg), parent);
    return dlg;
}

static uid_t uid_from_name(const char* user_name)
{
    uid_t uid = -1;

    struct passwd* pw;
    pw = getpwnam(user_name);
    if (pw)
    {
        uid = pw->pw_uid;
    }
    else
    {
        uid = 0;
        const char* p;
        for (p = user_name; *p; ++p)
        {
            if (!g_ascii_isdigit(*p))
                return -1;
            uid *= 10;
            uid += (*p - '0');
        }
    }
    return uid;
}

static gid_t gid_from_name(const char* group_name)
{
    gid_t gid = -1;

    struct group* grp;
    grp = getgrnam(group_name);
    if (grp)
    {
        gid = grp->gr_gid;
    }
    else
    {
        gid = 0;
        const char* p;
        for (p = group_name; *p; ++p)
        {
            if (!g_ascii_isdigit(*p))
                return -1;
            gid *= 10;
            gid += (*p - '0');
        }
    }
    return gid;
}

static void on_dlg_response(GtkDialog* dialog, int response_id, void* user_data)
{
    uid_t uid = -1;
    gid_t gid = -1;
    GList* l;
    char* file_path;
    VFSFileInfo* file;
    GtkAllocation allocation;

    gtk_widget_get_allocation(GTK_WIDGET(dialog), &allocation);

    int width = allocation.width;
    int height = allocation.height;
    if (width && height)
    {
        char* str = g_strdup_printf("%d", width);
        xset_set("app_dlg", "s", str);
        g_free(str);
        str = g_strdup_printf("%d", height);
        xset_set("app_dlg", "z", str);
        g_free(str);
    }

    FilePropertiesDialogData* data =
        (FilePropertiesDialogData*)g_object_get_data(G_OBJECT(dialog), "DialogData");
    if (data)
    {
        if (data->update_label_timer)
            g_source_remove(data->update_label_timer);
        data->cancel = TRUE;

        if (data->calc_size_thread)
            g_thread_join(data->calc_size_thread);

        if (response_id == GTK_RESPONSE_OK)
        {
            bool mod_change;
            PtkFileTask* task;
            // change file dates
            char* cmd = NULL;
            char* quoted_time;
            char* quoted_path;
            const char* new_mtime = gtk_entry_get_text(data->mtime);
            if (!(new_mtime && new_mtime[0]) || !g_strcmp0(data->orig_mtime, new_mtime))
                new_mtime = NULL;
            const char* new_atime = gtk_entry_get_text(data->atime);
            if (!(new_atime && new_atime[0]) || !g_strcmp0(data->orig_atime, new_atime))
                new_atime = NULL;

            if ((new_mtime || new_atime) && data->file_list)
            {
                GString* gstr = g_string_new(NULL);
                for (l = data->file_list; l; l = l->next)
                {
                    file_path =
                        g_build_filename(data->dir_path, ((VFSFileInfo*)l->data)->name, NULL);
                    quoted_path = bash_quote(file_path);
                    g_string_append_printf(gstr, " %s", quoted_path);
                    g_free(file_path);
                    g_free(quoted_path);
                }

                if (new_mtime)
                {
                    quoted_time = bash_quote(new_mtime);
                    cmd = g_strdup_printf("touch --no-dereference --no-create -m -d %s%s",
                                          quoted_time,
                                          gstr->str);
                }
                if (new_atime)
                {
                    quoted_time = bash_quote(new_atime);
                    quoted_path = cmd; // temp str
                    cmd = g_strdup_printf("%s%stouch --no-dereference --no-create -a -d %s%s",
                                          cmd ? cmd : "",
                                          cmd ? "\n" : "",
                                          quoted_time,
                                          gstr->str);
                    g_free(quoted_path);
                }
                g_free(quoted_time);
                g_string_free(gstr, TRUE);
                if (cmd)
                {
                    task = ptk_file_exec_new(_("Change File Date"), "/", GTK_WIDGET(dialog), NULL);
                    task->task->exec_command = cmd;
                    task->task->exec_sync = TRUE;
                    task->task->exec_export = FALSE;
                    task->task->exec_show_output = TRUE;
                    task->task->exec_show_error = TRUE;
                    ptk_file_task_run(task);
                }
            }

            /* Set default action for mimetype */
            GtkWidget* open_with;
            if ((open_with = (GtkWidget*)g_object_get_data(G_OBJECT(dialog), "open_with")))
            {
                GtkTreeModel* model = gtk_combo_box_get_model(GTK_COMBO_BOX(open_with));
                GtkTreeIter it;

                if (model && gtk_combo_box_get_active_iter(GTK_COMBO_BOX(open_with), &it))
                {
                    char* action;
                    gtk_tree_model_get(model, &it, 2, &action, -1);
                    if (action)
                    {
                        file = (VFSFileInfo*)data->file_list->data;
                        VFSMimeType* mime = vfs_file_info_get_mime_type(file);
                        vfs_mime_type_set_default_action(mime, action);
                        vfs_mime_type_unref(mime);
                        g_free(action);
                    }
                }
            }

            /* Check if we need chown */
            const char* owner_name = gtk_entry_get_text(data->owner);
            if (owner_name && *owner_name &&
                (!data->owner_name || strcmp(owner_name, data->owner_name)))
            {
                uid = uid_from_name(owner_name);
                if (uid == -1)
                {
                    ptk_show_error(GTK_WINDOW(dialog), _("Error"), _("Invalid User"));
                    return;
                }
            }
            const char* group_name = gtk_entry_get_text(data->group);
            if (group_name && *group_name &&
                (!data->group_name || strcmp(group_name, data->group_name)))
            {
                gid = gid_from_name(group_name);
                if (gid == -1)
                {
                    ptk_show_error(GTK_WINDOW(dialog), _("Error"), _("Invalid Group"));
                    return;
                }
            }

            int i;
            for (i = 0; i < N_CHMOD_ACTIONS; ++i)
            {
                if (gtk_toggle_button_get_inconsistent(data->chmod_btns[i]))
                {
                    data->chmod_states[i] = 2; /* Don't touch this bit */
                }
                else if (data->chmod_states[i] != gtk_toggle_button_get_active(data->chmod_btns[i]))
                {
                    mod_change = TRUE;
                    data->chmod_states[i] = gtk_toggle_button_get_active(data->chmod_btns[i]);
                }
                else /* Don't change this bit */
                {
                    data->chmod_states[i] = 2;
                }
            }

            if (uid != -1 || gid != -1 || mod_change)
            {
                GList* file_list = NULL;
                for (l = data->file_list; l; l = l->next)
                {
                    file = (VFSFileInfo*)l->data;
                    file_path =
                        g_build_filename(data->dir_path, vfs_file_info_get_name(file), NULL);
                    file_list = g_list_prepend(file_list, file_path);
                }

                task = ptk_file_task_new(VFS_FILE_TASK_CHMOD_CHOWN,
                                         file_list,
                                         NULL,
                                         GTK_WINDOW(gtk_widget_get_parent(GTK_WIDGET(dialog))),
                                         NULL);
                // MOD
                ptk_file_task_set_recursive(
                    task,
                    gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->recurse)));
                if (mod_change)
                {
                    /* If the permissions of file has been changed by the user */
                    ptk_file_task_set_chmod(task, data->chmod_states);
                }
                /* For chown */
                ptk_file_task_set_chown(task, uid, gid);
                ptk_file_task_run(task);

                /*
                 * This file list will be freed by file operation, so we don't
                 * need to do this. Just set the pointer to NULL.
                 */
                data->file_list = NULL;
            }
        }

        g_free(data->owner_name);
        g_free(data->group_name);
        g_free(data->orig_mtime);
        g_free(data->orig_atime);
        /*
         *NOTE: File operation chmod/chown will free the list when it's done,
         *and we only need to free it when there is no file operation applyed.
         */
        g_slice_free(FilePropertiesDialogData, data);
    }

    gtk_widget_destroy(GTK_WIDGET(dialog));
}
