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

#pragma once

#include <string>
#include <string_view>

#include <filesystem>

#include <map>

#include <memory>

#include <ztd/ztd.hxx>

// trash directories. There might be several on a system:
//
// One in $XDG_DATA_HOME/Trash or ~/.local/share/Trash
// if $XDG_DATA_HOME is not set
//
// Every mountpoint will get a trash directory at $TOPLEVEL/.Trash-$UID.
namespace vfs
{
    // This class implements some of the XDG Trash specification:
    //
    // https://standards.freedesktop.org/trash-spec/trashspec-1.0.html
    struct trash_can
    {
        trash_can() noexcept;
        static const std::shared_ptr<vfs::trash_can> create() noexcept;

        // Move a file or directory into the trash.
        static bool trash(const std::filesystem::path& path) noexcept;

        // Restore a file or directory from the trash to its original location.
        // Currently a NOOP
        static bool restore(const std::filesystem::path& path) noexcept;

        // Empty all trash cans
        // Currently a NOOP
        static void empty() noexcept;

        // Empty a trash can
        // Currently a NOOP
        static void empty(const std::filesystem::path& path) noexcept;

      private:
        struct trash_dir
        {
            // Create the trash directory and subdirectories if they do not exist.
            trash_dir(const std::filesystem::path& path) noexcept;
            ~trash_dir() = default;

            // Get a unique name for use within the trash directory
            const std::string unique_name(const std::filesystem::path& path) const noexcept;

            void create_trash_dir() const noexcept;

            // Create a .trashinfo file for a file or directory 'path'
            void create_trash_info(const std::filesystem::path& path,
                                   const std::string_view target_name) const noexcept;

            // Move a file or directory into the trash directory
            void move(const std::filesystem::path& path,
                      const std::string_view target_name) const noexcept;

          private:
            const std::string create_trash_date(const std::time_t time) const noexcept;

            // Create a directory if it does not exist
            static void check_dir_exists(const std::filesystem::path& dir) noexcept;

            // the full path for this trash directory
            std::filesystem::path trash_path_{};
            // the path of the "files" subdirectory of this trash dir
            std::filesystem::path files_path_{};
            // the path of the "info" subdirectory of this trash dir
            std::filesystem::path info_path_{};
        };

        // return the mount point id for the file or directory
        static u64 mount_id(const std::filesystem::path& path) noexcept;

        // Find the toplevel directory (mount point) for the device that 'path' is on.
        static const std::filesystem::path toplevel(const std::filesystem::path& path) noexcept;

        // Return the trash dir to use for 'path'.
        const std::shared_ptr<trash_dir> get_trash_dir(const std::filesystem::path& path) noexcept;

        // Data Members
        std::map<u64, std::shared_ptr<trash_dir>> trash_dirs_;
    };
} // namespace vfs

void vfs_trash_init();
