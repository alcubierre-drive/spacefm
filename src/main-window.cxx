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

#include <memory>
#include <string>
#include <string_view>

#include <fmt/core.h>

#include <filesystem>

#include <array>
#include <vector>

#include <optional>

#include <functional>

#include <ranges>

#include <malloc.h>

#include <cassert>

#include <fmt/format.h>

#include <glibmm.h>

#include <ztd/ztd.hxx>
#include <ztd/ztd_logger.hxx>

#include <nlohmann/json.hpp>

#include "compat/gtk4-porting.hxx"

#include "ptk/ptk-file-browser.hxx"
#include "types.hxx"

#include "ptk/ptk-location-view.hxx"

#include "main-window.hxx"

#include "ptk/ptk-dialog.hxx"
#include "ptk/ptk-keyboard.hxx"
#include "ptk/ptk-file-menu.hxx"
#include "ptk/ptk-utils.hxx"

#include "about.hxx"
#include "preference-dialog.hxx"

#include "xset/xset.hxx"

#include "settings/app.hxx"
#include "settings/disk-format.hxx"

#include "bookmarks.hxx"
#include "settings.hxx"
#include "file-search.hxx"

#include "autosave.hxx"

#include "vfs/vfs-user-dirs.hxx"
#include "vfs/vfs-utils.hxx"
#include "vfs/vfs-file-task.hxx"

#include "ptk/ptk-task-view.hxx"
#include "ptk/ptk-bookmark-view.hxx"

static void rebuild_menus(MainWindow* main_window);

static void on_folder_notebook_switch_pape(GtkNotebook* notebook, GtkWidget* page, u32 page_num,
                                           void* user_data);
static bool on_tab_drag_motion(GtkWidget* widget, GdkDragContext* drag_context, i32 x, i32 y,
                               u32 time, PtkFileBrowser* file_browser);

static bool on_main_window_keypress(MainWindow* main_window, GdkEvent* event, void* user_data);
static bool on_main_window_keypress_found_key(MainWindow* main_window, const xset_t& set);
static bool on_window_button_press_event(GtkWidget* widget, GdkEvent* event,
                                         MainWindow* main_window);
static void on_new_window_activate(GtkMenuItem* menuitem, void* user_data);
static void main_window_close(MainWindow* main_window);

static void on_preference_activate(GtkMenuItem* menuitem, void* user_data);
static void on_about_activate(GtkMenuItem* menuitem, void* user_data);
static void update_window_title(MainWindow* main_window);
static void on_update_window_title(GtkMenuItem* item, MainWindow* main_window);
static void on_fullscreen_activate(GtkMenuItem* menuitem, MainWindow* main_window);
static bool delayed_focus_file_browser(PtkFileBrowser* file_browser);

static GtkApplicationWindowClass* parent_class = nullptr;

static std::vector<MainWindow*> all_windows;

//  Drag & Drop/Clipboard targets
static GtkTargetEntry drag_targets[] = {{ztd::strdup("text/uri-list"), 0, 0}};

struct MainWindowClass
{
    GtkApplicationWindowClass parent;
};

static void main_window_class_init(MainWindowClass* klass);
static void main_window_init(MainWindow* main_window);
static void main_window_finalize(GObject* obj);
static void main_window_get_property(GObject* obj, u32 prop_id, GValue* value, GParamSpec* pspec);
static void main_window_set_property(GObject* obj, u32 prop_id, const GValue* value,
                                     GParamSpec* pspec);
static gboolean main_window_delete_event(GtkWidget* widget, GdkEventAny* event);
static gboolean main_window_window_state_event(GtkWidget* widget, GdkEventWindowState* event);

GType
main_window_get_type()
{
    static GType type = G_TYPE_INVALID;
    if (type == G_TYPE_INVALID)
    {
        static const GTypeInfo info = {
            sizeof(MainWindowClass),
            nullptr,
            nullptr,
            (GClassInitFunc)main_window_class_init,
            nullptr,
            nullptr,
            sizeof(MainWindow),
            0,
            (GInstanceInitFunc)main_window_init,
            nullptr,
        };
        type = g_type_register_static(GTK_TYPE_APPLICATION_WINDOW,
                                      "MainWindow",
                                      &info,
                                      GTypeFlags::G_TYPE_FLAG_NONE);
    }
    return type;
}

static void
main_window_class_init(MainWindowClass* klass)
{
    GObjectClass* object_class;
    GtkWidgetClass* widget_class;

    object_class = (GObjectClass*)klass;
    parent_class = (GtkApplicationWindowClass*)g_type_class_peek_parent(klass);

    object_class->set_property = main_window_set_property;
    object_class->get_property = main_window_get_property;
    object_class->finalize = main_window_finalize;

    widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->delete_event = main_window_delete_event;
    widget_class->window_state_event = main_window_window_state_event;

    /*  this works but desktop_window does not
    g_signal_new ( "task-notify",
                       G_TYPE_FROM_CLASS ( klass ),
                       GSignalMatchType::G_SIGNAL_RUN_FIRST,
                       0,
                       nullptr, nullptr,
                       g_cclosure_marshal_VOID__POINTER,
                       G_TYPE_NONE, 1, G_TYPE_POINTER );
    */
}

static void
on_devices_show(GtkMenuItem* item, MainWindow* main_window)
{
    (void)item;
    PtkFileBrowser* file_browser = main_window->current_file_browser();
    if (!file_browser)
    {
        return;
    }
    const xset::main_window_panel mode = main_window->panel_context[file_browser->panel() - 1];

    xset_set_b_panel_mode(file_browser->panel(),
                          xset::panel::show_devmon,
                          mode,
                          !file_browser->side_dev);
    update_views_all_windows(nullptr, file_browser);
    if (file_browser->side_dev)
    {
        gtk_widget_grab_focus(GTK_WIDGET(file_browser->side_dev));
    }
}

static void
on_open_url(GtkWidget* widget, MainWindow* main_window)
{
    (void)widget;
    PtkFileBrowser* file_browser = main_window->current_file_browser();
    const auto url = xset_get_s(xset::name::main_save_session);
    if (file_browser && url)
    {
        ptk_location_view_mount_network(file_browser, url.value(), true, true);
    }
}

static void
on_find_file_activate(GtkMenuItem* menuitem, void* user_data)
{
    (void)menuitem;
    MainWindow* main_window = MAIN_WINDOW(user_data);
    PtkFileBrowser* file_browser = main_window->current_file_browser();
    const auto& cwd = file_browser->cwd();

    const std::vector<std::filesystem::path> search_dirs{cwd};

    find_files(search_dirs);
}

static void
main_window_open_terminal(MainWindow* main_window)
{
    PtkFileBrowser* file_browser = main_window->current_file_browser();
    if (!file_browser)
    {
        return;
    }

    const auto main_term = xset_get_s(xset::name::main_terminal);
    if (!main_term)
    {
#if (GTK_MAJOR_VERSION == 4)
        GtkWidget* parent = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(file_browser)));
#elif (GTK_MAJOR_VERSION == 3)
        GtkWidget* parent = gtk_widget_get_toplevel(GTK_WIDGET(file_browser));
#endif

        ptk_show_error(GTK_WINDOW(parent),
                       "Terminal Not Available",
                       "Please set your terminal program in View|Preferences|Advanced");
        return;
    }

    // task
    PtkFileTask* ptask = ptk_file_exec_new("Open Terminal",
                                           file_browser->cwd(),
                                           GTK_WIDGET(file_browser),
                                           file_browser->task_view());

    const std::string terminal = Glib::find_program_in_path(main_term.value());
    if (terminal.empty())
    {
        ztd::logger::warn("Cannot locate terminal in $PATH : {}", main_term.value());
        return;
    }

    ptask->task->exec_command = terminal;
    ptask->task->exec_sync = false;
    ptask->task->exec_export = true;
    ptask->task->exec_browser = file_browser;
    ptk_file_task_run(ptask);
}

static void
on_open_terminal_activate(GtkMenuItem* menuitem, void* user_data)
{
    (void)menuitem;
    MainWindow* main_window = MAIN_WINDOW(user_data);
    main_window_open_terminal(main_window);
}

static void
on_quit_activate(GtkMenuItem* menuitem, void* user_data)
{
    (void)menuitem;
    main_window_delete_event(GTK_WIDGET(user_data), nullptr);
    // main_window_close( GTK_WIDGET( user_data ) );
}

void
main_window_rubberband_all()
{
    for (MainWindow* window : all_windows)
    {
        for (const panel_t p : PANELS)
        {
            GtkNotebook* notebook = window->get_panel_notebook(p);
            const i32 num_pages = gtk_notebook_get_n_pages(notebook);
            for (const auto i : std::views::iota(0z, num_pages))
            {
                PtkFileBrowser* a_browser =
                    PTK_FILE_BROWSER_REINTERPRET(gtk_notebook_get_nth_page(notebook, i));
                if (a_browser->is_view_mode(ptk::file_browser::view_mode::list_view))
                {
                    gtk_tree_view_set_rubber_banding(GTK_TREE_VIEW(a_browser->folder_view()),
                                                     xset_get_b(xset::name::rubberband));
                }
            }
        }
    }
}

void
main_window_refresh_all()
{
    for (MainWindow* window : all_windows)
    {
        for (const panel_t p : PANELS)
        {
            GtkNotebook* notebook = window->get_panel_notebook(p);
            const i32 num_pages = gtk_notebook_get_n_pages(notebook);
            for (const auto i : std::views::iota(0z, num_pages))
            {
                PtkFileBrowser* a_browser =
                    PTK_FILE_BROWSER_REINTERPRET(gtk_notebook_get_nth_page(notebook, i));
                a_browser->refresh();
            }
        }
    }
}

void
MainWindow::update_window_icon() noexcept
{
    ptk_set_window_icon(GTK_WINDOW(this));
}

void
main_window_close_all_invalid_tabs()
{
    // do all windows all panels all tabs
    for (MainWindow* window : all_windows)
    {
        for (const panel_t p : PANELS)
        {
            GtkNotebook* notebook = window->get_panel_notebook(p);
            const i32 pages = gtk_notebook_get_n_pages(notebook);
            for (const auto cur_tabx : std::views::iota(0z, pages))
            {
                PtkFileBrowser* browser =
                    PTK_FILE_BROWSER_REINTERPRET(gtk_notebook_get_nth_page(notebook, cur_tabx));

                // will close all tabs that no longer exist on the filesystem
                browser->refresh();
            }
        }
    }
}

void
main_window_refresh_all_tabs_matching(const std::filesystem::path& path)
{
    (void)path;
    // This function actually closes the tabs because refresh does not work.
    // dir objects have multiple refs and unreffing them all would not finalize
    // the dir object for unknown reason.

    // This breaks auto open of tabs on automount
}

void
main_window_rebuild_all_toolbars(PtkFileBrowser* file_browser)
{
    // ztd::logger::info("main_window_rebuild_all_toolbars");

    // do this browser first
    if (file_browser)
    {
        file_browser->rebuild_toolbars();
    }

    // do all windows all panels all tabs
    for (MainWindow* window : all_windows)
    {
        for (const panel_t p : PANELS)
        {
            GtkNotebook* notebook = window->get_panel_notebook(p);
            const i32 pages = gtk_notebook_get_n_pages(notebook);
            for (const auto cur_tabx : std::views::iota(0z, pages))
            {
                PtkFileBrowser* a_browser =
                    PTK_FILE_BROWSER_REINTERPRET(gtk_notebook_get_nth_page(notebook, cur_tabx));
                if (a_browser != file_browser)
                {
                    a_browser->rebuild_toolbars();
                }
            }
        }
    }
    autosave_request_add();
}

void
update_views_all_windows(GtkWidget* item, PtkFileBrowser* file_browser)
{
    (void)item;
    // ztd::logger::info("update_views_all_windows");
    // do this browser first
    if (!file_browser)
    {
        return;
    }
    const panel_t p = file_browser->panel();

    file_browser->update_views();

    // do other windows
    for (MainWindow* window : all_windows)
    {
        if (gtk_widget_get_visible(GTK_WIDGET(window->get_panel_notebook(p))))
        {
            GtkNotebook* notebook = window->get_panel_notebook(p);
            const i32 cur_tabx = gtk_notebook_get_current_page(notebook);
            if (cur_tabx != -1)
            {
                PtkFileBrowser* a_browser =
                    PTK_FILE_BROWSER_REINTERPRET(gtk_notebook_get_nth_page(notebook, cur_tabx));
                if (a_browser != file_browser)
                {
                    a_browser->update_views();
                }
            }
        }
    }
    autosave_request_add();
}

void
main_window_reload_thumbnails_all_windows()
{
    // update all windows/all panels/all browsers
    for (MainWindow* window : all_windows)
    {
        for (const panel_t p : PANELS)
        {
            GtkNotebook* notebook = window->get_panel_notebook(p);
            const i32 num_pages = gtk_notebook_get_n_pages(notebook);
            for (const auto i : std::views::iota(0z, num_pages))
            {
                PtkFileBrowser* file_browser =
                    PTK_FILE_BROWSER_REINTERPRET(gtk_notebook_get_nth_page(notebook, i));
                file_browser->show_thumbnails(
                    app_settings.show_thumbnail() ? app_settings.max_thumb_size() : 0);
            }
        }
    }

    /* Ensuring free space at the end of the heap is freed to the OS,
     * mainly to deal with the possibility thousands of large thumbnails
     * have been freed but the memory not actually released by SpaceFM */
    malloc_trim(0);
}

void
main_window_toggle_thumbnails_all_windows()
{
    // toggle
    app_settings.show_thumbnail(!app_settings.show_thumbnail());

    main_window_reload_thumbnails_all_windows();
}

void
MainWindow::focus_panel(const panel_t panel) noexcept
{
    panel_t panel_focus;
    panel_t panel_hide;

    switch (panel)
    {
        case panel_control_code_prev:
            // prev
            panel_focus = this->curpanel - 1;
            do
            {
                if (panel_focus < panel_1)
                {
                    panel_focus = panel_4;
                }
                if (xset_get_b_panel(panel_focus, xset::panel::show))
                {
                    break;
                }
                panel_focus--;
            } while (panel_focus != this->curpanel - 1);
            break;
        case panel_control_code_next:
            // next
            panel_focus = this->curpanel + 1;
            do
            {
                if (!is_valid_panel(panel_focus))
                {
                    panel_focus = panel_1;
                }
                if (xset_get_b_panel(panel_focus, xset::panel::show))
                {
                    break;
                }
                panel_focus++;
            } while (panel_focus != this->curpanel + 1);
            break;
        case panel_control_code_hide:
            // hide
            panel_hide = this->curpanel;
            panel_focus = this->curpanel + 1;
            do
            {
                if (!is_valid_panel(panel_focus))
                {
                    panel_focus = panel_1;
                }
                if (xset_get_b_panel(panel_focus, xset::panel::show))
                {
                    break;
                }
                panel_focus++;
            } while (panel_focus != panel_hide);
            if (panel_focus == panel_hide)
            {
                panel_focus = 0;
            }
            break;
        default:
            panel_focus = panel;
            break;
    }

    if (is_valid_panel(panel_focus))
    {
        if (gtk_widget_get_visible(GTK_WIDGET(this->get_panel_notebook(panel_focus))))
        {
            gtk_widget_grab_focus(GTK_WIDGET(this->get_panel_notebook(panel_focus)));
            this->curpanel = panel_focus;
            this->notebook = this->get_panel_notebook(panel_focus);
            PtkFileBrowser* file_browser = this->current_file_browser();
            if (file_browser)
            {
                gtk_widget_grab_focus(GTK_WIDGET(file_browser->folder_view()));
                set_panel_focus(this, file_browser);
            }
        }
        else if (panel != panel_control_code_hide)
        {
            xset_set_b_panel(panel_focus, xset::panel::show, true);
            show_panels_all_windows(nullptr, this);
            gtk_widget_grab_focus(GTK_WIDGET(this->get_panel_notebook(panel_focus)));
            this->curpanel = panel_focus;
            this->notebook = this->get_panel_notebook(panel_focus);
            PtkFileBrowser* file_browser = this->current_file_browser();
            if (file_browser)
            {
                gtk_widget_grab_focus(GTK_WIDGET(file_browser->folder_view()));
                set_panel_focus(this, file_browser);
            }
        }
        else if (panel == panel_control_code_hide)
        {
            xset_set_b_panel(panel_hide, xset::panel::show, false);
            show_panels_all_windows(nullptr, this);
        }
    }
}

static void
on_focus_panel(GtkMenuItem* item, void* user_data)
{
    const panel_t panel = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "panel"));
    MainWindow* main_window = MAIN_WINDOW(user_data);
    main_window->focus_panel(panel);
}

void
show_panels_all_windows(GtkMenuItem* item, MainWindow* main_window)
{
    (void)item;
    // do this window first
    main_window->panel_change = true;
    main_window->show_panels();

    // do other windows
    main_window->panel_change = false; // do not save columns for other windows
    for (MainWindow* window : all_windows)
    {
        if (main_window != window)
        {
            main_window->show_panels();
        }
    }

    autosave_request_add();
}

void
MainWindow::show_panels() noexcept
{
    // start the index at 1 for clarity
    std::array<bool, 5> show;

    // save column widths and side sliders of visible panels
    if (this->panel_change)
    {
        for (const panel_t p : PANELS)
        {
            if (gtk_widget_get_visible(GTK_WIDGET(this->get_panel_notebook(p))))
            {
                const tab_t cur_tabx = gtk_notebook_get_current_page(this->get_panel_notebook(p));
                if (cur_tabx != -1)
                {
                    PtkFileBrowser* file_browser = PTK_FILE_BROWSER_REINTERPRET(
                        gtk_notebook_get_nth_page(this->get_panel_notebook(p), cur_tabx));
                    if (file_browser)
                    {
                        if (file_browser->is_view_mode(ptk::file_browser::view_mode::list_view))
                        {
                            file_browser->save_column_widths(
                                GTK_TREE_VIEW(file_browser->folder_view()));
                        }
                        // file_browser->slider_release(nullptr);
                    }
                }
            }
        }
    }

    // which panels to show
    for (const panel_t p : PANELS)
    {
        show[p] = xset_get_b_panel(p, xset::panel::show);
    }

    // TODO - write and move this to MainWindow constructor
    if (this->panel_context.empty())
    {
        this->panel_context = {
            {panel_1, xset::main_window_panel::panel_neither},
            {panel_2, xset::main_window_panel::panel_neither},
            {panel_3, xset::main_window_panel::panel_neither},
            {panel_4, xset::main_window_panel::panel_neither},
        };
    }

    bool horiz;
    bool vert;
    for (const panel_t p : PANELS)
    {
        // panel context - how panels share horiz and vert space with other panels
        switch (p)
        {
            case panel_1:
                horiz = show[panel_2];
                vert = show[panel_3] || show[panel_4];
                break;
            case panel_2:
                horiz = show[panel_1];
                vert = show[panel_3] || show[panel_4];
                break;
            case panel_3:
                horiz = show[panel_4];
                vert = show[panel_1] || show[panel_2];
                break;
            case panel_4:
                horiz = show[panel_3];
                vert = show[panel_1] || show[panel_2];
                break;
        }

        if (horiz && vert)
        {
            this->panel_context.at(p) = xset::main_window_panel::panel_both;
        }
        else if (horiz)
        {
            this->panel_context.at(p) = xset::main_window_panel::panel_horiz;
        }
        else if (vert)
        {
            this->panel_context.at(p) = xset::main_window_panel::panel_vert;
        }
        else
        {
            this->panel_context.at(p) = xset::main_window_panel::panel_neither;
        }

        if (show[p])
        {
            // shown
            // test if panel and mode exists
            xset_t set;

            const auto mode = this->panel_context.at(p);

            set =
                xset_is(xset::get_xsetname_from_panel_mode(p, xset::panel::slider_positions, mode));
            if (!set)
            {
                // ztd::logger::warn("no config for {}, {}", p, INT(mode));

                xset_set_b_panel_mode(p,
                                      xset::panel::show_toolbox,
                                      mode,
                                      xset_get_b_panel(p, xset::panel::show_toolbox));
                xset_set_b_panel_mode(p,
                                      xset::panel::show_devmon,
                                      mode,
                                      xset_get_b_panel(p, xset::panel::show_devmon));
                xset_set_b_panel_mode(p,
                                      xset::panel::show_dirtree,
                                      mode,
                                      xset_get_b_panel(p, xset::panel::show_dirtree));
                xset_set_b_panel_mode(p,
                                      xset::panel::show_sidebar,
                                      mode,
                                      xset_get_b_panel(p, xset::panel::show_sidebar));
                xset_set_b_panel_mode(p,
                                      xset::panel::detcol_name,
                                      mode,
                                      xset_get_b_panel(p, xset::panel::detcol_name));
                xset_set_b_panel_mode(p,
                                      xset::panel::detcol_size,
                                      mode,
                                      xset_get_b_panel(p, xset::panel::detcol_size));
                xset_set_b_panel_mode(p,
                                      xset::panel::detcol_bytes,
                                      mode,
                                      xset_get_b_panel(p, xset::panel::detcol_bytes));
                xset_set_b_panel_mode(p,
                                      xset::panel::detcol_type,
                                      mode,
                                      xset_get_b_panel(p, xset::panel::detcol_type));
                xset_set_b_panel_mode(p,
                                      xset::panel::detcol_mime,
                                      mode,
                                      xset_get_b_panel(p, xset::panel::detcol_mime));
                xset_set_b_panel_mode(p,
                                      xset::panel::detcol_perm,
                                      mode,
                                      xset_get_b_panel(p, xset::panel::detcol_perm));
                xset_set_b_panel_mode(p,
                                      xset::panel::detcol_owner,
                                      mode,
                                      xset_get_b_panel(p, xset::panel::detcol_owner));
                xset_set_b_panel_mode(p,
                                      xset::panel::detcol_group,
                                      mode,
                                      xset_get_b_panel(p, xset::panel::detcol_group));
                xset_set_b_panel_mode(p,
                                      xset::panel::detcol_atime,
                                      mode,
                                      xset_get_b_panel(p, xset::panel::detcol_atime));
                xset_set_b_panel_mode(p,
                                      xset::panel::detcol_btime,
                                      mode,
                                      xset_get_b_panel(p, xset::panel::detcol_btime));
                xset_set_b_panel_mode(p,
                                      xset::panel::detcol_ctime,
                                      mode,
                                      xset_get_b_panel(p, xset::panel::detcol_ctime));
                xset_set_b_panel_mode(p,
                                      xset::panel::detcol_mtime,
                                      mode,
                                      xset_get_b_panel(p, xset::panel::detcol_mtime));
                const xset_t set_old = xset_get_panel(p, xset::panel::slider_positions);
                set = xset_get_panel_mode(p, xset::panel::slider_positions, mode);
                set->x = set_old->x ? set_old->x : "0";
                set->y = set_old->y ? set_old->y : "0";
                set->s = set_old->s ? set_old->s : "0";
            }
            // load dynamic slider positions for this panel context
            this->panel_slide_x[p - 1] = set->x ? std::stoi(set->x.value()) : 0;
            this->panel_slide_y[p - 1] = set->y ? std::stoi(set->y.value()) : 0;
            this->panel_slide_s[p - 1] = set->s ? std::stoi(set->s.value()) : 0;
            // ztd::logger::info("loaded panel {}", p);
            if (!gtk_notebook_get_n_pages(this->get_panel_notebook(p)))
            {
                this->notebook = this->get_panel_notebook(p);
                this->curpanel = p;
                // load saved tabs
                bool tab_added = false;
                set = xset_get_panel(p, xset::panel::show);
                if ((set->s && app_settings.load_saved_tabs()) || set->ob1)
                {
                    // set->ob1 is preload path

                    const std::string tabs_add =
                        fmt::format("{}{}{}",
                                    set->s && app_settings.load_saved_tabs() ? set->s.value() : "",
                                    set->ob1 ? CONFIG_FILE_TABS_DELIM : "",
                                    set->ob1 ? set->ob1 : "");

                    const std::vector<std::string> tab_dirs =
                        ztd::split(tabs_add, CONFIG_FILE_TABS_DELIM);

                    for (const std::string_view tab_dir : tab_dirs)
                    {
                        if (tab_dir.empty())
                        {
                            continue;
                        }

                        std::filesystem::path folder_path;
                        if (std::filesystem::is_directory(tab_dir))
                        {
                            folder_path = tab_dir;
                        }
                        else
                        {
                            folder_path = vfs::user_dirs->home_dir();
                        }
                        this->new_tab(folder_path);
                        tab_added = true;
                    }
                    if (set->x && !set->ob1)
                    {
                        // set current tab
                        const tab_t cur_tabx = std::stoi(set->x.value());
                        if (cur_tabx >= 0 &&
                            cur_tabx < gtk_notebook_get_n_pages(this->get_panel_notebook(p)))
                        {
                            gtk_notebook_set_current_page(this->get_panel_notebook(p), cur_tabx);
                            PtkFileBrowser* file_browser = PTK_FILE_BROWSER_REINTERPRET(
                                gtk_notebook_get_nth_page(this->get_panel_notebook(p), cur_tabx));
                            // if (file_browser->folder_view)
                            //      gtk_widget_grab_focus(file_browser->folder_view);
                            // ztd::logger::info("call delayed (showpanels) #{} {} window={}", cur_tabx, fmt::ptr(file_browser->folder_view_), fmt::ptr(main_window));
                            g_idle_add((GSourceFunc)delayed_focus_file_browser, file_browser);
                        }
                    }
                    std::free(set->ob1);
                    set->ob1 = nullptr;
                }
                if (!tab_added)
                {
                    // open default tab
                    std::filesystem::path folder_path;
                    const auto default_path = xset_get_s(xset::name::go_set_default);
                    if (default_path)
                    {
                        folder_path = default_path.value();
                    }
                    else
                    {
                        folder_path = vfs::user_dirs->home_dir();
                    }
                    this->new_tab(folder_path);
                }
            }
            gtk_widget_show(GTK_WIDGET(this->get_panel_notebook(p)));
        }
        else
        {
            // not shown
            gtk_widget_hide(GTK_WIDGET(this->get_panel_notebook(p)));
        }
    }
    if (show[panel_1] || show[panel_2])
    {
        gtk_widget_show(GTK_WIDGET(this->hpane_top));
    }
    else
    {
        gtk_widget_hide(GTK_WIDGET(this->hpane_top));
    }
    if (show[panel_3] || show[panel_4])
    {
        gtk_widget_show(GTK_WIDGET(this->hpane_bottom));
    }
    else
    {
        gtk_widget_hide(GTK_WIDGET(this->hpane_bottom));
    }

    // current panel hidden?
    if (!xset_get_b_panel(this->curpanel, xset::panel::show))
    {
        for (const panel_t p : PANELS)
        {
            if (xset_get_b_panel(p, xset::panel::show))
            {
                this->curpanel = p;
                this->notebook = this->get_panel_notebook(p);
                const tab_t cur_tabx = gtk_notebook_get_current_page(this->notebook);
                PtkFileBrowser* file_browser = PTK_FILE_BROWSER_REINTERPRET(
                    gtk_notebook_get_nth_page(this->notebook, cur_tabx));
                if (!file_browser)
                {
                    continue;
                }
                // if (file_browser->folder_view)
                gtk_widget_grab_focus(file_browser->folder_view());
                break;
            }
        }
    }
    set_panel_focus(this, nullptr);

    // update views all panels
    for (const panel_t p : PANELS)
    {
        if (show[p])
        {
            const tab_t cur_tabx = gtk_notebook_get_current_page(this->get_panel_notebook(p));
            if (cur_tabx != -1)
            {
                PtkFileBrowser* file_browser = PTK_FILE_BROWSER_REINTERPRET(
                    gtk_notebook_get_nth_page(this->get_panel_notebook(p), cur_tabx));
                if (file_browser)
                {
                    file_browser->update_views();
                }
            }
        }
    }
}

static bool
on_menu_bar_event(GtkWidget* widget, GdkEvent* event, MainWindow* main_window)
{
    (void)widget;
    (void)event;
    rebuild_menus(main_window);
    return false;
}

static bool
bookmark_menu_keypress(GtkWidget* widget, GdkEvent* event, void* user_data)
{
    (void)event;
    (void)user_data;

    if (widget)
    {
        const std::string file_path =
            static_cast<const char*>(g_object_get_data(G_OBJECT(widget), "path"));

        if (file_path.empty())
        {
            return false;
        }

        const auto file_browser =
            static_cast<PtkFileBrowser*>(g_object_get_data(G_OBJECT(widget), "file_browser"));
        MainWindow* main_window = file_browser->main_window();

        main_window->new_tab(file_path);

        return true;
    }

    return false;
}

static void
rebuild_menu_file(MainWindow* main_window, PtkFileBrowser* file_browser)
{
#if (GTK_MAJOR_VERSION == 4)
    GtkEventController* accel_group = gtk_shortcut_controller_new();
#elif (GTK_MAJOR_VERSION == 3)
    GtkAccelGroup* accel_group = gtk_accel_group_new();
#endif

    GtkWidget* newmenu = gtk_menu_new();
    xset_set_cb(xset::name::main_new_window, (GFunc)on_new_window_activate, main_window);
    xset_set_cb(xset::name::main_search, (GFunc)on_find_file_activate, main_window);
    xset_set_cb(xset::name::main_terminal, (GFunc)on_open_terminal_activate, main_window);
    xset_set_cb(xset::name::main_save_session, (GFunc)on_open_url, main_window);
    xset_set_cb(xset::name::main_exit, (GFunc)on_quit_activate, main_window);
    xset_add_menu(file_browser,
                  newmenu,
                  accel_group,
                  {
                      xset::name::main_save_session,
                      xset::name::main_search,
                      xset::name::separator,
                      xset::name::main_terminal,
                      xset::name::main_new_window,
                      xset::name::separator,
                      xset::name::main_save_tabs,
                      xset::name::separator,
                      xset::name::main_exit,
                  });
    gtk_widget_show_all(GTK_WIDGET(newmenu));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(main_window->file_menu_item), newmenu);
}

static void
rebuild_menu_view(MainWindow* main_window, PtkFileBrowser* file_browser)
{
    GtkWidget* newmenu = gtk_menu_new();
    xset_set_cb(xset::name::main_prefs, (GFunc)on_preference_activate, main_window);
    xset_set_cb(xset::name::main_full, (GFunc)on_fullscreen_activate, main_window);
    xset_set_cb(xset::name::main_title, (GFunc)on_update_window_title, main_window);

    i32 vis_count = 0;
    for (const panel_t p : PANELS)
    {
        if (xset_get_b_panel(p, xset::panel::show))
        {
            vis_count++;
        }
    }
    if (!vis_count)
    {
        xset_set_b_panel(1, xset::panel::show, true);
        vis_count++;
    }

    xset_t set;

    set = xset_get(xset::name::panel1_show);
    xset_set_cb(set, (GFunc)show_panels_all_windows, main_window);
    set->disable = (main_window->curpanel == 1 && vis_count == 1);
    set = xset_get(xset::name::panel2_show);
    xset_set_cb(set, (GFunc)show_panels_all_windows, main_window);
    set->disable = (main_window->curpanel == 2 && vis_count == 1);
    set = xset_get(xset::name::panel3_show);
    xset_set_cb(set, (GFunc)show_panels_all_windows, main_window);
    set->disable = (main_window->curpanel == 3 && vis_count == 1);
    set = xset_get(xset::name::panel4_show);
    xset_set_cb(set, (GFunc)show_panels_all_windows, main_window);
    set->disable = (main_window->curpanel == 4 && vis_count == 1);

    set = xset_get(xset::name::panel_prev);
    xset_set_cb(set, (GFunc)on_focus_panel, main_window);
    xset_set_ob1_int(set, "panel", panel_control_code_prev);
    set->disable = (vis_count == 1);
    set = xset_get(xset::name::panel_next);
    xset_set_cb(set, (GFunc)on_focus_panel, main_window);
    xset_set_ob1_int(set, "panel", panel_control_code_next);
    set->disable = (vis_count == 1);
    set = xset_get(xset::name::panel_hide);
    xset_set_cb(set, (GFunc)on_focus_panel, main_window);
    xset_set_ob1_int(set, "panel", panel_control_code_hide);
    set->disable = (vis_count == 1);
    set = xset_get(xset::name::panel_1);
    xset_set_cb(set, (GFunc)on_focus_panel, main_window);
    xset_set_ob1_int(set, "panel", panel_1);
    set->disable = (main_window->curpanel == 1);
    set = xset_get(xset::name::panel_2);
    xset_set_cb(set, (GFunc)on_focus_panel, main_window);
    xset_set_ob1_int(set, "panel", panel_2);
    set->disable = (main_window->curpanel == 2);
    set = xset_get(xset::name::panel_3);
    xset_set_cb(set, (GFunc)on_focus_panel, main_window);
    xset_set_ob1_int(set, "panel", panel_3);
    set->disable = (main_window->curpanel == 3);
    set = xset_get(xset::name::panel_4);
    xset_set_cb(set, (GFunc)on_focus_panel, main_window);
    xset_set_ob1_int(set, "panel", panel_4);
    set->disable = (main_window->curpanel == 4);

#if (GTK_MAJOR_VERSION == 4)
    GtkEventController* accel_group = gtk_shortcut_controller_new();
#elif (GTK_MAJOR_VERSION == 3)
    GtkAccelGroup* accel_group = gtk_accel_group_new();
#endif

    ptk_task_view_prepare_menu(main_window, newmenu);

    xset_add_menu(file_browser,
                  newmenu,
                  accel_group,
                  {
                      xset::name::panel1_show,
                      xset::name::panel2_show,
                      xset::name::panel3_show,
                      xset::name::panel4_show,
                      xset::name::main_focus_panel,
                  });

    // Panel View submenu
    ptk_file_menu_add_panel_view_menu(file_browser, newmenu, accel_group);

    xset_add_menu(file_browser,
                  newmenu,
                  accel_group,
                  {
                      xset::name::separator,
                      xset::name::main_tasks,
                      xset::name::separator,
                      xset::name::main_title,
                      xset::name::main_full,
                      xset::name::separator,
                      xset::name::main_prefs,
                  });
    gtk_widget_show_all(GTK_WIDGET(newmenu));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(main_window->view_menu_item), newmenu);
}

static void
rebuild_menu_device(MainWindow* main_window, PtkFileBrowser* file_browser)
{
    GtkWidget* newmenu = gtk_menu_new();

#if (GTK_MAJOR_VERSION == 4)
    GtkEventController* accel_group = gtk_shortcut_controller_new();
#elif (GTK_MAJOR_VERSION == 3)
    GtkAccelGroup* accel_group = gtk_accel_group_new();
#endif

    xset_t set;

    set = xset_get(xset::name::main_dev);
    xset_set_cb(set, (GFunc)on_devices_show, main_window);
    set->b = file_browser->side_dev ? xset::b::xtrue : xset::b::unset;
    xset_add_menuitem(file_browser, newmenu, accel_group, set);

    set = xset_get(xset::name::separator);
    xset_add_menuitem(file_browser, newmenu, accel_group, set);

    ptk_location_view_dev_menu(GTK_WIDGET(file_browser), file_browser, newmenu);

    set = xset_get(xset::name::separator);
    xset_add_menuitem(file_browser, newmenu, accel_group, set);

    set = xset_get(xset::name::dev_menu_settings);
    xset_add_menuitem(file_browser, newmenu, accel_group, set);

    // show all
    gtk_widget_show_all(GTK_WIDGET(newmenu));

    main_window->dev_menu = newmenu;
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(main_window->dev_menu_item), main_window->dev_menu);
}

static void
rebuild_menu_bookmarks(MainWindow* main_window, PtkFileBrowser* file_browser)
{
#if (GTK_MAJOR_VERSION == 4)
    GtkEventController* accel_group = gtk_shortcut_controller_new();
#elif (GTK_MAJOR_VERSION == 3)
    GtkAccelGroup* accel_group = gtk_accel_group_new();
#endif

    GtkWidget* newmenu = gtk_menu_new();
    xset_t set = xset_get(xset::name::book_add);
    xset_set_cb(set, (GFunc)ptk_bookmark_view_add_bookmark_cb, file_browser);
    set->disable = false;
    xset_add_menuitem(file_browser, newmenu, accel_group, set);
    gtk_menu_shell_append(GTK_MENU_SHELL(newmenu), gtk_separator_menu_item_new());

    // Add All Bookmarks
    for (auto [book_path, book_name] : get_all_bookmarks())
    {
        GtkWidget* item = gtk_menu_item_new_with_label(book_path.c_str());

        g_object_set_data(G_OBJECT(item), "file_browser", file_browser);
        g_object_set_data(G_OBJECT(item), "path", ztd::strdup(book_path));
        g_object_set_data(G_OBJECT(item), "name", ztd::strdup(book_name));

        g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(bookmark_menu_keypress), nullptr);

        gtk_widget_set_sensitive(item, true);
        gtk_menu_shell_append(GTK_MENU_SHELL(newmenu), item);
    }

    gtk_widget_show_all(GTK_WIDGET(newmenu));
    // clang-format off
    g_signal_connect(G_OBJECT(newmenu), "key-press-event", G_CALLBACK(bookmark_menu_keypress), nullptr);
    // clang-format on
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(main_window->book_menu_item), newmenu);
}

static void
rebuild_menu_help(MainWindow* main_window, PtkFileBrowser* file_browser)
{
#if (GTK_MAJOR_VERSION == 4)
    GtkEventController* accel_group = gtk_shortcut_controller_new();
#elif (GTK_MAJOR_VERSION == 3)
    GtkAccelGroup* accel_group = gtk_accel_group_new();
#endif

    GtkWidget* newmenu = gtk_menu_new();
    xset_set_cb(xset::name::main_about, (GFunc)on_about_activate, main_window);
    xset_add_menu(file_browser, newmenu, accel_group, {xset::name::main_about});
    gtk_widget_show_all(GTK_WIDGET(newmenu));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(main_window->help_menu_item), newmenu);
}

static void
rebuild_menus(MainWindow* main_window)
{
    if (main_window == nullptr)
    {
        main_window = main_window_get_last_active();
        if (main_window == nullptr)
        {
            return;
        }
    }

    // ztd::logger::debug("rebuild_menus()");

    PtkFileBrowser* file_browser = main_window->current_file_browser();

    if (!file_browser)
    {
        return;
    }

    // File
    rebuild_menu_file(main_window, file_browser);

    // View
    rebuild_menu_view(main_window, file_browser);

    // Devices
    rebuild_menu_device(main_window, file_browser);

    // Bookmarks
    rebuild_menu_bookmarks(main_window, file_browser);

    // Help
    rebuild_menu_help(main_window, file_browser);

    // ztd::logger::debug("rebuild_menus()  DONE");
}

static void
main_window_init(MainWindow* main_window)
{
    main_window->configure_evt_timer = 0;
    main_window->fullscreen = false;
    main_window->opened_maximized = app_settings.maximized();
    main_window->maximized = app_settings.maximized();

    /* this is used to limit the scope of gtk_grab and modal dialogs */
    main_window->wgroup = gtk_window_group_new();
    gtk_window_group_add_window(main_window->wgroup, GTK_WINDOW(main_window));

    /* Add to total window count */
    all_windows.emplace_back(main_window);

    // g_signal_connect(G_OBJECT(main_window), "task-notify", G_CALLBACK(ptk_file_task_notify_handler), nullptr);

    /* Start building GUI */
    main_window->update_window_icon();

    main_window->main_vbox = GTK_BOX(gtk_box_new(GtkOrientation::GTK_ORIENTATION_VERTICAL, 0));

#if (GTK_MAJOR_VERSION == 4)
    gtk_box_prepend(GTK_BOX(main_window), GTK_WIDGET(main_window->main_vbox));
#elif (GTK_MAJOR_VERSION == 3)
    gtk_container_add(GTK_CONTAINER(main_window), GTK_WIDGET(main_window->main_vbox));
#endif

    // Create menu bar
#if (GTK_MAJOR_VERSION == 4)
    main_window->accel_group = gtk_shortcut_controller_new();
#elif (GTK_MAJOR_VERSION == 3)
    main_window->accel_group = gtk_accel_group_new();
#endif
    main_window->menu_bar = gtk_menu_bar_new();
    GtkBox* menu_hbox = GTK_BOX(gtk_box_new(GtkOrientation::GTK_ORIENTATION_HORIZONTAL, 0));
    gtk_box_pack_start(menu_hbox, main_window->menu_bar, true, true, 0);

    gtk_box_pack_start(main_window->main_vbox, GTK_WIDGET(menu_hbox), false, false, 0);

    main_window->file_menu_item = gtk_menu_item_new_with_mnemonic("_File");
    gtk_menu_shell_append(GTK_MENU_SHELL(main_window->menu_bar), main_window->file_menu_item);

    main_window->view_menu_item = gtk_menu_item_new_with_mnemonic("_View");
    gtk_menu_shell_append(GTK_MENU_SHELL(main_window->menu_bar), main_window->view_menu_item);

    main_window->dev_menu_item = gtk_menu_item_new_with_mnemonic("_Devices");
    gtk_menu_shell_append(GTK_MENU_SHELL(main_window->menu_bar), main_window->dev_menu_item);

    main_window->book_menu_item = gtk_menu_item_new_with_mnemonic("_Bookmarks");
    gtk_menu_shell_append(GTK_MENU_SHELL(main_window->menu_bar), main_window->book_menu_item);

    main_window->help_menu_item = gtk_menu_item_new_with_mnemonic("_Help");
    gtk_menu_shell_append(GTK_MENU_SHELL(main_window->menu_bar), main_window->help_menu_item);

    rebuild_menus(main_window);

    /* Create client area */
    // clang-format off
    main_window->task_vpane = GTK_PANED(gtk_paned_new(GtkOrientation::GTK_ORIENTATION_VERTICAL));
    main_window->vpane = GTK_PANED(gtk_paned_new(GtkOrientation::GTK_ORIENTATION_VERTICAL));
    main_window->hpane_top = GTK_PANED(gtk_paned_new(GtkOrientation::GTK_ORIENTATION_HORIZONTAL));
    main_window->hpane_bottom = GTK_PANED(gtk_paned_new(GtkOrientation::GTK_ORIENTATION_HORIZONTAL));
    // clang-format on

    for (const panel_t p : PANELS)
    {
        GtkNotebook* notebook = GTK_NOTEBOOK(gtk_notebook_new());
        gtk_notebook_set_show_border(notebook, false);
        gtk_notebook_set_scrollable(notebook, true);

        // clang-format off
        g_signal_connect(G_OBJECT(notebook), "switch-page", G_CALLBACK(on_folder_notebook_switch_pape), main_window);
        // clang-format on

        main_window->panels[p - 1] = notebook;
    }

    main_window->task_scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new(nullptr, nullptr));

    // clang-format off
    gtk_paned_pack1(main_window->hpane_top, GTK_WIDGET(main_window->get_panel_notebook(panel_1)), false, true);
    gtk_paned_pack2(main_window->hpane_top, GTK_WIDGET(main_window->get_panel_notebook(panel_2)), true, true);
    gtk_paned_pack1(main_window->hpane_bottom, GTK_WIDGET(main_window->get_panel_notebook(panel_3)), false, true);
    gtk_paned_pack2(main_window->hpane_bottom, GTK_WIDGET(main_window->get_panel_notebook(panel_4)), true, true);

    gtk_paned_pack1(main_window->vpane, GTK_WIDGET(main_window->hpane_top), false, true);
    gtk_paned_pack2(main_window->vpane, GTK_WIDGET(main_window->hpane_bottom), true, true);

    gtk_paned_pack1(main_window->task_vpane, GTK_WIDGET(main_window->vpane), true, true);
    gtk_paned_pack2(main_window->task_vpane, GTK_WIDGET(main_window->task_scroll), false, true);

    gtk_box_pack_start(main_window->main_vbox, GTK_WIDGET(main_window->task_vpane), true, true, 0);
    // clang-format off

    main_window->notebook = main_window->get_panel_notebook(panel_1);
    main_window->curpanel = 1;

    // Task View
    gtk_scrolled_window_set_policy(main_window->task_scroll,
                                   GtkPolicyType::GTK_POLICY_AUTOMATIC,
                                   GtkPolicyType::GTK_POLICY_AUTOMATIC);
    main_window->task_view = main_task_view_new(main_window);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(main_window->task_scroll), GTK_WIDGET(main_window->task_view));

    gtk_widget_show_all(GTK_WIDGET(main_window->main_vbox));

    // clang-format off
    g_signal_connect(G_OBJECT(main_window->file_menu_item), "button-press-event", G_CALLBACK(on_menu_bar_event), main_window);
    g_signal_connect(G_OBJECT(main_window->view_menu_item), "button-press-event", G_CALLBACK(on_menu_bar_event), main_window);
    g_signal_connect(G_OBJECT(main_window->dev_menu_item), "button-press-event", G_CALLBACK(on_menu_bar_event), main_window);
    g_signal_connect(G_OBJECT(main_window->book_menu_item), "button-press-event", G_CALLBACK(on_menu_bar_event), main_window);
    g_signal_connect(G_OBJECT(main_window->tool_menu_item), "button-press-event", G_CALLBACK(on_menu_bar_event), main_window);
    g_signal_connect(G_OBJECT(main_window->help_menu_item), "button-press-event", G_CALLBACK(on_menu_bar_event), main_window);

    // use this OR widget_class->key_press_event = on_main_window_keypress;
    g_signal_connect(G_OBJECT(main_window), "key-press-event", G_CALLBACK(on_main_window_keypress), nullptr);
    g_signal_connect(G_OBJECT(main_window), "button-press-event", G_CALLBACK(on_window_button_press_event), main_window);
    // clang-format on

    main_window->panel_change = false;
    main_window->show_panels();

    gtk_widget_hide(GTK_WIDGET(main_window->task_scroll));
    ptk_task_view_popup_show(main_window, "");

    // show window
    gtk_window_set_default_size(GTK_WINDOW(main_window),
                                app_settings.width(),
                                app_settings.height());
    if (app_settings.maximized())
    {
        gtk_window_maximize(GTK_WINDOW(main_window));
    }
    gtk_widget_show(GTK_WIDGET(main_window));

    // restore panel sliders
    // do this after maximizing/showing window so slider positions are valid
    // in actual window size
    i32 pos = xset_get_int(xset::name::panel_sliders, xset::var::x);
    if (pos < 200)
    {
        pos = 200;
    }
    gtk_paned_set_position(main_window->hpane_top, pos);
    pos = xset_get_int(xset::name::panel_sliders, xset::var::y);
    if (pos < 200)
    {
        pos = 200;
    }
    gtk_paned_set_position(main_window->hpane_bottom, pos);
    pos = xset_get_int(xset::name::panel_sliders, xset::var::s);
    if (pos < 200)
    {
        pos = -1;
    }
    gtk_paned_set_position(main_window->vpane, pos);

    // build the main menu initially, eg for F10 - Note: file_list is nullptr
    // NOT doing this because it slows down the initial opening of the window
    // and shows a stale menu anyway.
    // rebuild_menus(main_window);
}

static void
main_window_finalize(GObject* obj)
{
    ztd::remove(all_windows, MAIN_WINDOW_REINTERPRET(obj));

    g_object_unref((MAIN_WINDOW_REINTERPRET(obj))->wgroup);

    gtk_window_close(GTK_WINDOW(MAIN_WINDOW_REINTERPRET(obj)));

    G_OBJECT_CLASS(parent_class)->finalize(obj);
}

static void
main_window_get_property(GObject* obj, u32 prop_id, GValue* value, GParamSpec* pspec)
{
    (void)obj;
    (void)prop_id;
    (void)value;
    (void)pspec;
}

static void
main_window_set_property(GObject* obj, u32 prop_id, const GValue* value, GParamSpec* pspec)
{
    (void)obj;
    (void)prop_id;
    (void)value;
    (void)pspec;
}

static void
main_window_close(MainWindow* main_window)
{
    /*
    ztd::logger::info("DISC={}", g_signal_handlers_disconnect_by_func(
                            G_OBJECT(main_window),
                            G_CALLBACK(ptk_file_task_notify_handler), nullptr));
    */
    gtk_widget_destroy(GTK_WIDGET(main_window));
}

void
main_window_store_positions(MainWindow* main_window)
{
    if (main_window == nullptr)
    {
        main_window = main_window_get_last_active();
        if (main_window == nullptr)
        {
            return;
        }
    }

    // if the window is not fullscreen (is normal or maximized) save sliders
    // and columns
    if (!main_window->fullscreen)
    {
        // store width/height + sliders
        i32 pos;
        GtkAllocation allocation;
        gtk_widget_get_allocation(GTK_WIDGET(main_window), &allocation);

        if (!main_window->maximized && allocation.width > 0)
        {
            app_settings.width(allocation.width);
            app_settings.height(allocation.height);
        }
        if (GTK_IS_PANED(main_window->hpane_top))
        {
            pos = gtk_paned_get_position(main_window->hpane_top);
            if (pos)
            {
                xset_set(xset::name::panel_sliders, xset::var::x, std::to_string(pos));
            }

            pos = gtk_paned_get_position(main_window->hpane_bottom);
            if (pos)
            {
                xset_set(xset::name::panel_sliders, xset::var::y, std::to_string(pos));
            }

            pos = gtk_paned_get_position(main_window->vpane);
            if (pos)
            {
                xset_set(xset::name::panel_sliders, xset::var::s, std::to_string(pos));
            }

            if (gtk_widget_get_visible(GTK_WIDGET(main_window->task_scroll)))
            {
                pos = gtk_paned_get_position(main_window->task_vpane);
                if (pos)
                {
                    // save absolute height
                    xset_set(xset::name::task_show_manager,
                             xset::var::x,
                             std::to_string(allocation.height - pos));
                    // ztd::logger::info("CLOS  win {}x{}    task height {}   slider {}",
                    // allocation.width, allocation.height, allocation.height - pos, pos);
                }
            }
        }

        // store fb columns
        PtkFileBrowser* a_browser;
        if (main_window->maximized)
        {
            main_window->opened_maximized = true; // force save of columns
        }
        for (const panel_t p : PANELS)
        {
            const i32 page_x = gtk_notebook_get_current_page(main_window->get_panel_notebook(p));
            if (page_x != -1)
            {
                a_browser = PTK_FILE_BROWSER_REINTERPRET(
                    gtk_notebook_get_nth_page(main_window->get_panel_notebook(p), page_x));
                if (a_browser && a_browser->is_view_mode(ptk::file_browser::view_mode::list_view))
                {
                    a_browser->save_column_widths(GTK_TREE_VIEW(a_browser->folder_view()));
                }
            }
        }
    }
}

static gboolean
main_window_delete_event(GtkWidget* widget, GdkEventAny* event)
{
    (void)event;
    // ztd::logger::info("main_window_delete_event");

    MainWindow* main_window = MAIN_WINDOW_REINTERPRET(widget);

    main_window_store_positions(main_window);

    // save settings
    app_settings.maximized(main_window->maximized);
    autosave_request_cancel();
    save_settings();

    // tasks running?
    if (main_window->is_main_tasks_running())
    {
        const auto response = ptk_show_message(GTK_WINDOW(widget),
                                               GtkMessageType::GTK_MESSAGE_QUESTION,
                                               "MainWindow Delete Event",
                                               GtkButtonsType::GTK_BUTTONS_YES_NO,
                                               "Stop all tasks running in this window?");

        if (response == GtkResponseType::GTK_RESPONSE_YES)
        {
            ptk_show_message(GTK_WINDOW(widget),
                             GtkMessageType::GTK_MESSAGE_INFO,
                             "MainWindow Delete Event",
                             GtkButtonsType::GTK_BUTTONS_CLOSE,
                             "Aborting tasks...");
            main_window_close(main_window);

            ptk_task_view_task_stop(main_window->task_view,
                                    xset_get(xset::name::task_stop_all),
                                    nullptr);
            while (main_window->is_main_tasks_running())
            {
                while (g_main_context_pending(nullptr))
                {
                    g_main_context_iteration(nullptr, true);
                }
            }
        }
        else
        {
            return true;
        }
    }
    main_window_close(main_window);
    return true;
}

static gboolean
main_window_window_state_event(GtkWidget* widget, GdkEventWindowState* event)
{
    MainWindow* main_window = MAIN_WINDOW_REINTERPRET(widget);

    const bool maximized =
        ((event->new_window_state & GdkWindowState::GDK_WINDOW_STATE_MAXIMIZED) != 0);

    main_window->maximized = maximized;
    app_settings.maximized(maximized);

    if (!main_window->maximized)
    {
        if (main_window->opened_maximized)
        {
            main_window->opened_maximized = false;
        }
        main_window->show_panels(); // restore columns
    }

    return true;
}

const std::optional<std::filesystem::path>
main_window_get_tab_cwd(PtkFileBrowser* file_browser, tab_t tab_num)
{
    if (!file_browser)
    {
        return std::nullopt;
    }
    i32 page_x;
    MainWindow* main_window = file_browser->main_window();
    GtkNotebook* notebook = main_window->get_panel_notebook(file_browser->panel());
    const i32 pages = gtk_notebook_get_n_pages(notebook);
    const i32 page_num = gtk_notebook_page_num(notebook, GTK_WIDGET(file_browser));

    switch (tab_num)
    {
        case tab_control_code_prev:
            // prev
            page_x = page_num - 1;
            break;
        case tab_control_code_next:
            // next
            page_x = page_num + 1;
            break;
        default:
            // tab_num starts counting at 1
            page_x = tab_num - 1;
            break;
    }

    if (page_x > -1 && page_x < pages)
    {
        const PtkFileBrowser* tab_file_browser =
            PTK_FILE_BROWSER_REINTERPRET(gtk_notebook_get_nth_page(notebook, page_x));
        return tab_file_browser->cwd();
    }

    return std::nullopt;
}

const std::optional<std::filesystem::path>
main_window_get_panel_cwd(PtkFileBrowser* file_browser, panel_t panel_num)
{
    if (!file_browser)
    {
        return std::nullopt;
    }
    const MainWindow* main_window = file_browser->main_window();
    panel_t panel_x = file_browser->panel();

    switch (panel_num)
    {
        case panel_control_code_prev:
            // prev
            do
            {
                if (--panel_x < 1)
                {
                    panel_x = 4;
                }
                if (panel_x == file_browser->panel())
                {
                    return std::nullopt;
                }
            } while (!gtk_widget_get_visible(GTK_WIDGET(main_window->get_panel_notebook(panel_x))));
            break;
        case panel_control_code_next:
            // next
            do
            {
                if (!is_valid_panel(++panel_x))
                {
                    panel_x = 1;
                }
                if (panel_x == file_browser->panel())
                {
                    return std::nullopt;
                }
            } while (!gtk_widget_get_visible(GTK_WIDGET(main_window->get_panel_notebook(panel_x))));
            break;
        default:
            panel_x = panel_num;
            if (!gtk_widget_get_visible(GTK_WIDGET(main_window->get_panel_notebook(panel_x))))
            {
                return std::nullopt;
            }
            break;
    }

    GtkNotebook* notebook = main_window->get_panel_notebook(panel_x);
    const i32 page_x = gtk_notebook_get_current_page(notebook);

    const PtkFileBrowser* panel_file_browser =
        PTK_FILE_BROWSER_REINTERPRET(gtk_notebook_get_nth_page(notebook, page_x));
    return panel_file_browser->cwd();
}

void
main_window_open_in_panel(PtkFileBrowser* file_browser, panel_t panel_num,
                          const std::filesystem::path& file_path)
{
    if (!file_browser)
    {
        return;
    }
    MainWindow* main_window = file_browser->main_window();
    panel_t panel_x = file_browser->panel();

    switch (panel_num)
    {
        case panel_control_code_prev:
            // prev
            do
            {
                if (!is_valid_panel(--panel_x))
                { // loop to end
                    panel_x = 4;
                }
                if (panel_x == file_browser->panel())
                {
                    return;
                }
            } while (!gtk_widget_get_visible(GTK_WIDGET(main_window->get_panel_notebook(panel_x))));
            break;
        case panel_control_code_next:
            // next
            do
            {
                if (!is_valid_panel(++panel_x))
                { // loop to start
                    panel_x = 1;
                }
                if (panel_x == file_browser->panel())
                {
                    return;
                }
            } while (!gtk_widget_get_visible(GTK_WIDGET(main_window->get_panel_notebook(panel_x))));
            break;
        default:
            panel_x = panel_num;
            break;
    }

    if (!is_valid_panel(panel_x))
    {
        return;
    }

    // show panel
    if (!gtk_widget_get_visible(GTK_WIDGET(main_window->get_panel_notebook(panel_x))))
    {
        xset_set_b_panel(panel_x, xset::panel::show, true);
        show_panels_all_windows(nullptr, main_window);
    }

    // open in tab in panel
    const i32 save_curpanel = main_window->curpanel;

    main_window->curpanel = panel_x;
    main_window->notebook = main_window->get_panel_notebook(panel_x);

    main_window->new_tab(file_path);

    main_window->curpanel = save_curpanel;
    main_window->notebook = main_window->get_panel_notebook(main_window->curpanel);

    // focus original panel
    // while(g_main_context_pending(nullptr))
    //    g_main_context_iteration(nullptr, true);
    // gtk_widget_grab_focus(GTK_WIDGET(main_window->notebook));
    // gtk_widget_grab_focus(GTK_WIDGET(file_browser->folder_view));
    g_idle_add((GSourceFunc)delayed_focus_file_browser, file_browser);
}

bool
main_window_panel_is_visible(PtkFileBrowser* file_browser, panel_t panel)
{
    if (!is_valid_panel(panel))
    {
        return false;
    }
    const MainWindow* main_window = file_browser->main_window();
    return gtk_widget_get_visible(GTK_WIDGET(main_window->get_panel_notebook(panel)));
}

const main_window_counts_data
main_window_get_counts(PtkFileBrowser* file_browser)
{
    if (!file_browser)
    {
        return {0, 0, 0};
    }

    MainWindow* main_window = file_browser->main_window();
    GtkNotebook* notebook = main_window->get_panel_notebook(file_browser->panel());
    const tab_t tab_count = gtk_notebook_get_n_pages(notebook);

    // tab_num starts counting from 1
    const tab_t tab_num = gtk_notebook_page_num(notebook, GTK_WIDGET(file_browser)) + 1;
    panel_t panel_count = 0;
    for (const panel_t p : PANELS)
    {
        if (gtk_widget_get_visible(GTK_WIDGET(main_window->get_panel_notebook(p))))
        {
            panel_count++;
        }
    }

    return {panel_count, tab_count, tab_num};
}

static bool
notebook_clicked(GtkWidget* widget, GdkEvent* event, PtkFileBrowser* file_browser)
{
    (void)widget;
    MainWindow* main_window = file_browser->main_window();
    main_window->on_file_browser_panel_change(file_browser);

    const auto button = gdk_button_event_get_button(event);
    const auto type = gdk_event_get_event_type(event);

    // middle-click on tab closes
    if (type == GdkEventType::GDK_BUTTON_PRESS)
    {
        if (button == 2)
        {
            file_browser->close_tab();
            return true;
        }
        else if (button == 3)
        {
            GtkWidget* popup = gtk_menu_new();

#if (GTK_MAJOR_VERSION == 4)
            GtkEventController* accel_group = gtk_shortcut_controller_new();
#elif (GTK_MAJOR_VERSION == 3)
            GtkAccelGroup* accel_group = gtk_accel_group_new();
#endif

            xset_t set;

            set = xset_get(xset::name::tab_close);
            xset_set_cb(set, (GFunc)ptk_file_browser_close_tab, file_browser);
            xset_add_menuitem(file_browser, popup, accel_group, set);
            set = xset_get(xset::name::tab_restore);
            xset_set_cb(set, (GFunc)ptk_file_browser_restore_tab, file_browser);
            xset_add_menuitem(file_browser, popup, accel_group, set);
            set = xset_get(xset::name::tab_new);
            xset_set_cb(set, (GFunc)ptk_file_browser_new_tab, file_browser);
            xset_add_menuitem(file_browser, popup, accel_group, set);
            set = xset_get(xset::name::tab_new_here);
            xset_set_cb(set, (GFunc)ptk_file_browser_new_tab_here, file_browser);
            xset_add_menuitem(file_browser, popup, accel_group, set);
            gtk_widget_show_all(GTK_WIDGET(popup));
            // clang-format off
            g_signal_connect(G_OBJECT(popup), "selection-done", G_CALLBACK(gtk_widget_destroy), nullptr);
            // clang-format on
            gtk_menu_popup_at_pointer(GTK_MENU(popup), nullptr);
            return true;
        }
    }
    return false;
}

void
MainWindow::on_file_browser_before_chdir(PtkFileBrowser* file_browser)
{
    this->update_status_bar(file_browser);
}

void
MainWindow::on_file_browser_begin_chdir(PtkFileBrowser* file_browser)
{
    this->update_status_bar(file_browser);
}

void
MainWindow::on_file_browser_after_chdir(PtkFileBrowser* file_browser)
{
    // main_window_stop_busy_task( main_window );

    if (this->current_file_browser() == file_browser)
    {
        this->set_window_title(file_browser);
    }

    if (file_browser->inhibit_focus_)
    {
        // complete ptk_file_browser.c PtkFileBrowser::seek_path()
        file_browser->inhibit_focus_ = false;
        if (file_browser->seek_name_)
        {
            file_browser->seek_path("", file_browser->seek_name_.value());
            file_browser->seek_name_ = std::nullopt;
        }
    }
    else
    {
        file_browser->select_last(); // restore last selections
        gtk_widget_grab_focus(GTK_WIDGET(file_browser->folder_view()));
    }
    if (xset_get_b(xset::name::main_save_tabs))
    {
        autosave_request_add();
    }
}

GtkWidget*
MainWindow::create_tab_label(PtkFileBrowser* file_browser) const noexcept
{
    /* Create tab label */
#if (GTK_MAJOR_VERSION == 3)
    GtkEventBox* ebox = GTK_EVENT_BOX(gtk_event_box_new());
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(ebox), false);
#endif

    GtkBox* box = GTK_BOX(gtk_box_new(GtkOrientation::GTK_ORIENTATION_HORIZONTAL, 0));
    GtkWidget* icon =
        gtk_image_new_from_icon_name(ICON_FULLCOLOR_FOLDER.data(), GtkIconSize::GTK_ICON_SIZE_MENU);
    gtk_box_pack_start(box, icon, false, false, 4);

    const auto& cwd = file_browser->cwd();
    GtkLabel* label = GTK_LABEL(gtk_label_new(cwd.filename().c_str()));

    if (cwd.string().size() < 30)
    {
        gtk_label_set_ellipsize(label, PangoEllipsizeMode::PANGO_ELLIPSIZE_NONE);
        gtk_label_set_width_chars(label, -1);
    }
    else
    {
        gtk_label_set_ellipsize(label, PangoEllipsizeMode::PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_width_chars(label, 30);
    }
    gtk_label_set_max_width_chars(label, 30);
    gtk_box_pack_start(box, GTK_WIDGET(label), false, false, 4);

    if (app_settings.show_close_tab_buttons())
    {
        GtkButton* close_btn = GTK_BUTTON(gtk_button_new());
        gtk_widget_set_focus_on_click(GTK_WIDGET(close_btn), false);
        gtk_button_set_relief(close_btn, GTK_RELIEF_NONE);
        GtkWidget* close_icon =
            gtk_image_new_from_icon_name("window-close", GtkIconSize::GTK_ICON_SIZE_MENU);

        gtk_button_set_child(GTK_BUTTON(close_btn), close_icon);
        gtk_box_pack_end(box, GTK_WIDGET(close_btn), false, false, 0);

        // clang-format off
        g_signal_connect(G_OBJECT(close_btn), "clicked", G_CALLBACK(ptk_file_browser_close_tab), file_browser);
        // clang-format on
    }

#if (GTK_MAJOR_VERSION == 3)
    gtk_container_add(GTK_CONTAINER(ebox), GTK_WIDGET(box));
    g_object_set_data(G_OBJECT(ebox), "box", box);
#endif

    gtk_widget_set_events(GTK_WIDGET(box), GdkEventMask::GDK_ALL_EVENTS_MASK);
    gtk_drag_dest_set(
        GTK_WIDGET(box),
        GTK_DEST_DEFAULT_ALL,
        drag_targets,
        sizeof(drag_targets) / sizeof(GtkTargetEntry),
        GdkDragAction(GdkDragAction::GDK_ACTION_DEFAULT | GdkDragAction::GDK_ACTION_COPY |
                      GdkDragAction::GDK_ACTION_MOVE | GdkDragAction::GDK_ACTION_LINK));

    g_object_set_data(G_OBJECT(box), "label", label);
    g_object_set_data(G_OBJECT(box), "icon", icon);

    // clang-format off
#if (GTK_MAJOR_VERSION == 4)
    g_signal_connect(G_OBJECT(box), "drag-motion", G_CALLBACK(on_tab_drag_motion), file_browser);
    g_signal_connect(G_OBJECT(box), "button-press-event", G_CALLBACK(notebook_clicked), file_browser);

    gtk_widget_show_all(GTK_WIDGET(box));
#elif (GTK_MAJOR_VERSION == 3)
    g_signal_connect(G_OBJECT(ebox), "drag-motion", G_CALLBACK(on_tab_drag_motion), file_browser);
    g_signal_connect(G_OBJECT(ebox), "button-press-event", G_CALLBACK(notebook_clicked), file_browser);

    gtk_widget_show_all(GTK_WIDGET(ebox));
#endif
    // clang-format on

#if (GTK_MAJOR_VERSION == 4)
    return GTK_WIDGET(box);
#elif (GTK_MAJOR_VERSION == 3)
    return GTK_WIDGET(ebox);
#endif
}

void
MainWindow::new_tab(const std::filesystem::path& folder_path) noexcept
{
    // ztd::logger::debug("New tab fb={} panel={} path={}", fmt::ptr(file_browser), this->curpanel, folder_path);

    PtkFileBrowser* current_file_browser = this->current_file_browser();
    if (GTK_IS_WIDGET(current_file_browser))
    {
        // save sliders of current fb ( new tab while task manager is shown changes vals )
        current_file_browser->slider_release(nullptr);
        // save column widths of fb so new tab has same
        current_file_browser->save_column_widths(GTK_TREE_VIEW(current_file_browser->folder_view_));
    }
    PtkFileBrowser* file_browser = PTK_FILE_BROWSER_REINTERPRET(
        ptk_file_browser_new(this->curpanel, this->notebook, this->task_view, this));
    if (!file_browser)
    {
        return;
    }

    file_browser->set_single_click(app_settings.single_click());

    file_browser->show_thumbnails(app_settings.show_thumbnail() ? app_settings.max_thumb_size()
                                                                : 0);

    const auto sort_order =
        xset_get_int_panel(file_browser->panel(), xset::panel::list_detailed, xset::var::x);
    file_browser->set_sort_order((ptk::file_browser::sort_order)sort_order);

    const auto sort_type =
        xset_get_int_panel(file_browser->panel(), xset::panel::list_detailed, xset::var::y);
    file_browser->set_sort_type((GtkSortType)sort_type);

    gtk_widget_show(GTK_WIDGET(file_browser));

    file_browser->add_event<spacefm::signal::chdir_before>(
        std::bind(&MainWindow::on_file_browser_before_chdir, this, std::placeholders::_1));
    file_browser->add_event<spacefm::signal::chdir_begin>(
        std::bind(&MainWindow::on_file_browser_begin_chdir, this, std::placeholders::_1));
    file_browser->add_event<spacefm::signal::chdir_after>(
        std::bind(&MainWindow::on_file_browser_after_chdir, this, std::placeholders::_1));
    file_browser->add_event<spacefm::signal::open_item>(
        std::bind(&MainWindow::on_file_browser_open_item,
                  this,
                  std::placeholders::_1,
                  std::placeholders::_2,
                  std::placeholders::_3));
    file_browser->add_event<spacefm::signal::change_content>(
        std::bind(&MainWindow::on_file_browser_content_change, this, std::placeholders::_1));
    file_browser->add_event<spacefm::signal::change_sel>(
        std::bind(&MainWindow::on_file_browser_sel_change, this, std::placeholders::_1));
    file_browser->add_event<spacefm::signal::change_pane>(
        std::bind(&MainWindow::on_file_browser_panel_change, this, std::placeholders::_1));

    GtkWidget* tab_label = this->create_tab_label(file_browser);
    const i32 idx =
        gtk_notebook_append_page(this->notebook, GTK_WIDGET(file_browser), GTK_WIDGET(tab_label));
    gtk_notebook_set_tab_reorderable(this->notebook, GTK_WIDGET(file_browser), true);
    gtk_notebook_set_current_page(this->notebook, idx);

    if (app_settings.always_show_tabs())
    {
        gtk_notebook_set_show_tabs(this->notebook, true);
    }
    else if (gtk_notebook_get_n_pages(this->notebook) > 1)
    {
        gtk_notebook_set_show_tabs(this->notebook, true);
    }
    else
    {
        gtk_notebook_set_show_tabs(this->notebook, false);
    }

    if (!file_browser->chdir(folder_path))
    {
        file_browser->chdir("/");
    }

    set_panel_focus(this, file_browser);

    //    while(g_main_context_pending(nullptr))  // wait for chdir to grab focus
    //        g_main_context_iteration(nullptr, true);
    // gtk_widget_grab_focus(GTK_WIDGET(file_browser->folder_view_));
    // ztd::logger::info("focus browser {} {}", idx, fmt::ptr(file_browser->folder_view_));
    // ztd::logger::info("call delayed (newtab) #{} {}", idx, fmt::ptr(file_browser->folder_view_));
    // g_idle_add((GSourceFunc)delayed_focus_file_browser, file_browser);
}

PtkFileBrowser*
MainWindow::current_file_browser() const noexcept
{
    PtkFileBrowser* file_browser = nullptr;
    if (this->notebook)
    {
        const tab_t tab = gtk_notebook_get_current_page(this->notebook);
        if (tab >= 0)
        {
            GtkWidget* widget = gtk_notebook_get_nth_page(this->notebook, tab);
            file_browser = PTK_FILE_BROWSER_REINTERPRET(widget);
        }
    }
    return file_browser;
}

PtkFileBrowser*
main_window_get_current_file_browser()
{
    MainWindow* main_window = main_window_get_last_active();
    if (main_window == nullptr)
    {
        return nullptr;
    }
    PtkFileBrowser* file_browser = nullptr;
    if (main_window->notebook)
    {
        const tab_t tab = gtk_notebook_get_current_page(main_window->notebook);
        if (tab >= 0)
        {
            GtkWidget* widget = gtk_notebook_get_nth_page(main_window->notebook, tab);
            file_browser = PTK_FILE_BROWSER_REINTERPRET(widget);
        }
    }
    return file_browser;
}

static void
on_preference_activate(GtkMenuItem* menuitem, void* user_data)
{
    (void)menuitem;
    MainWindow* main_window = MAIN_WINDOW(user_data);
    show_preference_dialog(GTK_WINDOW(main_window));
}

static void
on_about_activate(GtkMenuItem* menuitem, void* user_data)
{
    (void)menuitem;
    MainWindow* main_window = MAIN_WINDOW(user_data);
    show_about_dialog(GTK_WINDOW(main_window));
}

static void
main_window_add_new_window(MainWindow* main_window)
{
    if (main_window && !main_window->maximized && !main_window->fullscreen)
    {
        // use current main_window's size for new window
        GtkAllocation allocation;
        gtk_widget_get_allocation(GTK_WIDGET(main_window), &allocation);
        if (allocation.width > 0)
        {
            app_settings.width(allocation.width);
            app_settings.height(allocation.height);
        }
    }

    app_settings.load_saved_tabs(false);

    ztd::logger::info("Opening another window");

    GtkApplication* app = gtk_window_get_application(GTK_WINDOW(main_window));
    assert(GTK_IS_APPLICATION(app));

    MainWindow* another_main_window =
        MAIN_WINDOW(g_object_new(main_window_get_type(), "application", app, nullptr));
    gtk_window_set_application(GTK_WINDOW(main_window), app);
    assert(GTK_IS_APPLICATION_WINDOW(another_main_window));

    gtk_window_present(GTK_WINDOW(another_main_window));

    app_settings.load_saved_tabs(true);
}

static void
on_new_window_activate(GtkMenuItem* menuitem, void* user_data)
{
    (void)menuitem;
    MainWindow* main_window = MAIN_WINDOW(user_data);

    autosave_request_cancel();
    main_window_store_positions(main_window);
    save_settings();
    main_window_add_new_window(main_window);
}

static bool
delayed_focus_file_browser(PtkFileBrowser* file_browser)
{
    if (GTK_IS_WIDGET(file_browser) && GTK_IS_WIDGET(file_browser->folder_view()))
    {
        // ztd::logger::info("delayed_focus_file_browser fb={}", fmt::ptr(file_browser));
        if (GTK_IS_WIDGET(file_browser) && GTK_IS_WIDGET(file_browser->folder_view()))
        {
            gtk_widget_grab_focus(file_browser->folder_view());
            set_panel_focus(nullptr, file_browser);
        }
    }
    return false;
}

void
set_panel_focus(MainWindow* main_window, PtkFileBrowser* file_browser)
{
    if (!file_browser && !main_window)
    {
        return;
    }

    MainWindow* mw = main_window;
    if (mw == nullptr)
    {
        mw = file_browser->main_window();
    }

    update_window_title(mw);
}

bool
MainWindow::is_main_tasks_running() const noexcept
{
    return ptk_task_view_is_main_tasks_running(this->task_view);
}

void
main_window_fullscreen_activate(MainWindow* main_window)
{
    PtkFileBrowser* file_browser = main_window->current_file_browser();
    if (xset_get_b(xset::name::main_full))
    {
        if (file_browser && file_browser->is_view_mode(ptk::file_browser::view_mode::list_view))
        {
            file_browser->save_column_widths(GTK_TREE_VIEW(file_browser->folder_view()));
        }
        gtk_widget_hide(GTK_WIDGET(main_window->menu_bar));
        gtk_window_fullscreen(GTK_WINDOW(main_window));
        main_window->fullscreen = true;
    }
    else
    {
        main_window->fullscreen = false;
        gtk_window_unfullscreen(GTK_WINDOW(main_window));
        gtk_widget_show(main_window->menu_bar);

        if (!main_window->maximized)
        {
            main_window->show_panels(); // restore columns
        }
    }
}

static void
on_fullscreen_activate(GtkMenuItem* menuitem, MainWindow* main_window)
{
    (void)menuitem;
    main_window_fullscreen_activate(main_window);
}

void
MainWindow::set_window_title(PtkFileBrowser* file_browser) noexcept
{
    assert(file_browser != nullptr);

    std::filesystem::path disp_path;
    std::string disp_name;

    if (file_browser->dir_)
    {
        disp_path = file_browser->dir_->path();
        disp_name = std::filesystem::equivalent(disp_path, "/") ? "/" : disp_path.filename();
    }
    else
    {
        const auto& cwd = file_browser->cwd();
        if (!cwd.empty())
        {
            disp_path = cwd;
            disp_name = std::filesystem::equivalent(disp_path, "/") ? "/" : disp_path.filename();
        }
    }

    const auto orig_fmt = xset_get_s(xset::name::main_title);
    std::string fmt;
    if (orig_fmt)
    {
        fmt = orig_fmt.value();
    }
    else
    {
        fmt = "%d";
    }

    if (fmt.contains("%t") || fmt.contains("%T") || fmt.contains("%p") || fmt.contains("%P"))
    {
        // get panel/tab info
        const auto counts = main_window_get_counts(file_browser);
        const panel_t panel_count = counts.panel_count;
        const tab_t tab_count = counts.tab_count;
        const tab_t tab_num = counts.tab_num;

        fmt = ztd::replace(fmt, "%t", std::to_string(tab_num));
        fmt = ztd::replace(fmt, "%T", std::to_string(tab_count));
        fmt = ztd::replace(fmt, "%p", std::to_string(this->curpanel));
        fmt = ztd::replace(fmt, "%P", std::to_string(panel_count));
    }
    if (fmt.contains('*') && !this->is_main_tasks_running())
    {
        fmt = ztd::replace(fmt, "*", "");
    }
    if (fmt.contains("%n"))
    {
        fmt = ztd::replace(fmt, "%n", disp_name);
    }
    if (orig_fmt && orig_fmt.value().contains("%d"))
    {
        fmt = ztd::replace(fmt, "%d", disp_path.string());
    }

    gtk_window_set_title(GTK_WINDOW(this), fmt.data());
}

static void
update_window_title(MainWindow* main_window)
{
    PtkFileBrowser* file_browser = main_window->current_file_browser();
    if (file_browser)
    {
        main_window->set_window_title(file_browser);
    }
}

static void
on_update_window_title(GtkMenuItem* item, MainWindow* main_window)
{
    (void)item;
    update_window_title(main_window);
}

static void
on_folder_notebook_switch_pape(GtkNotebook* notebook, GtkWidget* page, u32 page_num,
                               void* user_data)
{
    (void)page;
    MainWindow* main_window = MAIN_WINDOW(user_data);
    PtkFileBrowser* file_browser;

    // save sliders of current fb ( new tab while task manager is shown changes vals )
    PtkFileBrowser* current_file_browser = main_window->current_file_browser();
    if (current_file_browser)
    {
        current_file_browser->slider_release(nullptr);
        if (current_file_browser->view_mode_ == ptk::file_browser::view_mode::list_view)
        {
            current_file_browser->save_column_widths(
                GTK_TREE_VIEW(current_file_browser->folder_view_));
        }
    }

    file_browser = PTK_FILE_BROWSER_REINTERPRET(gtk_notebook_get_nth_page(notebook, page_num));
    // ztd::logger::info("on_folder_notebook_switch_pape fb={}   panel={}   page={}", fmt::ptr(file_browser), file_browser->mypanel, page_num);
    main_window->curpanel = file_browser->panel();
    main_window->notebook = main_window->get_panel_notebook(main_window->curpanel);

    main_window->update_status_bar(file_browser);

    main_window->set_window_title(file_browser);

    file_browser->update_views();

    if (GTK_IS_WIDGET(file_browser))
    {
        g_idle_add((GSourceFunc)delayed_focus_file_browser, file_browser);
    }
}

void
MainWindow::open_path_in_current_tab(const std::filesystem::path& path) noexcept
{
    PtkFileBrowser* file_browser = this->current_file_browser();
    if (!file_browser)
    {
        return;
    }
    file_browser->chdir(path);
}

void
main_window_open_network(MainWindow* main_window, const std::string_view url, bool new_tab)
{
    PtkFileBrowser* file_browser = main_window->current_file_browser();
    if (!file_browser)
    {
        return;
    }
    ptk_location_view_mount_network(file_browser, url, new_tab, false);
}

void
MainWindow::on_file_browser_open_item(PtkFileBrowser* file_browser,
                                      const std::filesystem::path& path, ptk::open_action action)
{
    if (path.empty())
    {
        return;
    }

    switch (action)
    {
        case ptk::open_action::dir:
            file_browser->chdir(path);
            break;
        case ptk::open_action::new_tab:
            this->new_tab(path);
            break;
        case ptk::open_action::new_window:
        case ptk::open_action::terminal:
        case ptk::open_action::file:
            break;
    }
}

void
MainWindow::update_status_bar(PtkFileBrowser* file_browser) const noexcept
{
    assert(file_browser != nullptr);

    const auto& cwd = file_browser->cwd();
    if (cwd.empty())
    {
        return;
    }

    std::string statusbar_txt;

    if (std::filesystem::exists(cwd))
    {
        const auto fs_stat = ztd::statvfs(cwd);

        // calc free space
        const std::string free_size = vfs_file_size_format(fs_stat.bsize() * fs_stat.bavail());
        // calc total space
        const std::string disk_size = vfs_file_size_format(fs_stat.frsize() * fs_stat.blocks());

        statusbar_txt.append(fmt::format(" {} / {}   ", free_size, disk_size));
    }

    // Show Reading... while sill loading
    if (file_browser->is_busy())
    {
        statusbar_txt.append(fmt::format("Reading {} ...", file_browser->cwd().string()));
        gtk_statusbar_pop(file_browser->statusbar, 0);
        gtk_statusbar_push(file_browser->statusbar, 0, statusbar_txt.data());
        return;
    }

    u64 total_size;
    u64 total_on_disk_size;

    // note: total size will not include content changes since last selection change
    const u32 num_sel = file_browser->get_n_sel(&total_size, &total_on_disk_size);
    const u32 num_vis = file_browser->get_n_visible_files();

    if (num_sel > 0)
    {
        const auto selected_files = file_browser->selected_files();
        if (selected_files.empty())
        {
            return;
        }

        const std::string file_size = vfs_file_size_format(total_size);
        const std::string disk_size = vfs_file_size_format(total_on_disk_size);

        statusbar_txt.append(
            fmt::format("{:L} / {:L} ({} / {})", num_sel, num_vis, file_size, disk_size));

        if (num_sel == 1)
        // display file name or symlink info in status bar if one file selected
        {
            const auto& file = selected_files.front();
            if (!file)
            {
                return;
            }

            if (file->is_symlink())
            {
                const auto target = std::filesystem::absolute(file->path());
                if (!target.empty())
                {
                    std::filesystem::path target_path;

                    // ztd::logger::info("LINK: {}", file->path());
                    if (!target.is_absolute())
                    {
                        // relative link
                        target_path = cwd / target;
                    }
                    else
                    {
                        target_path = target;
                    }

                    if (file->is_directory())
                    {
                        if (std::filesystem::exists(target_path))
                        {
                            statusbar_txt.append(fmt::format("  Link -> {}/", target.string()));
                        }
                        else
                        {
                            statusbar_txt.append(
                                fmt::format("  !Link -> {}/ (missing)", target.string()));
                        }
                    }
                    else
                    {
                        const auto results = ztd::statx(target_path);
                        if (results)
                        {
                            const std::string lsize = vfs_file_size_format(results.size());
                            statusbar_txt.append(
                                fmt::format("  Link -> {} ({})", target.string(), lsize));
                        }
                        else
                        {
                            statusbar_txt.append(
                                fmt::format("  !Link -> {} (missing)", target.string()));
                        }
                    }
                }
                else
                {
                    statusbar_txt.append(fmt::format("  !Link -> (error reading target)"));
                }
            }
            else
            {
                statusbar_txt.append(fmt::format("  {}", file->name()));
            }
        }
        else
        {
            u32 count_dir = 0;
            u32 count_file = 0;
            u32 count_symlink = 0;
            u32 count_socket = 0;
            u32 count_pipe = 0;
            u32 count_block = 0;
            u32 count_char = 0;

            for (const auto& file : selected_files)
            {
                if (!file)
                {
                    continue;
                }

                if (file->is_directory())
                {
                    ++count_dir;
                }
                else if (file->is_regular_file())
                {
                    ++count_file;
                }
                else if (file->is_symlink())
                {
                    ++count_symlink;
                }
                else if (file->is_socket())
                {
                    ++count_socket;
                }
                else if (file->is_fifo())
                {
                    ++count_pipe;
                }
                else if (file->is_block_file())
                {
                    ++count_block;
                }
                else if (file->is_character_file())
                {
                    ++count_char;
                }
            }

            if (count_dir)
            {
                statusbar_txt.append(fmt::format("  Directories ({:L})", count_dir));
            }
            if (count_file)
            {
                statusbar_txt.append(fmt::format("  Files ({:L})", count_file));
            }
            if (count_symlink)
            {
                statusbar_txt.append(fmt::format("  Symlinks ({:L})", count_symlink));
            }
            if (count_socket)
            {
                statusbar_txt.append(fmt::format("  Sockets ({:L})", count_socket));
            }
            if (count_pipe)
            {
                statusbar_txt.append(fmt::format("  Named Pipes ({:L})", count_pipe));
            }
            if (count_block)
            {
                statusbar_txt.append(fmt::format("  Block Devices ({:L})", count_block));
            }
            if (count_char)
            {
                statusbar_txt.append(fmt::format("  Character Devices ({:L})", count_char));
            }
        }
    }
    else
    {
        // size of files in dir, does not get subdir size
        // TODO, can use file_browser->dir->file_list
        u64 disk_size_bytes = 0;
        u64 disk_size_disk = 0;
        for (const auto& file : std::filesystem::directory_iterator(cwd))
        {
            const auto file_stat = ztd::statx(file.path());
            if (!file_stat.is_regular_file())
            {
                continue;
            }
            disk_size_bytes += file_stat.size();
            disk_size_disk += file_stat.size_on_disk();
        }
        const std::string file_size = vfs_file_size_format(disk_size_bytes);
        const std::string disk_size = vfs_file_size_format(disk_size_disk);

        // count for .hidden files
        const u32 num_hid = file_browser->get_n_all_files() - num_vis;
        const u32 num_hidx = file_browser->dir_ ? file_browser->dir_->hidden_files() : 0;
        if (num_hid || num_hidx)
        {
            statusbar_txt.append(fmt::format("{:L} visible ({:L} hidden)  ({} / {})",
                                             num_vis,
                                             num_hid,
                                             file_size,
                                             disk_size));
        }
        else
        {
            statusbar_txt.append(fmt::format("{:L} {}  ({} / {})",
                                             num_vis,
                                             num_vis == 1 ? "item" : "items",
                                             file_size,
                                             disk_size));
        }

        // cur dir is a symlink? canonicalize path
        if (std::filesystem::is_symlink(cwd))
        {
            const auto canon = std::filesystem::read_symlink(cwd);
            statusbar_txt.append(fmt::format("  {} -> {}", cwd.string(), canon.string()));
        }
        else
        {
            statusbar_txt.append(fmt::format("  {}", cwd.string()));
        }
    }

    // too much padding
    gtk_widget_set_margin_top(GTK_WIDGET(file_browser->statusbar), 0);
    gtk_widget_set_margin_bottom(GTK_WIDGET(file_browser->statusbar), 0);

    gtk_statusbar_pop(file_browser->statusbar, 0);
    gtk_statusbar_push(file_browser->statusbar, 0, statusbar_txt.data());
}

GtkNotebook*
MainWindow::get_panel_notebook(const panel_t panel) const noexcept
{
    assert(is_valid_panel(panel));
    // need to convert the panel number to an array index
    return this->panels[panel - 1];
}

void
MainWindow::on_file_browser_panel_change(PtkFileBrowser* file_browser)
{
    // ztd::logger::info("panel_change  panel {}", file_browser->mypanel);
    this->curpanel = file_browser->panel();
    this->notebook = this->get_panel_notebook(this->curpanel);
    set_panel_focus(this, file_browser);
}

void
MainWindow::on_file_browser_sel_change(PtkFileBrowser* file_browser)
{
    // ztd::logger::info("sel_change  panel {}", file_browser->mypanel);
    this->update_status_bar(file_browser);
}

void
MainWindow::on_file_browser_content_change(PtkFileBrowser* file_browser)
{
    // ztd::logger::info("content_change  panel {}", file_browser->mypanel);
    this->update_status_bar(file_browser);
}

static bool
on_tab_drag_motion(GtkWidget* widget, GdkDragContext* drag_context, i32 x, i32 y, u32 time,
                   PtkFileBrowser* file_browser)
{
    (void)widget;
    (void)drag_context;
    (void)x;
    (void)y;
    (void)time;
    GtkNotebook* notebook = GTK_NOTEBOOK(gtk_widget_get_parent(GTK_WIDGET(file_browser)));
    // TODO: Add a timeout here and do not set current page immediately
    const i32 idx = gtk_notebook_page_num(notebook, GTK_WIDGET(file_browser));
    gtk_notebook_set_current_page(notebook, idx);
    return false;
}

static bool
on_window_button_press_event(GtkWidget* widget, GdkEvent* event, MainWindow* main_window)
{
    (void)widget;
    const auto type = gdk_event_get_event_type(event);
    if (type != GdkEventType::GDK_BUTTON_PRESS)
    {
        return false;
    }

    const auto button = gdk_button_event_get_button(event);

    // handle mouse back/forward buttons anywhere in the main window
    if (button == 4 || button == 5 || button == 8 || button == 9)
    {
        PtkFileBrowser* file_browser = main_window->current_file_browser();
        if (!file_browser)
        {
            return false;
        }
        if (button == 4 || button == 8)
        {
            file_browser->go_back();
        }
        else
        {
            file_browser->go_forward();
        }
        return true;
    }
    return false;
}

static bool
on_main_window_keypress(MainWindow* main_window, GdkEvent* event, void* user_data)
{
    const auto keymod = ptk_get_keymod(gdk_event_get_modifier_state(event));
    const auto keyval = gdk_key_event_get_keyval(event);
    // ztd::logger::debug("main_keypress {} {}", keyval, keymod);

    if (user_data)
    {
        const xset_t known_set = static_cast<xset::XSet*>(user_data)->shared_from_this();
        return on_main_window_keypress_found_key(main_window, known_set);
    }

    if (keyval == 0)
    {
        return false;
    }

    PtkFileBrowser* browser;

    if ((keyval == GDK_KEY_Home && (keymod == 0 || keymod == GdkModifierType::GDK_SHIFT_MASK)) ||
        (keyval == GDK_KEY_End && (keymod == 0 || keymod == GdkModifierType::GDK_SHIFT_MASK)) ||
        (keyval == GDK_KEY_Delete && keymod == 0) || (keyval == GDK_KEY_Tab && keymod == 0) ||
        (keymod == 0 && (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)) ||
        (keyval == GDK_KEY_Left && (keymod == 0 || keymod == GdkModifierType::GDK_SHIFT_MASK)) ||
        (keyval == GDK_KEY_Right && (keymod == 0 || keymod == GdkModifierType::GDK_SHIFT_MASK)) ||
        (keyval == GDK_KEY_BackSpace && keymod == 0) ||
        (keymod == 0 && keyval != GDK_KEY_Escape && gdk_keyval_to_unicode(keyval))) // visible char
    {
        browser = main_window->current_file_browser();
        if (browser && browser->path_bar() && gtk_widget_has_focus(GTK_WIDGET(browser->path_bar())))
        {
            return false; // send to pathbar
        }
    }

    for (const xset_t& set : xsets)
    {
        assert(set != nullptr);

        if (set->shared_key)
        {
            // set has shared key
            xset_t shared_key_set = set->shared_key;
            if (shared_key_set->key == keyval && shared_key_set->keymod == keymod)
            {
                // shared key match
                if (shared_key_set->name.starts_with("panel"))
                {
                    // use current panel's set
                    browser = main_window->current_file_browser();
                    if (browser)
                    {
                        const std::string new_set_name =
                            fmt::format("panel{}_{}",
                                        browser->panel(),
                                        shared_key_set->name.data() + 6);
                        shared_key_set = xset_get(new_set_name);
                    }
                    else
                    { // failsafe
                        return false;
                    }
                }
                return on_main_window_keypress_found_key(main_window, shared_key_set);
            }
            else
            {
                continue;
            }
        }
        if (set->key == keyval && set->keymod == keymod)
        {
            return on_main_window_keypress_found_key(main_window, set);
        }
    }

#if (GTK_MAJOR_VERSION == 4)
    if ((keymod & GdkModifierType::GDK_ALT_MASK))
#elif (GTK_MAJOR_VERSION == 3)
    if ((keymod & GdkModifierType::GDK_MOD1_MASK))
#endif
    {
        rebuild_menus(main_window);
    }

    return false;
}

bool
main_window_keypress(MainWindow* main_window, GdkEvent* event, void* user_data)
{
    return on_main_window_keypress(main_window, event, user_data);
}

static bool
on_main_window_keypress_found_key(MainWindow* main_window, const xset_t& set)
{
    PtkFileBrowser* browser = main_window->current_file_browser();
    if (!browser)
    {
        return true;
    }

    // special edit items
    if (set->xset_name == xset::name::edit_cut || set->xset_name == xset::name::edit_copy ||
        set->xset_name == xset::name::edit_delete || set->xset_name == xset::name::select_all)
    {
        if (!gtk_widget_is_focus(browser->folder_view()))
        {
            return false;
        }
    }
    else if (set->xset_name == xset::name::edit_paste)
    {
        const bool side_dir_focus =
            (browser->side_dir && gtk_widget_is_focus(GTK_WIDGET(browser->side_dir)));
        if (!gtk_widget_is_focus(GTK_WIDGET(browser->folder_view())) && !side_dir_focus)
        {
            return false;
        }
    }

    // run menu_cb
    if (set->menu_style < xset::menu::submenu)
    {
        set->browser = browser;
        xset_menu_cb(nullptr, set); // also does custom activate
    }
    if (!set->lock)
    {
        return true;
    }

    // handlers
    if (set->name.starts_with("dev_"))
    {
        ptk_location_view_on_action(GTK_WIDGET(browser->side_dev), set);
    }
    else if (set->name.starts_with("main_"))
    {
        if (set->xset_name == xset::name::main_new_window)
        {
            on_new_window_activate(nullptr, main_window);
        }
        else if (set->xset_name == xset::name::main_search)
        {
            on_find_file_activate(nullptr, main_window);
        }
        else if (set->xset_name == xset::name::main_terminal)
        {
            on_open_terminal_activate(nullptr, main_window);
        }
        else if (set->xset_name == xset::name::main_save_session)
        {
            on_open_url(nullptr, main_window);
        }
        else if (set->xset_name == xset::name::main_exit)
        {
            on_quit_activate(nullptr, main_window);
        }
        else if (set->xset_name == xset::name::main_full)
        {
            xset_set_b(xset::name::main_full, !main_window->fullscreen);
            on_fullscreen_activate(nullptr, main_window);
        }
        else if (set->xset_name == xset::name::main_prefs)
        {
            on_preference_activate(nullptr, main_window);
        }
        else if (set->xset_name == xset::name::main_title)
        {
            update_window_title(main_window);
        }
        else if (set->xset_name == xset::name::main_about)
        {
            on_about_activate(nullptr, main_window);
        }
    }
    else if (set->name.starts_with("panel_"))
    {
        i32 i;
        if (set->xset_name == xset::name::panel_prev)
        {
            i = panel_control_code_prev;
        }
        else if (set->xset_name == xset::name::panel_next)
        {
            i = panel_control_code_next;
        }
        else if (set->xset_name == xset::name::panel_hide)
        {
            i = panel_control_code_hide;
        }
        else
        {
            i = std::stoi(set->name);
        }
        main_window->focus_panel(i);
    }
    else if (set->name.starts_with("task_"))
    {
        if (set->xset_name == xset::name::task_manager)
        {
            ptk_task_view_popup_show(main_window, set->name);
        }
        else if (set->xset_name == xset::name::task_col_reorder)
        {
            on_reorder(nullptr, GTK_WIDGET(browser->task_view()));
        }
        else if (set->xset_name == xset::name::task_col_status ||
                 set->xset_name == xset::name::task_col_count ||
                 set->xset_name == xset::name::task_col_path ||
                 set->xset_name == xset::name::task_col_file ||
                 set->xset_name == xset::name::task_col_to ||
                 set->xset_name == xset::name::task_col_progress ||
                 set->xset_name == xset::name::task_col_total ||
                 set->xset_name == xset::name::task_col_started ||
                 set->xset_name == xset::name::task_col_elapsed ||
                 set->xset_name == xset::name::task_col_curspeed ||
                 set->xset_name == xset::name::task_col_curest ||
                 set->xset_name == xset::name::task_col_avgspeed ||
                 set->xset_name == xset::name::task_col_avgest ||
                 set->xset_name == xset::name::task_col_reorder)
        {
            ptk_task_view_column_selected(browser->task_view());
        }
        else if (set->xset_name == xset::name::task_stop ||
                 set->xset_name == xset::name::task_stop_all ||
                 set->xset_name == xset::name::task_pause ||
                 set->xset_name == xset::name::task_pause_all ||
                 set->xset_name == xset::name::task_que ||
                 set->xset_name == xset::name::task_que_all ||
                 set->xset_name == xset::name::task_resume ||
                 set->xset_name == xset::name::task_resume_all)
        {
            PtkFileTask* ptask = ptk_task_view_get_selected_task(browser->task_view());
            ptk_task_view_task_stop(browser->task_view(), set, ptask);
        }
        else if (set->xset_name == xset::name::task_showout)
        {
            ptk_task_view_show_task_dialog(browser->task_view());
        }
        else if (set->name.starts_with("task_err_"))
        {
            ptk_task_view_popup_errset(main_window, set->name);
        }
    }
    else if (set->xset_name == xset::name::rubberband)
    {
        main_window_rubberband_all();
    }
    else
    {
        browser->on_action(set->xset_name);
    }

    return true;
}

MainWindow*
main_window_get_last_active()
{
    if (!all_windows.empty())
    {
        return all_windows.at(0);
    }
    return nullptr;
}

const std::vector<MainWindow*>&
main_window_get_all()
{
    return all_windows;
}

static long
get_desktop_index(GtkWindow* win)
{
#if 1
    (void)win;

    return -1;
#else // Broken with wayland
    i64 desktop = -1;
    GdkDisplay* display;
    GdkWindow* window = nullptr;

    if (win)
    {
        // get desktop of win
        display = gtk_widget_get_display(GTK_WIDGET(win));
        window = gtk_widget_get_window(GTK_WIDGET(win));
    }
    else
    {
        // get current desktop
        display = gdk_display_get_default();
        if (display)
            window = gdk_x11_window_lookup_for_display(display, gdk_x11_get_default_root_xwindow());
    }

    if (!(GDK_IS_DISPLAY(display) && GDK_IS_WINDOW(window)))
        return desktop;

    // find out what desktop (workspace) window is on   #include <gdk/gdkx.h>
    Atom type;
    i32 format;
    u64 nitems;
    u64 bytes_after;
    unsigned char* data;
    const char* atom_name = win ? "_NET_WM_DESKTOP" : "_NET_CURRENT_DESKTOP";
    Atom net_wm_desktop = gdk_x11_get_xatom_by_name_for_display(display, atom_name);

    if (net_wm_desktop == None)
        ztd::logger::error("atom not found: {}", atom_name);
    else if (XGetWindowProperty(GDK_DISPLAY_XDISPLAY(display),
                                GDK_WINDOW_XID(window),
                                net_wm_desktop,
                                0,
                                1,
                                False,
                                XA_CARDINAL,
                                (Atom*)&type,
                                &format,
                                &nitems,
                                &bytes_after,
                                &data) != Success ||
             type == None || data == nullptr)
    {
        if (type == None)
            ztd::logger::error("No such property from XGetWindowProperty() {}", atom_name);
        else if (data == nullptr)
            ztd::logger::error("No data returned from XGetWindowProperty() {}", atom_name);
        else
            ztd::logger::error("XGetWindowProperty() {} failed\n", atom_name);
    }
    else
    {
        desktop = *data;
        XFree(data);
    }
    return desktop;
#endif
}

MainWindow*
main_window_get_on_current_desktop()
{ // find the last used spacefm window on the current desktop
    const i64 cur_desktop = get_desktop_index(nullptr);
    // ztd::logger::info("current_desktop = {}", cur_desktop);
    if (cur_desktop == -1)
    {
        return main_window_get_last_active(); // revert to dumb if no current
    }

    bool invalid = false;
    for (MainWindow* window : all_windows)
    {
        const i64 desktop = get_desktop_index(GTK_WINDOW(window));
        // ztd::logger::info( "    test win {} = {}", fmt::ptr(window), desktop);
        if (desktop == cur_desktop || desktop > 254 /* 255 == all desktops */)
        {
            return window;
        }
        else if (desktop == -1 && !invalid)
        {
            invalid = true;
        }
    }
    // revert to dumb if one or more window desktops unreadable
    return invalid ? main_window_get_last_active() : nullptr;
}

const std::string
main_write_exports(const std::shared_ptr<vfs::file_task>& vtask, const std::string_view value)
{
    PtkFileBrowser* file_browser = PTK_FILE_BROWSER(vtask->exec_browser);
    const MainWindow* main_window = file_browser->main_window();

    const xset_t set = vtask->exec_set;

    std::string buf;

    // buf.append("\n#source");
    // buf.append("\n\ncp $0 /tmp\n\n");

    // panels
    for (panel_t p : PANELS)
    {
        PtkFileBrowser* a_browser;

        if (!xset_get_b_panel(p, xset::panel::show))
        {
            continue;
        }
        const i32 current_page = gtk_notebook_get_current_page(main_window->get_panel_notebook(p));
        if (current_page != -1)
        {
            a_browser = PTK_FILE_BROWSER_REINTERPRET(
                gtk_notebook_get_nth_page(main_window->get_panel_notebook(p), current_page));
        }
        else
        {
            continue;
        }

        if (!a_browser || !gtk_widget_get_visible(GTK_WIDGET(a_browser)))
        {
            continue;
        }

        // cwd
        const auto& cwd = a_browser->cwd();
        buf.append(fmt::format("set fm_pwd_panel[{}] {}\n", p, ztd::shell::quote(cwd.string())));
        buf.append(fmt::format("set fm_tab_panel[{}] {}\n", p, current_page + 1));

        // selected files
        const auto selected_files = a_browser->selected_files();
        if (!selected_files.empty())
        {
            // create fish array
            buf.append(fmt::format("set fm_panel{}_files (echo ", p));
            for (const auto& file : selected_files)
            {
                buf.append(fmt::format("{} ", ztd::shell::quote(file->path().string())));
            }
            buf.append(fmt::format(")\n"));

            if (file_browser == a_browser)
            {
                // create fish array
                buf.append(fmt::format("set fm_filenames (echo "));
                for (const auto& file : selected_files)
                {
                    buf.append(fmt::format("{} ", ztd::shell::quote(file->name())));
                }
                buf.append(fmt::format(")\n"));
            }
        }

        // device
        if (a_browser->side_dev)
        {
            const auto vol = ptk_location_view_get_selected_vol(GTK_TREE_VIEW(a_browser->side_dev));
            if (vol)
            {
                if (file_browser == a_browser)
                {
                    // clang-format off
                    buf.append(fmt::format("set fm_device {}\n", ztd::shell::quote(vol->device_file())));
                    buf.append(fmt::format("set fm_device_udi {}\n", ztd::shell::quote(vol->udi())));
                    buf.append(fmt::format("set fm_device_mount_point {}\n", ztd::shell::quote(vol->mount_point())));
                    buf.append(fmt::format("set fm_device_label {}\n", ztd::shell::quote(vol->label())));
                    buf.append(fmt::format("set fm_device_fstype {}\n", ztd::shell::quote(vol->fstype())));
                    buf.append(fmt::format("set fm_device_size {}\n", vol->size()));
                    buf.append(fmt::format("set fm_device_display_name {}\n", ztd::shell::quote(vol->display_name())));
                    buf.append(fmt::format("set fm_device_icon {}\n", ztd::shell::quote(vol->icon())));
                    buf.append(fmt::format("set fm_device_is_mounted {}\n", vol->is_mounted() ? 1 : 0));
                    buf.append(fmt::format("set fm_device_is_optical {}\n", vol->is_optical() ? 1 : 0));
                    buf.append(fmt::format("set fm_device_is_removable {}\n", vol->is_removable() ? 1 : 0));
                    buf.append(fmt::format("set fm_device_is_mountable {}\n", vol->is_mountable() ? 1 : 0));
                    // clang-format on
                }
                // clang-format off
                buf.append(fmt::format("set fm_panel{}_device {}\n", p, ztd::shell::quote(vol->device_file())));
                buf.append(fmt::format("set fm_panel{}_device_udi {}\n", p, ztd::shell::quote(vol->udi())));
                buf.append(fmt::format("set fm_panel{}_device_mount_point {}\n", p, ztd::shell::quote(vol->mount_point())));
                buf.append(fmt::format("set fm_panel{}_device_label {}\n", p, ztd::shell::quote(vol->label())));
                buf.append(fmt::format("set fm_panel{}_device_fstype {}\n", p, ztd::shell::quote(vol->fstype())));
                buf.append(fmt::format("set fm_panel{}_device_size {}\n", p, vol->size()));
                buf.append(fmt::format("set fm_panel{}_device_display_name {}\n", p, ztd::shell::quote(vol->display_name())));
                buf.append(fmt::format("set fm_panel{}_device_icon {}\n", p, ztd::shell::quote(vol->icon())));
                buf.append(fmt::format("set fm_panel{}_device_is_mounted {}\n", p, vol->is_mounted() ? 1 : 0));
                buf.append(fmt::format("set fm_panel{}_device_is_optical {}\n", p, vol->is_optical() ? 1 : 0));
                buf.append(fmt::format("set fm_panel{}_device_is_removable{}\n", p, vol->is_removable() ? 1 : 0));
                buf.append(fmt::format("set fm_panel{}_device_is_mountable{}\n", p, vol->is_mountable() ? 1 : 0));
                // clang-format on
            }
        }

        // tabs
        const i32 num_pages = gtk_notebook_get_n_pages(main_window->get_panel_notebook(p));
        for (const auto i : std::views::iota(0z, num_pages))
        {
            PtkFileBrowser* t_browser = PTK_FILE_BROWSER_REINTERPRET(
                gtk_notebook_get_nth_page(main_window->get_panel_notebook(p), i));
            const std::string path = ztd::shell::quote(t_browser->cwd().string());
            buf.append(fmt::format("set fm_pwd_panel{}_tab[{}] {}\n", p, i + 1, path));
            if (p == file_browser->panel())
            {
                buf.append(fmt::format("set fm_pwd_tab[{}] {}\n", i + 1, path));
            }
            if (file_browser == t_browser)
            {
                // my browser
                buf.append(fmt::format("set fm_pwd {}\n", path));
                buf.append(fmt::format("set fm_panel {}\n", p));
                buf.append(fmt::format("set fm_tab {}\n", i + 1));
            }
        }
    }

    // my selected files
    buf.append("\n");
    buf.append(fmt::format("set fm_files (echo $fm_panel{}_files)\n", file_browser->panel()));
    buf.append(fmt::format("set fm_file $fm_panel{}_files[1]\n", file_browser->panel()));
    buf.append(fmt::format("set fm_filename $fm_filenames[1]\n"));
    buf.append("\n");

    // user
    buf.append(fmt::format("set fm_user {}\n", ztd::shell::quote(Glib::get_user_name())));

    // variable value
    buf.append(fmt::format("set fm_value {}\n", ztd::shell::quote(value)));
    if (vtask->exec_ptask)
    {
        buf.append(fmt::format("set fm_my_task {}\n", fmt::ptr(vtask->exec_ptask)));
        buf.append(fmt::format("set fm_my_task_id {}\n", fmt::ptr(vtask->exec_ptask)));
    }
    buf.append(fmt::format("set fm_my_window {}\n", fmt::ptr(main_window)));
    buf.append(fmt::format("set fm_my_window_id {}\n", fmt::ptr(main_window)));

    // utils
    buf.append(fmt::format("set fm_editor {}\n",
                           ztd::shell::quote(xset_get_s(xset::name::editor).value_or(""))));
    buf.append(fmt::format("set fm_editor_terminal {}\n", xset_get_b(xset::name::editor) ? 1 : 0));

    // set
    if (set)
    {
        // cmd_dir
        const auto path = vfs::user_dirs->program_config_dir() / "scripts" / set->name;
        buf.append(fmt::format("set fm_cmd_dir {}\n", ztd::shell::quote(path.string())));

        // cmd_name
        if (set->menu_label)
        {
            buf.append(
                fmt::format("set fm_cmd_name {}\n", ztd::shell::quote(set->menu_label.value())));
        }
    }

    // tmp
    buf.append(fmt::format("set fm_tmp_dir {}\n",
                           ztd::shell::quote(vfs::user_dirs->program_tmp_dir().string())));

    // tasks
    PtkFileTask* ptask = ptk_task_view_get_selected_task(file_browser->task_view());
    if (ptask)
    {
        const std::map<vfs::file_task::type, const std::string_view> job_titles{
            {vfs::file_task::type::move, "move"},
            {vfs::file_task::type::copy, "copy"},
            {vfs::file_task::type::trash, "trash"},
            {vfs::file_task::type::del, "delete"},
            {vfs::file_task::type::link, "link"},
            {vfs::file_task::type::chmod_chown, "change"},
            {vfs::file_task::type::exec, "run"},
        };

        buf.append("\n");
        buf.append(fmt::format("set fm_task_type {}\n", job_titles.at(ptask->task->type_)));

        const auto dest_dir = ptask->task->dest_dir.value_or("");
        const auto current_file = ptask->task->current_file.value_or("");
        const auto current_dest = ptask->task->current_dest.value_or("");

        if (ptask->task->type_ == vfs::file_task::type::exec)
        {
            // clang-format off
            buf.append(fmt::format("set fm_task_pwd {}\n", ztd::shell::quote(dest_dir.string())));
            buf.append(fmt::format("set fm_task_name {}\n", ztd::shell::quote(current_file.string())));
            buf.append(fmt::format("set fm_task_command {}\n", ztd::shell::quote(ptask->task->exec_command)));
            buf.append(fmt::format("set fm_task_icon {}\n", ztd::shell::quote(ptask->task->exec_icon)));
            buf.append(fmt::format("set fm_task_pid {}\n", ptask->task->exec_pid));
            // clang-format on
        }
        else
        {
            // clang-format off
            buf.append(fmt::format("set fm_task_dest_dir {}\n", ztd::shell::quote(dest_dir.string())));
            buf.append(fmt::format("set fm_task_current_src_file {}\n", ztd::shell::quote(current_file.string())));
            buf.append(fmt::format("set fm_task_current_dest_file {}\n", ztd::shell::quote(current_dest.string())));
            // clang-format on
        }
        buf.append(fmt::format("set fm_task_id {}\n", fmt::ptr(ptask)));
        // if (ptask->task_view && (main_window = get_task_view_window(ptask->task_view)))
        // {
        //     buf.append(fmt::format("set fm_task_window {}\n", fmt::ptr(main_window)));
        //     buf.append(fmt::format("set fm_task_window_id {}\n", fmt::ptr(main_window)));
        // }
    }

    buf.append("\n\n");

    return buf;
}
