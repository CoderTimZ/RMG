#include "PluginAPI.h"
#include "Types.h"
#include "mupenplus/GLideN64_mupenplus.h"
#include "N64.h"
#include "gDP.h"
#include "gSP.h"
#include "RDP.h"
#include "VI.h"
#include <cstring>
#include <type_traits>

extern "C" {

EXPORT int CALL RomOpen(void)
{
	if (rdram_size != nullptr)
		RDRAMSize = *rdram_size - 1;
	else
		RDRAMSize = 0;

	return api().RomOpen();
}

EXPORT m64p_error CALL PluginGetVersion(
	m64p_plugin_type * _PluginType,
	int * _PluginVersion,
	int * _APIVersion,
	const char ** _PluginNamePtr,
	int * _Capabilities
)
{
	return api().PluginGetVersion(_PluginType, _PluginVersion, _APIVersion, _PluginNamePtr, _Capabilities);
}

EXPORT m64p_error CALL PluginStartup(
	m64p_dynlib_handle CoreLibHandle,
	void *Context,
	void (*DebugCallback)(void *, int, const char *)
)
{
	return api().PluginStartup(CoreLibHandle, Context, DebugCallback);
}

#ifdef M64P_GLIDENUI
EXPORT m64p_error CALL PluginConfig(void* parent)
{
	return api().PluginConfig(parent);
}
#endif // M64P_GLIDENUI

EXPORT m64p_error CALL PluginShutdown(void)
{
	return api().PluginShutdown();
}

EXPORT void CALL ReadScreen2(void *dest, int *width, int *height, int front)
{
	api().ReadScreen2(dest, width, height, front);
}

EXPORT void CALL SetRenderingCallback(void (*callback)(int))
{
	api().SetRenderingCallback(callback);
}

EXPORT void CALL ResizeVideoOutput(int width, int height)
{
	api().ResizeVideoOutput(width, height);
}

// Optional, for RMG's new netplay: the RDP's and the display list parser's
// state outside RDRAM (the settings that commands set, which carry over from
// one display list to the next, and TMEM). Rollback saves and restores it with
// the machine: a frame run again must start with the settings it had the first
// time, not the ones later frames left (a game's first display list can rely on
// the scissor being 0, for example). The frame buffers on the GPU aren't part
// of it; with the new netplay nothing is copied from them to RDRAM.
static_assert(std::is_trivially_copyable<gDPInfo>::value && std::is_trivially_copyable<gSPInfo>::value &&
              std::is_trivially_copyable<VIInfo>::value,
              "the rollback state is copied as bytes");

// Of the command list buffer (for an LLE RSP), only what's left of a command
// that hasn't arrived whole: commands are run as they arrive, and the buffer
// starts over once none is left.
struct RdpListState
{
	u32 w0, w1, w2, w3;
	u32 cmd_ptr, cmd_cur;
	u32 pending[64];
};

EXPORT unsigned int CALL RdpStateSize(void)
{
	return unsigned(sizeof(gDP) + sizeof(gSP) + sizeof(TMEM) + sizeof(RdpListState) + sizeof(VI));
}

EXPORT void CALL SaveRdpState(void *data)
{
	u8 * bytes = static_cast<u8 *>(data);
	// (the pointers in gDP and gSP point into gDP itself, which stays where it is)
	memcpy(bytes, &gDP, sizeof(gDP));
	bytes += sizeof(gDP);
	memcpy(bytes, &gSP, sizeof(gSP));
	bytes += sizeof(gSP);
	memcpy(bytes, TMEM, sizeof(TMEM));
	bytes += sizeof(TMEM);
	{
		RdpListState list = { RDP.w0, RDP.w1, RDP.w2, RDP.w3, RDP.cmd_ptr, RDP.cmd_cur, {} };
		for (u32 i = 0; i < 64; ++i)
			list.pending[i] = RDP.cmd_data[(RDP.cmd_cur + i) & maxCMDMask];
		memcpy(bytes, &list, sizeof(list));
		bytes += sizeof(list);
	}
	memcpy(bytes, &VI, sizeof(VI));
}

EXPORT void CALL LoadRdpState(const void *data)
{
	const u8 * bytes = static_cast<const u8 *>(data);
	memcpy(&gDP, bytes, sizeof(gDP));
	bytes += sizeof(gDP);
	memcpy(&gSP, bytes, sizeof(gSP));
	bytes += sizeof(gSP);
	memcpy(TMEM, bytes, sizeof(TMEM));
	bytes += sizeof(TMEM);
	{
		RdpListState list;
		memcpy(&list, bytes, sizeof(list));
		bytes += sizeof(list);
		RDP.w0 = list.w0;
		RDP.w1 = list.w1;
		RDP.w2 = list.w2;
		RDP.w3 = list.w3;
		RDP.cmd_ptr = list.cmd_ptr;
		RDP.cmd_cur = list.cmd_cur;
		for (u32 i = 0; i < 64; ++i)
			RDP.cmd_data[(RDP.cmd_cur + i) & maxCMDMask] = list.pending[i];
	}
	memcpy(&VI, bytes, sizeof(VI));

	// what the GPU was set up with came from other settings
	gDP.changed = ~0U;
	gSP.changed = ~0U;
}

} // extern "C"
