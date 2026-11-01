/*
Instrumental Isolator for OBS
Copyright (C) 2026 Daren Wallace

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* OBS source id of the filter. Stored in scene collections: never change. */
#define INSTRUMENTAL_ISOLATOR_FILTER_ID "instrumental_isolator_filter"

/* Registers the "Instrumental Isolator" audio filter with libobs. */
void instrumental_isolator_register(void);

#ifdef __cplusplus
}
#endif
