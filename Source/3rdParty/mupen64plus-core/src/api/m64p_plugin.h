/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-core - m64p_plugin.h                                      *
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

#if !defined(M64P_PLUGIN_H)
#define M64P_PLUGIN_H

#include "m64p_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*** Controller plugin's ****/
#define PLUGIN_NONE                 1
#define PLUGIN_MEMPAK               2
#define PLUGIN_RUMBLE_PAK           3 /* not implemented for non raw data */
#define PLUGIN_TRANSFER_PAK         4 /* not implemented for non raw data */
#define PLUGIN_RAW                  5 /* the controller plugin is passed in raw data */
#define PLUGIN_BIO_PAK              6

/***** Structures *****/
typedef struct {
    unsigned char * RDRAM;
    unsigned char * DMEM;
    unsigned char * IMEM;

    unsigned int * MI_INTR_REG;

    unsigned int * SP_MEM_ADDR_REG;
    unsigned int * SP_DRAM_ADDR_REG;
    unsigned int * SP_RD_LEN_REG;
    unsigned int * SP_WR_LEN_REG;
    unsigned int * SP_STATUS_REG;
    unsigned int * SP_DMA_FULL_REG;
    unsigned int * SP_DMA_BUSY_REG;
    unsigned int * SP_PC_REG;
    unsigned int * SP_SEMAPHORE_REG;

    unsigned int * DPC_START_REG;
    unsigned int * DPC_END_REG;
    unsigned int * DPC_CURRENT_REG;
    unsigned int * DPC_STATUS_REG;
    unsigned int * DPC_CLOCK_REG;
    unsigned int * DPC_BUFBUSY_REG;
    unsigned int * DPC_PIPEBUSY_REG;
    unsigned int * DPC_TMEM_REG;

    void (*CheckInterrupts)(void);
    void (*ProcessDlistList)(void);
    void (*ProcessAlistList)(void);
    void (*ProcessRdpList)(void);
    void (*ShowCFB)(void);
} RSP_INFO;

typedef struct {
    unsigned char * HEADER;  /* This is the rom header (first 40h bytes of the rom) */
    unsigned char * RDRAM;
    unsigned char * DMEM;
    unsigned char * IMEM;

    unsigned int * MI_INTR_REG;

    unsigned int * DPC_START_REG;
    unsigned int * DPC_END_REG;
    unsigned int * DPC_CURRENT_REG;
    unsigned int * DPC_STATUS_REG;
    unsigned int * DPC_CLOCK_REG;
    unsigned int * DPC_BUFBUSY_REG;
    unsigned int * DPC_PIPEBUSY_REG;
    unsigned int * DPC_TMEM_REG;

    unsigned int * VI_STATUS_REG;
    unsigned int * VI_ORIGIN_REG;
    unsigned int * VI_WIDTH_REG;
    unsigned int * VI_INTR_REG;
    unsigned int * VI_V_CURRENT_LINE_REG;
    unsigned int * VI_TIMING_REG;
    unsigned int * VI_V_SYNC_REG;
    unsigned int * VI_H_SYNC_REG;
    unsigned int * VI_LEAP_REG;
    unsigned int * VI_H_START_REG;
    unsigned int * VI_V_START_REG;
    unsigned int * VI_V_BURST_REG;
    unsigned int * VI_X_SCALE_REG;
    unsigned int * VI_Y_SCALE_REG;

    void (*CheckInterrupts)(void);

    /* The GFX_INFO.version parameter was added in version 2.5.1 of the core.
       Plugins should ensure the core is at least this version before
       attempting to read GFX_INFO.version. */
    unsigned int version;
    /* SP_STATUS_REG and RDRAM_SIZE were added in version 2 of GFX_INFO.version.
       Plugins should only attempt to read these values if GFX_INFO.version is at least 2. */

    /* The RSP plugin should set (HALT | BROKE | TASKDONE) *before* calling ProcessDList.
       It should not modify SP_STATUS_REG after ProcessDList has returned.
       This will allow the GFX plugin to unset these bits if it needs. */
    unsigned int * SP_STATUS_REG;
    const unsigned int * RDRAM_SIZE;
} GFX_INFO;

typedef struct {
    unsigned char * RDRAM;
    unsigned char * DMEM;
    unsigned char * IMEM;

    unsigned int * MI_INTR_REG;

    unsigned int * AI_DRAM_ADDR_REG;
    unsigned int * AI_LEN_REG;
    unsigned int * AI_CONTROL_REG;
    unsigned int * AI_STATUS_REG;
    unsigned int * AI_DACRATE_REG;
    unsigned int * AI_BITRATE_REG;

    void (*CheckInterrupts)(void);
} AUDIO_INFO;

/*** Controller types ****/
#define CONT_TYPE_STANDARD          0
#define CONT_TYPE_VRU               1

typedef struct {
    int Present;
    int RawData;
    int Plugin;
    int Type;
} CONTROL;

typedef union {
    unsigned int Value;
    struct {
        unsigned R_DPAD       : 1;
        unsigned L_DPAD       : 1;
        unsigned D_DPAD       : 1;
        unsigned U_DPAD       : 1;
        unsigned START_BUTTON : 1;
        unsigned Z_TRIG       : 1;
        unsigned B_BUTTON     : 1;
        unsigned A_BUTTON     : 1;

        unsigned R_CBUTTON    : 1;
        unsigned L_CBUTTON    : 1;
        unsigned D_CBUTTON    : 1;
        unsigned U_CBUTTON    : 1;
        unsigned R_TRIG       : 1;
        unsigned L_TRIG       : 1;
        unsigned Reserved1    : 1;
        unsigned Reserved2    : 1;

        signed   X_AXIS       : 8;
        signed   Y_AXIS       : 8;
    };
} BUTTONS;

typedef struct {
    CONTROL *Controls;      /* A pointer to an array of 4 controllers .. eg:
                               CONTROL Controls[4]; */
} CONTROL_INFO;

/* The kinds of hook an execution plugin sets with EXECUTION_INFO's set_hook */
typedef enum {
    M64HOOK_EXECUTE = 0, /* Execute() before the instruction at the address runs, and ExecuteDone() after it */
    M64HOOK_READ,        /* Read8..Read64() for loads from the address */
    M64HOOK_WRITE        /* Write8..Write64() for stores to it */
} m64p_execution_hook;

typedef struct {
    void* window;
    uint8_t* rom_name;
    uint8_t* rom_hash;
    /* Turns the core's calls of a kind (m64p_execution_hook) for an address on or off. The core keeps a bit per word of
     * RDRAM for each kind, which addresses that agree in bits 2..22 share (like a KSEG0 address and its KSEG1 mirror, or
     * the bytes of a word): the plugin turns it off only once none of them is hooked, and checks the full address in
     * the calls itself. */
    void (*set_hook)(uint32_t addr, int kind, int on);
    uint32_t (*pc)();
    void (*jump)(uint32_t addr);
    int64_t* gpr;
    double* fpr;
    void* rdram;
    uint32_t rdram_size;
    void (*save_state)();
    void (*set_save_state_slot)(int32_t slot);
} EXECUTION_INFO;

/* common plugin function pointer types */
typedef void (*ptr_RomClosed)(void);
typedef int  (*ptr_RomOpen)(void);
#if defined(M64P_PLUGIN_PROTOTYPES)
EXPORT int  CALL RomOpen(void);
EXPORT void CALL RomClosed(void);
#endif

/* video plugin function pointer types */
typedef void (*ptr_ChangeWindow)(void);
typedef int  (*ptr_InitiateGFX)(GFX_INFO Gfx_Info);
typedef void (*ptr_MoveScreen)(int x, int y);
typedef void (*ptr_ProcessDList)(void);
typedef void (*ptr_ProcessRDPList)(void);
typedef void (*ptr_ShowCFB)(void);
typedef void (*ptr_UpdateScreen)(void);
typedef void (*ptr_ViStatusChanged)(void);
typedef void (*ptr_ViWidthChanged)(void);
typedef void (*ptr_ReadScreen2)(void *dest, int *width, int *height, int front);
typedef void (*ptr_SetRenderingCallback)(void (*callback)(int));
typedef void (*ptr_ResizeVideoOutput)(int width, int height);
#if defined(M64P_PLUGIN_PROTOTYPES)
EXPORT void CALL ChangeWindow(void);
EXPORT int  CALL InitiateGFX(GFX_INFO Gfx_Info);
EXPORT void CALL MoveScreen(int x, int y);
EXPORT void CALL ProcessDList(void);
EXPORT void CALL ProcessRDPList(void);
EXPORT void CALL ShowCFB(void);
EXPORT void CALL UpdateScreen(void);
EXPORT void CALL ViStatusChanged(void);
EXPORT void CALL ViWidthChanged(void);
EXPORT void CALL ReadScreen2(void *dest, int *width, int *height, int front);
EXPORT void CALL SetRenderingCallback(void (*callback)(int));
EXPORT void CALL ResizeVideoOutput(int width, int height);
#endif

/* frame buffer plugin spec extension */
typedef struct
{
   unsigned int addr;
   unsigned int size;
   unsigned int width;
   unsigned int height;
} FrameBufferInfo;
typedef void (*ptr_FBRead)(unsigned int addr);
typedef void (*ptr_FBWrite)(unsigned int addr, unsigned int size);
typedef void (*ptr_FBGetFrameBufferInfo)(void *p);
#if defined(M64P_PLUGIN_PROTOTYPES)
EXPORT void CALL FBRead(unsigned int addr);
EXPORT void CALL FBWrite(unsigned int addr, unsigned int size);
EXPORT void CALL FBGetFrameBufferInfo(void *p);
#endif

/* audio plugin function pointers */
typedef void (*ptr_AiDacrateChanged)(int SystemType);
typedef void (*ptr_AiLenChanged)(void);
typedef int  (*ptr_InitiateAudio)(AUDIO_INFO Audio_Info);
typedef void (*ptr_ProcessAList)(void);
typedef void (*ptr_SetSpeedFactor)(int percent);
typedef void (*ptr_VolumeUp)(void);
typedef void (*ptr_VolumeDown)(void);
typedef int  (*ptr_VolumeGetLevel)(void);
typedef void (*ptr_VolumeSetLevel)(int level);
typedef void (*ptr_VolumeMute)(void);
typedef const char * (*ptr_VolumeGetString)(void);
#if defined(M64P_PLUGIN_PROTOTYPES)
EXPORT void CALL AiDacrateChanged(int SystemType);
EXPORT void CALL AiLenChanged(void);
EXPORT int  CALL InitiateAudio(AUDIO_INFO Audio_Info);
EXPORT void CALL ProcessAList(void);
EXPORT void CALL SetSpeedFactor(int percent);
EXPORT void CALL VolumeUp(void);
EXPORT void CALL VolumeDown(void);
EXPORT int  CALL VolumeGetLevel(void);
EXPORT void CALL VolumeSetLevel(int level);
EXPORT void CALL VolumeMute(void);
EXPORT const char * CALL VolumeGetString(void);
#endif

/* input plugin function pointers */
typedef void (*ptr_ControllerCommand)(int Control, unsigned char *Command);
typedef void (*ptr_GetKeys)(int Control, BUTTONS *Keys);
typedef void (*ptr_InitiateControllers)(CONTROL_INFO ControlInfo);
typedef void (*ptr_ReadController)(int Control, unsigned char *Command);
typedef void (*ptr_SDL_KeyDown)(int keymod, int keysym);
typedef void (*ptr_SDL_KeyUp)(int keymod, int keysym);
typedef void (*ptr_RenderCallback)(void);
typedef void (*ptr_SendVRUWord)(uint16_t length, uint16_t *word, uint8_t lang);
typedef void (*ptr_SetMicState)(int state);
typedef void (*ptr_ReadVRUResults)(uint16_t *error_flags, uint16_t *num_results, uint16_t *mic_level, uint16_t *voice_level, uint16_t *voice_length, uint16_t *matches);
typedef void (*ptr_ClearVRUWords)(uint8_t length);
typedef void (*ptr_SetVRUWordMask)(uint8_t length, uint8_t *mask);
#if defined(M64P_PLUGIN_PROTOTYPES)
EXPORT void CALL ControllerCommand(int Control, unsigned char *Command);
EXPORT void CALL GetKeys(int Control, BUTTONS *Keys);
EXPORT void CALL InitiateControllers(CONTROL_INFO ControlInfo);
EXPORT void CALL ReadController(int Control, unsigned char *Command);
EXPORT void CALL SDL_KeyDown(int keymod, int keysym);
EXPORT void CALL SDL_KeyUp(int keymod, int keysym);
EXPORT void CALL RenderCallback(void);
EXPORT void CALL SendVRUWord(uint16_t length, uint16_t *word, uint8_t lang);
EXPORT void CALL SetMicState(int state);
EXPORT void CALL ReadVRUResults(uint16_t *error_flags, uint16_t *num_results, uint16_t *mic_level, uint16_t *voice_level, uint16_t *voice_length, uint16_t *matches);
EXPORT void CALL ClearVRUWords(uint8_t length);
EXPORT void CALL SetVRUWordMask(uint8_t length, uint8_t *mask);
#endif

/* RSP plugin function pointers */
typedef unsigned int (*ptr_DoRspCycles)(unsigned int Cycles);
typedef void (*ptr_InitiateRSP)(RSP_INFO Rsp_Info, unsigned int *CycleCount);
#if defined(M64P_PLUGIN_PROTOTYPES)
EXPORT unsigned int CALL DoRspCycles(unsigned int Cycles);
EXPORT void CALL InitiateRSP(RSP_INFO Rsp_Info, unsigned int *CycleCount);
#endif

/* execution plugin function pointers */
typedef void(*ptr_InitiateExecution)(EXECUTION_INFO ExecutionInfo);
typedef void(*ptr_Input)(int port, BUTTONS* buttons);
typedef void(*ptr_Frame)  (uint32_t frame);
typedef void(*ptr_Execute)(uint32_t pc);
typedef void(*ptr_ExecuteDone)(uint32_t pc);
typedef void(*ptr_Read8)  (uint8_t*  value, uint32_t pc, uint32_t addr);
typedef void(*ptr_Read16) (uint16_t* value, uint32_t pc, uint32_t addr);
typedef void(*ptr_Read32) (uint32_t* value, uint32_t pc, uint32_t addr);
typedef void(*ptr_Read64) (uint64_t* value, uint32_t pc, uint32_t addr);
typedef void(*ptr_Write8) (uint8_t*  value, uint32_t pc, uint32_t addr);
typedef void(*ptr_Write16)(uint16_t* value, uint32_t pc, uint32_t addr);
typedef void(*ptr_Write32)(uint32_t* value, uint32_t pc, uint32_t addr);
typedef void(*ptr_Write64)(uint64_t* value, uint32_t pc, uint32_t addr);
/* Optional plugin functions: the core calls them when the plugin exports them */
typedef void(*ptr_StateLoaded)(void);  /* after a save state was loaded */
typedef int (*ptr_StateSaving)(void);  /* before a save state is made: 0 puts it off until the next interrupt (the
                                          plugin is in the middle of something it can't save), 1 lets it be made */
typedef void(*ptr_Reset)(int hard);    /* when the game is reset (hard = 1 for a hard reset) */
/* Optional, for the new netplay. Plugins that change the game need the same settings and state on every machine.
 * Every machine calls NetplayStart(is_host) before InitiateExecution. The host's plugin then starts as usual, and
 * NetplayExport() hands its settings and state to the core, which sends them to the guests (the data stays valid until
 * the next call or RomClosed). Each guest's plugin gets them with NetplayImport() before its InitiateExecution, and uses
 * them instead of its own files. The format is the plugin's own. Both return 0 on success. The plugin stays in netplay
 * mode until RomClosed (it shouldn't do anything that depends on the machine, like talking to a server, except as the
 * speaker, see NetplayFrame). */
typedef void(*ptr_NetplayStart)(int is_host);
typedef int (*ptr_NetplayExport)(const void** data, uint32_t* size);
typedef int (*ptr_NetplayImport)(const void* data, uint32_t size);
/* Optional, for the new netplay (each is used when the plugin exports it):
 *
 * NetplayFrame(frame, final, flags): at the start of every frame, also before InitiateExecution (frame 0), and while
 * frames are re-run after a rollback: once for each frame run, the first frame run again included (right after its
 * NetplayLoadState). The frame's number is the one Frame() gets at its end (during the new netplay, the session's frame
 * number, which goes back for frames run again). Frames before `final` are final: their input won't change. A plugin
 * does what reaches outside the game (sending to a server, writing files, logging) only for final frames: it waits for them, and
 * forgets what it was going to do for frames it goes back from (NetplayLoadState). A final frame can still be run again
 * (a rollback goes back to the newest frame the core saved before the wrong guess, which can be before `final`), but
 * with the same input it does the same, so what it does outside the game was done already and is skipped.
 * flags: NETPLAY_FRAME_RERUN when the frame is run again after a rollback; NETPLAY_FRAME_SPEAKER when this machine is
 * the one that talks to services outside the session (one machine at a time: the host in RMG's new netplay).
 *
 * NetplayMessage(data, size): a message a plugin broadcast with the core's ExecutionNetplaySendMessage, on every
 * machine at the end of the same frame (and again when that frame is re-run). That frame is the one the session decides
 * (see get_message in m64p_types.h): the new netplay's session hands a message over MessageDelayFrames after the frame
 * it put the message into, so that frame has arrived everywhere by then, also in Rollback mode. Plugins apply what a
 * server tells them here, rather than when it arrives, so every game applies it at the same point.
 *
 * Rollback: the core saves the plugin's state with each state it saves for rollback (NetplaySaveState, at the start of
 * the frame), restores it when it goes back to that frame (NetplayLoadState, the frames after it are then run again)
 * and drops it when it can't go back that far anymore (NetplayDropState). When it goes back, the core also drops the
 * saves of the frames after that one (NetplayDropState for each), so NetplayLoadState needn't. NetplayStateSize() is the size of the buffer
 * the core keeps for the plugin with each saved frame (it can be 0, for a plugin that keeps its saves itself). A game
 * with an execution plugin can only use Rollback mode if the plugin exports these four.
 *
 * NetplayStateHash(): optional, for the core's rollback self-test (RMG_NETPLAY_NEW_ROLLBACK_TEST): a hash of the
 * plugin's state, which must be the same at the end of a frame run again as at the end of its first run. */
#define NETPLAY_FRAME_RERUN   1
#define NETPLAY_FRAME_SPEAKER 2
typedef void(*ptr_NetplayFrame)(uint32_t frame, uint32_t final_frames, int flags);
typedef void(*ptr_NetplayMessage)(const void* data, uint32_t size);
typedef uint32_t(*ptr_NetplayStateSize)(void);
typedef void(*ptr_NetplaySaveState)(uint32_t frame, void* data);
typedef void(*ptr_NetplayLoadState)(uint32_t frame, const void* data);
typedef void(*ptr_NetplayDropState)(uint32_t frame);
typedef uint64_t(*ptr_NetplayStateHash)(void);
/* Exported by the core (find it with the core's library handle): broadcasts a message to every machine's plugin during
 * the new netplay (see NetplayMessage), up to 32 KB. Returns 0 if it can't (no netplay game, or the message is too
 * big). Call it from the emulation thread (any plugin callback). */
typedef int(*ptr_ExecutionNetplaySendMessage)(const void* data, uint32_t size);
/* Exported by the core (find it with the core's library handle): tells the cached interpreter and the dynarec that the
 * plugin changed code at the address, so they don't keep running the old instructions. The address is a virtual
 * address in KSEG0 or KSEG1 (like 0x80001234); both are invalidated. */
typedef void(*ptr_ExecutionInvalidateCode)(uint32_t addr, uint32_t size);
#if defined(M64P_PLUGIN_PROTOTYPES)
EXPORT void CALL InitiateExecution(EXECUTION_INFO ExecutionInfo);
EXPORT void CALL Input(int port, BUTTONS* buttons);
EXPORT void CALL Frame(uint32_t frame);
EXPORT void CALL Execute(uint32_t pc);
EXPORT void CALL ExecuteDone(uint32_t pc);
EXPORT void CALL Read8  (uint8_t*  value, uint32_t pc, uint32_t addr);
EXPORT void CALL Read16 (uint16_t* value, uint32_t pc, uint32_t addr);
EXPORT void CALL Read32 (uint32_t* value, uint32_t pc, uint32_t addr);
EXPORT void CALL Read64 (uint64_t* value, uint32_t pc, uint32_t addr);
EXPORT void CALL Write8 (uint8_t*  value, uint32_t pc, uint32_t addr);
EXPORT void CALL Write16(uint16_t* value, uint32_t pc, uint32_t addr);
EXPORT void CALL Write32(uint32_t* value, uint32_t pc, uint32_t addr);
EXPORT void CALL Write64(uint64_t* value, uint32_t pc, uint32_t addr);
EXPORT void CALL StateLoaded(void);
EXPORT void CALL Reset(int hard);
EXPORT void CALL NetplayStart(int is_host);
EXPORT int CALL NetplayExport(const void** data, uint32_t* size);
EXPORT int CALL NetplayImport(const void* data, uint32_t size);
EXPORT void CALL NetplayFrame(uint32_t frame, uint32_t final_frames, int flags);
EXPORT void CALL NetplayMessage(const void* data, uint32_t size);
EXPORT uint32_t CALL NetplayStateSize(void);
EXPORT void CALL NetplaySaveState(uint32_t frame, void* data);
EXPORT void CALL NetplayLoadState(uint32_t frame, const void* data);
EXPORT void CALL NetplayDropState(uint32_t frame);
EXPORT uint64_t CALL NetplayStateHash(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* M64P_PLUGIN_H */


