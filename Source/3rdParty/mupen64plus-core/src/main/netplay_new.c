/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - netplay_new.c                                           *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
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

#include "netplay_new.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define M64P_CORE_PROTOTYPES 1
#include "api/callbacks.h"
#include "api/m64p_plugin.h"
#include "backends/file_storage.h"
#include "backends/plugins_compat/plugins_compat.h"
#include "device/device.h"
#include "device/r4300/interrupt.h"
#include "main/main.h"
#include "main/savestates.h"
#include "plugin/plugin.h"

#include <stdlib.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#ifdef USE_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif

/* The reserved BUTTONS bits that the core uses for its pak and GB cart
 * switching hotkeys. They never leave this machine: pak switches travel
 * in the input's own pak field. */
enum { LOCAL_ONLY_BUTTONS = 0xC000 };

static int l_active = 0;
static m64p_netplay_new_callbacks l_callbacks;

/* the session ended (a callback failed); emulation stops at the next chance */
static int l_ended = 0;

/* the frame being emulated: the number of VIs since emulation started */
static unsigned int l_frame = 0;

/* The state hash can't always be taken at the end of a frame: the RDP is
 * often still drawing then, and a GPU plugin like Parallel writes the frame
 * buffer and the Z buffer into RDRAM while the CPU runs, so the hash would
 * catch it partway through, differently on every machine. When the RDP has
 * commands it hasn't finished with a full sync, the frame's hash waits for
 * the next full sync, which is where the plugin waits for the GPU, so RDRAM
 * is complete, and which happens at the same point of the game everywhere.
 * A game at 30 fps full syncs every other frame, so the two frames share a
 * hash. */
#define MAX_PENDING_HASHES 8
static unsigned int l_hash_first = 0; /* the first frame waiting for its hash */
static unsigned int l_hash_count = 0; /* the number of frames waiting */
static int l_rdp_busy = 0;            /* commands sent to the RDP since its last full sync */

/* every frame below this one has been sampled. This is deliberately not
 * part of the emulation state: if emulation ever goes back to an earlier
 * frame (rollback), those frames must not be sampled again. */
static unsigned int l_next_sample_frame = 0;

/* this machine's controllers, as the input plugin set them up */
static m64p_netplay_new_port l_local[4];

/* the session's ports */
static m64p_netplay_new_port l_ports[4];

/* Pak switches: every frame's switch is fetched exactly once per port, at
 * the game's first read of the port in that frame, or at the end of the
 * frame if the game didn't read it, and made at the port's next read. That
 * read happens at the same point of the game on every machine. */
static int l_pending_pak[4];
static unsigned int l_events_through[4]; /* frames below this have been fetched */

/* the session clock */
static int64_t l_start_time = 0;

/* How long each frame takes to emulate, for the session's automatic lag: a
 * frame is timed from the end of the speed limiter's sleep to its end, and
 * the time spent waiting for input in get_input doesn't count (a long wait
 * mustn't look like a slow machine). */
static uint64_t l_frame_start = 0;
static uint64_t l_frame_waited = 0;

/* Controls[] as the core uses it, while the input plugin sees its own controller */
static int l_saved_present[4];
static int l_saved_plugin[4];

/* Rollback.
 *
 * When the session guesses input that hasn't arrived, it asks for the
 * machine's state at the start of that frame to be saved (a snapshot).
 * When a guess turns out wrong, the core goes back to the snapshot of a
 * frame at or before the wrong one and re-runs the frames from there to
 * where it was, without showing, playing or rumbling them.
 *
 * A snapshot is the m64p savestate in memory (without the TLB lookup
 * tables), plus what savestates don't hold: the save files, pak switching,
 * the cheats' counter, the AI's DMA addresses, this file's frame state,
 * the video plugin's RDP state outside RDRAM (its hidden bits, TMEM and its
 * settings: a frame drawn again uses them), the RSP plugin's state between
 * tasks (an LLE RSP's registers carry over from one task to the next, and a
 * microcode can read what the last task left there; the HLE RSP saves its
 * own state and its fallback's) and the execution plugin's state
 * (NetplaySaveState).
 * It's taken at the end of gen_interrupt, where savestates are saved, once
 * the frame has started: at the VI's interrupt, or at a later one if an RSP
 * task was running or the RDP was still in the middle of its commands then.
 * A frame whose controllers were read before that gets no snapshot (a
 * rollback goes further back instead). */
#define MAX_SNAPSHOTS 12
#define MAX_TRACKED 8
enum { STORAGE_EEP, STORAGE_SRA, STORAGE_FLA, STORAGE_MPK, STORAGE_COUNT };

struct tracked_state
{
    void* data;
    size_t size;
};

struct snapshot
{
    int kept;
    unsigned int frame;
    unsigned char* state;
    uint8_t* saves[STORAGE_COUNT];
    unsigned int save_generation[STORAGE_COUNT];
    uint8_t* tracked[MAX_TRACKED];
    /* the video plugin's RDP state outside RDRAM */
    uint8_t* rdp_state;
    /* the RSP plugin's state between tasks */
    uint8_t* rsp_state;
    /* the execution plugin's state (none if it keeps its saves itself) */
    uint8_t* exec_state;
    /* (self-test: hashes of the machine when the snapshot was taken, see
     * selftest_hash_machine; NULL when the self-test is off) */
    uint32_t* check;
    uint32_t ai_address[2];
    unsigned int hash_first;
    unsigned int hash_count;
    int rdp_busy;
    int pending_pak[4];
    unsigned int events_through[4];
};

/* whether this game can roll back (the frontend supports it, and the
 * execution plugin, if any, saves and restores its own state) */
static int l_rollback_possible = 0;
static struct snapshot l_snapshots[MAX_SNAPSHOTS];
/* the session wants a snapshot of the frame that just started */
static int l_snapshot_wanted = 0;
/* a snapshot to go back to at the end of this gen_interrupt, or NULL */
static struct snapshot* l_restore = NULL;
/* the self-test asked for it, not the session (which then doesn't know
 * which snapshots the restore drops) */
static int l_restore_selftest = 0;
/* frames below this are being re-run */
static unsigned int l_rerun_until = 0;
/* the game read the controllers during this frame */
static int l_read_this_frame = 0;
static struct tracked_state l_tracked[MAX_TRACKED];
static size_t l_tracked_count = 0;
/* the size of the video plugin's RDP state (0: it has none to hand over) */
static unsigned int l_rdp_state_size = 0;
static unsigned int l_rsp_state_size = 0;
static unsigned int l_exec_state_size = 0;
/* (asked once the video plugin has started, at the first snapshot) */
static int l_rdp_state_known = 0;

/* The save files. During the new netplay, writing them to disk waits until
 * the frames that changed them are final: their content is a generation,
 * and the host writes the newest final one (see flush_saves). */
static struct file_storage* l_storages[STORAGE_COUNT];
static unsigned int l_save_generation[STORAGE_COUNT];
static unsigned int l_written_generation[STORAGE_COUNT];

/* frames below this were final at the last frame boundary */
static unsigned int l_confirmed = 0;

static void free_snapshots(void);
static void rollback_boundary(void);
static void flush_saves(unsigned int confirmed);

/* For testing rollback on one machine: with RMG_NETPLAY_NEW_ROLLBACK_TEST=N,
 * the core saves every frame and goes back 4 frames every N frames by
 * itself. A re-run frame has the same input as the first time, so it must
 * end in exactly the same state; any part that doesn't is logged. Needs a
 * session in Rollback mode (it can have just the host). */
#define SELFTEST_FRAMES 64
#define SELFTEST_PAGES (RDRAM_MAX_SIZE / 0x4000)
/* what selftest_hash_machine fills in: 16 parts, then 512 RDRAM pages */
#define SELFTEST_CHECKS (16 + 512)
/* (exec: the execution plugin's own state, see NetplayStateHash) */
enum { SELFTEST_CP0, SELFTEST_SP, SELFTEST_GPR, SELFTEST_FPR, SELFTEST_QUEUE, SELFTEST_DP, SELFTEST_EXEC, SELFTEST_PARTS };
static const char* const l_selftest_names[SELFTEST_PARTS] = {"cp0", "sp", "gpr", "fpr", "queue", "dp", "exec"};
static int l_selftest = -1;
static struct
{
    unsigned int frame;
    int valid;
    uint32_t parts[SELFTEST_PARTS];
    uint32_t pages[SELFTEST_PAGES];
} l_selftest_runs[SELFTEST_FRAMES];
static unsigned int l_selftest_checked = 0;
static unsigned int l_selftest_failed = 0;
static unsigned int l_selftest_last = 0;
/* RMG_NETPLAY_NEW_ROLLBACK_TEST_FROM: the first frame it goes back from (64) */
static unsigned int l_selftest_from = 64;
/* how many frames rollback saved, and how many it couldn't (the game read
 * the controllers before the machine's state was whole), for the log */
static unsigned int l_snapshots_taken = 0;
static unsigned int l_snapshots_missed = 0;

static void selftest_frame_end(int rerun);
static void selftest_hash_machine(uint32_t* out);


void netplay_new_local_controller_begin(int controller)
{
    if (controller < 0 || controller > 3)
        return;
    l_saved_present[controller] = Controls[controller].Present;
    l_saved_plugin[controller] = Controls[controller].Plugin;
    Controls[controller].Present = l_local[controller].present;
    Controls[controller].Plugin = l_local[controller].plugin;
}

void netplay_new_local_controller_end(int controller)
{
    if (controller < 0 || controller > 3)
        return;
    Controls[controller].Present = l_saved_present[controller];
    Controls[controller].Plugin = l_saved_plugin[controller];
}

static void end_session(const char* reason)
{
    if (!l_ended)
    {
        DebugMessage(M64MSG_ERROR, "Netplay: %s", reason);
    }
    l_ended = 1;
    main_stop();
}

m64p_error netplay_new_set_callbacks(const m64p_netplay_new_callbacks* callbacks)
{
    if (callbacks == NULL)
    {
        /* hashes still waiting when the game ended have nowhere to go */
        l_hash_count = 0;
        l_active = 0;
        free_snapshots();
        memset(&l_callbacks, 0, sizeof(l_callbacks));
        return M64ERR_SUCCESS;
    }

    if (callbacks->api_version != M64P_NETPLAY_NEW_API_VERSION)
        return M64ERR_INCOMPATIBLE;

    if (callbacks->sync == NULL ||
        callbacks->configure_ports == NULL ||
        callbacks->sample == NULL ||
        callbacks->get_input == NULL ||
        callbacks->frame_end == NULL)
        return M64ERR_INPUT_INVALID;

    l_callbacks = *callbacks;
    l_active = 1;
    return M64ERR_SUCCESS;
}

int netplay_new_active(void)
{
    return l_active;
}

unsigned int netplay_new_frame(void)
{
    return l_frame;
}

/* whether this machine is the one that talks to services outside the
 * session (see speaker in m64p_types.h) */
static int netplay_new_speaker(void)
{
    return l_active && l_callbacks.speaker != NULL && l_callbacks.speaker(l_callbacks.context);
}

/* For execution plugins (they look it up in the core library): broadcasts a
 * message to every machine's plugin, which each gets at the end of the same
 * frame, some frames after the one the session puts it into (see
 * NetplayMessage in m64p_plugin.h) */
EXPORT int CALL ExecutionNetplaySendMessage(const void* data, uint32_t size)
{
    if (!l_active || l_ended || l_callbacks.send_message == NULL || (data == NULL && size > 0) ||
        size > M64P_NETPLAY_NEW_MAX_MESSAGE)
        return 0;
    return l_callbacks.send_message(l_callbacks.context, data, size);
}

/* hands the messages put into this frame to the execution plugin */
static void deliver_messages(void)
{
    unsigned int index;

    if (l_callbacks.get_message == NULL)
        return;

    for (index = 0; !l_ended; ++index)
    {
        unsigned int size = 0;
        uint64_t start = SDL_GetPerformanceCounter();
        const void* data = l_callbacks.get_message(l_callbacks.context, l_frame, index, &size);
        l_frame_waited += SDL_GetPerformanceCounter() - start;
        if (data == NULL)
            break;
        execution_netplay_message(data, size);
    }
}

/* For plugins (they look it up in the core library): 1 while a game of the
 * new netplay runs, when what a video plugin writes to RDRAM has to be the
 * same on every machine and come back the same after a rollback.
 * GLideN64 then stops copying its frame buffers to RDRAM (what it copies
 * comes from its own buffers on the GPU, so it differs between graphics
 * cards and resolutions), forces the same value on every machine for its
 * other settings that change what it writes to RDRAM (frame buffer
 * emulation on, depth rendered in software, no copy from RDRAM, the plain
 * aspect modes), and draws every display list while its window is resized
 * (a skipped list's writes to RDRAM would be missing on that machine only).
 * Parallel turns its superscaled readbacks off (they give the game pixels
 * from its upscaled copy of RDRAM on the GPU, which a rollback doesn't
 * save). */
EXPORT int CALL NetplayNewGame(void)
{
    return l_active;
}

int netplay_new_can_write_saves(void)
{
    return !l_active || l_callbacks.is_host;
}

void netplay_new_start(void)
{
    l_ended = 0;
    l_frame = 0;
    l_hash_count = 0;
    l_rdp_busy = 0;
    l_next_sample_frame = 0;
    l_start_time = (int64_t)time(NULL);
    l_frame_start = SDL_GetPerformanceCounter();
    l_frame_waited = 0;
    memset(l_local, 0, sizeof(l_local));
    memset(l_ports, 0, sizeof(l_ports));
    for (int i = 0; i < 4; ++i)
    {
        l_pending_pak[i] = -1;
        l_events_through[i] = 0;
    }

    /* an execution plugin keeps state of its own, which a rollback can only
     * restore if the plugin saves it */
    free_snapshots();
    l_rollback_possible = l_callbacks.rollback != NULL && l_callbacks.snapshot != NULL && execution_netplay_can_roll_back();
    if (l_callbacks.rollback != NULL && !l_rollback_possible)
    {
        DebugMessage(M64MSG_INFO, "Netplay: rollback isn't possible with an execution plugin that can't save its state");
    }
    l_snapshot_wanted = 0;
    l_restore = NULL;
    l_restore_selftest = 0;
    l_rerun_until = 0;
    l_read_this_frame = 0;
    l_rdp_state_size = 0;
    l_rsp_state_size = 0;
    l_exec_state_size = 0;
    l_rdp_state_known = 0;
    {
        const char* test = getenv("RMG_NETPLAY_NEW_ROLLBACK_TEST");
        l_selftest = test != NULL ? atoi(test) : 0;
        {
            const char* from = getenv("RMG_NETPLAY_NEW_ROLLBACK_TEST_FROM");
            l_selftest_from = from != NULL ? (unsigned int)atoi(from) : 64;
        }
        memset(l_selftest_runs, 0, sizeof(l_selftest_runs));
        l_selftest_checked = 0;
        l_selftest_failed = 0;
        l_selftest_last = 0;
        l_snapshots_taken = 0;
        l_snapshots_missed = 0;
        if (l_selftest > 0)
            DebugMessage(M64MSG_INFO, "Rollback test: going back 4 frames every %d frames", l_selftest);
    }

    /* a motor a re-run held back in an earlier game has nothing to flush to */
    input_plugin_rumble_reset();
    l_tracked_count = 0;
    l_confirmed = 0;
    memset(l_storages, 0, sizeof(l_storages));
    memset(l_save_generation, 0, sizeof(l_save_generation));
    memset(l_written_generation, 0, sizeof(l_written_generation));

    /* what savestates leave out: which pak is in each controller (and
     * whether it's being switched), and the cheats' counter */
    netplay_new_track_state(Controls, sizeof(Controls));
    netplay_new_track_state(g_dev.controllers, sizeof(g_dev.controllers));
    netplay_new_track_state(&g_gs_vi_counter, sizeof(g_gs_vi_counter));
}

void netplay_new_sync_settings(m64p_netplay_new_settings* settings)
{
    if (!l_active || l_ended)
        return;

    if (!l_callbacks.sync(l_callbacks.context, M64P_NETPLAY_NEW_SYNC_SETTINGS, settings, sizeof(*settings)))
    {
        end_session("the session ended while starting the game");
        return;
    }

    /* the clock is shared right after the settings */
    if (!l_callbacks.sync(l_callbacks.context, M64P_NETPLAY_NEW_SYNC_CLOCK, &l_start_time, sizeof(l_start_time)))
    {
        end_session("the session ended while starting the game");
    }
}

void netplay_new_sync_data(m64p_netplay_new_sync kind, void* data, size_t size)
{
    if (!l_active || l_ended)
        return;

    if (!l_callbacks.sync(l_callbacks.context, kind, data, (unsigned int)size))
    {
        end_session("the session ended while starting the game");
        if (data != NULL && size > 0)
            memset(data, 0, size);
    }
}

void netplay_new_initiate_execution_plugin(void)
{
    const void* data = NULL;
    uint32_t size = 0;

    execution_netplay_start(l_callbacks.is_host);

    /* frame 0 (whether this machine is the speaker matters before the plugin
     * starts) */
    execution_netplay_frame(0, 0, netplay_new_speaker() ? NETPLAY_FRAME_SPEAKER : 0);

    if (l_callbacks.is_host)
    {
        /* the host's plugin starts with its own settings and state, then shares them */
        initiate_execution_plugin();
        execution_netplay_export(&data, &size);
        netplay_new_sync_data(M64P_NETPLAY_NEW_SYNC_EXECUTION_SIZE, &size, sizeof(size));
        netplay_new_sync_data(M64P_NETPLAY_NEW_SYNC_EXECUTION, (void*)data, size);
    }
    else
    {
        /* the guests' plugins start with the host's */
        void* buffer;
        netplay_new_sync_data(M64P_NETPLAY_NEW_SYNC_EXECUTION_SIZE, &size, sizeof(size));
        buffer = malloc(size > 0 ? size : 1);
        if (buffer != NULL)
        {
            netplay_new_sync_data(M64P_NETPLAY_NEW_SYNC_EXECUTION, buffer, size);
            if (!l_ended)
                execution_netplay_import(buffer, size);
            free(buffer);
        }
        initiate_execution_plugin();
    }
}

void netplay_new_configure_ports(int control_ids[4])
{
    int i;

    for (i = 0; i < 4; ++i)
    {
        l_local[i].present = Controls[i].Present;
        l_local[i].plugin = Controls[i].Plugin;
        l_local[i].local_controller = i;

        /* VRU and raw data controllers can't be shared */
        if (Controls[i].Type == CONT_TYPE_VRU)
        {
            l_local[i].present = 0;
        }

        l_ports[i].present = 0;
        l_ports[i].plugin = PLUGIN_NONE;
        l_ports[i].local_controller = -1;
    }

    if (!l_ended && !l_callbacks.configure_ports(l_callbacks.context, l_local, l_ports))
    {
        end_session("the session ended while starting the game");
    }

    for (i = 0; i < 4; ++i)
    {
        /* the transfer pak isn't supported in netplay (PLUGIN_RAW is how
         * some input plugins say rumble pak; raw data itself is turned off) */
        if (l_ports[i].plugin == PLUGIN_TRANSFER_PAK)
        {
            l_ports[i].plugin = PLUGIN_NONE;
        }
        if (l_ports[i].local_controller < -1 || l_ports[i].local_controller > 3)
        {
            l_ports[i].local_controller = -1;
        }

        Controls[i].Present = l_ports[i].present;
        Controls[i].Plugin = l_ports[i].plugin;
        Controls[i].RawData = 0;
        Controls[i].Type = CONT_TYPE_STANDARD;

        control_ids[i] = l_ports[i].local_controller;
    }
}

static void sample_local(unsigned int frame)
{
    m64p_netplay_new_input local[4];
    int i;

    for (i = 0; i < 4; ++i)
    {
        BUTTONS keys;

        keys.Value = 0;

        if (l_local[i].present && input.getKeys != NULL)
        {
            /* Controls[] holds the session's ports now, so give the input
             * plugin back its own view of this controller while it reads it */
            netplay_new_local_controller_begin(i);

            input.getKeys(i, &keys);

            /* the player switched this controller's pak: tell everyone, once */
            local[i].pak = -1;
            if (Controls[i].Plugin != l_local[i].plugin)
            {
                l_local[i].plugin = Controls[i].Plugin;
                local[i].pak = l_local[i].plugin;
            }

            netplay_new_local_controller_end(i);
        }
        else
        {
            local[i].pak = -1;
        }

        local[i].buttons = keys.Value & ~(uint32_t)LOCAL_ONLY_BUTTONS;
    }

#ifdef NETPLAY_NEW_TESTS
    /* For tests with several copies on one computer, which can't all have
     * the keyboard: with RMG_NETPLAY_NEW_TEST_PRESS=a,s the first controller
     * presses A for 3 frames every a frames and Start every s frames (0: never),
     * from frame f on, and Start only until frame u (0: always), which gets
     * through a game's menus without pausing it once it plays (a,s,f,u) */
    {
        static int test_press = -1;
        static unsigned int press_a = 0, press_start = 0, press_from = 0, press_start_until = 0;
        if (test_press < 0)
        {
            const char* value = getenv("RMG_NETPLAY_NEW_TEST_PRESS");
            test_press = value != NULL && sscanf(value, "%u,%u,%u,%u", &press_a, &press_start, &press_from,
                                                 &press_start_until) >= 1;
        }
        if (test_press && l_local[0].present && frame >= press_from)
        {
            if (press_a != 0 && frame % press_a < 3)
                local[0].buttons |= 0x0080;
            if (press_start != 0 && frame % press_start < 3 && (press_start_until == 0 || frame < press_start_until))
                local[0].buttons |= 0x0010;
        }
    }
#endif

    l_callbacks.sample(l_callbacks.context, frame, local);
}

static void sample_through(unsigned int frame)
{
    while (l_next_sample_frame <= frame)
    {
        sample_local(l_next_sample_frame);
        ++l_next_sample_frame;
    }
}

/* the session's get_input, with the time it blocks counted as waiting */
static int timed_get_input(unsigned int frame, int port, m64p_netplay_new_input* value)
{
    uint64_t start = SDL_GetPerformanceCounter();
    int ret = l_callbacks.get_input(l_callbacks.context, frame, port, value);
    l_frame_waited += SDL_GetPerformanceCounter() - start;
    return ret;
}

void netplay_new_frame_start(void)
{
    l_frame_start = SDL_GetPerformanceCounter();
    l_frame_waited = 0;
}

static unsigned int frame_busy_us(void)
{
    uint64_t elapsed = SDL_GetPerformanceCounter() - l_frame_start;
    uint64_t busy = elapsed > l_frame_waited ? elapsed - l_frame_waited : 0;
    uint64_t us = busy * 1000000 / SDL_GetPerformanceFrequency();
    return us > 1000000 ? 1000000 : (unsigned int)us;
}

static void take_event(int port, const m64p_netplay_new_input* value)
{
    if (l_events_through[port] > l_frame)
        return; /* this frame's switch was taken already */

    if (value->pak >= 0)
    {
        /* the transfer pak isn't supported in netplay */
        l_pending_pak[port] = (value->pak == PLUGIN_TRANSFER_PAK) ? PLUGIN_NONE : value->pak;
    }
    l_events_through[port] = l_frame + 1;
}

int netplay_new_get_input(int port, uint32_t* input, int* pak)
{
    m64p_netplay_new_input value;

    *input = 0;
    *pak = -1;

    if (port < 0 || port > 3 || !l_ports[port].present)
        return 0;

    if (l_ended)
    {
        end_session("the session ended");
        return 1;
    }

    sample_through(l_frame);
    l_read_this_frame = 1;

    if (!timed_get_input(l_frame, port, &value))
    {
        end_session("the session ended");
        return 1;
    }

    *input = value.buttons & ~(uint32_t)LOCAL_ONLY_BUTTONS;
    take_event(port, &value);
    *pak = l_pending_pak[port];
    l_pending_pak[port] = -1;

    return 1;
}

/* What has to match on every machine: memory, the CP0 registers (including
 * the timers) and the RSP's memory. The CPU's other registers aren't
 * included: the dynamic recompiler keeps some of them in host registers. A
 * difference there shows up in memory within a few frames anyway. XXH3 hashes
 * the 8 MB of RDRAM in well under a millisecond. */
static uint64_t state_hash(void)
{
    XXH3_state_t state;
    XXH3_64bits_reset(&state);
    XXH3_64bits_update(&state, g_dev.rdram.dram, g_dev.rdram.dram_size);
    XXH3_64bits_update(&state, r4300_cp0_regs(&g_dev.r4300.cp0), CP0_REGS_COUNT * sizeof(uint32_t));
    XXH3_64bits_update(&state, g_dev.sp.mem, SP_MEM_SIZE);
    return XXH3_64bits_digest(&state);
}

/* For tracking down desyncs: with the environment variable
 * RMG_NETPLAY_NEW_HASH_LOG=1, every frame's hash is also logged in parts (CP0,
 * the RSP's memory, and RDRAM in 16 KB pages, by address). Frames are
 * numbered the same on every machine, so comparing two players' logs shows
 * the first frame and the memory that differ. */
static uint32_t hash32(const void* data, size_t size)
{
    return (uint32_t)XXH3_64bits(data, size);
}

static void log_state_hash(unsigned int frame, uint64_t sent, char where)
{
    static int enabled = -1;
    char line[480];
    size_t length;
    size_t page;
    const size_t page_size = 0x4000;
    const size_t pages_per_line = 40;
    const uint8_t* rdram = (const uint8_t*)g_dev.rdram.dram;
    char queue[1024];
    uint32_t cpu[3];

    if (enabled < 0)
    {
        const char* value = getenv("RMG_NETPLAY_NEW_HASH_LOG");
        enabled = value != NULL && value[0] == '1';
    }
    if (!enabled)
        return;

    /* the hash that was sent, and where it was taken (at a full sync, at the
     * end of the frame, or forced after waiting for the RDP) */
    snprintf(line, sizeof(line), "hash %u at %c sent %016llx cp0 %08x sp %08x",
             frame, where, (unsigned long long)sent,
             (unsigned int)hash32(r4300_cp0_regs(&g_dev.r4300.cp0), CP0_REGS_COUNT * sizeof(uint32_t)),
             (unsigned int)hash32(g_dev.sp.mem, SP_MEM_SIZE));
    DebugMessage(M64MSG_VERBOSE, "%s", line);

    /* what the hash leaves out, which a desync often shows in first: the
     * CPU's registers, the interrupt queue and the RCP's registers */
    cpu[0] = hash32(r4300_regs(&g_dev.r4300), 32 * sizeof(int64_t));
    cpu[1] = hash32(r4300_cp1_regs(&g_dev.r4300.cp1), 32 * sizeof(int64_t));
    cpu[2] = (uint32_t)(*r4300_mult_hi(&g_dev.r4300) ^ *r4300_mult_lo(&g_dev.r4300));
    memset(queue, 0, sizeof(queue));
    save_eventqueue_infos(&g_dev.r4300.cp0, queue);
    snprintf(line, sizeof(line), "hash %u state gpr %08x fpr %08x hilo %08x queue %08x mi %08x pi %08x si %08x ai %08x %08x %08x vi %08x sp %08x %08x %08x dp %08x pif %08x",
             frame, cpu[0], cpu[1], cpu[2], hash32(queue, sizeof(queue)),
             hash32(g_dev.mi.regs, sizeof(g_dev.mi.regs)),
             hash32(g_dev.pi.regs, sizeof(g_dev.pi.regs)),
             hash32(g_dev.si.regs, sizeof(g_dev.si.regs)),
             hash32(g_dev.ai.regs, sizeof(g_dev.ai.regs)),
             hash32(g_dev.ai.fifo, sizeof(g_dev.ai.fifo)),
             (unsigned int)(g_dev.ai.last_read ^ (g_dev.ai.delayed_carry << 31)),
             hash32(g_dev.vi.regs, sizeof(g_dev.vi.regs)),
             hash32(g_dev.sp.regs, sizeof(g_dev.sp.regs)),
             hash32(g_dev.sp.regs2, sizeof(g_dev.sp.regs2)),
             (unsigned int)(g_dev.sp.rsp_status ^ (g_dev.sp.rsp_wait << 16) ^ (g_dev.sp.first_run << 31)),
             hash32(g_dev.dp.dpc_regs, sizeof(g_dev.dp.dpc_regs)),
             hash32(g_dev.pif.ram, PIF_RAM_SIZE));
    DebugMessage(M64MSG_VERBOSE, "%s", line);

    /* RDRAM in 16 KB pages, by address, a few dozen per line (the
     * frontend cuts long messages) */
    for (page = 0; page * page_size < g_dev.rdram.dram_size; page += pages_per_line)
    {
        size_t k;
        length = (size_t)snprintf(line, sizeof(line), "hash %u rdram %x", frame, (unsigned int)page);
        for (k = page; k < page + pages_per_line && k * page_size < g_dev.rdram.dram_size; ++k)
        {
            length += (size_t)snprintf(line + length, sizeof(line) - length, " %08x",
                                       hash32(rdram + k * page_size, page_size));
        }
        DebugMessage(M64MSG_VERBOSE, "%s", line);
    }
}

static void send_hashes(char where)
{
    uint64_t hash;
    unsigned int i;

    if (l_hash_count == 0)
        return;

    hash = state_hash();
    for (i = 0; i < l_hash_count; ++i)
    {
        l_callbacks.frame_hash(l_callbacks.context, l_hash_first + i, hash, l_frame);
        log_state_hash(l_hash_first + i, hash, where);
    }
    l_hash_count = 0;
}

void netplay_new_rdp_list(void)
{
    l_rdp_busy = 1;
}

void netplay_new_rdp_full_sync(void)
{
    l_rdp_busy = 0;

    /* these hooks run in every game; outside a session there are no
     * callbacks to send hashes to */
    if (!l_active)
        return;

    send_hashes('s');
}

int netplay_new_frame_end(void)
{
    int result;
    int rerun;

    if (l_ended)
    {
        end_session("the session ended");
        return 0;
    }

    sample_through(l_frame);

    /* the pak switches on ports the game didn't read this frame */
    for (int port = 0; port < 4; ++port)
    {
        m64p_netplay_new_input value;
        if (!l_ports[port].present || l_events_through[port] > l_frame)
            continue;
        if (!timed_get_input(l_frame, port, &value))
        {
            end_session("the session ended");
            return 0;
        }
        take_event(port, &value);
    }

    /* the execution plugins' messages for this frame */
    deliver_messages();

    if (l_callbacks.frame_hash != NULL)
    {
        if (l_hash_count == 0)
            l_hash_first = l_frame;
        ++l_hash_count;

        /* when the RDP is idle (or an HLE graphics plugin has drawn
         * everything already) RDRAM is complete now */
        if (!l_rdp_busy)
        {
            send_hashes('f');
        }
        /* a game that doesn't full sync for a long time gets a hash anyway,
         * once a GPU plugin has finished writing RDRAM for the commands the
         * RDP was given */
        else if (l_hash_count >= MAX_PENDING_HASHES)
        {
            plugin_wait_rdp_idle();
            send_hashes('w');
        }
    }

    rerun = netplay_new_rerunning();

    if (l_selftest > 0)
    {
        selftest_frame_end(rerun);
    }
    {
        unsigned int rate = g_dev.vi.expected_refresh_rate != 0 ? g_dev.vi.expected_refresh_rate : 60;
        result = l_callbacks.frame_end(l_callbacks.context, l_frame, frame_busy_us(), 1000000 / rate,
                                       rerun ? M64P_NETPLAY_NEW_RERUN : 0);
    }
    ++l_frame;
    l_read_this_frame = 0;

    if (result < 0)
    {
        end_session("the session ended");
        return 0;
    }

    /* the re-run caught up; a motor a wrong guess left running stops now */
    if (rerun && l_frame >= l_rerun_until)
    {
        input_plugin_rumble_flush();
    }

    rollback_boundary();

    /* re-run frames run back to back (and so does the self-test) */
    if (rerun || l_selftest > 0)
    {
        return M64P_NETPLAY_NEW_SKIP_LIMITER;
    }
    return result;
}

int netplay_new_rerunning(void)
{
    return l_active && l_frame < l_rerun_until;
}

void netplay_new_track_state(void* data, size_t size)
{
    if (l_tracked_count < MAX_TRACKED)
    {
        l_tracked[l_tracked_count].data = data;
        l_tracked[l_tracked_count].size = size;
        ++l_tracked_count;
    }
}

void netplay_new_set_storages(struct file_storage* eep, struct file_storage* sra,
                              struct file_storage* fla, struct file_storage* mpk)
{
    l_storages[STORAGE_EEP] = eep;
    l_storages[STORAGE_SRA] = sra;
    l_storages[STORAGE_FLA] = fla;
    l_storages[STORAGE_MPK] = mpk;
}

int netplay_new_defer_save(struct file_storage* storage)
{
    int k;

    if (!l_active)
        return 0;

    for (k = 0; k < STORAGE_COUNT; ++k)
    {
        if (l_storages[k] == storage)
        {
            ++l_save_generation[k];
            return 1;
        }
    }
    return 0;
}

/* The host writes a save file once the frames that changed it are final:
 * the live copy when everything emulated so far is final, otherwise the
 * copy in the newest snapshot that is. */
static void flush_saves(unsigned int confirmed)
{
    const struct snapshot* base = NULL;
    int k;
    int i;

    if (!l_callbacks.is_host)
        return;

    for (i = 0; i < MAX_SNAPSHOTS; ++i)
    {
        const struct snapshot* s = &l_snapshots[i];
        if (s->kept && s->frame <= confirmed && (base == NULL || s->frame > base->frame))
            base = s;
    }

    for (k = 0; k < STORAGE_COUNT; ++k)
    {
        const uint8_t* data = NULL;
        unsigned int generation = 0;

        if (l_storages[k] == NULL)
            continue;

        if (confirmed >= l_frame && l_restore == NULL)
        {
            data = l_storages[k]->data;
            generation = l_save_generation[k];
        }
        else if (base != NULL && base->saves[k] != NULL)
        {
            data = base->saves[k];
            generation = base->save_generation[k];
        }

        if (data != NULL && generation != l_written_generation[k])
        {
            file_storage_write_all(l_storages[k], data);
            l_written_generation[k] = generation;
        }
    }
}

static void free_snapshots(void)
{
    int i, k;
    for (i = 0; i < MAX_SNAPSHOTS; ++i)
    {
        free(l_snapshots[i].state);
        for (k = 0; k < STORAGE_COUNT; ++k)
            free(l_snapshots[i].saves[k]);
        for (k = 0; k < MAX_TRACKED; ++k)
            free(l_snapshots[i].tracked[k]);
        free(l_snapshots[i].rdp_state);
        free(l_snapshots[i].rsp_state);
        free(l_snapshots[i].exec_state);
        free(l_snapshots[i].check);
    }
    memset(l_snapshots, 0, sizeof(l_snapshots));
}

static void drop_snapshot(struct snapshot* s, int tell)
{
    if (!s->kept)
        return;
    s->kept = 0;
    execution_netplay_drop_state(s->frame);
    if (tell)
        l_callbacks.snapshot(l_callbacks.context, s->frame, 0);
}

static struct snapshot* find_snapshot(unsigned int frame)
{
    int i;
    for (i = 0; i < MAX_SNAPSHOTS; ++i)
    {
        if (l_snapshots[i].kept && l_snapshots[i].frame == frame)
            return &l_snapshots[i];
    }
    return NULL;
}

/* returns 0 if the frame couldn't be saved */
static int take_snapshot(void)
{
    struct snapshot* s = NULL;
    size_t k;
    int i;
    static int warned = 0;

    if (find_snapshot(l_frame) != NULL)
        return 1;

    for (i = 0; i < MAX_SNAPSHOTS && s == NULL; ++i)
    {
        if (!l_snapshots[i].kept)
            s = &l_snapshots[i];
    }
    if (s == NULL)
    {
        /* the session only guesses as far as its snapshots reach */
        if (!warned)
            DebugMessage(M64MSG_WARNING, "Netplay: no room for another rollback snapshot");
        warned = 1;
        return 0;
    }

    if (s->state == NULL)
    {
        s->state = malloc(savestates_rollback_size());
        if (s->state == NULL)
            return 0;
    }
    if (l_selftest > 0 && s->check == NULL)
    {
        s->check = malloc(SELFTEST_CHECKS * sizeof(*s->check));
        if (s->check == NULL)
            return 0;
    }
    for (k = 0; k < STORAGE_COUNT; ++k)
    {
        if (l_storages[k] != NULL && s->saves[k] == NULL)
            s->saves[k] = malloc(l_storages[k]->size);
    }
    for (k = 0; k < l_tracked_count; ++k)
    {
        if (s->tracked[k] == NULL)
            s->tracked[k] = malloc(l_tracked[k].size);
        if (s->tracked[k] == NULL)
            return 0;
    }
    if (!l_rdp_state_known)
    {
        l_rdp_state_size = plugin_rdp_state_size();
        l_rsp_state_size = plugin_rsp_state_size();
        l_exec_state_size = execution_netplay_state_size();
        l_rdp_state_known = 1;
        DebugMessage(M64MSG_VERBOSE, "Netplay: rollback saves %u bytes of the video plugin's RDP state, %u of the RSP plugin's and %u of the execution plugin's",
                     l_rdp_state_size, l_rsp_state_size, l_exec_state_size);
    }
    if (l_exec_state_size > 0 && s->exec_state == NULL)
    {
        s->exec_state = malloc(l_exec_state_size);
        if (s->exec_state == NULL)
            return 0;
    }
    if (l_rsp_state_size > 0 && s->rsp_state == NULL)
    {
        s->rsp_state = malloc(l_rsp_state_size);
        if (s->rsp_state == NULL)
            return 0;
    }
    if (l_rdp_state_size > 0 && s->rdp_state == NULL)
    {
        s->rdp_state = malloc(l_rdp_state_size);
        if (s->rdp_state == NULL)
            return 0;
    }

    /* a GPU plugin may still be writing RDRAM for commands the RDP was given */
    plugin_wait_rdp_idle();
    savestates_rollback_save(s->state);
    if (l_rdp_state_size > 0)
        plugin_save_rdp_state(s->rdp_state);
    if (l_rsp_state_size > 0)
        plugin_save_rsp_state(s->rsp_state);
    execution_netplay_save_state(l_frame, s->exec_state);

    for (k = 0; k < STORAGE_COUNT; ++k)
    {
        if (l_storages[k] != NULL && s->saves[k] != NULL)
            memcpy(s->saves[k], l_storages[k]->data, l_storages[k]->size);
        s->save_generation[k] = l_save_generation[k];
    }
    for (k = 0; k < l_tracked_count; ++k)
        memcpy(s->tracked[k], l_tracked[k].data, l_tracked[k].size);

    /* (the savestate format only has the AI's DMA lengths) */
    s->ai_address[0] = g_dev.ai.fifo[0].address;
    s->ai_address[1] = g_dev.ai.fifo[1].address;
    s->hash_first = l_hash_first;
    s->hash_count = l_hash_count;
    s->rdp_busy = l_rdp_busy;
    memcpy(s->pending_pak, l_pending_pak, sizeof(s->pending_pak));
    memcpy(s->events_through, l_events_through, sizeof(s->events_through));

    s->frame = l_frame;
    s->kept = 1;
    if (s->check != NULL)
        selftest_hash_machine(s->check);
    l_callbacks.snapshot(l_callbacks.context, l_frame, 1);
    return 1;
}

/* tell: the self-test asked for the restore, so the session has to hear
 * about the snapshots it drops */
static void restore_snapshot(struct snapshot* s, int tell)
{
    size_t k;
    int i;

    /* a GPU plugin may still be writing RDRAM for commands the RDP was given */
    plugin_wait_rdp_idle();
    savestates_rollback_load(s->state);
    if (l_rdp_state_size > 0 && s->rdp_state != NULL)
        plugin_load_rdp_state(s->rdp_state);
    if (l_rsp_state_size > 0 && s->rsp_state != NULL)
        plugin_load_rsp_state(s->rsp_state);
    /* (after the machine's own state: the plugin sets its hooks again) */
    execution_netplay_load_state(s->frame, s->exec_state);

    for (k = 0; k < STORAGE_COUNT; ++k)
    {
        if (l_storages[k] != NULL && s->saves[k] != NULL)
            memcpy(l_storages[k]->data, s->saves[k], l_storages[k]->size);
        l_save_generation[k] = s->save_generation[k];
    }
    for (k = 0; k < l_tracked_count; ++k)
        memcpy(l_tracked[k].data, s->tracked[k], l_tracked[k].size);

    g_dev.ai.fifo[0].address = s->ai_address[0];
    g_dev.ai.fifo[1].address = s->ai_address[1];
    l_hash_first = s->hash_first;
    l_hash_count = s->hash_count;
    l_rdp_busy = s->rdp_busy;
    memcpy(l_pending_pak, s->pending_pak, sizeof(l_pending_pak));
    memcpy(l_events_through, s->events_through, sizeof(l_events_through));
    l_frame = s->frame;
    l_read_this_frame = 0;

    /* (self-test: the machine must look exactly as when it was saved) */
    if (s->check != NULL)
    {
        static const char* const names[16] = {"cp0", "sp", "gpr", "fpr", "queue", "dp", "mi", "pi", "si", "ai", "vi",
                                               "spregs", "pif", "pc", "controllers", "aififo"};
        uint32_t now[SELFTEST_CHECKS];
        char line[400];
        size_t length;
        int k;
        unsigned int pages = 0;
        selftest_hash_machine(now);
        length = (size_t)snprintf(line, sizeof(line), "Rollback test: restoring frame %u differs:", s->frame);
        for (k = 0; k < 16; ++k)
        {
            if (now[k] != s->check[k])
                length += (size_t)snprintf(line + length, sizeof(line) - length, " %s", names[k]);
        }
        for (k = 16; k < SELFTEST_CHECKS; ++k)
        {
            if (now[k] != s->check[k])
            {
                if (pages < 8 && length + 10 < sizeof(line))
                    length += (size_t)snprintf(line + length, sizeof(line) - length, " %06x", (unsigned int)((k - 16) * 0x4000));
                ++pages;
            }
        }
        if (memcmp(now, s->check, sizeof(now)) != 0)
            DebugMessage(M64MSG_WARNING, "%s (%u pages)", line, pages);
    }

    /* The frames after it are gone (the session forgot them already, unless
     * the self-test went back). This drops the execution plugin's saves of
     * them too, so its NetplayLoadState needn't. */
    for (i = 0; i < MAX_SNAPSHOTS; ++i)
    {
        if (l_snapshots[i].kept && l_snapshots[i].frame > s->frame)
            drop_snapshot(&l_snapshots[i], tell);
    }

    /* The frame starts again. Its boundary went to the rollback instead
     * (rollback_boundary tells the plugin about the frames after it), so
     * this is its only NetplayFrame; the final frames are the ones of that
     * last boundary. */
    execution_netplay_frame(s->frame, l_confirmed,
                            (netplay_new_rerunning() ? NETPLAY_FRAME_RERUN : 0) |
                            (netplay_new_speaker() ? NETPLAY_FRAME_SPEAKER : 0));
}

/* the self-test's view of the machine: its parts and RDRAM in 16 KB pages */
static void selftest_hash_machine(uint32_t* out)
{
    char queue[1024];
    size_t page;
    const uint8_t* rdram = (const uint8_t*)g_dev.rdram.dram;

    memset(queue, 0, sizeof(queue));
    save_eventqueue_infos(&g_dev.r4300.cp0, queue);
    out[0] = hash32(r4300_cp0_regs(&g_dev.r4300.cp0), CP0_REGS_COUNT * sizeof(uint32_t));
    out[1] = hash32(g_dev.sp.mem, SP_MEM_SIZE);
    out[2] = hash32(r4300_regs(&g_dev.r4300), 32 * sizeof(int64_t));
    out[3] = hash32(r4300_cp1_regs(&g_dev.r4300.cp1), 32 * sizeof(int64_t));
    out[4] = hash32(queue, sizeof(queue));
    out[5] = hash32(g_dev.dp.dpc_regs, sizeof(g_dev.dp.dpc_regs));
    out[6] = hash32(g_dev.mi.regs, sizeof(g_dev.mi.regs));
    out[7] = hash32(g_dev.pi.regs, sizeof(g_dev.pi.regs));
    out[8] = hash32(g_dev.si.regs, sizeof(g_dev.si.regs));
    out[9] = hash32(g_dev.ai.regs, sizeof(g_dev.ai.regs));
    out[10] = hash32(g_dev.vi.regs, sizeof(g_dev.vi.regs));
    out[11] = hash32(g_dev.sp.regs, sizeof(g_dev.sp.regs));
    out[12] = hash32(g_dev.pif.ram, PIF_RAM_SIZE);
    out[13] = *r4300_pc(&g_dev.r4300);
    out[14] = hash32(g_dev.controllers, sizeof(g_dev.controllers));
    out[15] = hash32(&g_dev.ai.fifo, sizeof(g_dev.ai.fifo));
    for (page = 0; page < 512; ++page)
        out[16 + page] = hash32(rdram + page * 0x4000, 0x4000);
}

static void selftest_frame_end(int rerun)
{
    char queue[1024];
    uint32_t parts[SELFTEST_PARTS];
    size_t page;
    int k;
    const uint8_t* rdram = (const uint8_t*)g_dev.rdram.dram;
    unsigned int slot = l_frame % SELFTEST_FRAMES;

    /* only the frames that are run again are checked (waiting for the GPU
     * on the others would hide a difference that comes from not waiting) */
    if (l_frame % (unsigned int)l_selftest + 4 < (unsigned int)l_selftest)
        return;

    /* (RDRAM must be complete) */
    plugin_wait_rdp_idle();

    memset(queue, 0, sizeof(queue));
    save_eventqueue_infos(&g_dev.r4300.cp0, queue);
    parts[SELFTEST_CP0] = hash32(r4300_cp0_regs(&g_dev.r4300.cp0), CP0_REGS_COUNT * sizeof(uint32_t));
    parts[SELFTEST_SP] = hash32(g_dev.sp.mem, SP_MEM_SIZE);
    parts[SELFTEST_GPR] = hash32(r4300_regs(&g_dev.r4300), 32 * sizeof(int64_t));
    parts[SELFTEST_FPR] = hash32(r4300_cp1_regs(&g_dev.r4300.cp1), 32 * sizeof(int64_t));
    parts[SELFTEST_QUEUE] = hash32(queue, sizeof(queue));
    parts[SELFTEST_DP] = hash32(g_dev.dp.dpc_regs, sizeof(g_dev.dp.dpc_regs));
    {
        uint64_t exec = execution_netplay_state_hash();
        parts[SELFTEST_EXEC] = (uint32_t)(exec ^ (exec >> 32));
    }

    if (!rerun || !l_selftest_runs[slot].valid || l_selftest_runs[slot].frame != l_frame)
    {
        /* the first run: remember how it ended */
        l_selftest_runs[slot].frame = l_frame;
        l_selftest_runs[slot].valid = 1;
        memcpy(l_selftest_runs[slot].parts, parts, sizeof(parts));
        for (page = 0; page < SELFTEST_PAGES; ++page)
            l_selftest_runs[slot].pages[page] = hash32(rdram + page * 0x4000, 0x4000);
        return;
    }

    /* a re-run: it must end the same */
    {
        char line[480];
        size_t length;
        int differs = 0;
        size_t pages = 0;

        length = (size_t)snprintf(line, sizeof(line), "Rollback test: frame %u re-run differs:", l_frame);
        for (k = 0; k < SELFTEST_PARTS; ++k)
        {
            if (parts[k] != l_selftest_runs[slot].parts[k])
            {
                length += (size_t)snprintf(line + length, sizeof(line) - length, " %s", l_selftest_names[k]);
                differs = 1;
            }
        }
        for (page = 0; page < SELFTEST_PAGES; ++page)
        {
            if (hash32(rdram + page * 0x4000, 0x4000) != l_selftest_runs[slot].pages[page])
            {
                if (pages < 16 && length + 12 < sizeof(line))
                    length += (size_t)snprintf(line + length, sizeof(line) - length, " %06x", (unsigned int)(page * 0x4000));
                ++pages;
                differs = 1;
            }
        }
        ++l_selftest_checked;
        if (differs)
        {
            ++l_selftest_failed;
            DebugMessage(M64MSG_WARNING, "%s (%u RDRAM pages; %u of %u re-run frames differed so far)",
                         line, (unsigned int)pages, l_selftest_failed, l_selftest_checked);
        }
        else if (l_selftest_checked % 600 == 0)
        {
            DebugMessage(M64MSG_INFO, "Rollback test: %u of %u re-run frames differed so far",
                         l_selftest_failed, l_selftest_checked);
        }
    }
}

static void rollback_boundary(void)
{
    unsigned int confirmed = l_frame;
    struct snapshot* base = NULL;
    int snapshot = 0;
    int target = -1;
    int i;

    l_snapshot_wanted = 0;

    if (l_rollback_possible)
    {
        target = l_callbacks.rollback(l_callbacks.context, l_frame, &confirmed, &snapshot);
    }

    /* the self-test: save every frame, and go back now and then */
    if (l_selftest > 0 && l_rollback_possible && target < 0)
    {
        snapshot = 1;
        if (l_frame >= l_selftest_from && l_frame % (unsigned int)l_selftest == 0 && l_frame > l_selftest_last)
        {
            struct snapshot* back = NULL;
            for (i = 0; i < MAX_SNAPSHOTS; ++i)
            {
                struct snapshot* s = &l_snapshots[i];
                if (s->kept && s->frame + 4 <= l_frame && (back == NULL || s->frame > back->frame))
                    back = s;
            }
            if (back != NULL)
            {
                l_selftest_last = l_frame;
                l_restore = back;
                l_restore_selftest = 1;
                l_rerun_until = l_frame;
                return;
            }
        }
        /* keep the last few frames' snapshots */
        confirmed = l_frame > 10 ? l_frame - 10 : 0;
    }

    /* Going back: the frame that was guessed wrong is final now, but the
     * snapshots after the one gone back to were taken with the wrong input,
     * so none of them counts as final (restoring drops them) */
    if (target >= 0)
    {
        l_restore = find_snapshot((unsigned int)target);
        l_restore_selftest = 0;
        if (l_restore == NULL)
        {
            end_session("a rollback needed a frame that wasn't saved");
            return;
        }
        if (l_rerun_until < l_frame)
            l_rerun_until = l_frame;
        return;
    }

    /* the newest snapshot of a final frame is where a rollback can go back
     * to at the farthest; the ones before it can go */
    for (i = 0; i < MAX_SNAPSHOTS; ++i)
    {
        struct snapshot* s = &l_snapshots[i];
        if (s->kept && s->frame <= confirmed && (base == NULL || s->frame > base->frame))
            base = s;
    }
    for (i = 0; i < MAX_SNAPSHOTS; ++i)
    {
        struct snapshot* s = &l_snapshots[i];
        if (s->kept && base != NULL && s->frame < base->frame)
            drop_snapshot(s, 1);
    }

    l_snapshot_wanted = snapshot;
    l_confirmed = confirmed;
    flush_saves(confirmed);

    /* the frame that starts now, and which ones are final */
    execution_netplay_frame(l_frame, confirmed,
                            (netplay_new_rerunning() ? NETPLAY_FRAME_RERUN : 0) |
                            (netplay_new_speaker() ? NETPLAY_FRAME_SPEAKER : 0));
}

void netplay_new_stop(void)
{
    /* the newest final copy of each save file; frames after the last
     * boundary (part of a frame) are dropped like guesses */
    flush_saves(l_confirmed);
}

void netplay_new_interrupt_end(void)
{
    if (!l_active || l_ended)
        return;

    if (l_restore != NULL)
    {
        /* Not from inside an instruction or a memory access (an MTC0, an
         * ERET, an MI register write): the recompiled code that called it
         * writes the guest registers it still holds back afterwards, over
         * the restored ones. Nor between the execution plugin's Execute and
         * ExecuteDone for a hooked instruction (a hooked branch takes
         * interrupts): its ExecuteDone would come after its restored state.
         * Restore at the next interrupt check instead, which this asks for
         * right away, again whenever the check it asked for came at another
         * such point (the event only exists in the abandoned timeline:
         * restoring replaces the event queue). */
        if (g_dev.r4300.cp0.interrupt_unsafe_state & (INTR_UNSAFE_R4300 | INTR_UNSAFE_CALL | INTR_UNSAFE_EXECUTION))
        {
            if (get_event(&g_dev.r4300.cp0.q, CHECK_INT) == NULL)
                add_interrupt_event(&g_dev.r4300.cp0, CHECK_INT, 0);
            return;
        }

        restore_snapshot(l_restore, l_restore_selftest);
        l_restore = NULL;
        l_restore_selftest = 0;
        return;
    }

    if (l_snapshot_wanted)
    {
        /* too late once the game read this frame's input */
        if (l_read_this_frame)
        {
            l_snapshot_wanted = 0;
            ++l_snapshots_missed;
            return;
        }

        /* An RSP task is running (its plugin holds state of its own), the
         * RDP is in the middle of a command list (the video plugin holds
         * its state, which restoring RDRAM can't put back), or the CPU is
         * somewhere savestates can't be taken (also between the execution
         * plugin's Execute and ExecuteDone): wait for the next interrupt */
        if (g_dev.r4300.cp0.interrupt_unsafe_state ||
            !(g_dev.sp.regs[SP_STATUS_REG] & SP_STATUS_HALT) ||
            l_rdp_busy)
            return;

        if (take_snapshot())
            ++l_snapshots_taken;
        else
            ++l_snapshots_missed;
        l_snapshot_wanted = 0;
        if ((l_snapshots_taken + l_snapshots_missed) % 600 == 0)
        {
            DebugMessage(M64MSG_VERBOSE, "Netplay: %u frames saved for rollback, %u couldn't be",
                         l_snapshots_taken, l_snapshots_missed);
        }
    }
}

static time_t netplay_new_get_time(void* clock)
{
    unsigned int rate = g_dev.vi.expected_refresh_rate;
    (void)clock;

    if (rate == 0)
        rate = 60;

    return (time_t)(l_start_time + (int64_t)(l_frame / rate));
}

const struct clock_backend_interface g_iclock_netplay_new =
{
    netplay_new_get_time
};
