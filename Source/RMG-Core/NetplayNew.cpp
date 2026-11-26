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
#include "Error.hpp"

#include "Settings.hpp"
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

CORE_EXPORT std::string CoreGetNetplayNewRollbackProblem(std::string gfxPlugin)
{
    std::transform(gfxPlugin.begin(), gfxPlugin.end(), gfxPlugin.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // GLideN64 copies its frame buffers to RDRAM one or two frames late in
    // its double and triple buffer modes (2, 3). A rollback restores RDRAM
    // but can't take back a copy that's still on its way, which would then
    // land in the corrected game.
    if (gfxPlugin.find("glide") != std::string::npos &&
        CoreSettingsGetBoolValue("Video-GLideN64", "EnableFBEmulation", true) &&
        CoreSettingsGetIntValue("Video-GLideN64", "EnableCopyColorToRDRAM", 1) >= 2)
    {
        return "GLideN64 copies frame buffers to RDRAM asynchronously (set \"Copy color buffer to RDRAM\" to synchronous or off)";
    }

    // Parallel's upscaling keeps an upscaled copy of RDRAM on the GPU, which
    // rollback doesn't save. That's fine while the game only reads native
    // RDRAM (at worst a frame after a rollback shows upscaled pixels from the
    // abandoned frames), but superscaled reads give the game pixels made
    // from that copy.
    if (gfxPlugin.find("parallel") != std::string::npos &&
        CoreSettingsGetIntValue("Video-Parallel", "Upscaling", 1) > 1 &&
        CoreSettingsGetBoolValue("Video-Parallel", "SuperscaledReads", false))
    {
        return "Parallel's superscaled reads are on (turn them off, or set Upscaling to 1x, native)";
    }

    return std::string();
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
