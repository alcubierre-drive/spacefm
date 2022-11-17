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

#include <array>
#include <vector>

#include <fmt/format.h>

#include <glibmm.h>
#include <glibmm/keyfile.h>

#include <ztd/ztd.hxx>
#include <ztd/ztd_logger.hxx>

// sfm breaks vfs independence for exec_in_terminal
#include "ptk/ptk-file-task.hxx"

#include "vfs/vfs-utils.hxx"
#include "vfs/vfs-user-dir.hxx"
#include "vfs/vfs-app-desktop.hxx"

#include "utils.hxx"

VFSAppDesktop::VFSAppDesktop(std::string_view open_file_name) noexcept
{
    // LOG_INFO("VFSAppDesktop constructor");

    bool load;

    const auto kf = Glib::KeyFile::create();

    if (Glib::path_is_absolute(open_file_name.data()))
    {
        this->file_name = Glib::path_get_basename(open_file_name.data());
        this->full_path = open_file_name.data();
        load = kf->load_from_file(open_file_name.data(), Glib::KeyFile::Flags::NONE);
    }
    else
    {
        this->file_name = open_file_name.data();
        const std::string relative_path = Glib::build_filename("applications", this->file_name);
        load = kf->load_from_data_dirs(relative_path, this->full_path, Glib::KeyFile::Flags::NONE);
    }

    if (load)
    {
        static const std::string desktop_entry = "Desktop Entry";
        Glib::ustring g_disp_name, g_exec, g_icon_name, g_path;

        try
        {
            g_disp_name = kf->get_string(desktop_entry, "Name");
            if (!g_disp_name.empty())
                this->disp_name = g_disp_name;
        }
        catch (Glib::KeyFileError)
        {
            this->disp_name = "";
        }

        try
        {
            g_exec = kf->get_string(desktop_entry, "Exec");
            if (!g_exec.empty())
                this->exec = g_exec;
        }
        catch (Glib::KeyFileError)
        {
            this->exec = "";
        }

        try
        {
            g_icon_name = kf->get_string(desktop_entry, "Icon");
            if (!g_icon_name.empty())
                this->icon_name = g_icon_name;
        }
        catch (Glib::KeyFileError)
        {
            this->icon_name = "";
        }

        try
        {
            g_path = kf->get_string(desktop_entry, "Path");
            if (!g_path.empty())
                this->path = g_path;
        }
        catch (Glib::KeyFileError)
        {
            this->path = "";
        }

        try
        {
            this->terminal = kf->get_boolean(desktop_entry, "Terminal");
        }
        catch (Glib::KeyFileError)
        {
            this->terminal = false;
        }
    }
    else
    {
        this->exec = this->file_name;
    }
}

const std::string&
VFSAppDesktop::get_name() const noexcept
{
    return this->file_name;
}

const std::string&
VFSAppDesktop::get_disp_name() const noexcept
{
    if (!this->disp_name.empty())
        return this->disp_name;
    return this->file_name;
}

const std::string&
VFSAppDesktop::get_exec() const noexcept
{
    return this->exec;
}

bool
VFSAppDesktop::use_terminal() const noexcept
{
    return this->terminal;
}

const std::string&
VFSAppDesktop::get_full_path() const noexcept
{
    return this->full_path;
}

const std::string&
VFSAppDesktop::get_icon_name() const noexcept
{
    return this->icon_name;
}

GdkPixbuf*
VFSAppDesktop::get_icon(i32 size) const noexcept
{
    GdkPixbuf* icon = nullptr;

    if (!this->icon_name.empty())
    {
        icon = vfs_load_icon(this->icon_name, size);
    }

    // fallback to generic icon
    if (!icon)
    {
        icon = vfs_load_icon("application-x-executable", size);
        // fallback to generic icon
        if (!icon)
            icon = vfs_load_icon("gnome-mime-application-x-executable", size);
    }
    return icon;
}

bool
VFSAppDesktop::open_multiple_files() const noexcept
{
    if (this->exec.empty())
        return false;

    static constexpr std::array<std::string_view, 2> keys{"%U", "%F"};
    if (ztd::contains(this->exec, keys))
        return true;

    return false;
}

const std::vector<std::string>
VFSAppDesktop::app_exec_to_argv(const std::vector<std::string>& file_list) const noexcept
{
    // https://standards.freedesktop.org/desktop-entry-spec/desktop-entry-spec-latest.html#exec-variables

    std::vector<std::string> argv{ztd::split(this->exec, " ")};

    bool add_files = false;

    static constexpr std::array<std::string_view, 2> open_files_keys{"%F", "%U"};
    if (ztd::contains(this->exec, open_files_keys))
    {
        // TODO
        // %F and %U should always be at the end
        // probably need a better way to do this

        argv.pop_back(); // remove open_files_key
        for (std::string_view file : file_list)
        {
            argv.emplace_back(file.data());
        }

        add_files = true;
    }

    static constexpr std::array<std::string_view, 2> open_file_keys{"%f", "%u"};
    if (ztd::contains(this->exec, open_file_keys))
    {
        // TODO
        // %f and %u should always be at the end
        // probably need a better way to do this

        argv.pop_back(); // remove open_file_key
        for (std::string_view file : file_list)
        {
            argv.emplace_back(file.data());
        }

        add_files = true;
    }

    if (ztd::contains(this->exec, "%c"))
    {
        for (std::string& arg : argv)
        {
            if (!ztd::contains(arg, "%c"))
                continue;

            arg = ztd::replace(arg, "%c", this->get_disp_name());
            break;
        }
    }

    if (ztd::contains(this->exec, "%i"))
    {
        for (std::string& arg : argv)
        {
            if (!ztd::contains(arg, "%i"))
                continue;

            arg = ztd::replace(arg, "%i", fmt::format("--icon {}", this->get_icon_name()));
            break;
        }
    }

    if (!add_files)
    {
        for (std::string_view file : file_list)
        {
            argv.emplace_back(file.data());
        }
    }

    return argv;
}

const std::string
VFSAppDesktop::app_exec_to_command_line(const std::vector<std::string>& file_list) const noexcept
{
    // https://standards.freedesktop.org/desktop-entry-spec/desktop-entry-spec-latest.html#exec-variables

    std::string cmd = this->exec;

    bool add_files = false;

    static constexpr std::array<std::string_view, 2> open_files_keys{"%F", "%U"};
    if (ztd::contains(cmd, open_files_keys))
    {
        std::string tmp;
        for (std::string_view file : file_list)
        {
            tmp.append(bash_quote(file));
            tmp.append(" ");
        }
        if (ztd::contains(cmd, open_files_keys[0]))
            cmd = ztd::replace(cmd, open_files_keys[0], tmp);
        if (ztd::contains(cmd, open_files_keys[1]))
            cmd = ztd::replace(cmd, open_files_keys[1], tmp);

        add_files = true;
    }

    static constexpr std::array<std::string_view, 2> open_file_keys{"%f", "%u"};
    if (ztd::contains(cmd, open_file_keys))
    {
        std::string tmp;
        for (std::string_view file : file_list)
        {
            tmp.append(bash_quote(file));
        }
        if (ztd::contains(cmd, open_file_keys[0]))
            cmd = ztd::replace(cmd, open_file_keys[0], tmp);
        if (ztd::contains(cmd, open_file_keys[1]))
            cmd = ztd::replace(cmd, open_file_keys[1], tmp);

        add_files = true;
    }

    if (ztd::contains(cmd, "%c"))
    {
        cmd = ztd::replace(cmd, "%c", this->get_disp_name());
    }

    if (ztd::contains(cmd, "%i"))
    {
        const std::string icon = fmt::format("--icon {}", this->get_icon_name());
        cmd = ztd::replace(cmd, "%i", icon);
    }

    if (!add_files)
    {
        std::string tmp;

        cmd.append(" ");
        for (std::string_view file : file_list)
        {
            tmp.append(bash_quote(file));
            tmp.append(" ");
        }
        cmd.append(tmp);
    }

    return cmd;
}

void
VFSAppDesktop::exec_in_terminal(std::string_view app_name, std::string_view cwd,
                                std::string_view cmd) const noexcept
{
    // task
    PtkFileTask* ptask = ptk_file_exec_new(app_name, cwd, nullptr, nullptr);

    ptask->task->exec_command = cmd.data();

    ptask->task->exec_terminal = true;
    // ptask->task->exec_keep_terminal = true;  // for test only
    ptask->task->exec_sync = false;
    ptask->task->exec_export = false;

    ptk_file_task_run(ptask);
}

bool
VFSAppDesktop::open_files(std::string_view working_dir,
                          const std::vector<std::string>& file_paths) const
{
    if (this->exec.empty())
    {
        const std::string msg = fmt::format("Command not found\n\n{}", this->file_name);
        throw VFSAppDesktopException(msg);
    }

    if (this->open_multiple_files())
    {
        this->exec_desktop(working_dir, file_paths);
    }
    else
    {
        // app does not accept multiple files, so run multiple times
        for (std::string_view open_file : file_paths)
        {
            // const std::vector<std::string> open_files{open_file};
            this->exec_desktop(working_dir, {open_file.data()});
        }
    }
    return true;
}

void
VFSAppDesktop::exec_desktop(std::string_view working_dir,
                            const std::vector<std::string>& file_paths) const noexcept
{
    // LOG_DEBUG("Execute: {}", command);

    if (this->use_terminal())
    {
        const std::string command = this->app_exec_to_command_line(file_paths);
        if (command.empty())
            return;

        const std::string app_name = this->get_disp_name();
        exec_in_terminal(app_name, !this->path.empty() ? this->path : working_dir, command);
    }
    else
    {
        const std::vector<std::string> argv = this->app_exec_to_argv(file_paths);
        if (argv.empty())
            return;

        Glib::spawn_async_with_pipes(!this->path.empty() ? this->path : working_dir.data(),
                                     argv,
                                     Glib::SpawnFlags::SEARCH_PATH |
                                         Glib::SpawnFlags::STDOUT_TO_DEV_NULL |
                                         Glib::SpawnFlags::STDERR_TO_DEV_NULL,
                                     Glib::SlotSpawnChildSetup(),
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     nullptr);
    }
}
