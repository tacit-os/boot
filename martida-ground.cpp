// martida-ground: a Hyprland plugin of one call.
//
// Hyprland decodes its built-in wallpaper (misc:force_default_wallpaper -> wall0.png, which
// install.sh links to the boot ground) on a worker thread, and only asks for it from the
// first render of a monitor. So the first frame the compositor ever commits is the flat
// background_color, and the picture the splash daemon was holding "jumps in" a few frames
// later. Plugins load at STAGE_LATE, before the pending outputs are connected, so asking
// for the picture here means it is decoded by the time that first frame is drawn.
//
// requestBackgroundResource() is protected; there is no public way to trigger it.
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#undef protected
#include <hyprland/src/plugins/PluginAPI.hpp>

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    if (g_pHyprRenderer)
        g_pHyprRenderer->requestBackgroundResource();
    return {"martida-ground", "Decodes the built-in wallpaper before the first frame", "martida", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {}

// keep the inline client-hash export emitted
static const char* const g_clientHash = __hyprland_api_get_client_hash();
