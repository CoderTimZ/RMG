/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - plugin.c                                                *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2002 Hacktarux                                          *
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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "api/callbacks.h"
#include "api/m64p_common.h"
#include "api/m64p_plugin.h"
#include "api/m64p_types.h"
#include "device/memory/memory.h"
#include "device/rcp/ai/ai_controller.h"
#include "device/rcp/mi/mi_controller.h"
#include "device/rcp/rdp/rdp_core.h"
#include "device/rcp/rsp/rsp_core.h"
#include "device/rcp/vi/vi_controller.h"
#include "dummy_audio.h"
#include "dummy_input.h"
#include "dummy_rsp.h"
#include "dummy_video.h"
#include "dummy_execution.h"
#include "main/main.h"
#include "main/netplay_new.h"
#include "main/rom.h"
#include "main/version.h"
#include "osal/dynamiclib.h"
#include "plugin.h"

CONTROL Controls[4];

/* global function pointers - initialized on core startup */
gfx_plugin_functions gfx;
audio_plugin_functions audio;
input_plugin_functions input;
rsp_plugin_functions rsp;
execution_plugin_functions execution;

uint8_t execution_hooks[EXECUTION_HOOK_MAPS][EXECUTION_HOOK_WORDS / 8];
uint8_t execution_hook_pages[EXECUTION_HOOK_MAPS][EXECUTION_HOOK_PAGES / 8];
int execution_branch_cancelled = 0;
unsigned int execution_sync_exceptions = 0;
int execution_reload_pending = 0;

/* local data structures and functions */
static const gfx_plugin_functions dummy_gfx = {
    dummyvideo_PluginGetVersion,
    dummyvideo_ChangeWindow,
    dummyvideo_InitiateGFX,
    dummyvideo_MoveScreen,
    dummyvideo_ProcessDList,
    dummyvideo_ProcessRDPList,
    dummyvideo_RomClosed,
    dummyvideo_RomOpen,
    dummyvideo_ShowCFB,
    dummyvideo_UpdateScreen,
    dummyvideo_ViStatusChanged,
    dummyvideo_ViWidthChanged,
    dummyvideo_ReadScreen2,
    dummyvideo_SetRenderingCallback,
    dummyvideo_ResizeVideoOutput,
    dummyvideo_FBRead,
    dummyvideo_FBWrite,
    dummyvideo_FBGetFrameBufferInfo
};

static const audio_plugin_functions dummy_audio = {
    dummyaudio_PluginGetVersion,
    dummyaudio_AiDacrateChanged,
    dummyaudio_AiLenChanged,
    dummyaudio_InitiateAudio,
    dummyaudio_ProcessAList,
    dummyaudio_RomClosed,
    dummyaudio_RomOpen,
    dummyaudio_SetSpeedFactor,
    dummyaudio_VolumeUp,
    dummyaudio_VolumeDown,
    dummyaudio_VolumeGetLevel,
    dummyaudio_VolumeSetLevel,
    dummyaudio_VolumeMute,
    dummyaudio_VolumeGetString
};

static const input_plugin_functions dummy_input = {
    dummyinput_PluginGetVersion,
    dummyinput_ControllerCommand,
    dummyinput_GetKeys,
    dummyinput_InitiateControllers,
    dummyinput_ReadController,
    dummyinput_RomClosed,
    dummyinput_RomOpen,
    dummyinput_SDL_KeyDown,
    dummyinput_SDL_KeyUp,
    dummyinput_RenderCallback,
    dummyinput_SendVRUWord,
    dummyinput_SetMicState,
    dummyinput_ReadVRUResults,
    dummyinput_ClearVRUWords,
    dummyinput_SetVRUWordMask
};

static const rsp_plugin_functions dummy_rsp = {
    dummyrsp_PluginGetVersion,
    dummyrsp_DoRspCycles,
    dummyrsp_InitiateRSP,
    dummyrsp_RomClosed
};

static const execution_plugin_functions dummy_execution = {
    dummyexecution_PluginGetVersion,
    dummyexecution_RomOpen,
    dummyexecution_RomClosed,
    dummyexecution_InitiateExecution,
    dummyexecution_Input,
    dummyexecution_Frame,
    dummyexecution_Execute,
    dummyexecution_ExecuteDone,
    dummyexecution_Read8,
    dummyexecution_Read16,
    dummyexecution_Read32,
    dummyexecution_Read64,
    dummyexecution_Write8,
    dummyexecution_Write16,
    dummyexecution_Write32,
    dummyexecution_Write64,
    dummyexecution_StateLoaded,
    dummyexecution_Reset
};

static GFX_INFO gfx_info;
static AUDIO_INFO audio_info;
static CONTROL_INFO control_info;
static RSP_INFO rsp_info;
static EXECUTION_INFO execution_info;

static int l_RspAttached = 0;
static int l_InputAttached = 0;
static int l_AudioAttached = 0;
static int l_GfxAttached = 0;
static int l_ExecutionAttached = 0;

static unsigned int dummy;

/* local functions */
static void EmptyFunc(void)
{
}

/* An LLE RSP plugin sends the RDP its commands through this, not through the
 * DPC registers, so it's where a full sync is seen as soon as it's done. */
static void rsp_process_rdp_list(void)
{
    uint32_t dp_bit_set = g_dev.mi.regs[MI_INTR_REG] & MI_INTR_DP;

    netplay_new_rdp_list();
    gfx.processRDPList();

    if ((g_dev.mi.regs[MI_INTR_REG] & MI_INTR_DP) && !dp_bit_set)
    {
        netplay_new_rdp_full_sync();
    }
}

// Handy macro to avoid code bloat when loading symbols
#define GET_FUNC(type, field, name) \
    ((field = (type)osal_dynlib_getproc(plugin_handle, name)) != NULL)

// code to handle backwards-compatibility to video plugins with API_VERSION < 02.1.0.  This API version introduced a boolean
// flag in the rendering callback, which told the core whether or not the current screen has been freshly redrawn since the
// last time the callback was called.
static void                     (*l_mainRenderCallback)(int) = NULL;
static ptr_SetRenderingCallback   l_old1SetRenderingCallback = NULL;

static void backcompat_videoRenderCallback(int unused)  // this function will be called by the video plugin as the render callback
{
    if (l_mainRenderCallback != NULL)
        l_mainRenderCallback(1);  // assume screen is always freshly redrawn (otherwise screenshots won't work w/ OSD enabled)
}

static void backcompat_setRenderCallbackIntercept(void (*callback)(int))
{
    l_mainRenderCallback = callback;
}

static void plugin_disconnect_gfx(void)
{
    gfx = dummy_gfx;
    l_GfxAttached = 0;
    l_mainRenderCallback = NULL;
}

static m64p_error plugin_connect_gfx(m64p_dynlib_handle plugin_handle)
{
    /* attach the Video plugin function pointers */
    if (plugin_handle != NULL)
    {
        m64p_plugin_type PluginType;
        int PluginVersion, APIVersion;

        if (l_GfxAttached)
            return M64ERR_INVALID_STATE;

        /* set function pointers for required functions */
        if (!GET_FUNC(ptr_PluginGetVersion, gfx.getVersion, "PluginGetVersion") ||
            !GET_FUNC(ptr_ChangeWindow, gfx.changeWindow, "ChangeWindow") ||
            !GET_FUNC(ptr_InitiateGFX, gfx.initiateGFX, "InitiateGFX") ||
            !GET_FUNC(ptr_MoveScreen, gfx.moveScreen, "MoveScreen") ||
            !GET_FUNC(ptr_ProcessDList, gfx.processDList, "ProcessDList") ||
            !GET_FUNC(ptr_ProcessRDPList, gfx.processRDPList, "ProcessRDPList") ||
            !GET_FUNC(ptr_RomClosed, gfx.romClosed, "RomClosed") ||
            !GET_FUNC(ptr_RomOpen, gfx.romOpen, "RomOpen") ||
            !GET_FUNC(ptr_ShowCFB, gfx.showCFB, "ShowCFB") ||
            !GET_FUNC(ptr_UpdateScreen, gfx.updateScreen, "UpdateScreen") ||
            !GET_FUNC(ptr_ViStatusChanged, gfx.viStatusChanged, "ViStatusChanged") ||
            !GET_FUNC(ptr_ViWidthChanged, gfx.viWidthChanged, "ViWidthChanged") ||
            !GET_FUNC(ptr_ReadScreen2, gfx.readScreen, "ReadScreen2") ||
            !GET_FUNC(ptr_SetRenderingCallback, gfx.setRenderingCallback, "SetRenderingCallback") ||
            !GET_FUNC(ptr_FBRead, gfx.fBRead, "FBRead") ||
            !GET_FUNC(ptr_FBWrite, gfx.fBWrite, "FBWrite") ||
            !GET_FUNC(ptr_FBGetFrameBufferInfo, gfx.fBGetFrameBufferInfo, "FBGetFrameBufferInfo"))
        {
            DebugMessage(M64MSG_ERROR, "broken Video plugin; function(s) not found.");
            plugin_disconnect_gfx();
            return M64ERR_INPUT_INVALID;
        }

        /* set function pointers for optional functions */
        gfx.resizeVideoOutput = (ptr_ResizeVideoOutput)osal_dynlib_getproc(plugin_handle, "ResizeVideoOutput");

        /* check the version info */
        (*gfx.getVersion)(&PluginType, &PluginVersion, &APIVersion, NULL, NULL);
        if (PluginType != M64PLUGIN_GFX || (APIVersion & 0xffff0000) != (GFX_API_VERSION & 0xffff0000))
        {
            DebugMessage(M64MSG_ERROR, "incompatible Video plugin");
            plugin_disconnect_gfx();
            return M64ERR_INCOMPATIBLE;
        }

        /* handle backwards-compatibility */
        if (APIVersion < 0x020100)
        {
            DebugMessage(M64MSG_WARNING, "Fallback for Video plugin API (%02i.%02i.%02i) < 2.1.0. Screenshots may contain On Screen Display text", VERSION_PRINTF_SPLIT(APIVersion));
            // tell the video plugin to make its rendering callback to me (it's old, and doesn't have the bScreenRedrawn flag)
            gfx.setRenderingCallback(backcompat_videoRenderCallback);
            l_old1SetRenderingCallback = gfx.setRenderingCallback; // save this just for future use
            gfx.setRenderingCallback = (ptr_SetRenderingCallback) backcompat_setRenderCallbackIntercept;
        }
        if (APIVersion < 0x20200 || gfx.resizeVideoOutput == NULL)
        {
            DebugMessage(M64MSG_WARNING, "Fallback for Video plugin API (%02i.%02i.%02i) < 2.2.0. Resizable video will not work", VERSION_PRINTF_SPLIT(APIVersion));
            gfx.resizeVideoOutput = dummyvideo_ResizeVideoOutput;
        }

        l_GfxAttached = 1;
    }
    else
        plugin_disconnect_gfx();

    return M64ERR_SUCCESS;
}

static m64p_error plugin_start_gfx(void)
{
    uint8_t media = g_rom_size == 0 ? 0 : *((uint8_t*)mem_base_u32(g_mem_base, MM_CART_ROM) + (0x3b ^ S8));

    /* Here we feed 64DD IPL ROM header to GFX plugin if 64DD is present.
     * We use g_media_loader.get_dd_rom to detect 64DD presence
     * instead of g_dev because the latter is not yet initialized at plugin_start time */
    /* XXX: Not sure it is the best way to convey which game is being played to the GFX plugin
     * as 64DD IPL is the same for all 64DD games... */
    char* dd_ipl_rom_filename = (g_media_loader.get_dd_rom == NULL)
        ? NULL
        : g_media_loader.get_dd_rom(g_media_loader.cb_data);

    uint32_t rom_base = (g_rom_size == 0 || (dd_ipl_rom_filename != NULL && strlen(dd_ipl_rom_filename) != 0 && media != 'C'))
        ? MM_DD_ROM
        : MM_CART_ROM;

    free(dd_ipl_rom_filename);

    /* fill in the GFX_INFO data structure */
    gfx_info.HEADER = (unsigned char *)mem_base_u32(g_mem_base, rom_base);
    gfx_info.RDRAM = (unsigned char *)mem_base_u32(g_mem_base, MM_RDRAM_DRAM);
    gfx_info.DMEM = (unsigned char *)mem_base_u32(g_mem_base, MM_RSP_MEM);
    gfx_info.IMEM = (unsigned char *)mem_base_u32(g_mem_base, MM_RSP_MEM + 0x1000);
    gfx_info.MI_INTR_REG = &(g_dev.mi.regs[MI_INTR_REG]);
    gfx_info.DPC_START_REG = &(g_dev.dp.dpc_regs[DPC_START_REG]);
    gfx_info.DPC_END_REG = &(g_dev.dp.dpc_regs[DPC_END_REG]);
    gfx_info.DPC_CURRENT_REG = &(g_dev.dp.dpc_regs[DPC_CURRENT_REG]);
    gfx_info.DPC_STATUS_REG = &(g_dev.dp.dpc_regs[DPC_STATUS_REG]);
    gfx_info.DPC_CLOCK_REG = &(g_dev.dp.dpc_regs[DPC_CLOCK_REG]);
    gfx_info.DPC_BUFBUSY_REG = &(g_dev.dp.dpc_regs[DPC_BUFBUSY_REG]);
    gfx_info.DPC_PIPEBUSY_REG = &(g_dev.dp.dpc_regs[DPC_PIPEBUSY_REG]);
    gfx_info.DPC_TMEM_REG = &(g_dev.dp.dpc_regs[DPC_TMEM_REG]);
    gfx_info.VI_STATUS_REG = &(g_dev.vi.regs[VI_STATUS_REG]);
    gfx_info.VI_ORIGIN_REG = &(g_dev.vi.regs[VI_ORIGIN_REG]);
    gfx_info.VI_WIDTH_REG = &(g_dev.vi.regs[VI_WIDTH_REG]);
    gfx_info.VI_INTR_REG = &(g_dev.vi.regs[VI_V_INTR_REG]);
    gfx_info.VI_V_CURRENT_LINE_REG = &(g_dev.vi.regs[VI_CURRENT_REG]);
    gfx_info.VI_TIMING_REG = &(g_dev.vi.regs[VI_BURST_REG]);
    gfx_info.VI_V_SYNC_REG = &(g_dev.vi.regs[VI_V_SYNC_REG]);
    gfx_info.VI_H_SYNC_REG = &(g_dev.vi.regs[VI_H_SYNC_REG]);
    gfx_info.VI_LEAP_REG = &(g_dev.vi.regs[VI_LEAP_REG]);
    gfx_info.VI_H_START_REG = &(g_dev.vi.regs[VI_H_START_REG]);
    gfx_info.VI_V_START_REG = &(g_dev.vi.regs[VI_V_START_REG]);
    gfx_info.VI_V_BURST_REG = &(g_dev.vi.regs[VI_V_BURST_REG]);
    gfx_info.VI_X_SCALE_REG = &(g_dev.vi.regs[VI_X_SCALE_REG]);
    gfx_info.VI_Y_SCALE_REG = &(g_dev.vi.regs[VI_Y_SCALE_REG]);
    gfx_info.CheckInterrupts = EmptyFunc;

    gfx_info.version = 2; //Version 2 added SP_STATUS_REG and RDRAM_SIZE
    gfx_info.SP_STATUS_REG = &g_dev.sp.regs[SP_STATUS_REG];
    gfx_info.RDRAM_SIZE = (unsigned int*) &g_dev.rdram.dram_size;

    /* call the audio plugin */
    if (!gfx.initiateGFX(gfx_info))
        return M64ERR_PLUGIN_FAIL;

    return M64ERR_SUCCESS;
}

static void plugin_disconnect_audio(void)
{
    audio = dummy_audio;
    l_AudioAttached = 0;
}

static m64p_error plugin_connect_audio(m64p_dynlib_handle plugin_handle)
{
    /* attach the Audio plugin function pointers */
    if (plugin_handle != NULL)
    {
        m64p_plugin_type PluginType;
        int PluginVersion, APIVersion;

        if (l_AudioAttached)
            return M64ERR_INVALID_STATE;

        if (!GET_FUNC(ptr_PluginGetVersion, audio.getVersion, "PluginGetVersion") ||
            !GET_FUNC(ptr_AiDacrateChanged, audio.aiDacrateChanged, "AiDacrateChanged") ||
            !GET_FUNC(ptr_AiLenChanged, audio.aiLenChanged, "AiLenChanged") ||
            !GET_FUNC(ptr_InitiateAudio, audio.initiateAudio, "InitiateAudio") ||
            !GET_FUNC(ptr_ProcessAList, audio.processAList, "ProcessAList") ||
            !GET_FUNC(ptr_RomOpen, audio.romOpen, "RomOpen") ||
            !GET_FUNC(ptr_RomClosed, audio.romClosed, "RomClosed") ||
            !GET_FUNC(ptr_SetSpeedFactor, audio.setSpeedFactor, "SetSpeedFactor") ||
            !GET_FUNC(ptr_VolumeUp, audio.volumeUp, "VolumeUp") ||
            !GET_FUNC(ptr_VolumeDown, audio.volumeDown, "VolumeDown") ||
            !GET_FUNC(ptr_VolumeGetLevel, audio.volumeGetLevel, "VolumeGetLevel") ||
            !GET_FUNC(ptr_VolumeSetLevel, audio.volumeSetLevel, "VolumeSetLevel") ||
            !GET_FUNC(ptr_VolumeMute, audio.volumeMute, "VolumeMute") ||
            !GET_FUNC(ptr_VolumeGetString, audio.volumeGetString, "VolumeGetString"))
        {
            DebugMessage(M64MSG_ERROR, "broken Audio plugin; function(s) not found.");
            plugin_disconnect_audio();
            return M64ERR_INPUT_INVALID;
        }

        /* check the version info */
        (*audio.getVersion)(&PluginType, &PluginVersion, &APIVersion, NULL, NULL);
        if (PluginType != M64PLUGIN_AUDIO || (APIVersion & 0xffff0000) != (AUDIO_API_VERSION & 0xffff0000))
        {
            DebugMessage(M64MSG_ERROR, "incompatible Audio plugin");
            plugin_disconnect_audio();
            return M64ERR_INCOMPATIBLE;
        }

        l_AudioAttached = 1;
    }
    else
        plugin_disconnect_audio();

    return M64ERR_SUCCESS;
}

static m64p_error plugin_start_audio(void)
{
    /* fill in the AUDIO_INFO data structure */
    audio_info.RDRAM = (unsigned char *)mem_base_u32(g_mem_base, MM_RDRAM_DRAM);
    audio_info.DMEM = (unsigned char *)mem_base_u32(g_mem_base, MM_RSP_MEM);
    audio_info.IMEM = (unsigned char *)mem_base_u32(g_mem_base, MM_RSP_MEM + 0x1000);
    audio_info.MI_INTR_REG = &(g_dev.mi.regs[MI_INTR_REG]);
    audio_info.AI_DRAM_ADDR_REG = &(g_dev.ai.regs[AI_DRAM_ADDR_REG]);
    audio_info.AI_LEN_REG = &(g_dev.ai.regs[AI_LEN_REG]);
    audio_info.AI_CONTROL_REG = &(g_dev.ai.regs[AI_CONTROL_REG]);
    audio_info.AI_STATUS_REG = &dummy;
    audio_info.AI_DACRATE_REG = &(g_dev.ai.regs[AI_DACRATE_REG]);
    audio_info.AI_BITRATE_REG = &(g_dev.ai.regs[AI_BITRATE_REG]);
    audio_info.CheckInterrupts = EmptyFunc;

    /* call the audio plugin */
    if (!audio.initiateAudio(audio_info))
        return M64ERR_PLUGIN_FAIL;

    return M64ERR_SUCCESS;
}

static void plugin_disconnect_input(void)
{
    input = dummy_input;
    l_InputAttached = 0;
}

static m64p_error plugin_connect_input(m64p_dynlib_handle plugin_handle)
{
    /* attach the Input plugin function pointers */
    if (plugin_handle != NULL)
    {
        m64p_plugin_type PluginType;
        int PluginVersion, APIVersion;

        if (l_InputAttached)
            return M64ERR_INVALID_STATE;

        if (!GET_FUNC(ptr_PluginGetVersion, input.getVersion, "PluginGetVersion") ||
            !GET_FUNC(ptr_ControllerCommand, input.controllerCommand, "ControllerCommand") ||
            !GET_FUNC(ptr_GetKeys, input.getKeys, "GetKeys") ||
            !GET_FUNC(ptr_InitiateControllers, input.initiateControllers, "InitiateControllers") ||
            !GET_FUNC(ptr_ReadController, input.readController, "ReadController") ||
            !GET_FUNC(ptr_RomOpen, input.romOpen, "RomOpen") ||
            !GET_FUNC(ptr_RomClosed, input.romClosed, "RomClosed") ||
            !GET_FUNC(ptr_SDL_KeyDown, input.keyDown, "SDL_KeyDown") ||
            !GET_FUNC(ptr_SDL_KeyUp, input.keyUp, "SDL_KeyUp"))
        {
            DebugMessage(M64MSG_ERROR, "broken Input plugin; function(s) not found.");
            plugin_disconnect_input();
            return M64ERR_INPUT_INVALID;
        }

        if (!GET_FUNC(ptr_SendVRUWord, input.sendVRUWord, "SendVRUWord") ||
            !GET_FUNC(ptr_SetMicState, input.setMicState, "SetMicState") ||
            !GET_FUNC(ptr_ReadVRUResults, input.readVRUResults, "ReadVRUResults") ||
            !GET_FUNC(ptr_ClearVRUWords, input.clearVRUWords, "ClearVRUWords") ||
            !GET_FUNC(ptr_SetVRUWordMask, input.setVRUWordMask, "SetVRUWordMask"))
        {
            DebugMessage(M64MSG_WARNING, "Input plugin does not contain VRU support.");
        }

        /* check the version info */
        (*input.getVersion)(&PluginType, &PluginVersion, &APIVersion, NULL, NULL);
        if (PluginType != M64PLUGIN_INPUT || (APIVersion & 0xffff0000) != (INPUT_API_VERSION & 0xffff0000) || APIVersion < 0x020100)
        {
            DebugMessage(M64MSG_ERROR, "incompatible Input plugin");
            plugin_disconnect_input();
            return M64ERR_INCOMPATIBLE;
        }

        if (!GET_FUNC(ptr_RenderCallback, input.renderCallback, "RenderCallback"))
        {
            DebugMessage(M64MSG_INFO, "input plugin did not specify a render callback; there will be no on screen display by the input plugin.");
        }

        l_InputAttached = 1;
    }
    else
        plugin_disconnect_input();

    return M64ERR_SUCCESS;
}

static m64p_error plugin_start_input(void)
{
    int i;

    /* fill in the CONTROL_INFO data structure */
    control_info.Controls = Controls;
    for (i=0; i<4; i++)
      {
         Controls[i].Present = 0;
         Controls[i].RawData = 0;
         Controls[i].Plugin = PLUGIN_NONE;
         Controls[i].Type = CONT_TYPE_STANDARD;
      }

    /* call the input plugin */
    input.initiateControllers(control_info);

    return M64ERR_SUCCESS;
}

static void plugin_disconnect_rsp(void)
{
    rsp = dummy_rsp;
    l_RspAttached = 0;
}

static m64p_error plugin_connect_rsp(m64p_dynlib_handle plugin_handle)
{
    /* attach the RSP plugin function pointers */
    if (plugin_handle != NULL)
    {
        m64p_plugin_type PluginType;
        int PluginVersion, APIVersion;

        if (l_RspAttached)
            return M64ERR_INVALID_STATE;

        if (!GET_FUNC(ptr_PluginGetVersion, rsp.getVersion, "PluginGetVersion") ||
            !GET_FUNC(ptr_DoRspCycles, rsp.doRspCycles, "DoRspCycles") ||
            !GET_FUNC(ptr_InitiateRSP, rsp.initiateRSP, "InitiateRSP") ||
            !GET_FUNC(ptr_RomClosed, rsp.romClosed, "RomClosed"))
        {
            DebugMessage(M64MSG_ERROR, "broken RSP plugin; function(s) not found.");
            plugin_disconnect_rsp();
            return M64ERR_INPUT_INVALID;
        }

        /* check the version info */
        (*rsp.getVersion)(&PluginType, &PluginVersion, &APIVersion, NULL, NULL);
        if (PluginType != M64PLUGIN_RSP || (APIVersion & 0xffff0000) != (RSP_API_VERSION & 0xffff0000))
        {
            DebugMessage(M64MSG_ERROR, "incompatible RSP plugin");
            plugin_disconnect_rsp();
            return M64ERR_INCOMPATIBLE;
        }

        l_RspAttached = 1;
    }
    else
        plugin_disconnect_rsp();

    return M64ERR_SUCCESS;
}

static m64p_error plugin_start_rsp(void)
{
    /* fill in the RSP_INFO data structure */
    rsp_info.RDRAM = (unsigned char *)mem_base_u32(g_mem_base, MM_RDRAM_DRAM);
    rsp_info.DMEM = (unsigned char *)mem_base_u32(g_mem_base, MM_RSP_MEM);
    rsp_info.IMEM = (unsigned char *)mem_base_u32(g_mem_base, MM_RSP_MEM + 0x1000);
    rsp_info.MI_INTR_REG = &g_dev.mi.regs[MI_INTR_REG];
    rsp_info.SP_MEM_ADDR_REG = &g_dev.sp.regs[SP_MEM_ADDR_REG];
    rsp_info.SP_DRAM_ADDR_REG = &g_dev.sp.regs[SP_DRAM_ADDR_REG];
    rsp_info.SP_RD_LEN_REG = &g_dev.sp.regs[SP_RD_LEN_REG];
    rsp_info.SP_WR_LEN_REG = &g_dev.sp.regs[SP_WR_LEN_REG];
    rsp_info.SP_STATUS_REG = &g_dev.sp.regs[SP_STATUS_REG];
    rsp_info.SP_DMA_FULL_REG = &g_dev.sp.regs[SP_DMA_FULL_REG];
    rsp_info.SP_DMA_BUSY_REG = &g_dev.sp.regs[SP_DMA_BUSY_REG];
    rsp_info.SP_PC_REG = &g_dev.sp.regs2[SP_PC_REG];
    rsp_info.SP_SEMAPHORE_REG = &g_dev.sp.regs[SP_SEMAPHORE_REG];
    rsp_info.DPC_START_REG = &g_dev.dp.dpc_regs[DPC_START_REG];
    rsp_info.DPC_END_REG = &g_dev.dp.dpc_regs[DPC_END_REG];
    rsp_info.DPC_CURRENT_REG = &g_dev.dp.dpc_regs[DPC_CURRENT_REG];
    rsp_info.DPC_STATUS_REG = &g_dev.dp.dpc_regs[DPC_STATUS_REG];
    rsp_info.DPC_CLOCK_REG = &g_dev.dp.dpc_regs[DPC_CLOCK_REG];
    rsp_info.DPC_BUFBUSY_REG = &g_dev.dp.dpc_regs[DPC_BUFBUSY_REG];
    rsp_info.DPC_PIPEBUSY_REG = &g_dev.dp.dpc_regs[DPC_PIPEBUSY_REG];
    rsp_info.DPC_TMEM_REG = &g_dev.dp.dpc_regs[DPC_TMEM_REG];
    rsp_info.CheckInterrupts = EmptyFunc;
    rsp_info.ProcessDlistList = gfx.processDList;
    rsp_info.ProcessAlistList = audio.processAList;
    rsp_info.ProcessRdpList = rsp_process_rdp_list;
    rsp_info.ShowCFB = gfx.showCFB;

    /* call the RSP plugin  */
    rsp.initiateRSP(rsp_info, NULL);

    return M64ERR_SUCCESS;
}

static void plugin_disconnect_execution(void)
{
    execution = dummy_execution;
    memset(execution_hooks, 0, sizeof(execution_hooks));
    memset(execution_hook_pages, 0, sizeof(execution_hook_pages));
    execution_branch_cancelled = 0;
    execution_reload_pending = 0;
    l_ExecutionAttached = 0;
}

static m64p_error plugin_connect_execution(m64p_dynlib_handle plugin_handle)
{
    /* attach the Execution plugin function pointers */
    if (plugin_handle != NULL)
    {
        m64p_plugin_type PluginType;
        int PluginVersion, APIVersion;

        if (l_ExecutionAttached)
            return M64ERR_INVALID_STATE;

        if (!GET_FUNC(ptr_PluginGetVersion, execution.getVersion, "PluginGetVersion") ||
            !GET_FUNC(ptr_RomOpen, execution.romOpen, "RomOpen") ||
            !GET_FUNC(ptr_RomClosed, execution.romClosed, "RomClosed") ||
            !GET_FUNC(ptr_InitiateExecution, execution.initiateExecution, "InitiateExecution") ||
            !GET_FUNC(ptr_Input, execution.input, "Input") ||
            !GET_FUNC(ptr_Frame, execution.frame, "Frame") ||
            !GET_FUNC(ptr_Execute, execution.execute, "Execute") ||
            !GET_FUNC(ptr_ExecuteDone, execution.executeDone, "ExecuteDone") ||
            !GET_FUNC(ptr_Read8, execution.read8, "Read8") ||
            !GET_FUNC(ptr_Read16, execution.read16, "Read16") ||
            !GET_FUNC(ptr_Read32, execution.read32, "Read32") ||
            !GET_FUNC(ptr_Read64, execution.read64, "Read64") ||
            !GET_FUNC(ptr_Write8, execution.write8, "Write8") ||
            !GET_FUNC(ptr_Write16, execution.write16, "Write16") ||
            !GET_FUNC(ptr_Write32, execution.write32, "Write32") ||
            !GET_FUNC(ptr_Write64, execution.write64, "Write64"))
        {
            DebugMessage(M64MSG_ERROR, "broken execution plugin; function(s) not found.");
            plugin_disconnect_execution();
            return M64ERR_INPUT_INVALID;
        }

        /* optional functions */
        if (!GET_FUNC(ptr_StateLoaded, execution.stateLoaded, "StateLoaded"))
            execution.stateLoaded = dummyexecution_StateLoaded;
        if (!GET_FUNC(ptr_Reset, execution.reset, "Reset"))
            execution.reset = dummyexecution_Reset;
        if (!GET_FUNC(ptr_NetplayStart, execution.netplayStart, "NetplayStart"))
            execution.netplayStart = NULL;
        if (!GET_FUNC(ptr_NetplayExport, execution.netplayExport, "NetplayExport"))
            execution.netplayExport = NULL;
        if (!GET_FUNC(ptr_NetplayImport, execution.netplayImport, "NetplayImport"))
            execution.netplayImport = NULL;

        /* check the version info */
        (*execution.getVersion)(&PluginType, &PluginVersion, &APIVersion, NULL, NULL);
        if (PluginType != M64PLUGIN_EXECUTION || (APIVersion & 0xffff0000) != (EXECUTION_API_VERSION & 0xffff0000))
        {
            DebugMessage(M64MSG_ERROR, "incompatible execution plugin");
            plugin_disconnect_execution();
            return M64ERR_INCOMPATIBLE;
        }

        l_ExecutionAttached = 1;
    }
    else {
        plugin_disconnect_execution();
    }

    return M64ERR_SUCCESS;
}

static m64p_error plugin_start_execution(void)
{
    return M64ERR_SUCCESS;
}

/* global functions */
m64p_error plugin_connect(m64p_plugin_type type, m64p_dynlib_handle plugin_handle)
{
    switch(type)
    {
        case M64PLUGIN_GFX:
            if (plugin_handle != NULL && (l_AudioAttached || l_InputAttached || l_RspAttached))
                DebugMessage(M64MSG_WARNING, "Front-end bug: plugins are attached in wrong order.");
            return plugin_connect_gfx(plugin_handle);
        case M64PLUGIN_AUDIO:
            if (plugin_handle != NULL && (l_InputAttached || l_RspAttached))
                DebugMessage(M64MSG_WARNING, "Front-end bug: plugins are attached in wrong order.");
            return plugin_connect_audio(plugin_handle);
        case M64PLUGIN_INPUT:
            if (plugin_handle != NULL && (l_RspAttached))
                DebugMessage(M64MSG_WARNING, "Front-end bug: plugins are attached in wrong order.");
            return plugin_connect_input(plugin_handle);
        case M64PLUGIN_RSP:
            return plugin_connect_rsp(plugin_handle);
        case M64PLUGIN_EXECUTION:
            return plugin_connect_execution(plugin_handle);
        default:
            return M64ERR_INPUT_INVALID;
    }

    return M64ERR_INTERNAL;
}

m64p_error plugin_start(m64p_plugin_type type)
{
    switch(type)
    {
        case M64PLUGIN_RSP:
            return plugin_start_rsp();
        case M64PLUGIN_GFX:
            return plugin_start_gfx();
        case M64PLUGIN_AUDIO:
            return plugin_start_audio();
        case M64PLUGIN_INPUT:
            return plugin_start_input();
        case M64PLUGIN_EXECUTION:
            return plugin_start_execution();
        default:
            return M64ERR_INPUT_INVALID;
    }

    return M64ERR_INTERNAL;
}

m64p_error plugin_check(void)
{
    if (!l_GfxAttached)
        DebugMessage(M64MSG_WARNING, "No video plugin attached.  There will be no video output.");
    if (!l_RspAttached)
        DebugMessage(M64MSG_WARNING, "No RSP plugin attached.  The video output will be corrupted.");
    if (!l_AudioAttached)
        DebugMessage(M64MSG_WARNING, "No audio plugin attached.  There will be no sound output.");
    if (!l_InputAttached)
        DebugMessage(M64MSG_WARNING, "No input plugin attached.  You won't be able to control the game.");

    return M64ERR_SUCCESS;
}

static void save_state(void)
{
    main_state_save(1, NULL);
}

/* jump() for the plugin: continues at the address instead of the current instruction (from Execute), or after the
 * current instruction (from the other callbacks) */
static void execution_jump(uint32_t address)
{
    struct r4300_core* r4300 = &g_dev.r4300;

    /* The interpreters count cycles from how far the PC got since the last branch (cp0.last_addr): count what ran up
     * to here, then count from the new address, as a branch does. Without this, the next count added the distance
     * jumped: about a cycle per 4 bytes forward, and about 2^30 cycles for a jump back (the subtraction wrapped). */
    cp0_update_count(r4300);

    /* In a delay slot, the branch would take the PC away from where the plugin wants it */
    if (r4300->delay_slot)
        execution_branch_cancelled = 1;

    generic_jump_to(r4300, address);
    r4300->cp0.last_addr = *r4300_pc(r4300);
}

static void invalidate_code(uint32_t addr, uint32_t size)
{
    struct r4300_core* r4300 = &g_dev.r4300;
    uint32_t start, end;

    if (size == 0 || r4300->emumode == EMUMODE_PURE_INTERPRETER)
        return;

    start = addr & ~UINT32_C(3);
    end = (addr + size + 3) & ~UINT32_C(3);

    /* KSEG0 and KSEG1, as a game's own writes do (TLB-mapped addresses are invalidated as they are) */
    if ((start & UINT32_C(0xc0000000)) == UINT32_C(0x80000000))
    {
        invalidate_r4300_cached_code(r4300, (start & UINT32_C(0x1fffffff)) | UINT32_C(0x80000000), end - start);
        invalidate_r4300_cached_code(r4300, (start & UINT32_C(0x1fffffff)) | UINT32_C(0xa0000000), end - start);
    }
    else
    {
        invalidate_r4300_cached_code(r4300, start, end - start);
    }

    execution_reload_pending = 1;
}

EXPORT void CALL ExecutionInvalidateCode(uint32_t addr, uint32_t size)
{
    if (!g_EmulatorRunning)
        return;

    invalidate_code(addr, size);
}

/* Sets or clears a word's bit in a kind's hook map, and its page's bit (on while any word in the page has one) */
static void execution_set_hook(uint32_t addr, int kind, int on)
{
    uint32_t word = (addr >> 2) & (EXECUTION_HOOK_WORDS - 1);
    uint32_t page = (addr >> 12) & (EXECUTION_HOOK_PAGES - 1);
    const uint8_t* page_words;
    size_t i;

    if (kind < M64HOOK_EXECUTE || kind > M64HOOK_WRITE)
        return;

    page_words = &execution_hooks[kind][page * (0x1000 / 4 / 8)];
    if (on)
        execution_hooks[kind][word >> 3] |= (uint8_t)(1 << (word & 7));
    else
        execution_hooks[kind][word >> 3] &= (uint8_t)~(1 << (word & 7));

    for (i = 0; i < 0x1000 / 4 / 8 && !page_words[i]; ++i);
    if (i < 0x1000 / 4 / 8)
        execution_hook_pages[kind][page >> 3] |= (uint8_t)(1 << (page & 7));
    else
        execution_hook_pages[kind][page >> 3] &= (uint8_t)~(1 << (page & 7));
}

int execution_plugin_attached(void)
{
    return l_ExecutionAttached;
}

void initiate_execution_plugin(void)
{
    memset(execution_hooks, 0, sizeof(execution_hooks));
    memset(execution_hook_pages, 0, sizeof(execution_hook_pages));
    execution_branch_cancelled = 0;
    execution_reload_pending = 0;

    execution_info.window = NULL;
    execution_info.rom_name = ROM_HEADER.Name;
    execution_info.rom_hash = (uint8_t*)ROM_SETTINGS.MD5;
    execution_info.set_hook = execution_set_hook;
    execution_info.pc = r4300_pc_g;
    execution_info.jump = execution_jump;
    execution_info.gpr = r4300_regs(&g_dev.r4300);
    execution_info.fpr = (double*)r4300_cp1_regs(&g_dev.r4300.cp1);
    execution_info.rdram = mem_base_u32(g_mem_base, MM_RDRAM_DRAM);
    execution_info.rdram_size = g_dev.rdram.dram_size;
    execution_info.save_state = save_state;
    execution_info.set_save_state_slot = main_state_set_slot;
    execution.initiateExecution(execution_info);
}

void execution_state_loaded(void)
{
    /* A jump the plugin made in a delay slot belongs to the state before the load */
    execution_branch_cancelled = 0;
    execution.stateLoaded();
}

void execution_reset(int hard)
{
    execution_branch_cancelled = 0;
    execution.reset(hard);
}

void execution_netplay_start(int is_host)
{
    if (l_ExecutionAttached && execution.netplayStart != NULL)
        execution.netplayStart(is_host);
}

int execution_netplay_export(const void** data, uint32_t* size)
{
    *data = NULL;
    *size = 0;
    if (!l_ExecutionAttached || execution.netplayExport == NULL)
        return 0;
    if (execution.netplayExport(data, size) != 0 || *data == NULL)
    {
        DebugMessage(M64MSG_WARNING, "The execution plugin couldn't export its settings for netplay");
        *data = NULL;
        *size = 0;
        return 0;
    }
    return 1;
}

void execution_netplay_import(const void* data, uint32_t size)
{
    if (!l_ExecutionAttached || execution.netplayImport == NULL || size == 0)
        return;
    if (execution.netplayImport(data, size) != 0)
        DebugMessage(M64MSG_WARNING, "The execution plugin couldn't import the host's settings for netplay");
}
