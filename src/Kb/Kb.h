/*
  Kb.h - lightweight on-screen keyboard

  Copyright (C) 2026 @steadramon

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

// lv_buttonmatrix-based on-screen keyboard. ~18 KB lighter than lv_keyboard
// across the three layouts in use (password / IP / hostname). Fires
// LV_EVENT_READY on OK so handlers wired against lv_keyboard's events work
// unchanged.

#pragma once

#include <lvgl.h>
#include <stdint.h>

namespace Kb {

enum class Layout : uint8_t {
  Password,    // full charset, lower/upper/symbols pages
  Ip,          // digits + . + :
  Hostname,    // a-z + 0-9 + -
};

lv_obj_t * create(lv_obj_t * parent, lv_obj_t * textarea, Layout layout);

}  // namespace Kb
