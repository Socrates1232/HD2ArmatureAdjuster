#include <reshade.hpp>

#include "matrix_scan.hpp"
#include "fingerprint_data.hpp"
#include "wc_read.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace reshade;
using namespace reshade::api;

namespace
{
constexpr uint64_t k_min_buffer_bytes = 8 * 32;
constexpr uint64_t k_scan_bytes = 64 * 1024;
constexpr uint64_t k_scan_overlap = 8 * 1024;
constexpr uint32_t k_min_slots = 8;
constexpr uint32_t k_max_slots = 256;
constexpr size_t k_max_candidates = 64;
constexpr size_t k_max_ring_targets = 1024;
constexpr size_t k_max_active_targets = 64;
constexpr uint64_t k_ring_scan_interval = 4;
constexpr float k_motion_epsilon = 1.0e-5f;
constexpr float k_edit_translation = 1.5f;
constexpr bool k_enable_anonymous_scan = false;
constexpr uint32_t k_marker_repeats = 64;
constexpr uint32_t k_marker_palette_slots = 152;
constexpr uint32_t k_marker_stride = 48;
constexpr uint64_t k_marker_scan_bytes = 1024 * 1024;
constexpr uint64_t k_marker_scan_overlap = (k_marker_repeats + 2) * k_marker_stride;
constexpr uint64_t k_marker_palette_bytes =
	static_cast<uint64_t>(k_marker_palette_slots - 1) * k_marker_stride;
constexpr uint32_t k_focus_index_count_a = 80586;
constexpr uint32_t k_focus_index_count_b = 23526;
constexpr size_t k_max_residency_probes = 8;
constexpr size_t k_ib_matrix_bytes = 64;
constexpr size_t k_ib_fingerprint_bytes = armature_fingerprint::table_prefix_bytes;
constexpr size_t k_ib_scan_chunk_bytes = 4 * 1024 * 1024;
constexpr size_t k_max_ib_hits = 32;

struct buffer_info
{
	uint64_t size = 0;
	memory_heap heap = memory_heap::unknown;
	void *map_ptr = nullptr;
	uint64_t map_offset = 0;
	uint64_t map_size = 0;
	uint64_t scan_offset = 0;
};

struct candidate
{
	uint32_t id = 0;
	uint64_t resource = 0;
	uint64_t offset = 0;
	uint32_t stride = 0;
	uint32_t slots = 0;
	uint32_t detected_slots = 0;
	bool alternate_layout = false;
	float score = 0.0f;
	uint32_t changed_slots = 0;
	uint64_t samples = 0;
	uint64_t motion_frames = 0;
	uint64_t last_seen_frame = 0;
	uint64_t last_change_frame = 0;
	bool fresh = true;
	std::vector<uint8_t> bytes;
	std::vector<float> delta;
	std::vector<float> peak_delta;
};

struct ring_target
{
	uint64_t resource = 0;
	uint64_t offset = 0;
	uint32_t slots = 0;
	bool alternate_layout = false;
	uint32_t observations = 0;
	uint32_t changes = 0;
	uint64_t last_seen_frame = 0;
	uint64_t last_change_frame = 0;
	std::array<uint8_t, 48 * 7> sample {};
};

struct active_edit_target
{
	uintptr_t address = 0;
	uint64_t resource = 0;
	uint64_t offset = 0;
	uint64_t palette_end = 0;
	uint32_t slots = 0;
	bool alternate_layout = false;
	bool wrote = false;
	std::vector<uint8_t> original;
	std::vector<uint8_t> injected;
};

struct residency_probe
{
	uint64_t resource = 0;
	uint64_t palette_end = 0;
	std::array<uint8_t, k_marker_stride> original {};
	std::array<uint8_t, k_marker_stride> poisoned {};
};

struct scan_sample
{
	uint64_t resource = 0;
	uint64_t offset = 0;
	uint64_t frame = 0;
	std::vector<uint8_t> bytes;
};

struct edit_probe_state
{
	bool requested = false;
	bool active = false;
	bool completed = false;
	bool target_had_motion = false;
	uint32_t candidate_id = 0;
	uint32_t slot = 0;
	uint32_t slots_modified = 0;
	uint32_t stride = 0;
	uint64_t resource = 0;
	uint64_t offset = 0;
	uint64_t write_attempts = 0;
	uint64_t write_successes = 0;
	uint64_t immediate_readbacks = 0;
	uint64_t present_readbacks = 0;
	uint64_t stale_skips = 0;
	uint32_t targets_selected = 0;
	uint32_t targets_written = 0;
	uint32_t targets_restored = 0;
	uint32_t round = 0;
	uint32_t total_rounds = 0;
	bool restore_attempted = false;
	bool restore_succeeded = false;
	bool overwrite_observed = false;
	uint32_t error = 0;
};

std::mutex g_mutex;
std::unordered_map<uint64_t, buffer_info> g_buffers;
std::vector<uint64_t> g_buffer_order;
std::vector<candidate> g_candidates;
std::vector<ring_target> g_ring_targets;
std::vector<active_edit_target> g_active_edit_targets;
std::vector<residency_probe> g_residency_probes;
size_t g_residency_cursor = 0;
uint64_t g_residency_restores = 0;
std::vector<scan_sample> g_scan_queue;
device *g_device = nullptr;
size_t g_buffer_cursor = 0;
size_t g_candidate_cursor = 0;
size_t g_selected = 0;
uint32_t g_slot_page = 0;
uint32_t g_next_candidate_id = 1;
uint64_t g_frame = 0;
uint64_t g_scanned_bytes = 0;
bool g_paused = false;
HANDLE g_console = INVALID_HANDLE_VALUE;
HWND g_game_window = nullptr;
std::chrono::steady_clock::time_point g_last_display;
std::chrono::steady_clock::time_point g_last_telemetry;
std::atomic<uint32_t> g_draws { 0 };
std::atomic<uint32_t> g_last_draws { 0 };
std::wstring g_telemetry_path;
std::wstring g_automation_path;
std::wstring g_capture_directory;
uint64_t g_session_id = 0;
uint32_t g_automation_stage = 0;
uint32_t g_automation_delay_ms = 20000;
uint32_t g_capture_count = 0;
uint32_t g_intro_attempts = 0;
uint32_t g_automation_error = 0;
bool g_skip_intro = false;
bool g_capture_enabled = false;
bool g_steam_capture_enabled = false;
bool g_steam_screenshot_key_down = false;
std::chrono::steady_clock::time_point g_steam_screenshot_release;
bool g_automation_input_started = false;
bool g_automation_completed = false;
bool g_focus_click_attempted = false;
bool g_w_held = false;
bool g_d_held = false;
bool g_b_held = false;
std::chrono::steady_clock::time_point g_automation_deadline;
edit_probe_state g_edit;
std::atomic<bool> g_edit_active { false };
bool g_paused_before_edit = false;
std::atomic<bool> g_scan_thread_stop { false };
HANDLE g_scan_thread = nullptr;
std::vector<uintptr_t> g_ib_hits;
std::atomic<uint32_t> g_ib_hunt_phase { 0 }; // 0 idle, 1 scanning, 2 found, 3 exhausted
std::atomic<uint64_t> g_ib_hunt_bytes { 0 };
std::atomic<uint32_t> g_ib_hunt_passes { 0 };
std::atomic<bool> g_ib_hunt_stop { false };
HANDLE g_ib_hunt_thread = nullptr;
std::chrono::steady_clock::time_point g_ib_hunt_deadline;
uint64_t g_live_resource = 0;
uint64_t g_live_end = 0;
double g_live_stride = static_cast<double>(k_marker_palette_bytes);
uint64_t g_live_frame = 0;
uint32_t g_live_misses = 0;
uint64_t g_focus_draws = 0;
uint64_t g_live_locations = 0;

void translation_of(const candidate &c, uint32_t slot, float &x, float &y, float &z);
bool begin_edit_test(effect_runtime *runtime, std::chrono::steady_clock::time_point now);
void end_edit_pulse();
void write_edit_probe();
bool ensure_scan_thread();
bool ensure_ib_hunt_thread();
void locate_and_write_live_palette(uint32_t index_count);
void probe_and_write_fresh_palettes();

bool has_reused_marker_target()
{
	std::lock_guard lock(g_mutex);
	return std::any_of(g_ring_targets.begin(), g_ring_targets.end(),
		[](const ring_target &target) { return target.changes != 0; });
}

bool writable_data_page(DWORD protect)
{
	if ((protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_WRITECOMBINE)) != 0)
		return false;
	switch (protect & 0xFF)
	{
	case PAGE_READWRITE:
	case PAGE_WRITECOPY:
		return true;
	default:
		return false;
	}
}

bool fingerprint_at(const uint8_t *data, size_t available)
{
	if (available < k_ib_fingerprint_bytes ||
		std::memcmp(data + armature_fingerprint::needle_offset,
			armature_fingerprint::needle.data(), armature_fingerprint::needle.size()) != 0)
		return false;
	uint64_t hash = 14695981039346656037ull;
	for (size_t i = 0; i < k_ib_fingerprint_bytes; ++i)
		hash = (hash ^ data[i]) * 1099511628211ull;
	return hash == armature_fingerprint::table_prefix_fnv1a &&
		armature_probe::transform_like(
			data + static_cast<size_t>(armature_fingerprint::target_slot) * k_ib_matrix_bytes,
			static_cast<uint32_t>(k_ib_matrix_bytes));
}

DWORD WINAPI ib_hunt_thread_proc(void *)
{
	std::vector<uint8_t> scratch(k_ib_scan_chunk_bytes + k_ib_fingerprint_bytes);
	SYSTEM_INFO system = {};
	GetSystemInfo(&system);
	const uintptr_t minimum = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
	const uintptr_t maximum = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);

	for (uint32_t pass = 0; pass < 40 && !g_ib_hunt_stop.load(std::memory_order_acquire); ++pass)
	{
		std::vector<uintptr_t> found;
		uintptr_t cursor = minimum;
		while (cursor < maximum && found.size() < k_max_ib_hits &&
			!g_ib_hunt_stop.load(std::memory_order_acquire))
		{
			MEMORY_BASIC_INFORMATION info = {};
			if (VirtualQuery(reinterpret_cast<const void *>(cursor), &info, sizeof(info)) == 0)
				break;
			const uintptr_t base = reinterpret_cast<uintptr_t>(info.BaseAddress);
			const size_t region_size = info.RegionSize;
			if (info.State == MEM_COMMIT && writable_data_page(info.Protect) && info.Type == MEM_PRIVATE)
			{
				for (size_t region_offset = 0; region_offset < region_size &&
					found.size() < k_max_ib_hits; region_offset += k_ib_scan_chunk_bytes)
				{
					const size_t wanted = std::min(scratch.size(), region_size - region_offset);
					SIZE_T got = 0;
					ReadProcessMemory(GetCurrentProcess(),
						reinterpret_cast<const void *>(base + region_offset), scratch.data(), wanted, &got);
					g_ib_hunt_bytes.fetch_add(got, std::memory_order_relaxed);
					if (got < k_ib_fingerprint_bytes)
						continue;
					const size_t scan_bytes = std::min(k_ib_scan_chunk_bytes, static_cast<size_t>(got));
					const size_t phase = (16 - ((base + region_offset) & 15)) & 15;
					for (size_t offset = phase; offset + k_ib_fingerprint_bytes <= got &&
						offset < scan_bytes && found.size() < k_max_ib_hits; offset += 16)
					{
						const uintptr_t address = base + region_offset + offset;
						const uintptr_t scratch_begin = reinterpret_cast<uintptr_t>(scratch.data());
						const uintptr_t scratch_end = scratch_begin + scratch.size();
						if ((address < scratch_end && address + k_ib_fingerprint_bytes > scratch_begin) ||
							!fingerprint_at(scratch.data() + offset, got - offset))
							continue;
						const bool duplicate = std::any_of(found.begin(), found.end(), [address](uintptr_t known) {
							return known == address;
						});
						if (!duplicate)
							found.push_back(address);
					}
				}
			}
			if (region_size == 0 || base > maximum - region_size)
				break;
			cursor = base + region_size;
		}
		g_ib_hunt_passes.fetch_add(1, std::memory_order_relaxed);
		if (!found.empty())
		{
			std::lock_guard lock(g_mutex);
			g_ib_hits = std::move(found);
			g_ib_hunt_phase.store(2, std::memory_order_release);
			return 0;
		}
		Sleep(500);
	}
	g_ib_hunt_phase.store(3, std::memory_order_release);
	return 0;
}

bool ensure_ib_hunt_thread()
{
	const uint32_t phase = g_ib_hunt_phase.load(std::memory_order_acquire);
	if (phase == 1 || phase == 2)
		return true;
	g_ib_hunt_bytes.store(0, std::memory_order_relaxed);
	g_ib_hunt_passes.store(0, std::memory_order_relaxed);
	{
		std::lock_guard lock(g_mutex);
		g_ib_hits.clear();
		g_ring_targets.clear();
		for (auto &[unused, info] : g_buffers)
			info.scan_offset = info.map_offset;
	}
	g_ib_hunt_phase.store(1, std::memory_order_release);
	g_ib_hunt_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	return true;
}

void initialize_telemetry()
{
	wchar_t base[32768] = {};
	constexpr DWORD capacity = static_cast<DWORD>(sizeof(base) / sizeof(base[0]));
	DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", base, capacity);
	if (length == 0 || length >= capacity)
		length = GetTempPathW(capacity, base);
	if (length == 0 || length >= capacity)
		return;
	std::wstring directory(base, length);
	if (!directory.empty() && directory.back() != L'\\')
		directory.push_back(L'\\');
	directory += L"HD2ArmatureAdjuster";
	CreateDirectoryW(directory.c_str(), nullptr);
	g_telemetry_path = directory + L"\\telemetry.json";
	g_automation_path = directory + L"\\automation.request";
	g_capture_directory = directory;
	g_session_id = (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^ GetTickCount64();
}

uint64_t unix_time_ms()
{
	FILETIME file_time = {};
	GetSystemTimeAsFileTime(&file_time);
	ULARGE_INTEGER ticks = {};
	ticks.LowPart = file_time.dwLowDateTime;
	ticks.HighPart = file_time.dwHighDateTime;
	return (ticks.QuadPart - 116444736000000000ull) / 10000ull;
}

bool game_has_focus()
{
	return g_game_window != nullptr && GetForegroundWindow() == g_game_window;
}

bool focus_runtime_window(effect_runtime *runtime)
{
	g_game_window = static_cast<HWND>(runtime->get_hwnd());
	if (g_game_window == nullptr || !IsWindow(g_game_window))
		return false;
	if (game_has_focus())
		return true;

	const HWND foreground = GetForegroundWindow();
	const DWORD current_thread = GetCurrentThreadId();
	const DWORD foreground_thread = foreground != nullptr ?
		GetWindowThreadProcessId(foreground, nullptr) : 0;
	const DWORD game_thread = GetWindowThreadProcessId(g_game_window, nullptr);
	const bool attached_foreground = foreground_thread != 0 && foreground_thread != current_thread &&
		AttachThreadInput(current_thread, foreground_thread, TRUE) != FALSE;
	const bool attached_game = game_thread != 0 && game_thread != current_thread &&
		game_thread != foreground_thread && AttachThreadInput(current_thread, game_thread, TRUE) != FALSE;

	ShowWindowAsync(g_game_window, SW_RESTORE);
	BringWindowToTop(g_game_window);
	SetForegroundWindow(g_game_window);
	SetActiveWindow(g_game_window);
	SetFocus(g_game_window);

	if (attached_game)
		AttachThreadInput(current_thread, game_thread, FALSE);
	if (attached_foreground)
		AttachThreadInput(current_thread, foreground_thread, FALSE);
	return game_has_focus();
}

bool send_key(WORD virtual_key, bool down)
{
	INPUT input = {};
	input.type = INPUT_KEYBOARD;
	input.ki.wVk = virtual_key;
	input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
	return SendInput(1, &input, sizeof(input)) == 1;
}

bool click_runtime_window()
{
	RECT bounds = {};
	if (g_game_window == nullptr || !GetWindowRect(g_game_window, &bounds))
		return false;
	BringWindowToTop(g_game_window);
	SetCursorPos(bounds.left + (bounds.right - bounds.left) / 2,
		bounds.top + (bounds.bottom - bounds.top) / 2);
	INPUT input[2] = {};
	input[0].type = INPUT_MOUSE;
	input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
	input[1].type = INPUT_MOUSE;
	input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
	return SendInput(2, input, sizeof(INPUT)) == 2;
}

void release_automation_keys()
{
	if (g_w_held)
		send_key('W', false);
	if (g_d_held)
		send_key('D', false);
	if (g_b_held)
		send_key('B', false);
	if (g_steam_screenshot_key_down)
		send_key(VK_F12, false);
	g_w_held = false;
	g_d_held = false;
	g_b_held = false;
	g_steam_screenshot_key_down = false;
}

bool read_automation_request()
{
	if (g_automation_path.empty())
		return false;
	HANDLE file = CreateFileW(g_automation_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	char text[64] = {};
	DWORD read = 0;
	const bool ok = ReadFile(file, text, sizeof(text) - 1, &read, nullptr) != FALSE;
	CloseHandle(file);
	DeleteFileW(g_automation_path.c_str());
	unsigned delay_ms = 20000, skip_intro = 0, edit_test = 0, capture_enabled = 0,
		steam_capture_enabled = 0;
	if (!ok || sscanf_s(text, "%u %u %u %u %u", &delay_ms, &skip_intro, &edit_test,
		&capture_enabled, &steam_capture_enabled) < 1)
		return false;
	g_automation_delay_ms = std::min(delay_ms, 120000u);
	g_skip_intro = skip_intro != 0;
	g_edit.requested = edit_test != 0;
	g_capture_enabled = capture_enabled != 0;
	g_steam_capture_enabled = steam_capture_enabled != 0;
	return true;
}

bool capture_frame(effect_runtime *runtime, const wchar_t *name)
{
	if (g_steam_capture_enabled)
	{
		if (std::wcsncmp(name, L"capture-edit-", 13) != 0)
			return true;
		if (g_steam_screenshot_key_down || !send_key(VK_F12, true))
			return false;
		g_steam_screenshot_key_down = true;
		g_steam_screenshot_release = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
		++g_capture_count;
		return true;
	}
	if (!g_capture_enabled)
		return true;
	uint32_t width = 0, height = 0;
	runtime->get_screenshot_width_and_height(&width, &height);
	if (width == 0 || height == 0 || width > 16384 || height > 16384)
		return false;

	std::vector<uint8_t> source(static_cast<size_t>(width) * height * 4);
	if (!runtime->capture_screenshot(source.data()))
		return false;

	const uint32_t output_width = std::min(width, 480u);
	const uint32_t output_height = std::max(1u, static_cast<uint32_t>(
		static_cast<uint64_t>(height) * output_width / width));
	const uint32_t row_size = (output_width * 3 + 3) & ~3u;
	std::vector<uint8_t> image(static_cast<size_t>(row_size) * output_height);
	for (uint32_t y = 0; y < output_height; ++y)
	{
		const uint32_t source_y = static_cast<uint32_t>(static_cast<uint64_t>(y) * height / output_height);
		uint8_t *row = image.data() + static_cast<size_t>(output_height - 1 - y) * row_size;
		for (uint32_t x = 0; x < output_width; ++x)
		{
			const uint32_t source_x = static_cast<uint32_t>(static_cast<uint64_t>(x) * width / output_width);
			const uint8_t *pixel = source.data() + (static_cast<size_t>(source_y) * width + source_x) * 4;
			row[x * 3 + 0] = pixel[2];
			row[x * 3 + 1] = pixel[1];
			row[x * 3 + 2] = pixel[0];
		}
	}

	BITMAPFILEHEADER file_header = {};
	BITMAPINFOHEADER info_header = {};
	file_header.bfType = 0x4D42;
	file_header.bfOffBits = sizeof(file_header) + sizeof(info_header);
	file_header.bfSize = file_header.bfOffBits + static_cast<DWORD>(image.size());
	info_header.biSize = sizeof(info_header);
	info_header.biWidth = static_cast<LONG>(output_width);
	info_header.biHeight = static_cast<LONG>(output_height);
	info_header.biPlanes = 1;
	info_header.biBitCount = 24;
	info_header.biCompression = BI_RGB;
	info_header.biSizeImage = static_cast<DWORD>(image.size());

	const std::wstring path = g_capture_directory + L"\\" + name;
	const std::wstring temporary = path + L".tmp";
	HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	DWORD written = 0;
	bool ok = WriteFile(file, &file_header, sizeof(file_header), &written, nullptr) != FALSE;
	ok = ok && WriteFile(file, &info_header, sizeof(info_header), &written, nullptr) != FALSE;
	ok = ok && WriteFile(file, image.data(), static_cast<DWORD>(image.size()), &written, nullptr) != FALSE;
	CloseHandle(file);
	if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
	{
		DeleteFileW(temporary.c_str());
		return false;
	}
	++g_capture_count;
	return true;
}

void fail_automation(uint32_t error)
{
	release_automation_keys();
	g_automation_error = error;
	g_automation_stage = 9;
}

void begin_scene_delay(effect_runtime *runtime, std::chrono::steady_clock::time_point now)
{
	release_automation_keys();
	if (!capture_frame(runtime, L"capture-start.bmp"))
		g_automation_error = 3;
	g_automation_deadline = now + std::chrono::milliseconds(g_automation_delay_ms);
	g_automation_stage = 3;
}

void on_reshade_present(effect_runtime *runtime)
{
	if (runtime->get_device() != g_device)
		return;
	const auto now = std::chrono::steady_clock::now();
	if (g_steam_screenshot_key_down && now >= g_steam_screenshot_release)
	{
		send_key(VK_F12, false);
		g_steam_screenshot_key_down = false;
	}
	if (g_automation_stage == 0 && read_automation_request())
		g_automation_stage = 1;

	if ((g_w_held || g_d_held || g_b_held) && !game_has_focus())
	{
		fail_automation(2);
		return;
	}

	if (g_automation_stage == 1)
	{
		if (const HWND console_window = GetConsoleWindow(); console_window != nullptr)
			ShowWindowAsync(console_window, SW_MINIMIZE);
		if (!focus_runtime_window(runtime))
		{
			if (!g_focus_click_attempted)
			{
				g_focus_click_attempted = true;
				click_runtime_window();
			}
			return;
		}
		if (!capture_frame(runtime, L"capture-load.bmp"))
			g_automation_error = 3;
		if (g_skip_intro)
		{
			g_automation_deadline = now + std::chrono::seconds(25);
			g_automation_stage = 2;
		}
		else
			begin_scene_delay(runtime, now);
	}
	else if (g_automation_stage == 2 && now >= g_automation_deadline)
	{
		if (!g_w_held)
		{
			if (!focus_runtime_window(runtime) || !send_key('W', true))
			{
				fail_automation(1);
				return;
			}
			g_w_held = true;
			g_automation_deadline = now + std::chrono::milliseconds(150);
		}
		else
		{
			send_key('W', false);
			g_w_held = false;
			++g_intro_attempts;
			begin_scene_delay(runtime, now);
		}
	}
	else if (g_automation_stage == 3 && now >= g_automation_deadline &&
		g_last_draws.load(std::memory_order_relaxed) >= 20)
	{
		if (!focus_runtime_window(runtime))
			return;
		if (!capture_frame(runtime, L"capture-ready.bmp"))
			g_automation_error = 3;
		{
			std::lock_guard lock(g_mutex);
			g_candidates.clear();
			g_ring_targets.clear();
			g_scan_queue.clear();
			g_candidate_cursor = 0;
			g_selected = 0;
		}
		if (g_edit.requested && !ensure_ib_hunt_thread())
		{
			fail_automation(4);
			return;
		}
		if (!send_key('W', true))
		{
			fail_automation(1);
			return;
		}
		g_w_held = true;
		g_automation_input_started = true;
		g_automation_deadline = now + std::chrono::seconds(3);
		g_automation_stage = 4;
	}
	else if (g_automation_stage == 4 && now >= g_automation_deadline)
	{
		if (!send_key('D', true))
		{
			fail_automation(1);
			return;
		}
		g_d_held = true;
		g_automation_deadline = now + std::chrono::seconds(2);
		g_automation_stage = 5;
	}
	else if (g_automation_stage == 5 && now >= g_automation_deadline)
	{
		release_automation_keys();
		if (!capture_frame(runtime, L"capture-walk.bmp"))
			g_automation_error = 3;
		g_automation_deadline = now + std::chrono::seconds(1);
		g_automation_stage = 6;
	}
	else if (g_automation_stage == 6 && now >= g_automation_deadline)
	{
		if (!focus_runtime_window(runtime) || !send_key('B', true))
		{
			fail_automation(1);
			return;
		}
		g_b_held = true;
		g_automation_deadline = now + std::chrono::milliseconds(150);
		g_automation_stage = 14;
	}
	else if (g_automation_stage == 14 && now >= g_automation_deadline)
	{
		if (!send_key('B', false))
		{
			fail_automation(1);
			return;
		}
		g_b_held = false;
		g_automation_deadline = now + std::chrono::seconds(g_edit.requested ? 1 : 2);
		g_automation_stage = g_edit.requested ? 12 : 7;
	}
	else if (g_automation_stage == 10 && now >= g_automation_deadline)
	{
		if (!capture_frame(runtime, L"capture-edit-active.bmp"))
			g_automation_error = 3;
		if (g_steam_capture_enabled)
		{
			g_automation_deadline = now + std::chrono::seconds(1);
			g_automation_stage = 16;
			return;
		}
		end_edit_pulse();
		g_automation_deadline = now + std::chrono::seconds(2);
		g_automation_stage = 11;
	}
	else if (g_automation_stage == 16 && now >= g_automation_deadline)
	{
		end_edit_pulse();
		g_automation_deadline = now + std::chrono::seconds(2);
		g_automation_stage = 11;
	}
	else if (g_automation_stage == 11 && now >= g_automation_deadline)
	{
		if (!capture_frame(runtime, L"capture-edit-restored.bmp"))
			g_automation_error = 3;
		{
			std::lock_guard lock(g_mutex);
			g_edit.completed = true;
		}
		g_automation_completed = true;
		g_automation_stage = 8;
	}
	else if (g_automation_stage == 12 && now >= g_automation_deadline)
	{
		if (!capture_frame(runtime, L"capture-stretch.bmp"))
			g_automation_error = 3;
		g_automation_deadline = now + std::chrono::seconds(2);
		g_automation_stage = 13;
	}
	else if (g_automation_stage == 13 && now >= g_automation_deadline)
	{
		if (now < g_ib_hunt_deadline &&
			(g_ib_hunt_phase.load(std::memory_order_acquire) == 1 || !has_reused_marker_target()))
			return;
		if (g_steam_capture_enabled)
		{
			if (!capture_frame(runtime, L"capture-edit-before.bmp"))
			{
				fail_automation(3);
				return;
			}
			g_automation_deadline = now + std::chrono::seconds(1);
			g_automation_stage = 15;
			return;
		}
		if (!begin_edit_test(runtime, now))
			fail_automation(4);
	}
	else if (g_automation_stage == 15 && now >= g_automation_deadline)
	{
		if (!begin_edit_test(runtime, now))
			fail_automation(4);
	}
	else if (g_automation_stage == 7 && now >= g_automation_deadline)
	{
		if (!capture_frame(runtime, L"capture-stretch.bmp"))
			g_automation_error = 3;
		g_automation_completed = true;
		g_automation_stage = 8;
	}
}

void write_telemetry(uint32_t draws)
{
	struct slot_sample
	{
		uint32_t slot = 0;
		float delta = 0.0f;
		float x = 0.0f, y = 0.0f, z = 0.0f;
	};

	const auto now = std::chrono::steady_clock::now();
	if (g_telemetry_path.empty() || now - g_last_telemetry < std::chrono::seconds(1))
		return;
	g_last_telemetry = now;

	size_t buffer_count = 0, mapped_count = 0, candidate_count = 0, moving_count = 0;
	size_t ring_count = 0, moving_ring_count = 0;
	size_t ib_hit_count = 0;
	uint32_t selected_id = 0, selected_slots = 0, selected_changed = 0;
	uint64_t live_resource = 0, live_end = 0, focus_draws = 0, live_locations = 0,
		residency_restores = 0;
	bool paused = false;
	edit_probe_state edit;
	std::vector<slot_sample> top_slots;
	{
		std::lock_guard lock(g_mutex);
		buffer_count = g_buffers.size();
		candidate_count = g_candidates.size();
		ring_count = g_ring_targets.size();
		ib_hit_count = g_ring_targets.size();
		live_resource = g_live_resource;
		live_end = g_live_end;
		focus_draws = g_focus_draws;
		live_locations = g_live_locations;
		residency_restores = g_residency_restores;
		paused = g_paused;
		edit = g_edit;
		for (const auto &[unused, info] : g_buffers)
			mapped_count += info.map_ptr != nullptr;
		for (const candidate &c : g_candidates)
		{
			moving_count += c.last_change_frame != 0 && g_frame - c.last_change_frame <= 120;
		}
		for (const ring_target &target : g_ring_targets)
			moving_ring_count += target.last_change_frame != 0 &&
				g_frame - target.last_change_frame <= 12000;
		if (!g_candidates.empty())
		{
			const candidate &selected = g_candidates[std::min(g_selected, g_candidates.size() - 1)];
			selected_id = selected.id;
			selected_slots = selected.detected_slots;
			selected_changed = selected.changed_slots;
			for (uint32_t slot = 0; slot < selected.slots; ++slot)
			{
				slot_sample sample;
				sample.slot = slot;
				sample.delta = selected.delta[slot];
				translation_of(selected, slot, sample.x, sample.y, sample.z);
				top_slots.push_back(sample);
			}
		}
	}
	std::sort(top_slots.begin(), top_slots.end(),
		[](const slot_sample &a, const slot_sample &b) { return a.delta > b.delta; });
	if (top_slots.size() > 12)
		top_slots.resize(12);

	std::ostringstream json;
	json << "{\n"
		<< "  \"schema\": 2,\n"
		<< "  \"addon\": \"HD2 Palette Probe\",\n"
		<< "  \"api_version\": " << RESHADE_API_VERSION << ",\n"
		<< "  \"process_id\": " << GetCurrentProcessId() << ",\n"
		<< "  \"window_handle\": " << reinterpret_cast<uintptr_t>(g_game_window) << ",\n"
		<< "  \"session_id\": " << g_session_id << ",\n"
		<< "  \"updated_unix_ms\": " << unix_time_ms() << ",\n"
		<< "  \"frame\": " << g_frame << ",\n"
		<< "  \"draws_last_frame\": " << draws << ",\n"
		<< "  \"tracked_buffers\": " << buffer_count << ",\n"
		<< "  \"mapped_buffers\": " << mapped_count << ",\n"
		<< "  \"scanned_bytes\": " << g_scanned_bytes << ",\n"
		<< "  \"ib_hunt_phase\": " << g_ib_hunt_phase.load(std::memory_order_relaxed) << ",\n"
		<< "  \"ib_hunt_passes\": " << g_ib_hunt_passes.load(std::memory_order_relaxed) << ",\n"
		<< "  \"ib_hunt_bytes\": " << g_ib_hunt_bytes.load(std::memory_order_relaxed) << ",\n"
		<< "  \"ib_fingerprint_hits\": " << ib_hit_count << ",\n"
		<< "  \"live_palette_resource\": " << live_resource << ",\n"
		<< "  \"live_palette_end\": " << live_end << ",\n"
		<< "  \"focus_draws\": " << focus_draws << ",\n"
		<< "  \"live_locations\": " << live_locations << ",\n"
		<< "  \"residency_restores\": " << residency_restores << ",\n"
		<< "  \"candidates\": " << candidate_count << ",\n"
		<< "  \"moving_candidates\": " << moving_count << ",\n"
		<< "  \"ring_targets\": " << ring_count << ",\n"
		<< "  \"moving_ring_targets\": " << moving_ring_count << ",\n"
		<< "  \"selected_candidate\": " << selected_id << ",\n"
		<< "  \"selected_slots\": " << selected_slots << ",\n"
		<< "  \"selected_changed_slots\": " << selected_changed << ",\n"
		<< "  \"paused\": " << (paused ? "true" : "false") << ",\n"
		<< "  \"automation_stage\": " << g_automation_stage << ",\n"
		<< "  \"automation_intro_attempts\": " << g_intro_attempts << ",\n"
		<< "  \"automation_input_started\": " << (g_automation_input_started ? "true" : "false") << ",\n"
		<< "  \"automation_completed\": " << (g_automation_completed ? "true" : "false") << ",\n"
		<< "  \"automation_error\": " << g_automation_error << ",\n"
		<< "  \"captures\": " << g_capture_count << ",\n"
		<< "  \"edit_test_requested\": " << (edit.requested ? "true" : "false") << ",\n"
		<< "  \"edit_test_active\": " << (edit.active ? "true" : "false") << ",\n"
		<< "  \"edit_test_completed\": " << (edit.completed ? "true" : "false") << ",\n"
		<< "  \"edit_target_had_motion\": " << (edit.target_had_motion ? "true" : "false") << ",\n"
		<< "  \"edit_candidate\": " << edit.candidate_id << ",\n"
		<< "  \"edit_slot\": " << edit.slot << ",\n"
		<< "  \"edit_slots_modified\": " << edit.slots_modified << ",\n"
		<< "  \"edit_stride\": " << edit.stride << ",\n"
		<< "  \"edit_translation_delta\": " << k_edit_translation << ",\n"
		<< "  \"edit_write_attempts\": " << edit.write_attempts << ",\n"
		<< "  \"edit_write_successes\": " << edit.write_successes << ",\n"
		<< "  \"edit_immediate_readbacks\": " << edit.immediate_readbacks << ",\n"
		<< "  \"edit_present_readbacks\": " << edit.present_readbacks << ",\n"
		<< "  \"edit_refills_observed\": " << edit.present_readbacks << ",\n"
		<< "  \"edit_stale_skips\": " << edit.stale_skips << ",\n"
		<< "  \"edit_targets_selected\": " << edit.targets_selected << ",\n"
		<< "  \"edit_targets_written\": " << edit.targets_written << ",\n"
		<< "  \"edit_targets_restored\": " << edit.targets_restored << ",\n"
		<< "  \"edit_round\": " << edit.round << ",\n"
		<< "  \"edit_total_rounds\": " << edit.total_rounds << ",\n"
		<< "  \"edit_restore_attempted\": " << (edit.restore_attempted ? "true" : "false") << ",\n"
		<< "  \"edit_restore_succeeded\": " << (edit.restore_succeeded ? "true" : "false") << ",\n"
		<< "  \"edit_overwrite_observed\": " << (edit.overwrite_observed ? "true" : "false") << ",\n"
		<< "  \"edit_error\": " << edit.error << ",\n"
		<< "  \"top_changed_slots\": [";
	for (size_t i = 0; i < top_slots.size(); ++i)
	{
		const slot_sample &slot = top_slots[i];
		json << (i == 0 ? "\n" : ",\n")
			<< "    { \"slot\": " << slot.slot
			<< ", \"delta\": " << std::setprecision(8) << slot.delta
			<< ", \"translation\": [" << slot.x << ", " << slot.y << ", " << slot.z << "] }";
	}
	if (!top_slots.empty())
		json << '\n';
	json << "  ]\n}\n";
	const std::string bytes = json.str();
	const std::wstring temporary = g_telemetry_path + L".tmp";
	HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return;
	DWORD written = 0;
	const bool complete = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
		written == static_cast<DWORD>(bytes.size());
	CloseHandle(file);
	if (complete)
		MoveFileExW(temporary.c_str(), g_telemetry_path.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void open_console()
{
	if (g_console != INVALID_HANDLE_VALUE)
		return;
	if (GetConsoleWindow() == nullptr && !AllocConsole())
		return;

	SetConsoleTitleW(L"HD2 Palette Probe");
	SetConsoleOutputCP(CP_UTF8);
	g_console = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_EXISTING, 0, nullptr);
	if (g_console != INVALID_HANDLE_VALUE)
	{
		DWORD mode = 0;
		if (GetConsoleMode(g_console, &mode))
			SetConsoleMode(g_console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
	}

	if (HWND window = GetConsoleWindow())
	{
		if (HMENU menu = GetSystemMenu(window, FALSE))
		{
			DeleteMenu(menu, SC_CLOSE, MF_BYCOMMAND);
			DrawMenuBar(window);
		}
	}
}

void console_write(const std::string &text)
{
	if (g_console == INVALID_HANDLE_VALUE)
		return;
	DWORD written = 0;
	WriteFile(g_console, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

float read_float(const uint8_t *data, size_t index)
{
	float value = 0.0f;
	std::memcpy(&value, data + index * sizeof(float), sizeof(value));
	return value;
}

void translation_of(const candidate &c, uint32_t slot, float &x, float &y, float &z)
{
	const uint8_t *p = c.bytes.data() + static_cast<size_t>(slot) * c.stride;
	if (c.stride == 64)
	{
		const size_t first = c.alternate_layout ? 3 : 12;
		x = read_float(p, first);
		y = read_float(p, first + (c.alternate_layout ? 4 : 1));
		z = read_float(p, first + (c.alternate_layout ? 8 : 2));
	}
	else if (c.stride == 48)
	{
		const size_t first = c.alternate_layout ? 9 : 3;
		x = read_float(p, first);
		y = read_float(p, first + (c.alternate_layout ? 1 : 4));
		z = read_float(p, first + (c.alternate_layout ? 2 : 8));
	}
	else
	{
		const float rx = read_float(p, 0), ry = read_float(p, 1);
		const float rz = read_float(p, 2), rw = read_float(p, 3);
		const float dx = read_float(p, 4), dy = read_float(p, 5);
		const float dz = read_float(p, 6), dw = read_float(p, 7);
		x = 2.0f * (-dw * rx + rw * dx + ry * dz - rz * dy);
		y = 2.0f * (-dw * ry + rw * dy + rz * dx - rx * dz);
		z = 2.0f * (-dw * rz + rw * dz + rx * dy - ry * dx);
	}
}

float skinning_distance(const candidate &c, const uint8_t *bytes)
{
	float total = 0.0f;
	for (uint32_t slot = 0; slot < c.slots; ++slot)
	{
		const uint8_t *p = bytes + static_cast<size_t>(slot) * c.stride;
		const size_t first = c.alternate_layout ? 9 : 3;
		const float x = read_float(p, first);
		const float y = read_float(p, first + (c.alternate_layout ? 1 : 4));
		const float z = read_float(p, first + (c.alternate_layout ? 2 : 8));
		total += std::sqrt(x * x + y * y + z * z);
		if (c.alternate_layout)
			for (size_t i = 0; i < 9; ++i)
				total += 0.25f * std::fabs(read_float(p, i) - (i % 4 == 0 ? 1.0f : 0.0f));
		else
			for (size_t row = 0; row < 3; ++row)
				for (size_t column = 0; column < 3; ++column)
					total += 0.25f * std::fabs(read_float(p, row * 4 + column) -
						(row == column ? 1.0f : 0.0f));
	}
	return c.slots == 0 ? 1.0e6f : total / c.slots;
}

void make_test_element(bool alternate_layout, float translation, uint8_t *out)
{
	std::memset(out, 0, 48);
	const float one = 1.0f;
	if (alternate_layout)
	{
		std::memcpy(out + 0 * sizeof(float), &one, sizeof(one));
		std::memcpy(out + 4 * sizeof(float), &one, sizeof(one));
		std::memcpy(out + 8 * sizeof(float), &one, sizeof(one));
		std::memcpy(out + 9 * sizeof(float), &translation, sizeof(translation));
	}
	else
	{
		std::memcpy(out + 0 * sizeof(float), &one, sizeof(one));
		std::memcpy(out + 5 * sizeof(float), &one, sizeof(one));
		std::memcpy(out + 10 * sizeof(float), &one, sizeof(one));
		std::memcpy(out + 3 * sizeof(float), &translation, sizeof(translation));
	}
}

uint8_t *mapped_pointer_locked(uint64_t resource, uint64_t offset, size_t size)
{
	auto it = g_buffers.find(resource);
	if (it == g_buffers.end() || it->second.map_ptr == nullptr || offset < it->second.map_offset)
		return nullptr;
	const uint64_t relative = offset - it->second.map_offset;
	if (relative > it->second.map_size || size > it->second.map_size - relative)
		return nullptr;
	return static_cast<uint8_t *>(it->second.map_ptr) + relative;
}

uint8_t *edit_target_pointer_locked(const active_edit_target &target)
{
	return mapped_pointer_locked(target.resource, target.offset, target.injected.size());
}

bool read_process_bytes(uintptr_t address, void *data, size_t size)
{
	SIZE_T read = 0;
	return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(address),
		data, size, &read) != FALSE && read == size;
}

bool write_process_bytes(uintptr_t address, const void *data, size_t size)
{
	SIZE_T written = 0;
	return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(address),
		data, size, &written) != FALSE && written == size;
}

void make_displaced_runtime_transform(const uint8_t *original, uint8_t *injected)
{
	std::memcpy(injected, original, k_marker_stride);
	float matrix[12] = {};
	std::memcpy(matrix, original, sizeof(matrix));
	for (size_t column = 0; column < 3; ++column)
		matrix[9 + column] += k_edit_translation * matrix[column];
	std::memcpy(injected, matrix, sizeof(matrix));
}

bool begin_edit_test(effect_runtime *runtime, std::chrono::steady_clock::time_point now)
{
	if (g_ib_hunt_phase.load(std::memory_order_acquire) != 2)
		return false;
	if (!g_steam_capture_enabled && !capture_frame(runtime, L"capture-edit-before.bmp"))
		g_automation_error = 3;

	std::lock_guard lock(g_mutex);
	g_active_edit_targets.clear();
	g_residency_probes.clear();
	g_residency_cursor = 0;
	g_residency_restores = 0;
	g_edit = edit_probe_state {};
	g_edit.requested = true;
	g_edit.stride = k_marker_stride;
	g_edit.slot = armature_fingerprint::target_slot;
	g_edit.round = 1;
	g_edit.total_rounds = 1;
	g_paused_before_edit = g_paused;
	g_paused = true;
	std::vector<ring_target> reused = g_ring_targets;
	std::sort(reused.begin(), reused.end(), [](const ring_target &a, const ring_target &b) {
		if ((a.changes != 0) != (b.changes != 0))
			return a.changes != 0;
		return std::max(a.last_change_frame, a.last_seen_frame) >
			std::max(b.last_change_frame, b.last_seen_frame);
	});
	for (const ring_target &marker : reused)
	{
		if (g_residency_probes.size() >= k_max_residency_probes)
			break;
		if (marker.offset < static_cast<uint64_t>(k_marker_repeats) * k_marker_stride)
			continue;
		const uint64_t probe_offset = marker.offset -
			static_cast<uint64_t>(k_marker_repeats) * k_marker_stride;
		uint8_t *pointer = mapped_pointer_locked(marker.resource, probe_offset,
			2u * k_marker_stride);
		if (pointer == nullptr)
			continue;
		residency_probe probe;
		probe.resource = marker.resource;
		probe.palette_end = marker.offset;
		armature_probe::copy_from_write_combined(probe.original.data(), pointer,
			probe.original.size());
		if (std::memcmp(probe.original.data(), pointer + k_marker_stride,
			k_marker_stride) != 0 ||
			!armature_probe::transform_like(probe.original.data(), k_marker_stride))
			continue;
		make_displaced_runtime_transform(probe.original.data(), probe.poisoned.data());
		std::memcpy(pointer, probe.poisoned.data(), probe.poisoned.size());
		std::array<uint8_t, k_marker_stride> check = {};
		armature_probe::copy_from_write_combined(check.data(), pointer, check.size());
		if (std::memcmp(check.data(), probe.poisoned.data(), check.size()) == 0)
			g_residency_probes.push_back(probe);
	}

	g_edit.targets_selected = static_cast<uint32_t>(g_residency_probes.size());
	g_edit.active = !g_residency_probes.empty();
	if (!g_edit.active)
	{
		g_edit.error = 2;
		g_paused = g_paused_before_edit;
		return false;
	}
	g_edit_active.store(true, std::memory_order_release);
	g_automation_deadline = now + std::chrono::seconds(15);
	g_automation_stage = 10;
	return true;
}

void write_edit_probe()
{
	if (!g_edit_active.load(std::memory_order_acquire))
		return;
	std::unique_lock lock(g_mutex, std::try_to_lock);
	if (!lock.owns_lock() || !g_edit.active)
		return;
	for (active_edit_target &target : g_active_edit_targets)
	{
		++g_edit.write_attempts;
		uint8_t *pointer = edit_target_pointer_locked(target);
		if (pointer == nullptr)
		{
			++g_edit.stale_skips;
			continue;
		}
		std::array<uint8_t, k_marker_stride> current = {};
		armature_probe::copy_from_write_combined(current.data(), pointer, current.size());
		if (std::memcmp(current.data(), target.injected.data(), current.size()) == 0)
		{
			++g_edit.present_readbacks;
			continue;
		}
		if (!armature_probe::transform_like(current.data(), k_marker_stride))
		{
			++g_edit.stale_skips;
			continue;
		}
		g_edit.overwrite_observed = g_edit.overwrite_observed || target.wrote;
		target.original.assign(current.begin(), current.end());
		make_displaced_runtime_transform(target.original.data(), target.injected.data());
		std::memcpy(pointer, target.injected.data(), target.injected.size());
		armature_probe::copy_from_write_combined(current.data(), pointer, current.size());
		if (std::memcmp(current.data(), target.injected.data(), current.size()) == 0)
		{
			++g_edit.write_successes;
			++g_edit.immediate_readbacks;
			if (!target.wrote)
			{
				target.wrote = true;
				++g_edit.targets_written;
			}
		}
	}
}

void end_edit_pulse()
{
	g_edit_active.store(false, std::memory_order_release);
	std::lock_guard lock(g_mutex);
	g_edit.active = false;
	bool restored = true;
	for (active_edit_target &target : g_active_edit_targets)
	{
		if (!target.wrote)
			continue;
		uint8_t *pointer = edit_target_pointer_locked(target);
		if (pointer == nullptr)
		{
			restored = false;
			continue;
		}
		std::array<uint8_t, k_marker_stride> current = {};
		armature_probe::copy_from_write_combined(current.data(), pointer, current.size());
		g_edit.restore_attempted = true;
		if (std::memcmp(current.data(), target.injected.data(), current.size()) != 0)
		{
			++g_edit.targets_restored;
			continue;
		}
		std::memcpy(pointer, target.original.data(), target.original.size());
		armature_probe::copy_from_write_combined(current.data(), pointer, current.size());
		if (std::memcmp(current.data(), target.original.data(), current.size()) == 0)
			++g_edit.targets_restored;
		else
			restored = false;
	}
	for (residency_probe &probe : g_residency_probes)
	{
		const uint64_t probe_offset = probe.palette_end -
			static_cast<uint64_t>(k_marker_repeats) * k_marker_stride;
		uint8_t *pointer = mapped_pointer_locked(probe.resource, probe_offset,
			k_marker_stride);
		if (pointer == nullptr)
			continue;
		std::array<uint8_t, k_marker_stride> current = {};
		armature_probe::copy_from_write_combined(current.data(), pointer, current.size());
		if (std::memcmp(current.data(), probe.poisoned.data(), current.size()) == 0)
			std::memcpy(pointer, probe.original.data(), probe.original.size());
	}
	g_edit.restore_succeeded = restored && g_edit.targets_restored == g_edit.targets_written &&
		g_edit.targets_written != 0;
	if (!g_edit.restore_succeeded)
		g_edit.error = 3;
	g_paused = g_paused_before_edit;
}

size_t copy_buffer(uint64_t handle, uint64_t offset, uint64_t wanted, std::vector<uint8_t> &out)
{
	std::lock_guard lock(g_mutex);
	auto it = g_buffers.find(handle);
	if (it == g_buffers.end() || it->second.map_ptr == nullptr ||
		offset < it->second.map_offset || offset >= it->second.size)
		return 0;

	const buffer_info &info = it->second;
	const uint64_t relative = offset - info.map_offset;
	if (relative >= info.map_size)
		return 0;
	const uint64_t count = std::min({ wanted, info.size - offset, info.map_size - relative });
	out.resize(static_cast<size_t>(count));
	armature_probe::copy_from_write_combined(out.data(),
		static_cast<const uint8_t *>(info.map_ptr) + relative, static_cast<size_t>(count));
	return out.size();
}

void refresh_candidate(uint32_t id)
{
	uint64_t handle = 0, offset = 0;
	uint32_t stride = 0, slots = 0;
	{
		std::lock_guard lock(g_mutex);
		auto it = std::find_if(g_candidates.begin(), g_candidates.end(),
			[id](const candidate &c) { return c.id == id; });
		if (it == g_candidates.end())
			return;
		handle = it->resource;
		offset = it->offset;
		stride = it->stride;
		slots = it->slots;
	}

	std::vector<uint8_t> bytes;
	if (copy_buffer(handle, offset, static_cast<uint64_t>(stride) * slots, bytes) == 0)
		return;

	const armature_probe::format_score score = armature_probe::assess(bytes.data(), bytes.size(), stride);
	std::lock_guard lock(g_mutex);
	auto it = std::find_if(g_candidates.begin(), g_candidates.end(),
		[id](const candidate &c) { return c.id == id; });
	if (it == g_candidates.end())
		return;

	it->fresh = score.confidence >= 0.70f;
	it->score = score.confidence;
	if (!it->fresh || bytes.size() != it->bytes.size())
		return;

	it->changed_slots = 0;
	for (uint32_t slot = 0; slot < it->slots; ++slot)
	{
		float largest = 0.0f;
		const size_t begin = static_cast<size_t>(slot) * stride;
		for (size_t p = begin; p < begin + stride; p += sizeof(float))
			largest = std::max(largest, std::fabs(read_float(bytes.data() + p, 0) - read_float(it->bytes.data() + p, 0)));
		it->delta[slot] = largest;
		it->peak_delta[slot] = std::max(it->peak_delta[slot], largest);
		if (largest > k_motion_epsilon)
			++it->changed_slots;
	}
	it->bytes.swap(bytes);
	++it->samples;
	if (it->changed_slots != 0)
	{
		++it->motion_frames;
		it->last_change_frame = g_frame;
	}
}

void remember_run(uint64_t handle, uint64_t offset, const armature_probe::matrix_run &run,
	const uint8_t *bytes)
{
	if (run.elements < k_min_slots || run.non_identity == 0)
		return;

	const uint32_t slots = std::min(run.elements, k_max_slots);
	const size_t byte_count = static_cast<size_t>(slots) * run.stride;
	const armature_probe::format_score score = armature_probe::assess(bytes, byte_count, run.stride);
	if (score.confidence < 0.80f)
		return;

	std::lock_guard lock(g_mutex);
	for (candidate &known : g_candidates)
	{
		if (known.resource == handle && known.offset == offset && known.stride == run.stride)
		{
			known.last_seen_frame = g_frame;
			known.detected_slots = std::max(known.detected_slots, run.elements);
			return;
		}
	}
	if (g_candidates.size() == k_max_candidates)
		return;

	candidate found;
	found.id = g_next_candidate_id++;
	found.resource = handle;
	found.offset = offset;
	found.stride = run.stride;
	found.slots = slots;
	found.detected_slots = run.elements;
	found.score = score.confidence;
	found.last_seen_frame = g_frame;
	found.bytes.assign(bytes, bytes + byte_count);
	found.delta.resize(slots);
	found.peak_delta.resize(slots);
	if (run.stride == 64)
		found.alternate_layout = score.layout_b > score.layout_a;
	else if (run.stride == 48)
		found.alternate_layout = score.layout_b > score.layout_a;
	g_candidates.push_back(std::move(found));
}

void remember_ring_targets(uint64_t handle, uint64_t window_offset, uint64_t seen_frame,
	const std::vector<armature_probe::matrix_run> &runs, const uint8_t *bytes)
{
	struct observation
	{
		uint64_t offset = 0;
		uint32_t slots = 0;
		bool alternate_layout = false;
		std::array<uint8_t, 48 * 7> sample {};
	};
	std::vector<observation> observations;
	for (const armature_probe::matrix_run &run : runs)
	{
		const armature_probe::format_score score = armature_probe::assess(
			bytes + run.offset, static_cast<size_t>(run.elements) * 48, 48);
		const uint32_t first = run.elements >= 22 ? 15 : 0;
		for (uint32_t slot = first; slot < run.elements; slot += 16)
		{
			observation found;
			found.offset = window_offset + run.offset + static_cast<uint64_t>(slot) * 48;
			found.slots = std::min(7u, run.elements - slot);
			found.alternate_layout = score.layout_b > score.layout_a;
			std::memcpy(found.sample.data(), bytes + run.offset + static_cast<size_t>(slot) * 48,
				static_cast<size_t>(found.slots) * 48);
			observations.push_back(found);
		}
	}

	std::lock_guard lock(g_mutex);
	if (g_buffers.find(handle) == g_buffers.end())
		return;
	for (const observation &found : observations)
	{
		auto known = std::find_if(g_ring_targets.begin(), g_ring_targets.end(),
			[handle, &found](const ring_target &target) {
				return target.resource == handle && target.offset == found.offset;
			});
		if (known == g_ring_targets.end())
		{
			if (g_ring_targets.size() < k_max_ring_targets)
			{
				g_ring_targets.emplace_back();
				known = g_ring_targets.end() - 1;
			}
			else
			{
				known = std::min_element(g_ring_targets.begin(), g_ring_targets.end(),
					[](const ring_target &a, const ring_target &b) {
						if ((a.changes == 0) != (b.changes == 0))
							return a.changes == 0;
						return a.last_seen_frame < b.last_seen_frame;
					});
				if (known->changes != 0)
					continue;
			}
			*known = ring_target {};
			known->resource = handle;
			known->offset = found.offset;
		}
		else if (known->observations != 0 && known->sample != found.sample)
		{
			++known->changes;
			known->last_change_frame = seen_frame;
		}
		known->slots = found.slots;
		known->alternate_layout = found.alternate_layout;
		known->last_seen_frame = seen_frame;
		++known->observations;
		known->sample = found.sample;
	}
}

DWORD WINAPI scan_thread_proc(void *)
{
	while (!g_scan_thread_stop.load(std::memory_order_acquire))
	{
		scan_sample sample;
		{
			std::lock_guard lock(g_mutex);
			if (!g_scan_queue.empty())
			{
				sample = std::move(g_scan_queue.front());
				g_scan_queue.erase(g_scan_queue.begin());
			}
		}
		if (sample.bytes.empty())
		{
			Sleep(10);
			continue;
		}
		const std::vector<armature_probe::matrix_run> runs =
			armature_probe::find_runs(sample.bytes.data(), sample.bytes.size(), 48, k_min_slots);
		remember_ring_targets(sample.resource, sample.offset, sample.frame, runs, sample.bytes.data());
	}
	return 0;
}

bool ensure_scan_thread()
{
	if (g_scan_thread != nullptr)
		return true;
	g_scan_thread_stop.store(false, std::memory_order_release);
	g_scan_thread = CreateThread(nullptr, 0, scan_thread_proc, nullptr, 0, nullptr);
	return g_scan_thread != nullptr;
}

void scan_one_window()
{
	uint64_t handle = 0, offset = 0, count = 0;
	{
		std::lock_guard lock(g_mutex);
		if (g_buffer_order.empty())
			return;

		for (size_t checked = 0; checked < g_buffer_order.size(); ++checked)
		{
			if (g_buffer_cursor >= g_buffer_order.size())
				g_buffer_cursor = 0;
			handle = g_buffer_order[g_buffer_cursor];
			auto it = g_buffers.find(handle);
			if (it == g_buffers.end())
			{
				g_buffer_order.erase(g_buffer_order.begin() + static_cast<std::ptrdiff_t>(g_buffer_cursor));
				continue;
			}

			buffer_info &info = it->second;
			if (info.map_ptr == nullptr || info.map_size < k_min_buffer_bytes)
			{
				++g_buffer_cursor;
				continue;
			}

			const uint64_t mapped_end = std::min(info.size, info.map_offset + info.map_size);
			if (info.scan_offset < info.map_offset || info.scan_offset >= mapped_end)
				info.scan_offset = info.map_offset;
			offset = info.scan_offset;
			count = std::min(k_scan_bytes, mapped_end - offset);
			const uint64_t advance = k_scan_bytes - k_scan_overlap;
			if (offset + count >= mapped_end || count <= k_scan_overlap)
			{
				info.scan_offset = info.map_offset;
				++g_buffer_cursor;
			}
			else
				info.scan_offset += advance;
			break;
		}
	}

	if (count < k_min_buffer_bytes)
		return;
	std::vector<uint8_t> bytes;
	const size_t copied = copy_buffer(handle, offset, count, bytes);
	if (copied == 0)
		return;
	g_scanned_bytes += copied;
	const armature_probe::matrix_run run = armature_probe::find_longest_run(bytes.data(), bytes.size());
	if (run.elements >= k_min_slots)
		remember_run(handle, offset + run.offset, run, bytes.data() + run.offset);
	if (g_frame % k_ring_scan_interval == 0 && ensure_scan_thread())
	{
		std::lock_guard lock(g_mutex);
		if (g_scan_queue.size() < 4)
			g_scan_queue.push_back(scan_sample { handle, offset, g_frame, std::move(bytes) });
	}
}

void remember_marker_target(uint64_t resource, uint64_t end, const uint8_t *sample)
{
	std::lock_guard lock(g_mutex);
	if (g_buffers.find(resource) == g_buffers.end())
		return;
	auto target = std::find_if(g_ring_targets.begin(), g_ring_targets.end(),
		[resource, end](const ring_target &known) {
			return known.resource == resource && known.offset == end;
		});
	if (target == g_ring_targets.end())
	{
		if (g_ring_targets.size() >= k_max_ring_targets)
			return;
		g_ring_targets.emplace_back();
		target = g_ring_targets.end() - 1;
		target->resource = resource;
		target->offset = end;
		target->slots = k_marker_repeats;
		target->alternate_layout = true;
	}
	else if (target->observations != 0 &&
		std::memcmp(target->sample.data(), sample, k_marker_stride) != 0)
	{
		++target->changes;
		target->last_change_frame = g_frame;
	}
	std::memcpy(target->sample.data(), sample, k_marker_stride);
	target->last_seen_frame = g_frame;
	++target->observations;
	g_ib_hunt_phase.store(2, std::memory_order_release);
}

void scan_one_marker_window()
{
	uint64_t resource = 0, offset = 0, count = 0;
	{
		std::lock_guard lock(g_mutex);
		if (g_buffer_order.empty())
			return;
		for (size_t checked = 0; checked < g_buffer_order.size(); ++checked)
		{
			if (g_buffer_cursor >= g_buffer_order.size())
				g_buffer_cursor = 0;
			resource = g_buffer_order[g_buffer_cursor];
			auto it = g_buffers.find(resource);
			if (it == g_buffers.end())
			{
				g_buffer_order.erase(g_buffer_order.begin() +
					static_cast<std::ptrdiff_t>(g_buffer_cursor));
				continue;
			}
			buffer_info &info = it->second;
			if (info.map_ptr == nullptr || info.map_size < k_marker_scan_overlap)
			{
				++g_buffer_cursor;
				continue;
			}
			const uint64_t mapped_end = std::min(info.size, info.map_offset + info.map_size);
			if (info.scan_offset < info.map_offset || info.scan_offset >= mapped_end)
				info.scan_offset = info.map_offset;
			offset = info.scan_offset;
			count = std::min(k_marker_scan_bytes, mapped_end - offset);
			const uint64_t advance = k_marker_scan_bytes - k_marker_scan_overlap;
			if (offset + count >= mapped_end || count <= k_marker_scan_overlap)
			{
				info.scan_offset = info.map_offset;
				++g_buffer_cursor;
				g_ib_hunt_passes.fetch_add(1, std::memory_order_relaxed);
			}
			else
				info.scan_offset += advance;
			break;
		}
	}
	if (count < k_marker_scan_overlap)
		return;

	std::vector<uint8_t> bytes;
	if (copy_buffer(resource, offset, count, bytes) != count)
		return;
	g_ib_hunt_bytes.fetch_add(bytes.size(), std::memory_order_relaxed);
	for (uint32_t phase = 0; phase < k_marker_stride; phase += 16)
	{
		size_t start = phase;
		uint32_t run = 1;
		for (size_t at = phase + k_marker_stride;
			at + k_marker_stride <= bytes.size(); at += k_marker_stride)
		{
			if (std::memcmp(bytes.data() + at - k_marker_stride,
				bytes.data() + at, k_marker_stride) == 0)
			{
				++run;
				continue;
			}
			if (run == k_marker_repeats &&
				armature_probe::transform_like(bytes.data() + start, k_marker_stride))
			{
				const uint64_t end = offset + start +
					static_cast<uint64_t>(run) * k_marker_stride;
				const uint64_t palette_bytes =
					static_cast<uint64_t>(k_marker_palette_slots - 1) * k_marker_stride;
				if (end >= palette_bytes)
				{
					std::vector<uint8_t> prefix;
					if (copy_buffer(resource, end - palette_bytes,
						14u * k_marker_stride, prefix) == 14u * k_marker_stride)
					{
						bool valid = true;
						for (uint32_t slot = 0; slot < 14; ++slot)
							valid = valid && armature_probe::transform_like(
								prefix.data() + static_cast<size_t>(slot) * k_marker_stride,
								k_marker_stride);
						if (valid)
							remember_marker_target(resource, end, bytes.data() + start);
					}
				}
			}
			start = at;
			run = 1;
		}
	}
}

uint64_t highest_marker_end(const uint8_t *bytes, size_t size, uint64_t base,
	uint64_t after)
{
	uint64_t best = 0;
	for (uint32_t phase = 0; phase < k_marker_stride; phase += 16)
	{
		size_t start = phase;
		uint32_t run = 1;
		auto finish = [&] {
			if (run == k_marker_repeats &&
				armature_probe::transform_like(bytes + start, k_marker_stride))
			{
				const uint64_t end = base + start +
					static_cast<uint64_t>(run) * k_marker_stride;
				if (end > after)
					best = std::max(best, end);
			}
		};
		for (size_t at = phase + k_marker_stride;
			at + k_marker_stride <= size; at += k_marker_stride)
		{
			if (std::memcmp(bytes + at - k_marker_stride, bytes + at,
				k_marker_stride) == 0)
			{
				++run;
				continue;
			}
			finish();
			start = at;
			run = 1;
		}
		finish();
	}
	return best;
}

void locate_and_write_live_palette(uint32_t index_count)
{
	if (index_count != k_focus_index_count_a && index_count != k_focus_index_count_b)
		return;

	uint64_t resource = 0, previous_end = 0, mapped_begin = 0, mapped_end = 0;
	double stride = 0.0;
	uint64_t previous_frame = 0;
	uint32_t misses = 0;
	{
		std::lock_guard lock(g_mutex);
		++g_focus_draws;
		resource = g_live_resource;
		previous_end = g_live_end;
		stride = g_live_stride;
		previous_frame = g_live_frame;
		misses = g_live_misses;
		const auto it = g_buffers.find(resource);
		if (it == g_buffers.end() || it->second.map_ptr == nullptr)
			return;
		mapped_begin = it->second.map_offset;
		mapped_end = std::min(it->second.size,
			it->second.map_offset + it->second.map_size);
	}
	if (mapped_end <= mapped_begin + k_marker_scan_overlap)
		return;

	const uint64_t frames = std::max<uint64_t>(1, g_frame - previous_frame);
	uint64_t predicted = previous_end + static_cast<uint64_t>(
		stride * static_cast<double>(std::min<uint64_t>(frames, 60)));
	uint64_t low = std::min(previous_end, predicted);
	if (low > 6 * k_marker_palette_bytes)
		low -= 6 * k_marker_palette_bytes;
	else
		low = mapped_begin;
	if (misses >= 16)
	{
		previous_end = mapped_begin;
		predicted = mapped_begin;
		low = mapped_begin;
	}
	low = std::max(low, mapped_begin) & ~15ull;
	const uint64_t high = std::min(mapped_end,
		std::max(previous_end, predicted) + (512ull << 10));
	if (high <= low)
		return;

	std::vector<uint8_t> bytes;
	const size_t got = copy_buffer(resource, low, high - low, bytes);
	if (got < k_marker_scan_overlap)
		return;
	const uint64_t found_end = highest_marker_end(bytes.data(), got, low, previous_end);
	if (found_end == 0)
	{
		std::lock_guard lock(g_mutex);
		++g_live_misses;
		return;
	}

	std::lock_guard lock(g_mutex);
	if (found_end > g_live_end && g_live_end != 0)
	{
		const double per_frame = static_cast<double>(found_end - g_live_end) /
			static_cast<double>(frames);
		if (per_frame <= 262144.0)
			g_live_stride = 0.8 * g_live_stride + 0.2 * per_frame;
	}
	g_live_end = found_end;
	g_live_frame = g_frame;
	g_live_misses = 0;
	++g_live_locations;

	const uint64_t distance = static_cast<uint64_t>(k_marker_palette_slots -
		armature_fingerprint::target_slot) * k_marker_stride;
	if (found_end < distance)
		return;
	const uint64_t target_offset = found_end - distance;
	if (!g_active_edit_targets.empty() &&
		g_active_edit_targets.front().resource == resource &&
		g_active_edit_targets.front().offset == target_offset)
		return;

	for (active_edit_target &old : g_active_edit_targets)
	{
		uint8_t *old_pointer = edit_target_pointer_locked(old);
		if (old_pointer == nullptr || !old.wrote)
			continue;
		std::array<uint8_t, k_marker_stride> current = {};
		armature_probe::copy_from_write_combined(current.data(), old_pointer, current.size());
		if (std::memcmp(current.data(), old.injected.data(), current.size()) == 0)
			std::memcpy(old_pointer, old.original.data(), old.original.size());
		++g_edit.targets_restored;
	}
	g_active_edit_targets.clear();

	active_edit_target target;
	target.resource = resource;
	target.palette_end = found_end;
	target.offset = target_offset;
	target.original.resize(k_marker_stride);
	target.injected.resize(k_marker_stride);
	uint8_t *pointer = edit_target_pointer_locked(target);
	++g_edit.write_attempts;
	++g_edit.targets_selected;
	if (pointer == nullptr)
	{
		++g_edit.stale_skips;
		return;
	}
	armature_probe::copy_from_write_combined(target.original.data(), pointer, k_marker_stride);
	if (!armature_probe::transform_like(target.original.data(), k_marker_stride))
	{
		++g_edit.stale_skips;
		return;
	}
	make_displaced_runtime_transform(target.original.data(), target.injected.data());
	std::memcpy(pointer, target.injected.data(), target.injected.size());
	std::array<uint8_t, k_marker_stride> check = {};
	armature_probe::copy_from_write_combined(check.data(), pointer, check.size());
	if (std::memcmp(check.data(), target.injected.data(), check.size()) != 0)
		return;
	target.wrote = true;
	++g_edit.write_successes;
	++g_edit.immediate_readbacks;
	++g_edit.targets_written;
	g_active_edit_targets.push_back(std::move(target));
}

void probe_and_write_fresh_palettes()
{
	std::unique_lock lock(g_mutex, std::try_to_lock);
	if (!lock.owns_lock() || !g_edit.active || g_residency_probes.empty())
		return;
	constexpr size_t batch = 256;
	const size_t count = std::min(batch, g_residency_probes.size());
	for (size_t checked = 0; checked < count; ++checked)
	{
		if (g_residency_cursor >= g_residency_probes.size())
			g_residency_cursor = 0;
		residency_probe &probe = g_residency_probes[g_residency_cursor++];
		const uint64_t probe_offset = probe.palette_end -
			static_cast<uint64_t>(k_marker_repeats) * k_marker_stride;
		uint8_t *probe_pointer = mapped_pointer_locked(probe.resource, probe_offset,
			2u * k_marker_stride);
		if (probe_pointer == nullptr)
			continue;
		std::array<uint8_t, 2 * k_marker_stride> tail = {};
		armature_probe::copy_from_write_combined(tail.data(), probe_pointer, tail.size());
		if (std::memcmp(tail.data(), probe.poisoned.data(), k_marker_stride) == 0 ||
			std::memcmp(tail.data(), tail.data() + k_marker_stride, k_marker_stride) != 0 ||
			!armature_probe::transform_like(tail.data(), k_marker_stride))
			continue;

		++g_residency_restores;
		g_edit.overwrite_observed = true;
		g_edit.target_had_motion = true;
		const uint64_t distance = static_cast<uint64_t>(k_marker_palette_slots -
			armature_fingerprint::target_slot) * k_marker_stride;
		if (probe.palette_end < distance)
			continue;
		const uint64_t target_offset = probe.palette_end - distance;
		auto known = std::find_if(g_active_edit_targets.begin(), g_active_edit_targets.end(),
			[&](const active_edit_target &target) {
				return target.resource == probe.resource && target.offset == target_offset;
			});
		if (known == g_active_edit_targets.end())
		{
			if (g_active_edit_targets.size() >= k_max_ring_targets)
				continue;
			g_active_edit_targets.emplace_back();
			known = g_active_edit_targets.end() - 1;
			known->resource = probe.resource;
			known->palette_end = probe.palette_end;
			known->offset = target_offset;
			known->original.resize(k_marker_stride);
			known->injected.resize(k_marker_stride);
		}
		uint8_t *target_pointer = edit_target_pointer_locked(*known);
		++g_edit.write_attempts;
		if (target_pointer == nullptr)
		{
			++g_edit.stale_skips;
			continue;
		}
		armature_probe::copy_from_write_combined(known->original.data(), target_pointer,
			k_marker_stride);
		if (!armature_probe::transform_like(known->original.data(), k_marker_stride))
		{
			++g_edit.stale_skips;
			continue;
		}
		make_displaced_runtime_transform(known->original.data(), known->injected.data());
		std::memcpy(target_pointer, known->injected.data(), known->injected.size());
		std::array<uint8_t, k_marker_stride> check = {};
		armature_probe::copy_from_write_combined(check.data(), target_pointer, check.size());
		if (std::memcmp(check.data(), known->injected.data(), check.size()) == 0)
		{
			++g_edit.write_successes;
			++g_edit.immediate_readbacks;
			if (!known->wrote)
			{
				known->wrote = true;
				++g_edit.targets_written;
				g_edit.slots_modified = g_edit.targets_written;
			}
		}

		std::memcpy(probe.original.data(), tail.data(), k_marker_stride);
		make_displaced_runtime_transform(probe.original.data(), probe.poisoned.data());
		std::memcpy(probe_pointer, probe.poisoned.data(), probe.poisoned.size());
	}
}

void handle_keys()
{
	std::lock_guard lock(g_mutex);
	if ((GetAsyncKeyState(VK_F6) & 1) && !g_candidates.empty())
	{
		g_selected = (g_selected + 1) % g_candidates.size();
		g_slot_page = 0;
	}
	if ((GetAsyncKeyState(VK_F7) & 1) && !g_candidates.empty())
	{
		const uint32_t pages = (g_candidates[g_selected].slots + 15) / 16;
		g_slot_page = pages == 0 ? 0 : (g_slot_page + 1) % pages;
	}
	if (GetAsyncKeyState(VK_F8) & 1)
		g_paused = !g_paused;
	if (GetAsyncKeyState(VK_F9) & 1)
	{
		g_candidates.clear();
		g_ring_targets.clear();
		g_selected = 0;
		g_slot_page = 0;
		g_candidate_cursor = 0;
		for (auto &[unused, info] : g_buffers)
			info.scan_offset = 0;
	}
}

void draw_console(uint32_t draws)
{
	const auto now = std::chrono::steady_clock::now();
	if (now - g_last_display < std::chrono::milliseconds(250))
		return;
	g_last_display = now;

	std::ostringstream out;
	out << "\x1b[2J\x1b[H"
		<< "HD2 Palette Probe 0.5  |  FINGERPRINTED INVERSE-BIND EDIT TEST  |  D3D12\n"
		<< "The edit test writes one slot in an offline-fingerprinted inverse-bind table.\n\n";

	std::lock_guard lock(g_mutex);
	size_t mapped = 0;
	for (const auto &[unused, info] : g_buffers)
		mapped += info.map_ptr != nullptr;
	out << "frame " << g_frame << "  draws " << draws
		<< "  buffers " << g_buffers.size() << " (mapped " << mapped << ")"
		<< "  swept " << std::fixed << std::setprecision(1)
		<< (static_cast<double>(g_scanned_bytes) / (1024.0 * 1024.0)) << " MiB"
		<< "  candidates " << g_candidates.size()
		<< "  ring targets " << g_ring_targets.size()
		<< (g_paused ? "  [PAUSED]\n" : "\n")
		<< "IB hunt phase " << g_ib_hunt_phase.load(std::memory_order_relaxed)
		<< "  passes " << g_ib_hunt_passes.load(std::memory_order_relaxed)
		<< "  hits " << g_ib_hits.size()
		<< "  scanned " << std::fixed << std::setprecision(1)
		<< (static_cast<double>(g_ib_hunt_bytes.load(std::memory_order_relaxed)) / (1024.0 * 1024.0))
		<< " MiB\n"
		<< "F6 next candidate   F7 next 16 slots   F8 pause   F9 rescan\n"
		<< "edit: " << (g_edit.active ? "ACTIVE" : g_edit.completed ? "complete" : g_edit.requested ? "armed" : "off")
		<< "  live targets " << g_edit.targets_written << '/' << g_edit.targets_selected
		<< " (" << g_edit.slots_modified << " slots)"
		<< "  writes " << g_edit.write_successes << '/' << g_edit.write_attempts
		<< "  refills " << g_edit.present_readbacks
		<< "  restored " << (g_edit.restore_succeeded ? "yes" : "no") << "\n\n";

	if (g_candidates.empty())
	{
		out << "Legacy mapped-buffer scan disabled for this focused IB write test.\n";
		console_write(out.str());
		return;
	}

	if (g_selected >= g_candidates.size())
		g_selected = 0;
	const candidate &c = g_candidates[g_selected];
	const uint32_t first = std::min(g_slot_page * 16, c.slots);
	const uint32_t last = std::min(first + 16, c.slots);
	out << "candidate " << (g_selected + 1) << '/' << g_candidates.size()
		<< "  id " << c.id << "  resource 0x" << std::hex << c.resource << std::dec
		<< "  offset 0x" << std::hex << c.offset << std::dec << '\n'
		<< "stride " << c.stride << "  slots " << c.detected_slots;
	if (c.detected_slots > c.slots)
		out << " (showing first " << c.slots << ')';
	out << "  score " << std::fixed << std::setprecision(3) << c.score
		<< "  changed now " << c.changed_slots
		<< "  motion frames " << c.motion_frames << '/' << c.samples
		<< (c.fresh ? "\n\n" : "  [STALE / OVERWRITTEN]\n\n")
		<< " slot       max |delta|             translation xyz\n";

	for (uint32_t slot = first; slot < last; ++slot)
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
		translation_of(c, slot, x, y, z);
		out << std::setw(5) << slot << "    " << std::scientific << std::setprecision(3)
			<< std::setw(12) << c.delta[slot] << "    " << std::fixed << std::setprecision(4)
			<< std::setw(10) << x << ' ' << std::setw(10) << y << ' ' << std::setw(10) << z << '\n';
	}
	console_write(out.str());
}

void on_init_device(device *device)
{
	if (device->get_api() != device_api::d3d12)
		return;
	std::lock_guard lock(g_mutex);
	if (g_device == nullptr)
	{
		g_device = device;
		initialize_telemetry();
		open_console();
	}
}

void on_destroy_device(device *device)
{
	std::lock_guard lock(g_mutex);
	if (device != g_device)
		return;
	g_buffers.clear();
	g_buffer_order.clear();
	g_candidates.clear();
	g_ring_targets.clear();
	g_active_edit_targets.clear();
	g_residency_probes.clear();
	g_scan_queue.clear();
	g_edit_active.store(false, std::memory_order_release);
	g_edit.active = false;
	g_device = nullptr;
}

void on_init_resource(device *device, const resource_desc &desc, const subresource_data *,
	resource_usage, resource resource)
{
	if (device != g_device || desc.type != resource_type::buffer ||
		desc.buffer.size < k_marker_scan_overlap)
		return;
	std::lock_guard lock(g_mutex);
	g_buffers[resource.handle] = buffer_info { desc.buffer.size, desc.heap };
	g_buffer_order.push_back(resource.handle);
}

void on_destroy_resource(device *device, resource resource)
{
	if (device != g_device)
		return;
	{
		std::lock_guard lock(g_mutex);
		auto it = g_buffers.find(resource.handle);
		if (it == g_buffers.end())
			return;
		g_buffers.erase(it);
		g_candidates.erase(std::remove_if(g_candidates.begin(), g_candidates.end(),
			[resource](const candidate &c) { return c.resource == resource.handle; }), g_candidates.end());
		g_ring_targets.erase(std::remove_if(g_ring_targets.begin(), g_ring_targets.end(),
			[resource](const ring_target &target) { return target.resource == resource.handle; }),
			g_ring_targets.end());
		g_residency_probes.erase(std::remove_if(g_residency_probes.begin(), g_residency_probes.end(),
			[resource](const residency_probe &probe) { return probe.resource == resource.handle; }),
			g_residency_probes.end());
		g_active_edit_targets.erase(std::remove_if(g_active_edit_targets.begin(),
			g_active_edit_targets.end(), [resource](const active_edit_target &target) {
				return target.resource == resource.handle;
			}), g_active_edit_targets.end());
		if (g_selected >= g_candidates.size())
			g_selected = 0;
	}
}

void on_map_buffer(device *device, resource resource, uint64_t offset, uint64_t size,
	map_access, void **data)
{
	if (device != g_device || data == nullptr || *data == nullptr)
		return;
	std::lock_guard lock(g_mutex);
	auto it = g_buffers.find(resource.handle);
	if (it == g_buffers.end() || offset >= it->second.size)
		return;
	buffer_info &info = it->second;
	info.map_ptr = *data;
	info.map_offset = offset;
	info.map_size = (size == 0 || size == UINT64_MAX) ? info.size - offset : std::min(size, info.size - offset);
	info.scan_offset = offset;
}

void on_unmap_buffer(device *device, resource resource)
{
	if (device != g_device)
		return;
	std::lock_guard lock(g_mutex);
	auto it = g_buffers.find(resource.handle);
	if (it != g_buffers.end())
	{
		it->second.map_ptr = nullptr;
		it->second.map_offset = 0;
		it->second.map_size = 0;
	}
}

bool on_draw_indexed(command_list *, uint32_t index_count, uint32_t, uint32_t, int32_t, uint32_t)
{
	g_draws.fetch_add(1, std::memory_order_relaxed);
	if (g_edit_active.load(std::memory_order_acquire) &&
		(index_count == k_focus_index_count_a || index_count == k_focus_index_count_b))
	{
		{
			std::lock_guard lock(g_mutex);
			++g_focus_draws;
		}
		probe_and_write_fresh_palettes();
	}
	return false;
}

void on_present(command_queue *queue, swapchain *, const rect *, const rect *, uint32_t, const rect *)
{
	if (queue->get_device() != g_device)
		return;
	++g_frame;
	const uint32_t draws = g_draws.exchange(0, std::memory_order_relaxed);
	g_last_draws.store(draws, std::memory_order_relaxed);
	handle_keys();

	if (k_enable_anonymous_scan)
	{
		uint32_t selected_id = 0, rotating_id = 0;
		{
			std::lock_guard lock(g_mutex);
			if (!g_candidates.empty())
			{
				if (g_selected >= g_candidates.size())
					g_selected = 0;
				selected_id = g_candidates[g_selected].id;
				if (g_candidate_cursor >= g_candidates.size())
					g_candidate_cursor = 0;
				rotating_id = g_candidates[g_candidate_cursor++].id;
			}
		}
		if (selected_id != 0)
			refresh_candidate(selected_id);
		if (rotating_id != 0 && rotating_id != selected_id)
			refresh_candidate(rotating_id);
		if (!g_paused)
			scan_one_window();
	}
	const uint32_t hunt_phase = g_ib_hunt_phase.load(std::memory_order_acquire);
	if (hunt_phase == 1 || hunt_phase == 2)
		scan_one_marker_window();
	draw_console(draws);
	write_telemetry(draws);
}
}

extern "C"
{
__declspec(dllexport) const char *NAME = "HD2 Palette Probe";
__declspec(dllexport) const char *DESCRIPTION =
	"D3D12 transform scanner with a fingerprinted, reversible inverse-bind edit probe.";
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(module);
		if (!reshade::register_addon(module))
			return FALSE;
		reshade::register_event<addon_event::init_device>(on_init_device);
		reshade::register_event<addon_event::destroy_device>(on_destroy_device);
		reshade::register_event<addon_event::init_resource>(on_init_resource);
		reshade::register_event<addon_event::destroy_resource>(on_destroy_resource);
		reshade::register_event<addon_event::map_buffer_region>(on_map_buffer);
		reshade::register_event<addon_event::unmap_buffer_region>(on_unmap_buffer);
		reshade::register_event<addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<addon_event::present>(on_present);
		reshade::register_event<addon_event::reshade_present>(on_reshade_present);
	}
	else if (reason == DLL_PROCESS_DETACH)
	{
		g_scan_thread_stop.store(true, std::memory_order_release);
		g_ib_hunt_stop.store(true, std::memory_order_release);
		release_automation_keys();
		if (reserved != nullptr)
			return TRUE;
		if (g_scan_thread != nullptr)
		{
			WaitForSingleObject(g_scan_thread, 1000);
			CloseHandle(g_scan_thread);
			g_scan_thread = nullptr;
		}
		if (g_ib_hunt_thread != nullptr)
		{
			WaitForSingleObject(g_ib_hunt_thread, 1000);
			CloseHandle(g_ib_hunt_thread);
			g_ib_hunt_thread = nullptr;
		}
		reshade::unregister_event<addon_event::reshade_present>(on_reshade_present);
		reshade::unregister_event<addon_event::present>(on_present);
		reshade::unregister_event<addon_event::draw_indexed>(on_draw_indexed);
		reshade::unregister_event<addon_event::unmap_buffer_region>(on_unmap_buffer);
		reshade::unregister_event<addon_event::map_buffer_region>(on_map_buffer);
		reshade::unregister_event<addon_event::destroy_resource>(on_destroy_resource);
		reshade::unregister_event<addon_event::init_resource>(on_init_resource);
		reshade::unregister_event<addon_event::destroy_device>(on_destroy_device);
		reshade::unregister_event<addon_event::init_device>(on_init_device);
		reshade::unregister_addon(module);
		if (g_console != INVALID_HANDLE_VALUE)
		{
			CloseHandle(g_console);
			g_console = INVALID_HANDLE_VALUE;
		}
	}
	return TRUE;
}
