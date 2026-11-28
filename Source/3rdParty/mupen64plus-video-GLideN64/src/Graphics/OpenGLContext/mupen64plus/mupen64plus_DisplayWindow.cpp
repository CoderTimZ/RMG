#include <stdio.h>
#include <cstdlib>
#include <Graphics/Context.h>
#include <Graphics/OpenGLContext/GLFunctions.h>
#include <Graphics/OpenGLContext/opengl_Utils.h>
#include <Graphics/OpenGLContext/ThreadedOpenGl/opengl_Wrapper.h>
#include <mupenplus/GLideN64_mupenplus.h>
#include <GLideN64.h>
#include <Config.h>
#include <N64.h>
#include <VI.h>
#include <cstring>
#include <gSP.h>
#include <Log.h>
#include <Revision.h>
#include <FrameBuffer.h>
#include <GLideNUI/GLideNUI.h>
#include <DisplayWindow.h>

#ifdef VC
#include <bcm_host.h>
#endif

using namespace opengl;

class DisplayWindowMupen64plus : public DisplayWindow
{
public:
	DisplayWindowMupen64plus() {}

private:
	void _setAttributes();
	void _getDisplaySize();

	bool _start() override;
	void _stop() override;
	void _restart() override;
	void _swapBuffers() override;
	void _saveScreenshot() override;
	void _saveBufferContent(graphics::ObjectHandle _fbo, CachedTexture *_pTexture) override;
	bool _resizeWindow() override;
	void _changeWindow() override;
	void _readScreen(void **_pDest, long *_pWidth, long *_pHeight) override {}
	void _readScreen2(void * _dest, int * _width, int * _height, int _front) override;
#ifdef M64P_GLIDENUI
	bool _supportsWithRateFunctions = true;
#endif // M64P_GLIDENUI
	graphics::ObjectHandle _getDefaultFramebuffer() override;

	void _readCrop(u32 _crop[4]) const;
	void _getCroppedWindowSize(u32 _width, u32 _height, u32 * _croppedWidth, u32 * _croppedHeight);
	void _followCrop();
};

DisplayWindow & DisplayWindow::get()
{
	static DisplayWindowMupen64plus video;
	return video;
}

void DisplayWindowMupen64plus::_setAttributes()
{
	LOG(LOG_VERBOSE, "_setAttributes");

	FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_CONTEXT_PROFILE_MASK, M64P_GL_CONTEXT_PROFILE_CORE);
	FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_CONTEXT_MAJOR_VERSION, 3);
	FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_CONTEXT_MINOR_VERSION, 3);

	FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_DOUBLEBUFFER, 1);
	FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_SWAP_CONTROL, config.video.verticalSync);
	FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_BUFFER_SIZE, 32);
	FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_DEPTH_SIZE, 16);
	if (config.video.multisampling > 0 && config.frameBufferEmulation.enable == 0) {
		FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_MULTISAMPLEBUFFERS, 1);
		if (config.video.multisampling <= 2)
			FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_MULTISAMPLESAMPLES, 2);
		else if (config.video.multisampling <= 4)
			FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_MULTISAMPLESAMPLES, 4);
		else if (config.video.multisampling <= 8)
			FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_MULTISAMPLESAMPLES, 8);
		else
			FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_MULTISAMPLESAMPLES, 16);
	}
}

bool DisplayWindowMupen64plus::_start()
{
	FunctionWrapper::setThreadedMode(config.video.threadedVideo);
	auto returnValue = FunctionWrapper::CoreVideo_Init();
	if (returnValue != M64ERR_SUCCESS) {
		LOG(LOG_ERROR, "Error in CoreVideo_Init. Error code: %d", returnValue);
		FunctionWrapper::CoreVideo_Quit();
		return false;
	}

	_setAttributes();

	m_bFullscreen = config.video.fullscreen > 0;
	m_screenWidth = config.video.windowedWidth;
	m_screenHeight = config.video.windowedHeight;
	m_screenRefresh = config.video.fullscreenRefresh;

	// the window shows the picture less the frontend's crop at the same scale, so it's that much smaller
	_readCrop(m_crop);
	if (!m_bFullscreen)
		_getCroppedWindowSize(m_screenWidth, m_screenHeight, &m_screenWidth, &m_screenHeight);

	_getDisplaySize();
	_setBufferSize();

	LOG(LOG_VERBOSE, "Setting video mode %dx%d", m_screenWidth, m_screenHeight);
	const m64p_video_flags flags = M64VIDEOFLAG_SUPPORT_RESIZING;
#ifdef M64P_GLIDENUI
	returnValue = FunctionWrapper::CoreVideo_SetVideoModeWithRate(m_screenWidth, m_screenHeight, m_screenRefresh, 0, m_bFullscreen ? M64VIDEO_FULLSCREEN : M64VIDEO_WINDOWED, flags);
	if (returnValue != M64ERR_SUCCESS)
	{
		_supportsWithRateFunctions = false;
#endif // M64P_GLIDENUI
		returnValue = FunctionWrapper::CoreVideo_SetVideoMode(m_screenWidth, m_screenHeight, 0, m_bFullscreen ? M64VIDEO_FULLSCREEN : M64VIDEO_WINDOWED, flags);
#ifdef M64P_GLIDENUI
	}
#endif // M64P_GLIDENUI
	if (returnValue != M64ERR_SUCCESS) {
		LOG(LOG_ERROR, "Error setting videomode %dx%d @ %d. Error code: %d", m_screenWidth, m_screenHeight, m_screenRefresh, returnValue);
		FunctionWrapper::CoreVideo_Quit();
		return false;
	}

	char caption[128];
#ifdef PLUGIN_REVISION
# ifdef _DEBUG
	sprintf(caption, "%s debug. Revision %s", pluginName, PLUGIN_REVISION);
# else // _DEBUG
	sprintf(caption, "%s. Revision %s", pluginName, PLUGIN_REVISION);
# endif // _DEBUG
#else // PLUGIN_REVISION
# ifdef _DEBUG
	sprintf(caption, "%s debug", pluginName);
# else // _DEBUG
	sprintf(caption, "%s", pluginName);
# endif // _DEBUG
#endif // PLUGIN_REVISION
	CoreVideo_SetCaption(caption);

	return true;
}

void DisplayWindowMupen64plus::_stop()
{
	FunctionWrapper::CoreVideo_Quit();
}

void DisplayWindowMupen64plus::_restart()
{
#ifdef M64P_GLIDENUI
	m_resizeWidth = 0;
	m_resizeHeight = 0;
#endif // M64P_GLIDENUI
}

void DisplayWindowMupen64plus::_swapBuffers()
{
	_followCrop();

	// if emulator defined a render callback function, call it before buffer swap
	if (renderCallback != nullptr) {
		gfxContext.resetShaderProgram();
		if (config.frameBufferEmulation.N64DepthCompare == Config::dcDisable) {
			gfxContext.setViewport(0, getHeightOffset(), getScreenWidth(), getScreenHeight());
			gSP.changed |= CHANGED_VIEWPORT;
		}
		gDP.changed |= CHANGED_COMBINE;
		(*renderCallback)((gDP.changed&CHANGED_CPU_FB_WRITE) == 0 ? 1 : 0);
	}

	//Don't let the command queue grow too big buy waiting on no more swap buffers being queued
	FunctionWrapper::WaitForSwapBuffersQueued();

	FunctionWrapper::CoreVideo_GL_SwapBuffers();
}

void DisplayWindowMupen64plus::_saveScreenshot()
{
}

void DisplayWindowMupen64plus::_saveBufferContent(graphics::ObjectHandle /*_fbo*/, CachedTexture* /*_pTexture*/)
{
}

bool DisplayWindowMupen64plus::_resizeWindow()
{
#ifdef M64P_GLIDENUI
	if (m_resizeWidth == 0 && m_resizeHeight == 0) {
		return true;
	}
#endif // M64P_GLIDENUI

	_setAttributes();

	m_width = m_screenWidth = m_resizeWidth;
	m_height = m_screenHeight = m_resizeHeight;

	_setBufferSize();
	opengl::Utils::isGLError(); // reset GL error.
	return true;
}

void DisplayWindowMupen64plus::_changeWindow()
{
#ifdef M64P_GLIDENUI
	if (_supportsWithRateFunctions) {
		m64p_error returnValue;
		m_bFullscreen = !m_bFullscreen;
		if (m_bFullscreen) {
			m_screenWidth = config.video.fullscreenWidth;
			m_screenHeight = config.video.fullscreenHeight;
			m_screenRefresh = config.video.fullscreenRefresh;
		} else {
			m_screenWidth = config.video.windowedWidth;
			m_screenHeight = config.video.windowedHeight;
		}

		m64p_video_flags flags = {};
		returnValue = FunctionWrapper::CoreVideo_SetVideoModeWithRate(m_screenWidth, m_screenHeight, m_screenRefresh, 0, m_bFullscreen ? M64VIDEO_FULLSCREEN : M64VIDEO_WINDOWED, flags);

		if (returnValue != M64ERR_SUCCESS) {
			LOG(LOG_ERROR, "Error setting videomode %dx%d @ %d. Error code: %d", m_screenWidth, m_screenHeight, m_screenRefresh, returnValue);
			FunctionWrapper::CoreVideo_Quit();
		}
	} else {
#endif // M64P_GLIDENUI
		CoreVideo_ToggleFullScreen();
#ifdef M64P_GLIDENUI
	}
#endif // M64P_GLIDENUI
}

void DisplayWindowMupen64plus::_getDisplaySize()
{
#ifdef VC
	if( m_bFullscreen ) {
		// Use VC get_display_size function to get the current screen resolution
		u32 fb_width;
		u32 fb_height;
		auto returnValue = graphics_get_display_size(0 /* LCD */, &fb_width, &fb_height);
		if (returnValue < 0)
			LOG(LOG_ERROR, "Failed to get display size. Error code: %d", returnValue);
		else {
			LOG(LOG_VERBOSE, "Display size %dx%d", fb_width, fb_height);
			m_screenWidth = fb_width;
			m_screenHeight = fb_height;
		}
	}
#endif
}

void DisplayWindowMupen64plus::_readScreen2(void * _dest, int * _width, int * _height, int _front)
{
	if (_width == nullptr || _height == nullptr)
		return;

	*_width = m_screenWidth;
	*_height = m_screenHeight;

	if (_dest == nullptr)
		return;

	u8 *pBufferData = (u8*)malloc((*_width)*(*_height) * 4);
	if (pBufferData == nullptr)
		return;
	u8 *pDest = (u8*)_dest;

#if !defined(OS_ANDROID) && !defined(OS_IOS)
	GLint oldMode;
	glGetIntegerv(GL_READ_BUFFER, &oldMode);
	gfxContext.bindFramebuffer(graphics::bufferTarget::READ_FRAMEBUFFER, graphics::ObjectHandle::defaultFramebuffer);
	if (_front != 0)
		glReadBuffer(GL_FRONT);
	else
		glReadBuffer(GL_BACK);
	glReadPixels(0, m_heightOffset, m_screenWidth, m_screenHeight, GL_RGBA, GL_UNSIGNED_BYTE, pBufferData);
	if (graphics::BufferAttachmentParam(oldMode) == graphics::bufferAttachment::COLOR_ATTACHMENT0) {
		FrameBuffer * pBuffer = frameBufferList().getCurrent();
		if (pBuffer != nullptr)
			gfxContext.bindFramebuffer(graphics::bufferTarget::READ_FRAMEBUFFER, pBuffer->m_FBO);
	}
	glReadBuffer(oldMode);
#else
	glReadPixels(0, m_heightOffset, m_screenWidth, m_screenHeight, GL_RGBA, GL_UNSIGNED_BYTE, pBufferData);
#endif

	//Convert RGBA to RGB
	for (s32 y = 0; y < *_height; ++y) {
		u8 *ptr = pBufferData + ((*_width) * 4 * y);
		for (s32 x = 0; x < *_width; ++x) {
			pDest[x * 3] = ptr[0]; // red
			pDest[x * 3 + 1] = ptr[1]; // green
			pDest[x * 3 + 2] = ptr[2]; // blue
			ptr += 4;
		}
		pDest += (*_width) * 3;
	}

	free(pBufferData);
}

graphics::ObjectHandle DisplayWindowMupen64plus::_getDefaultFramebuffer()
{
	if (CoreVideo_GL_GetDefaultFramebuffer != nullptr)
		return graphics::ObjectHandle(CoreVideo_GL_GetDefaultFramebuffer());
	return graphics::ObjectHandle::null;
}

// The crop the frontend wants (VidExt_GetCrop), or none from a core that doesn't have it
void DisplayWindowMupen64plus::_readCrop(u32 _crop[4]) const
{
	_crop[0] = _crop[1] = _crop[2] = _crop[3] = 0;
	m64p_video_crop crop = {};
	if (CoreVideo_GetCrop == nullptr || CoreVideo_GetCrop(&crop) != M64ERR_SUCCESS)
		return;
	if (crop.Left < 0 || crop.Right < 0 || crop.Top < 0 || crop.Bottom < 0)
		return;
	_crop[0] = static_cast<u32>(crop.Left);
	_crop[1] = static_cast<u32>(crop.Right);
	_crop[2] = static_cast<u32>(crop.Top);
	_crop[3] = static_cast<u32>(crop.Bottom);
}

// The window size that shows the picture a window of the given size shows, less the crop, at the same scale
void DisplayWindowMupen64plus::_getCroppedWindowSize(u32 _width, u32 _height, u32 * _croppedWidth, u32 * _croppedHeight)
{
	_fitPicture(_width, _height);
	s32 pixels[4];
	_getCropPixels(m_width, m_height, pixels);
	*_croppedWidth = static_cast<u32>(static_cast<s32>(_width) - pixels[0] - pixels[1]);
	*_croppedHeight = static_cast<u32>(static_cast<s32>(_height) - pixels[2] - pixels[3]);
}

// Follows a change of the frontend's crop: the picture is laid out again (see resizeWindow), and a window, which
// shows the cropped picture at its scale, is resized for the new crop at the same scale
void DisplayWindowMupen64plus::_followCrop()
{
	u32 crop[4];
	_readCrop(crop);
	if (memcmp(crop, m_crop, sizeof(crop)) == 0)
		return;

	// the window's size without the old crop
	s32 pixels[4];
	_getCropPixels(m_width, m_height, pixels);
	u32 width = static_cast<u32>(static_cast<s32>(m_screenWidth) + pixels[0] + pixels[1]);
	u32 height = static_cast<u32>(static_cast<s32>(m_screenHeight) + pixels[2] + pixels[3]);

	memcpy(m_crop, crop, sizeof(crop));
	if (!m_bFullscreen) {
		_getCroppedWindowSize(width, height, &width, &height);
		_setBufferSize();
		if (CoreVideo_ResizeWindow != nullptr)
			CoreVideo_ResizeWindow(static_cast<int>(width), static_cast<int>(height));
	}

	m_resizeWidth = m_screenWidth;
	m_resizeHeight = m_screenHeight;
	m_bResizeWindow = true;
}
