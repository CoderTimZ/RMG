/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - netplay_new.h                                           *
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

/* The new netplay: the core side.
 *
 * The frontend owns the session and the network (see
 * m64p_netplay_new_callbacks in m64p_types.h). This file only connects the
 * core's emulation to those callbacks: it counts frames (one per VI), samples
 * the local controllers once per frame, answers controller reads with the
 * session's input, and routes the data every player has to share through
 * the frontend before emulation starts. It also hashes every frame for
 * desync detection, saves and restores the machine for rollback, and hands
 * the execution plugin's messages over.
 *
 * This is separate from netplay.c, the server-based netplay. */

#ifndef M64P_MAIN_NETPLAY_NEW_H
#define M64P_MAIN_NETPLAY_NEW_H

#include <stddef.h>
#include <stdint.h>

#include "api/m64p_types.h"
#include "backends/api/clock_backend.h"

m64p_error netplay_new_set_callbacks(const m64p_netplay_new_callbacks* callbacks);

/* whether a new netplay session is attached */
int netplay_new_active(void);

/* the frame being emulated: the number of VIs since emulation started, which
 * goes back when frames are re-run after a rollback */
unsigned int netplay_new_frame(void);

/* whether this machine may write save files */
int netplay_new_can_write_saves(void);

/* called when emulation starts, before any other function below */
void netplay_new_start(void);

/* called when emulation stops: writes save files that waited for their
 * frames to be final */
void netplay_new_stop(void);

/* replace the values with the host's */
void netplay_new_sync_settings(m64p_netplay_new_settings* settings);
void netplay_new_sync_data(m64p_netplay_new_sync kind, void* data, size_t size);

/* starts the execution plugin with the host's settings and state
 * (instead of initiate_execution_plugin()) */
void netplay_new_initiate_execution_plugin(void);

/* sets up Controls[] for the session; control_ids receives the local
 * controller driving each port (-1 if none) */
void netplay_new_configure_ports(int control_ids[4]);

/* lets the input plugin see its own controller in Controls[] (it holds the
 * session's ports during netplay) while the core talks to it */
void netplay_new_local_controller_begin(int controller);
void netplay_new_local_controller_end(int controller);

/* answer a controller read on a port; returns 0 if the port has no controller.
 * pak is -1, or the pak the port switches to now (the same frame on every machine). */
int netplay_new_get_input(int port, uint32_t* input, int* pak);

/* called at the end of each VI; returns 1 to skip the speed limiter */
int netplay_new_frame_end(void);

/* Called when a frame starts, after the speed limiter's sleep. */
void netplay_new_frame_start(void);

/* Called before the RDP is given commands, and when it has finished a full
 * sync, when its writes to RDRAM are complete. */
void netplay_new_rdp_list(void);
void netplay_new_rdp_full_sync(void);

/* Rollback: called at the end of gen_interrupt, where the machine's state is
 * whole (the same place savestates are saved). Saves the state for the
 * frame that just started, or goes back to a saved frame. */
void netplay_new_interrupt_end(void);

/* Whether the frame being emulated is re-run after a rollback: it isn't
 * shown, played or rumbled, and nothing waits for it. */
int netplay_new_rerunning(void);

/* The save files (EEPROM, SRAM, FlashRAM, memory paks), which rollback
 * keeps with each saved state. Writing them to disk waits until the game
 * that wrote them is final (see netplay_new_defer_save). */
struct file_storage;
void netplay_new_set_storages(struct file_storage* eep, struct file_storage* sra,
                              struct file_storage* fla, struct file_storage* mpk);

/* Called instead of writing one of those files during the new netplay;
 * returns 0 for any other file, which is written as usual */
int netplay_new_defer_save(struct file_storage* storage);

/* More of the emulated machine's state that savestates don't hold (pak
 * switching, for one), which rollback saves and restores as it is */
void netplay_new_track_state(void* data, size_t size);

/* the session clock: the host's start time plus emulated time */
extern const struct clock_backend_interface g_iclock_netplay_new;

#endif
