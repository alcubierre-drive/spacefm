/**
 * Copyright (C) 2007 PCMan <pcman.tw@gmail.com>
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

#pragma once

#include <string>
#include <string_view>

#include <vector>

#include <glib.h>

enum MimeTypeAction
{
    MIME_TYPE_ACTION_DEFAULT,
    MIME_TYPE_ACTION_APPEND,
    MIME_TYPE_ACTION_REMOVE
};

/*
 *  Get a list of applications supporting this mime-type
 */
const std::vector<std::string> mime_type_get_actions(std::string_view type);

/*
 * Add an applications used to open this mime-type
 * desktop_id is the name of *.desktop file.
 *
 * custom_desktop: used to store name of the newly created user-custom desktop file, can be nullptr.
 */
void mime_type_add_action(const char* type, const char* desktop_id, char** custom_desktop);

/*
 * Check if an applications currently set to open this mime-type
 * desktop_id is the name of *.desktop file.
 */
// bool mime_type_has_action(const char* type, const char* desktop_id);

/*
 * Get default applications used to open this mime-type
 *
 * The returned string was newly allocated, and should be freed when no longer
 * used.  If nullptr is returned, that means a default app is not set for this
 * mime-type.  This is very roughly based on specs:
 * http://standards.freedesktop.org/mime-apps-spec/mime-apps-spec-latest.html
 *
 * The old defaults.list is also checked.
 */
char* mime_type_get_default_action(std::string_view type);

/*
 * Set applications used to open or never used to open this mime-type
 * desktop_id is the name of *.desktop file.
 * action ==
 *     MimeTypeAction::MIME_TYPE_ACTION_DEFAULT - make desktop_id the default app
 *     MimeTypeAction::MIME_TYPE_ACTION_APPEND  - add desktop_id to Default and Added apps
 *     MimeTypeAction::MIME_TYPE_ACTION_REMOVE  - add desktop id to Removed apps
 *
 * http://standards.freedesktop.org/mime-apps-spec/mime-apps-spec-latest.html
 */
void mime_type_update_association(const char* type, const char* desktop_id, i32 action);

/* Locate the file path of desktop file by desktop_id */
char* mime_type_locate_desktop_file(const char* dir, const char* desktop_id);
