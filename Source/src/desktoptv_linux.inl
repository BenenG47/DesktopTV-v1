// DesktopTV on Linux (Proton) - included at the end of desktoptv_addon.cpp.
//
// Wine cannot see the Linux desktop, move the real mouse onto it or reach the sound of Linux programs.
// Linux/desktoptv_capture.py does those parts (desktop portal, PipeWire, KWin) and talks to this
// add-on over TCP on 127.0.0.1:LinuxPort. The add-on keeps every rule and calculation; only the picture
// source, moving the pointer and setting the volume go through the helper.
//
//   LinuxSource=2  0 = Windows only, 1 = always the Linux helper, 2 = the Linux helper when running in Wine/Proton
//   LinuxPort=47474
//
// Messages, both directions: 4-byte type, uint32 payload length (little endian), payload.
//   from the helper: FRAM u32 w, u32 h, w*h*4 BGRA | CPOS i32 x, i32 y, u32 visible (cursor hot spot)
//                    CSHP u32 w, u32 h, i32 hx, i32 hy, w*h*4 BGRA (straight alpha) | TEXT line
//   to the helper:   TEXT line ("hello", "fps", "active", "pc_on", "pc_off", "sound", "sound_restore")

#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")

static int g_linux_source = 2, g_linux_port = 47474;

static std::mutex g_net_mutex; // everything below up to g_net_socket
static std::vector<uint8_t> g_net_ready, g_net_front;
static UINT g_net_w = 0, g_net_h = 0;
static bool g_net_new = false;
static POINT g_net_cpos = {}; static bool g_net_cvis = false, g_net_cpos_new = false;
static std::vector<uint8_t> g_net_shape; static UINT g_net_sw = 0, g_net_sh = 0; static POINT g_net_hot = {}; static bool g_net_shape_new = false;
static std::string g_net_lost;
static SOCKET g_net_socket = INVALID_SOCKET;
static std::thread g_net_thread;
static std::atomic<bool> g_net_stop { false };
static std::atomic<int> g_net_active_sent { -1 };
static HANDLE g_net_restored = nullptr;
static POINT g_linux_cursor = {}; // desktop cursor hot spot in picture pixels (present thread)

static void linux_settings()
{
	auto get = [](const char *key, int &v) {
		if (!reshade::get_config_value(nullptr, "DesktopTV", key, v))
			reshade::set_config_value(nullptr, "DesktopTV", key, v);
	};
	get("LinuxSource", g_linux_source); get("LinuxPort", g_linux_port);
	const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
	g_linux = g_linux_source == 1 || (g_linux_source == 2 && wine);
	if (g_linux)
		LOG_INFO("Linux helper on 127.0.0.1:%d%s - Monitor= is chosen in the KDE dialog instead", g_linux_port, wine ? " (Wine/Proton detected)" : "");
}

static bool linux_send(const std::string &text)
{
	std::lock_guard<std::mutex> lock(g_net_mutex);
	if (g_net_socket == INVALID_SOCKET) return false;
	uint8_t hdr[8];
	memcpy(hdr, "TEXT", 4);
	const uint32_t len = (uint32_t)text.size();
	memcpy(hdr + 4, &len, 4);
	return send(g_net_socket, reinterpret_cast<const char *>(hdr), 8, 0) == 8 &&
		(len == 0 || send(g_net_socket, text.data(), (int)len, 0) == (int)len);
}

static bool recv_all(SOCKET s, void *dst, size_t n)
{
	char *p = static_cast<char *>(dst);
	while (n > 0)
	{
		const int r = recv(s, p, (int)std::min<size_t>(n, 1 << 20), 0);
		if (r <= 0) return false;
		p += r; n -= r;
	}
	return true;
}

static void linux_text(const std::string &t)
{
	if (t == "key pc") g_req_pc = true;
	else if (t == "key hide") g_req_hide = true;
	else if (t == "restored") { if (g_net_restored) SetEvent(g_net_restored); }
	else if (t.rfind("pc_lost ", 0) == 0) { std::lock_guard<std::mutex> lock(g_net_mutex); g_net_lost = t.substr(8); }
	else if (t.rfind("info ", 0) == 0) LOG_INFO("%s", t.c_str() + 5);
}

static void net_thread()
{
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	std::vector<uint8_t> payload;
	bool warned = false;
	while (!g_net_stop)
	{
		SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_port = htons((u_short)g_linux_port);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (s == INVALID_SOCKET || connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
		{
			if (s != INVALID_SOCKET) closesocket(s);
			if (!warned) { warned = true; LOG_WARN("Linux helper not reachable on 127.0.0.1:%d - is desktoptv_capture.py running? (retrying)", g_linux_port); }
			Sleep(2000);
			continue;
		}
		const BOOL nodelay = TRUE;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay), sizeof(nodelay));
		{ std::lock_guard<std::mutex> lock(g_net_mutex); g_net_socket = s; }
		LOG_INFO("connected to the Linux helper");
		warned = false;
		char hello[160];
		snprintf(hello, sizeof(hello), "hello v4.8-linux pckey=%d hidekey=%d keepfront=%d", g_pckey, g_hidekey, g_keepfront);
		linux_send(hello);
		linux_send("fps " + std::to_string(g_fps));
		g_net_active_sent = -1; // sent again by the next present
		for (;;)
		{
			uint8_t hdr[8];
			if (!recv_all(s, hdr, 8)) break;
			uint32_t len; memcpy(&len, hdr + 4, 4);
			if (len > 8192u * 8192u * 4u + 8u) { LOG_WARN("Linux helper sent a bad message"); break; }
			payload.resize(len);
			if (len && !recv_all(s, payload.data(), len)) break;
			if (memcmp(hdr, "FRAM", 4) == 0 && len >= 8)
			{
				uint32_t w, h; memcpy(&w, &payload[0], 4); memcpy(&h, &payload[4], 4);
				if (w == 0 || h == 0 || w > 8192 || h > 8192 || len != 8 + w * h * 4) { LOG_WARN("Linux helper sent a bad picture"); break; }
				std::lock_guard<std::mutex> lock(g_net_mutex);
				g_net_ready.assign(payload.begin() + 8, payload.end());
				g_net_w = w; g_net_h = h; g_net_new = true;
			}
			else if (memcmp(hdr, "CPOS", 4) == 0 && len == 12)
			{
				int32_t v[3]; memcpy(v, payload.data(), 12);
				std::lock_guard<std::mutex> lock(g_net_mutex);
				g_net_cpos = { v[0], v[1] }; g_net_cvis = v[2] != 0; g_net_cpos_new = true;
			}
			else if (memcmp(hdr, "CSHP", 4) == 0 && len >= 16)
			{
				uint32_t w, h; int32_t hx, hy;
				memcpy(&w, &payload[0], 4); memcpy(&h, &payload[4], 4); memcpy(&hx, &payload[8], 4); memcpy(&hy, &payload[12], 4);
				if (w > PTR_MAX || h > PTR_MAX || len != 16 + w * h * 4) continue;
				std::lock_guard<std::mutex> lock(g_net_mutex);
				g_net_shape.assign(payload.begin() + 16, payload.end());
				g_net_sw = w; g_net_sh = h; g_net_hot = { hx, hy }; g_net_shape_new = true;
			}
			else if (memcmp(hdr, "TEXT", 4) == 0)
				linux_text(std::string(payload.begin(), payload.end()));
		}
		{ std::lock_guard<std::mutex> lock(g_net_mutex); g_net_socket = INVALID_SOCKET; }
		closesocket(s);
		if (!g_net_stop) { LOG_WARN("Linux helper disconnected (retrying)"); Sleep(1000); }
	}
}

static void linux_stop()
{
	g_net_stop = true;
	{
		std::lock_guard<std::mutex> lock(g_net_mutex);
		if (g_net_socket != INVALID_SOCKET) shutdown(g_net_socket, SD_BOTH);
	}
	if (g_net_thread.joinable()) g_net_thread.detach(); // ends at its next timeout; may run inside DllMain
}

// Present thread: what Desktop Duplication + capture() do on Windows
static void linux_present(ID3D11Device *d3d)
{
	if (!g_net_thread.joinable() && !g_net_stop)
		g_net_thread = std::thread(net_thread);
	const int need = (g_target_count.load() > 0 || g_pcmode) ? 1 : 0; // outside Jodi's office: no capture at all
	if (need != g_net_active_sent && linux_send(need ? "active 1" : "active 0"))
		g_net_active_sent = need;
	if (!need)
		return;

	bool new_frame = false, cursor_moved = false;
	UINT w = 0, h = 0;
	{
		std::lock_guard<std::mutex> lock(g_net_mutex);
		if (g_net_new) { g_net_new = false; g_net_front.swap(g_net_ready); w = g_net_w; h = g_net_h; new_frame = true; }
		if (g_net_shape_new)
		{
			g_net_shape_new = false;
			g_ptr_shape = g_net_shape;
			g_ptr_info = {};
			g_ptr_info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
			g_ptr_info.Width = g_net_sw; g_ptr_info.Height = g_net_sh; g_ptr_info.Pitch = g_net_sw * 4;
			g_ptr_info.HotSpot = g_net_hot;
			g_sprite_dirty = true;
			cursor_moved = true;
		}
		if (g_net_cpos_new)
		{
			g_net_cpos_new = false;
			g_linux_cursor = g_net_cpos;
			g_ptr_visible = g_net_cvis;
			cursor_moved = true;
		}
	}
	// Desktop Duplication gives the top-left corner of the cursor picture, the helper the hot spot
	g_ptr_pos = { g_linux_cursor.x - g_ptr_info.HotSpot.x, g_linux_cursor.y - g_ptr_info.HotSpot.y };

	if (new_frame && (g_desktop_tex == nullptr || w != g_tex_w || h != g_tex_h))
	{
		release_capture();
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w; td.Height = h;
		td.MipLevels = 1; td.ArraySize = 1;
		td.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if (!create_textures(d3d, td)) { LOG_WARN("could not create the desktop texture"); release_capture(); return; }
		g_mon_rect = { 0, 0, (LONG)w, (LONG)h };
		g_capturing = true;
		LOG_INFO("receiving %ux%u from the Linux helper", w, h);
	}
	if (g_desktop_tex == nullptr || !(new_frame || (g_pcmode && cursor_moved)))
		return;
	ID3D11DeviceContext *ctx = nullptr;
	d3d->GetImmediateContext(&ctx);
	ctx->UpdateSubresource(g_desktop_tex, 0, nullptr, g_net_front.data(), g_tex_w * 4, 0);
	draw_cursor(ctx);
	update_pc_copy(ctx);
	run_tv_pass(ctx);
	ctx->Release();
}

// PC mode: the helper moves the real pointer, hands the keyboard over and keeps the pointer on the monitor
static void linux_pc_on(float u, float v)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "pc_on %.4f %.4f", u, v);
	{ std::lock_guard<std::mutex> lock(g_net_mutex); g_net_lost.clear(); }
	if (!linux_send(buf))
		{ std::lock_guard<std::mutex> lock(g_net_mutex); g_net_lost = "the Linux helper is not running"; }
}

static void linux_pc_off(bool back_to_game)
{
	if (!back_to_game) { linux_send("pc_off release"); return; }
	POINT target = g_saved_cursor;
	if (g_mapped_entry && g_tex_w > 1 && g_tex_h > 1) // game cursor goes where the desktop cursor is now
	{
		const float u = g_linux_cursor.x / float(g_tex_w - 1), v = g_linux_cursor.y / float(g_tex_h - 1);
		target = screen_to_game_cursor(std::max(0.0f, std::min(1.0f, u)), std::max(0.0f, std::min(1.0f, v)));
	}
	RECT cr = {};
	GetClientRect(g_game_hwnd, &cr);
	ScreenToClient(g_game_hwnd, &target);
	char buf[96];
	snprintf(buf, sizeof(buf), "pc_off game %.4f %.4f", target.x / float(std::max<LONG>(1, cr.right)), target.y / float(std::max<LONG>(1, cr.bottom)));
	linux_send(buf);
}

// Called every ~30 ms on the background thread while PC mode is on (instead of the ClipCursor checks)
static void linux_watch_pcmode()
{
	static int lost = 0;
	std::string why;
	{ std::lock_guard<std::mutex> lock(g_net_mutex); why.swap(g_net_lost); }
	if (!why.empty()) { lost = 0; leave_pcmode(false, why.c_str()); return; }
	const bool gone = IsIconic(g_game_hwnd) || !IsWindowVisible(g_game_hwnd);
	if (gone && g_keepfront && lost == 0)
		ShowWindowAsync(g_game_hwnd, SW_SHOWNOACTIVATE); // try to bring it back once
	lost = gone ? lost + 1 : 0;
	if (lost >= 10) { lost = 0; leave_pcmode(false, "the game left the screen - cursor released"); }
}

// Sound: the add-on decides, the helper applies it to the Linux programs on the shown monitor
static void linux_sound(bool restore_all, bool want, float gl, float gr)
{
	if (restore_all)
	{
		if (g_net_restored == nullptr) g_net_restored = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		ResetEvent(g_net_restored);
		if (linux_send("sound_restore")) WaitForSingleObject(g_net_restored, 1200);
		return;
	}
	char buf[64];
	snprintf(buf, sizeof(buf), "sound %d %.3f %.3f", want ? 1 : 0, gl, gr);
	linux_send(buf);
}
