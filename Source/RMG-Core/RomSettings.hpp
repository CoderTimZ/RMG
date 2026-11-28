/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_ROMSETTINGS_HPP
#define CORE_ROMSETTINGS_HPP

#include <cstdint>
#include <string>

#include "RomHeader.hpp"

struct CoreRomSettings
{
    // rom goodname
    std::string GoodName;
    // rom internal name
    std::string InternalName;
    // rom MD5 string
    std::string MD5;
    // rom save type
    uint16_t SaveType;
    // whether the rom has the 4MB expansion RAM pak disabled
    bool DisableExtraMem;
    // whether the rom supports the transfer pak
    bool TransferPak;
    // Number of CPU cycles per instruction
    int32_t CountPerOp;
    // SI DMA duration
    int32_t SiDMADuration;

    bool operator==(const CoreRomSettings& other) const
    {
        return GoodName == other.GoodName &&
                InternalName == other.InternalName &&
                MD5 == other.MD5 &&
                SaveType == other.SaveType &&
                DisableExtraMem == other.DisableExtraMem &&
                TransferPak == other.TransferPak &&
                CountPerOp == other.CountPerOp &&
                SiDMADuration == other.SiDMADuration;
    }
};

// retrieves the currently opened ROM settings
bool CoreGetCurrentRomSettings(CoreRomSettings& settings);

// stores the currently opened ROM settings as default settings
bool CoreStoreCurrentDefaultRomSettings(void);

// clears the currently opened ROM default settings
bool CoreClearCurrentDefaultRomSettings(void);

// retrieves the currently opened ROM default settings
bool CoreGetCurrentDefaultRomSettings(CoreRomSettings& settings);

// applies the ROM settings to the currently opened ROM
bool CoreApplyRomSettings(CoreRomSettings settings);

// applies the ROM settings settings if they exist
bool CoreApplyRomSettingsOverlay(void);

// returns the section that holds the ROM's game settings: its internal name
// or its MD5, following the save filename format setting
std::string CoreGetRomSettingsSection(const CoreRomSettings& settings);

// returns the section that holds the ROM's crop override: one for its game ID, which
// ROM hacks of the game share, or its game settings' section when it has none
std::string CoreGetRomCropSettingsSection(const CoreRomHeader& header, const CoreRomSettings& settings);

#endif // CORE_ROMSETTINGS_HPP