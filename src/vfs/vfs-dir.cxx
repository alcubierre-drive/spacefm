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

#include <vector>

#include <algorithm>

#include <mutex>

#include <optional>

#include <memory>

#include <functional>

#include <fstream>

#include <cassert>

#include <glibmm.h>

#include <malloc.h>

#include <ztd/ztd.hxx>
#include <ztd/ztd_logger.hxx>

#include "write.hxx"
#include "utils.hxx"

#include "vfs/vfs-async-thread.hxx"
#include "vfs/vfs-async-task.hxx"
#include "vfs/vfs-file.hxx"
#include "vfs/vfs-thumbnailer.hxx"
#include "vfs/vfs-volume.hxx"

#include "vfs/vfs-dir.hxx"

static ztd::smart_cache<std::filesystem::path, vfs::dir> dir_smart_cache;

vfs::dir::dir(const std::filesystem::path& path) : path_(path)
{
    // ztd::logger::debug("vfs::dir::dir({})   {}", fmt::ptr(this), path);

    this->update_avoid_changes();

    this->task_ = vfs::async_thread::create(std::bind(&vfs::dir::load_thread, this));

    this->signal_task_load_dir = this->task_->add_event<spacefm::signal::task_finish>(
        std::bind(&vfs::dir::on_list_task_finished, this, std::placeholders::_1));

    this->task_->run(); /* asynchronous operation */
}

vfs::dir::~dir()
{
    // ztd::logger::debug("vfs::dir::~dir({})  {}", fmt::ptr(this), path);

    this->signal_task_load_dir.disconnect();

    if (this->task_)
    {
        // FIXME: should we generate a "file-list" signal to indicate the dir loading was cancelled?

        // ztd::logger::trace("this->task({})", fmt::ptr(this->task));
        this->task_->cancel();
    }
}

const std::shared_ptr<vfs::dir>
vfs::dir::create(const std::filesystem::path& path) noexcept
{
    std::shared_ptr<vfs::dir> dir = nullptr;
    if (dir_smart_cache.contains(path))
    {
        dir = dir_smart_cache.at(path);
        // ztd::logger::debug("vfs::dir::dir({}) cache   {}", fmt::ptr(dir.get()), path);
    }
    else
    {
        dir = dir_smart_cache.create(
            path,
            std::bind([](const auto& path) { return std::make_shared<vfs::dir>(path); }, path));
        // ztd::logger::debug("vfs::dir::dir({}) new     {}", fmt::ptr(dir.get()), path);
    }
    // ztd::logger::debug("dir({})     {}", fmt::ptr(dir.get()), path);
    return dir;
}

void
vfs::dir::on_list_task_finished(bool is_cancelled)
{
    this->task_ = nullptr;
    this->run_event<spacefm::signal::file_listed>(is_cancelled);
    this->file_listed_ = true;
    this->load_complete_ = true;
}

const std::filesystem::path&
vfs::dir::path() const noexcept
{
    return this->path_;
}

const std::span<const std::shared_ptr<vfs::file>>
vfs::dir::files() const noexcept
{
    return this->files_;
}

bool
vfs::dir::avoid_changes() const noexcept
{
    return this->avoid_changes_;
}

u64
vfs::dir::hidden_files() const noexcept
{
    return this->xhidden_count_;
}

void
vfs::dir::update_avoid_changes() noexcept
{
    this->avoid_changes_ = vfs_volume_dir_avoid_changes(this->path_);
}

const std::optional<std::vector<std::filesystem::path>>
vfs::dir::get_hidden_files() const noexcept
{
    std::vector<std::filesystem::path> hidden;

    // Read .hidden into string
    const auto hidden_path = this->path_ / ".hidden";

    if (!std::filesystem::is_regular_file(hidden_path))
    {
        return std::nullopt;
    }

    // test access first because open() on missing file may cause
    // long delay on nfs
    if (!have_rw_access(hidden_path))
    {
        return std::nullopt;
    }

    std::ifstream file(hidden_path);
    if (!file)
    {
        ztd::logger::error("Failed to open the file: {}", hidden_path.string());
        return std::nullopt;
    }

    std::string line;
    while (std::getline(file, line))
    {
        const auto hidden_file = std::filesystem::path(ztd::strip(line));
        if (hidden_file.is_absolute())
        {
            ztd::logger::warn("Absolute path ignored in {}", hidden_path.string());
            continue;
        }

        hidden.push_back(hidden_file);
    }
    file.close();

    return hidden;
}

void
vfs::dir::load_thread()
{
    this->file_listed_ = false;
    this->load_complete_ = false;
    this->xhidden_count_ = 0;

    /* Install file alteration monitor */
    this->monitor_ = vfs::monitor::create(
        this->path_,
        std::bind(&vfs::dir::on_monitor_event, this, std::placeholders::_1, std::placeholders::_2));

    // MOD  dir contains .hidden file?
    const auto hidden_files = this->get_hidden_files();

    for (const auto& dfile : std::filesystem::directory_iterator(this->path_))
    {
        if (this->task_->is_canceled())
        {
            break;
        }

        const auto file_name = dfile.path().filename();
        const auto full_path = this->path_ / file_name;

        // MOD ignore if in .hidden
        if (hidden_files)
        {
            bool hide_file = false;
            for (const auto& hidden_file : hidden_files.value())
            {
                // if (ztd::same(hidden_file.string(), file_name.string()))
                std::error_code ec;
                const bool equivalent = std::filesystem::equivalent(hidden_file, file_name, ec);
                if (!ec && equivalent)
                {
                    hide_file = true;
                    this->xhidden_count_++;
                    break;
                }
            }
            if (hide_file)
            {
                continue;
            }
        }

        const auto file = vfs::file::create(full_path);
        this->files_.emplace_back(file);
    }
}

/* Callback function which will be called when monitored events happen */
void
vfs::dir::on_monitor_event(const vfs::monitor::event event, const std::filesystem::path& path)
{
    switch (event)
    {
        case vfs::monitor::event::created:
            this->emit_file_created(path.filename(), false);
            break;
        case vfs::monitor::event::deleted:
            this->emit_file_deleted(path.filename(), nullptr);
            break;
        case vfs::monitor::event::changed:
            this->emit_file_changed(path.filename(), nullptr, false);
            break;
        case vfs::monitor::event::other:
            break;
    }
}

void
vfs_dir_mime_type_reload()
{
    // ztd::logger::debug("reload mime-type");
    // const auto action = [](const auto& dir) { dir.second.lock()->reload_mime_type(); };
    // std::ranges::for_each(dir_smart_cache, action);
}

/**
* vfs::dir class
*/

const std::shared_ptr<vfs::file>
vfs::dir::find_file(const std::filesystem::path& filename,
                    const std::shared_ptr<vfs::file>& file) const noexcept
{
    for (const auto& file2 : this->files_)
    {
        if (file == file2)
        {
            return file2;
        }
        if (file2->name() == filename.string())
        {
            return file2;
        }
    }
    return nullptr;
}

bool
vfs::dir::add_hidden(const std::shared_ptr<vfs::file>& file) const noexcept
{
    const auto file_path = std::filesystem::path() / this->path_ / ".hidden";
    const std::string data = fmt::format("{}\n", file->name());

    return write_file(file_path, data);
}

void
vfs::dir::cancel_all_thumbnail_requests() noexcept
{
    this->thumbnailer = nullptr;
}

void
vfs::dir::load_thumbnail(const std::shared_ptr<vfs::file>& file, const bool is_big) noexcept
{
    bool new_task = false;

    // ztd::logger::debug("request thumbnail: {}, is_big: {}", file->name(), is_big);
    if (!this->thumbnailer)
    {
        // ztd::logger::debug("new_task: !this->thumbnailer");
        this->thumbnailer = vfs::thumbnailer::create(this->shared_from_this());
        assert(this->thumbnailer != nullptr);
        new_task = true;
    }

    this->thumbnailer->loader_request(file, is_big);

    if (new_task)
    {
        // ztd::logger::debug("new_task: this->thumbnailer->queue={}", this->thumbnailer->queue.size());
        this->thumbnailer->task->run();
    }
}

bool
vfs::dir::is_file_listed() const noexcept
{
    return this->file_listed_;
}

bool
vfs::dir::is_directory_empty() const noexcept
{
    return this->files_.empty();
}

struct __contains_fn
{
    template<std::input_iterator I, std::sentinel_for<I> S,
             class T, class Proj = std::identity>
    requires std::indirect_binary_predicate<std::ranges::equal_to, std::projected<I, Proj>,
                                            const T*>
    constexpr bool operator()(I first, S last, const T& value, Proj proj = {}) const
    {
        return std::ranges::find(std::move(first), last, value, proj) != last;
    }

    template<std::ranges::input_range R, class T, class Proj = std::identity>
    requires std::indirect_binary_predicate<std::ranges::equal_to,
                                            std::projected<std::ranges::iterator_t<R>, Proj>,
                                            const T*>
    constexpr bool operator()(R&& r, const T& value, Proj proj = {}) const
    {
        return (*this)(std::ranges::begin(r), std::ranges::end(r), std::move(value), proj);
    }
};
inline constexpr __contains_fn ztd_contains {};

bool
vfs::dir::update_file_info(const std::shared_ptr<vfs::file>& file) noexcept
{
    bool ret = false;

    const bool is_file_valid = file->update();
    if (is_file_valid)
    {
        ret = true;
    }
    else /* The file does not exist */
    {
        if (ztd_contains(this->files_, file))
        {
            ztd::remove(this->files_, file);
            if (file)
            {
                this->run_event<spacefm::signal::file_deleted>(file);
            }
        }
        ret = false;
    }

    return ret;
}

void
vfs::dir::update_changed_files() noexcept
{
    //std::scoped_lock<std::mutex> lock(this->mutex);

    if (this->changed_files_.empty())
    {
        return;
    }

    for (const auto& file : this->changed_files_)
    {
        if (this->update_file_info(file))
        {
            this->run_event<spacefm::signal::file_changed>(file);
        }
        // else was deleted, signaled, and unrefed in update_file_info
    }
    this->changed_files_.clear();
}

void
vfs::dir::update_created_files() noexcept
{
    // std::scoped_lock<std::mutex> lock(this->mutex);

    if (this->created_files_.empty())
    {
        return;
    }

    for (const auto& created_file : this->created_files_)
    {
        const auto file_found = this->find_file(created_file, nullptr);
        if (!file_found)
        {
            // file is not in dir this->files_
            const auto full_path = std::filesystem::path() / this->path_ / created_file;
            if (std::filesystem::exists(full_path))
            {
                const auto file = vfs::file::create(full_path);
                this->files_.emplace_back(file);

                this->run_event<spacefm::signal::file_created>(file);
            }
            // else file does not exist in filesystem
        }
        else
        {
            // file already exists in dir this->files_
            if (this->update_file_info(file_found))
            {
                this->run_event<spacefm::signal::file_changed>(file_found);
            }
            // else was deleted, signaled, and unrefed in update_file_info
        }
    }
    this->created_files_.clear();
}

void
vfs::dir::unload_thumbnails(bool is_big) noexcept
{
    std::scoped_lock<std::mutex> lock(this->lock_);

    for (const auto& file : this->files_)
    {
        if (is_big)
        {
            file->unload_big_thumbnail();
        }
        else
        {
            file->unload_small_thumbnail();
        }
    }

    /* Ensuring free space at the end of the heap is freed to the OS,
     * mainly to deal with the possibility thousands of large thumbnails
     * have been freed but the memory not actually released by SpaceFM */
    malloc_trim(0);
}

void
vfs::dir::reload_mime_type() noexcept
{
    std::scoped_lock<std::mutex> lock(this->lock_);

    if (this->is_directory_empty())
    {
        return;
    }

    const auto reload_file_mime_action = [](const auto& file) { file->reload_mime_type(); };
    std::ranges::for_each(this->files_, reload_file_mime_action);

    const auto signal_file_changed_action = [this](const auto& file)
    { this->run_event<spacefm::signal::file_changed>(file); };
    std::ranges::for_each(this->files_, signal_file_changed_action);
}

/* signal handlers */
void
vfs::dir::emit_file_created(const std::filesystem::path& filename, bool force) noexcept
{
    (void)force;
    // Ignore avoid_changes for creation of files
    // if (!force && this->avoid_changes_)
    // {
    //     return;
    // }

    if (std::filesystem::equivalent(filename, this->path_))
    { // Special Case: The directory itself was created?
        return;
    }

    this->created_files_.emplace_back(filename);

    this->update_changed_files();
    this->update_created_files();
}

void
vfs::dir::emit_file_deleted(const std::filesystem::path& filename,
                            const std::shared_ptr<vfs::file>& file) noexcept
{
    std::scoped_lock<std::mutex> lock(this->lock_);

    if (std::filesystem::equivalent(filename, this->path_))
    {
        /* Special Case: The directory itself was deleted... */

        /* clear the whole list */
        this->files_.clear();

        this->run_event<spacefm::signal::file_deleted>(nullptr);

        return;
    }

    const auto file_found = this->find_file(filename, file);
    if (file_found)
    {
        if (!ztd_contains(this->changed_files_, file_found))
        {
            this->changed_files_.emplace_back(file_found);

            this->update_changed_files();
            this->update_created_files();
        }
    }
}

void
vfs::dir::emit_file_changed(const std::filesystem::path& filename,
                            const std::shared_ptr<vfs::file>& file, bool force) noexcept
{
    std::scoped_lock<std::mutex> lock(this->lock_);

    // ztd::logger::info("vfs::dir::emit_file_changed dir={} filename={} avoid={}", this->path_, filename, this->avoid_changes_);

    if (!force && this->avoid_changes_)
    {
        return;
    }

    if (std::filesystem::equivalent(filename, this->path_))
    {
        // Special Case: The directory itself was changed
        this->run_event<spacefm::signal::file_changed>(nullptr);
        return;
    }

    const auto file_found = this->find_file(filename, file);
    if (file_found)
    {
        if (!ztd_contains(this->changed_files_, file_found))
        {
            if (force)
            {
                this->changed_files_.emplace_back(file_found);

                this->update_changed_files();
                this->update_created_files();
            }
            else if (this->update_file_info(file_found)) // update file info the first time
            {
                this->changed_files_.emplace_back(file_found);

                this->update_changed_files();
                this->update_created_files();

                this->run_event<spacefm::signal::file_changed>(file_found);
            }
        }
    }
}

void
vfs::dir::emit_thumbnail_loaded(const std::shared_ptr<vfs::file>& file) noexcept
{
    std::scoped_lock<std::mutex> lock(this->lock_);

    const auto file_found = this->find_file(file->name(), file);
    if (file_found)
    {
        assert(file == file_found);
        this->run_event<spacefm::signal::file_thumbnail_loaded>(file_found);
    }
}
