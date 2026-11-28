#include <assert.h>
#include <cstdlib>
#include "Config.h"
#include "RSP.h"
#include "VI.h"
#include "Graphics/Context.h"
#include "DisplayWindow.h"
#include "PluginAPI.h"
#include "FrameBuffer.h"
#include "DepthBuffer.h"
#include "gDP.h"

bool DisplayWindow::start()
{
	if (!_start())
		return false;

	graphics::ObjectHandle::defaultFramebuffer = _getDefaultFramebuffer();

	gfxContext.init();
	m_drawer._initData();
	m_buffersSwapCount = 0;

	// only query max MSAA when needed
	if (m_maxMsaa == 0) {
		m_maxMsaa = gfxContext.getMaxMSAALevel();
	}

	if (m_maxAnisotropy == 0) {
		m_maxAnisotropy = static_cast<u32>(gfxContext.getMaxAnisotropy());
	}

	return true;
}

void DisplayWindow::stop()
{
	m_drawer._destroyData();
	gfxContext.destroy();
	_stop();
}

void DisplayWindow::restart()
{
	_restart();
	m_bResizeWindow = true;
}

void DisplayWindow::swapBuffers()
{
	m_drawer.drawOSD();
	m_drawer.clearStatistics();
	_swapBuffers();
	if (!RSP.LLE) {
		if ((config.generalEmulation.hacks & hack_doNotResetOtherModeL) == 0)
			gDP.otherMode.l = 0;
		if ((config.generalEmulation.hacks & hack_doNotResetOtherModeH) == 0)
			gDP.otherMode.h = 0x0CFF;
	}
	++m_buffersSwapCount;
}

void DisplayWindow::setCaptureScreen(const char * const _strDirectory)
{
	::mbstowcs(m_strScreenDirectory, _strDirectory, PLUGIN_PATH_SIZE - 1);
	m_bCaptureScreen = true;
}

void DisplayWindow::saveScreenshot()
{
	if (!m_bCaptureScreen)
		return;
	_saveScreenshot();
	m_bCaptureScreen = false;
}

void DisplayWindow::saveBufferContent(FrameBuffer * _pBuffer)
{
	saveBufferContent(_pBuffer->m_FBO, _pBuffer->m_pTexture);
}

void DisplayWindow::saveBufferContent(graphics::ObjectHandle _fbo, CachedTexture *_pTexture)
{
	if (wcslen(m_strScreenDirectory) == 0) {
		api().FindPluginPath(m_strScreenDirectory);
		std::wstring pluginPath(m_strScreenDirectory);
		if (pluginPath.back() != L'/')
			pluginPath += L'/';
		::wcsncpy(m_strScreenDirectory, pluginPath.c_str(), std::min(size_t(PLUGIN_PATH_SIZE), pluginPath.length() + 1));
	}
	_saveBufferContent(_fbo, _pTexture);
}

bool DisplayWindow::changeWindow()
{
	if (!m_bToggleFullscreen)
		return false;
	m_drawer._destroyData();
	_changeWindow();
	updateScale();
	m_drawer._initData();
	if (Config_isNetplayNewGame())
		rebindImages();
	m_bToggleFullscreen = false;
	return true;
}

void DisplayWindow::closeWindow()
{
	if (!m_bToggleFullscreen || !m_bFullscreen)
		return;
	m_drawer._destroyData();
	_changeWindow();
	m_bToggleFullscreen = false;
}


void DisplayWindow::setWindowSize(u32 _width, u32 _height)
{
	if (m_width != _width || m_height != _height) {
		m_resizeWidth = _width;
		m_resizeHeight = _height;
		m_bResizeWindow = true;
	}
}

bool DisplayWindow::resizeWindow()
{
	if (!m_bResizeWindow)
		return false;
	m_drawer._destroyData();
	if (!_resizeWindow())
		if(!_start())
			return false;
	updateScale();
	m_drawer._initData();
	if (Config_isNetplayNewGame())
		rebindImages();
	m_bResizeWindow = false;
	return true;
}

void DisplayWindow::rebindImages()
{
	// Binds the color and depth images gDP has again. _initData() starts with no frame buffers, so the ones the game
	// bound stay unbound until it binds them again, which can be in a later display list of the same frame; until then,
	// what it draws is dropped, with the depth that's rendered into RDRAM in software. In a game of RMG's new netplay
	// that would make RDRAM differ from when the window wasn't resized or switched to or from fullscreen, so they're
	// bound again then (elsewhere the game binds them itself, as upstream). Loading a rollback state also binds them,
	// since gDP's images then come from the state and the current buffers from the abandoned frames.
	if (config.frameBufferEmulation.enable == 0 || gDP.colorImage.address == 0 || gDP.colorImage.width == 0)
		return;
	if (gDP.depthImageAddress != 0)
		depthBufferList().saveBuffer(gDP.depthImageAddress);
	frameBufferList().saveBuffer(gDP.colorImage.address, static_cast<u16>(gDP.colorImage.format),
		static_cast<u16>(gDP.colorImage.size), static_cast<u16>(gDP.colorImage.width), false);
}

void DisplayWindow::updateScale()
{
	if (VI.width == 0 || VI.height == 0)
		return;
	m_scaleX = static_cast<f32>(m_width) / static_cast<f32>(VI.width);
	m_scaleY = static_cast<f32>(m_height) / static_cast<f32>(VI.height);
}

void DisplayWindow::_getCrop(u32 _crop[4]) const
{
	const u32 frameHeight = VI.PAL ? 576 : 480;
	if ((m_crop[0] + m_crop[1]) * 4 > 640 * 3 || (m_crop[2] + m_crop[3]) * 4 > frameHeight * 3) {
		_crop[0] = _crop[1] = _crop[2] = _crop[3] = 0;
		return;
	}
	for (u32 i = 0; i < 4; ++i)
		_crop[i] = m_crop[i];
}

void DisplayWindow::_getCropPixels(u32 _width, u32 _height, s32 _pixels[4]) const
{
	u32 crop[4];
	_getCrop(crop);
	const u32 frameHeight = VI.PAL ? 576 : 480;
	_pixels[0] = static_cast<s32>((crop[0] * _width + 320) / 640);
	_pixels[1] = static_cast<s32>((crop[1] * _width + 320) / 640);
	_pixels[2] = static_cast<s32>((crop[2] * _height + frameHeight / 2) / frameHeight);
	_pixels[3] = static_cast<s32>((crop[3] * _height + frameHeight / 2) / frameHeight);
}

void DisplayWindow::_fitPicture(u32 _screenWidth, u32 _screenHeight)
{
	m_bAdjustScreen = false;
	m_adjustScale = 1.0f; // (only the "adjust" modes change it: switching away from one left its scale)
	switch (config.frameBufferEmulation.aspect) {
	case Config::aStretch: // stretch
		m_width = _screenWidth;
		m_height = _screenHeight;
		break;
	case Config::a43: // force 4/3
		if (_screenWidth * 3 / 4 > _screenHeight) {
			m_height = _screenHeight;
			m_width = _screenHeight * 4 / 3;
		} else if (_screenHeight * 4 / 3 > _screenWidth) {
			m_width = _screenWidth;
			m_height = _screenWidth * 3 / 4;
		} else {
			m_width = _screenWidth;
			m_height = _screenHeight;
		}
		break;
	case Config::a169: // force 16/9
		if (_screenWidth * 9 / 16 > _screenHeight) {
			m_height = _screenHeight;
			m_width = _screenHeight * 16 / 9;
		} else if (_screenHeight * 16 / 9 > _screenWidth) {
			m_width = _screenWidth;
			m_height = _screenWidth * 9 / 16;
		} else {
			m_width = _screenWidth;
			m_height = _screenHeight;
		}
		break;
	case Config::aAdjust43: // adjust
		m_width = _screenWidth;
		m_height = _screenHeight;
		if (_screenWidth * 3 / 4 > _screenHeight) {
			f32 width43 = _screenHeight * 4.0f / 3.0f;
			m_adjustScale = width43 / _screenWidth;
			m_bAdjustScreen = true;
		}
		break;
	case Config::aAdjust169: // adjust
		m_width = _screenWidth;
		m_height = _screenHeight;
		if (_screenWidth * 9 / 16 > _screenHeight) {
			f32 width169 = _screenHeight * 16.0f / 9.0f;
			m_adjustScale = width169 / _screenWidth;
			m_bAdjustScreen = true;
		}
		break;
	default:
		assert(false && "Unknown aspect ratio");
		m_width = _screenWidth;
		m_height = _screenHeight;
	}
}

// With the frontend's crop, the picture is drawn larger than the screen, the size that makes its cropped part fit,
// and placed so the cropped edges fall outside the screen, where they're clipped: it's fitted into the screen that
// would hold all of it at that size, and its cropped part centered in the real one.
void DisplayWindow::_setBufferSize()
{
	u32 crop[4];
	_getCrop(crop);
	const u32 frameHeight = VI.PAL ? 576 : 480;
	const u32 keptWidth = 640 - crop[0] - crop[1];
	const u32 keptHeight = frameHeight - crop[2] - crop[3];
	_fitPicture((m_screenWidth * 640 + keptWidth / 2) / keptWidth, (m_screenHeight * frameHeight + keptHeight / 2) / keptHeight);

	s32 pixels[4];
	_getCropPixels(m_width, m_height, pixels);
	m_shownWidth = static_cast<s32>(m_width) - pixels[0] - pixels[1];
	m_shownHeight = static_cast<s32>(m_height) - pixels[2] - pixels[3];
	m_shownX = (static_cast<s32>(m_screenWidth) - m_shownWidth) / 2;
	m_shownY = (static_cast<s32>(m_screenHeight) - m_shownHeight) / 2;
	// (the frame copies that use the picture's place count down from the top of the screen area, so it's the top
	// crop that's above the shown part)
	m_pictureX = m_shownX - pixels[0];
	m_pictureY = m_shownY - pixels[2];
}

void DisplayWindow::readScreen(void **_pDest, long *_pWidth, long *_pHeight)
{
	_readScreen(_pDest, _pWidth, _pHeight);
}

void DisplayWindow::readScreen2(void * _dest, int * _width, int * _height, int _front)
{
	_readScreen2(_dest, _width, _height, _front);
}

u32 DisplayWindow::maxMSAALevel() const
{
	return m_maxMsaa;
}

u32 DisplayWindow::maxAnisotropy() const
{
	return m_maxAnisotropy;
}
