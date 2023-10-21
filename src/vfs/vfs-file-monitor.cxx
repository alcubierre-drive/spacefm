/**
 * Copyright (C) 2014 IgnorantGuru <ignorantguru@gmx.com>
 * Copyright (C) 2006 Hong Jen Yee (PCMan) <pcman.tw@gmail.com>
 * Copyright (C) 2005 Red Hat, Inc.
 * Copyright (C) 2006 Mark McLoughlin
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

#include <filesystem>

#include <memory>

#include <sys/inotify.h>

#include <gtkmm.h>
#include <glibmm.h>
#include <sigc++/sigc++.h>

#include <ztd/ztd.hxx>
#include <ztd/ztd_logger.hxx>

#include "vfs/vfs-file-monitor.hxx"

// #define VFS_FILE_MONITOR_DEBUG

inline constexpr u32 EVENT_SIZE = (sizeof(inotify_event));
inline constexpr u32 EVENT_BUF_LEN = (1024 * (EVENT_SIZE + 16));

vfs::file_monitor::file_monitor(const std::filesystem::path& path,
                                vfs::file_monitor_callback_t callback, void* user_data)
    : path_(path)
{
    this->inotify_fd_ = inotify_init();
    if (this->inotify_fd_ == -1)
    {
        // ztd::logger::error("failed to initialize inotify");
        throw std::runtime_error("failed to initialize inotify");
    }

    this->inotify_io_channel = Glib::IOChannel::create_from_fd(this->inotify_fd_);
    this->inotify_io_channel->set_buffered(true);
#if (GTK_MAJOR_VERSION == 4)
    this->inotify_io_channel->set_flags(Glib::IOFlags::NONBLOCK);
#elif (GTK_MAJOR_VERSION == 3)
    this->inotify_io_channel->set_flags(Glib::IO_FLAG_NONBLOCK);
#endif

    this->signal_io_handler =
        Glib::signal_io().connect(sigc::mem_fun(this, &file_monitor::on_inotify_event),
                                  this->inotify_io_channel,
                                  Glib::IOCondition::IO_IN | Glib::IOCondition::IO_PRI |
                                      Glib::IOCondition::IO_HUP | Glib::IOCondition::IO_ERR);

    // inotify does not follow symlinks, need to get real path
    const auto real_path = std::filesystem::absolute(this->path_);

    this->inotify_wd_ = inotify_add_watch(this->inotify_fd_,
                                          real_path.c_str(),
                                          IN_MODIFY | IN_CREATE | IN_DELETE | IN_DELETE_SELF |
                                              IN_MOVE | IN_MOVE_SELF | IN_UNMOUNT | IN_ATTRIB);
    if (this->inotify_wd_ == -1)
    {
        throw std::runtime_error(std::format("Failed to add inotify watch on '{}' ({})",
                                             real_path.string(),
                                             this->path_.string()));
    }

    if (callback)
    { // Install a callback
        this->callback_ = callback_entry{callback, user_data};
    }

    // ztd::logger::debug("vfs::file_monitor::file_monitor({})  {} ({})  {}", fmt::ptr(this), real_path, this->path_, this->inotify_wd_);
}

vfs::file_monitor::~file_monitor()
{
    // ztd::logger::debug("vfs::file_monitor::~file_monitor({}) {}", fmt::ptr(this),this->path_);

    this->signal_io_handler.disconnect();

    inotify_rm_watch(this->inotify_fd_, this->inotify_wd_);
    close(this->inotify_fd_);
}

void
vfs::file_monitor::dispatch_event(vfs::file_monitor_event event,
                                  const std::filesystem::path& path) noexcept
{
    this->callback_.callback(event, path, this->callback_.user_data);
}

bool
vfs::file_monitor::on_inotify_event(const Glib::IOCondition condition)
{
    // ztd::logger::debug("vfs::file_monitor::on_inotify_event({})  {}", fmt::ptr(this), this->path_);

    if (condition == Glib::IOCondition::IO_HUP || condition == Glib::IOCondition::IO_ERR)
    {
        ztd::logger::error("Disconnected from inotify server");
        return false;
    }

    char buffer[EVENT_BUF_LEN];
    const auto length = read(this->inotify_fd_, buffer, EVENT_BUF_LEN);
    if (length < 0)
    {
        ztd::logger::error("Error reading inotify event: {}", std::strerror(errno));
        return false;
    }

    u32 i = 0;
    while (i < length)
    {
        const auto event = (inotify_event*)&buffer[i];
        if (event->len)
        {
            const std::filesystem::path event_filename = event->name;

            std::filesystem::path event_path;
            if (std::filesystem::is_directory(this->path_))
            {
                event_path = this->path_ / event_filename;
            }
            else
            {
                event_path = this->path_.parent_path() / event_filename;
            }

            vfs::file_monitor_event monitor_event;
            if (event->mask & (IN_CREATE | IN_MOVED_TO))
            {
                monitor_event = vfs::file_monitor_event::created;
#if defined(VFS_FILE_MONITOR_DEBUG)
                ztd::logger::debug("inotify-event MASK={} CREATE={}", event->mask, event_path);
#endif
            }
            else if (event->mask & (IN_DELETE | IN_MOVED_FROM | IN_DELETE_SELF | IN_UNMOUNT))
            {
                monitor_event = vfs::file_monitor_event::deleted;
#if defined(VFS_FILE_MONITOR_DEBUG)
                ztd::logger::debug("inotify-event MASK={} DELETE={}", event->mask, event_path);
#endif
            }
            else if (event->mask & (IN_MODIFY | IN_ATTRIB))
            {
                monitor_event = vfs::file_monitor_event::changed;
#if defined(VFS_FILE_MONITOR_DEBUG)
                ztd::logger::debug("inotify-event MASK={} CHANGE={}", event->mask, event_path);
#endif
            }
            else
            { // IN_IGNORED not handled
                monitor_event = vfs::file_monitor_event::other;
#if defined(VFS_FILE_MONITOR_DEBUG)
                ztd::logger::debug("inotify-event MASK={} OTHER={}", event->mask, event_path);
#endif
            }

            this->dispatch_event(monitor_event, event_path);
        }
        i += EVENT_SIZE + event->len;
    }

    return true;
}
