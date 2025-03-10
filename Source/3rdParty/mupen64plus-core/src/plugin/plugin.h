/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - plugin.h                                                *
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

#ifndef PLUGIN_H
#define PLUGIN_H

#include "api/m64p_common.h"
#include "api/m64p_plugin.h"
#include "api/m64p_types.h"

extern m64p_error plugin_connect(m64p_plugin_type, m64p_dynlib_handle plugin_handle);
extern m64p_error plugin_start(m64p_plugin_type);
extern m64p_error plugin_check(void);

enum { NUM_CONTROLLER = 4 };
extern CONTROL Controls[NUM_CONTROLLER];

/*** Version requirement information ***/
#define RSP_API_VERSION       0x20000
#define GFX_API_VERSION       0x20200
#define AUDIO_API_VERSION     0x20000
#define INPUT_API_VERSION     0x20101
#define EXECUTION_API_VERSION 0x20000

/* video plugin function pointers */
typedef struct _gfx_plugin_functions
{
	ptr_PluginGetVersion getVersion;
	ptr_ChangeWindow     changeWindow;
	ptr_InitiateGFX      initiateGFX;
	ptr_MoveScreen       moveScreen;
	ptr_ProcessDList     processDList;
	ptr_ProcessRDPList   processRDPList;
	ptr_RomClosed        romClosed;
	ptr_RomOpen          romOpen;
	ptr_ShowCFB          showCFB;
	ptr_UpdateScreen     updateScreen;
	ptr_ViStatusChanged  viStatusChanged;
	ptr_ViWidthChanged   viWidthChanged;
	ptr_ReadScreen2      readScreen;
	ptr_SetRenderingCallback setRenderingCallback;
    ptr_ResizeVideoOutput    resizeVideoOutput;

	/* frame buffer plugin spec extension */
	ptr_FBRead          fBRead;
	ptr_FBWrite         fBWrite;
	ptr_FBGetFrameBufferInfo fBGetFrameBufferInfo;
} gfx_plugin_functions;

extern gfx_plugin_functions gfx;

/* audio plugin function pointers */
typedef struct _audio_plugin_functions
{
	ptr_PluginGetVersion  getVersion;
	ptr_AiDacrateChanged  aiDacrateChanged;
	ptr_AiLenChanged      aiLenChanged;
	ptr_InitiateAudio     initiateAudio;
	ptr_ProcessAList      processAList;
	ptr_RomClosed         romClosed;
	ptr_RomOpen           romOpen;
	ptr_SetSpeedFactor    setSpeedFactor;
	ptr_VolumeUp          volumeUp;
	ptr_VolumeDown        volumeDown;
	ptr_VolumeGetLevel    volumeGetLevel;
	ptr_VolumeSetLevel    volumeSetLevel;
	ptr_VolumeMute        volumeMute;
	ptr_VolumeGetString   volumeGetString;
} audio_plugin_functions;

extern audio_plugin_functions audio;

/* input plugin function pointers */
typedef struct _input_plugin_functions
{
	ptr_PluginGetVersion    getVersion;
	ptr_ControllerCommand   controllerCommand;
	ptr_GetKeys             getKeys;
	ptr_InitiateControllers initiateControllers;
	ptr_ReadController      readController;
	ptr_RomClosed           romClosed;
	ptr_RomOpen             romOpen;
	ptr_SDL_KeyDown         keyDown;
	ptr_SDL_KeyUp           keyUp;
	ptr_RenderCallback      renderCallback;
	ptr_SendVRUWord         sendVRUWord;
	ptr_SetMicState         setMicState;
	ptr_ReadVRUResults      readVRUResults;
	ptr_ClearVRUWords       clearVRUWords;
	ptr_SetVRUWordMask      setVRUWordMask;
} input_plugin_functions;

extern input_plugin_functions input;

/* RSP plugin function pointers */
typedef struct _rsp_plugin_functions
{
	ptr_PluginGetVersion    getVersion;
	ptr_DoRspCycles         doRspCycles;
	ptr_InitiateRSP         initiateRSP;
	ptr_RomClosed           romClosed;
} rsp_plugin_functions;

extern rsp_plugin_functions rsp;

/* execution plugin function pointers */
typedef struct _execution_plugin_functions
{
	ptr_PluginGetVersion    getVersion;
	ptr_RomOpen             romOpen;
	ptr_RomClosed           romClosed;
	ptr_InitiateExecution   initiateExecution;
	ptr_Input               input;
	ptr_Frame               frame;
	ptr_Execute             execute;
	ptr_ExecuteDone         executeDone;
	ptr_Read8               read8;
	ptr_Read16              read16;
	ptr_Read32              read32;
	ptr_Read64              read64;
	ptr_Write8              write8;
	ptr_Write16             write16;
	ptr_Write32             write32;
	ptr_Write64             write64;
} execution_plugin_functions;

extern execution_plugin_functions execution;

extern uint8_t execution_addr_mask[1024];

extern void initiate_execution_plugin(void);

inline int execution_addr_masked(uint32_t addr) {
	return execution_addr_mask[(addr & 0b111111111100000) >> 5] & (1 << ((addr & 0b11100) >> 2));
}

#endif

