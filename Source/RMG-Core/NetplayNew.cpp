/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define CORE_INTERNAL
#include "NetplayNew.hpp"
#include "Library.hpp"
#include "Plugins.hpp"
#include "Error.hpp"

#include "m64p/Api.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

//
// Local Variables
//

static bool l_HasCallbacks = false;
static m64p_netplay_new_callbacks l_Callbacks;

//
// Exported Functions
//

CORE_EXPORT bool CoreSetNetplayNewCallbacks(const m64p_netplay_new_callbacks* callbacks)
{
    if (callbacks == nullptr)
    {
        l_HasCallbacks = false;
        std::memset(&l_Callbacks, 0, sizeof(l_Callbacks));
        return true;
    }

    if (callbacks->api_version != M64P_NETPLAY_NEW_API_VERSION)
    {
        CoreSetError("CoreSetNetplayNewCallbacks Failed: incompatible API version");
        return false;
    }

    l_Callbacks    = *callbacks;
    l_HasCallbacks = true;
    return true;
}

CORE_EXPORT bool CoreHasNetplayNew(void)
{
    return l_HasCallbacks;
}

CORE_EXPORT std::string CoreGetNetplayNewRollbackProblem(std::string rspPlugin, std::string gfxPlugin)
{
    // (GLideN64 copies nothing to RDRAM in a game of the new netplay, and
    // Parallel doesn't read superscaled pixels back: the core tells them,
    // see NetplayNewGame)

    // A rollback restores the RDP's settings and TMEM, which a video plugin
    // keeps outside RDRAM, only if the plugin hands them over (angrylion
    // doesn't). Without them, a frame run again doesn't draw what it drew
    // the first time.
    if (!gfxPlugin.empty() &&
        !CorePluginHasFunctions(CorePluginType::Gfx, gfxPlugin, {"RdpStateSize", "SaveRdpState", "LoadRdpState"}))
    {
        return "video plugin can't save the RDP's state for rollback";
    }

    // The same for an LLE RSP plugin's registers, which carry over from one
    // task to the next. The HLE RSP plugin keeps none that matter (its
    // re-run frames are exact without them).
    std::string rsp = rspPlugin;
    std::transform(rsp.begin(), rsp.end(), rsp.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!rspPlugin.empty() && rsp.find("rsp-hle") == std::string::npos &&
        !CorePluginHasFunctions(CorePluginType::Rsp, rspPlugin, {"RspStateSize", "SaveRspState", "LoadRspState"}))
    {
        return "RSP plugin can't save its state for rollback";
    }

    return std::string();
}

CORE_EXPORT bool CoreNetplayNewExecutionPluginSavesState(std::string executionPlugin)
{
    // (mupen64plus-core's plugin.c takes all four or none)
    return CorePluginHasFunctions(CorePluginType::Execution, executionPlugin,
                                  {"NetplayStateSize", "NetplaySaveState", "NetplayLoadState", "NetplayDropState"});
}

//
// Internal Functions
//

bool CoreAttachNetplayNew(void)
{
    std::string error;
    m64p_error  ret;

    if (!l_HasCallbacks)
    {
        return true;
    }

    ret = m64p::Core.DoCommand(M64CMD_NETPLAY_NEW_SET_CALLBACKS, sizeof(l_Callbacks), &l_Callbacks);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreAttachNetplayNew m64p::Core.DoCommand(M64CMD_NETPLAY_NEW_SET_CALLBACKS) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
        return false;
    }

    return true;
}

void CoreDetachNetplayNew(void)
{
    if (!m64p::Core.IsHooked())
    {
        return;
    }

    m64p::Core.DoCommand(M64CMD_NETPLAY_NEW_SET_CALLBACKS, 0, nullptr);
}
