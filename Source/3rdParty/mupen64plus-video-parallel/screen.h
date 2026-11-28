#pragma once

#include "m64p_vidext.h"
#include "volk.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif
    bool screen_init();
    void screen_close();
    void screen_toggle_fullscreen();
    bool screen_set_mode(bool fullscreen);
    void screen_resize_window(uint32_t width, uint32_t height);
    // the crop the frontend wants (VidExt_GetCrop), or none
    void screen_get_crop(m64p_video_crop* crop);
    m64p_error screen_get_surface(VkInstance instance, VkSurfaceKHR* surface);
    m64p_error screen_get_instance_extensions(const char** ext[], uint32_t* ext_num);

    extern bool window_fullscreen;
    extern bool window_widescreen;
    extern uint32_t window_width;
    extern uint32_t window_height;
    extern bool window_vsync;

#ifdef __cplusplus
}
#endif