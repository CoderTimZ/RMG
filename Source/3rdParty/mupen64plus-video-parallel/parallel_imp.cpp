// Adapted from https://github.com/Themaister/parallel-rdp/blob/master/integration_example.cpp
#include "vulkan_headers.hpp"
#include "parallel_imp.h"
#include "screen.h"
#include <memory>
#include <vector>
#include "rdp_device.hpp"
#include "wsi.hpp"
#include "wsi_platform.hpp"

static int cmd_cur;
static int cmd_ptr;

// For estimating how long the RDP takes to reach each full sync (DPC_CLOCK_REG, which the core delays the DP interrupt
// by): the area of the most recent scissor, and the largest scissor area set since the last full sync (0 if none)
static uint32_t last_scissor_area;
static uint32_t scissor_area_since_sync;
static uint32_t cmd_data[0x00040000 >> 2];

static std::unique_ptr<RDP::CommandProcessor> processor;
static std::unique_ptr<Vulkan::WSI> wsi;
static std::unique_ptr<MupenWSIPlatform> wsi_platform;

int32_t vk_rescaling;
bool vk_ssreadbacks;
bool vk_ssdither;
uint32_t vk_overscan_left;
uint32_t vk_overscan_right;
uint32_t vk_overscan_top;
uint32_t vk_overscan_bottom;
bool vk_overscan_enable;
uint32_t vk_vertical_stretch;
uint32_t vk_downscaling_steps;
bool vk_native_texture_lod;
bool vk_native_tex_rect;
bool vk_divot_filter, vk_gamma_dither;
bool vk_vi_aa, vk_vi_scale, vk_dither_filter;
bool vk_interlacing;
int32_t vk_window_scale;

// Whether the VI puts out a PAL (576-line) picture rather than an NTSC (480-line) one: guessed from the ROM header
// until the VI's registers say
static bool output_is_pal;

// The VI's horizontal scale (2.10 fixed point: framebuffer pixels per pixel of its 640-wide output), which gives the
// game's horizontal resolution: 0x200 for 320-wide games (the usual case, assumed until the VI says), 0x400 for 640
static uint32_t output_x_scale;

// A change to output_is_pal or output_x_scale seen in the VI's registers, and for how many frames in a row, so the
// window isn't resized for a frame or two of a passing mode
static bool pending_is_pal;
static uint32_t pending_x_scale;
static uint32_t pending_frames;

// Where the last frame was drawn on the swapchain, so screenshots leave out the letterbox or pillarbox around it
static VkRect2D frame_rect;

static const unsigned cmd_len_lut[64] = {
	1, 1, 1, 1, 1, 1, 1, 1, 4, 6, 12, 14, 12, 14, 20, 22,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1,  1,  1,
	1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1,  1,  1,  1,  1,  1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1,  1,  1,
};

static const uint32_t vertex_spirv[] =
		{0x07230203,0x00010000,0x000d000a,0x00000034,
		 0x00000000,0x00020011,0x00000001,0x0006000b,
		 0x00000001,0x4c534c47,0x6474732e,0x3035342e,
		 0x00000000,0x0003000e,0x00000000,0x00000001,
		 0x0008000f,0x00000000,0x00000004,0x6e69616d,
		 0x00000000,0x00000008,0x00000016,0x0000002b,
		 0x00040047,0x00000008,0x0000000b,0x0000002a,
		 0x00050048,0x00000014,0x00000000,0x0000000b,
		 0x00000000,0x00050048,0x00000014,0x00000001,
		 0x0000000b,0x00000001,0x00050048,0x00000014,
		 0x00000002,0x0000000b,0x00000003,0x00050048,
		 0x00000014,0x00000003,0x0000000b,0x00000004,
		 0x00030047,0x00000014,0x00000002,0x00040047,
		 0x0000002b,0x0000001e,0x00000000,0x00020013,
		 0x00000002,0x00030021,0x00000003,0x00000002,
		 0x00040015,0x00000006,0x00000020,0x00000001,
		 0x00040020,0x00000007,0x00000001,0x00000006,
		 0x0004003b,0x00000007,0x00000008,0x00000001,
		 0x0004002b,0x00000006,0x0000000a,0x00000000,
		 0x00020014,0x0000000b,0x00030016,0x0000000f,
		 0x00000020,0x00040017,0x00000010,0x0000000f,
		 0x00000004,0x00040015,0x00000011,0x00000020,
		 0x00000000,0x0004002b,0x00000011,0x00000012,
		 0x00000001,0x0004001c,0x00000013,0x0000000f,
		 0x00000012,0x0006001e,0x00000014,0x00000010,
		 0x0000000f,0x00000013,0x00000013,0x00040020,
		 0x00000015,0x00000003,0x00000014,0x0004003b,
		 0x00000015,0x00000016,0x00000003,0x0004002b,
		 0x0000000f,0x00000017,0xbf800000,0x0004002b,
		 0x0000000f,0x00000018,0x00000000,0x0004002b,
		 0x0000000f,0x00000019,0x3f800000,0x0007002c,
		 0x00000010,0x0000001a,0x00000017,0x00000017,
		 0x00000018,0x00000019,0x00040020,0x0000001b,
		 0x00000003,0x00000010,0x0004002b,0x00000006,
		 0x0000001f,0x00000001,0x0004002b,0x0000000f,
		 0x00000023,0x40400000,0x0007002c,0x00000010,
		 0x00000024,0x00000017,0x00000023,0x00000018,
		 0x00000019,0x0007002c,0x00000010,0x00000027,
		 0x00000023,0x00000017,0x00000018,0x00000019,
		 0x00040017,0x00000029,0x0000000f,0x00000002,
		 0x00040020,0x0000002a,0x00000003,0x00000029,
		 0x0004003b,0x0000002a,0x0000002b,0x00000003,
		 0x0004002b,0x0000000f,0x0000002f,0x3f000000,
		 0x0005002c,0x00000029,0x00000033,0x0000002f,
		 0x0000002f,0x00050036,0x00000002,0x00000004,
		 0x00000000,0x00000003,0x000200f8,0x00000005,
		 0x0004003d,0x00000006,0x00000009,0x00000008,
		 0x000500aa,0x0000000b,0x0000000c,0x00000009,
		 0x0000000a,0x000300f7,0x0000000e,0x00000000,
		 0x000400fa,0x0000000c,0x0000000d,0x0000001d,
		 0x000200f8,0x0000000d,0x00050041,0x0000001b,
		 0x0000001c,0x00000016,0x0000000a,0x0003003e,
		 0x0000001c,0x0000001a,0x000200f9,0x0000000e,
		 0x000200f8,0x0000001d,0x000500aa,0x0000000b,
		 0x00000020,0x00000009,0x0000001f,0x000300f7,
		 0x00000022,0x00000000,0x000400fa,0x00000020,
		 0x00000021,0x00000026,0x000200f8,0x00000021,
		 0x00050041,0x0000001b,0x00000025,0x00000016,
		 0x0000000a,0x0003003e,0x00000025,0x00000024,
		 0x000200f9,0x00000022,0x000200f8,0x00000026,
		 0x00050041,0x0000001b,0x00000028,0x00000016,
		 0x0000000a,0x0003003e,0x00000028,0x00000027,
		 0x000200f9,0x00000022,0x000200f8,0x00000022,
		 0x000200f9,0x0000000e,0x000200f8,0x0000000e,
		 0x00050041,0x0000001b,0x0000002c,0x00000016,
		 0x0000000a,0x0004003d,0x00000010,0x0000002d,
		 0x0000002c,0x0007004f,0x00000029,0x0000002e,
		 0x0000002d,0x0000002d,0x00000000,0x00000001,
		 0x0005008e,0x00000029,0x00000030,0x0000002e,
		 0x0000002f,0x00050081,0x00000029,0x00000032,
		 0x00000030,0x00000033,0x0003003e,0x0000002b,
		 0x00000032,0x000100fd,0x00010038};

static const uint32_t fragment_spirv[] =
		{0x07230203,0x00010000,0x000d000a,0x00000015,
		 0x00000000,0x00020011,0x00000001,0x0006000b,
		 0x00000001,0x4c534c47,0x6474732e,0x3035342e,
		 0x00000000,0x0003000e,0x00000000,0x00000001,
		 0x0007000f,0x00000004,0x00000004,0x6e69616d,
		 0x00000000,0x00000009,0x00000011,0x00030010,
		 0x00000004,0x00000007,0x00040047,0x00000009,
		 0x0000001e,0x00000000,0x00040047,0x0000000d,
		 0x00000022,0x00000000,0x00040047,0x0000000d,
		 0x00000021,0x00000000,0x00040047,0x00000011,
		 0x0000001e,0x00000000,0x00020013,0x00000002,
		 0x00030021,0x00000003,0x00000002,0x00030016,
		 0x00000006,0x00000020,0x00040017,0x00000007,
		 0x00000006,0x00000004,0x00040020,0x00000008,
		 0x00000003,0x00000007,0x0004003b,0x00000008,
		 0x00000009,0x00000003,0x00090019,0x0000000a,
		 0x00000006,0x00000001,0x00000000,0x00000000,
		 0x00000000,0x00000001,0x00000000,0x0003001b,
		 0x0000000b,0x0000000a,0x00040020,0x0000000c,
		 0x00000000,0x0000000b,0x0004003b,0x0000000c,
		 0x0000000d,0x00000000,0x00040017,0x0000000f,
		 0x00000006,0x00000002,0x00040020,0x00000010,
		 0x00000001,0x0000000f,0x0004003b,0x00000010,
		 0x00000011,0x00000001,0x0004002b,0x00000006,
		 0x00000013,0x00000000,0x00050036,0x00000002,
		 0x00000004,0x00000000,0x00000003,0x000200f8,
		 0x00000005,0x0004003d,0x0000000b,0x0000000e,
		 0x0000000d,0x0004003d,0x0000000f,0x00000012,
		 0x00000011,0x00070058,0x00000007,0x00000014,
		 0x0000000e,0x00000012,0x00000002,0x00000013,
		 0x0003003e,0x00000009,0x00000014,0x000100fd,
		 0x00010038};

static RDP::ScanoutOptions::CropRect get_crop_rect()
{
	RDP::ScanoutOptions::CropRect crop;
	crop.left = vk_overscan_left;
	crop.right = vk_overscan_right;
	crop.top = vk_overscan_top;
	crop.bottom = vk_overscan_bottom;
	crop.enable = vk_overscan_enable;
	if (vk_vertical_stretch) {
		crop.top = vk_vertical_stretch;
		crop.bottom = vk_vertical_stretch;
		crop.enable = true;
	}
	return crop;
}

// The size of the frame on screen at 1x, before any widescreen stretch: the VI's 640-pixel-wide picture with 480
// (NTSC) or 576 (PAL) lines, less the crop, whose top and bottom count 240p lines (so two lines each here)
static void get_frame_size(uint32_t *width, uint32_t *height)
{
	RDP::ScanoutOptions::CropRect crop = get_crop_rect();
	*width = RDP::VI_SCANOUT_WIDTH;
	*height = output_is_pal ? RDP::VI_V_RES_PAL : RDP::VI_V_RES_NTSC;

	// parallel-rdp skips a crop that would leave nothing
	if (crop.enable && crop.left + crop.right < *width && (crop.top + crop.bottom) * 2 < *height) {
		*width -= crop.left + crop.right;
		*height -= (crop.top + crop.bottom) * 2;
	}
}

// The frame's size in the game's own pixels, before upscaling: its width from the VI's horizontal scale (320 for
// most games, less the crop), its height from the frame's shape on a TV (so 240 lines for a 320x240 game)
static void get_pixel_size(uint32_t *width, uint32_t *height)
{
	uint32_t frame_width, frame_height;
	get_frame_size(&frame_width, &frame_height);

	*width = (frame_width * output_x_scale + 0x200) / 0x400;
	if (*width == 0)
		*width = 1;

	*height = (*width * frame_height + frame_width / 2) / frame_width;
	if (*height == 0)
		*height = 1;
}

// The window size that shows the game's pixels 1:1 at its upscaled resolution, times the window scale (so at 1x with
// no upscaling, a 320x240 game gets a 320x240 window). Widescreen stretches it to 4/3 its width.
static void get_window_size(uint32_t *width, uint32_t *height)
{
	uint32_t pixel_width, pixel_height;
	get_pixel_size(&pixel_width, &pixel_height);

	uint32_t upscale = vk_rescaling > 0 ? uint32_t(vk_rescaling) : 1;
	uint32_t scale = (vk_window_scale > 0 ? uint32_t(vk_window_scale) : 1) * upscale;
	*width = window_widescreen ? (pixel_width * scale * 4 + 1) / 3 : pixel_width * scale;
	*height = pixel_height * scale;
}

// Whether the ROM is for a PAL console, from the country code at byte 0x3E of its header. The core keeps the ROM as
// native-endian 32-bit words, so on a little-endian host that byte sits at 0x3E ^ 3.
static bool rom_is_pal()
{
	if (gfx.HEADER == NULL)
		return false;

	switch (gfx.HEADER[0x3E ^ 3])
	{
	case 'D': case 'F': case 'I': case 'P': case 'S': case 'U': case 'X': case 'Y':
		return true;
	default:
		return false;
	}
}

static uint32_t distance(uint32_t a, uint32_t b)
{
	return a > b ? a - b : b - a;
}

// Fits the frame into the surface (the viewport passed in), centered with letterboxes or pillarboxes as needed.
// Outside widescreen, the frame snaps to a whole multiple of its size in the game's pixels when the surface is within
// a couple of pixels of one, so it isn't scaled by a fraction of a pixel: at fractional display scaling (125%, 150%)
// the window is sized in scaled units and can come out a pixel larger than asked for. Offsets are whole pixels so the
// frame stays on the pixel grid. Returns whether it snapped.
static bool calculate_viewport(float* x, float* y, float* width, float* height)
{
	uint32_t frame_width, frame_height;
	get_frame_size(&frame_width, &frame_height);

	uint32_t pixel_width, pixel_height;
	get_pixel_size(&pixel_width, &pixel_height);
	bool snapped = false;

	// the shape the frame is shown at
	uint64_t display_width = window_widescreen ? uint64_t(frame_width) * 4 : frame_width;
	uint64_t display_height = window_widescreen ? uint64_t(frame_height) * 3 : frame_height;

	uint32_t surface_width = uint32_t(*width);
	uint32_t surface_height = uint32_t(*height);
	uint32_t w = surface_width;
	uint32_t h = surface_height;

	if (surface_width * display_height > surface_height * display_width)
		w = uint32_t(surface_height * display_width / display_height);
	else
		h = uint32_t(surface_width * display_height / display_width);

	if (!window_widescreen) {
		uint32_t multiple = (w + pixel_width / 2) / pixel_width;
		if (multiple == 0)
			multiple = 1;

		uint32_t snapped_w = pixel_width * multiple;
		uint32_t snapped_h = pixel_height * multiple;
		if (snapped_w <= surface_width && snapped_h <= surface_height &&
		    distance(w, snapped_w) <= 2 && distance(h, snapped_h) <= 2) {
			w = snapped_w;
			h = snapped_h;
			snapped = true;
		}
	}

	*x += float((surface_width - w) / 2);
	*y += float((surface_height - h) / 2);
	*width = float(w);
	*height = float(h);
	return snapped;
}

// When the scanout image is a whole number of times the viewport's size along an axis (a 320-wide game comes out of
// the VI at 640, doubled), shifts the viewport so nearest sampling lands on the first of each run of image pixels:
// the game's own pixel, rather than the VI's blend of it with the next. A viewport pixel's center at i + 0.5 samples
// the image at (i + 0.5 - offset) * ratio, which is the center of image pixel i * ratio when offset = 0.5 - 0.5 / ratio.
// Returns whether the image fits the viewport in whole pixels along this axis (a whole ratio either way).
static bool align_to_image(float *offset, float viewport_size, uint32_t image_size)
{
	uint32_t size = uint32_t(viewport_size);
	if (size == 0)
		return false;

	if (image_size >= size) {
		if (image_size % size != 0)
			return false;

		*offset += 0.5f - 0.5f / float(image_size / size);
		return true;
	}

	return size % image_size == 0;
}

static void render_frame(Vulkan::Device &device)
{
	RDP::ScanoutOptions opts = {};
	opts.persist_frame_on_invalid_input = true;
	opts.vi.aa = vk_vi_aa;
	opts.vi.scale = vk_vi_scale;
	opts.vi.dither_filter = vk_dither_filter;
	opts.vi.divot_filter = vk_divot_filter;
	opts.vi.gamma_dither = vk_gamma_dither;
	opts.blend_previous_frame = vk_interlacing;
	opts.upscale_deinterlacing = !vk_interlacing;
	opts.downscale_steps = vk_downscaling_steps;
	opts.crop_rect = get_crop_rect();
	Vulkan::ImageHandle image = processor->scanout(opts);

	// Normally reflection is automated.
	Vulkan::ResourceLayout vertex_layout = {};
	Vulkan::ResourceLayout fragment_layout = {};
	fragment_layout.output_mask = 1 << 0;
	fragment_layout.sets[0].sampled_image_mask = 1 << 0;

	// This request is cached.
	auto *program = device.request_program(vertex_spirv, sizeof(vertex_spirv),
	                                       fragment_spirv, sizeof(fragment_spirv),
	                                       &vertex_layout,
	                                       &fragment_layout);

	// Blit image on screen.
	auto cmd = device.request_command_buffer();
	{
		auto rp = device.get_swapchain_render_pass(Vulkan::SwapchainRenderPass::ColorOnly);
		cmd->begin_render_pass(rp);
		if (image.get() != NULL)
		{
			VkViewport vp = cmd->get_viewport();
			bool snapped = calculate_viewport(&vp.x, &vp.y, &vp.width, &vp.height);
			frame_rect = { { int32_t(vp.x), int32_t(vp.y) }, { uint32_t(vp.width), uint32_t(vp.height) } };

			// At a whole multiple of the game's pixels, sample them 1:1 (or repeat them) with nearest sampling.
			// Otherwise the frame is scaled by a fraction: widescreen stretches it by 4/3, which whole pixels can't
			// do, and a large upscale shrunk into a small window looks better blended.
			bool whole_pixels = false;
			if (snapped) {
				bool whole_x = align_to_image(&vp.x, vp.width, image->get_width());
				bool whole_y = align_to_image(&vp.y, vp.height, image->get_height());
				whole_pixels = whole_x && whole_y;
			}

			auto sampler = !whole_pixels && (window_widescreen || vk_rescaling >= 4)
				? Vulkan::StockSampler::LinearClamp
				: Vulkan::StockSampler::NearestClamp;

			cmd->set_program(program);

			// Basic default render state.
			cmd->set_opaque_state();
			cmd->set_depth_test(false, false);
			cmd->set_cull_mode(VK_CULL_MODE_NONE);

			cmd->set_texture(0, 0, image->get_view(), sampler);
			cmd->set_viewport(vp);

			// The vertices are constants in the shader.
			// Draws fullscreen quad using oversized triangle.
			cmd->draw(3);
		}
		cmd->end_render_pass();
	}
	device.submit(cmd);
}

void vk_rasterize()
{
	processor->set_vi_register(RDP::VIRegister::Control, *GET_GFX_INFO(VI_STATUS_REG));
	processor->set_vi_register(RDP::VIRegister::Origin, *GET_GFX_INFO(VI_ORIGIN_REG));
	processor->set_vi_register(RDP::VIRegister::Width, *GET_GFX_INFO(VI_WIDTH_REG));
	processor->set_vi_register(RDP::VIRegister::Intr, *GET_GFX_INFO(VI_INTR_REG));
	processor->set_vi_register(RDP::VIRegister::VCurrentLine, *GET_GFX_INFO(VI_V_CURRENT_LINE_REG));
	processor->set_vi_register(RDP::VIRegister::Timing, *GET_GFX_INFO(VI_V_BURST_REG));
	processor->set_vi_register(RDP::VIRegister::VSync, *GET_GFX_INFO(VI_V_SYNC_REG));
	processor->set_vi_register(RDP::VIRegister::HSync, *GET_GFX_INFO(VI_H_SYNC_REG));
	processor->set_vi_register(RDP::VIRegister::Leap, *GET_GFX_INFO(VI_LEAP_REG));
	processor->set_vi_register(RDP::VIRegister::HStart, *GET_GFX_INFO(VI_H_START_REG));
	processor->set_vi_register(RDP::VIRegister::VStart, *GET_GFX_INFO(VI_V_START_REG));
	processor->set_vi_register(RDP::VIRegister::VBurst, *GET_GFX_INFO(VI_V_BURST_REG));
	processor->set_vi_register(RDP::VIRegister::XScale, *GET_GFX_INFO(VI_X_SCALE_REG));
	processor->set_vi_register(RDP::VIRegister::YScale, *GET_GFX_INFO(VI_Y_SCALE_REG));

	// Follow the VI between NTSC and PAL timings (the check parallel-rdp makes) and between horizontal resolutions,
	// resizing the window to fit the frame once a change has held for a few frames. A register of 0 means the game
	// hasn't set that part of the VI up yet.
	uint32_t v_sync = *GET_GFX_INFO(VI_V_SYNC_REG) & 0x3ff;
	uint32_t x_scale = *GET_GFX_INFO(VI_X_SCALE_REG) & 0xfff;
	bool pal = v_sync != 0 ? v_sync > RDP::VI_V_SYNC_NTSC + 25 : output_is_pal;
	if (x_scale == 0)
		x_scale = output_x_scale;

	if (pal == output_is_pal && x_scale == output_x_scale)
	{
		pending_frames = 0;
	}
	else if (pending_frames == 0 || pal != pending_is_pal || x_scale != pending_x_scale)
	{
		pending_is_pal = pal;
		pending_x_scale = x_scale;
		pending_frames = 1;
	}
	else if (++pending_frames >= 10)
	{
		output_is_pal = pal;
		output_x_scale = x_scale;
		pending_frames = 0;

		uint32_t width, height;
		get_window_size(&width, &height);
		screen_resize_window(width, height);
	}

	RDP::Quirks quirks;
	quirks.set_native_texture_lod(vk_native_texture_lod);
	quirks.set_native_resolution_tex_rect(vk_native_tex_rect);
	processor->set_quirks(quirks);

	auto &device = wsi->get_device();
	if (wsi->has_swapchain_image())
		render_frame(device);
	else
	{
		// A frame skipped for want of a free image (without VSync) has no
		// scanout, which is what submits the RDP's batched work. Submit it
		// anyway: ending the frame waits for every command buffer to be
		// submitted, and the command thread holds its open one until more
		// RDP commands come, so the game hung.
		processor->flush();
	}
	(*render_callback)(1);
	wsi->end_frame();
	wsi->begin_frame();
}

// The part of the swapchain image to take screenshots of: where the last frame was drawn, or the whole image before
// any frame was, or if the swapchain has since shrunk past it
static VkRect2D get_screenshot_rect()
{
	Vulkan::ImageView* image = &wsi->get_device().get_swapchain_view();
	uint32_t image_width = image->get_image().get_width();
	uint32_t image_height = image->get_image().get_height();

	if (frame_rect.extent.width == 0 || frame_rect.extent.height == 0 ||
	    uint32_t(frame_rect.offset.x) + frame_rect.extent.width > image_width ||
	    uint32_t(frame_rect.offset.y) + frame_rect.extent.height > image_height)
		return { { 0, 0 }, { image_width, image_height } };

	return frame_rect;
}

void vk_get_screen_size(uint32_t* width, uint32_t* height)
{
	if (!wsi)
	{
		*width = window_width;
		*height = window_height;
		return;
	}

	VkRect2D rect = get_screenshot_rect();
	*width = rect.extent.width;
	*height = rect.extent.height;
}

void vk_read_screen(unsigned char* dest)
{
	Vulkan::Fence fence;
	Vulkan::BufferHandle buffer;
	Vulkan::BufferCreateInfo info = {};
	Vulkan::ImageView* image = &wsi->get_device().get_swapchain_view();
	VkRect2D rect = get_screenshot_rect();
	uint32_t width = rect.extent.width;
	uint32_t height = rect.extent.height;
	info.size = width * height * sizeof(uint32_t);
	info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	info.domain = Vulkan::BufferDomain::CachedHost;
	if (!buffer || buffer->get_create_info().size < info.size)
		buffer = wsi->get_device().create_buffer(info);

	auto cmd = wsi->get_device().request_command_buffer();
	cmd->copy_image_to_buffer(*buffer, image->get_image(), 0, { rect.offset.x, rect.offset.y, 0 }, { width, height, 1 }, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });
	cmd->barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
	             VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);

	fence.reset();
	wsi->get_device().submit(cmd, &fence);

	fence->wait();
	unsigned char *image_buffer = (unsigned char*)wsi->get_device().map_host_buffer(*buffer, Vulkan::MEMORY_ACCESS_READ_BIT);

	image_buffer = image_buffer + info.size;
	for (int i = 0; i < height; ++i)
	{
		image_buffer -= width * 4;
		for (int j = 0; j < width; ++j)
		{
			auto image_format = image->get_format();
			switch (image_format) {
				case VK_FORMAT_B8G8R8A8_UNORM:
					dest[0] = image_buffer[2];
					dest[1] = image_buffer[1];
					dest[2] = image_buffer[0];
					break;
				case VK_FORMAT_R8G8B8A8_UNORM:
					dest[0] = image_buffer[0];
					dest[1] = image_buffer[1];
					dest[2] = image_buffer[2];
					break;
				case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
					dest[0] = image_buffer[3];
					dest[1] = image_buffer[2];
					dest[2] = image_buffer[1];
					break;
				case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
				{
					uint32_t pixel = *(uint32_t*) image_buffer;
					dest[0] = pixel >> 2;
					dest[1] = pixel >> 12;
					dest[2] = pixel >> 22;
					break;
				}
				case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
				{
					uint32_t pixel = *(uint32_t*) image_buffer;
					dest[0] = pixel >> 22;
					dest[1] = pixel >> 12;
					dest[2] = pixel >> 2;
					break;
				}
				default:
					LOGE("Encountered unknown image format in vk_read_screen: %d\n", image_format);
					wsi->get_device().unmap_host_buffer(*buffer, Vulkan::MEMORY_ACCESS_READ_BIT);
					return;
			}
			dest += 3;
			image_buffer += 4;
		}
		image_buffer -= width * 4;
	}
	wsi->get_device().unmap_host_buffer(*buffer, Vulkan::MEMORY_ACCESS_READ_BIT);
}

uint32_t viCalculateHorizonalWidth(uint32_t hstart, uint32_t xscale, uint32_t width)
{
	if (xscale == 0)
		return 320;

	uint32_t start = ((hstart & 0x03FF0000) >> 16) & 0x3FF;
	uint32_t end = (hstart & 0x3FF);
	uint32_t delta;
	if (end > start)
		delta = end - start;
	else
		delta = start - end;
	uint32_t scale = (xscale & 0xFFF);

	if (delta == 0) {
		delta = width;
	}

	return (delta * scale) / 0x400;
}

uint32_t viCalculateVerticalHeight(uint32_t vstart, uint32_t yscale)
{
	if (yscale == 0)
		return 240;

	uint32_t start = ((vstart & 0x03FF0000) >> 16) & 0x3FF;
	uint32_t end = (vstart & 0x3FF);
	uint32_t delta;
	if (end > start)
		delta = end - start;
	else
		delta = start - end;
	uint32_t scale = (yscale & 0xFFF);

	return (delta * scale) / 0x800;
}

void vk_process_commands()
{
	const uint32_t DP_CURRENT = *GET_GFX_INFO(DPC_CURRENT_REG) & 0x00FFFFF8;
	const uint32_t DP_END = *GET_GFX_INFO(DPC_END_REG) & 0x00FFFFF8;

	int length = DP_END - DP_CURRENT;
	if (length <= 0)
		return;

	length = unsigned(length) >> 3;
	if ((cmd_ptr + length) & ~(0x0003FFFF >> 3))
		return;

	*GET_GFX_INFO(DPC_STATUS_REG) |= DP_STATUS_PIPE_BUSY | DP_STATUS_START_GCLK;

	uint32_t offset = DP_CURRENT;
	if (*GET_GFX_INFO(DPC_STATUS_REG) & DP_STATUS_XBUS_DMA)
	{
		do
		{
			offset &= 0xFF8;
			cmd_data[2 * cmd_ptr + 0] = *reinterpret_cast<const uint32_t *>(SP_DMEM + offset);
			cmd_data[2 * cmd_ptr + 1] = *reinterpret_cast<const uint32_t *>(SP_DMEM + offset + 4);
			offset += sizeof(uint64_t);
			cmd_ptr++;
		} while (--length > 0);
	}
	else
	{
		if (DP_END > 0x7ffffff || DP_CURRENT > 0x7ffffff)
		{
			return;
		}
		else
		{
			do
			{
				offset &= 0xFFFFF8;
				cmd_data[2 * cmd_ptr + 0] = *reinterpret_cast<const uint32_t *>(DRAM + offset);
				cmd_data[2 * cmd_ptr + 1] = *reinterpret_cast<const uint32_t *>(DRAM + offset + 4);
				offset += sizeof(uint64_t);
				cmd_ptr++;
			} while (--length > 0);
		}
	}

	while (cmd_cur - cmd_ptr < 0)
	{
		uint32_t w1 = cmd_data[2 * cmd_cur];
		uint32_t command = (w1 >> 24) & 63;
		int cmd_length = cmd_len_lut[command];

		if (cmd_ptr - cmd_cur - cmd_length < 0)
		{
			*GET_GFX_INFO(DPC_START_REG) = *GET_GFX_INFO(DPC_CURRENT_REG) = *GET_GFX_INFO(DPC_END_REG);
			return;
		}

		if (command >= 8)
			processor->enqueue_command(cmd_length * 2, &cmd_data[2 * cmd_cur]);

		if (RDP::Op(command) == RDP::Op::SetScissor)
		{
			// Coordinates are 10.2 fixed point: upper left in the first word, lower right in the second
			uint32_t w0 = cmd_data[2 * cmd_cur], w1 = cmd_data[2 * cmd_cur + 1];
			uint32_t xh = (w0 >> 12) & 0xfff, yh = w0 & 0xfff, xl = (w1 >> 12) & 0xfff, yl = w1 & 0xfff;
			uint32_t area = (xl > xh && yl > yh) ? ((xl - xh) >> 2) * ((yl - yh) >> 2) : 0;
			last_scissor_area = area;
			if (area > scissor_area_since_sync)
				scissor_area_since_sync = area;
		}

		if (RDP::Op(command) == RDP::Op::SyncFull)
		{
			uint32_t width = viCalculateHorizonalWidth(*GET_GFX_INFO(VI_H_START_REG), *GET_GFX_INFO(VI_X_SCALE_REG), *GET_GFX_INFO(VI_WIDTH_REG));
			uint32_t height = viCalculateVerticalHeight(*GET_GFX_INFO(VI_V_START_REG), *GET_GFX_INFO(VI_Y_SCALE_REG));
			// Charge the RDP time for the area drawn since the last full sync (the scissor), not the whole screen:
			// split-screen games full sync once per viewport, and charging the full screen for each made them
			// slower than on hardware (e.g. Mario Party 3's four-player split-screen mini-games)
			uint32_t area = width * height;
			uint32_t drawn = scissor_area_since_sync ? scissor_area_since_sync : last_scissor_area;
			if (drawn && drawn < area)
				area = drawn;
			scissor_area_since_sync = 0;
			*GET_GFX_INFO(DPC_CLOCK_REG) = area * 2;

			processor->wait_for_timeline(processor->signal_timeline());

			*gfx.MI_INTR_REG |= DP_INTERRUPT;
			*GET_GFX_INFO(DPC_STATUS_REG) &= ~(DP_STATUS_PIPE_BUSY | DP_STATUS_START_GCLK);
			gfx.CheckInterrupts();
		}

		cmd_cur += cmd_length;
	}

	cmd_ptr = 0;
	cmd_cur = 0;
	*GET_GFX_INFO(DPC_CURRENT_REG) = *GET_GFX_INFO(DPC_END_REG);
	*GET_GFX_INFO(DPC_STATUS_REG) |= DP_STATUS_CBUF_READY;
}

void vk_resize()
{
	MupenWSIPlatform* platform = (MupenWSIPlatform*)&wsi->get_platform();
	platform->do_resize();
}

void vk_destroy()
{
	if (wsi)
		wsi->end_frame();

	processor.reset();
	wsi.reset();
	wsi_platform.reset();

	screen_close();
}

bool vk_init()
{
	if (!screen_init())
		return false;

	/* start the window at the frame's size, so it's shown 1:1 at the window scale */
	frame_rect = {};
	output_is_pal = rom_is_pal();
	output_x_scale = 0x200;
	pending_frames = 0;
	get_window_size(&window_width, &window_height);

	/* we have to create a window before we initialize Vulkan */
	if (!screen_set_mode(window_fullscreen))
		return false;

	/* initialize Vulkan loader */
	if (!::Vulkan::Context::init_loader(nullptr))
		return false;

	Vulkan::Context::SystemHandles handles = {};

	/* initialize WSI */
	wsi.reset(new Vulkan::WSI);
	wsi_platform.reset(new MupenWSIPlatform);
	wsi->set_platform(wsi_platform.get());
	wsi->set_present_mode(window_vsync ? Vulkan::PresentMode::SyncToVBlank : Vulkan::PresentMode::UnlockedMaybeTear);
	wsi->set_backbuffer_format(Vulkan::BackbufferFormat::UNORM);
	if (!wsi->init_simple(1, handles))
	{
		vk_destroy();
		return false;
	}

	uintptr_t aligned_rdram = reinterpret_cast<uintptr_t>(gfx.RDRAM);
	uintptr_t offset = 0;

	if (wsi->get_device().get_device_features().supports_external_memory_host)
	{
		size_t align = wsi->get_device().get_device_features().host_memory_properties.minImportedHostPointerAlignment;
		offset = aligned_rdram & (align - 1);

		if (offset)
		{
			vk_destroy();
			return false;
		}
		aligned_rdram -= offset;
	}

	RDP::CommandProcessorFlags flags = 0;

	switch (vk_rescaling)
	{
	case 1:
		break;
	case 2:
		flags |= RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_2X_BIT;
		break;
	case 4:
		flags |= RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_4X_BIT;
		break;
	case 8:
		flags |= RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_8X_BIT;
		break;
	default:
		break;
	}
	if (vk_rescaling > 1 && vk_ssreadbacks)
		flags |= RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_READ_BACK_BIT;
	if (vk_ssdither)
		flags |= RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_DITHER_BIT;

	processor.reset(new RDP::CommandProcessor(wsi->get_device(), reinterpret_cast<void *>(aligned_rdram),
											 offset, rdram_size, rdram_size / 2, flags));
	if (!processor->device_is_supported())
	{
		vk_destroy();
		return false;
	}

	RDP::Quirks quirks;
	quirks.set_native_texture_lod(vk_native_texture_lod);
	quirks.set_native_resolution_tex_rect(vk_native_tex_rect);
	processor->set_quirks(quirks);

	wsi->begin_frame();
	return true;
}