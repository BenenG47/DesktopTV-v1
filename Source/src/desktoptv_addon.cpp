// DesktopTV v4.8 - ReShade add-on (D3D11) for The Killing Antidote. Benja's private mod.
//
// What it does:
//   1. Copies a monitor of this PC (DXGI Desktop Duplication) into a texture, up to FPS times a second.
//   2. Every time the game binds one of these textures for drawing, it binds the desktop instead:
//        - TV:  the "!Desktop" marker video (Width x Height, default 1264x720) playing on Jodi's TV
//        - PC:  Jodi's PC wallpaper (/Game/Project/UserInterface/Assets/wallpaper, 1920x1080 DXT1)
//   3. PC mode (PCModeKey, default Insert): the real mouse cursor jumps onto the captured monitor and
//      stays there, so mouse and keyboard work on that monitor while you watch it in game. The cursor
//      is drawn into the picture. The game is kept in front (it is not told that it lost focus).
//      Press the key again to get the mouse back in the game. If the game leaves the screen anyway
//      (minimised, Alt+Tab), the cursor is released and put back on the main screen.
//   4. HideUIKey (default Delete): hides Jodi's own icons/windows drawn on top of the wallpaper.
//   5. Sound=1: programs whose windows are only on the captured monitor are muted, except while the
//      TV plays the marker video or PC mode is on. Everything muted is unmuted when the game closes.
//      With the DesktopTVPos UE4SS script (Spatial=1): instead of on/off, their volume fades with
//      Jodi's distance to her PC / the TV and is placed left/right from where the camera looks.
//   6. PCModeKey and HideUIKey are caught before other programs see them (in Jodi's office, while the
//      game is in front or PC mode is on), so they never type into a browser or delete a desktop icon.
//   7. Outside Jodi's office (no TV / PC picture loaded) nothing runs: no capture, no sound checks, keys free.
//   8. Sound is always given back: when the game window closes, the add-on restores every program it muted
//      or turned down (it waits for that before letting the window close). The programs it touched are also
//      listed in DesktopTV_touched.txt; if the game ever ended without restoring them (crash), the next start
//      unmutes them and resets their left/right volume.
//
// Settings (ReShade.ini, section [DesktopTV], written with defaults on first start):
//   Monitor=1      monitor to show (1 = first, 2 = second ...); the list is written to ReShade.log
//   Width=1264     size of the TV marker video
//   Height=720
//   FPS=30         desktop updates per second
//   SRGB=2         TV colours: 2 = same as the game's own TV picture (auto), or force 0 / 1
//   TVOpaque=1     1 = make the desktop fully opaque for the TV (a desktop capture's alpha is undefined)
//   TVGamma=100    TV contrast curve in percent (100 = unchanged, 120 = darker mid-tones, 80 = lighter)
//   PCScreen=1     1 = also show the desktop on Jodi's PC screen (replaces the wallpaper picture)
//   PCWidth=1920   size of the wallpaper picture
//   PCHeight=1080
//   PCSRGB=2       PC screen colours: 2 = same as the game's own wallpaper (auto), or force 0 / 1
//   PCModeKey=45   key for PC mode (Windows virtual-key code: 45 = Insert, 35 = End ...)
//   HideUIKey=145  key to hide/show Jodi's PC interface (145 = Scroll Lock, 46 = Delete)
//   HideUI=0       1 = start with Jodi's PC interface hidden
//   HideUIInPCMode=1  1 = Jodi's PC interface is hidden automatically while PC mode is on
//   CursorScale=2  size of the cursor drawn in game (1 = like Windows, 1.5, 2 = twice as big ... up to 4)
//   CursorOutline=1  dark outline around the drawn cursor, so it shows on light and dark pages
//   SoundOutside=0 sound of second-screen programs outside Jodi's office: 0 = muted, 1 = normal
//   PCScreenLeft/Top/Right/Bottom, PCScreenTRx/TRy, PCScreenBLx/BLy  where the corners of Jodi's screen are in
//                  the game window while she leans in (1/10000 of the window). Set them while leaning in, game
//                  cursor on the corner: Ctrl+Shift+F8 = top-left, F10 = top-right, F9 = bottom-right,
//                  F11 = bottom-left. With all four, the tilt of her screen is taken into account (exact in the
//                  middle too); with only top-left + bottom-right it is a plain stretch.
//                  PC mode then puts your cursor where the game's cursor is, and back again.
//   PCCorner=120   rounded black corners on Jodi's PC screen (radius in desktop pixels, 0 = square)
//   KeepFront=1    1 = keep the game in front during PC mode
//   Sound=1        1 = mute second-screen programs unless the TV shows them or PC mode is on
//   Spatial=1      1 = use DesktopTV_pos.txt (written by the DesktopTVPos UE4SS script) for distance/left-right
//   NearCm=150     full volume within this distance of the PC / TV spot (game units = cm)
//   FarCm=1200     silent beyond this distance
//   LinuxSource=2  Linux (Proton): 0 = off, 1 = always, 2 = when running in Wine/Proton - see desktoptv_linux.inl
//   LinuxPort=47474  TCP port of the Linux helper (Linux/desktoptv_capture.py)
// Log lines go to ReShade.log, prefixed "DesktopTV:".

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <reshade.hpp>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <tlhelp32.h>
#include <psapi.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d3dcompiler.lib")
#include <dxgi1_2.h>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include <algorithm>
#include <thread>
#include <set>
#include <map>
#include <cmath>
#include <string>

using namespace reshade::api;

static int g_monitor = 1, g_width = 1264, g_height = 720, g_fps = 30, g_srgb = 2, g_tvopaque = 1, g_tvgamma = 100;
static int g_pcscreen = 1, g_pcwidth = 1920, g_pcheight = 1080, g_pcsrgb = 2, g_pckey = VK_INSERT;
static int g_hide_in_pcmode = 1, g_pccorner = 120;
static ID3D11Texture2D *g_pc_tex = nullptr; // copy of the desktop with rounded corners, for Jodi's PC
static int g_hidekey = VK_SCROLL, g_hideui_start = 0, g_keepfront = 1, g_sound = 1, g_spatial = 1, g_near = 150, g_far = 1200;
static HMODULE g_module = nullptr;

static std::atomic<device *> g_device { nullptr };
static HWND g_game_hwnd = nullptr;

// Shader resource views of the textures we replace; kind 0 = TV, 1 = PC
struct target { uint64_t view; int kind; bool srgb; };
static std::mutex g_targets_mutex;
static std::vector<target> g_targets;
static std::atomic<int> g_target_count { 0 };

// Desktop capture (used on the presenting thread, apart from the two views)
static IDXGIOutputDuplication *g_dupl = nullptr;
static ID3D11Texture2D *g_desktop_tex = nullptr;
static std::atomic<ID3D11ShaderResourceView *> g_srv[2][2] = {}; // [kind 0 TV / 1 PC][0 plain / 1 sRGB]
// TV picture: the desktop made fully opaque (+ optional contrast curve) by a small GPU pass
static ID3D11Texture2D *g_tv_tex = nullptr;
static ID3D11RenderTargetView *g_tv_rtv = nullptr;
static ID3D11ShaderResourceView *g_desktop_raw = nullptr;
static ID3D11VertexShader *g_vs = nullptr;
static ID3D11PixelShader *g_ps = nullptr;
static ID3D11Buffer *g_cb = nullptr;
static RECT g_mon_rect = {};
static std::atomic<bool> g_capturing { false }; // g_mon_rect is valid
static UINT g_tex_w = 0, g_tex_h = 0;
static std::chrono::steady_clock::time_point g_last_capture, g_next_retry;
static bool g_logged_swap[2] = {};

// Other graphics card (e.g. some virtual monitors): capture there, copy through the CPU
static ID3D11Device *g_other_dev = nullptr;
static ID3D11DeviceContext *g_other_ctx = nullptr;
static ID3D11Texture2D *g_other_staging = nullptr;

// Mouse cursor drawn into the picture (Desktop Duplication does not include it)
static std::vector<uint8_t> g_ptr_shape;
static DXGI_OUTDUPL_POINTER_SHAPE_INFO g_ptr_info = {};
static POINT g_ptr_pos = {};
static bool g_ptr_visible = false;
static ID3D11Texture2D *g_ptr_staging = nullptr;
static const UINT PTR_MAX = 256;
static float g_cursor_scale = 2.0f;
static int g_cursor_outline = 1;

// PC mode
static std::atomic<bool> g_pcmode { false };
static POINT g_saved_cursor = {};
// Jodi's screen inside the game window (fractions 0..1, from the calibration keys); -1 = not set
static float g_scr_l = -1, g_scr_t = -1, g_scr_r = -1, g_scr_b = -1;
static float g_scr_trx = -1, g_scr_try = -1, g_scr_blx = -1, g_scr_bly = -1; // the other two corners (optional)
static int g_sound_outside = 0;
static std::atomic<bool> g_in_office { false };
static std::atomic<int> g_req_calib { 0 }; // 1 = top-left, 2 = bottom-right, 3 = top-right, 4 = bottom-left
static std::atomic<bool> g_req_restore { false }, g_closing { false }; // game window closing: give the sound back
static HANDLE g_restored_event = nullptr;
static WNDPROC g_orig_wndproc = nullptr;

// Hide Jodi's PC interface: skip the draws made after the wallpaper on the PC screen's render target
static std::atomic<bool> g_hide_ui { false };
static uint64_t g_cur_rt = 0, g_pc_rt = 0;
static uint32_t g_cur_rt_count = 0;
static bool g_pc_rt_ok = false; // the PC screen has its own surface (safe to skip draws on it)
static uint64_t g_logged_rt = 0;
static uint32_t g_bb_w = 0, g_bb_h = 0;
static int g_skipped = 0;
static int g_pc_state = 0; // 0 = nothing, 1 = wallpaper bound (next draw allowed), 2 = wallpaper drawn
static std::set<uint64_t> g_back_buffers;
static bool g_logged_hide = false;

// Background thread: keys, cursor safety, sound
static std::thread g_worker;
static std::atomic<bool> g_worker_stop { false };

// Linux (Proton): picture, pointer and sound go through the Linux helper (desktoptv_linux.inl, end of this file)
static bool g_linux = false;
static void linux_settings();
static void linux_present(ID3D11Device *d3d);
static void linux_pc_on(float u, float v);
static void linux_pc_off(bool back_to_game);
static void linux_watch_pcmode();
static void linux_sound(bool restore_all, bool want, float gl, float gr);
static void linux_stop();

static void logf(reshade::log::level lvl, const char *fmt, ...)
{
	char buf[512] = "DesktopTV: ";
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf + 11, sizeof(buf) - 11, fmt, args);
	va_end(args);
	reshade::log::message(lvl, buf);
}
#define LOG_INFO(...) logf(reshade::log::level::info, __VA_ARGS__)
#define LOG_WARN(...) logf(reshade::log::level::warning, __VA_ARGS__)

// ReShade's methods that return a struct (get_resource_desc, get_resource_from_view, get_back_buffer) are
// called differently by MinGW (the Linux build) and MSVC, and the mismatch crashes the game. These ask D3D11
// directly instead - same result with both compilers. (ReShade's D3D11 handles are the D3D11 object pointers.)
static resource_desc native_desc(resource res)
{
	resource_desc d = {};
	auto *r = reinterpret_cast<ID3D11Resource *>(res.handle);
	ID3D11Texture2D *t = nullptr;
	if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&t))))
	{
		D3D11_TEXTURE2D_DESC td = {};
		t->GetDesc(&td);
		t->Release();
		d.type = resource_type::texture_2d;
		d.texture.width = td.Width;
		d.texture.height = td.Height;
		d.texture.format = static_cast<format>(td.Format); // ReShade formats use the DXGI numbers
		if (td.BindFlags & D3D11_BIND_RENDER_TARGET) d.usage = resource_usage::render_target;
	}
	return d;
}
static uint64_t native_resource_of_view(resource_view view)
{
	ID3D11Resource *r = nullptr;
	reinterpret_cast<ID3D11View *>(view.handle)->GetResource(&r);
	if (r) r->Release(); // the game still holds it
	return reinterpret_cast<uint64_t>(r);
}
static uint64_t native_back_buffer(swapchain *sc)
{
	ID3D11Texture2D *bb = nullptr;
	auto *chain = reinterpret_cast<IDXGISwapChain *>(sc->get_native());
	if (chain && SUCCEEDED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&bb))) && bb)
		bb->Release(); // the swap chain still holds it
	return reinterpret_cast<uint64_t>(bb);
}

static void read_settings()
{
	auto get = [](const char *key, int &v) {
		if (!reshade::get_config_value(nullptr, "DesktopTV", key, v))
			reshade::set_config_value(nullptr, "DesktopTV", key, v);
	};
	get("Monitor", g_monitor); get("Width", g_width); get("Height", g_height);
	get("FPS", g_fps); get("SRGB", g_srgb); get("TVOpaque", g_tvopaque); get("TVGamma", g_tvgamma);
	get("PCScreen", g_pcscreen); get("PCWidth", g_pcwidth); get("PCHeight", g_pcheight); get("PCSRGB", g_pcsrgb);
	get("PCModeKey", g_pckey); get("HideUIKey", g_hidekey); get("HideUI", g_hideui_start); get("HideUIInPCMode", g_hide_in_pcmode); get("PCCorner", g_pccorner); get("SoundOutside", g_sound_outside);
	{
		int l = -1, t = -1, r = -1, b = -1;
		reshade::get_config_value(nullptr, "DesktopTV", "PCScreenLeft", l);
		reshade::get_config_value(nullptr, "DesktopTV", "PCScreenTop", t);
		reshade::get_config_value(nullptr, "DesktopTV", "PCScreenRight", r);
		reshade::get_config_value(nullptr, "DesktopTV", "PCScreenBottom", b);
		if (l >= 0 && t >= 0 && r > l && b > t) { g_scr_l = l / 10000.0f; g_scr_t = t / 10000.0f; g_scr_r = r / 10000.0f; g_scr_b = b / 10000.0f; }
		int trx = -1, try_ = -1, blx = -1, bly = -1;
		reshade::get_config_value(nullptr, "DesktopTV", "PCScreenTRx", trx);
		reshade::get_config_value(nullptr, "DesktopTV", "PCScreenTRy", try_);
		reshade::get_config_value(nullptr, "DesktopTV", "PCScreenBLx", blx);
		reshade::get_config_value(nullptr, "DesktopTV", "PCScreenBLy", bly);
		if (trx >= 0 && try_ >= 0) { g_scr_trx = trx / 10000.0f; g_scr_try = try_ / 10000.0f; }
		if (blx >= 0 && bly >= 0) { g_scr_blx = blx / 10000.0f; g_scr_bly = bly / 10000.0f; }
		LOG_INFO("Jodi's screen in the game window: %s", g_scr_l < 0 ? "not calibrated (Ctrl+Shift+F8 / F10 / F9 / F11 while leaning in)" :
			(g_scr_trx >= 0 && g_scr_blx >= 0) ? "calibrated, 4 corners (tilt corrected)" : "calibrated, 2 corners (plain stretch)");
	} get("CursorOutline", g_cursor_outline);
	{
		// decimal value: "1.5" (a comma "1,5" works too)
		char buf[32] = {}; size_t n = sizeof(buf) - 1;
		if (reshade::get_config_value(nullptr, "DesktopTV", "CursorScale", buf, &n))
		{
			buf[std::min(n, sizeof(buf) - 1)] = 0;
			for (char &c : buf) if (c == ',') c = '.';
			const float v = (float)atof(buf);
			if (v > 0) g_cursor_scale = v;
		}
		else
			reshade::set_config_value(nullptr, "DesktopTV", "CursorScale", "2");
		g_cursor_scale = std::max(1.0f, std::min(4.0f, g_cursor_scale));
		LOG_INFO("cursor scale %.2f, outline %d", g_cursor_scale, g_cursor_outline);
	}
	get("KeepFront", g_keepfront); get("Sound", g_sound);
	get("Spatial", g_spatial); get("NearCm", g_near); get("FarCm", g_far);
	if (g_far <= g_near) g_far = g_near + 100;
	g_hide_ui = g_hideui_start != 0;
	if (g_fps < 1) g_fps = 1;
	if (g_monitor < 1) g_monitor = 1;
	linux_settings();
	LOG_INFO("settings TVOpaque=%d TVGamma=%d Monitor=%d TV %dx%d SRGB=%d | PCScreen=%d %dx%d PCSRGB=%d | FPS=%d PCModeKey=%d HideUIKey=%d HideUI=%d KeepFront=%d Sound=%d Spatial=%d Near=%d Far=%d",
		g_tvopaque, g_tvgamma, g_monitor, g_width, g_height, g_srgb, g_pcscreen, g_pcwidth, g_pcheight, g_pcsrgb, g_fps, g_pckey, g_hidekey, g_hideui_start, g_keepfront, g_sound, g_spatial, g_near, g_far);
}

// ---------------------------------------------------------------- capture

static void release_capture()
{
	g_capturing = false;
	for (auto &k : g_srv)
		for (auto &v : k) { ID3D11ShaderResourceView *srv = v.exchange(nullptr); if (srv) srv->Release(); }
	if (g_tv_rtv) { g_tv_rtv->Release(); g_tv_rtv = nullptr; }
	if (g_tv_tex) { g_tv_tex->Release(); g_tv_tex = nullptr; }
	if (g_desktop_raw) { g_desktop_raw->Release(); g_desktop_raw = nullptr; }
	if (g_vs) { g_vs->Release(); g_vs = nullptr; }
	if (g_ps) { g_ps->Release(); g_ps = nullptr; }
	if (g_cb) { g_cb->Release(); g_cb = nullptr; }
	if (g_desktop_tex) { g_desktop_tex->Release(); g_desktop_tex = nullptr; }
	if (g_pc_tex) { g_pc_tex->Release(); g_pc_tex = nullptr; }
	if (g_ptr_staging) { g_ptr_staging->Release(); g_ptr_staging = nullptr; }
	if (g_dupl) { g_dupl->Release(); g_dupl = nullptr; }
	if (g_other_staging) { g_other_staging->Release(); g_other_staging = nullptr; }
	if (g_other_ctx) { g_other_ctx->Release(); g_other_ctx = nullptr; }
	if (g_other_dev) { g_other_dev->Release(); g_other_dev = nullptr; }
}

static bool same_adapter(IDXGIAdapter *a, ID3D11Device *d3d)
{
	bool same = false;
	IDXGIDevice *dxgi_device = nullptr;
	IDXGIAdapter *game = nullptr;
	if (SUCCEEDED(d3d->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_device))) && SUCCEEDED(dxgi_device->GetAdapter(&game)))
	{
		DXGI_ADAPTER_DESC x = {}, y = {};
		a->GetDesc(&x); game->GetDesc(&y);
		same = x.AdapterLuid.LowPart == y.AdapterLuid.LowPart && x.AdapterLuid.HighPart == y.AdapterLuid.HighPart;
	}
	if (game) game->Release();
	if (dxgi_device) dxgi_device->Release();
	return same;
}

static ID3D11ShaderResourceView *make_view(ID3D11Device *d3d, ID3D11Texture2D *tex, bool srgb)
{
	D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
	vd.Format = srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
	vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	vd.Texture2D.MipLevels = 1;
	ID3D11ShaderResourceView *srv = nullptr;
	if (FAILED(d3d->CreateShaderResourceView(tex, &vd, &srv)))
		return nullptr;
	return srv;
}

static const char *TV_SHADER = R"(
Texture2D t : register(t0);
cbuffer c : register(b0) { float gamma; float opaque; float2 pad; };
void vs(uint id : SV_VertexID, out float4 pos : SV_Position)
{
	float2 uv = float2((id << 1) & 2, id & 2);
	pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target
{
	float4 c = t.Load(int3(pos.xy, 0));
	c.rgb = pow(saturate(c.rgb), gamma);
	return float4(c.rgb, opaque > 0.5 ? 1.0 : c.a);
}
)";

static void create_tv_pass(ID3D11Device *d3d, const D3D11_TEXTURE2D_DESC &desk)
{
	ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
	if (FAILED(D3DCompile(TV_SHADER, strlen(TV_SHADER), "tv", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
		FAILED(D3DCompile(TV_SHADER, strlen(TV_SHADER), "tv", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err)))
	{
		LOG_WARN("TV pass: shader compile failed: %s", err ? static_cast<const char *>(err->GetBufferPointer()) : "?");
	}
	else
	{
		D3D11_TEXTURE2D_DESC td = desk;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		D3D11_RENDER_TARGET_VIEW_DESC rd = {};
		rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = 16;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		const float cb[4] = { g_tvgamma / 100.0f, g_tvopaque ? 1.0f : 0.0f, 0, 0 };
		D3D11_SUBRESOURCE_DATA init = { cb, 0, 0 };
		if (SUCCEEDED(d3d->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs)) &&
			SUCCEEDED(d3d->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps)) &&
			SUCCEEDED(d3d->CreateBuffer(&bd, &init, &g_cb)) &&
			SUCCEEDED(d3d->CreateTexture2D(&td, nullptr, &g_tv_tex)) &&
			SUCCEEDED(d3d->CreateRenderTargetView(g_tv_tex, &rd, &g_tv_rtv)))
		{
			g_desktop_raw = make_view(d3d, g_desktop_tex, false);
			LOG_INFO("TV pass ready (opaque=%d, gamma=%d%%)", g_tvopaque, g_tvgamma);
		}
		else
		{
			LOG_WARN("TV pass: could not create its resources");
		}
	}
	if (vsb) vsb->Release();
	if (psb) psb->Release();
	if (err) err->Release();
	if (g_desktop_raw == nullptr)
	{
		// fall back to the plain desktop for the TV
		if (g_tv_rtv) { g_tv_rtv->Release(); g_tv_rtv = nullptr; }
		if (g_tv_tex) { g_tv_tex->Release(); g_tv_tex = nullptr; }
	}
}

// Draw the desktop into the TV picture; every piece of state touched is saved and put back,
// because the game keeps its own record of what is bound.
static void run_tv_pass(ID3D11DeviceContext *ctx)
{
	if (g_tv_tex == nullptr || g_desktop_raw == nullptr)
		return;
	D3D11_PRIMITIVE_TOPOLOGY topo; ctx->IAGetPrimitiveTopology(&topo);
	ID3D11InputLayout *il = nullptr; ctx->IAGetInputLayout(&il);
	ID3D11VertexShader *vs = nullptr; ID3D11ClassInstance *vci[256]; UINT nvci = 256; ctx->VSGetShader(&vs, vci, &nvci);
	ID3D11PixelShader *ps = nullptr; ID3D11ClassInstance *pci[256]; UINT npci = 256; ctx->PSGetShader(&ps, pci, &npci);
	ID3D11GeometryShader *gs = nullptr; ID3D11ClassInstance *gci[256]; UINT ngci = 256; ctx->GSGetShader(&gs, gci, &ngci);
	ID3D11HullShader *hs = nullptr; ID3D11ClassInstance *hci[256]; UINT nhci = 256; ctx->HSGetShader(&hs, hci, &nhci);
	ID3D11DomainShader *ds = nullptr; ID3D11ClassInstance *dci[256]; UINT ndci = 256; ctx->DSGetShader(&ds, dci, &ndci);
	ID3D11ShaderResourceView *srv0 = nullptr; ctx->PSGetShaderResources(0, 1, &srv0);
	ID3D11Buffer *cb0 = nullptr; ctx->PSGetConstantBuffers(0, 1, &cb0);
	ID3D11RenderTargetView *rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {}; ID3D11DepthStencilView *dsv = nullptr;
	ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
	ID3D11BlendState *bs = nullptr; FLOAT bf[4]; UINT sm; ctx->OMGetBlendState(&bs, bf, &sm);
	ID3D11DepthStencilState *dss = nullptr; UINT sref; ctx->OMGetDepthStencilState(&dss, &sref);
	ID3D11RasterizerState *rs = nullptr; ctx->RSGetState(&rs);
	D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]; UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; ctx->RSGetViewports(&nvp, vps);

	D3D11_VIEWPORT vp = { 0, 0, (float)g_tex_w, (float)g_tex_h, 0, 1 };
	ctx->OMSetRenderTargets(1, &g_tv_rtv, nullptr);
	ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
	ctx->OMSetDepthStencilState(nullptr, 0);
	ctx->RSSetState(nullptr);
	ctx->RSSetViewports(1, &vp);
	ctx->IASetInputLayout(nullptr);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx->VSSetShader(g_vs, nullptr, 0);
	ctx->GSSetShader(nullptr, nullptr, 0);
	ctx->HSSetShader(nullptr, nullptr, 0);
	ctx->DSSetShader(nullptr, nullptr, 0);
	ctx->PSSetShader(g_ps, nullptr, 0);
	ctx->PSSetShaderResources(0, 1, &g_desktop_raw);
	ctx->PSSetConstantBuffers(0, 1, &g_cb);
	ctx->Draw(3, 0);

	ID3D11ShaderResourceView *null_srv = nullptr;
	ctx->PSSetShaderResources(0, 1, &null_srv);
	ctx->IASetPrimitiveTopology(topo); ctx->IASetInputLayout(il);
	ctx->VSSetShader(vs, vci, nvci); ctx->PSSetShader(ps, pci, npci);
	ctx->GSSetShader(gs, gci, ngci); ctx->HSSetShader(hs, hci, nhci); ctx->DSSetShader(ds, dci, ndci);
	ctx->PSSetShaderResources(0, 1, &srv0); ctx->PSSetConstantBuffers(0, 1, &cb0);
	ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
	ctx->OMSetBlendState(bs, bf, sm); ctx->OMSetDepthStencilState(dss, sref);
	ctx->RSSetState(rs); ctx->RSSetViewports(nvp, vps);

	auto rel = [](IUnknown *u) { if (u) u->Release(); };
	rel(il); rel(vs); rel(ps); rel(gs); rel(hs); rel(ds); rel(srv0); rel(cb0); rel(dsv); rel(bs); rel(dss); rel(rs);
	for (auto *r : rtvs) rel(r);
	for (UINT i = 0; i < nvci; ++i) rel(vci[i]);
	for (UINT i = 0; i < npci; ++i) rel(pci[i]);
	for (UINT i = 0; i < ngci; ++i) rel(gci[i]);
	for (UINT i = 0; i < nhci; ++i) rel(hci[i]);
	for (UINT i = 0; i < ndci; ++i) rel(dci[i]);
}

// The desktop texture, the cursor staging copy, Jodi's PC copy, the TV pass and the four views
static bool create_textures(ID3D11Device *d3d, const D3D11_TEXTURE2D_DESC &td)
{
	D3D11_TEXTURE2D_DESC pd = td; // small CPU copy of the area under the cursor
	pd.Width = PTR_MAX; pd.Height = PTR_MAX;
	pd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	pd.Usage = D3D11_USAGE_STAGING;
	pd.BindFlags = 0;
	pd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(d3d->CreateTexture2D(&td, nullptr, &g_desktop_tex)) || FAILED(d3d->CreateTexture2D(&pd, nullptr, &g_ptr_staging)))
		return false;
	if (g_pccorner > 0 && FAILED(d3d->CreateTexture2D(&td, nullptr, &g_pc_tex)))
		g_pc_tex = nullptr;
	if (g_tvopaque || g_tvgamma != 100)
		create_tv_pass(d3d, td);
	ID3D11Texture2D *tv_src = g_tv_tex ? g_tv_tex : g_desktop_tex;
	ID3D11Texture2D *pc_src = g_pc_tex ? g_pc_tex : g_desktop_tex;
	g_srv[0][0].store(make_view(d3d, tv_src, false));
	g_srv[0][1].store(make_view(d3d, tv_src, true));
	g_srv[1][0].store(make_view(d3d, pc_src, false));
	g_srv[1][1].store(make_view(d3d, pc_src, true));
	if (!(g_srv[0][0].load() && g_srv[0][1].load() && g_srv[1][0].load() && g_srv[1][1].load()))
		return false;
	g_tex_w = td.Width; g_tex_h = td.Height;
	return true;
}

static bool create_capture(ID3D11Device *d3d)
{
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
	{
		LOG_WARN("CreateDXGIFactory1 failed");
		return false;
	}

	// Number the monitors across all graphics cards
	IDXGIAdapter1 *pick_adapter = nullptr;
	IDXGIOutput *pick_output = nullptr;
	int n = 0;
	static bool listed = false; // write the monitor list once
	IDXGIAdapter1 *a = nullptr;
	for (UINT ai = 0; factory->EnumAdapters1(ai, &a) != DXGI_ERROR_NOT_FOUND; ++ai)
	{
		DXGI_ADAPTER_DESC1 ad = {};
		a->GetDesc1(&ad);
		char aname[128] = {};
		WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, aname, sizeof(aname), nullptr, nullptr);
		const bool same = same_adapter(a, d3d);
		IDXGIOutput *o = nullptr;
		for (UINT oi = 0; a->EnumOutputs(oi, &o) != DXGI_ERROR_NOT_FOUND; ++oi)
		{
			DXGI_OUTPUT_DESC od = {};
			o->GetDesc(&od);
			char oname[64] = {};
			WideCharToMultiByte(CP_UTF8, 0, od.DeviceName, -1, oname, sizeof(oname), nullptr, nullptr);
			++n;
			if (!listed) LOG_INFO("monitor %d: %s %ldx%ld on '%s'%s", n, oname,
				od.DesktopCoordinates.right - od.DesktopCoordinates.left, od.DesktopCoordinates.bottom - od.DesktopCoordinates.top,
				aname, same ? " (the game's card)" : "");
			if (n == g_monitor && pick_output == nullptr) { pick_output = o; pick_adapter = a; a->AddRef(); g_mon_rect = od.DesktopCoordinates; }
			else o->Release();
		}
		a->Release();
	}
	factory->Release();
	listed = true;
	if (pick_output == nullptr)
	{
		LOG_WARN("monitor %d not found (%d monitor(s) seen) - set Monitor= in ReShade.ini", g_monitor, n);
		return false;
	}

	bool ok = false;
	const bool same = same_adapter(pick_adapter, d3d);
	ID3D11Device *cap_dev = d3d;
	if (!same)
	{
		HRESULT hr = D3D11CreateDevice(pick_adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_other_dev, nullptr, &g_other_ctx);
		if (FAILED(hr)) LOG_WARN("could not open the other graphics card (hr=0x%08X)", (unsigned)hr);
		cap_dev = g_other_dev;
	}
	IDXGIOutput1 *output1 = nullptr;
	if (cap_dev && SUCCEEDED(pick_output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void **>(&output1))))
	{
		HRESULT hr = output1->DuplicateOutput(cap_dev, &g_dupl);
		if (FAILED(hr))
		{
			LOG_WARN("DuplicateOutput failed for monitor %d (hr=0x%08X)", g_monitor, (unsigned)hr);
		}
		else
		{
			DXGI_OUTDUPL_DESC dd = {};
			g_dupl->GetDesc(&dd);
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = dd.ModeDesc.Width;
			td.Height = dd.ModeDesc.Height;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			bool staging_ok = true;
			if (!same)
			{
				D3D11_TEXTURE2D_DESC sd = td;
				sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
				sd.Usage = D3D11_USAGE_STAGING;
				sd.BindFlags = 0;
				sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				staging_ok = SUCCEEDED(g_other_dev->CreateTexture2D(&sd, nullptr, &g_other_staging));
			}
			if (staging_ok && create_textures(d3d, td))
			{
				ok = true;
				g_capturing = true;
				LOG_INFO("capturing monitor %d %ux%u%s", g_monitor, td.Width, td.Height, same ? "" : " (other card: copied through the CPU)");
			}
			if (!ok) LOG_WARN("could not create the desktop texture");
		}
	}
	if (output1) output1->Release();
	pick_output->Release();
	pick_adapter->Release();
	if (!ok) release_capture();
	return ok;
}

// The cursor as a picture with transparency (BGRA, straight alpha), scaled and outlined.
// Windows' "invert" pixels (I-beam, some arrows) are drawn white so they stay visible.
static std::vector<uint32_t> g_sprite;
static int g_sprite_w = 0, g_sprite_h = 0, g_sprite_hx = 0, g_sprite_hy = 0; // size, hot spot
static bool g_sprite_dirty = true;

static void build_sprite()
{
	g_sprite_dirty = false;
	const auto &si = g_ptr_info;
	const int w = (int)si.Width, h = si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? (int)si.Height / 2 : (int)si.Height;
	if (w <= 0 || h <= 0 || g_ptr_shape.empty()) { g_sprite_w = g_sprite_h = 0; return; }
	std::vector<uint32_t> base(w * h, 0);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
		{
			uint32_t px = 0;
			if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME)
			{
				const uint8_t mask = 0x80 >> (x % 8);
				const bool a = (g_ptr_shape[y * si.Pitch + x / 8] & mask) != 0;
				const bool xo = (g_ptr_shape[(y + h) * si.Pitch + x / 8] & mask) != 0;
				if (!a) px = xo ? 0xFFFFFFFF : 0xFF000000;
				else if (xo) px = 0xFFFFFFFF; // invert -> white
			}
			else
			{
				const uint32_t c = *reinterpret_cast<const uint32_t *>(&g_ptr_shape[y * si.Pitch + x * 4]);
				if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR)
					px = (c >> 24) == 0 ? (c | 0xFF000000) : ((c & 0x00FFFFFF) ? 0xFFFFFFFF : 0);
				else
					px = c;
			}
			base[y * w + x] = px;
		}
	const float k = g_cursor_scale;
	const int o = g_cursor_outline ? std::max(1, (int)std::lround(k)) : 0;
	const int sw = std::max(1, (int)std::lround(w * k)), sh = std::max(1, (int)std::lround(h * k));
	g_sprite_w = sw + 2 * o; g_sprite_h = sh + 2 * o;
	g_sprite_hx = (int)std::lround(si.HotSpot.x * k) + o; g_sprite_hy = (int)std::lround(si.HotSpot.y * k) + o;
	g_sprite.assign(g_sprite_w * g_sprite_h, 0);
	for (int y = 0; y < sh; ++y)
		for (int x = 0; x < sw; ++x)
			g_sprite[(y + o) * g_sprite_w + x + o] = base[std::min(h - 1, (int)(y / k)) * w + std::min(w - 1, (int)(x / k))];
	if (o > 0)
	{
		// dark outline: transparent pixels next to the cursor become dark
		std::vector<uint32_t> out = g_sprite;
		for (int y = 0; y < g_sprite_h; ++y)
			for (int x = 0; x < g_sprite_w; ++x)
			{
				if ((g_sprite[y * g_sprite_w + x] >> 24) != 0) continue;
				bool near_cursor = false;
				for (int dy = -o; dy <= o && !near_cursor; ++dy)
					for (int dx = -o; dx <= o && !near_cursor; ++dx)
					{
						const int nx = x + dx, ny = y + dy;
						if (nx >= 0 && ny >= 0 && nx < g_sprite_w && ny < g_sprite_h && (g_sprite[ny * g_sprite_w + nx] >> 24) > 128)
							near_cursor = true;
					}
				if (near_cursor) out[y * g_sprite_w + x] = 0xC0101010;
			}
		g_sprite.swap(out);
	}
}

static void draw_cursor(ID3D11DeviceContext *ctx)
{
	if (!g_pcmode || !g_ptr_visible || g_ptr_shape.empty())
		return;
	if (g_sprite_dirty) build_sprite();
	if (g_sprite_w == 0) return;
	// Keep the hot spot (the tip of the arrow) where Windows has it
	int x0 = g_ptr_pos.x + (int)g_ptr_info.HotSpot.x - g_sprite_hx, y0 = g_ptr_pos.y + (int)g_ptr_info.HotSpot.y - g_sprite_hy;
	int x1 = std::min<int>(x0 + g_sprite_w, (int)g_tex_w), y1 = std::min<int>(y0 + g_sprite_h, (int)g_tex_h);
	const int ox = x0 < 0 ? -x0 : 0, oy = y0 < 0 ? -y0 : 0;
	x0 = std::max(x0, 0); y0 = std::max(y0, 0);
	x1 = std::min<int>(x1, x0 + (int)PTR_MAX); y1 = std::min<int>(y1, y0 + (int)PTR_MAX);
	if (x1 <= x0 || y1 <= y0)
		return;
	D3D11_BOX box = { (UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1 };
	ctx->CopySubresourceRegion(g_ptr_staging, 0, 0, 0, 0, g_desktop_tex, 0, &box);
	D3D11_MAPPED_SUBRESOURCE m = {};
	if (FAILED(ctx->Map(g_ptr_staging, 0, D3D11_MAP_READ, 0, &m)))
		return;
	static std::vector<uint32_t> tmp;
	const UINT w = x1 - x0, h = y1 - y0;
	tmp.resize(w * h);
	for (UINT y = 0; y < h; ++y)
		std::memcpy(&tmp[y * w], static_cast<uint8_t *>(m.pData) + y * m.RowPitch, w * 4);
	ctx->Unmap(g_ptr_staging, 0);
	for (UINT y = 0; y < h; ++y)
		for (UINT x = 0; x < w; ++x)
		{
			const uint32_t sp = g_sprite[(y + oy) * g_sprite_w + x + ox];
			const uint32_t a = sp >> 24;
			if (a == 0) continue;
			uint32_t &d = tmp[y * w + x];
			uint32_t r = 0xFF000000;
			for (int c = 0; c < 24; c += 8)
				r |= ((((sp >> c) & 0xFF) * a + ((d >> c) & 0xFF) * (255 - a)) / 255) << c;
			d = r;
		}
	ctx->UpdateSubresource(g_desktop_tex, 0, &box, tmp.data(), w * 4, 0);
}

// Jodi's PC: copy of the desktop with black rounded corners (her monitor's frame is rounded)
static void update_pc_copy(ID3D11DeviceContext *ctx)
{
	if (g_pc_tex == nullptr)
		return;
	ctx->CopyResource(g_pc_tex, g_desktop_tex);
	const int r = std::min<int>(g_pccorner, (int)std::min(g_tex_w, g_tex_h) / 2);
	static std::vector<uint32_t> black;
	if ((int)black.size() < 2 * r) black.assign(2 * r, 0xFF000000);
	for (int y = 0; y < r; y += 2) // 2-pixel steps: invisible at this size, half the work
	{
		const double dy = r - (y + 1.0);
		const int run = (int)std::ceil(r - std::sqrt(std::max(0.0, double(r) * r - dy * dy)));
		if (run <= 0) continue;
		const UINT h = (UINT)std::min(2, r - y);
		const UINT rows[2] = { (UINT)y, g_tex_h - (UINT)y - h };
		for (UINT top : rows)
		{
			D3D11_BOX left = { 0, top, 0, (UINT)run, top + h, 1 };
			D3D11_BOX right = { g_tex_w - run, top, 0, g_tex_w, top + h, 1 };
			ctx->UpdateSubresource(g_pc_tex, 0, &left, black.data(), run * 4, 0);
			ctx->UpdateSubresource(g_pc_tex, 0, &right, black.data(), run * 4, 0);
		}
	}
}

static void capture(ID3D11Device *d3d)
{
	DXGI_OUTDUPL_FRAME_INFO info = {};
	IDXGIResource *frame = nullptr;
	HRESULT hr = g_dupl->AcquireNextFrame(0, &info, &frame);
	if (hr == DXGI_ERROR_WAIT_TIMEOUT)
		return; // desktop did not change
	if (FAILED(hr))
	{
		LOG_WARN("AcquireNextFrame failed (hr=0x%08X), restarting capture", (unsigned)hr);
		release_capture();
		g_next_retry = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		return;
	}
	if (info.LastMouseUpdateTime.QuadPart != 0)
	{
		g_ptr_visible = info.PointerPosition.Visible != FALSE;
		g_ptr_pos = info.PointerPosition.Position;
	}
	if (info.PointerShapeBufferSize != 0)
	{
		g_ptr_shape.resize(info.PointerShapeBufferSize);
		UINT needed = 0;
		if (FAILED(g_dupl->GetFramePointerShape(info.PointerShapeBufferSize, g_ptr_shape.data(), &needed, &g_ptr_info)))
			g_ptr_shape.clear();
		g_sprite_dirty = true;
	}

	ID3D11Texture2D *tex = nullptr;
	if (SUCCEEDED(frame->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))))
	{
		ID3D11DeviceContext *ctx = nullptr;
		d3d->GetImmediateContext(&ctx);
		if (info.LastPresentTime.QuadPart != 0 || g_pcmode) // new picture (or the cursor moved in PC mode)
		{
			if (g_other_dev == nullptr)
			{
				ctx->CopyResource(g_desktop_tex, tex);
			}
			else
			{
				g_other_ctx->CopyResource(g_other_staging, tex);
				D3D11_MAPPED_SUBRESOURCE m = {};
				if (SUCCEEDED(g_other_ctx->Map(g_other_staging, 0, D3D11_MAP_READ, 0, &m)))
				{
					ctx->UpdateSubresource(g_desktop_tex, 0, nullptr, m.pData, m.RowPitch, 0);
					g_other_ctx->Unmap(g_other_staging, 0);
				}
			}
			draw_cursor(ctx);
			update_pc_copy(ctx);
			run_tv_pass(ctx);
		}
		ctx->Release();
		tex->Release();
	}
	frame->Release();
	g_dupl->ReleaseFrame();
}

// ---------------------------------------------------------------- PC mode

// While PC mode is on, the game is not told that it lost focus, so it stays in front and keeps running.
static void restore_before_close();
static LRESULT CALLBACK game_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_CLOSE || msg == WM_DESTROY || (msg == WM_ENDSESSION && wp))
		restore_before_close();
	if (g_pcmode && g_keepfront)
	{
		switch (msg)
		{
		case WM_ACTIVATEAPP:
			if (!wp) return 0;
			break;
		case WM_ACTIVATE:
			if (LOWORD(wp) == WA_INACTIVE) return 0;
			break;
		case WM_NCACTIVATE:
			if (!wp) return TRUE;
			break;
		case WM_KILLFOCUS:
			return 0;
		case WM_SIZE:
			if (wp == SIZE_MINIMIZED) return 0;
			break;
		case WM_SYSCOMMAND:
			if ((wp & 0xFFF0) == SC_MINIMIZE) return 0;
			break;
		}
	}
	return CallWindowProcW(g_orig_wndproc, hwnd, msg, wp, lp);
}

// The game is closing: have the background thread give the sound back, and wait for it (1.5 s at most)
static void restore_before_close()
{
	if (g_closing.exchange(true) || !g_worker.joinable())
		return;
	if (g_restored_event == nullptr) g_restored_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	g_req_restore = true;
	if (g_restored_event) WaitForSingleObject(g_restored_event, 1500);
}

static void hook_game_window()
{
	if (g_orig_wndproc != nullptr || g_game_hwnd == nullptr)
		return;
	g_orig_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_game_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&game_wndproc)));
	LOG_INFO(g_orig_wndproc ? "game window hooked (keeps the game in front in PC mode, gives the sound back on close)" : "could not hook the game window");
}

static void unhook_game_window()
{
	if (g_orig_wndproc == nullptr || g_game_hwnd == nullptr)
		return;
	if (GetWindowLongPtrW(g_game_hwnd, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&game_wndproc))
		SetWindowLongPtrW(g_game_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig_wndproc));
	g_orig_wndproc = nullptr; // if someone hooked after us we stay in their chain, passing everything through
}

static void focus_window(HWND w)
{
	if (w == nullptr) return;
	// Windows only lets the active program hand over focus; a tapped Alt key lifts that limit
	INPUT in[2] = {};
	in[0].type = in[1].type = INPUT_KEYBOARD;
	in[0].ki.wVk = in[1].ki.wVk = VK_MENU;
	in[1].ki.dwFlags = KEYEVENTF_KEYUP;
	SendInput(2, in, sizeof(INPUT));
	SetForegroundWindow(w);
}

// Game cursor <-> point on Jodi's screen (u, v in 0..1), using the calibration
// Mapping between a point (u, v) of Jodi's screen (0..1 each) and the game window (0..1 each).
// 4 corners: projective mapping of the unit square onto the tilted quad (Heckbert's square-to-quad);
// 2 corners: plain stretch between top-left and bottom-right.
struct quad_map { bool ok = false; double a, b, c, d, e, f, g, h; };
static quad_map make_map()
{
	quad_map m;
	if (g_scr_l < 0) return m;
	double x0 = g_scr_l, y0 = g_scr_t, x2 = g_scr_r, y2 = g_scr_b, x1, y1, x3, y3;
	if (g_scr_trx >= 0 && g_scr_blx >= 0) { x1 = g_scr_trx; y1 = g_scr_try; x3 = g_scr_blx; y3 = g_scr_bly; }
	else { x1 = x2; y1 = y0; x3 = x0; y3 = y2; } // rectangle from the 2 corners
	const double dx1 = x1 - x2, dx2 = x3 - x2, dx3 = x0 - x1 + x2 - x3;
	const double dy1 = y1 - y2, dy2 = y3 - y2, dy3 = y0 - y1 + y2 - y3;
	if (std::fabs(dx3) < 1e-9 && std::fabs(dy3) < 1e-9)
	{
		m.a = x1 - x0; m.b = x2 - x1; m.c = x0; m.d = y1 - y0; m.e = y2 - y1; m.f = y0; m.g = m.h = 0;
	}
	else
	{
		const double det = dx1 * dy2 - dx2 * dy1;
		if (std::fabs(det) < 1e-12) return m;
		m.g = (dx3 * dy2 - dx2 * dy3) / det;
		m.h = (dx1 * dy3 - dx3 * dy1) / det;
		m.a = x1 - x0 + m.g * x1; m.b = x3 - x0 + m.h * x3; m.c = x0;
		m.d = y1 - y0 + m.g * y1; m.e = y3 - y0 + m.h * y3; m.f = y0;
	}
	m.ok = true;
	return m;
}

static bool game_cursor_to_screen(POINT p, float &u, float &v)
{
	const quad_map m = make_map();
	if (!m.ok || g_game_hwnd == nullptr) return false;
	RECT cr; if (!GetClientRect(g_game_hwnd, &cr) || cr.right <= 0 || cr.bottom <= 0) return false;
	ScreenToClient(g_game_hwnd, &p);
	const double x = p.x / double(cr.right), y = p.y / double(cr.bottom);
	// invert [a b c; d e f; g h 1]
	const double A = m.e - m.f * m.h, B = m.c * m.h - m.b, C = m.b * m.f - m.c * m.e;
	const double D = m.f * m.g - m.d, E = m.a - m.c * m.g, F = m.c * m.d - m.a * m.f;
	const double G = m.d * m.h - m.e * m.g, H = m.b * m.g - m.a * m.h, I = m.a * m.e - m.b * m.d;
	const double w = G * x + H * y + I;
	if (std::fabs(w) < 1e-12) return false;
	u = float((A * x + B * y + C) / w);
	v = float((D * x + E * y + F) / w);
	if (u < -0.05f || u > 1.05f || v < -0.05f || v > 1.05f) return false; // not over her screen
	u = std::max(0.0f, std::min(1.0f, u)); v = std::max(0.0f, std::min(1.0f, v));
	return true;
}
static POINT screen_to_game_cursor(float u, float v)
{
	const quad_map m = make_map();
	RECT cr; GetClientRect(g_game_hwnd, &cr);
	const double w = m.g * u + m.h * v + 1;
	POINT p = { (LONG)((m.a * u + m.b * v + m.c) / w * cr.right), (LONG)((m.d * u + m.e * v + m.f) / w * cr.bottom) };
	ClientToScreen(g_game_hwnd, &p);
	return p;
}
static bool g_mapped_entry = false;

static void enter_pcmode()
{
	if (!g_capturing) { LOG_WARN("PC mode: no desktop capture yet (show the desktop on the TV or the PC first)"); return; }
	hook_game_window();
	GetCursorPos(&g_saved_cursor);
	POINT c = { (g_mon_rect.left + g_mon_rect.right) / 2, (g_mon_rect.top + g_mon_rect.bottom) / 2 };
	float u, v;
	g_mapped_entry = game_cursor_to_screen(g_saved_cursor, u, v);
	if (g_linux)
	{
		g_pcmode = true;
		linux_pc_on(g_mapped_entry ? u : -1.0f, g_mapped_entry ? v : -1.0f);
		LOG_INFO("PC mode ON (mouse and keyboard go to the shared monitor%s)", g_mapped_entry ? ", cursor placed where the game's cursor was" : "");
		return;
	}
	if (g_mapped_entry) // same spot on the desktop as the game's cursor on her screen
	{
		c.x = g_mon_rect.left + (LONG)(u * (g_mon_rect.right - g_mon_rect.left - 1));
		c.y = g_mon_rect.top + (LONG)(v * (g_mon_rect.bottom - g_mon_rect.top - 1));
	}
	g_pcmode = true;
	SetCursorPos(c.x, c.y);
	HWND w = WindowFromPoint(c);
	if (w) w = GetAncestor(w, GA_ROOT);
	if (w == nullptr || w == g_game_hwnd) w = GetShellWindow(); // empty desktop: the desktop takes the keyboard
	SetForegroundWindow(w);
	ClipCursor(&g_mon_rect);
	LOG_INFO("PC mode ON (mouse and keyboard go to monitor %d%s)", g_monitor, g_mapped_entry ? ", cursor placed where the game's cursor was" : "");
}

static void leave_pcmode(bool back_to_game, const char *why)
{
	g_pcmode = false;
	if (g_linux)
	{
		linux_pc_off(back_to_game);
		LOG_INFO("PC mode OFF (%s)", why);
		return;
	}
	ClipCursor(nullptr);
	if (back_to_game)
	{
		POINT target = g_saved_cursor;
		if (g_mapped_entry) // game cursor goes where the desktop cursor is now
		{
			POINT d; GetCursorPos(&d);
			const float u = (d.x - g_mon_rect.left) / float(std::max<LONG>(1, g_mon_rect.right - g_mon_rect.left - 1));
			const float v = (d.y - g_mon_rect.top) / float(std::max<LONG>(1, g_mon_rect.bottom - g_mon_rect.top - 1));
			target = screen_to_game_cursor(std::max(0.0f, std::min(1.0f, u)), std::max(0.0f, std::min(1.0f, v)));
		}
		SetCursorPos(target.x, target.y);
		focus_window(g_game_hwnd);
	}
	else
	{
		// Put the cursor back on the screen the game is on, so it can be seen
		RECT r = {};
		HMONITOR m = MonitorFromWindow(g_game_hwnd, MONITOR_DEFAULTTOPRIMARY);
		MONITORINFO mi = { sizeof(mi) };
		if (GetMonitorInfoW(m, &mi)) r = mi.rcWork;
		SetCursorPos((r.left + r.right) / 2, (r.top + r.bottom) / 2);
	}
	LOG_INFO("PC mode OFF (%s)", why);
}

// Calibration: remember where the game's cursor is as a corner of Jodi's screen
static void calibrate(int corner) // 1 = top-left, 2 = bottom-right, 3 = top-right, 4 = bottom-left
{
	if (g_game_hwnd == nullptr) return;
	POINT p; GetCursorPos(&p);
	RECT cr; if (!GetClientRect(g_game_hwnd, &cr) || cr.right <= 0 || cr.bottom <= 0) return;
	ScreenToClient(g_game_hwnd, &p);
	const int fx = (int)std::lround(10000.0 * p.x / cr.right), fy = (int)std::lround(10000.0 * p.y / cr.bottom);
	static const char *kx[] = { "", "PCScreenLeft", "PCScreenRight", "PCScreenTRx", "PCScreenBLx" };
	static const char *ky[] = { "", "PCScreenTop", "PCScreenBottom", "PCScreenTRy", "PCScreenBLy" };
	static const char *names[] = { "", "top-left", "bottom-right", "top-right", "bottom-left" };
	reshade::set_config_value(nullptr, "DesktopTV", kx[corner], fx);
	reshade::set_config_value(nullptr, "DesktopTV", ky[corner], fy);
	const float x = fx / 10000.0f, y = fy / 10000.0f;
	switch (corner)
	{
	case 1: g_scr_l = x; g_scr_t = y; break;
	case 2: g_scr_r = x; g_scr_b = y; break;
	case 3: g_scr_trx = x; g_scr_try = y; break;
	case 4: g_scr_blx = x; g_scr_bly = y; break;
	}
	LOG_INFO("Jodi's screen %s corner saved (%d, %d per 10000 of the game window)", names[corner], fx, fy);
	if (g_scr_l >= 0 && g_scr_r > g_scr_l && g_scr_b > g_scr_t)
		LOG_INFO("Jodi's screen: %s", (g_scr_trx >= 0 && g_scr_blx >= 0) ? "4 corners set (tilt corrected)" : "2 corners set (add F10 top-right + F11 bottom-left for the tilt)");
}

// Called every ~30 ms on the background thread
static void watch_pcmode()
{
	if (!g_pcmode)
		return;
	if (g_linux) { linux_watch_pcmode(); return; }
	ClipCursor(&g_mon_rect); // the game releases the clip when it loses focus: keep ours
	static int lost = 0;
	bool gone = IsIconic(g_game_hwnd) || !IsWindowVisible(g_game_hwnd);
	if (gone && g_keepfront && lost == 0)
		ShowWindowAsync(g_game_hwnd, SW_SHOWNOACTIVATE); // try to bring it back once
	// The player switched to something on the main screen (Alt+Tab, Windows key ...)
	HWND fg = GetForegroundWindow();
	if (fg && fg != g_game_hwnd)
	{
		HMONITOR mon_fg = MonitorFromWindow(fg, MONITOR_DEFAULTTONULL);
		HMONITOR mon_cap = MonitorFromRect(&g_mon_rect, MONITOR_DEFAULTTONULL);
		if (mon_fg != nullptr && mon_fg != mon_cap && fg != GetShellWindow())
			gone = true;
	}
	lost = gone ? lost + 1 : 0;
	if (lost >= 10) // ~300 ms
	{
		lost = 0;
		leave_pcmode(false, "the game left the screen - cursor released");
	}
}

// ---------------------------------------------------------------- sound

// Process id -> parent process id (browsers play sound from a child process without windows)
static std::map<DWORD, DWORD> process_parents()
{
	std::map<DWORD, DWORD> parents;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) return parents;
	PROCESSENTRY32W pe = { sizeof(pe) };
	for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
		parents[pe.th32ProcessID] = pe.th32ParentProcessID;
	CloseHandle(snap);
	return parents;
}

struct window_places { std::map<DWORD, int> flags; HMONITOR cap; }; // flag 1 = on captured monitor, 2 = elsewhere

static BOOL CALLBACK enum_windows_cb(HWND w, LPARAM lp)
{
	auto *wp = reinterpret_cast<window_places *>(lp);
	if (!IsWindowVisible(w) || IsIconic(w) || GetWindow(w, GW_OWNER) != nullptr)
		return TRUE;
	RECT r;
	if (!GetWindowRect(w, &r) || r.right - r.left < 50 || r.bottom - r.top < 50)
		return TRUE;
	DWORD pid = 0;
	GetWindowThreadProcessId(w, &pid);
	HMONITOR m = MonitorFromWindow(w, MONITOR_DEFAULTTONULL);
	wp->flags[pid] |= (m != nullptr && m == wp->cap) ? 1 : 2;
	return TRUE;
}

// Position data from the DesktopTVPos UE4SS script (DesktopTV_pos.txt next to this add-on)
struct position_data
{
	bool fresh = false;
	double jodi[3] = {}, cam[3] = {}, cam_yaw = 0;
	bool has_pc = false, has_tv = false;
	double pc[3] = {}, tv[3] = {};
};

static std::string pos_file_path()
{
	char path[MAX_PATH] = {};
	GetModuleFileNameA(g_module, path, MAX_PATH);
	std::string p = path;
	const size_t slash = p.find_last_of("\\/");
	return (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) + "DesktopTV_pos.txt";
}

static position_data read_position()
{
	position_data d;
	if (!g_spatial)
		return d;
	static const std::string path = pos_file_path();
	WIN32_FILE_ATTRIBUTE_DATA fa = {};
	if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa))
		return d;
	FILETIME now_ft;
	GetSystemTimeAsFileTime(&now_ft);
	ULARGE_INTEGER now, mod;
	now.LowPart = now_ft.dwLowDateTime; now.HighPart = now_ft.dwHighDateTime;
	mod.LowPart = fa.ftLastWriteTime.dwLowDateTime; mod.HighPart = fa.ftLastWriteTime.dwHighDateTime;
	if (now.QuadPart > mod.QuadPart + 20000000ULL) // older than 2 s: the script is not running (menu, other map)
		return d;
	FILE *f = nullptr;
	if (fopen_s(&f, path.c_str(), "r") != 0 || f == nullptr)
		return d;
	char line[256];
	bool has_jodi = false, has_cam = false;
	while (fgets(line, sizeof(line), f))
	{
		double x, y, z, w;
		if (sscanf_s(line, "jodi %lf %lf %lf", &x, &y, &z) == 3) { d.jodi[0] = x; d.jodi[1] = y; d.jodi[2] = z; has_jodi = true; }
		else if (sscanf_s(line, "cam %lf %lf %lf %lf", &x, &y, &z, &w) == 4) { d.cam[0] = x; d.cam[1] = y; d.cam[2] = z; d.cam_yaw = w; has_cam = true; }
		else if (sscanf_s(line, "pc %lf %lf %lf", &x, &y, &z) == 3) { d.pc[0] = x; d.pc[1] = y; d.pc[2] = z; d.has_pc = true; }
		else if (sscanf_s(line, "tv %lf %lf %lf", &x, &y, &z) == 3) { d.tv[0] = x; d.tv[1] = y; d.tv[2] = z; d.has_tv = true; }
	}
	fclose(f);
	d.fresh = has_jodi && has_cam;
	return d;
}

// Left/right gains (0..1) for one sound source at spot, heard by Jodi with the camera looking along cam_yaw
static void source_gains(const position_data &d, const double spot[3], float &l, float &r)
{
	const double dx = spot[0] - d.jodi[0], dy = spot[1] - d.jodi[1], dz = spot[2] - d.jodi[2];
	const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
	double vol = dist <= g_near ? 1.0 : dist >= g_far ? 0.0 : 1.0 - (dist - g_near) / double(g_far - g_near);
	vol = vol * vol; // softer fade at the far end
	// Direction from the camera to the spot, against the camera's right-hand side (Unreal: X forward, Y right)
	const double cx = spot[0] - d.cam[0], cy = spot[1] - d.cam[1];
	const double len = std::sqrt(cx * cx + cy * cy);
	double pan = 0;
	if (len > 30)
	{
		const double yaw = d.cam_yaw * 3.14159265358979 / 180.0;
		pan = (cx * -std::sin(yaw) + cy * std::cos(yaw)) / len; // -1 = left, +1 = right
		pan *= 0.85; // never fully one-sided
	}
	const double a = (pan + 1.0) * 3.14159265358979 / 4.0; // equal-power panning, 1/1 in the middle
	l = float(vol * std::min(1.0, std::sqrt(2.0) * std::cos(a)));
	r = float(vol * std::min(1.0, std::sqrt(2.0) * std::sin(a)));
}

// What the second-screen programs should sound like now: left/right gains, or (-1,-1) = no position data
static void wanted_gains(bool tv_on, bool pc_on, float &l, float &r)
{
	l = r = -1;
	if (!g_spatial)
		return;
	const position_data d = read_position();
	if (!d.fresh)
		return;
	if (g_pcmode) { l = r = 1; return; }
	l = r = 0;
	float a, b;
	if (pc_on && d.has_pc) { source_gains(d, d.pc, a, b); l = std::max(l, a); r = std::max(r, b); }
	if (tv_on && d.has_tv) { source_gains(d, d.tv, a, b); l = std::max(l, a); r = std::max(r, b); }
}

struct session_state
{
	bool muted_by_us = false;
	bool volume_saved = false;
	float master = 1;
	std::vector<float> channels;
	float cur_l = -1, cur_r = -1;
};
static std::map<DWORD, session_state> g_sessions;

static bool get_mute(IAudioSessionControl2 *ctl)
{
	BOOL m = FALSE;
	ISimpleAudioVolume *vol = nullptr;
	if (SUCCEEDED(ctl->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void **>(&vol)))) { vol->GetMute(&m); vol->Release(); }
	return m != FALSE;
}

static void set_session_mute(IAudioSessionControl2 *ctl, BOOL mute)
{
	ISimpleAudioVolume *vol = nullptr;
	if (SUCCEEDED(ctl->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void **>(&vol))))
	{
		vol->SetMute(mute, nullptr);
		vol->Release();
	}
}

// Set left/right volume (relative to the program's own volume, saved the first time)
static void set_session_gains(IAudioSessionControl2 *ctl, session_state &st, float l, float r)
{
	IChannelAudioVolume *ch = nullptr;
	ISimpleAudioVolume *sv = nullptr;
	ctl->QueryInterface(__uuidof(IChannelAudioVolume), reinterpret_cast<void **>(&ch));
	ctl->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void **>(&sv));
	UINT n = 0;
	if (ch) ch->GetChannelCount(&n);
	if (!st.volume_saved)
	{
		if (sv) sv->GetMasterVolume(&st.master);
		st.channels.assign(n, 1.0f);
		for (UINT i = 0; i < n; ++i) ch->GetChannelVolume(i, &st.channels[i]);
		st.volume_saved = true;
	}
	if (ch && n == 2 && st.channels.size() == 2)
	{
		ch->SetChannelVolume(0, st.channels[0] * l, nullptr);
		ch->SetChannelVolume(1, st.channels[1] * r, nullptr);
	}
	else if (sv)
	{
		sv->SetMasterVolume(st.master * std::max(l, r), nullptr); // not stereo: fade only
	}
	if (ch) ch->Release();
	if (sv) sv->Release();
}

static void restore_session_volume(IAudioSessionControl2 *ctl, session_state &st)
{
	if (!st.volume_saved) return;
	IChannelAudioVolume *ch = nullptr;
	ISimpleAudioVolume *sv = nullptr;
	if (SUCCEEDED(ctl->QueryInterface(__uuidof(IChannelAudioVolume), reinterpret_cast<void **>(&ch))))
	{
		UINT n = 0;
		ch->GetChannelCount(&n);
		for (UINT i = 0; i < n && i < st.channels.size(); ++i) ch->SetChannelVolume(i, st.channels[i], nullptr);
		ch->Release();
	}
	if (SUCCEEDED(ctl->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void **>(&sv))))
	{
		sv->SetMasterVolume(st.master, nullptr);
		sv->Release();
	}
	st.volume_saved = false;
	st.cur_l = st.cur_r = -1;
}

static HMONITOR find_captured_monitor()
{
	if (g_capturing)
		return MonitorFromRect(&g_mon_rect, MONITOR_DEFAULTTONULL);
	// Not capturing yet: find monitor number g_monitor the same way the capture numbers them
	HMONITOR found = nullptr;
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
		return nullptr;
	int n = 0;
	IDXGIAdapter1 *a = nullptr;
	for (UINT ai = 0; found == nullptr && factory->EnumAdapters1(ai, &a) != DXGI_ERROR_NOT_FOUND; ++ai)
	{
		IDXGIOutput *o = nullptr;
		for (UINT oi = 0; a->EnumOutputs(oi, &o) != DXGI_ERROR_NOT_FOUND; ++oi)
		{
			DXGI_OUTPUT_DESC od = {};
			o->GetDesc(&od);
			o->Release();
			if (++n == g_monitor) { found = od.Monitor; break; }
		}
		a->Release();
	}
	factory->Release();
	return found;
}

// One pass over the programs playing sound. want = on/off rule (TV shows the desktop, or PC mode);
// gl/gr = left/right gains from the position script, or -1 when there is none. restore_all = undo everything.
// ---- record of the programs whose sound we changed (survives a crash of the game)
static std::set<std::wstring> g_touched;
static std::string touched_file_path()
{
	std::string p = pos_file_path();
	return p.substr(0, p.find_last_of("\\/") + 1) + "DesktopTV_touched.txt";
}
static std::wstring exe_of(DWORD pid)
{
	std::wstring name;
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (h)
	{
		wchar_t buf[MAX_PATH]; DWORD n = MAX_PATH;
		if (QueryFullProcessImageNameW(h, 0, buf, &n)) name.assign(buf, n);
		CloseHandle(h);
	}
	const size_t slash = name.find_last_of(L"\\/");
	if (slash != std::wstring::npos) name = name.substr(slash + 1);
	for (auto &ch : name) ch = (wchar_t)towlower(ch);
	return name;
}
static void save_touched()
{
	const std::string path = touched_file_path();
	if (g_touched.empty()) { DeleteFileA(path.c_str()); return; }
	FILE *f = nullptr;
	if (_wfopen_s(&f, std::wstring(path.begin(), path.end()).c_str(), L"w, ccs=UTF-8") != 0 || !f) return;
	for (const auto &e : g_touched) fwprintf(f, L"%s\n", e.c_str());
	fclose(f);
}
static void note_touched(DWORD pid)
{
	const std::wstring e = exe_of(pid);
	if (!e.empty() && g_touched.insert(e).second) save_touched();
}
static std::set<std::wstring> load_touched()
{
	std::set<std::wstring> out;
	const std::string path = touched_file_path();
	FILE *f = nullptr;
	if (_wfopen_s(&f, std::wstring(path.begin(), path.end()).c_str(), L"r, ccs=UTF-8") != 0 || !f) return out;
	wchar_t line[MAX_PATH];
	while (fgetws(line, MAX_PATH, f))
	{
		std::wstring e = line;
		while (!e.empty() && (e.back() == L'\n' || e.back() == L'\r' || e.back() == L' ')) e.pop_back();
		if (!e.empty()) out.insert(e);
	}
	fclose(f);
	return out;
}

// At start: programs listed from a run that did not restore them get unmuted, left/right reset to full
static void heal_from_last_run()
{
	if (g_linux) return; // the Linux helper keeps its own record
	const auto list = load_touched();
	if (list.empty()) return;
	IMMDeviceEnumerator *en = nullptr; IMMDevice *dev = nullptr; IAudioSessionManager2 *mgr = nullptr; IAudioSessionEnumerator *sessions = nullptr;
	if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&en))) &&
		SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev)) &&
		SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&mgr))) &&
		SUCCEEDED(mgr->GetSessionEnumerator(&sessions)))
	{
		int count = 0;
		sessions->GetCount(&count);
		for (int i = 0; i < count; ++i)
		{
			IAudioSessionControl *c = nullptr; IAudioSessionControl2 *c2 = nullptr;
			if (FAILED(sessions->GetSession(i, &c))) continue;
			if (SUCCEEDED(c->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void **>(&c2))))
			{
				DWORD pid = 0;
				c2->GetProcessId(&pid);
				if (pid != 0 && list.count(exe_of(pid)))
				{
					set_session_mute(c2, FALSE);
					IChannelAudioVolume *ch = nullptr;
					if (SUCCEEDED(c2->QueryInterface(__uuidof(IChannelAudioVolume), reinterpret_cast<void **>(&ch))))
					{
						UINT n = 0; ch->GetChannelCount(&n);
						for (UINT k = 0; k < n; ++k) ch->SetChannelVolume(k, 1.0f, nullptr);
						ch->Release();
					}
					LOG_INFO("sound: process %lu given back its sound (left changed by an earlier run)", pid);
				}
				c2->Release();
			}
			c->Release();
		}
	}
	if (sessions) sessions->Release(); if (mgr) mgr->Release(); if (dev) dev->Release(); if (en) en->Release();
	DeleteFileA(touched_file_path().c_str());
}

static void update_sound(bool restore_all, bool want, float gl, float gr)
{
	if (!g_sound)
		return;
	if (g_linux) { linux_sound(restore_all, want, gl, gr); return; }
	window_places places;
	places.cap = restore_all ? nullptr : find_captured_monitor();
	if (!restore_all && places.cap == nullptr)
		return;
	if (!restore_all)
		EnumWindows(enum_windows_cb, reinterpret_cast<LPARAM>(&places));
	const auto parents = process_parents();
	const DWORD self = GetCurrentProcessId();
	const bool spatial = gl >= 0;

	IMMDeviceEnumerator *en = nullptr;
	IMMDevice *dev = nullptr;
	IAudioSessionManager2 *mgr = nullptr;
	IAudioSessionEnumerator *sessions = nullptr;
	if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&en))) &&
		SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev)) &&
		SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&mgr))) &&
		SUCCEEDED(mgr->GetSessionEnumerator(&sessions)))
	{
		int count = 0;
		sessions->GetCount(&count);
		for (int i = 0; i < count; ++i)
		{
			IAudioSessionControl *c = nullptr;
			IAudioSessionControl2 *c2 = nullptr;
			if (FAILED(sessions->GetSession(i, &c))) continue;
			if (SUCCEEDED(c->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void **>(&c2))))
			{
				DWORD pid = 0;
				c2->GetProcessId(&pid);
				if (pid != 0 && pid != self)
				{
					auto known = g_sessions.find(pid);
					if (restore_all)
					{
						if (known != g_sessions.end())
						{
							restore_session_volume(c2, known->second);
							if (known->second.muted_by_us) set_session_mute(c2, FALSE);
						}
					}
					else
					{
						// Where are this program's windows? (or its parent's, for browsers)
						int f = 0;
						auto it = places.flags.find(pid);
						if (it != places.flags.end()) f = it->second;
						else { auto p = parents.find(pid); if (p != parents.end()) { auto q = places.flags.find(p->second); if (q != places.flags.end()) f = q->second; } }
						const bool only_on_second = f == 1;
						session_state &st = g_sessions[pid];
						if (!only_on_second)
						{
							// Not (or no longer) a second-screen program: undo what we did to it
							restore_session_volume(c2, st);
							if (st.muted_by_us) { set_session_mute(c2, FALSE); LOG_INFO("sound: unmuted process %lu (left the second screen)", pid); }
							g_sessions.erase(pid);
						}
						else if (spatial)
						{
							// Position mode: never muted, volume follows Jodi (smoothly)
							if (st.muted_by_us || (get_mute(c2) && !st.volume_saved)) { set_session_mute(c2, FALSE); st.muted_by_us = false; }
							if (st.cur_l < 0) { st.cur_l = gl; st.cur_r = gr; LOG_INFO("sound: process %lu follows Jodi's position", pid); }
							st.cur_l += std::max(-0.15f, std::min(0.15f, gl - st.cur_l));
							st.cur_r += std::max(-0.15f, std::min(0.15f, gr - st.cur_r));
							if (!st.volume_saved) note_touched(pid);
							set_session_gains(c2, st, st.cur_l, st.cur_r);
						}
						else
						{
							restore_session_volume(c2, st);
							const bool m = get_mute(c2);
							if (!want && !m) { note_touched(pid); set_session_mute(c2, TRUE); st.muted_by_us = true; LOG_INFO("sound: muted process %lu (on the second screen)", pid); }
							else if (want && m) { set_session_mute(c2, FALSE); st.muted_by_us = false; LOG_INFO("sound: unmuted process %lu", pid); }
						}
					}
				}
				c2->Release();
			}
			c->Release();
		}
	}
	if (sessions) sessions->Release();
	if (mgr) mgr->Release();
	if (dev) dev->Release();
	if (en) en->Release();
	if (restore_all) { g_sessions.clear(); g_touched.clear(); save_touched(); }
}

// ---------------------------------------------------------------- background thread

// Our two keys are caught here, before any program sees them (while the game is in front or PC mode is on)
static std::atomic<bool> g_req_pc { false }, g_req_hide { false };
static bool g_pc_key_held = false, g_hide_key_held = false;

static LRESULT CALLBACK keyboard_hook(int code, WPARAM wp, LPARAM lp)
{
	if (code == HC_ACTION)
	{
		const auto *k = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lp);
		const bool active = g_pcmode || (g_in_office && GetForegroundWindow() == g_game_hwnd);
		if (active && !(k->flags & LLKHF_INJECTED) && (k->vkCode == VK_F8 || k->vkCode == VK_F9 || k->vkCode == VK_F10 || k->vkCode == VK_F11) &&
			(GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000))
		{
			if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) g_req_calib = k->vkCode == VK_F8 ? 1 : k->vkCode == VK_F9 ? 2 : k->vkCode == VK_F10 ? 3 : 4;
			return 1; // swallowed (so the Ctrl+F8 / F9 spot keys of DesktopTVPos don't fire too)
		}
		const bool ours = (int)k->vkCode == g_pckey || (int)k->vkCode == g_hidekey;
		if (ours && !(k->flags & LLKHF_INJECTED) && active)
		{
			const bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
			bool &held = (int)k->vkCode == g_pckey ? g_pc_key_held : g_hide_key_held;
			if (down && !held)
				((int)k->vkCode == g_pckey ? g_req_pc : g_req_hide) = true;
			held = down;
			return 1; // swallowed
		}
	}
	return CallNextHookEx(nullptr, code, wp, lp);
}

static void worker()
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	heal_from_last_run();
	HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_hook, g_module, 0);
	if (hook == nullptr) LOG_WARN("could not install the keyboard hook - keys will also reach other programs");
	int tick = 0;
	SHORT pc_prev = 0, hide_prev = 0;
	while (!g_worker_stop)
	{
		// The low-level keyboard hook needs this thread to pump messages
		MsgWaitForMultipleObjects(0, nullptr, FALSE, 30, QS_ALLINPUT);
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }

		if (hook == nullptr) // fallback: watch the keys like v3
		{
			const SHORT ps = GetAsyncKeyState(g_pckey), hs = GetAsyncKeyState(g_hidekey);
			if ((ps & 0x8000) && !(pc_prev & 0x8000)) g_req_pc = true;
			if ((hs & 0x8000) && !(hide_prev & 0x8000)) g_req_hide = true;
			pc_prev = ps; hide_prev = hs;
		}
		if (g_req_pc.exchange(false))
		{
			if (g_pcmode) leave_pcmode(true, "key");
			else enter_pcmode();
		}
		if (g_req_hide.exchange(false))
		{
			g_hide_ui = !g_hide_ui;
			LOG_INFO("Jodi's PC interface %s", g_hide_ui ? "hidden" : "shown");
		}

		if (const int corner = g_req_calib.exchange(0)) calibrate(corner);
		if (g_req_restore.exchange(false))
		{
			if (g_pcmode) leave_pcmode(false, "game closing");
			update_sound(true, true, -1, -1);
			LOG_INFO("game closing: sound of the second-screen programs given back");
			if (g_restored_event) SetEvent(g_restored_event);
		}
		if (g_closing)
			continue;

		watch_pcmode();
		if (++tick % 4 == 0) // ~0.12 s
		{
			bool tv_on, pc_on;
			{
				std::lock_guard<std::mutex> lock(g_targets_mutex);
				tv_on = std::any_of(g_targets.begin(), g_targets.end(), [](const target &t) { return t.kind == 0; });
				pc_on = std::any_of(g_targets.begin(), g_targets.end(), [](const target &t) { return t.kind == 1; });
			}
			const bool office = tv_on || pc_on || g_pcmode;
			if (office != g_in_office.exchange(office))
			{
				LOG_INFO(office ? "Jodi's office: DesktopTV active" : "left Jodi's office: DesktopTV idle");
				if (!office) // one last sound pass, then nothing until she is back
				{
					if (g_sound_outside) update_sound(true, true, -1, -1); // everything back to normal
					else update_sound(false, false, -1, -1);           // second-screen programs muted
				}
			}
			if (!office)
				continue;
			float gl, gr;
			wanted_gains(tv_on, pc_on, gl, gr);
			if (gl >= 0 || tick % 16 == 0) // position mode updates often, on/off mode every ~0.5 s
				update_sound(false, tv_on || g_pcmode, gl, gr);
		}
	}
	if (hook) UnhookWindowsHookEx(hook);
	if (g_pcmode) leave_pcmode(false, "game closing");
	update_sound(true, true, -1, -1);
	CoUninitialize();
}

static void start_worker()
{
	if (g_worker.joinable()) return;
	g_worker_stop = false;
	g_worker = std::thread(worker);
}

static void stop_worker()
{
	if (!g_worker.joinable()) return;
	g_worker_stop = true;
	g_worker.join();
}

// ---------------------------------------------------------------- ReShade events

// Present runs on the thread that owns the immediate context: safe place to copy the desktop.
static void on_present(command_queue *queue, swapchain *, const rect *, const rect *, uint32_t, const rect *)
{
	device *dev = queue->get_device();
	if (dev != g_device.load())
		return;
	if (g_target_count.load() == 0 && !g_pcmode && !g_linux)
		return; // only work while a replaced texture exists (the Linux part also says when it is not needed)

	const auto now = std::chrono::steady_clock::now();
	auto *d3d = reinterpret_cast<ID3D11Device *>(dev->get_native());
	if (g_linux) { linux_present(d3d); return; }
	if (g_dupl == nullptr)
	{
		if (now < g_next_retry) return;
		if (!create_capture(d3d)) { g_next_retry = now + std::chrono::seconds(15); return; }
	}
	if (now - g_last_capture < std::chrono::milliseconds(1000 / g_fps))
		return;
	g_last_capture = now;
	capture(d3d);
}

static int target_kind(const resource_desc &d)
{
	if (d.type != resource_type::texture_2d)
		return -1;
	const format f = format_to_typeless(d.texture.format);
	if (d.texture.width == (uint32_t)g_width && d.texture.height == (uint32_t)g_height)
	{
		switch (f)
		{
		case format::b8g8r8a8_typeless:
		case format::r8g8b8a8_typeless:
		case format::b8g8r8x8_typeless:
		case format::r10g10b10a2_typeless:
		case format::r16g16b16a16_typeless:
			return 0; // TV marker video (not the NV12 / R8 planes the video decoder writes)
		default:
			break;
		}
	}
	if (g_pcscreen && d.texture.width == (uint32_t)g_pcwidth && d.texture.height == (uint32_t)g_pcheight && f == format::bc1_typeless)
		return 1; // PC wallpaper (DXT1)
	// Proton: the wallpaper is never read; the game draws her whole screen into a BGRA render target and puts that on the monitor
	if (g_linux && g_pcscreen && d.texture.width == (uint32_t)g_pcwidth && d.texture.height == (uint32_t)g_pcheight &&
		f == format::b8g8r8a8_typeless && (d.usage & resource_usage::render_target) != 0)
		return 1;
	return -1;
}

static void on_init_resource_view(device *dev, resource res, resource_usage usage, const resource_view_desc &vdesc, resource_view view)
{
	if (usage != resource_usage::shader_resource || res == 0 || dev != g_device.load())
		return;
	const resource_desc d = native_desc(res);
	const int kind = target_kind(d);
	if (kind < 0)
		return;
	std::lock_guard<std::mutex> lock(g_targets_mutex);
	const int setting = kind == 0 ? g_srgb : g_pcsrgb;
	const format vf = vdesc.format == format::unknown ? d.texture.format : vdesc.format;
	const bool game_srgb = vf == format::b8g8r8a8_unorm_srgb || vf == format::r8g8b8a8_unorm_srgb || vf == format::b8g8r8x8_unorm_srgb ||
		vf == format::bc1_unorm_srgb || vf == format::bc2_unorm_srgb || vf == format::bc3_unorm_srgb || vf == format::bc7_unorm_srgb;
	const bool srgb = setting == 2 ? game_srgb : setting != 0;
	g_targets.push_back({ view.handle, kind, srgb });
	LOG_INFO("%s: the game reads it as format %u (%s) -> desktop given as %s", kind == 0 ? "TV" : "PC screen", (unsigned)vf,
		game_srgb ? "sRGB" : "plain", srgb ? "sRGB" : "plain");
	g_target_count = (int)g_targets.size();
	LOG_INFO("%s texture found (%ux%u, format %u, view %llx) - %d active", kind == 0 ? "TV" : "PC screen",
		d.texture.width, d.texture.height, (unsigned)d.texture.format, (unsigned long long)view.handle, g_target_count.load());
}

static void on_destroy_resource_view(device *, resource_view view)
{
	if (g_target_count.load() == 0)
		return;
	std::lock_guard<std::mutex> lock(g_targets_mutex);
	auto it = std::find_if(g_targets.begin(), g_targets.end(), [&](const target &t) { return t.view == view.handle; });
	if (it == g_targets.end())
		return;
	g_targets.erase(it);
	g_target_count = (int)g_targets.size();
	LOG_INFO("replaced texture released - %d active", g_target_count.load());
}

static void on_push_descriptors(command_list *cmd_list, shader_stage stages, pipeline_layout layout, uint32_t param, const descriptor_table_update &update)
{
	if (g_target_count.load() == 0 || update.type != descriptor_type::shader_resource_view || update.count == 0 || update.count > 128)
		return;
	if (g_srv[0][0].load() == nullptr || g_srv[1][1].load() == nullptr)
		return;
	const auto *views = static_cast<const resource_view *>(update.descriptors);
	resource_view replaced[128];
	int any = -1;
	{
		std::lock_guard<std::mutex> lock(g_targets_mutex);
		for (uint32_t i = 0; i < update.count; ++i)
		{
			replaced[i] = views[i];
			if (views[i].handle == 0)
				continue;
			for (const target &t : g_targets)
			{
				if (t.view == views[i].handle)
				{
					replaced[i] = resource_view { reinterpret_cast<uint64_t>(g_srv[t.kind][t.srgb ? 1 : 0].load()) };
					any = t.kind;
					break;
				}
			}
		}
	}
	if (any < 0)
		return;
	descriptor_table_update u = update;
	u.descriptors = replaced;
	cmd_list->push_descriptors(stages, layout, param, u);
	if (!g_logged_swap[any]) { g_logged_swap[any] = true; LOG_INFO("desktop is now drawn on the %s", any == 0 ? "TV" : "PC screen"); }
	if (any == 1 && g_cur_rt != 0 && g_back_buffers.count(g_cur_rt) == 0)
	{
		// The room is drawn into several render targets at once (the 3D monitor on her desk is part of it):
		// skipping draws there blacks out parts of the room. Her interface on its own is drawn into one.
		g_pc_rt_ok = g_cur_rt_count == 1;
		if (g_cur_rt != g_logged_rt)
		{
			g_logged_rt = g_cur_rt;
			const resource_desc rd = native_desc(resource { g_cur_rt });
			static std::set<uint64_t> seen; // log each kind of surface once
			const uint64_t key = (uint64_t)rd.texture.width << 40 | (uint64_t)rd.texture.height << 20 | (uint64_t)rd.texture.format << 4 | g_cur_rt_count;
			if (seen.insert(key).second)
				LOG_INFO("PC screen drawn into a surface %ux%u format %u with %u render target(s) -> interface hiding %s",
					rd.texture.width, rd.texture.height, (unsigned)rd.texture.format, g_cur_rt_count,
					g_pc_rt_ok ? "allowed" : "not used here (part of the room)");
		}
		g_pc_rt = g_cur_rt;
		g_pc_state = 1; // the next draw is the wallpaper
	}
}

static void on_bind_render_targets(command_list *cmd_list, uint32_t count, const resource_view *rtvs, resource_view)
{
	uint64_t rt = 0;
	if (count > 0 && rtvs[0].handle != 0)
		rt = native_resource_of_view(rtvs[0]);
	if (rt != g_cur_rt || count != g_cur_rt_count)
	{
		if (g_skipped > 0 && g_pc_state != 0) { static int logs = 0; if (logs < 5) { ++logs; LOG_INFO("interface hiding: skipped %d draw(s) on the PC screen", g_skipped); } }
		g_skipped = 0;
		g_cur_rt = rt;
		g_cur_rt_count = count;
		g_pc_state = 0;
	}
}

// Returning true skips the draw
static bool skip_draw()
{
	if (g_pc_state == 0 || g_cur_rt != g_pc_rt)
		return false;
	if (g_pc_state == 1) { g_pc_state = 2; return false; } // the wallpaper itself (= the desktop)
	if (!(g_hide_ui || (g_pcmode && g_hide_in_pcmode)) || !g_pc_rt_ok)
		return false;
	++g_skipped;
	if (!g_logged_hide) { g_logged_hide = true; LOG_INFO("hiding Jodi's PC interface (draws after the wallpaper on the PC screen)"); }
	return true;
}
static bool on_draw(command_list *, uint32_t, uint32_t, uint32_t, uint32_t) { return skip_draw(); }
static bool on_draw_indexed(command_list *, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { return skip_draw(); }
static bool on_draw_indirect(command_list *, indirect_command type, resource, uint64_t, uint32_t, uint32_t)
{
	return type != indirect_command::dispatch && skip_draw();
}

static void on_init_swapchain(swapchain *sc, bool)
{
	device *dev = sc->get_device();
	if (dev->get_api() != device_api::d3d11) return;
	g_game_hwnd = static_cast<HWND>(reinterpret_cast<void *>(sc->get_hwnd()));
	const uint64_t bb = native_back_buffer(sc); // D3D11: one back buffer the game can reach
	if (bb != 0)
	{
		g_back_buffers.insert(bb);
		const resource_desc bd = native_desc(resource { bb });
		g_bb_w = bd.texture.width; g_bb_h = bd.texture.height;
	}
	if (g_device.exchange(dev) != dev)
		LOG_INFO("using the game's D3D11 device %p", (void *)dev);
	start_worker();
	hook_game_window();
}

static void on_destroy_swapchain(swapchain *sc, bool resize)
{
	g_back_buffers.erase(native_back_buffer(sc));
	if (!resize && sc->get_device() == g_device.load())
	{
		stop_worker();
		unhook_game_window();
	}
}

static void on_destroy_device(device *dev)
{
	if (dev != g_device.load()) return;
	stop_worker();
	unhook_game_window();
	if (g_linux) linux_stop();
	release_capture();
	g_device = nullptr;
	std::lock_guard<std::mutex> lock(g_targets_mutex);
	g_targets.clear();
	g_target_count = 0;
}

#include "desktoptv_linux.inl"

extern "C" __declspec(dllexport) const char *NAME = "DesktopTV";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Shows a monitor of this PC on Jodi's TV and PC screen; PC mode moves mouse and keyboard there; hides Jodi's PC interface; mutes second-screen sound when not shown.";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		g_module = module;
		if (!reshade::register_addon(module))
			return FALSE;
		read_settings();
		reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
		reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
		reshade::register_event<reshade::addon_event::init_resource_view>(on_init_resource_view);
		reshade::register_event<reshade::addon_event::destroy_resource_view>(on_destroy_resource_view);
		reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
		reshade::register_event<reshade::addon_event::present>(on_present);
		reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_draw_indirect);
		LOG_INFO("v4.8 loaded");
		break;
	case DLL_PROCESS_DETACH:
		if (g_pcmode) ClipCursor(nullptr);
		if (g_worker.joinable()) { g_worker_stop = true; g_worker.detach(); } // cannot wait inside DllMain
		if (g_linux) linux_stop();
		reshade::unregister_addon(module);
		break;
	}
	return TRUE;
}
