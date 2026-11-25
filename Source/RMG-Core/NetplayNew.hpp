/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_NETPLAYNEW_HPP
#define CORE_NETPLAYNEW_HPP

#include "m64p/api/m64p_types.h"

// attaches a new netplay session to the games that
// CoreStartEmulation starts from now on; nullptr detaches it.
// The callbacks are handed to the core when a game starts and
// taken back when it ends.
bool CoreSetNetplayNewCallbacks(const m64p_netplay_new_callbacks* callbacks);

// returns whether a new netplay session is attached
bool CoreHasNetplayNew(void);

#ifdef CORE_INTERNAL
// hands the callbacks to the core, right before a game starts
bool CoreAttachNetplayNew(void);

// takes the callbacks back from the core, after the game ended
void CoreDetachNetplayNew(void);
#endif // CORE_INTERNAL

#endif // CORE_NETPLAYNEW_HPP
