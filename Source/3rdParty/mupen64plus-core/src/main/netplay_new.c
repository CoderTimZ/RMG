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
#include "device/device.h"
#include "main/main.h"
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
 * the RSP's memory, and RDRAM in 16 KB pages, by address), with the VI's
 * origin and width, which show where the frame buffer being displayed is.
 * Frames are numbered the same on every machine, so comparing two players'
 * logs shows the first frame and the memory that differ. */
static void log_state_hash(unsigned int frame, uint64_t sent, char where)
{
    static int enabled = -1;
    static char line[1024 * 9 + 256];
    size_t length;
    size_t page;
    const size_t page_size = 0x4000;
    const uint8_t* rdram = (const uint8_t*)g_dev.rdram.dram;

    if (enabled < 0)
    {
        const char* value = getenv("RMG_NETPLAY_NEW_HASH_LOG");
        enabled = value != NULL && value[0] == '1';
    }
    if (!enabled)
        return;

    /* the hash that was sent, where it was taken (at a full sync or at the
     * end of the frame), and the same hash taken again now, which differs
     * when something (a GPU plugin) is still writing RDRAM */
    length = (size_t)snprintf(line, sizeof(line), "hash %u at %c sent %016llx again %016llx vi %08x %u cp0 %08x sp %08x rdram",
                              frame, where,
                              (unsigned long long)sent, (unsigned long long)state_hash(),
                              (unsigned int)g_dev.vi.regs[VI_ORIGIN_REG],
                              (unsigned int)g_dev.vi.regs[VI_WIDTH_REG],
                              (unsigned int)XXH3_64bits(r4300_cp0_regs(&g_dev.r4300.cp0), CP0_REGS_COUNT * sizeof(uint32_t)),
                              (unsigned int)XXH3_64bits(g_dev.sp.mem, SP_MEM_SIZE));
    for (page = 0; page * page_size < g_dev.rdram.dram_size && length + 10 < sizeof(line); ++page)
    {
        length += (size_t)snprintf(line + length, sizeof(line) - length, " %08x",
                                   (unsigned int)XXH3_64bits(rdram + page * page_size, page_size));
    }
    DebugMessage(M64MSG_VERBOSE, "%s", line);
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
        l_callbacks.frame_hash(l_callbacks.context, l_hash_first + i, hash);
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
    int skip_limiter;

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

    if (l_callbacks.frame_hash != NULL)
    {
        if (l_hash_count == 0)
            l_hash_first = l_frame;
        ++l_hash_count;

        /* when the RDP is idle (or an HLE graphics plugin has drawn
         * everything already) RDRAM is complete now; a game that doesn't
         * full sync for a long time gets a hash anyway */
        if (!l_rdp_busy || l_hash_count >= MAX_PENDING_HASHES)
        {
            send_hashes('f');
        }
    }

    {
        unsigned int rate = g_dev.vi.expected_refresh_rate != 0 ? g_dev.vi.expected_refresh_rate : 60;
        skip_limiter = l_callbacks.frame_end(l_callbacks.context, l_frame, frame_busy_us(), 1000000 / rate);
    }
    ++l_frame;

    if (skip_limiter < 0)
    {
        end_session("the session ended");
        return 0;
    }

    return skip_limiter;
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
