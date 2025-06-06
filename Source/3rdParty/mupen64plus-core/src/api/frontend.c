/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-core - api/frontend.c                                     *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2012 CasualJames                                        *
 *   Copyright (C) 2009 Richard Goedeken                                   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* This file contains the Core front-end functions which will be exported
 * outside of the core library.
 */

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <md5.h>

#define M64P_CORE_PROTOTYPES 1
#include "callbacks.h"
#include "config.h"
#include "m64p_config.h"
#include "m64p_frontend.h"
#include "m64p_types.h"
#include "main/cheat.h"
#include "main/eventloop.h"
#include "main/main.h"
#include "main/rom.h"
#include "main/savestates.h"
#include "main/util.h"
#include "main/version.h"
#include "main/workqueue.h"
#include "main/screenshot.h"
#include "main/netplay.h"
#include "plugin/plugin.h"
#include "vidext.h"

/* some local state variables */
static int l_CoreInit = 0;
static int l_ROMOpen = 0;
static int l_DiskOpen = 0;
static int l_CallerUsingSDL = 0;

/* functions exported outside of libmupen64plus to front-end application */
EXPORT m64p_error CALL CoreStartup(int APIVersion, const char *ConfigPath, const char *DataPath, void *Context,
                                   void (*DebugCallback)(void *, int, const char *), void *Context2,
                                   void (*StateCallback)(void *, m64p_core_param, int))
{
    if (l_CoreInit)
        return M64ERR_ALREADY_INIT;

    /* check wether the caller has already initialized SDL */
    l_CallerUsingSDL = (SDL_WasInit(0) != 0);

    /* very first thing is to set the callback functions for debug info and state changing*/
    SetDebugCallback(DebugCallback, Context);
    SetStateCallback(StateCallback, Context2);

    /* check front-end's API version */
    if ((APIVersion & 0xffff0000) != (FRONTEND_API_VERSION & 0xffff0000))
    {
        DebugMessage(M64MSG_ERROR, "CoreStartup(): Front-end (API version %i.%i.%i) is incompatible with this core (API %i.%i.%i)",
                     VERSION_PRINTF_SPLIT(APIVersion), VERSION_PRINTF_SPLIT(FRONTEND_API_VERSION));
        return M64ERR_INCOMPATIBLE;
    }

    /* set up the default (dummy) plugins */
    plugin_connect(M64PLUGIN_GFX, NULL);
    plugin_connect(M64PLUGIN_AUDIO, NULL);
    plugin_connect(M64PLUGIN_INPUT, NULL);
    plugin_connect(M64PLUGIN_EXECUTION, NULL);
    plugin_connect(M64PLUGIN_CORE, NULL);

    savestates_init();

    /* next, start up the configuration handling code by loading and parsing the config file */
    if (ConfigInit(ConfigPath, DataPath) != M64ERR_SUCCESS)
        return M64ERR_INTERNAL;

    /* set default configuration parameter values for Core */
    if (ConfigOpenSection("Core", &g_CoreConfig) != M64ERR_SUCCESS || g_CoreConfig == NULL)
        return M64ERR_INTERNAL;

    if (!main_set_core_defaults())
        return M64ERR_INTERNAL;

    /* allocate base memory */
    g_mem_base = init_mem_base();
    if (g_mem_base == NULL) {
        return M64ERR_NO_MEMORY;
    }

    /* The ROM database contains MD5 hashes, goodnames, and some game-specific parameters */
    romdatabase_open();

    workqueue_init();

    l_CoreInit = 1;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL CoreShutdown(void)
{
    if (!l_CoreInit)
        return M64ERR_NOT_INIT;

    /* close down some core sub-systems */
    romdatabase_close();
    ConfigShutdown();
    workqueue_shutdown();
    savestates_deinit();

    /* if the calling code is using SDL, don't shut it down */
    if (!l_CallerUsingSDL)
        SDL_Quit();

    /* deallocate base memory */
    release_mem_base(g_mem_base);
    g_mem_base = NULL;

    /* deallocate rom memory */
    release_mem_rom();

    l_CoreInit = 0;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL CoreAttachPlugin(m64p_plugin_type PluginType, m64p_dynlib_handle PluginLibHandle)
{
    m64p_error rval;

    if (!l_CoreInit)
        return M64ERR_NOT_INIT;
    if (g_EmulatorRunning || (!l_ROMOpen && !l_DiskOpen))
        return M64ERR_INVALID_STATE;

    rval = plugin_connect(PluginType, PluginLibHandle);
    if (rval != M64ERR_SUCCESS)
        return rval;

    rval = plugin_start(PluginType);
    if (rval != M64ERR_SUCCESS)
        return rval;

    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL CoreDetachPlugin(m64p_plugin_type PluginType)
{
    if (!l_CoreInit)
        return M64ERR_NOT_INIT;
    if (g_EmulatorRunning)
        return M64ERR_INVALID_STATE;

    return plugin_connect(PluginType, NULL);
}

EXPORT m64p_error CALL CoreDoCommand(m64p_command Command, int ParamInt, void *ParamPtr)
{
    m64p_error rval;
    int keysym, keymod;

    if (!l_CoreInit)
        return M64ERR_NOT_INIT;

    switch(Command)
    {
        case M64CMD_NOP:
            return M64ERR_SUCCESS;
        case M64CMD_ROM_OPEN:
            if (g_EmulatorRunning || l_DiskOpen || l_ROMOpen)
                return M64ERR_INVALID_STATE;
            // ROM buffer size must be divisible by 4 to avoid out-of-bounds read in swap_copy_rom (v64/n64 formats)
            if (ParamPtr == NULL || ParamInt < 4096 || ParamInt > CART_ROM_MAX_SIZE)
                return M64ERR_INPUT_ASSERT;
            rval = open_rom((const unsigned char *) ParamPtr, ParamInt);
            if (rval == M64ERR_SUCCESS)
            {
                l_ROMOpen = 1;
                ScreenshotRomOpen();
                cheat_init(&g_cheat_ctx);
            }
            return rval;
        case M64CMD_ROM_CLOSE:
            if (g_EmulatorRunning || !l_ROMOpen)
                return M64ERR_INVALID_STATE;
            l_ROMOpen = 0;
            cheat_delete_all(&g_cheat_ctx);
            cheat_uninit(&g_cheat_ctx);
            return close_rom();
        case M64CMD_DISK_OPEN:
            if (g_EmulatorRunning || l_DiskOpen || l_ROMOpen)
                return M64ERR_INVALID_STATE;
            if (ParamPtr != NULL)
                return M64ERR_INPUT_INVALID;
            rval = open_disk();
            if (rval == M64ERR_SUCCESS)
            {
                l_DiskOpen = 1;
                ScreenshotRomOpen();
                cheat_init(&g_cheat_ctx);
            }
            return rval;
        case M64CMD_DISK_CLOSE:
            if (g_EmulatorRunning || !l_DiskOpen)
                return M64ERR_INVALID_STATE;
            l_DiskOpen = 0;
            cheat_delete_all(&g_cheat_ctx);
            cheat_uninit(&g_cheat_ctx);
            return close_disk();
        case M64CMD_PIF_OPEN:
            if (g_EmulatorRunning)
                return M64ERR_INVALID_STATE;
            if (ParamPtr == NULL || ParamInt < 1984 || ParamInt > 2048 || ParamInt % 4 != 0)
                return M64ERR_INPUT_ASSERT;
            return open_pif((const unsigned char *) ParamPtr, ParamInt);
        case M64CMD_ROM_GET_HEADER:
            if (!l_ROMOpen && !l_DiskOpen)
                return M64ERR_INVALID_STATE;
            if (ParamPtr == NULL)
                return M64ERR_INPUT_ASSERT;
            if ((int)sizeof(m64p_rom_header) < ParamInt)
                ParamInt = sizeof(m64p_rom_header);
            memcpy(ParamPtr, &ROM_HEADER, ParamInt);
            // Mupen64Plus used to keep a m64p_rom_header with a clean ROM name
            // Keep returning a clean ROM name for backwards compatibility
            if (ParamInt >= 0x20)
            {
                int size = (ParamInt >= 0x20 + 20) ? 20 : (ParamInt - 0x20);
                memcpy((char *)ParamPtr + 0x20, ROM_PARAMS.headername, size);
            }
            return M64ERR_SUCCESS;
        case M64CMD_ROM_GET_SETTINGS:
            if (!l_ROMOpen && !l_DiskOpen)
                return M64ERR_INVALID_STATE;
            if (ParamPtr == NULL)
                return M64ERR_INPUT_ASSERT;
            if ((int)sizeof(m64p_rom_settings) < ParamInt)
                ParamInt = sizeof(m64p_rom_settings);
            memcpy(ParamPtr, &ROM_SETTINGS, ParamInt);
            return M64ERR_SUCCESS;
        case M64CMD_ROM_SET_SETTINGS:
            if (g_EmulatorRunning || (!l_ROMOpen && !l_DiskOpen))
                return M64ERR_INVALID_STATE;
            if (ParamPtr == NULL)
                return M64ERR_INPUT_ASSERT;
            if ((int)sizeof(m64p_rom_settings) < ParamInt)
                ParamInt = sizeof(m64p_rom_settings);
            memcpy(&ROM_SETTINGS, ParamPtr, ParamInt);
            return M64ERR_SUCCESS;
        case M64CMD_EXECUTE:
            if (g_EmulatorRunning || (!l_ROMOpen && !l_DiskOpen))
                return M64ERR_INVALID_STATE;
            /* print out plugin-related warning messages */
            plugin_check();
            /* the main_run() function will not return until the player has quit the game */
            rval = main_run();
            return rval;
        case M64CMD_STOP:
            if (!g_EmulatorRunning)
                return M64ERR_INVALID_STATE;
            /* this stop function is asynchronous.  The emulator may not terminate until later */
            return main_core_state_set(M64CORE_EMU_STATE, M64EMU_STOPPED);
        case M64CMD_PAUSE:
            if (!g_EmulatorRunning)
                return M64ERR_INVALID_STATE;
            return main_core_state_set(M64CORE_EMU_STATE, M64EMU_PAUSED);
        case M64CMD_RESUME:
            if (!g_EmulatorRunning)
                return M64ERR_INVALID_STATE;
            return main_core_state_set(M64CORE_EMU_STATE, M64EMU_RUNNING);
        case M64CMD_CORE_STATE_QUERY:
            if (ParamPtr == NULL)
                return M64ERR_INPUT_ASSERT;
            return main_core_state_query((m64p_core_param) ParamInt, (int *) ParamPtr);
        case M64CMD_CORE_STATE_SET:
            if (ParamPtr == NULL)
                return M64ERR_INPUT_ASSERT;
            return main_core_state_set((m64p_core_param) ParamInt, *((int *)ParamPtr));
        case M64CMD_STATE_LOAD:
            main_state_load((char *) ParamPtr);
            return M64ERR_SUCCESS;
        case M64CMD_STATE_SAVE:
            if (!g_EmulatorRunning)
                return M64ERR_INVALID_STATE;
            if (ParamPtr != NULL && (ParamInt < 1 || ParamInt > 3))
                return M64ERR_INPUT_INVALID;
            main_state_save(ParamInt, (char *) ParamPtr);
            return M64ERR_SUCCESS;
        case M64CMD_STATE_SET_SLOT:
            if (ParamInt < 0 || ParamInt > 9)
                return M64ERR_INPUT_INVALID;
            return main_core_state_set(M64CORE_SAVESTATE_SLOT, ParamInt);
        case M64CMD_SEND_SDL_KEYDOWN:
            if (!g_EmulatorRunning)
                return M64ERR_INVALID_STATE;
            keysym = ParamInt & 0xffff;
            keymod = (ParamInt >> 16) & 0xffff;
            event_sdl_keydown(keysym, keymod);
            return M64ERR_SUCCESS;
        case M64CMD_SEND_SDL_KEYUP:
            if (!g_EmulatorRunning)
                return M64ERR_INVALID_STATE;
            keysym = ParamInt & 0xffff;
            keymod = (ParamInt >> 16) & 0xffff;
            event_sdl_keyup(keysym, keymod);
            return M64ERR_SUCCESS;
        case M64CMD_SET_FRAME_CALLBACK:
            *(void**)&g_FrameCallback = ParamPtr;
            return M64ERR_SUCCESS;
        case M64CMD_TAKE_NEXT_SCREENSHOT:
            if (!g_EmulatorRunning)
                return M64ERR_INVALID_STATE;
            main_take_next_screenshot();
            return M64ERR_SUCCESS;
        case M64CMD_READ_SCREEN:
            if (!g_EmulatorRunning)
                return M64ERR_INVALID_STATE;
            if (ParamPtr == NULL)
                return M64ERR_INPUT_ASSERT;
            if (ParamInt < 0 || ParamInt > 1)
                return M64ERR_INPUT_INVALID;
            return main_read_screen(ParamPtr, ParamInt);
        case M64CMD_RESET:
            if (!g_EmulatorRunning)
                return M64ERR_INVALID_STATE;
            if (ParamInt < 0 || ParamInt > 1)
                return M64ERR_INPUT_INVALID;
            return main_reset(ParamInt);
        case M64CMD_ADVANCE_FRAME:
            if (!g_EmulatorRunning)
                return M64ERR_INVALID_STATE;
            main_advance_one();
            return M64ERR_SUCCESS;
        case M64CMD_SET_MEDIA_LOADER:
            if (ParamInt != sizeof(m64p_media_loader) || ParamPtr == NULL)
                return M64ERR_INPUT_INVALID;
            g_media_loader = *(m64p_media_loader*)ParamPtr;
            return M64ERR_SUCCESS;
        case M64CMD_NETPLAY_INIT:
            if (ParamInt < 1 || ParamPtr == NULL)
                return M64ERR_INPUT_INVALID;
            return netplay_start(ParamPtr, ParamInt);
        case M64CMD_NETPLAY_CONTROL_PLAYER:
            if (ParamInt < 1 || ParamInt > 4 || ParamPtr == NULL)
                return M64ERR_INPUT_INVALID;
            if (netplay_register_player(ParamInt - 1, Controls[netplay_next_controller()].Plugin, Controls[netplay_next_controller()].RawData, *(uint32_t*)ParamPtr))
            {
                netplay_set_controller(ParamInt - 1);
                return M64ERR_SUCCESS;
            }
            else
                return M64ERR_INPUT_ASSERT; // player already in use
        case M64CMD_NETPLAY_GET_VERSION:
            if (ParamPtr == NULL)
                return M64ERR_INPUT_INVALID;
            *(uint32_t*)ParamPtr = NETPLAY_CORE_VERSION;
            if (ParamInt == NETPLAY_API_VERSION)
                return M64ERR_SUCCESS;
            else
                return M64ERR_INCOMPATIBLE;
        case M64CMD_NETPLAY_CLOSE:
            return netplay_stop();
        default:
            return M64ERR_INPUT_INVALID;
    }

    return M64ERR_INTERNAL;
}

EXPORT m64p_error CALL CoreOverrideVidExt(m64p_video_extension_functions *VideoFunctionStruct)
{
    if (!l_CoreInit)
        return M64ERR_NOT_INIT;

    return OverrideVideoFunctions(VideoFunctionStruct); /* in vidext.c */
}

EXPORT m64p_error CALL CoreAddCheat(const char *CheatName, m64p_cheat_code *CodeList, int NumCodes)
{
    if (!l_CoreInit)
        return M64ERR_NOT_INIT;
    if (netplay_is_init())
        return M64ERR_INVALID_STATE;
    if (CheatName == NULL || CodeList == NULL)
        return M64ERR_INPUT_ASSERT;
    if (strlen(CheatName) < 1 || NumCodes < 1)
        return M64ERR_INPUT_INVALID;

    if (cheat_add_new(&g_cheat_ctx, CheatName, CodeList, NumCodes))
        return M64ERR_SUCCESS;

    return M64ERR_INPUT_INVALID;
}

EXPORT m64p_error CALL CoreCheatEnabled(const char *CheatName, int Enabled)
{
    if (!l_CoreInit)
        return M64ERR_NOT_INIT;
    if (netplay_is_init())
        return M64ERR_INVALID_STATE;
    if (CheatName == NULL)
        return M64ERR_INPUT_ASSERT;

    if (cheat_set_enabled(&g_cheat_ctx, CheatName, Enabled))
        return M64ERR_SUCCESS;

    return M64ERR_INPUT_INVALID;
}

EXPORT m64p_error CALL CoreGetRomSettings(m64p_rom_settings *RomSettings, int RomSettingsLength, int Crc1, int Crc2)
{
    romdatabase_entry* entry;
    int i;

    if (!l_CoreInit)
        return M64ERR_NOT_INIT;
    if (RomSettings == NULL)
        return M64ERR_INPUT_ASSERT;
    if (RomSettingsLength < (int)sizeof(m64p_rom_settings))
        return M64ERR_INPUT_INVALID;

    /* Look up this ROM in the .ini file and fill in goodname, etc */
    entry = ini_search_by_crc(Crc1, Crc2);
    if (entry == NULL)
        return M64ERR_INPUT_NOT_FOUND;

    strncpy(RomSettings->goodname, entry->goodname, 255);
    RomSettings->goodname[255] = '\0';
    for (i = 0; i < 16; i++)
        sprintf(RomSettings->MD5 + i*2, "%02X", entry->md5[i]);
    RomSettings->MD5[32] = '\0';
    RomSettings->savetype = entry->savetype;
    RomSettings->status = entry->status;
    RomSettings->players = entry->players;
    RomSettings->rumble = entry->rumble;
    RomSettings->transferpak = entry->transferpak;
    RomSettings->mempak = entry->mempak;
    RomSettings->disableextramem = entry->disableextramem;
    RomSettings->countperop = entry->countperop;
    RomSettings->savetype = entry->savetype;
    RomSettings->sidmaduration = entry->sidmaduration;
    RomSettings->aidmamodifier = entry->aidmamodifier;

    return M64ERR_SUCCESS;
}


