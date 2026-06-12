/*
  Version.h - build version

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

// CI writes include/VersionGenerated.h from the release tag; local
// builds fall through to "devel".

#pragma once

#if __has_include("VersionGenerated.h")
#include "VersionGenerated.h"
#endif

#ifndef GADGET_VERSION
#define GADGET_VERSION "devel"
#endif
