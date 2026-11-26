#pragma once

#include "gfx_m64p.h"
#include "screen.h"
#include "gfxstructdefs.h"

#ifdef __cplusplus
extern "C"
{
#endif

    extern int32_t vk_rescaling;
    extern bool vk_ssreadbacks;
    extern bool vk_ssdither;

    extern uint32_t vk_overscan_left;
    extern uint32_t vk_overscan_right;
    extern uint32_t vk_overscan_top;
    extern uint32_t vk_overscan_bottom;
    extern bool vk_overscan_enable;
    extern uint32_t vk_vertical_stretch;
    extern uint32_t vk_downscaling_steps;
    extern bool vk_native_texture_lod;
    extern bool vk_native_tex_rect;
    extern bool vk_divot_filter, vk_gamma_dither;
    extern bool vk_vi_aa, vk_vi_scale, vk_dither_filter;
    extern bool vk_interlacing;
    extern bool vk_synchronous;
    extern int32_t vk_window_scale;

    void vk_rasterize();
    void vk_process_commands();
    void vk_wait_idle();
    unsigned vk_state_size();
    void vk_save_state(void *data);
    void vk_load_state(const void *data);
    bool vk_init();
    void vk_destroy();
    void vk_resize();
    void vk_get_screen_size(uint32_t* width, uint32_t* height);
    void vk_read_screen(unsigned char* dest);

#ifdef __cplusplus
}
#endif
