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

#include <fmt/core.h>

#include <filesystem>

#include <memory>

#include <chrono>

#include <cassert>

#include <CLI/CLI.hpp>

#include <fmt/format.h>

#include <gtkmm.h>
#include <gdkmm.h>
#include <glibmm.h>

#include <ztd/ztd.hxx>
#include <ztd/ztd_logger.hxx>

#include "types.hxx"

#include "main-window.hxx"

#include "vfs/vfs-app-desktop.hxx"
#include "vfs/vfs-file.hxx"
#include "vfs/vfs-user-dirs.hxx"
#include "vfs/vfs-trash-can.hxx"
#include "vfs/vfs-thumbnailer.hxx"

#include "ptk/ptk-app-chooser.hxx"
#include "ptk/ptk-dialog.hxx"
#include "ptk/ptk-location-view.hxx"

#include "settings/app.hxx"

#include "single-instance.hxx"
#include "program-timer.hxx"
#include "autosave.hxx"

#include "ipc.hxx"
#include "settings.hxx"

#include "bookmarks.hxx"

#include "commandline/commandline.hxx"

static void
open_file(const std::filesystem::path& path)
{
    const auto file = vfs::file::create(path);
    const auto mime_type = file->mime_type();

    const auto check_app_name = mime_type->default_action();
    if (!check_app_name)
    {
        const auto app_name =
            ptk_choose_app_for_mime_type(nullptr, mime_type, true, true, true, false);
        if (!app_name)
        {
            ztd::logger::error("no application to open file: {}", path.string());
            return;
        }
    }
    const auto& app_name = check_app_name.value();

    const auto desktop = vfs::desktop::create(app_name);

    const bool opened = desktop->open_file(vfs::user_dirs->current_dir(), path);
    if (!opened)
    {
        ptk_show_error(
            nullptr,
            "Error",
            fmt::format("Unable to use '{}' to open file:\n{}", app_name, path.string()));
    }
}

static void
open_in_tab(MainWindow* main_window, const std::filesystem::path& real_path,
            const commandline_opt_data_t& opt)
{
    // existing window
    bool tab_added = false;
    if (is_valid_panel(opt->panel))
    {
        // change to user-specified panel
        if (!gtk_notebook_get_n_pages(main_window->get_panel_notebook(opt->panel)))
        {
            // set panel to load real_path on panel load
            const xset_t set = xset_get_panel(opt->panel, xset::panel::show);
            set->ob1 = ztd::strdup(real_path);
            tab_added = true;
            set->b = xset::b::xtrue;
            show_panels_all_windows(nullptr, main_window);
        }
        else if (!gtk_widget_get_visible(GTK_WIDGET(main_window->get_panel_notebook(opt->panel))))
        {
            // show panel
            const xset_t set = xset_get_panel(opt->panel, xset::panel::show);
            set->b = xset::b::xtrue;
            show_panels_all_windows(nullptr, main_window);
        }
        main_window->curpanel = opt->panel;
        main_window->notebook = main_window->get_panel_notebook(opt->panel);
    }
    if (!tab_added)
    {
        if (opt->reuse_tab)
        {
            main_window->open_path_in_current_tab(real_path);
            opt->reuse_tab = false;
        }
        else
        {
            main_window->new_tab(real_path);
        }
    }
}

static void
tmp_clean()
{
    const auto& tmp = vfs::user_dirs->program_tmp_dir();
    if (std::filesystem::exists(tmp))
    {
        std::filesystem::remove_all(tmp);
        ztd::logger::info("Removed {}", tmp.string());
    }
}

static void
activate(GtkApplication* app, void* user_data)
{
    assert(GTK_IS_APPLICATION(app));

    const auto opt = static_cast<commandline_opt_data*>(user_data)->shared_from_this();

    app_settings.load_saved_tabs(!opt->no_tabs);

    MainWindow* main_window =
        MAIN_WINDOW(g_object_new(main_window_get_type(), "application", app, nullptr));
    gtk_window_set_application(GTK_WINDOW(main_window), app);
    assert(GTK_IS_APPLICATION_WINDOW(main_window));

    // ztd::logger::debug("main_window = {}  {} {}",
    //                    fmt::ptr(main_window),
    //                    opt->new_tab ? "new_tab" : "",
    //                    opt->reuse_tab ? "reuse_tab" : "");

    // open files passed in command line arguments
    for (const auto& file : opt->files)
    {
        const auto real_path = std::filesystem::absolute(file);

        if (std::filesystem::is_directory(real_path))
        {
            open_in_tab(main_window, real_path, opt);
        }
        else if (std::filesystem::exists(real_path))
        {
            const auto file_stat = ztd::statx(real_path);
            if (file_stat && file_stat.is_block_file())
            {
                // open block device eg /dev/sda1
                ptk_location_view_open_block(real_path, true);
            }
            else
            {
                open_file(real_path);
            }
        }
        else if ((!file.string().starts_with('/') && file.string().contains(":/")) ||
                 file.string().starts_with("//"))
        {
            main_window_open_network(main_window, file.string(), true);
        }
        else
        {
            ztd::logger::warn("File does not exist: {}", real_path.string());
        }
    }

    if (is_valid_panel(opt->panel))
    {
        // user specified a panel with no file, let's show the panel
        if (!gtk_widget_get_visible(GTK_WIDGET(main_window->get_panel_notebook(opt->panel))))
        {
            // show panel
            const xset_t set = xset_get_panel(opt->panel, xset::panel::show);
            set->b = xset::b::xtrue;
            show_panels_all_windows(nullptr, main_window);
        }
        main_window->focus_panel(opt->panel);
    }

    app_settings.load_saved_tabs(true);

    gtk_window_present(GTK_WINDOW(main_window));
}

int
main(int argc, char* argv[])
{
    // set locale to system default
    std::locale::global(std::locale(""));

    // logging init
    ztd::Logger->initialize();

    // CLI11
    CLI::App cli_app{PACKAGE_NAME_FANCY, "A multi-panel tabbed file manager"};

    auto opt = std::make_shared<commandline_opt_data>();
    setup_commandline(cli_app, opt);

    CLI11_PARSE(cli_app, argc, argv);

    // start program timer
    program_timer::start();

    // Gtk
    g_set_prgname(PACKAGE_NAME);

    // FIXME - This directs all writes to stderr into /dev/null, should try
    // and only have writes from ffmpeg get redirected.
    //
    // This is only done because ffmpeg, through libffmpegthumbnailer,
    // will output its warnings/errors when files are having their thumbnails generated. Which
    // floods stderr with messages that the user can do nothing about, such as
    // 'deprecated pixel format used, make sure you did set range correctly'
    //
    // An alternative solution to this would be to use Glib::spawn_command_line_sync
    // and redirecting that output to /dev/null, but that would involve using the
    // libffmpegthumbnailer CLI program and not the C++ interface. Not a solution that I want to do.
    //
    // In closing stderr is not used by this program for output, and this should only affect ffmpeg.
    (void)freopen("/dev/null", "w", stderr);

    // ensure that there is only one instance of spacefm.
    // if there is an existing instance, only the FILES
    // command line argument will be passed to the existing instance,
    // and then the new instance will exit.
    const bool is_single_instance = single_instance_check();
    if (!is_single_instance)
    {
        // if another instance is running then open a tab in the
        // existing instance for each passed directory
        for (const auto& file : opt->files)
        {
            if (!std::filesystem::is_directory(file))
            {
                ztd::logger::error("Not a directory: '{}'", file.string());
                continue;
            }
            const auto command = fmt::format("{} socket set new-tab {}",
                                             ztd::program::exe().string(),
                                             ztd::shell::quote(file.string()));
            Glib::spawn_command_line_sync(command);
        }

        std::exit(EXIT_SUCCESS);
    }
    // If we reach this point, we are the first instance.
    // Subsequent processes will exit and will not reach here.

    // Start a thread to receive socket messages
    const std::jthread socket_server(socket_server_thread);

    // Seed RNG
    // using the current time is a good enough seed
    const auto seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::srand(seed);

    // Initialize vfs system
    vfs_mime_type_init();
    vfs_volume_init();
    vfs_thumbnail_init();
    vfs_trash_init();

    // load config file
    load_settings();

    // load user bookmarks
    load_bookmarks();

    // start autosave thread
    autosave_init(save_settings);

    std::atexit(ztd::Logger->shutdown);
    std::atexit(free_settings);
    std::atexit(tmp_clean);
    std::atexit(autosave_terminate);
    std::atexit(vfs_mime_type_finalize);
    std::atexit(vfs_volume_finalize);
    std::atexit(single_instance_finalize);
    std::atexit(save_bookmarks);

    GtkApplication* app = gtk_application_new(PACKAGE_APPLICATION_NAME, G_APPLICATION_FLAGS_NONE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), opt.get());
    // Do not pass argc/argv, the CLI is not handled by GTK
    const auto status = g_application_run(G_APPLICATION(app), 0, nullptr);

    g_object_unref(app);

    std::exit(status);
}
