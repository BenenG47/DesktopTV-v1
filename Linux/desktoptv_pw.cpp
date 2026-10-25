// desktoptv_pw - the picture half of the DesktopTV Linux helper.
//
// Started by desktoptv_capture.py with the PipeWire remote of a desktop-portal screen cast. It reads the
// stream (picture + mouse cursor as separate metadata, like Desktop Duplication on Windows) and serves
// it to the DesktopTV add-on in the game over TCP on 127.0.0.1:<port>.
//
// Messages, both directions: 4-byte type, uint32 payload length (little endian), payload.
//   to the add-on:   FRAM  u32 w, u32 h, w*h*4 bytes BGRA (rows top to bottom, alpha 255)
//                    CPOS  i32 x, i32 y, u32 visible        (cursor hot spot in FRAM pixels)
//                    CSHP  u32 w, u32 h, i32 hx, i32 hy, w*h*4 bytes BGRA (straight alpha, FRAM scale)
//                    TEXT  one line of UTF-8 (control messages from desktoptv_capture.py)
//   from the add-on: TEXT  one line of UTF-8
//
// stdin  (from desktoptv_capture.py): "send <text>" = TEXT to the add-on, "quit"
// stdout (to desktoptv_capture.py):   "addon <text>", "client 0|1", "cursor <x> <y> <visible>" (stream
//                                     pixels), "stream <w> <h>", "ended"
// The add-on's "fps <n>" and "active <0|1>" are also handled here: no capture while it is not needed.

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/param/buffers.h>
#include <spa/param/param.h>
#include <spa/buffer/meta.h>
#include <spa/debug/types.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <csignal>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static const int CURSOR_MAX = 256;
#define CURSOR_META_SIZE(w, h) (sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) + (w) * (h) * 4)

static int g_out_w = 0, g_out_h = 0; // --size; 0 = the stream's own size
static std::atomic<int> g_fps { 30 };
static std::atomic<bool> g_active { false };
static std::atomic<bool> g_quit { false };

static std::mutex g_stdout_mutex;
static void out_line(const std::string &s)
{
	std::lock_guard<std::mutex> lock(g_stdout_mutex);
	fputs(s.c_str(), stdout);
	fputc('\n', stdout);
	fflush(stdout);
}

// ---------------------------------------------------------------- shared state (PipeWire thread -> sender)

struct shared_state
{
	std::mutex m;
	std::condition_variable cv;
	std::vector<uint8_t> frame; // BGRA, out size
	uint32_t fw = 0, fh = 0;
	uint64_t frame_seq = 0;
	int cx = 0, cy = 0; bool cvis = false; // cursor, FRAM pixels
	uint64_t cpos_seq = 0;
	std::vector<uint8_t> shape; uint32_t sw = 0, sh = 0; int shx = 0, shy = 0;
	uint64_t shape_seq = 0;
	std::deque<std::string> texts;
	bool client_changed = false;
} g_st;

// ---------------------------------------------------------------- PipeWire

struct pw_data
{
	pw_thread_loop *loop = nullptr;
	pw_context *context = nullptr;
	pw_core *core = nullptr;
	pw_stream *stream = nullptr;
	spa_hook stream_listener = {};
	spa_video_info_raw info = {};
	bool swap_rb = false;
	int last_cx = -100000, last_cy = -100000; bool last_vis = false;
} g_pw;

static void put_pixels(const uint8_t *src, uint32_t sw, uint32_t sh, uint32_t stride, bool swap_rb, std::vector<uint8_t> &dst, uint32_t dw, uint32_t dh)
{
	dst.resize((size_t)dw * dh * 4);
	if (dw == sw && dh == sh)
	{
		for (uint32_t y = 0; y < sh; ++y)
		{
			const uint8_t *s = src + (size_t)y * stride;
			uint8_t *d = &dst[(size_t)y * dw * 4];
			if (!swap_rb)
			{
				memcpy(d, s, (size_t)sw * 4);
				for (uint32_t x = 0; x < sw; ++x) d[x * 4 + 3] = 255;
			}
			else
			{
				for (uint32_t x = 0; x < sw; ++x) { d[x * 4] = s[x * 4 + 2]; d[x * 4 + 1] = s[x * 4 + 1]; d[x * 4 + 2] = s[x * 4]; d[x * 4 + 3] = 255; }
			}
		}
		return;
	}
	// Smaller picture requested: bilinear
	const float kx = float(sw) / dw, ky = float(sh) / dh;
	for (uint32_t y = 0; y < dh; ++y)
	{
		const float fy = std::max(0.0f, (y + 0.5f) * ky - 0.5f);
		const uint32_t y0 = std::min<uint32_t>((uint32_t)fy, sh - 1), y1 = std::min<uint32_t>(y0 + 1, sh - 1);
		const float wy = fy - y0;
		const uint8_t *r0 = src + (size_t)y0 * stride, *r1 = src + (size_t)y1 * stride;
		uint8_t *d = &dst[(size_t)y * dw * 4];
		for (uint32_t x = 0; x < dw; ++x)
		{
			const float fx = std::max(0.0f, (x + 0.5f) * kx - 0.5f);
			const uint32_t x0 = std::min<uint32_t>((uint32_t)fx, sw - 1), x1 = std::min<uint32_t>(x0 + 1, sw - 1);
			const float wx = fx - x0;
			for (int c = 0; c < 3; ++c)
			{
				const float a = r0[x0 * 4 + c] * (1 - wx) + r0[x1 * 4 + c] * wx;
				const float b = r1[x0 * 4 + c] * (1 - wx) + r1[x1 * 4 + c] * wx;
				d[x * 4 + (swap_rb ? 2 - c : c)] = (uint8_t)(a * (1 - wy) + b * wy + 0.5f);
			}
			d[x * 4 + 3] = 255;
		}
	}
}

static void on_process(void *)
{
	pw_buffer *b = pw_stream_dequeue_buffer(g_pw.stream);
	if (b == nullptr)
		return;
	spa_buffer *buf = b->buffer;
	const uint32_t sw = g_pw.info.size.width, sh = g_pw.info.size.height;
	if (sw == 0 || sh == 0) { pw_stream_queue_buffer(g_pw.stream, b); return; }
	const uint32_t dw = g_out_w ? (uint32_t)g_out_w : sw, dh = g_out_h ? (uint32_t)g_out_h : sh;
	const float scale_x = float(dw) / sw, scale_y = float(dh) / sh;

	// Cursor (sent as its own metadata, also in buffers without a new picture)
	auto *mc = static_cast<spa_meta_cursor *>(spa_buffer_find_meta_data(buf, SPA_META_Cursor, sizeof(spa_meta_cursor)));
	if (mc && spa_meta_cursor_is_valid(mc))
	{
		const int px = mc->position.x, py = mc->position.y;
		const bool vis = px > -CURSOR_MAX && py > -CURSOR_MAX && px < (int)sw + CURSOR_MAX && py < (int)sh + CURSOR_MAX;
		std::vector<uint8_t> shape; uint32_t cw = 0, ch = 0; int hx = 0, hy = 0; bool new_shape = false;
		if (mc->bitmap_offset >= sizeof(spa_meta_cursor))
		{
			auto *bm = SPA_PTROFF(mc, mc->bitmap_offset, spa_meta_bitmap);
			if (bm->size.width > 0 && bm->size.height > 0 && bm->size.width <= (uint32_t)CURSOR_MAX && bm->size.height <= (uint32_t)CURSOR_MAX)
			{
				const uint8_t *bits = SPA_PTROFF(bm, bm->offset, uint8_t);
				const bool rgba = bm->format == SPA_VIDEO_FORMAT_RGBA || bm->format == SPA_VIDEO_FORMAT_RGBx;
				// at the picture's scale (nearest neighbour; the add-on scales it again by CursorScale)
				cw = std::max(1u, (uint32_t)(bm->size.width * scale_x + 0.5f)); ch = std::max(1u, (uint32_t)(bm->size.height * scale_y + 0.5f));
				shape.resize((size_t)cw * ch * 4);
				for (uint32_t y = 0; y < ch; ++y)
					for (uint32_t x = 0; x < cw; ++x)
					{
						const uint32_t sx = std::min(bm->size.width - 1, (uint32_t)(x / scale_x)), sy = std::min(bm->size.height - 1, (uint32_t)(y / scale_y));
						const uint8_t *s = bits + (size_t)sy * bm->stride + sx * 4;
						uint8_t *d = &shape[((size_t)y * cw + x) * 4];
						d[0] = rgba ? s[2] : s[0]; d[1] = s[1]; d[2] = rgba ? s[0] : s[2];
						d[3] = (bm->format == SPA_VIDEO_FORMAT_RGBx || bm->format == SPA_VIDEO_FORMAT_BGRx) ? 255 : s[3];
					}
				// KWin hands over premultiplied alpha: the add-on blends straight alpha
				for (size_t i = 0; i < shape.size(); i += 4)
					if (shape[i + 3] != 0 && shape[i + 3] != 255)
						for (int c = 0; c < 3; ++c) shape[i + c] = (uint8_t)std::min(255, shape[i + c] * 255 / shape[i + 3]);
				hx = (int)(mc->hotspot.x * scale_x + 0.5f); hy = (int)(mc->hotspot.y * scale_y + 0.5f);
				new_shape = true;
			}
		}
		const int cx = (int)(px * scale_x), cy = (int)(py * scale_y);
		std::lock_guard<std::mutex> lock(g_st.m);
		if (new_shape) { g_st.shape.swap(shape); g_st.sw = cw; g_st.sh = ch; g_st.shx = hx; g_st.shy = hy; ++g_st.shape_seq; }
		if (cx != g_st.cx || cy != g_st.cy || vis != g_st.cvis || new_shape) { g_st.cx = cx; g_st.cy = cy; g_st.cvis = vis; ++g_st.cpos_seq; }
		g_st.cv.notify_all();
		if (px != g_pw.last_cx || py != g_pw.last_cy || vis != g_pw.last_vis)
		{
			g_pw.last_cx = px; g_pw.last_cy = py; g_pw.last_vis = vis;
			out_line("cursor " + std::to_string(px) + " " + std::to_string(py) + " " + (vis ? "1" : "0"));
		}
	}

	// Picture
	spa_data &d = buf->datas[0];
	auto *hdr = static_cast<spa_meta_header *>(spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(spa_meta_header)));
	const bool corrupted = (hdr && (hdr->flags & SPA_META_HEADER_FLAG_CORRUPTED)) || (d.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED);
	if (g_active && !corrupted && d.data != nullptr && d.chunk->size != 0)
	{
		const uint32_t stride = d.chunk->stride ? (uint32_t)d.chunk->stride : sw * 4;
		if ((uint64_t)d.chunk->offset + (uint64_t)stride * (sh - 1) + sw * 4 <= d.maxsize)
		{
			static std::vector<uint8_t> back; // filled outside the lock, then swapped in (the old picture is reused)
			put_pixels(static_cast<const uint8_t *>(d.data) + d.chunk->offset, sw, sh, stride, g_pw.swap_rb, back, dw, dh);
			std::lock_guard<std::mutex> lock(g_st.m);
			g_st.frame.swap(back);
			g_st.fw = dw; g_st.fh = dh;
			++g_st.frame_seq;
			g_st.cv.notify_all();
		}
	}
	pw_stream_queue_buffer(g_pw.stream, b);
}

static void on_param_changed(void *, uint32_t id, const spa_pod *param)
{
	if (param == nullptr || id != SPA_PARAM_Format)
		return;
	uint32_t media_type, media_subtype;
	if (spa_format_parse(param, &media_type, &media_subtype) < 0 || media_type != SPA_MEDIA_TYPE_video || media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return;
	spa_format_video_raw_parse(param, &g_pw.info);
	g_pw.swap_rb = g_pw.info.format == SPA_VIDEO_FORMAT_RGBx || g_pw.info.format == SPA_VIDEO_FORMAT_RGBA;
	fprintf(stderr, "DesktopTV: stream format %s %ux%u\n", spa_debug_type_find_name(spa_type_video_format, g_pw.info.format),
		g_pw.info.size.width, g_pw.info.size.height);
	out_line("stream " + std::to_string(g_pw.info.size.width) + " " + std::to_string(g_pw.info.size.height));

	uint8_t pbuf[1024];
	spa_pod_builder b = SPA_POD_BUILDER_INIT(pbuf, sizeof(pbuf));
	const spa_pod *params[3];
	params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 16),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))));
	params[1] = static_cast<const spa_pod *>(spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header))));
	params[2] = static_cast<const spa_pod *>(spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
		SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(CURSOR_META_SIZE(64, 64), CURSOR_META_SIZE(1, 1), CURSOR_META_SIZE(CURSOR_MAX, CURSOR_MAX))));
	pw_stream_update_params(g_pw.stream, params, 3);
}

static void on_state_changed(void *, pw_stream_state, pw_stream_state state, const char *error)
{
	fprintf(stderr, "DesktopTV: stream %s%s%s\n", pw_stream_state_as_string(state), error ? ": " : "", error ? error : "");
	if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED)
	{
		out_line("ended");
		g_quit = true;
	}
}

static const pw_stream_events g_stream_events = [] {
	pw_stream_events e = {};
	e.version = PW_VERSION_STREAM_EVENTS;
	e.state_changed = on_state_changed;
	e.param_changed = on_param_changed;
	e.process = on_process;
	return e;
}();

static bool start_pipewire(int fd, uint32_t node)
{
	pw_init(nullptr, nullptr);
	g_pw.loop = pw_thread_loop_new("desktoptv", nullptr);
	g_pw.context = pw_context_new(pw_thread_loop_get_loop(g_pw.loop), nullptr, 0);
	if (pw_thread_loop_start(g_pw.loop) < 0) return false;
	pw_thread_loop_lock(g_pw.loop);
	g_pw.core = pw_context_connect_fd(g_pw.context, fd, nullptr, 0);
	if (g_pw.core == nullptr) { pw_thread_loop_unlock(g_pw.loop); fprintf(stderr, "DesktopTV: cannot connect to PipeWire\n"); return false; }
	g_pw.stream = pw_stream_new(g_pw.core, "DesktopTV", pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr));
	pw_stream_add_listener(g_pw.stream, &g_pw.stream_listener, &g_stream_events, nullptr);

	uint8_t pbuf[1024];
	spa_pod_builder b = SPA_POD_BUILDER_INIT(pbuf, sizeof(pbuf));
	const spa_rectangle def_size = SPA_RECTANGLE(1920, 1080), min_size = SPA_RECTANGLE(1, 1), max_size = SPA_RECTANGLE(8192, 8192);
	const spa_fraction def_rate = SPA_FRACTION(60, 1), min_rate = SPA_FRACTION(0, 1), max_rate = SPA_FRACTION(1000, 1);
	const spa_pod *params[1];
	params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
		SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&def_size, &min_size, &max_size),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&def_rate, &min_rate, &max_rate)));
	const int r = pw_stream_connect(g_pw.stream, PW_DIRECTION_INPUT, node,
		static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
	pw_thread_loop_unlock(g_pw.loop);
	if (r < 0) { fprintf(stderr, "DesktopTV: cannot connect the stream (%d)\n", r); return false; }
	return true;
}

static void set_stream_active(bool on)
{
	pw_thread_loop_lock(g_pw.loop);
	pw_stream_set_active(g_pw.stream, on);
	pw_thread_loop_unlock(g_pw.loop);
}

// ---------------------------------------------------------------- TCP

static bool send_all(int s, const void *p, size_t n)
{
	const uint8_t *c = static_cast<const uint8_t *>(p);
	while (n > 0)
	{
		const ssize_t r = send(s, c, n, MSG_NOSIGNAL);
		if (r <= 0) return false;
		c += r; n -= (size_t)r;
	}
	return true;
}

static bool send_msg(int s, const char type[4], const void *a, size_t na, const void *b = nullptr, size_t nb = 0)
{
	uint8_t hdr[8];
	memcpy(hdr, type, 4);
	const uint32_t len = (uint32_t)(na + nb);
	memcpy(hdr + 4, &len, 4);
	return send_all(s, hdr, 8) && (na == 0 || send_all(s, a, na)) && (nb == 0 || send_all(s, b, nb));
}

static bool recv_all(int s, void *p, size_t n)
{
	uint8_t *c = static_cast<uint8_t *>(p);
	while (n > 0)
	{
		const ssize_t r = recv(s, c, n, 0);
		if (r <= 0) return false;
		c += r; n -= (size_t)r;
	}
	return true;
}

static void handle_addon_text(const std::string &t)
{
	if (t.rfind("fps ", 0) == 0) { const int f = atoi(t.c_str() + 4); if (f > 0) g_fps = f; }
	else if (t == "active 1" && !g_active.exchange(true)) set_stream_active(true);
	else if (t == "active 0" && g_active.exchange(false)) set_stream_active(false);
	out_line("addon " + t);
}

static void reader(int s)
{
	for (;;)
	{
		uint8_t hdr[8];
		if (!recv_all(s, hdr, 8)) break;
		uint32_t len; memcpy(&len, hdr + 4, 4);
		if (len > 1 << 20) break;
		std::string payload(len, '\0');
		if (len && !recv_all(s, &payload[0], len)) break;
		if (memcmp(hdr, "TEXT", 4) == 0) handle_addon_text(payload);
	}
	shutdown(s, SHUT_RDWR);
}

static void serve_client(int s)
{
	out_line("client 1");
	std::thread rd(reader, s);
	uint64_t frame_seq = 0, cpos_seq = 0, shape_seq = 0;
	auto last_frame = std::chrono::steady_clock::now() - std::chrono::seconds(1);
	std::vector<uint8_t> frame, shape;
	bool ok = true;
	while (ok && !g_quit)
	{
		std::unique_lock<std::mutex> lock(g_st.m);
		const auto gap = std::chrono::milliseconds(1000 / std::max(1, g_fps.load()));
		g_st.cv.wait_for(lock, std::chrono::milliseconds(100));
		if (g_st.client_changed) { g_st.client_changed = false; break; } // a newer add-on connection took over
		std::deque<std::string> texts; texts.swap(g_st.texts);
		const bool want_frame = g_st.frame_seq != frame_seq && std::chrono::steady_clock::now() - last_frame >= gap;
		uint32_t fw = 0, fh = 0;
		if (want_frame) { frame = g_st.frame; fw = g_st.fw; fh = g_st.fh; frame_seq = g_st.frame_seq; }
		const bool want_shape = g_st.shape_seq != shape_seq;
		uint32_t shp[4] = {};
		if (want_shape) { shape = g_st.shape; shp[0] = g_st.sw; shp[1] = g_st.sh; memcpy(&shp[2], &g_st.shx, 4); memcpy(&shp[3], &g_st.shy, 4); shape_seq = g_st.shape_seq; }
		const bool want_pos = g_st.cpos_seq != cpos_seq;
		int32_t pos[3] = { g_st.cx, g_st.cy, g_st.cvis ? 1 : 0 };
		cpos_seq = g_st.cpos_seq;
		lock.unlock();

		for (const auto &t : texts) ok = ok && send_msg(s, "TEXT", t.data(), t.size());
		if (!g_active) continue;
		if (want_shape && ok) ok = send_msg(s, "CSHP", shp, sizeof(shp), shape.data(), shape.size());
		if (want_pos && ok) ok = send_msg(s, "CPOS", pos, sizeof(pos));
		if (want_frame && ok)
		{
			const uint32_t wh[2] = { fw, fh };
			ok = send_msg(s, "FRAM", wh, sizeof(wh), frame.data(), frame.size());
			last_frame = std::chrono::steady_clock::now();
		}
	}
	shutdown(s, SHUT_RDWR);
	rd.join();
	close(s);
	if (g_active.exchange(false)) set_stream_active(false);
	out_line("client 0");
}

static void server(int port)
{
	const int srv = socket(AF_INET, SOCK_STREAM, 0);
	const int one = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(srv, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 || listen(srv, 4) != 0)
	{
		fprintf(stderr, "DesktopTV: port %d is in use - is the helper already running?\n", port);
		out_line("ended");
		g_quit = true;
		return;
	}
	fprintf(stderr, "DesktopTV: waiting for the add-on on 127.0.0.1:%d\n", port);
	std::thread current;
	while (!g_quit)
	{
		pollfd p = { srv, POLLIN, 0 };
		if (poll(&p, 1, 200) <= 0) continue;
		const int c = accept(srv, nullptr, nullptr);
		if (c < 0) continue;
		setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		if (current.joinable())
		{
			{ std::lock_guard<std::mutex> lock(g_st.m); g_st.client_changed = true; g_st.cv.notify_all(); }
			current.join();
		}
		{ std::lock_guard<std::mutex> lock(g_st.m); g_st.client_changed = false; g_st.texts.clear(); }
		current = std::thread(serve_client, c);
	}
	if (current.joinable()) { { std::lock_guard<std::mutex> lock(g_st.m); g_st.client_changed = true; g_st.cv.notify_all(); } current.join(); }
	close(srv);
}

int main(int argc, char **argv)
{
	int fd = -1, port = 47474;
	uint32_t node = 0;
	for (int i = 1; i + 1 < argc; i += 2)
	{
		const std::string k = argv[i], v = argv[i + 1];
		if (k == "--fd") fd = atoi(v.c_str());
		else if (k == "--node") node = (uint32_t)strtoul(v.c_str(), nullptr, 10);
		else if (k == "--port") port = atoi(v.c_str());
		else if (k == "--fps") g_fps = std::max(1, atoi(v.c_str()));
		else if (k == "--size") sscanf(v.c_str(), "%dx%d", &g_out_w, &g_out_h);
	}
	if (fd < 0 || node == 0) { fprintf(stderr, "usage: desktoptv_pw --fd N --node N [--port P] [--fps F] [--size WxH]\n"); return 2; }
	signal(SIGPIPE, SIG_IGN);
	if (!start_pipewire(fd, node)) return 1;
	set_stream_active(false); // until the add-on asks for pictures
	std::thread net(server, port);
	std::thread in([] {
		std::string line;
		while (std::getline(std::cin, line))
		{
			if (line == "quit") break;
			if (line.rfind("send ", 0) == 0)
			{
				std::lock_guard<std::mutex> lock(g_st.m);
				g_st.texts.push_back(line.substr(5));
				g_st.cv.notify_all();
			}
		}
		g_quit = true;
		g_st.cv.notify_all();
	});
	in.detach();
	while (!g_quit) std::this_thread::sleep_for(std::chrono::milliseconds(100));
	net.join();
	pw_thread_loop_stop(g_pw.loop);
	return 0;
}
