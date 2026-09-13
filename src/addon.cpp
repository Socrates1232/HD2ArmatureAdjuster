#include <reshade.hpp>

#include "matrix_scan.hpp"

#include <Windows.h>

#include <algorithm>
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

struct buffer_info
{
	device *owner = nullptr;
	uint64_t size = 0;
	memory_heap heap = memory_heap::unknown;
	void *map_ptr = nullptr;
	uint64_t map_offset = 0;
	uint64_t map_size = 0;
	uint64_t scan_offset = 0;
	bool mapped_by_probe = false;
	bool ever_mapped_by_game = false;
	bool map_failed = false;
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
std::chrono::steady_clock::time_point g_last_display;
std::chrono::steady_clock::time_point g_last_telemetry;
std::atomic<uint32_t> g_draws { 0 };
std::atomic<uint32_t> g_last_draws { 0 };
thread_local bool g_self_map = false;
std::wstring g_telemetry_path;
std::wstring g_automation_path;
std::wstring g_capture_directory;
uint64_t g_session_id = 0;
uint32_t g_automation_stage = 0;
uint32_t g_automation_delay_ms = 20000;
uint32_t g_capture_count = 0;
uint32_t g_automation_error = 0;
bool g_skip_intro = false;
bool g_automation_input_started = false;
bool g_automation_completed = false;
bool g_escape_held = false;
bool g_w_held = false;
bool g_d_held = false;
std::chrono::steady_clock::time_point g_automation_start;
std::chrono::steady_clock::time_point g_automation_deadline;

bool heap_is_cpu_visible(memory_heap heap)
{
	return heap == memory_heap::cpu_to_gpu || heap == memory_heap::cpu_only;
}

void translation_of(const candidate &c, uint32_t slot, float &x, float &y, float &z);

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
	DWORD process_id = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &process_id);
	return process_id == GetCurrentProcessId();
}

bool focus_runtime_window(effect_runtime *runtime)
{
	if (game_has_focus())
		return true;
	HWND window = static_cast<HWND>(runtime->get_hwnd());
	return window != nullptr && SetForegroundWindow(window) && game_has_focus();
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
	if (g_escape_held)
		send_key(VK_ESCAPE, false);
	if (g_w_held)
		send_key('W', false);
	if (g_d_held)
		send_key('D', false);
	g_escape_held = false;
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
	unsigned delay_ms = 20000, skip_intro = 0;
	if (!ok || sscanf_s(text, "%u %u", &delay_ms, &skip_intro) < 1)
		return false;
	g_automation_delay_ms = std::min(delay_ms, 120000u);
	g_skip_intro = skip_intro != 0;
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

void on_reshade_present(effect_runtime *runtime)
{
	if (runtime->get_device() != g_device)
		return;
	const auto now = std::chrono::steady_clock::now();
	if (g_automation_stage == 0 && read_automation_request())
		g_automation_stage = 1;

	if ((g_escape_held || g_w_held || g_d_held) && !game_has_focus())
	{
		fail_automation(2);
		return;
	}

	if (g_automation_stage == 1)
	{
		if (!focus_runtime_window(runtime))
			return;
		if (g_skip_intro)
		{
			g_automation_deadline = now + std::chrono::seconds(10);
			g_automation_stage = 2;
		}
		else
		{
			if (!capture_frame(runtime, L"capture-start.bmp"))
				g_automation_error = 3;
			g_automation_start = now;
			g_automation_deadline = now + std::chrono::milliseconds(g_automation_delay_ms);
			g_automation_stage = 3;
		}
	}
	else if (g_automation_stage == 2 && now >= g_automation_deadline)
	{
		if (!g_escape_held)
		{
			if (!game_has_focus() || !send_key(VK_ESCAPE, true))
			{
				fail_automation(1);
				return;
			}
			g_escape_held = true;
			g_automation_deadline = now + std::chrono::seconds(4);
		}
		else
		{
			send_key(VK_ESCAPE, false);
			g_escape_held = false;
			if (!capture_frame(runtime, L"capture-start.bmp"))
				g_automation_error = 3;
			g_automation_start = now;
			g_automation_deadline = now + std::chrono::milliseconds(g_automation_delay_ms);
			g_automation_stage = 3;
		}
	}
	else if (g_automation_stage == 3 && now >= g_automation_deadline &&
		g_last_draws.load(std::memory_order_relaxed) >= 20)
	{
		if (!game_has_focus())
			return;
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
		if (!game_has_focus())
			return;
		if (!send_key('B', true) || !send_key('B', false))
		{
			fail_automation(1);
			return;
		}
		g_automation_deadline = now + std::chrono::seconds(4);
		g_automation_stage = 7;
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
	std::vector<slot_sample> top_slots;
	{
		std::lock_guard lock(g_mutex);
		buffer_count = g_buffers.size();
		candidate_count = g_candidates.size();
		paused = g_paused;
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
		<< "  \"schema\": 1,\n"
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
		<< "  \"automation_input_started\": " << (g_automation_input_started ? "true" : "false") << ",\n"
		<< "  \"automation_completed\": " << (g_automation_completed ? "true" : "false") << ",\n"
		<< "  \"automation_error\": " << g_automation_error << ",\n"
		<< "  \"captures\": " << g_capture_count << ",\n"
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
	if (g_console != INVALID_HANDLE_VALUE || !AllocConsole())
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

size_t copy_buffer(uint64_t handle, uint64_t offset, uint64_t wanted, std::vector<uint8_t> &out)
{
	for (int attempt = 0; attempt != 2; ++attempt)
	{
		device *owner = nullptr;
		{
			std::lock_guard lock(g_mutex);
			auto it = g_buffers.find(handle);
			if (it == g_buffers.end() || offset >= it->second.size)
				return 0;

			buffer_info &info = it->second;
			const uint64_t count = std::min(wanted, info.size - offset);
			if (info.map_ptr != nullptr && offset >= info.map_offset)
			{
				const uint64_t relative = offset - info.map_offset;
				if (relative <= info.map_size && count <= info.map_size - relative)
				{
					out.resize(static_cast<size_t>(count));
					std::memcpy(out.data(), static_cast<const uint8_t *>(info.map_ptr) + relative,
						static_cast<size_t>(count));
					return out.size();
				}
			}

			if (attempt != 0 || info.map_failed ||
				(!heap_is_cpu_visible(info.heap) && !info.ever_mapped_by_game))
				return 0;
			owner = info.owner;
		}

		void *pointer = nullptr;
		g_self_map = true;
		const bool mapped = owner != nullptr && owner->map_buffer_region(
			resource { handle }, 0, UINT64_MAX, map_access::read_only, &pointer);
		g_self_map = false;

		bool release_extra_map = false;
		{
			std::lock_guard lock(g_mutex);
			auto it = g_buffers.find(handle);
			if (it == g_buffers.end())
				release_extra_map = mapped && pointer != nullptr;
			else if (!mapped || pointer == nullptr)
				it->second.map_failed = true;
			else if (it->second.map_ptr == nullptr)
			{
				it->second.map_ptr = pointer;
				it->second.map_offset = 0;
				it->second.map_size = it->second.size;
				it->second.mapped_by_probe = true;
			}
			else
				release_extra_map = true;
		}

		if (release_extra_map)
		{
			g_self_map = true;
			owner->unmap_buffer_region(resource { handle });
			g_self_map = false;
		}
	}
	return 0;
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
			if (info.map_ptr == nullptr && !heap_is_cpu_visible(info.heap) && !info.ever_mapped_by_game)
			{
				++g_buffer_cursor;
				continue;
			}

			offset = info.scan_offset;
			count = std::min(k_scan_bytes, info.size - offset);
			const uint64_t advance = k_scan_bytes - k_scan_overlap;
			if (offset + count >= info.size || count <= k_scan_overlap)
			{
				info.scan_offset = 0;
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
		<< "HD2 Palette Probe 0.2  |  BUFFER READ ONLY  |  D3D12\n"
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
		<< "F6 next candidate   F7 next 16 slots   F8 pause   F9 rescan\n\n";

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
	g_device = nullptr;
}

void on_init_resource(device *device, const resource_desc &desc, const subresource_data *,
	resource_usage, resource resource)
{
	if (device != g_device || desc.type != resource_type::buffer || desc.buffer.size < k_min_buffer_bytes)
		return;
	std::lock_guard lock(g_mutex);
	g_buffers[resource.handle] = buffer_info { device, desc.buffer.size, desc.heap };
	g_buffer_order.push_back(resource.handle);
}

void on_destroy_resource(device *device, resource resource)
{
	if (device != g_device)
		return;
	bool mapped_by_probe = false;
	{
		std::lock_guard lock(g_mutex);
		auto it = g_buffers.find(resource.handle);
		if (it == g_buffers.end())
			return;
		mapped_by_probe = it->second.mapped_by_probe;
		g_buffers.erase(it);
		g_candidates.erase(std::remove_if(g_candidates.begin(), g_candidates.end(),
			[resource](const candidate &c) { return c.resource == resource.handle; }), g_candidates.end());
		if (g_selected >= g_candidates.size())
			g_selected = 0;
	}
	if (mapped_by_probe)
	{
		g_self_map = true;
		device->unmap_buffer_region(resource);
		g_self_map = false;
	}
}

void on_map_buffer(device *device, resource resource, uint64_t offset, uint64_t size,
	map_access, void **data)
{
	if (g_self_map || device != g_device || data == nullptr || *data == nullptr)
		return;
	std::lock_guard lock(g_mutex);
	auto it = g_buffers.find(resource.handle);
	if (it == g_buffers.end() || it->second.mapped_by_probe || offset >= it->second.size)
		return;
	buffer_info &info = it->second;
	info.map_ptr = *data;
	info.map_offset = offset;
	info.map_size = (size == 0 || size == UINT64_MAX) ? info.size - offset : std::min(size, info.size - offset);
	info.ever_mapped_by_game = true;
}

void on_unmap_buffer(device *device, resource resource)
{
	if (g_self_map || device != g_device)
		return;
	std::lock_guard lock(g_mutex);
	auto it = g_buffers.find(resource.handle);
	if (it != g_buffers.end() && !it->second.mapped_by_probe)
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
	"Read-only D3D12 scanner that displays anonymous, animated transform slots in a standalone console.";
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
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
		release_automation_keys();
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
			FreeConsole();
		}
	}
	return TRUE;
}
