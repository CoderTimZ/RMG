/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-core - m64p_types.h                                       *
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

#if !defined(M64P_TYPES_H)
#define M64P_TYPES_H

/* ----------------------------------------- */
/* Platform-specific stuff                   */
/* ----------------------------------------- */

/* necessary headers */
#include <stdint.h>
#if defined(WIN32)
  #include <windows.h>
#endif

/* DLL handles and function declaration specifiers */
#if defined(WIN32)
  #define IMPORT extern "C" __declspec(dllimport)
  #define EXPORT __declspec(dllexport)
  #define CALL   __cdecl
  typedef HMODULE m64p_dynlib_handle;
#else
  #define IMPORT extern "C"
  #define EXPORT __attribute__((visibility("default")))
  #define CALL
  typedef void * m64p_dynlib_handle;
#endif

/* ----------------------------------------- */
/* Structures and Types for Core library API */
/* ----------------------------------------- */

typedef void * m64p_handle;

/* Generic function pointer returned from osal_dynlib_getproc (and the like)
 * Don't use it directly, cast to proper type before using it.
 */
typedef void (*m64p_function)(void);

typedef void (*m64p_frame_callback)(unsigned int FrameIndex);
typedef void (*m64p_input_callback)(void);
typedef void (*m64p_audio_callback)(void);
typedef void (*m64p_vi_callback)(void);

typedef enum {
  M64TYPE_INT = 1,
  M64TYPE_FLOAT,
  M64TYPE_BOOL,
  M64TYPE_STRING
} m64p_type;

typedef enum {
  M64MSG_ERROR = 1,
  M64MSG_WARNING,
  M64MSG_INFO,
  M64MSG_STATUS,
  M64MSG_VERBOSE
} m64p_msg_level;

typedef enum {
  M64ERR_SUCCESS = 0,
  M64ERR_NOT_INIT,        /* Function is disallowed before InitMupen64Plus() is called */
  M64ERR_ALREADY_INIT,    /* InitMupen64Plus() was called twice */
  M64ERR_INCOMPATIBLE,    /* API versions between components are incompatible */
  M64ERR_INPUT_ASSERT,    /* Invalid parameters for function call, such as ParamValue=NULL for GetCoreParameter() */
  M64ERR_INPUT_INVALID,   /* Invalid input data, such as ParamValue="maybe" for SetCoreParameter() to set a BOOL-type value */
  M64ERR_INPUT_NOT_FOUND, /* The input parameter(s) specified a particular item which was not found */
  M64ERR_NO_MEMORY,       /* Memory allocation failed */
  M64ERR_FILES,           /* Error opening, creating, reading, or writing to a file */
  M64ERR_INTERNAL,        /* Internal error (bug) */
  M64ERR_INVALID_STATE,   /* Current program state does not allow operation */
  M64ERR_PLUGIN_FAIL,     /* A plugin function returned a fatal error */
  M64ERR_SYSTEM_FAIL,     /* A system function call, such as an SDL or file operation, failed */
  M64ERR_UNSUPPORTED,     /* Function call is not supported (ie, core not built with debugger) */
  M64ERR_WRONG_TYPE       /* A given input type parameter cannot be used for desired operation */
} m64p_error;

typedef enum {
  M64CAPS_DYNAREC = 1,
  M64CAPS_DEBUGGER = 2,
  M64CAPS_CORE_COMPARE = 4
} m64p_core_caps;

typedef enum {
  M64PLUGIN_NULL = 0,
  M64PLUGIN_RSP = 1,
  M64PLUGIN_GFX,
  M64PLUGIN_AUDIO,
  M64PLUGIN_INPUT,
  M64PLUGIN_EXECUTION,
  M64PLUGIN_CORE
} m64p_plugin_type;

typedef enum {
  M64EMU_STOPPED = 1,
  M64EMU_RUNNING,
  M64EMU_PAUSED
} m64p_emu_state;

typedef enum {
  M64VIDEO_NONE = 1,
  M64VIDEO_WINDOWED,
  M64VIDEO_FULLSCREEN
} m64p_video_mode;

typedef enum {
  M64VIDEOFLAG_SUPPORT_RESIZING = 1
} m64p_video_flags;

typedef enum {
  M64CORE_EMU_STATE = 1,
  M64CORE_VIDEO_MODE,
  M64CORE_SAVESTATE_SLOT,
  M64CORE_SPEED_FACTOR,
  M64CORE_SPEED_LIMITER,
  M64CORE_VIDEO_SIZE,
  M64CORE_AUDIO_VOLUME,
  M64CORE_AUDIO_MUTE,
  M64CORE_INPUT_GAMESHARK,
  M64CORE_STATE_LOADCOMPLETE,
  M64CORE_STATE_SAVECOMPLETE,
  M64CORE_SCREENSHOT_CAPTURED,
  M64CORE_SPEED_UDPATE
} m64p_core_param;

typedef enum {
  M64CMD_NOP = 0,
  M64CMD_ROM_OPEN,
  M64CMD_ROM_CLOSE,
  M64CMD_ROM_GET_HEADER,
  M64CMD_ROM_GET_SETTINGS,
  M64CMD_EXECUTE,
  M64CMD_STOP,
  M64CMD_PAUSE,
  M64CMD_RESUME,
  M64CMD_CORE_STATE_QUERY,
  M64CMD_STATE_LOAD,
  M64CMD_STATE_SAVE,
  M64CMD_STATE_SET_SLOT,
  M64CMD_SEND_SDL_KEYDOWN,
  M64CMD_SEND_SDL_KEYUP,
  M64CMD_SET_FRAME_CALLBACK,
  M64CMD_TAKE_NEXT_SCREENSHOT,
  M64CMD_CORE_STATE_SET,
  M64CMD_READ_SCREEN,
  M64CMD_RESET,
  M64CMD_ADVANCE_FRAME,
  M64CMD_SET_MEDIA_LOADER,
  M64CMD_NETPLAY_INIT,
  M64CMD_NETPLAY_CONTROL_PLAYER,
  M64CMD_NETPLAY_GET_VERSION,
  M64CMD_NETPLAY_CLOSE,
  M64CMD_PIF_OPEN,
  M64CMD_ROM_SET_SETTINGS,
  M64CMD_DISK_OPEN,
  M64CMD_DISK_CLOSE,
  M64CMD_NETPLAY_NEW_SET_CALLBACKS
} m64p_command;

typedef struct {
  uint32_t address;
  int      value;
} m64p_cheat_code;

typedef struct {
  /* Frontend-defined callback data. */
  void* cb_data;

  /* Allow the frontend to specify the GB cart ROM file to load
   * cb_data: points to frontend-defined callback data.
   * controller_num: (0-3) tell the frontend which controller is about to load a GB cart
   * Returns a NULL-terminated string owned by the core specifying the GB cart ROM filename to load.
   * Empty or NULL string results in no GB cart being loaded (eg. empty transferpak).
   */
  char* (*get_gb_cart_rom)(void* cb_data, int controller_num);

  /* Allow the frontend to specify the GB cart RAM file to load
   * cb_data: points to frontend-defined callback data.
   * controller_num: (0-3) tell the frontend which controller is about to load a GB cart
   * Returns a NULL-terminated string owned by the core specifying the GB cart RAM filename to load
   * Empty or NULL string results in the core generating a default save file with empty content.
   */
  char* (*get_gb_cart_ram)(void* cb_data, int controller_num);

  /* Allow the frontend to know what DD IPL ROM region file to load
   * cb_data: points to frontend-defined callback data.
   * region: a region from m64p_system_type
   */
  void (*set_dd_rom_region)(void* cb_data, uint8_t region);

  /* Allow the frontend to specify the DD IPL ROM file to load
   * cb_data: points to frontend-defined callback data.
   * Returns a NULL-terminated string owned by the core specifying the DD IPL ROM filename to load
   * Empty or NULL string results in disabled 64DD.
   */
  char* (*get_dd_rom)(void* cb_data);

  /* Allow the frontend to specify the DD disk file to load
   * cb_data: points to frontend-defined callback data.
   * Returns a NULL-terminated string owned by the core specifying the DD disk filename to load
   * Empty or NULL string results in no DD disk being loaded (eg. empty disk drive).
   */
  char* (*get_dd_disk)(void* cb_data);
} m64p_media_loader;

/* ----------------------------------------- */
/* New netplay                               */
/* ----------------------------------------- */

/* The frontend runs the session and the network. The core only asks it for
 * the data that every player has to share (sync), tells it which ports are
 * plugged in (configure_ports), hands it this machine's controllers once per
 * frame (sample) and asks it for the input on each port (get_input).
 *
 * A frame is one VI. Frame numbers start at 0 when emulation starts.
 * sample() is called exactly once per frame, at the game's first controller
 * read during that frame or at the end of the frame if the game doesn't read
 * the controllers. Frame numbers passed to sample() only ever increase, even
 * when the core re-runs earlier frames (rollback).
 *
 * Rollback: when the frontend provides rollback() and snapshot(), it can
 * answer get_input() for a frame whose input hasn't arrived with a guess.
 * The core saves the machine's state at the start of frames the frontend
 * asks for (snapshot), and when a guess turns out wrong, the frontend names
 * one of those frames, the core goes back to it and re-runs the frames from
 * there to where it was, quickly and without showing or playing them.
 *
 * Messages: an execution plugin can broadcast a message (bytes of its own
 * format) to every machine's plugin with send_message(). The session puts
 * it into an input frame, and at the end of a later frame of the session's
 * choosing (the same everywhere) every machine's core hands it to its
 * plugin (get_message), so every game applies it at the same point. */

#define M64P_NETPLAY_NEW_API_VERSION 0x010600

/* the largest message send_message() takes */
#define M64P_NETPLAY_NEW_MAX_MESSAGE 32768

/* frame_end's flags and return value */
#define M64P_NETPLAY_NEW_RERUN        1 /* (flag) the frame was re-run after a rollback */
#define M64P_NETPLAY_NEW_SKIP_LIMITER 1 /* (returned) don't wait for the speed limiter after this frame */
#define M64P_NETPLAY_NEW_SLOW_DOWN    2 /* (returned) this machine runs ahead of the others; hold it back a little */

typedef enum {
  M64P_NETPLAY_NEW_SYNC_SETTINGS = 1, /* m64p_netplay_new_settings */
  M64P_NETPLAY_NEW_SYNC_CLOCK,        /* int64_t: seconds since the epoch when emulation started */
  M64P_NETPLAY_NEW_SYNC_MEMPAK,       /* the four memory paks */
  M64P_NETPLAY_NEW_SYNC_EEPROM,
  M64P_NETPLAY_NEW_SYNC_FLASHRAM,
  M64P_NETPLAY_NEW_SYNC_SRAM,
  M64P_NETPLAY_NEW_SYNC_EXECUTION_SIZE, /* uint32_t: the size of the next one */
  M64P_NETPLAY_NEW_SYNC_EXECUTION       /* the execution plugin's settings and state (see NetplayExport) */
} m64p_netplay_new_sync;

/* Settings that change how the game runs, taken from the host */
typedef struct {
  uint32_t count_per_op;
  uint32_t count_per_op_denom_pot;
  uint32_t disable_extra_mem;
  int32_t  si_dma_duration;
  uint32_t emumode;
  int32_t  no_compiled_jump;
  uint32_t save_type;
  uint32_t ai_dma_modifier;
  uint32_t rumble;
  uint32_t transferpak;
  uint32_t mempak;
  uint32_t biopak;
} m64p_netplay_new_settings;

/* A controller's input on one frame */
typedef struct {
  uint32_t buttons;     /* a BUTTONS value */
  int32_t pak;          /* -1, or the pak the controller switches to on this frame
                         * (PLUGIN_NONE, PLUGIN_MEMPAK, PLUGIN_RUMBLE_PAK, ...) */
} m64p_netplay_new_input;

typedef struct {
  int present;          /* whether a controller is plugged into the port */
  int plugin;           /* the pak in the controller: PLUGIN_NONE, PLUGIN_MEMPAK, ... */
  int local_controller; /* the controller on this machine that drives the port (for rumble), or -1 */
} m64p_netplay_new_port;

typedef struct {
  /* must be M64P_NETPLAY_NEW_API_VERSION */
  unsigned int api_version;
  /* passed back to every callback */
  void* context;
  /* whether this machine hosts the session; only the host writes save files */
  int is_host;

  /* On the host, shares data with everyone. Elsewhere, replaces data with the
   * host's. Returns 0 if the session ended. */
  int (*sync)(void* context, m64p_netplay_new_sync kind, void* data, unsigned int size);

  /* local: this machine's controllers, as the input plugin set them up.
   * ports: filled in with the session's controller ports.
   * Returns 0 if the session ended. */
  int (*configure_ports)(void* context, const m64p_netplay_new_port local[4], m64p_netplay_new_port ports[4]);

  /* This machine's four controllers at the given frame (no buttons for
   * controllers that aren't plugged in). A pak switch is reported once,
   * on the frame the player made it. */
  void (*sample)(void* context, unsigned int frame, const m64p_netplay_new_input local[4]);

  /* The input on a port at a frame. May block until it's known. Every
   * machine sees a pak switch on the same frame and makes it then.
   * Returns 0 if the session ended. */
  int (*get_input)(void* context, unsigned int frame, int port, m64p_netplay_new_input* input);

  /* Called at the end of each frame, with how long this machine spent
   * emulating it in microseconds (not counting the time it waited in
   * get_input, or the speed limiter's sleep), how long a frame lasts at
   * full speed (1/60 s, or 1/50 s for PAL) and M64P_NETPLAY_NEW_RERUN if
   * the frame was re-run after a rollback. Returns M64P_NETPLAY_NEW_SKIP_LIMITER
   * (when this machine needs to catch up) and M64P_NETPLAY_NEW_SLOW_DOWN, or
   * -1 if the session ended.
   * The time is measured from the start of the last live frame (re-run
   * frames don't restart it), so a live frame's busy_us includes the
   * restore and the frames re-run before it, and a re-run frame's includes
   * the ones re-run before it. That's on purpose: the automatic lag then
   * covers what rollbacks cost this machine. */
  int (*frame_end)(void* context, unsigned int frame, unsigned int busy_us, unsigned int frame_us, int flags);

  /* A hash of the emulated machine for the end of a frame (RDRAM, the CP0
   * registers and the RSP's memory), for noticing when players' games
   * drift apart. The hash is taken during frame taken_in (at the end of
   * the frame, or at a later full sync of the RDP), so it depends on the
   * input up to that frame. Called before frame_end. May be NULL. */
  void (*frame_hash)(void* context, unsigned int frame, uint64_t hash, unsigned int taken_in);

  /* Rollback (may be NULL, together with snapshot). Called at every frame
   * boundary, also while re-running frames, with the frame that starts
   * next. Sets *confirmed to the first frame whose input isn't final yet
   * (everything before it is), and *snapshot to 1 if the core should save
   * the machine's state at the start of next_frame. Returns -1 to go on,
   * or a frame to go back to: one the core saved (see snapshot) and hasn't
   * dropped. The frontend forgets the core's saves after that frame. */
  int (*rollback)(void* context, unsigned int next_frame, unsigned int* confirmed, int* snapshot);

  /* The core saved the machine's state at the start of frame (kept = 1),
   * or dropped that save (kept = 0) */
  void (*snapshot)(void* context, unsigned int frame, int kept);

  /* Messages (all four may be NULL).
   *
   * Broadcasts a message from this machine's execution plugin, up to
   * M64P_NETPLAY_NEW_MAX_MESSAGE bytes. Returns 0 if it can't be sent. */
  int (*send_message)(void* context, const void* data, unsigned int size);

  /* The messages to hand to the plugin at the end of a frame, in order
   * (index 0, 1, ...). Called at the end of the frame, after get_input.
   * Returns the message's bytes, valid until the next call, and sets *size;
   * NULL when there are no more. The session decides which messages apply at
   * which frame (the same everywhere): the new netplay's session applies a
   * message some frames after the input frame that carries it, so that frame
   * has arrived everywhere by then, also in Rollback mode. May block like
   * get_input. */
  const void* (*get_message)(void* context, unsigned int frame, unsigned int index, unsigned int* size);

  /* Whether this machine talks to services outside the session (like a
   * bingo site) for the execution plugin. One machine does at a time; the
   * others' plugins only take part through messages. Asked at every frame
   * boundary, so it can move to another machine during a game. */
  int (*speaker)(void* context);
} m64p_netplay_new_callbacks;

/* ----------------------------------------- */
/* Structures to hold ROM image information  */
/* ----------------------------------------- */

typedef enum
{
    SYSTEM_NTSC = 0,
    SYSTEM_PAL,
    SYSTEM_MPAL
} m64p_system_type;

typedef enum
{
    SAVETYPE_EEPROM_4K       = 0,
    SAVETYPE_EEPROM_4KB      = 0, // Preserve inaccurate/misleading name
    SAVETYPE_EEPROM_16K      = 1,
    SAVETYPE_EEPROM_16KB     = 1, // Preserve inaccurate/misleading name
    SAVETYPE_SRAM            = 2,
    SAVETYPE_FLASH_RAM       = 3,
    SAVETYPE_CONTROLLER_PAK  = 4,
    SAVETYPE_CONTROLLER_PACK = 4, // Preserve inaccurate/off-brand name
    SAVETYPE_NONE            = 5,
} m64p_rom_save_type;

typedef enum
{
    DDREGION_JAPAN   = 0,
    DDREGION_US      = 1,
    DDREGION_DEV     = 2,
    DDREGION_UNKNOWN = 3,
} m64p_disk_region;

typedef struct
{
   uint8_t  init_PI_BSB_DOM1_LAT_REG;  /* 0x00 */
   uint8_t  init_PI_BSB_DOM1_PGS_REG;  /* 0x01 */
   uint8_t  init_PI_BSB_DOM1_PWD_REG;  /* 0x02 */
   uint8_t  init_PI_BSB_DOM1_PGS_REG2; /* 0x03 */
   uint32_t ClockRate;                 /* 0x04 */
   uint32_t PC;                        /* 0x08 */
   uint32_t Release;                   /* 0x0C */
   uint32_t CRC1;                      /* 0x10 */
   uint32_t CRC2;                      /* 0x14 */
   uint32_t Unknown[2];                /* 0x18 */
   uint8_t  Name[20];                  /* 0x20 */
   uint32_t unknown;                   /* 0x34 */
   uint32_t Manufacturer_ID;           /* 0x38 */
   uint16_t Cartridge_ID;              /* 0x3C - Game serial number  */
   uint8_t  Country_code;              /* 0x3E */
   uint8_t  Version;                   /* 0x3F */
} m64p_rom_header;

typedef struct
{
   char goodname[256];
   char MD5[33];
   unsigned char savetype;
   unsigned char status;  /* Rom status on a scale from 0-5. */
   unsigned char players; /* Local players 0-4, 2/3/4 way Netplay indicated by 5/6/7. */
   unsigned char rumble;  /* 0 - No, 1 - Yes boolean for rumble support. */
   unsigned char transferpak; /* 0 - No, 1 - Yes boolean for transfer pak support. */
   unsigned char mempak; /* 0 - No, 1 - Yes boolean for memory pak support. */
   unsigned char biopak; /* 0 - No, 1 - Yes boolean for bio pak support. */
   unsigned char disableextramem; /* 0 - No, 1 - Yes boolean for disabling 4MB expansion RAM pack */
   unsigned int countperop; /* Number of CPU cycles per instruction. */
   unsigned int sidmaduration; /* Default SI DMA duration */
   unsigned int aidmamodifier; /* Percentage modifier for AI DMA duration */
} m64p_rom_settings;

/* ----------------------------------------- */
/* Structures and Types for the Debugger     */
/* ----------------------------------------- */

typedef enum {
  M64P_DBG_RUN_STATE = 1,
  M64P_DBG_PREVIOUS_PC,
  M64P_DBG_NUM_BREAKPOINTS,
  M64P_DBG_CPU_DYNACORE,
  M64P_DBG_CPU_NEXT_INTERRUPT
} m64p_dbg_state;

typedef enum {
  M64P_DBG_RUNSTATE_PAUSED = 0,
  M64P_DBG_RUNSTATE_STEPPING,
  M64P_DBG_RUNSTATE_RUNNING
} m64p_dbg_runstate;

typedef enum {
  M64P_DBG_MEM_TYPE = 1,
  M64P_DBG_MEM_FLAGS,
  M64P_DBG_MEM_HAS_RECOMPILED,
  M64P_DBG_MEM_NUM_RECOMPILED,
  M64P_DBG_RECOMP_OPCODE = 16,
  M64P_DBG_RECOMP_ARGS,
  M64P_DBG_RECOMP_ADDR
} m64p_dbg_mem_info;

typedef enum {
  M64P_MEM_NOMEM = 0,
  M64P_MEM_NOTHING,
  M64P_MEM_RDRAM,
  M64P_MEM_RDRAMREG,
  M64P_MEM_RSPMEM,
  M64P_MEM_RSPREG,
  M64P_MEM_RSP,
  M64P_MEM_DP,
  M64P_MEM_DPS,
  M64P_MEM_VI,
  M64P_MEM_AI,
  M64P_MEM_PI,
  M64P_MEM_RI,
  M64P_MEM_SI,
  M64P_MEM_FLASHRAMSTAT,
  M64P_MEM_ROM,
  M64P_MEM_PIF,
  M64P_MEM_MI,
  M64P_MEM_BREAKPOINT
} m64p_dbg_mem_type;

typedef enum {
  M64P_MEM_FLAG_READABLE = 0x01,
  M64P_MEM_FLAG_WRITABLE = 0x02,
  M64P_MEM_FLAG_READABLE_EMUONLY = 0x04,  /* the EMUONLY flags signify that emulated code can read/write here, but debugger cannot */
  M64P_MEM_FLAG_WRITABLE_EMUONLY = 0x08
} m64p_dbg_mem_flags;

typedef enum {
  M64P_DBG_PTR_RDRAM = 1,
  M64P_DBG_PTR_PI_REG,
  M64P_DBG_PTR_SI_REG,
  M64P_DBG_PTR_VI_REG,
  M64P_DBG_PTR_RI_REG,
  M64P_DBG_PTR_AI_REG
} m64p_dbg_memptr_type;

typedef enum {
  M64P_CPU_PC = 1,
  M64P_CPU_REG_REG,
  M64P_CPU_REG_HI,
  M64P_CPU_REG_LO,
  M64P_CPU_REG_COP0,
  M64P_CPU_REG_COP1_DOUBLE_PTR,
  M64P_CPU_REG_COP1_SIMPLE_PTR,
  M64P_CPU_REG_COP1_FGR_64,
  M64P_CPU_TLB
} m64p_dbg_cpu_data;

typedef enum {
  M64P_BKP_CMD_ADD_ADDR = 1,
  M64P_BKP_CMD_ADD_STRUCT,
  M64P_BKP_CMD_REPLACE,
  M64P_BKP_CMD_REMOVE_ADDR,
  M64P_BKP_CMD_REMOVE_IDX,
  M64P_BKP_CMD_ENABLE,
  M64P_BKP_CMD_DISABLE,
  M64P_BKP_CMD_CHECK
} m64p_dbg_bkp_command;

#define M64P_MEM_INVALID        0xFFFFFFFF  /* invalid memory read will return this */

#define BREAKPOINTS_MAX_NUMBER  128

typedef enum {
  M64P_BKP_FLAG_ENABLED = 0x01,
  M64P_BKP_FLAG_READ = 0x02,
  M64P_BKP_FLAG_WRITE = 0x04,
  M64P_BKP_FLAG_EXEC = 0x08,
  M64P_BKP_FLAG_LOG = 0x10 /* Log to the console when this breakpoint hits */
} m64p_dbg_bkp_flags;

#define BPT_CHECK_FLAG(a, b)  ((a.flags & b) == b)
#define BPT_SET_FLAG(a, b)    a.flags = (a.flags | b);
#define BPT_CLEAR_FLAG(a, b)  a.flags = (a.flags & (~b));
#define BPT_TOGGLE_FLAG(a, b) a.flags = (a.flags ^ b);

typedef struct {
  uint32_t     address;
  uint32_t     endaddr;
  unsigned int flags;
} m64p_breakpoint;

/* ------------------------------------------------- */
/* Structures and Types for Core Video Extension API */
/* ------------------------------------------------- */

typedef struct {
  unsigned int uiWidth;
  unsigned int uiHeight;
} m64p_2d_size;

typedef enum {
  M64P_GL_DOUBLEBUFFER = 1,
  M64P_GL_BUFFER_SIZE,
  M64P_GL_DEPTH_SIZE,
  M64P_GL_RED_SIZE,
  M64P_GL_GREEN_SIZE,
  M64P_GL_BLUE_SIZE,
  M64P_GL_ALPHA_SIZE,
  M64P_GL_SWAP_CONTROL,
  M64P_GL_MULTISAMPLEBUFFERS,
  M64P_GL_MULTISAMPLESAMPLES,
  M64P_GL_CONTEXT_MAJOR_VERSION,
  M64P_GL_CONTEXT_MINOR_VERSION,
  M64P_GL_CONTEXT_PROFILE_MASK
} m64p_GLattr;

typedef enum {
  M64P_GL_CONTEXT_PROFILE_CORE,
  M64P_GL_CONTEXT_PROFILE_COMPATIBILITY,
  M64P_GL_CONTEXT_PROFILE_ES
} m64p_GLContextType;

typedef enum {
  M64P_RENDER_OPENGL = 0,
  M64P_RENDER_VULKAN
} m64p_render_mode;

typedef struct {
  unsigned int Functions;
  m64p_error    (*VidExtFuncInit)(void);
  m64p_error    (*VidExtFuncQuit)(void);
  m64p_error    (*VidExtFuncListModes)(m64p_2d_size *, int *);
  m64p_error    (*VidExtFuncListRates)(m64p_2d_size, int *, int *);
  m64p_error    (*VidExtFuncSetMode)(int, int, int, int, int);
  m64p_error    (*VidExtFuncSetModeWithRate)(int, int, int, int, int, int);
  m64p_function (*VidExtFuncGLGetProc)(const char*);
  m64p_error    (*VidExtFuncGLSetAttr)(m64p_GLattr, int);
  m64p_error    (*VidExtFuncGLGetAttr)(m64p_GLattr, int *);
  m64p_error    (*VidExtFuncGLSwapBuf)(void);
  m64p_error    (*VidExtFuncSetCaption)(const char *);
  m64p_error    (*VidExtFuncToggleFS)(void);
  m64p_error    (*VidExtFuncResizeWindow)(int, int);
  uint32_t      (*VidExtFuncGLGetDefaultFramebuffer)(void);
  m64p_error    (*VidExtFuncInitWithRenderMode)(m64p_render_mode);
  m64p_error    (*VidExtFuncVKGetSurface)(void**, void*);
  m64p_error    (*VidExtFuncVKGetInstanceExtensions)(const char**[], uint32_t*);
} m64p_video_extension_functions;

#endif /* define M64P_TYPES_H */

