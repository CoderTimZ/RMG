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
	/* optional: a plugin without them gets the dummy (no-op) versions */
	ptr_StateLoaded         stateLoaded;
	ptr_Reset               reset;
	/* optional, NULL when the plugin doesn't have them */
	ptr_NetplayStart        netplayStart;
	ptr_NetplayExport       netplayExport;
	ptr_NetplayImport       netplayImport;
} execution_plugin_functions;

extern execution_plugin_functions execution;

/* The plugin's hooks: a bit per word of RDRAM for each kind (m64p_execution_hook), set by the plugin through set_hook().
 * Addresses are taken modulo 8 MB, so a KSEG0 address and its KSEG1 mirror share a bit, and the plugin checks the full
 * address itself. execution_hook_pages has a bit per 4 KB page for the pages with a hook (256 bytes per kind, which
 * stay in the L1 cache), so most checks never read the 256 KB maps. */
#define EXECUTION_HOOK_MAPS 3
#define EXECUTION_HOOK_WORDS (UINT32_C(0x800000) / 4)
#define EXECUTION_HOOK_PAGES (UINT32_C(0x800000) / 0x1000)
extern uint8_t execution_hooks[EXECUTION_HOOK_MAPS][EXECUTION_HOOK_WORDS / 8];
extern uint8_t execution_hook_pages[EXECUTION_HOOK_MAPS][EXECUTION_HOOK_PAGES / 8];

/* Set when the plugin calls jump() during a delay slot: the branch the delay slot belongs to is not taken, so execution
 * continues where the plugin jumped (the interpreters' branch code checks and clears it) */
extern int execution_branch_cancelled;

/* Counts the exceptions an instruction raised itself (not interrupts): ExecuteDone isn't sent for an instruction that
 * didn't complete because of one */
extern unsigned int execution_sync_exceptions;

/* Set when the plugin invalidated cached code: the cached interpreter reloads the current block before its next
 * instruction, so a change to code just ahead of the current instruction is seen right away */
extern int execution_reload_pending;

extern int execution_plugin_attached(void);

/* waits until the video plugin's RDP has written everything to RDRAM (for
 * plugins that write it asynchronously; others are done already) */
extern void plugin_wait_rdp_idle(void);

/* the video plugin's RDP state outside RDRAM (0 if it has none to hand over) */
extern unsigned int plugin_rdp_state_size(void);
extern void plugin_save_rdp_state(void* data);
extern void plugin_load_rdp_state(const void* data);

/* the RSP plugin's registers, which carry over between tasks (0 if it has
 * none to hand over) */
extern unsigned int plugin_rsp_state_size(void);
extern void plugin_save_rsp_state(void* data);
extern void plugin_load_rsp_state(const void* data);
extern void initiate_execution_plugin(void);
extern void execution_state_loaded(void);
extern void execution_reset(int hard);

/* the new netplay (see NetplayStart in m64p_plugin.h); the export and import return 0 if the plugin has nothing */
extern void execution_netplay_start(int is_host);
extern int execution_netplay_export(const void** data, uint32_t* size);
extern void execution_netplay_import(const void* data, uint32_t size);

/* Whether the plugin hooked the word at the address for a kind of call (an m64p_execution_hook) */
static inline int execution_hooked(int kind, uint32_t addr) {
	uint32_t page = (addr >> 12) & (EXECUTION_HOOK_PAGES - 1);
	uint32_t word = (addr >> 2) & (EXECUTION_HOOK_WORDS - 1);
	if (!(execution_hook_pages[kind][page >> 3] & (1 << (page & 7))))
		return 0;
	return execution_hooks[kind][word >> 3] & (1 << (word & 7));
}

/* Whether either word of an aligned doubleword is hooked */
static inline int execution_dword_hooked(int kind, uint32_t addr) {
	return execution_hooked(kind, addr) || execution_hooked(kind, addr + 4);
}

#endif

