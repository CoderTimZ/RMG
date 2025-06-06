/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_VOLUME_HPP
#define CORE_VOLUME_HPP

// attempts to retrieve the current volume,
// returns -1 on error
int CoreGetVolume(void);

// attempts to set the volume
bool CoreSetVolume(int value);

// attempts to increase the audio volume
bool CoreIncreaseVolume(void);

// attempts to decrease the audio volume
bool CoreDecreaseVolume(void);

// attempts to toggle whether the audio is muted
bool CoreToggleMuteVolume(void);

#endif // CORE_VOLUME_HPP