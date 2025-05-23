/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define CORE_INTERNAL
#include "ConvertStringEncoding.hpp"
#include "RomSettings.hpp"
#include "Settings.hpp"
#include "Library.hpp"
#include "Error.hpp"
#include "Rom.hpp"

#include "m64p/Api.hpp"

//
// Local Variables
//

static CoreRomSettings l_DefaultRomSettings;
static bool            l_HasDefaultRomSettings = false;

//
// Exported Functions
//

CORE_EXPORT bool CoreGetCurrentRomSettings(CoreRomSettings& settings)
{
    std::string       error;
    m64p_error        ret;
    m64p_rom_settings m64p_settings;
    m64p_rom_header   m64p_header;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_ROM_GET_SETTINGS, sizeof(m64p_rom_settings), &m64p_settings);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreGetCurrentRomSettings m64p::Core.DoCommand(M64CMD_ROM_GET_SETTINGS) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_ROM_GET_HEADER, sizeof(m64p_header), &m64p_header);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreGetCurrentRomSettings m64p::Core.DoCommand(M64CMD_ROM_GET_HEADER) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
        return false;
    }

    settings.GoodName = CoreConvertStringEncoding(m64p_settings.goodname, CoreStringEncoding::Shift_JIS);
    settings.InternalName = CoreConvertStringEncoding((char*)m64p_header.Name, CoreStringEncoding::Shift_JIS);
    settings.MD5 = std::string(m64p_settings.MD5);
    settings.SaveType = m64p_settings.savetype;
    settings.DisableExtraMem = m64p_settings.disableextramem;
    settings.TransferPak = m64p_settings.transferpak;
    settings.CountPerOp = m64p_settings.countperop;
    settings.SiDMADuration = m64p_settings.sidmaduration;
    return true;
}

CORE_EXPORT bool CoreStoreCurrentDefaultRomSettings(void)
{
    CoreRomSettings settings;

    if (!CoreGetCurrentRomSettings(settings))
    {
        return false;
    }

    l_DefaultRomSettings = settings;
    l_HasDefaultRomSettings = true;
    return true;
}

CORE_EXPORT bool CoreClearCurrentDefaultRomSettings(void)
{
    l_HasDefaultRomSettings = false;
    return true;
}

CORE_EXPORT bool CoreGetCurrentDefaultRomSettings(CoreRomSettings& settings)
{
    std::string error;

    if (!l_HasDefaultRomSettings)
    {
        error = "CoreGetCurrentDefaultRomSettings Failed: ";
        error += "cannot retreive default ROM settings when no defaults have been stored!";
        CoreSetError(error);
        return false;
    }

    settings = l_DefaultRomSettings;
    return true;
}

CORE_EXPORT bool CoreApplyRomSettings(CoreRomSettings settings)
{
    std::string       error;
    m64p_error        ret;
    m64p_rom_settings m64p_settings;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    if (!CoreHasRomOpen())
    {
        error = "CoreApplyRomSettings Failed: ";
        error += "cannot apply rom settings when no ROM has been opened!";
        CoreSetError(error);
        return false;
    }

    // retrieve ROM settings
    ret = m64p::Core.DoCommand(M64CMD_ROM_GET_SETTINGS, sizeof(m64p_rom_settings), &m64p_settings);
    if (ret != M64ERR_SUCCESS)
    {
        CoreCloseRom();
        error = "CoreApplyRomSettings m64p::Core.DoCommand(M64CMD_ROM_GET_SETTINGS) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
        return false;
    }

    // overide a few settings
    m64p_settings.savetype = settings.SaveType;
    m64p_settings.disableextramem = settings.DisableExtraMem;
    m64p_settings.transferpak = settings.TransferPak;
    m64p_settings.countperop = settings.CountPerOp;
    m64p_settings.sidmaduration = settings.SiDMADuration;

    // apply ROM settings
    ret = m64p::Core.DoCommand(M64CMD_ROM_SET_SETTINGS, sizeof(m64p_rom_settings), &m64p_settings);
    if (ret != M64ERR_SUCCESS)
    {
        CoreCloseRom();
        error = "CoreApplyRomSettings m64p::Core.DoCommand(M64CMD_ROM_SET_SETTINGS) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreApplyRomSettingsOverlay(void)
{
    CoreRomSettings settings;

    if (!CoreGetCurrentDefaultRomSettings(settings))
    {
        return false;
    }

    std::string section;
    int format = CoreSettingsGetIntValue(SettingsID::Core_SaveFileNameFormat);
    if (format == 0) {
        section = settings.InternalName;
    } else {
        section = settings.MD5;
    }

    // don't do anything when section doesn't exist
    if (!CoreSettingsSectionExists(section))
    {
        return false;
    }

    // or when we don't override the settings
    if (!CoreSettingsGetBoolValue(SettingsID::Game_OverrideSettings, section))
    {
        return false;
    }

    settings.SaveType = CoreSettingsGetIntValue(SettingsID::Game_SaveType, section);
    settings.DisableExtraMem = CoreSettingsGetBoolValue(SettingsID::Game_DisableExtraMem, section);
    settings.TransferPak = CoreSettingsGetBoolValue(SettingsID::Game_TransferPak, section);
    settings.CountPerOp = CoreSettingsGetIntValue(SettingsID::Game_CountPerOp, section);
    settings.SiDMADuration = CoreSettingsGetIntValue(SettingsID::Game_SiDmaDuration, section);

    return CoreApplyRomSettings(settings);
}
