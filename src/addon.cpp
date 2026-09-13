#include <reshade.hpp>

#include "matrix_scan.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
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
constexpr float k_motion_epsilon = 1.0e-5f;
constexpr float k_edit_translation = 4.0f;

struct buffer_info
{
	uint64_t size = 0;
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
	uint32_t round = 0;
	uint32_t total_rounds = 0;
	bool restore_attempted = false;
	bool restore_succeeded = false;
	bool overwrite_observed = false;
	uint32_t error = 0;
	std::vector<uint8_t> original;
	std::vector<uint8_t> injected;
};

std::mutex g_mutex;
std::unordered_map<uint64_t, buffer_info> g_buffers;
std::vector<uint64_t> g_buffer_order;
std::vector<candidate> g_candidates;
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
bool g_automation_input_started = false;
bool g_automation_completed = false;
bool g_w_held = false;
bool g_d_held = false;
std::chrono::steady_clock::time_point g_automation_deadline;
edit_probe_state g_edit;
std::atomic<bool> g_edit_active { false };
std::atomic<bool> g_edit_thread_stop { false };
HANDLE g_edit_thread = nullptr;
std::vector<uint32_t> g_edit_queue;
size_t g_edit_queue_index = 0;

void translation_of(const candidate &c, uint32_t slot, float &x, float &y, float &z);
bool begin_edit_test(effect_runtime *runtime, std::chrono::steady_clock::time_point now);
bool start_next_edit_target(std::chrono::steady_clock::time_point now);
void end_edit_pulse();
void write_edit_probe();
bool ensure_edit_thread();

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

void release_automation_keys()
{
	if (g_w_held)
		send_key('W', false);
	if (g_d_held)
		send_key('D', false);
	g_w_held = false;
	g_d_held = false;
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
	unsigned delay_ms = 20000, skip_intro = 0, edit_test = 0;
	if (!ok || sscanf_s(text, "%u %u %u", &delay_ms, &skip_intro, &edit_test) < 1)
		return false;
	g_automation_delay_ms = std::min(delay_ms, 120000u);
	g_skip_intro = skip_intro != 0;
	g_edit.requested = edit_test != 0;
	return true;
}

bool capture_frame(effect_runtime *runtime, const wchar_t *name)
{
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
	if (g_automation_stage == 0 && read_automation_request())
		g_automation_stage = 1;

	if ((g_w_held || g_d_held) && !game_has_focus())
	{
		fail_automation(2);
		return;
	}

	if (g_automation_stage == 1)
	{
		if (const HWND console_window = GetConsoleWindow(); console_window != nullptr)
			ShowWindowAsync(console_window, SW_MINIMIZE);
		if (!focus_runtime_window(runtime))
			return;
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
		if (g_edit.requested)
		{
			if (!focus_runtime_window(runtime))
				return;
			if (!send_key('B', true) || !send_key('B', false))
			{
				fail_automation(1);
				return;
			}
			g_automation_deadline = now + std::chrono::seconds(4);
			g_automation_stage = 12;
			return;
		}
		if (!focus_runtime_window(runtime))
			return;
		if (!send_key('B', true) || !send_key('B', false))
		{
			fail_automation(1);
			return;
		}
		g_automation_deadline = now + std::chrono::seconds(4);
		g_automation_stage = 7;
	}
	else if (g_automation_stage == 10 && now >= g_automation_deadline)
	{
		wchar_t name[64] = {};
		swprintf_s(name, L"capture-edit-c%03u-all.bmp", g_edit.candidate_id);
		if (!capture_frame(runtime, name))
			g_automation_error = 3;
		end_edit_pulse();
		g_automation_deadline = now + std::chrono::milliseconds(250);
		g_automation_stage = 11;
	}
	else if (g_automation_stage == 11 && now >= g_automation_deadline)
	{
		if (start_next_edit_target(now))
			return;
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
	uint32_t selected_id = 0, selected_slots = 0, selected_changed = 0;
	bool paused = false;
	edit_probe_state edit;
	std::vector<slot_sample> top_slots;
	{
		std::lock_guard lock(g_mutex);
		buffer_count = g_buffers.size();
		candidate_count = g_candidates.size();
		paused = g_paused;
		edit = g_edit;
		for (const auto &[unused, info] : g_buffers)
			mapped_count += info.map_ptr != nullptr;
		for (const candidate &c : g_candidates)
			moving_count += c.last_change_frame != 0 && g_frame - c.last_change_frame <= 120;
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
		<< "  \"session_id\": " << g_session_id << ",\n"
		<< "  \"updated_unix_ms\": " << unix_time_ms() << ",\n"
		<< "  \"frame\": " << g_frame << ",\n"
		<< "  \"draws_last_frame\": " << draws << ",\n"
		<< "  \"tracked_buffers\": " << buffer_count << ",\n"
		<< "  \"mapped_buffers\": " << mapped_count << ",\n"
		<< "  \"scanned_bytes\": " << g_scanned_bytes << ",\n"
		<< "  \"candidates\": " << candidate_count << ",\n"
		<< "  \"moving_candidates\": " << moving_count << ",\n"
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

bool write_edit_bytes_locked(const std::vector<uint8_t> &bytes, bool verify)
{
	auto it = g_buffers.find(g_edit.resource);
	if (it == g_buffers.end() || it->second.map_ptr == nullptr || g_edit.offset < it->second.map_offset)
		return false;
	const uint64_t relative = g_edit.offset - it->second.map_offset;
	if (relative > it->second.map_size || bytes.size() > it->second.map_size - relative)
		return false;
	uint8_t *target = static_cast<uint8_t *>(it->second.map_ptr) + relative;
	std::memcpy(target, bytes.data(), bytes.size());
	return !verify || std::memcmp(target, bytes.data(), bytes.size()) == 0;
}

bool begin_edit_test(effect_runtime *runtime, std::chrono::steady_clock::time_point now)
{
	if (!ensure_edit_thread())
	{
		std::lock_guard lock(g_mutex);
		g_edit.error = 5;
		return false;
	}

	struct ranked_candidate
	{
		uint32_t id = 0;
		uint32_t stride = 0;
		float peak = 0.0f;
	};
	std::vector<ranked_candidate> ranked;
	{
		std::lock_guard lock(g_mutex);
		for (const candidate &c : g_candidates)
		{
			if (!c.fresh || (c.stride != 48 && c.stride != 64) || c.bytes.size() < c.stride)
				continue;
			const float peak = *std::max_element(c.peak_delta.begin(), c.peak_delta.end());
			ranked.push_back({ c.id, c.stride, peak });
		}
	}
	if (ranked.empty())
	{
		std::lock_guard lock(g_mutex);
		g_edit.error = 1;
		return false;
	}
	std::sort(ranked.begin(), ranked.end(), [](const ranked_candidate &a, const ranked_candidate &b) {
		const bool a_moved = a.peak > k_motion_epsilon;
		const bool b_moved = b.peak > k_motion_epsilon;
		if (a_moved != b_moved)
			return a_moved > b_moved;
		if (a.stride != b.stride)
			return a.stride == 48;
		return a.peak > b.peak;
	});
	g_edit_queue.clear();
	for (const ranked_candidate &entry : ranked)
		g_edit_queue.push_back(entry.id);
	g_edit_queue_index = 0;
	{
		std::lock_guard lock(g_mutex);
		g_edit.requested = true;
		g_edit.total_rounds = static_cast<uint32_t>(g_edit_queue.size());
	}
	if (!capture_frame(runtime, L"capture-edit-before.bmp"))
		g_automation_error = 3;
	return start_next_edit_target(now);
}

bool start_next_edit_target(std::chrono::steady_clock::time_point now)

{
	while (g_edit_queue_index < g_edit_queue.size())
	{
		const uint32_t id = g_edit_queue[g_edit_queue_index++];
		std::lock_guard lock(g_mutex);
		auto it = std::find_if(g_candidates.begin(), g_candidates.end(),
			[id](const candidate &c) { return c.id == id; });
		if (it == g_candidates.end() || !it->fresh ||
			(it->stride != 48 && it->stride != 64) ||
			it->bytes.size() < static_cast<size_t>(it->stride) * it->slots)
			continue;

		uint32_t slot = 0;
		float peak = 0.0f;
		for (uint32_t i = 0; i < it->slots; ++i)
			if (it->peak_delta[i] > peak)
			{
				peak = it->peak_delta[i];
				slot = i;
			}
		g_edit.target_had_motion = peak > k_motion_epsilon;
		g_edit.candidate_id = it->id;
		g_edit.slot = slot;
		g_edit.slots_modified = it->slots;
		g_edit.stride = it->stride;
		g_edit.resource = it->resource;
		g_edit.offset = it->offset;
		g_edit.round = static_cast<uint32_t>(g_edit_queue_index);
		g_edit.restore_attempted = false;
		g_edit.restore_succeeded = false;
		g_edit.original = it->bytes;
		g_edit.injected = it->bytes;
		const size_t translation_index = it->stride == 64 ?
			(it->alternate_layout ? 3 : 12) : (it->alternate_layout ? 9 : 3);
		const size_t translation_step = (it->stride == 64) == it->alternate_layout ? 4 : 1;
		bool valid = true;
		for (uint32_t i = 0; i < it->slots; ++i)
		{
			uint8_t *element = g_edit.injected.data() + static_cast<size_t>(i) * it->stride;
			const float direction = (i & 1) == 0 ? 1.0f : -1.0f;
			for (size_t axis = 0; axis < 3; ++axis)
			{
				const size_t component = translation_index + axis * translation_step;
				float translated = read_float(element, component) +
					direction * k_edit_translation * static_cast<float>(axis + 1);
				if (!std::isfinite(translated) || std::fabs(translated) >= 1.0e6f)
				{
					valid = false;
					break;
				}
				std::memcpy(element + component * sizeof(float), &translated, sizeof(translated));
			}
			if (!valid)
				break;
		}
		if (!valid)
			continue;
		g_edit.active = true;
		g_edit_active.store(true, std::memory_order_release);
		g_automation_deadline = now + std::chrono::milliseconds(1500);
		g_automation_stage = 10;
		return true;
	}
	return false;
}

void write_edit_probe()
{
	if (!g_edit_active.load(std::memory_order_acquire))
		return;
	std::lock_guard lock(g_mutex);
	if (!g_edit.active)
		return;
	++g_edit.write_attempts;
	if (write_edit_bytes_locked(g_edit.injected, true))
	{
		++g_edit.write_successes;
		++g_edit.immediate_readbacks;
	}
	else
	{
		g_edit.error = 2;
		g_edit.active = false;
		g_edit_active.store(false, std::memory_order_release);
	}
}

DWORD WINAPI edit_thread_proc(void *)
{
	while (!g_edit_thread_stop.load(std::memory_order_acquire))
	{
		if (g_edit_active.load(std::memory_order_acquire))
		{
			write_edit_probe();
			Sleep(1);
		}
		else
		{
			Sleep(10);
		}
	}
	return 0;
}

bool ensure_edit_thread()
{
	if (g_edit_thread != nullptr)
		return true;
	g_edit_thread_stop.store(false, std::memory_order_release);
	g_edit_thread = CreateThread(nullptr, 0, edit_thread_proc, nullptr, 0, nullptr);
	return g_edit_thread != nullptr;
}

void end_edit_pulse()
{
	g_edit_active.store(false, std::memory_order_release);
	std::lock_guard lock(g_mutex);
	g_edit.active = false;
	g_edit.restore_attempted = true;
	g_edit.restore_succeeded = write_edit_bytes_locked(g_edit.original, true);
	if (!g_edit.restore_succeeded && g_edit.error == 0)
		g_edit.error = 3;
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
	std::memcpy(out.data(), static_cast<const uint8_t *>(info.map_ptr) + relative,
		static_cast<size_t>(count));
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
	if (it->id == g_edit.candidate_id && it->bytes.size() == g_edit.injected.size())
	{
		if (g_edit.active && std::memcmp(it->bytes.data(), g_edit.injected.data(), it->bytes.size()) == 0)
			++g_edit.present_readbacks;
		else if (g_edit.restore_attempted && std::memcmp(it->bytes.data(), g_edit.injected.data(), it->bytes.size()) != 0)
			g_edit.overwrite_observed = true;
	}
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
		<< "HD2 Palette Probe 0.3  |  READ + BOUNDED EDIT TEST  |  D3D12\n"
		<< "Anonymous matrix runs only; vertex ownership and bone names are not decoded.\n\n";

	std::lock_guard lock(g_mutex);
	size_t mapped = 0;
	for (const auto &[unused, info] : g_buffers)
		mapped += info.map_ptr != nullptr;
	out << "frame " << g_frame << "  draws " << draws
		<< "  buffers " << g_buffers.size() << " (mapped " << mapped << ")"
		<< "  swept " << std::fixed << std::setprecision(1)
		<< (static_cast<double>(g_scanned_bytes) / (1024.0 * 1024.0)) << " MiB"
		<< "  candidates " << g_candidates.size()
		<< (g_paused ? "  [PAUSED]\n" : "\n")
		<< "F6 next candidate   F7 next 16 slots   F8 pause   F9 rescan\n"
		<< "edit: " << (g_edit.active ? "ACTIVE" : g_edit.completed ? "complete" : g_edit.requested ? "armed" : "off")
		<< "  round " << g_edit.round << '/' << g_edit.total_rounds
		<< "  target " << g_edit.candidate_id << " (all " << g_edit.slots_modified << " slots)"
		<< "  writes " << g_edit.write_successes << '/' << g_edit.write_attempts
		<< "  readback " << g_edit.present_readbacks
		<< "  restored " << (g_edit.restore_succeeded ? "yes" : "no") << "\n\n";

	if (g_candidates.empty())
	{
		out << "Scanning CPU-visible buffers for 8+ consecutive transform-shaped entries...\n";
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
	g_edit_queue.clear();
	g_edit_queue_index = 0;
	g_edit_active.store(false, std::memory_order_release);
	g_edit.active = false;
	g_device = nullptr;
}

void on_init_resource(device *device, const resource_desc &desc, const subresource_data *,
	resource_usage, resource resource)
{
	if (device != g_device || desc.type != resource_type::buffer || desc.buffer.size < k_min_buffer_bytes)
		return;
	std::lock_guard lock(g_mutex);
	g_buffers[resource.handle] = buffer_info { desc.buffer.size };
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
		if (g_edit.resource == resource.handle)
		{
			g_edit.active = false;
			g_edit.error = 4;
			g_edit_active.store(false, std::memory_order_release);
		}
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

bool on_draw_indexed(command_list *, uint32_t, uint32_t, uint32_t, int32_t, uint32_t)
{
	g_draws.fetch_add(1, std::memory_order_relaxed);
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
	draw_console(draws);
	write_telemetry(draws);
}
}

extern "C"
{
__declspec(dllexport) const char *NAME = "HD2 Palette Probe";
__declspec(dllexport) const char *DESCRIPTION =
	"D3D12 transform scanner with a bounded, reversible anonymous-slot edit probe.";
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
		g_edit_thread_stop.store(true, std::memory_order_release);
		release_automation_keys();
		if (reserved != nullptr)
			return TRUE;
		if (g_edit_thread != nullptr)
		{
			WaitForSingleObject(g_edit_thread, 1000);
			CloseHandle(g_edit_thread);
			g_edit_thread = nullptr;
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
