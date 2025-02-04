/**
 * Copyright (C) 2006 Hong Jen Yee (PCMan) <pcman.tw@gmail.com>
 *
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

#include <fmt/core.h>

#include <filesystem>

#include <span>

#include <vector>

#include <map>

#include <optional>

#include <memory>

#include <ranges>

#include <gtkmm.h>
#include <glibmm.h>

#include <ztd/ztd.hxx>
#include <ztd/ztd_logger.hxx>

// sfm breaks vfs independence for exec_in_terminal
#include "ptk/ptk-file-task.hxx"

#include "vfs/vfs-utils.hxx"
#include "vfs/vfs-app-desktop.hxx"

static const std::string DESKTOP_ENTRY_GROUP = "Desktop Entry";

static const std::string DESKTOP_ENTRY_KEY_TYPE = "Type";
static const std::string DESKTOP_ENTRY_KEY_NAME = "Name";
static const std::string DESKTOP_ENTRY_KEY_GENERICNAME = "GenericName";
static const std::string DESKTOP_ENTRY_KEY_NODISPLAY = "NoDisplay";
static const std::string DESKTOP_ENTRY_KEY_COMMENT = "Comment";
static const std::string DESKTOP_ENTRY_KEY_ICON = "Icon";
static const std::string DESKTOP_ENTRY_KEY_TRYEXEC = "TryExec";
static const std::string DESKTOP_ENTRY_KEY_EXEC = "Exec";
static const std::string DESKTOP_ENTRY_KEY_PATH = "Path";
static const std::string DESKTOP_ENTRY_KEY_TERMINAL = "Terminal";
static const std::string DESKTOP_ENTRY_KEY_ACTIONS = "Actions";
static const std::string DESKTOP_ENTRY_KEY_MIMETYPE = "MimeType";
static const std::string DESKTOP_ENTRY_KEY_CATEGORIES = "Categories";
static const std::string DESKTOP_ENTRY_KEY_KEYWORDS = "Keywords";
static const std::string DESKTOP_ENTRY_KEY_STARTUPNOTIFY = "StartupNotify";

std::map<std::filesystem::path, std::shared_ptr<vfs::desktop>> desktops_cache;

const std::shared_ptr<vfs::desktop>
vfs::desktop::create(const std::filesystem::path& desktop_file) noexcept
{
    if (desktops_cache.contains(desktop_file))
    {
        auto desktop = desktops_cache.at(desktop_file);
        // ztd::logger::info("vfs::desktop::desktop({})  cache   {}", fmt::ptr(desktop), desktop_file.string());
        return desktop;
    }

    auto desktop = std::make_shared<vfs::desktop>(desktop_file);
    desktops_cache.insert({desktop_file, desktop});
    // ztd::logger::info("vfs::desktop::desktop({})  new     {}", fmt::ptr(desktop), desktop_file.string());
    return desktop;
}

vfs::desktop::desktop(const std::filesystem::path& desktop_file) noexcept
{
    // ztd::logger::info("vfs::desktop::desktop({})", fmt::ptr(this));
#if (GTK_MAJOR_VERSION == 4)

    const auto kf = Glib::KeyFile::create();

    if (desktop_file.is_absolute())
    {
        this->filename_ = desktop_file.filename();
        this->path_ = desktop_file;
        this->loaded_ = kf->load_from_file(desktop_file, Glib::KeyFile::Flags::NONE);
    }
    else
    {
        this->filename_ = desktop_file.filename();
        const auto relative_path = std::filesystem::path() / "applications" / this->filename_;
        std::string relative_full_path;
        try
        {
            this->loaded_ = kf->load_from_data_dirs(relative_path, relative_full_path);
        }
        catch (...) // Glib::KeyFileError, Glib::FileError
        {
            ztd::logger::error("Error opening desktop file: {}", desktop_file.string());
            return;
        }
        this->path_ = relative_full_path;
    }

    if (!this->loaded_)
    {
        ztd::logger::error("Failed to load desktop file: {}", desktop_file.string());
        return;
    }

    // Keys not loaded from .desktop files
    // - Hidden
    // - OnlyShowIn
    // - NotShowIn
    // - DBusActivatable
    // - StartupWMClass
    // - URL
    // - PrefersNonDefaultGPU
    // - SingleMainWindow

    // clang-format off
    if (kf->has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_TYPE))
    {
        this->desktop_entry_.type = kf->get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_TYPE);
    }
    if (kf->has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_NAME))
    {
        this->desktop_entry_.name = kf->get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_NAME);
    }
    if (kf->has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_GENERICNAME))
    {
        this->desktop_entry_.generic_name = kf->get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_GENERICNAME);
    }
    if (kf->has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_NODISPLAY))
    {
        this->desktop_entry_.no_display = kf->get_boolean(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_NODISPLAY);
    }
    if (kf->has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_COMMENT))
    {
        this->desktop_entry_.comment = kf->get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_COMMENT);
    }
    if (kf->has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_ICON))
    {
        this->desktop_entry_.icon = kf->get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_ICON);
    }
    if (kf->has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_TRYEXEC))
    {
        this->desktop_entry_.try_exec = kf->get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_TRYEXEC);
    }
    if (kf->has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_EXEC))
    {
        this->desktop_entry_.exec = kf->get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_EXEC);
    }
    if (kf->has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_PATH))
    {
        this->desktop_entry_.path = kf->get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_PATH);
    }
    if (kf->has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_TERMINAL))
    {
         this->desktop_entry_.terminal = kf->get_boolean(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_TERMINAL);
    }
    if (kf->has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_ACTIONS))
    {
        this->desktop_entry_.actions = kf->get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_ACTIONS);
    }
    if (kf->has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_MIMETYPE))
    {
        this->desktop_entry_.mime_type = kf->get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_MIMETYPE);
    }
    if (kf->has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_CATEGORIES))
    {
        this->desktop_entry_.categories = kf->get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_CATEGORIES);
    }
    if (kf->has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_KEYWORDS))
    {
        this->desktop_entry_.keywords = kf->get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_KEYWORDS);
    }
    if (kf->has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_STARTUPNOTIFY))
    {
        this->desktop_entry_.startup_notify = kf->get_boolean(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_STARTUPNOTIFY);
    }
    // clang-format on

#elif (GTK_MAJOR_VERSION == 3)

    Glib::KeyFile kf;

    if (desktop_file.is_absolute())
    {
        this->filename_ = desktop_file.filename();
        this->path_ = desktop_file;
        this->loaded_ = kf.load_from_file(desktop_file, Glib::KEY_FILE_NONE);
    }
    else
    {
        this->filename_ = desktop_file.filename();
        const auto relative_path = std::filesystem::path() / "applications" / this->filename_;
        std::string relative_full_path;
        try
        {
            this->loaded_ = kf.load_from_data_dirs(relative_path, relative_full_path);
        }
        catch (...) // Glib::KeyFileError, Glib::FileError
        {
            ztd::logger::error("Error opening desktop file: {}", desktop_file.string());
            return;
        }
        this->path_ = relative_full_path;
    }

    if (!this->loaded_)
    {
        ztd::logger::error("Failed to load desktop file: {}", desktop_file.string());
        return;
    }

    // Keys not loaded from .desktop files
    // - Hidden
    // - OnlyShowIn
    // - NotShowIn
    // - DBusActivatable
    // - StartupWMClass
    // - URL
    // - PrefersNonDefaultGPU
    // - SingleMainWindow

    // clang-format off
    if (kf.has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_TYPE))
    {
        this->desktop_entry_.type = kf.get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_TYPE);
    }
    if (kf.has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_NAME))
    {
        this->desktop_entry_.name = kf.get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_NAME);
    }
    if (kf.has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_GENERICNAME))
    {
        this->desktop_entry_.generic_name = kf.get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_GENERICNAME);
    }
    if (kf.has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_NODISPLAY))
    {
        this->desktop_entry_.no_display = kf.get_boolean(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_NODISPLAY);
    }
    if (kf.has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_COMMENT))
    {
        this->desktop_entry_.comment = kf.get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_COMMENT);
    }
    if (kf.has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_ICON))
    {
        this->desktop_entry_.icon = kf.get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_ICON);
    }
    if (kf.has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_TRYEXEC))
    {
        this->desktop_entry_.try_exec = kf.get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_TRYEXEC);
    }
    if (kf.has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_EXEC))
    {
        this->desktop_entry_.exec = kf.get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_EXEC);
    }
    if (kf.has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_PATH))
    {
        this->desktop_entry_.path = kf.get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_PATH);
    }
    if (kf.has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_TERMINAL))
    {
         this->desktop_entry_.terminal = kf.get_boolean(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_TERMINAL);
    }
    if (kf.has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_ACTIONS))
    {
        this->desktop_entry_.actions = kf.get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_ACTIONS);
    }
    if (kf.has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_MIMETYPE))
    {
        this->desktop_entry_.mime_type = kf.get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_MIMETYPE);
    }
    if (kf.has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_CATEGORIES))
    {
        this->desktop_entry_.categories = kf.get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_CATEGORIES);
    }
    if (kf.has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_KEYWORDS))
    {
        this->desktop_entry_.keywords = kf.get_string(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_KEYWORDS);
    }
    if (kf.has_key(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_STARTUPNOTIFY))
    {
        this->desktop_entry_.startup_notify = kf.get_boolean(DESKTOP_ENTRY_GROUP, DESKTOP_ENTRY_KEY_STARTUPNOTIFY);
    }
    // clang-format on

#endif
}

const std::string_view
vfs::desktop::name() const noexcept
{
    return this->filename_;
}

const std::string_view
vfs::desktop::display_name() const noexcept
{
    if (!this->desktop_entry_.name.empty())
    {
        return this->desktop_entry_.name;
    }
    return this->filename_;
}

const std::string_view
vfs::desktop::exec() const noexcept
{
    return this->desktop_entry_.exec;
}

bool
vfs::desktop::use_terminal() const noexcept
{
    return this->desktop_entry_.terminal;
}

const std::filesystem::path&
vfs::desktop::path() const noexcept
{
    return this->path_;
}

const std::string_view
vfs::desktop::icon_name() const noexcept
{
    return this->desktop_entry_.icon;
}

GdkPixbuf*
vfs::desktop::icon(i32 size) const noexcept
{
    GdkPixbuf* desktop_icon = nullptr;

    if (!this->desktop_entry_.icon.empty())
    {
        desktop_icon = vfs_load_icon(this->desktop_entry_.icon, size);
    }

    // fallback to generic icon
    if (!desktop_icon)
    {
        desktop_icon = vfs_load_icon("application-x-executable", size);
    }
    return desktop_icon;
}

const std::vector<std::string>
vfs::desktop::supported_mime_types() const noexcept
{
    return ztd::split(this->desktop_entry_.mime_type, ";");
}

bool
vfs::desktop::open_multiple_files() const noexcept
{
    return this->desktop_entry_.exec.contains("%F") || this->desktop_entry_.exec.contains("%U");
}

const std::optional<std::vector<std::vector<std::string>>>
vfs::desktop::app_exec_generate_desktop_argv(const std::span<const std::filesystem::path> file_list,
                                             bool quote_file_list) const noexcept
{
    // https://standards.freedesktop.org/desktop-entry-spec/desktop-entry-spec-latest.html#exec-variables

    std::vector<std::vector<std::string>> commands = {{ztd::split(this->desktop_entry_.exec, " ")}};

    bool add_files = false;

    if (this->desktop_entry_.exec.contains("%F") || this->desktop_entry_.exec.contains("%U"))
    {
        // %F and %U must always be at the end
        // if (!this->desktop_entry_.exec.ends_with("%F") ||
        //     !this->desktop_entry_.exec.ends_with("%U"))
        // {
        //     ztd::logger::error("Malformed desktop file, %F and %U must always be at the end: {}",
        //                        this->path_.string());
        //     return std::nullopt;
        // }

        for (auto& argv : commands)
        {
            argv.pop_back(); // remove open_files_key
            for (const auto& file : file_list)
            {
                if (quote_file_list)
                {
                    argv.emplace_back(ztd::shell::quote(file.string()));
                }
                else
                {
                    argv.emplace_back(file.string());
                }
            }
        }

        add_files = true;
    }

    if (this->desktop_entry_.exec.contains("%f") || this->desktop_entry_.exec.contains("%e"))
    {
        // %f and %u must always be at the end
        // if (!this->desktop_entry_.exec.ends_with("%f") ||
        //     !this->desktop_entry_.exec.ends_with("%u"))
        // {
        //     ztd::logger::error("Malformed desktop file, %f and %u must always be at the end: {}",
        //                        this->path_.string());
        //     return std::nullopt;
        // }

        // desktop files with these keys can only open one file.
        // spawn multiple copies of the program for each selected file
        commands.insert(commands.begin(), file_list.size() - 1, commands.front());

        for (auto& argv : commands)
        {
            argv.pop_back(); // remove open_file_key
            for (const auto& file : file_list)
            {
                if (quote_file_list)
                {
                    argv.emplace_back(ztd::shell::quote(file.string()));
                }
                else
                {
                    argv.emplace_back(file);
                }
            }
        }
        add_files = true;
    }

    if (!add_files && !file_list.empty())
    {
        ztd::logger::error("Malformed desktop file, trying to open a desktop file without file/url "
                           "keys with a file list: {}",
                           this->path_.string());
    }

    if (this->desktop_entry_.exec.contains("%c"))
    {
        for (auto& argv : commands)
        {
            /* for (const auto [index, arg] : std::views::enumerate(argv)) */
            /* { */
            for (auto iter=argv.begin(); iter!=argv.end(); iter++)
            {
                auto index = std::distance(argv.begin(), iter);
                auto& arg = *iter;
                if (arg != "%c")
                {
                    argv[index] = this->display_name();
                    break;
                }
            }
        }
    }

    if (this->desktop_entry_.exec.contains("%k"))
    {
        for (auto& argv : commands)
        {
            /* for (const auto [index, arg] : std::views::enumerate(argv)) */
            /* { */
            for (auto iter=argv.begin(); iter!=argv.end(); iter++)
            {
                auto index = std::distance(argv.begin(), iter);
                auto& arg = *iter;
                if (arg == "%k")
                {
                    argv[index] = this->path_;
                    break;
                }
            }
        }
    }

    if (this->desktop_entry_.exec.contains("%i"))
    {
        for (auto& argv : commands)
        {
            /* for (const auto [index, arg] : std::views::enumerate(argv)) */
            /* { */
            for (auto iter=argv.begin(); iter!=argv.end(); iter++)
            {
                auto index = std::distance(argv.begin(), iter);
                auto& arg = *iter;
                if (arg == "%i")
                {
                    argv[index] = fmt::format("--icon {}", this->icon_name());
                    break;
                }
            }
        }
    }

    return commands;
}

void
vfs::desktop::exec_in_terminal(const std::filesystem::path& cwd,
                               const std::string_view command) const noexcept
{
    // task
    PtkFileTask* ptask = ptk_file_exec_new(this->display_name(), cwd, nullptr, nullptr);

    ptask->task->exec_command = command;

    ptask->task->exec_terminal = true;
    // ptask->task->exec_keep_terminal = true;  // for test only
    ptask->task->exec_sync = false;
    ptask->task->exec_export = false;

    ptk_file_task_run(ptask);
}

bool
vfs::desktop::open_file(const std::filesystem::path& working_dir,
                        const std::filesystem::path& file_path) const
{
    if (this->desktop_entry_.exec.empty())
    {
        ztd::logger::error(
            fmt::format("Desktop Exec is empty, command not found: {}", this->filename_));
        return false;
    }

    const std::vector<std::filesystem::path> file_paths{file_path};
    this->exec_desktop(working_dir, file_paths);

    return true;
}

bool
vfs::desktop::open_files(const std::filesystem::path& working_dir,
                         const std::span<const std::filesystem::path> file_paths) const
{
    if (this->desktop_entry_.exec.empty())
    {
        ztd::logger::error(
            fmt::format("Desktop Exec is empty, command not found: {}", this->filename_));
        return false;
    }

    if (this->open_multiple_files())
    {
        this->exec_desktop(working_dir, file_paths);
    }
    else
    {
        // app does not accept multiple files, so run multiple times
        for (const auto& open_file : file_paths)
        {
            const std::vector<std::filesystem::path> open_files{open_file};
            this->exec_desktop(working_dir, open_files);
        }
    }

    return true;
}

void
vfs::desktop::exec_desktop(const std::filesystem::path& working_dir,
                           const std::span<const std::filesystem::path> file_paths) const noexcept
{
    const auto check_desktop_commands =
        this->app_exec_generate_desktop_argv(file_paths, this->use_terminal());
    if (!check_desktop_commands)
    {
        return;
    }
    const auto& desktop_commands = check_desktop_commands.value();

    if (this->use_terminal())
    {
        for (const auto& argv : desktop_commands)
        {
            const std::string command = ztd::join(argv, " ");
            this->exec_in_terminal(!this->desktop_entry_.path.empty() ? this->desktop_entry_.path
                                                                      : working_dir.string(),
                                   command);
        }
    }
    else
    {
        for (const auto& argv : desktop_commands)
        {
            Glib::spawn_async_with_pipes(
                !this->desktop_entry_.path.empty() ? this->desktop_entry_.path
                                                   : working_dir.string(),
                argv,
#if (GTK_MAJOR_VERSION == 4)
                Glib::SpawnFlags::SEARCH_PATH | Glib::SpawnFlags::STDOUT_TO_DEV_NULL |
                    Glib::SpawnFlags::STDERR_TO_DEV_NULL,
#elif (GTK_MAJOR_VERSION == 3)
                Glib::SpawnFlags::SPAWN_SEARCH_PATH | Glib::SpawnFlags::SPAWN_STDOUT_TO_DEV_NULL |
                    Glib::SpawnFlags::SPAWN_STDERR_TO_DEV_NULL,
#endif
                Glib::SlotSpawnChildSetup(),
                nullptr,
                nullptr,
                nullptr,
                nullptr);
        }
    }
}
