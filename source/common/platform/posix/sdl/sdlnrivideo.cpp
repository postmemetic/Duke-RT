// Keep SDL_syswm's platform headers out of renderer translation units.
#ifdef HAVE_NRI
#include "nri/system/nri_native_window.h"
#include <SDL.h>
#include <SDL_syswm.h>

extern SDL_Window* I_GetSDLWindowForNRI();

bool I_GetNRIWindow(nri::Window& window)
{
	window = {};
	SDL_Window* sdlWindow = I_GetSDLWindowForNRI();
	SDL_SysWMinfo info = {};
	SDL_VERSION(&info.version);
	if (!sdlWindow || !SDL_GetWindowWMInfo(sdlWindow, &info))
		return false;
#ifdef SDL_VIDEO_DRIVER_X11
	if (info.subsystem == SDL_SYSWM_X11)
	{
		window.x11.dpy = info.info.x11.display;
		window.x11.window = info.info.x11.window;
		return true;
	}
#endif
#ifdef SDL_VIDEO_DRIVER_WAYLAND
	if (info.subsystem == SDL_SYSWM_WAYLAND)
	{
		window.wayland.display = info.info.wl.display;
		window.wayland.surface = info.info.wl.surface;
		return true;
	}
#endif
	return false;
}
#endif
