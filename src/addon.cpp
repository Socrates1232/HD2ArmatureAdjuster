#include <reshade.hpp>

#include "ib_layout.hpp"
#include "instance_lifecycle.hpp"
#include "profile_scan.hpp"
#include "retarget/custom_intent.hpp"
#include "retarget/custom_runtime.hpp"
#include "retarget/sha256.hpp"
#include "runtime_profile.hpp"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>

using namespace reshade;
using namespace reshade::api;

namespace
{
constexpr char k_runtime_version[] = "1.7";
constexpr size_t k_scan_chunk_bytes = 4 * 1024 * 1024;
constexpr size_t k_custom_capture_bytes = 256 * 1024;
constexpr size_t k_custom_scan_chunk_bytes = 16 * 1024;
constexpr size_t k_custom_scan_overlap_bytes = 8 * 1024;
constexpr size_t k_candidate_region_bytes = 64 * 1024;
constexpr size_t k_max_hits = 256;
constexpr size_t k_max_instances = 1024;
constexpr uint32_t k_max_profile_tables = 4096;
constexpr float k_max_edit_translation = 10.0f;
constexpr uint32_t k_stale_frames_before_retire = 3;

enum class scan_reason : uint32_t
{
	initial,
	manual,
	enable,
	priority_refresh,
	resource_change,
	stale_instance,
};

struct buffer_info
{
	uint64_t size = 0;
	void *map_ptr = nullptr;
	uint64_t map_offset = 0;
	uint64_t map_size = 0;
	uint64_t scan_cursor = 0;
	uint64_t capture_cursor = 0;
	uint64_t scan_round = 0;
	uint64_t scan_map_offset = UINT64_MAX;
	uint64_t scan_map_size = 0;
};

struct loaded_table_profile
{
	std::string patch_name;
	std::string profile_file;
	uint64_t unit_id = 0;
	uint64_t table_key = 0;
	uint32_t lod_mask = 0;
	uint32_t entries = 0;
	uint32_t first_lod = 0;
	uint32_t source_records = 1;
	std::vector<uint8_t> t48;
	std::string profile_sha256;
	std::string table_sha256;
};

struct active_target
{
	uintptr_t address = 0;
	size_t profile_index = 0;
	std::vector<uint8_t> expected;
	std::vector<uint32_t> slots;
	uint32_t stale_frames = 0;
};

struct edit_request
{
	uint64_t unit_id = 0;
	uint64_t table_key = 0; // zero keeps legacy unit-wide targeting
	uint32_t slot = UINT32_MAX;
	std::array<float, 3> world_translation {};
};

struct edit_state
{
	bool requested = false;
	bool active = false;
	bool completed = false;
	uint32_t slot = UINT32_MAX;
	uint64_t write_attempts = 0;
	uint64_t write_successes = 0;
	uint64_t immediate_readbacks = 0;
	uint64_t present_readbacks = 0;
	uint64_t refills_observed = 0;
	uint64_t stale_skips = 0;
	uint32_t targets_selected = 0;
	uint32_t targets_written = 0;
	uint32_t targets_restored = 0;
	bool restore_attempted = false;
	bool restore_succeeded = false;
	uint32_t error = 0;
};

struct resource_sample
{
	double process_cpu_percent = 0.0;
	uint64_t process_working_set_bytes = 0;
	uint64_t process_private_bytes = 0;
	uint64_t interval_us = 0;
	double scan_cpu_core_percent = 0.0;
	double maintenance_wall_percent = 0.0;
	double rebind_wall_percent = 0.0;
	double custom_scan_wall_percent = 0.0;
};

struct custom_runtime_view
{
	bool configured = false;
	bool desired = false;
	hd2aa::retarget::custom_status status = hd2aa::retarget::custom_status::disabled;
	hd2aa::retarget::custom_metrics metrics;
	std::string rig_id;
};

std::mutex g_mutex;
std::mutex g_custom_view_mutex;
device *g_device = nullptr;
HMODULE g_addon_module = nullptr;
std::unordered_map<uint64_t, buffer_info> g_buffers;
std::vector<loaded_table_profile> g_profiles;
std::vector<std::string> g_profile_errors;
std::vector<instance_lifecycle::instance> g_hits;
std::vector<active_target> g_active_targets;
std::wstring g_profile_directory;
std::wstring g_rig_directory;
std::wstring g_active_rig_file;
std::wstring g_source_reference_file;
std::string g_custom_error;
hd2aa::retarget::custom_runtime g_custom_runtime;
custom_runtime_view g_custom_view;
hd2aa::retarget::custom_intent_mailbox g_custom_intent;
std::vector<uint8_t> g_custom_scan_buffer;
std::vector<uint8_t> g_custom_pending_scan_buffer;
std::vector<uint8_t> g_custom_active_scan_buffer;
uint64_t g_custom_pending_resource = 0;
uint64_t g_custom_pending_offset = 0;
uint64_t g_custom_pending_intent_revision = 0;
uint64_t g_custom_active_resource = 0;
uint64_t g_custom_active_offset = 0;
uint64_t g_custom_active_intent_revision = 0;
size_t g_custom_active_cursor = 0;
uint64_t g_custom_scan_resource = 0;
std::atomic<bool> g_custom_capture_enabled { false };
std::atomic<bool> g_custom_worker_stop { false };
std::atomic<bool> g_custom_worker_busy { false };
std::atomic<uint64_t> g_custom_worker_intent_revision { 0 };
std::atomic<uint64_t> g_custom_worker_cycles { 0 };
std::atomic<uint64_t> g_custom_worker_total_wall_us { 0 };
std::atomic<uint64_t> g_custom_worker_last_wall_us { 0 };
std::atomic<uint64_t> g_custom_worker_max_wall_us { 0 };
HANDLE g_custom_worker_thread = nullptr;
std::atomic<HANDLE> g_custom_worker_wake { nullptr };
uint32_t g_profile_files = 0;
uint32_t g_profile_records = 0;
uint32_t g_profile_duplicates = 0;

std::atomic<uint32_t> g_hunt_phase { 0 }; // 0 idle, 1 scanning, 2 found, 3 exhausted/error
std::atomic<uint32_t> g_hunt_passes { 0 };
std::atomic<uint64_t> g_hunt_bytes { 0 };
std::atomic<uint64_t> g_partial_candidates { 0 };
std::atomic<uint32_t> g_best_partial_entries { 0 };
std::atomic<bool> g_hunt_stop { false };
std::atomic<bool> g_hunt_again { false };
std::atomic<bool> g_hunt_full_next { true };
std::atomic<uint32_t> g_hunt_reason_next { static_cast<uint32_t>(scan_reason::initial) };
std::atomic<uint32_t> g_hunt_reason_current { static_cast<uint32_t>(scan_reason::initial) };
std::atomic<bool> g_hunt_full_current { true };
std::atomic<uint64_t> g_discovery_generation { 0 };
std::atomic<uint32_t> g_last_scan_hits { 0 };
std::atomic<uint32_t> g_last_scan_new_instances { 0 };
std::atomic<uint64_t> g_scan_requests { 0 };
std::atomic<uint64_t> g_scan_requests_coalesced { 0 };
std::atomic<uint64_t> g_scan_runs { 0 };
std::atomic<uint64_t> g_scan_full_runs { 0 };
std::atomic<uint64_t> g_scan_priority_runs { 0 };
std::atomic<uint64_t> g_scan_total_bytes { 0 };
std::atomic<uint64_t> g_scan_total_wall_us { 0 };
std::atomic<uint64_t> g_scan_total_cpu_us { 0 };
std::atomic<uint64_t> g_scan_last_bytes { 0 };
std::atomic<uint64_t> g_scan_last_wall_us { 0 };
std::atomic<uint64_t> g_scan_last_cpu_us { 0 };
std::atomic<uint64_t> g_scan_max_wall_us { 0 };
HANDLE g_hunt_thread = nullptr;
std::chrono::steady_clock::time_point g_hunt_deadline;
std::chrono::steady_clock::time_point g_hunt_not_before;

HANDLE g_console = INVALID_HANDLE_VALUE;
std::atomic<uint64_t> g_frame { 0 };
std::atomic<uint32_t> g_draws { 0 };
std::atomic<uint32_t> g_last_draws { 0 };
std::chrono::steady_clock::time_point g_last_console;
std::chrono::steady_clock::time_point g_last_telemetry;

std::atomic<uint64_t> g_maintenance_calls { 0 };
std::atomic<uint64_t> g_maintenance_table_checks { 0 };
std::atomic<uint64_t> g_maintenance_read_bytes { 0 };
std::atomic<uint64_t> g_maintenance_total_wall_us { 0 };
std::atomic<uint64_t> g_maintenance_last_wall_us { 0 };
std::atomic<uint64_t> g_maintenance_max_wall_us { 0 };
std::atomic<uint64_t> g_rebind_calls { 0 };
std::atomic<uint64_t> g_rebind_candidates { 0 };
std::atomic<uint64_t> g_rebind_instances_added_metric { 0 };
std::atomic<uint64_t> g_rebind_total_wall_us { 0 };
std::atomic<uint64_t> g_rebind_last_wall_us { 0 };
std::atomic<uint64_t> g_rebind_max_wall_us { 0 };
std::atomic<uint64_t> g_custom_scan_calls { 0 };
std::atomic<uint64_t> g_custom_scan_total_wall_us { 0 };
std::atomic<uint64_t> g_custom_scan_last_wall_us { 0 };
std::atomic<uint64_t> g_custom_scan_max_wall_us { 0 };
std::atomic<uint64_t> g_custom_callback_scan_last_tick { 0 };
uint64_t g_monitor_started_tick_ms = 0;
uint64_t g_previous_sample_tick_ms = 0;
uint64_t g_previous_process_cpu_100ns = 0;
uint64_t g_previous_scan_cpu_us = 0;
uint64_t g_previous_maintenance_wall_us = 0;
uint64_t g_previous_rebind_wall_us = 0;
uint64_t g_previous_custom_scan_wall_us = 0;
resource_sample g_resource_sample;

std::wstring g_telemetry_path;
std::wstring g_resource_log_path;
std::wstring g_automation_path;
uint64_t g_session_id = 0;
HWND g_game_window = nullptr;
uint32_t g_automation_stage = 0;
uint32_t g_automation_delay_ms = 20000;
uint32_t g_automation_error = 0;
uint32_t g_intro_attempts = 0;
uint32_t g_capture_count = 0;
bool g_skip_intro = false;
bool g_steam_capture = false;
bool g_input_started = false;
bool g_automation_completed = false;
bool g_focus_click_attempted = false;
bool g_w_held = false;
bool g_d_held = false;
bool g_b_held = false;
bool g_screenshot_key_held = false;
std::chrono::steady_clock::time_point g_screenshot_release;
std::chrono::steady_clock::time_point g_automation_deadline;
std::atomic<bool> g_edit_requested { false };
std::atomic<bool> g_edit_active { false };
edit_request g_requested_edit;
edit_state g_edit;
std::vector<edit_request> g_shoulder_targets;
std::string g_shoulder_error;
std::atomic<bool> g_shoulder_desired { false };
std::atomic<bool> g_shoulder_active { false };
uint64_t g_shoulder_toggles = 0;
uint64_t g_shoulder_intent_revision = 0;
uint64_t g_shoulder_applied_generation = 0;
uint64_t g_shoulder_instances_added = 0;
uint64_t g_shoulder_instances_retired = 0;
std::atomic<uint64_t> g_resource_generation { 0 };
std::atomic<uint64_t> g_last_resource_tick { 0 };
uint64_t g_observed_resource_generation = 0;
std::chrono::steady_clock::time_point g_next_priority_scan;
std::chrono::steady_clock::time_point g_next_full_scan;

custom_runtime_view read_custom_view()
{
	std::lock_guard lock(g_custom_view_mutex);
	return g_custom_view;
}

void publish_custom_view()
{
	custom_runtime_view view;
	view.configured = g_custom_runtime.configured();
	view.desired = g_custom_runtime.desired();
	view.status = g_custom_runtime.status();
	view.metrics = g_custom_runtime.metrics();
	if (view.configured)
		view.rig_id = g_custom_runtime.rig().rig_id;
	std::lock_guard lock(g_custom_view_mutex);
	g_custom_view = std::move(view);
}

void wake_custom_worker()
{
	if (const HANDLE event = g_custom_worker_wake.load(std::memory_order_acquire))
		SetEvent(event);
}

const char *experiment_mode_name(const custom_runtime_view &custom)
{
	if (custom.configured)
		return g_custom_intent.load().desired ? "custom_armature_retarget" : "custom_armature_ready";
	if (g_shoulder_desired.load(std::memory_order_relaxed))
		return "shoulder_narrow_toggle";
	return g_edit_requested.load(std::memory_order_relaxed) ?
		"converted_ib_edit" : "converted_ib_scan";
}

std::string utf8(const wchar_t *text)
{
	if (text == nullptr || *text == L'\0')
		return {};
	const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
	if (size <= 1)
		return {};
	std::string result(static_cast<size_t>(size), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
	result.pop_back();
	return result;
}

std::wstring wide(const std::string &text)
{
	if (text.empty())
		return {};
	const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
		static_cast<int>(text.size()), nullptr, 0);
	if (size <= 0)
		return {};
	std::wstring result(static_cast<size_t>(size), L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
		static_cast<int>(text.size()), result.data(), size);
	return result;
}

std::string json_escape(const std::string &value)
{
	std::string result;
	for (const unsigned char character : value)
	{
		switch (character)
		{
		case '\\': result += "\\\\"; break;
		case '"': result += "\\\""; break;
		case '\n': result += "\\n"; break;
		case '\r': result += "\\r"; break;
		case '\t': result += "\\t"; break;
		default:
			if (character >= 0x20)
				result.push_back(static_cast<char>(character));
			break;
		}
	}
	return result;
}

bool read_file(const std::wstring &path, std::vector<uint8_t> &bytes)
{
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	LARGE_INTEGER length = {};
	const bool valid_size = GetFileSizeEx(file, &length) != FALSE && length.QuadPart > 0 &&
		length.QuadPart <= 128ll * 1024 * 1024;
	if (!valid_size)
	{
		CloseHandle(file);
		return false;
	}
	bytes.resize(static_cast<size_t>(length.QuadPart));
	DWORD read = 0;
	const bool complete = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != FALSE &&
		read == static_cast<DWORD>(bytes.size());
	CloseHandle(file);
	return complete;
}

void load_profiles()
{
	g_profiles.clear();
	g_profile_errors.clear();
	g_profile_files = 0;
	g_profile_records = 0;
	g_profile_duplicates = 0;
	wchar_t module_path[32768] = {};
	const DWORD length = GetModuleFileNameW(g_addon_module, module_path,
		static_cast<DWORD>(sizeof(module_path) / sizeof(module_path[0])));
	if (length == 0 || length >= sizeof(module_path) / sizeof(module_path[0]))
	{
		g_profile_errors.push_back("could not resolve the add-on directory");
		return;
	}
	std::wstring directory(module_path, length);
	const size_t separator = directory.find_last_of(L"\\/");
	if (separator == std::wstring::npos)
	{
		g_profile_errors.push_back("could not resolve the add-on directory");
		return;
	}
	g_profile_directory = directory.substr(0, separator) + L"\\HD2ArmatureProfiles";
	std::vector<std::wstring> names;
	std::vector<uint8_t> active_bytes;
	if (read_file(g_profile_directory + L"\\active_profiles.txt", active_bytes))
	{
		std::istringstream lines(std::string(active_bytes.begin(), active_bytes.end()));
		std::string line;
		while (std::getline(lines, line))
		{
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (line.empty())
				continue;
			if (line.find('/') != std::string::npos || line.find('\\') != std::string::npos ||
				line.size() < 11 || line.substr(line.size() - 11) != ".hd2profile")
			{
				g_profile_errors.push_back("active_profiles.txt contains an invalid filename");
				return;
			}
			const std::wstring converted = wide(line);
			if (converted.empty())
			{
				g_profile_errors.push_back("active_profiles.txt is not valid UTF-8");
				return;
			}
			names.push_back(converted);
		}
	}
	else
	{
		WIN32_FIND_DATAW found = {};
		HANDLE search = FindFirstFileW((g_profile_directory + L"\\*.hd2profile").c_str(), &found);
		if (search != INVALID_HANDLE_VALUE)
		{
			do
			{
				if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
					names.emplace_back(found.cFileName);
			} while (FindNextFileW(search, &found) != FALSE);
			FindClose(search);
		}
	}
	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());
	if (names.empty())
	{
		g_profile_errors.push_back("no active .hd2profile files found beside the add-on");
		return;
	}

	for (const std::wstring &name : names)
	{
		std::vector<uint8_t> bytes;
		if (!read_file(g_profile_directory + L"\\" + name, bytes))
		{
			g_profile_errors.push_back(utf8(name.c_str()) + ": could not read profile");
			continue;
		}
		const std::string profile_sha256 = hd2aa::sha256_hex(bytes.data(), bytes.size());
		armature_profile::package package;
		std::string error;
		if (!armature_profile::parse(bytes.data(), bytes.size(), package, error))
		{
			g_profile_errors.push_back(utf8(name.c_str()) + ": " + error);
			continue;
		}
		if (package.patch_name.find('/') != std::string::npos ||
			package.patch_name.find('\\') != std::string::npos)
		{
			g_profile_errors.push_back(utf8(name.c_str()) + ": patch name is not a basename");
			continue;
		}
		for (armature_profile::table &table : package.tables)
		{
			++g_profile_records;
			auto duplicate = std::find_if(g_profiles.begin(), g_profiles.end(),
				[&table](const loaded_table_profile &loaded) {
					return armature_profile::same_runtime_table(loaded.unit_id, loaded.entries,
						loaded.t48, table);
				});
			if (duplicate != g_profiles.end())
			{
				duplicate->lod_mask |= table.lod_mask;
				duplicate->first_lod = std::min(duplicate->first_lod, table.first_lod);
				++duplicate->source_records;
				++g_profile_duplicates;
				continue;
			}
			if (g_profiles.size() >= k_max_profile_tables)
			{
				g_profile_errors.push_back("runtime table-profile limit reached");
				break;
			}
			const std::string table_sha256 = hd2aa::sha256_hex(table.t48.data(), table.t48.size());
			g_profiles.push_back({ package.patch_name, utf8(name.c_str()), table.unit_id,
				table.table_fnv1a, table.lod_mask, table.entries, table.first_lod, 1,
				std::move(table.t48), profile_sha256, table_sha256 });
		}
		++g_profile_files;
	}
}

void load_custom_rig()
{
	g_custom_runtime.clear();
	g_custom_intent.reset();
	publish_custom_view();
	g_custom_error.clear();
	g_active_rig_file.clear();
	g_source_reference_file.clear();
	const size_t separator = g_profile_directory.find_last_of(L"\\/");
	if (separator == std::wstring::npos)
		return;
	g_rig_directory = g_profile_directory.substr(0, separator) + L"\\HD2ArmatureRigs";
	std::vector<uint8_t> active_bytes;
	if (!read_file(g_rig_directory + L"\\active_rig.txt", active_bytes))
		return;
	std::vector<std::string> lines;
	std::istringstream input(std::string(active_bytes.begin(), active_bytes.end()));
	for (std::string line; std::getline(input, line);)
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (!line.empty())
			lines.push_back(line);
	}
	if (lines.size() != 2 || lines[0].find_first_of("/\\") != std::string::npos ||
		lines[1].find_first_of("/\\") != std::string::npos ||
		lines[0].size() < 12 || lines[0].substr(lines[0].size() - 12) != ".hd2rig.json" ||
		lines[1].size() < 15 || lines[1].substr(lines[1].size() - 15) != ".hd2source.json")
	{
		g_custom_error = "active_rig.txt must contain a rig basename and source-reference basename";
		return;
	}
	g_active_rig_file = wide(lines[0]);
	g_source_reference_file = wide(lines[1]);
	if (g_active_rig_file.empty() || g_source_reference_file.empty())
	{
		g_custom_error = "active_rig.txt is not valid UTF-8";
		return;
	}
	std::vector<uint8_t> rig_bytes, source_bytes;
	if (!read_file(g_rig_directory + L"\\" + g_active_rig_file, rig_bytes) ||
		!read_file(g_rig_directory + L"\\" + g_source_reference_file, source_bytes))
	{
		g_custom_error = "active rig or source-reference file is unreadable";
		return;
	}
	hd2aa::retarget::rig_package rig;
	if (!hd2aa::retarget::parse_rig_json(reinterpret_cast<const char *>(rig_bytes.data()),
		rig_bytes.size(), rig, g_custom_error))
		return;
	if (hd2aa::sha256_hex(source_bytes.data(), source_bytes.size()) !=
		rig.source_reference_sha256)
	{
		g_custom_error = "active source-reference hash does not match the rig";
		return;
	}
	std::vector<hd2aa::retarget::runtime_profile_data> profiles;
	profiles.reserve(g_profiles.size());
	for (const loaded_table_profile &profile : g_profiles)
	{
		hd2aa::retarget::runtime_profile_data item;
		item.identity = { profile.unit_id, profile.entries, profile.table_sha256,
			profile.profile_file, profile.profile_sha256 };
		item.pristine = profile.t48;
		profiles.push_back(std::move(item));
	}
	g_custom_runtime.configure(std::move(rig), std::move(profiles), g_custom_error);
	publish_custom_view();
}

void load_shoulder_targets()
{
	g_shoulder_targets.clear();
	g_shoulder_error.clear();
	std::vector<uint8_t> bytes;
	if (!read_file(g_profile_directory + L"\\shoulder_targets.txt", bytes))
	{
		g_shoulder_error = "shoulder_targets.txt is missing or empty";
		return;
	}

	std::istringstream lines(std::string(bytes.begin(), bytes.end()));
	std::string line;
	uint32_t line_number = 0;
	while (std::getline(lines, line))
	{
		++line_number;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (const size_t comment = line.find('#'); comment != std::string::npos)
			line.erase(comment);
		std::istringstream fields(line);
		std::vector<std::string> tokens;
		std::string token;
		while (fields >> token)
			tokens.push_back(token);
		std::string unit_text, table_text;
		edit_request target;
		if (tokens.empty())
			continue;
		if (tokens.size() != 5 && tokens.size() != 6)
		{
			g_shoulder_error = "invalid shoulder target on line " + std::to_string(line_number);
			g_shoulder_targets.clear();
			return;
		}
		unit_text = tokens[0];
		const size_t slot_field = tokens.size() == 6 ? 2 : 1;
		if (tokens.size() == 6)
			table_text = tokens[1];
		const auto parsed = std::from_chars(unit_text.data(), unit_text.data() + unit_text.size(),
			target.unit_id, 16);
		const auto table_parsed = table_text.empty() ? std::from_chars_result {} :
			std::from_chars(table_text.data(), table_text.data() + table_text.size(),
				target.table_key, 16);
		const auto slot_parsed = std::from_chars(tokens[slot_field].data(),
			tokens[slot_field].data() + tokens[slot_field].size(), target.slot, 10);
		bool numeric_ok = true;
		for (size_t axis = 0; axis < 3; ++axis)
		{
			std::istringstream value(tokens[slot_field + 1 + axis]);
			if (!(value >> target.world_translation[axis]) || value.peek() != EOF)
				numeric_ok = false;
		}
		if (unit_text.size() != 16 || parsed.ec != std::errc() ||
			parsed.ptr != unit_text.data() + unit_text.size() ||
			(!table_text.empty() && (table_text.size() != 16 || table_parsed.ec != std::errc() ||
				table_parsed.ptr != table_text.data() + table_text.size() || target.table_key == 0)) ||
			slot_parsed.ec != std::errc() ||
			slot_parsed.ptr != tokens[slot_field].data() + tokens[slot_field].size() || !numeric_ok ||
			target.slot == UINT32_MAX ||
			!std::all_of(target.world_translation.begin(), target.world_translation.end(),
				[](float value) { return std::isfinite(value) && std::fabs(value) <= k_max_edit_translation; }) ||
			std::all_of(target.world_translation.begin(), target.world_translation.end(),
				[](float value) { return value == 0.0f; }))
		{
			g_shoulder_error = "invalid shoulder target on line " + std::to_string(line_number);
			g_shoulder_targets.clear();
			return;
		}
		const bool available = std::any_of(g_profiles.begin(), g_profiles.end(),
			[&target](const loaded_table_profile &profile) {
				return profile.unit_id == target.unit_id &&
					(target.table_key == 0 || profile.table_key == target.table_key) &&
					target.slot < profile.entries;
			});
		const bool duplicate = std::any_of(g_shoulder_targets.begin(), g_shoulder_targets.end(),
			[&target](const edit_request &loaded) {
				return loaded.unit_id == target.unit_id && loaded.table_key == target.table_key &&
					loaded.slot == target.slot;
			});
		if (!available || duplicate)
		{
			g_shoulder_error = "unmatched or duplicate shoulder target on line " +
				std::to_string(line_number);
			g_shoulder_targets.clear();
			return;
		}
		g_shoulder_targets.push_back(target);
	}
	if (g_shoulder_targets.empty())
		g_shoulder_error = "shoulder_targets.txt contains no targets";
}

bool writable_private_page(const MEMORY_BASIC_INFORMATION &info)
{
	if (info.State != MEM_COMMIT || info.Type != MEM_PRIVATE ||
		(info.Protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_WRITECOMBINE)) != 0)
		return false;
	const DWORD access = info.Protect & 0xFF;
	return access == PAGE_READWRITE || access == PAGE_WRITECOPY;
}

const char *scan_reason_name(scan_reason reason)
{
	switch (reason)
	{
	case scan_reason::initial: return "initial";
	case scan_reason::manual: return "manual";
	case scan_reason::enable: return "enable";
	case scan_reason::priority_refresh: return "priority_refresh";
	case scan_reason::resource_change: return "resource_change";
	case scan_reason::stale_instance: return "stale_instance";
	default: return "unknown";
	}
}

uint64_t file_time_100ns(const FILETIME &value)
{
	return (static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}

uint64_t current_thread_cpu_us()
{
	FILETIME created = {}, exited = {}, kernel = {}, user = {};
	if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
		return 0;
	return (file_time_100ns(kernel) + file_time_100ns(user)) / 10;
}

void update_max(std::atomic<uint64_t> &target, uint64_t value)
{
	uint64_t current = target.load(std::memory_order_relaxed);
	while (current < value &&
		!target.compare_exchange_weak(current, value, std::memory_order_relaxed))
	{
	}
}

void record_scan_performance(bool full_scan,
	std::chrono::steady_clock::time_point started, uint64_t cpu_started)
{
	const uint64_t wall_us = static_cast<uint64_t>(std::chrono::duration_cast<
		std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
	const uint64_t cpu_now = current_thread_cpu_us();
	const uint64_t cpu_us = cpu_now >= cpu_started ? cpu_now - cpu_started : 0;
	const uint64_t bytes = g_hunt_bytes.load(std::memory_order_relaxed);
	g_scan_runs.fetch_add(1, std::memory_order_relaxed);
	(full_scan ? g_scan_full_runs : g_scan_priority_runs).fetch_add(1,
		std::memory_order_relaxed);
	g_scan_total_bytes.fetch_add(bytes, std::memory_order_relaxed);
	g_scan_total_wall_us.fetch_add(wall_us, std::memory_order_relaxed);
	g_scan_total_cpu_us.fetch_add(cpu_us, std::memory_order_relaxed);
	g_scan_last_bytes.store(bytes, std::memory_order_relaxed);
	g_scan_last_wall_us.store(wall_us, std::memory_order_relaxed);
	g_scan_last_cpu_us.store(cpu_us, std::memory_order_relaxed);
	update_max(g_scan_max_wall_us, wall_us);
}

void record_maintenance_performance(std::chrono::steady_clock::time_point started,
	uint64_t table_checks, uint64_t read_bytes)
{
	const uint64_t wall_us = static_cast<uint64_t>(std::chrono::duration_cast<
		std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
	g_maintenance_calls.fetch_add(1, std::memory_order_relaxed);
	g_maintenance_table_checks.fetch_add(table_checks, std::memory_order_relaxed);
	g_maintenance_read_bytes.fetch_add(read_bytes, std::memory_order_relaxed);
	g_maintenance_total_wall_us.fetch_add(wall_us, std::memory_order_relaxed);
	g_maintenance_last_wall_us.store(wall_us, std::memory_order_relaxed);
	update_max(g_maintenance_max_wall_us, wall_us);
}

void record_rebind_performance(std::chrono::steady_clock::time_point started,
	uint64_t candidates, uint64_t instances_added)
{
	const uint64_t wall_us = static_cast<uint64_t>(std::chrono::duration_cast<
		std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
	g_rebind_calls.fetch_add(1, std::memory_order_relaxed);
	g_rebind_candidates.fetch_add(candidates, std::memory_order_relaxed);
	g_rebind_instances_added_metric.fetch_add(instances_added, std::memory_order_relaxed);
	g_rebind_total_wall_us.fetch_add(wall_us, std::memory_order_relaxed);
	g_rebind_last_wall_us.store(wall_us, std::memory_order_relaxed);
	update_max(g_rebind_max_wall_us, wall_us);
}

void record_custom_scan_performance(std::chrono::steady_clock::time_point started)
{
	const uint64_t wall_us = static_cast<uint64_t>(std::chrono::duration_cast<
		std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
	g_custom_scan_calls.fetch_add(1, std::memory_order_relaxed);
	g_custom_scan_total_wall_us.fetch_add(wall_us, std::memory_order_relaxed);
	g_custom_scan_last_wall_us.store(wall_us, std::memory_order_relaxed);
	update_max(g_custom_scan_max_wall_us, wall_us);
}

void publish_hunt_results(const std::vector<instance_lifecycle::instance> &found_hits,
	uint64_t partial, uint32_t best_partial)
{
	const uint64_t generation = g_discovery_generation.fetch_add(1,
		std::memory_order_acq_rel) + 1;
	size_t added = 0;
	{
		std::lock_guard lock(g_mutex);
		added = instance_lifecycle::merge(g_hits, found_hits, generation, k_max_instances);
	}
	g_partial_candidates.store(partial, std::memory_order_relaxed);
	g_best_partial_entries.store(best_partial, std::memory_order_relaxed);
	g_hunt_passes.fetch_add(1, std::memory_order_relaxed);
	g_last_scan_hits.store(static_cast<uint32_t>(found_hits.size()), std::memory_order_release);
	g_last_scan_new_instances.store(static_cast<uint32_t>(added), std::memory_order_release);
	if (g_hunt_again.exchange(false, std::memory_order_acq_rel))
		g_hunt_phase.store(0, std::memory_order_release);
	else
		g_hunt_phase.store(found_hits.empty() ? 3u : 2u, std::memory_order_release);
	wake_custom_worker();
}

DWORD WINAPI hunt_thread_proc(void *)
{
	const auto performance_started = std::chrono::steady_clock::now();
	const uint64_t cpu_started = current_thread_cpu_us();
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	std::vector<loaded_table_profile> profiles;
	std::vector<edit_request> shoulder_targets;
	std::vector<instance_lifecycle::instance> known_regions;
	const bool shoulder_only = g_shoulder_desired.load(std::memory_order_acquire);
	const bool full_scan = g_hunt_full_current.load(std::memory_order_acquire);
	{
		std::lock_guard lock(g_mutex);
		profiles = g_profiles;
		if (shoulder_only)
			shoulder_targets = g_shoulder_targets;
		for (const instance_lifecycle::instance &hit : g_hits)
		{
			const bool duplicate = std::any_of(known_regions.begin(), known_regions.end(),
				[&hit](const instance_lifecycle::instance &known) {
					return known.region_base == hit.region_base && known.region_size == hit.region_size;
				});
			if (!duplicate)
				known_regions.push_back(hit);
		}
	}
	if (profiles.empty() || (!full_scan && known_regions.empty()))
	{
		record_scan_performance(full_scan, performance_started, cpu_started);
		publish_hunt_results({}, 0, 0);
		return 0;
	}

	std::vector<armature_probe::profile_view> views;
	std::vector<size_t> profile_indices;
	size_t largest_table = 0;
	for (size_t index = 0; index < profiles.size(); ++index)
	{
		const loaded_table_profile &profile = profiles[index];
		if (shoulder_only && std::none_of(shoulder_targets.begin(), shoulder_targets.end(),
			[&profile](const edit_request &target) {
				return target.unit_id == profile.unit_id &&
					(target.table_key == 0 || target.table_key == profile.table_key) &&
					target.slot < profile.entries;
			}))
			continue;
		views.push_back({ profile.t48.data(), profile.entries });
		profile_indices.push_back(index);
		largest_table = std::max(largest_table, profile.t48.size());
	}
	if (views.empty())
	{
		record_scan_performance(full_scan, performance_started, cpu_started);
		publish_hunt_results({}, 0, 0);
		return 0;
	}

	std::vector<uint8_t> scratch(k_scan_chunk_bytes + largest_table);
	std::vector<instance_lifecycle::instance> found_hits;
	std::vector<armature_probe::address_range> excluded;
	const uintptr_t scratch_begin = reinterpret_cast<uintptr_t>(scratch.data());
	excluded.push_back({ scratch_begin, scratch_begin + scratch.size() });
	for (const loaded_table_profile &profile : profiles)
	{
		const uintptr_t begin = reinterpret_cast<uintptr_t>(profile.t48.data());
		excluded.push_back({ begin, begin + profile.t48.size() });
	}
	if (g_addon_module != nullptr)
	{
		const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(g_addon_module);
		if (dos->e_magic == IMAGE_DOS_SIGNATURE)
		{
			const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(
				reinterpret_cast<const uint8_t *>(g_addon_module) + dos->e_lfanew);
			if (nt->Signature == IMAGE_NT_SIGNATURE)
			{
				const uintptr_t image = reinterpret_cast<uintptr_t>(g_addon_module);
				excluded.push_back({ image, image + nt->OptionalHeader.SizeOfImage });
			}
		}
	}
	{
		std::lock_guard lock(g_mutex);
		for (const loaded_table_profile &profile : g_profiles)
		{
			const uintptr_t begin = reinterpret_cast<uintptr_t>(profile.t48.data());
			excluded.push_back({ begin, begin + profile.t48.size() });
		}
		for (const auto &[unused, buffer] : g_buffers)
			if (buffer.map_ptr != nullptr && buffer.map_size != 0)
			{
				const uintptr_t begin = reinterpret_cast<uintptr_t>(buffer.map_ptr);
				excluded.push_back({ begin, begin + static_cast<size_t>(buffer.map_size) });
			}
	}

	uint64_t partial = 0;
	uint32_t best_partial = 0;
	auto scan_region = [&](uintptr_t base, size_t region_size, DWORD protection) {
		for (size_t offset = 0; offset < region_size && found_hits.size() < k_max_hits &&
			!g_hunt_stop.load(std::memory_order_acquire) &&
			std::chrono::steady_clock::now() < g_hunt_deadline; offset += k_scan_chunk_bytes)
		{
			const size_t wanted = std::min(scratch.size(), region_size - offset);
			SIZE_T got = 0;
			ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(base + offset),
				scratch.data(), wanted, &got);
			g_hunt_bytes.fetch_add(got, std::memory_order_relaxed);
			if (got < armature_profile::transform_stride)
				continue;
			auto scan = armature_probe::find_exact_profiles(scratch.data(), got, base + offset,
				views, excluded, k_max_hits - found_hits.size());
			partial += scan.partial_candidates;
			best_partial = std::max(best_partial, scan.best_partial_entries);
			const uintptr_t primary_end = base + offset +
				std::min(k_scan_chunk_bytes, static_cast<size_t>(got));
			for (const armature_probe::profile_hit &hit : scan.hits)
			{
				if (hit.address >= primary_end)
					continue;
				const size_t profile_index = profile_indices[hit.profile_index];
				const bool duplicate = std::any_of(found_hits.begin(), found_hits.end(),
					[&hit, profile_index](const instance_lifecycle::instance &known) {
						return known.address == hit.address && known.profile_index == profile_index;
					});
				if (!duplicate)
					found_hits.push_back({ hit.address, profile_index, base, region_size, protection, 0 });
			}
		}
	};

	if (full_scan)
	{
		SYSTEM_INFO system = {};
		GetSystemInfo(&system);
		const uintptr_t maximum = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);
		uintptr_t cursor = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
		while (cursor < maximum && found_hits.size() < k_max_hits &&
			!g_hunt_stop.load(std::memory_order_acquire) &&
			std::chrono::steady_clock::now() < g_hunt_deadline)
		{
			MEMORY_BASIC_INFORMATION info = {};
			if (VirtualQuery(reinterpret_cast<const void *>(cursor), &info, sizeof(info)) == 0)
				break;
			const uintptr_t base = reinterpret_cast<uintptr_t>(info.BaseAddress);
			const size_t region_size = info.RegionSize;
			if (writable_private_page(info) && region_size == k_candidate_region_bytes)
				scan_region(base, region_size, info.Protect);
			if (region_size == 0 || base > maximum - region_size)
				break;
			cursor = base + region_size;
		}
	}
	else
	{
		for (const instance_lifecycle::instance &known : known_regions)
		{
			MEMORY_BASIC_INFORMATION info = {};
			if (VirtualQuery(reinterpret_cast<const void *>(known.region_base), &info, sizeof(info)) == 0 ||
				reinterpret_cast<uintptr_t>(info.BaseAddress) != known.region_base ||
				!writable_private_page(info))
				continue;
			scan_region(known.region_base,
				std::min(known.region_size, static_cast<size_t>(info.RegionSize)), info.Protect);
		}
	}

	record_scan_performance(full_scan, performance_started, cpu_started);
	publish_hunt_results(found_hits, partial, best_partial);
	return 0;
}

bool ensure_hunt_thread()
{
	if (g_hunt_phase.load(std::memory_order_acquire) == 1)
		return true;
	{
		std::lock_guard lock(g_mutex);
		if (g_profiles.empty())
		{
			g_hunt_phase.store(3, std::memory_order_release);
			return false;
		}
	}
	if (g_hunt_thread != nullptr)
	{
		if (WaitForSingleObject(g_hunt_thread, 0) != WAIT_OBJECT_0)
			return true;
		CloseHandle(g_hunt_thread);
		g_hunt_thread = nullptr;
	}
	g_hunt_bytes.store(0, std::memory_order_relaxed);
	g_hunt_passes.store(0, std::memory_order_relaxed);
	g_partial_candidates.store(0, std::memory_order_relaxed);
	g_best_partial_entries.store(0, std::memory_order_relaxed);
	g_hunt_stop.store(false, std::memory_order_release);
	g_hunt_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	g_hunt_full_current.store(g_hunt_full_next.exchange(false, std::memory_order_acq_rel),
		std::memory_order_release);
	if (g_hunt_full_current.load(std::memory_order_acquire))
		g_observed_resource_generation = g_resource_generation.load(std::memory_order_acquire);
	g_hunt_reason_current.store(g_hunt_reason_next.load(std::memory_order_acquire),
		std::memory_order_release);
	g_hunt_phase.store(1, std::memory_order_release);
	g_hunt_thread = CreateThread(nullptr, 0, hunt_thread_proc, nullptr, 0, nullptr);
	if (g_hunt_thread == nullptr)
	{
		g_hunt_phase.store(3, std::memory_order_release);
		return false;
	}
	return true;
}

void request_discovery(bool full_scan, scan_reason reason)
{
	g_scan_requests.fetch_add(1, std::memory_order_relaxed);
	if (full_scan)
		g_hunt_full_next.store(true, std::memory_order_release);
	g_hunt_reason_next.store(static_cast<uint32_t>(reason), std::memory_order_release);
	if (g_hunt_phase.load(std::memory_order_acquire) == 1)
	{
		g_scan_requests_coalesced.fetch_add(1, std::memory_order_relaxed);
		g_hunt_again.store(true, std::memory_order_release);
		return;
	}
	g_hunt_phase.store(0, std::memory_order_release);
	g_hunt_not_before = std::chrono::steady_clock::now();
}

void request_rescan()
{
	request_discovery(true, scan_reason::manual);
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

void scan_custom_pose_input(uint64_t frame)
{
	if (!g_custom_runtime.configured() || !g_custom_runtime.desired())
		return;
	const uint64_t current_revision = g_custom_intent.load().revision;
	if (!g_custom_active_scan_buffer.empty() &&
		g_custom_active_intent_revision != current_revision)
	{
		g_custom_active_scan_buffer.clear();
		g_custom_active_cursor = 0;
	}
	if (g_custom_active_scan_buffer.empty())
	{
		{
			std::lock_guard lock(g_mutex);
			g_custom_active_scan_buffer.swap(g_custom_pending_scan_buffer);
			g_custom_active_resource = g_custom_pending_resource;
			g_custom_active_offset = g_custom_pending_offset;
			g_custom_active_intent_revision = g_custom_pending_intent_revision;
			g_custom_active_cursor = 0;
		}
		if (!g_custom_active_scan_buffer.empty() &&
			g_custom_active_intent_revision != current_revision)
		{
			g_custom_active_scan_buffer.clear();
			g_custom_active_cursor = 0;
		}
	}
	if (!g_custom_active_scan_buffer.empty())
	{
		const size_t start = g_custom_active_cursor;
		const size_t size = std::min(k_custom_scan_chunk_bytes + k_custom_scan_overlap_bytes,
			g_custom_active_scan_buffer.size() - start);
		const auto started = std::chrono::steady_clock::now();
		g_custom_runtime.observe_window(g_custom_active_scan_buffer.data() + start, size,
			g_custom_active_resource, g_custom_active_offset + start, frame);
		g_custom_active_cursor = std::min(start + k_custom_scan_chunk_bytes,
			g_custom_active_scan_buffer.size());
		if (g_custom_active_cursor >= g_custom_active_scan_buffer.size())
		{
			g_custom_active_scan_buffer.clear();
			g_custom_active_cursor = 0;
		}
		record_custom_scan_performance(started);
		return;
	}
	struct mapped_window
	{
		uint64_t resource = 0;
		uintptr_t address = 0;
		uint64_t offset = 0;
		size_t size = 0;
	};
	mapped_window selected;
	uint64_t live_resource = 0, live_offset = 0, live_frame = 0;
	const bool recent = g_custom_runtime.latest_pose_location(
		live_resource, live_offset, live_frame) && frame <= live_frame + 3;
	bool using_recent = false;
	{
		std::lock_guard lock(g_mutex);
		auto choose = g_buffers.end();
		if (recent)
			choose = g_buffers.find(live_resource);
		if (choose == g_buffers.end() || choose->second.map_ptr == nullptr ||
			choose->second.map_size < 48)
		{
			choose = g_buffers.find(g_custom_scan_resource);
			if (choose == g_buffers.end() || choose->second.map_ptr == nullptr ||
				choose->second.map_size < 4096)
				choose = g_buffers.end();
		}
		if (choose == g_buffers.end())
			for (auto item = g_buffers.begin(); item != g_buffers.end(); ++item)
				if (item->second.map_ptr != nullptr && item->second.map_size >= 4096 &&
					(choose == g_buffers.end() || item->second.scan_round < choose->second.scan_round ||
					 (item->second.scan_round == choose->second.scan_round &&
					  item->second.map_size > choose->second.map_size)))
					choose = item;
		if (choose == g_buffers.end() || choose->second.map_ptr == nullptr)
			return;
		buffer_info &buffer = choose->second;
		using_recent = recent && choose->first == live_resource;
		uint64_t start = buffer.scan_cursor;
		size_t advance = k_custom_scan_chunk_bytes;
		size_t wanted = advance + k_custom_scan_overlap_bytes;
		if (using_recent && live_offset >= buffer.map_offset)
		{
			const uint64_t relative = live_offset - buffer.map_offset;
			start = relative > k_custom_scan_chunk_bytes / 2 ?
				relative - k_custom_scan_chunk_bytes / 2 : 0;
			wanted = k_custom_scan_chunk_bytes + k_custom_scan_overlap_bytes;
		}
		if (start >= buffer.map_size)
			start = 0;
		wanted = static_cast<size_t>(std::min<uint64_t>(wanted, buffer.map_size - start));
		if (!using_recent)
		{
			if (start + advance >= buffer.map_size)
			{
				buffer.scan_cursor = 0;
				++buffer.scan_round;
				g_custom_scan_resource = 0;
			}
			else
			{
				buffer.scan_cursor = start + advance;
				g_custom_scan_resource = choose->first;
			}
		}
		selected = { choose->first,
			reinterpret_cast<uintptr_t>(buffer.map_ptr) + start,
			buffer.map_offset + start, wanted };
	}
	if (selected.size < 48)
		return;
	const auto started = std::chrono::steady_clock::now();
	g_custom_scan_buffer.resize(selected.size);
	if (!read_process_bytes(selected.address, g_custom_scan_buffer.data(), selected.size))
	{
		record_custom_scan_performance(started);
		return;
	}
	g_custom_runtime.observe_window(g_custom_scan_buffer.data(), g_custom_scan_buffer.size(),
		selected.resource, selected.offset, frame);
	record_custom_scan_performance(started);
}

void queue_custom_pose_input(uint64_t resource)
{
	const hd2aa::retarget::custom_intent_snapshot intent = g_custom_intent.load();
	if (!intent.desired)
		return;
	const uint64_t tick = GetTickCount64();
	uintptr_t address = 0;
	uint64_t absolute_offset = 0;
	size_t size = 0;
	{
		std::lock_guard lock(g_mutex);
		auto found = g_buffers.find(resource);
		if (found == g_buffers.end() || found->second.map_ptr == nullptr ||
			found->second.map_size < k_custom_scan_chunk_bytes + k_custom_scan_overlap_bytes)
			return;
		uint64_t previous = g_custom_callback_scan_last_tick.load(std::memory_order_relaxed);
		if (tick < previous + 32 || !g_custom_callback_scan_last_tick.compare_exchange_strong(
			previous, tick, std::memory_order_relaxed))
			return;
		buffer_info &buffer = found->second;
		uint64_t start = buffer.capture_cursor;
		if (start >= buffer.map_size)
			start = 0;
		size = static_cast<size_t>(std::min<uint64_t>(
			k_custom_capture_bytes + k_custom_scan_overlap_bytes,
			buffer.map_size - start));
		address = reinterpret_cast<uintptr_t>(buffer.map_ptr) + start;
		absolute_offset = buffer.map_offset + start;
		if (start + k_custom_capture_bytes >= buffer.map_size)
			buffer.capture_cursor = 0;
		else
			buffer.capture_cursor = start + k_custom_capture_bytes;
	}
	if (size < 48)
		return;
	const auto started = std::chrono::steady_clock::now();
	std::vector<uint8_t> pending(size);
	if (read_process_bytes(address, pending.data(), pending.size()))
	{
		std::lock_guard lock(g_mutex);
		g_custom_pending_scan_buffer.swap(pending);
		g_custom_pending_resource = resource;
		g_custom_pending_offset = absolute_offset;
		g_custom_pending_intent_revision = intent.revision;
	}
	record_custom_scan_performance(started);
	wake_custom_worker();
}

void service_custom_runtime_state(uint64_t frame)
{
	if (!g_custom_runtime.configured())
		return;
	std::vector<hd2aa::retarget::discovered_bind_instance> instances;
	{
		std::lock_guard lock(g_mutex);
		instances.reserve(g_hits.size());
		for (const instance_lifecycle::instance &hit : g_hits)
			instances.push_back({ hit.address, hit.profile_index, hit.last_seen_generation });
	}
	g_custom_runtime.service(instances,
		g_discovery_generation.load(std::memory_order_acquire), frame,
		[](uintptr_t address, void *data, size_t size) {
			return read_process_bytes(address, data, size);
		},
		[](uintptr_t address, const void *data, size_t size) {
			return write_process_bytes(address, data, size);
		});
}

bool apply_custom_intent(uint64_t &handled_revision)
{
	const hd2aa::retarget::custom_intent_snapshot intent = g_custom_intent.load();
	if (intent.revision == handled_revision)
		return false;
	if (g_custom_runtime.desired() != intent.desired)
		g_custom_runtime.toggle();
	handled_revision = intent.revision;
	g_custom_worker_intent_revision.store(handled_revision, std::memory_order_release);
	publish_custom_view();
	return true;
}

void record_custom_worker_performance(std::chrono::steady_clock::time_point started)
{
	const uint64_t wall_us = static_cast<uint64_t>(std::chrono::duration_cast<
		std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
	g_custom_worker_cycles.fetch_add(1, std::memory_order_relaxed);
	g_custom_worker_total_wall_us.fetch_add(wall_us, std::memory_order_relaxed);
	g_custom_worker_last_wall_us.store(wall_us, std::memory_order_relaxed);
	update_max(g_custom_worker_max_wall_us, wall_us);
}

DWORD WINAPI custom_worker_proc(void *)
{
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	uint64_t handled_revision = 0;
	auto next_scan = std::chrono::steady_clock::now();
	for (;;)
	{
		WaitForSingleObject(g_custom_worker_wake.load(std::memory_order_acquire), 50);
		const bool stopping = g_custom_worker_stop.load(std::memory_order_acquire);
		if (stopping)
			g_custom_intent.request(false);

		const auto started = std::chrono::steady_clock::now();
		g_custom_worker_busy.store(true, std::memory_order_release);
		bool intent_changed = apply_custom_intent(handled_revision);
		const auto intent = g_custom_intent.load();
		if (!intent.desired && !intent_changed && !stopping)
		{
			g_custom_worker_busy.store(false, std::memory_order_release);
			continue;
		}

		const uint64_t frame = g_frame.load(std::memory_order_acquire);
		const auto now = std::chrono::steady_clock::now();
		if (g_custom_runtime.desired() && now >= next_scan)
		{
			scan_custom_pose_input(frame);
			next_scan = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
		}

		intent_changed = apply_custom_intent(handled_revision) || intent_changed;
		if (g_custom_runtime.desired() || intent_changed || stopping)
			service_custom_runtime_state(frame);
		publish_custom_view();
		record_custom_worker_performance(started);
		g_custom_worker_busy.store(false, std::memory_order_release);
		if (stopping)
			break;
	}
	return 0;
}

bool start_custom_worker()
{
	if (!g_custom_runtime.configured() || g_custom_worker_thread != nullptr)
		return g_custom_runtime.configured();
	g_custom_worker_stop.store(false, std::memory_order_release);
	g_custom_worker_busy.store(false, std::memory_order_release);
	g_custom_worker_intent_revision.store(0, std::memory_order_release);
	g_custom_worker_cycles.store(0, std::memory_order_relaxed);
	g_custom_worker_total_wall_us.store(0, std::memory_order_relaxed);
	g_custom_worker_last_wall_us.store(0, std::memory_order_relaxed);
	g_custom_worker_max_wall_us.store(0, std::memory_order_relaxed);
	const HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	g_custom_worker_wake.store(wake, std::memory_order_release);
	if (wake == nullptr)
	{
		g_custom_error = "could not create the custom runtime wake event";
		g_custom_runtime.clear();
		publish_custom_view();
		return false;
	}
	g_custom_worker_thread = CreateThread(nullptr, 0, custom_worker_proc, nullptr, 0, nullptr);
	if (g_custom_worker_thread == nullptr)
	{
		CloseHandle(g_custom_worker_wake.exchange(nullptr, std::memory_order_acq_rel));
		g_custom_error = "could not create the custom runtime worker";
		g_custom_runtime.clear();
		publish_custom_view();
		return false;
	}
	return true;
}

void stop_custom_runtime()
{
	g_custom_capture_enabled.store(false, std::memory_order_release);
	g_custom_intent.request(false);
	g_custom_worker_stop.store(true, std::memory_order_release);
	wake_custom_worker();
	if (g_custom_worker_thread != nullptr)
	{
		WaitForSingleObject(g_custom_worker_thread, INFINITE);
		CloseHandle(g_custom_worker_thread);
		g_custom_worker_thread = nullptr;
	}
	if (const HANDLE wake = g_custom_worker_wake.exchange(nullptr, std::memory_order_acq_rel))
	{
		CloseHandle(wake);
	}
	g_custom_runtime.clear();
	g_custom_intent.reset();
	publish_custom_view();
}

bool has_requested_target()
{
	std::lock_guard lock(g_mutex);
	return std::any_of(g_hits.begin(), g_hits.end(), [](const instance_lifecycle::instance &hit) {
		return hit.profile_index < g_profiles.size() &&
			g_profiles[hit.profile_index].unit_id == g_requested_edit.unit_id &&
			(g_requested_edit.table_key == 0 ||
				g_profiles[hit.profile_index].table_key == g_requested_edit.table_key) &&
			g_requested_edit.slot < g_profiles[hit.profile_index].entries;
	});
}

bool make_expected_table(const loaded_table_profile &profile,
	const std::vector<edit_request> &requests, std::vector<uint8_t> &expected,
	std::vector<uint32_t> &slots)
{
	expected = profile.t48;
	slots.clear();
	for (const edit_request &request : requests)
	{
		if (request.unit_id != profile.unit_id ||
			(request.table_key != 0 && request.table_key != profile.table_key) ||
			request.slot >= profile.entries)
			continue;
		const size_t offset = static_cast<size_t>(request.slot) *
			armature_profile::transform_stride;
		armature_probe::translate_world_t48(profile.t48.data() + offset,
			request.world_translation[0], request.world_translation[1],
			request.world_translation[2], expected.data() + offset);
		if (std::memcmp(profile.t48.data() + offset, expected.data() + offset,
			armature_profile::transform_stride) != 0)
			slots.push_back(request.slot);
	}
	std::sort(slots.begin(), slots.end());
	slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
	return !slots.empty();
}

bool write_table_slots(const loaded_table_profile &profile, active_target &target,
	const std::vector<uint8_t> &source, const std::vector<uint8_t> &destination,
	bool rollback_on_failure = true)
{
	for (uint32_t slot : target.slots)
	{
		const size_t offset = static_cast<size_t>(slot) * armature_profile::transform_stride;
		++g_edit.write_attempts;
		if (!write_process_bytes(target.address + offset, destination.data() + offset,
			armature_profile::transform_stride))
			return false;
		++g_edit.write_successes;
	}
	std::vector<uint8_t> current(profile.t48.size());
	if (read_process_bytes(target.address, current.data(), current.size()) && current == destination)
	{
		g_edit.immediate_readbacks += target.slots.size();
		return true;
	}
	if (rollback_on_failure)
		for (uint32_t slot : target.slots)
		{
			const size_t offset = static_cast<size_t>(slot) * armature_profile::transform_stride;
			write_process_bytes(target.address + offset, source.data() + offset,
				armature_profile::transform_stride);
		}
	return false;
}

size_t active_target_count()
{
	std::lock_guard lock(g_mutex);
	return g_active_targets.size();
}

size_t append_discovered_edits_locked(const std::vector<edit_request> &requests)
{
	const auto performance_started = std::chrono::steady_clock::now();
	const uint64_t candidates = g_hits.size();
	size_t instances_added = 0;
	for (const instance_lifecycle::instance &hit : g_hits)
	{
		if (hit.profile_index >= g_profiles.size())
			continue;
		const bool already_owned = std::any_of(g_active_targets.begin(), g_active_targets.end(),
			[&hit](const active_target &target) {
				return target.address == hit.address && target.profile_index == hit.profile_index;
			});
		if (already_owned)
			continue;
		const loaded_table_profile &profile = g_profiles[hit.profile_index];
		active_target target;
		target.address = hit.address;
		target.profile_index = hit.profile_index;
		if (!make_expected_table(profile, requests, target.expected, target.slots))
			continue;
		g_edit.targets_selected += static_cast<uint32_t>(target.slots.size());
		std::vector<uint8_t> current(profile.t48.size());
		if (!read_process_bytes(target.address, current.data(), current.size()))
		{
			g_edit.stale_skips += target.slots.size();
			continue;
		}
		const instance_lifecycle::table_state state = instance_lifecycle::classify(
			current.data(), profile.t48.data(), target.expected.data(), current.size());
		if (state == instance_lifecycle::table_state::unknown)
		{
			g_edit.stale_skips += target.slots.size();
			continue;
		}
		if (state == instance_lifecycle::table_state::pristine &&
			!write_table_slots(profile, target, profile.t48, target.expected))
			continue;
		g_edit.targets_written += static_cast<uint32_t>(target.slots.size());
		g_active_targets.push_back(std::move(target));
		++instances_added;
	}
	record_rebind_performance(performance_started, candidates, instances_added);
	return instances_added;
}

bool begin_edits(const std::vector<edit_request> &requests)
{
	if (requests.empty() || g_edit_active.load(std::memory_order_acquire))
		return false;
	std::lock_guard lock(g_mutex);
	g_active_targets.clear();
	g_edit = {};
	g_edit.requested = true;
	g_edit.slot = requests.size() == 1 ? requests.front().slot : UINT32_MAX;
	append_discovered_edits_locked(requests);

	g_edit.active = !g_active_targets.empty();
	if (!g_edit.active)
	{
		g_edit.error = 2;
		return false;
	}
	g_edit_active.store(true, std::memory_order_release);
	return true;
}

size_t extend_edits(const std::vector<edit_request> &requests)
{
	if (!g_edit_active.load(std::memory_order_acquire))
		return 0;
	std::lock_guard lock(g_mutex);
	const size_t added = append_discovered_edits_locked(requests);
	g_edit.active = !g_active_targets.empty();
	return added;
}

bool begin_edit(std::chrono::steady_clock::time_point now)
{
	if (!begin_edits({ g_requested_edit }))
		return false;
	g_automation_deadline = now + std::chrono::seconds(1);
	g_automation_stage = 10;
	return true;
}

size_t maintain_edit()
{
	if (!g_edit_active.load(std::memory_order_acquire))
		return 0;
	std::unique_lock lock(g_mutex, std::try_to_lock);
	if (!lock.owns_lock() || !g_edit.active)
		return 0;
	const auto performance_started = std::chrono::steady_clock::now();
	uint64_t table_checks = 0;
	uint64_t read_bytes = 0;
	size_t retired = 0;
	for (auto item = g_active_targets.begin(); item != g_active_targets.end();)
	{
		active_target &target = *item;
		if (target.profile_index >= g_profiles.size())
		{
			item = g_active_targets.erase(item);
			++retired;
			continue;
		}
		const loaded_table_profile &profile = g_profiles[target.profile_index];
		++table_checks;
		read_bytes += profile.t48.size();
		std::vector<uint8_t> current(profile.t48.size());
		if (!read_process_bytes(target.address, current.data(), current.size()))
		{
			g_edit.stale_skips += target.slots.size();
			++target.stale_frames;
		}
		else
		{
			const instance_lifecycle::table_state state = instance_lifecycle::classify(
				current.data(), profile.t48.data(), target.expected.data(), current.size());
			if (state == instance_lifecycle::table_state::our_override)
			{
				target.stale_frames = 0;
				g_edit.present_readbacks += target.slots.size();
			}
			else if (state == instance_lifecycle::table_state::pristine)
			{
				++g_edit.refills_observed;
				if (write_table_slots(profile, target, profile.t48, target.expected))
					target.stale_frames = 0;
				else
					++target.stale_frames;
			}
			else
			{
				g_edit.stale_skips += target.slots.size();
				++target.stale_frames;
			}
		}
		if (target.stale_frames >= k_stale_frames_before_retire)
		{
			item = g_active_targets.erase(item);
			++retired;
		}
		else
			++item;
	}
	g_edit.active = !g_active_targets.empty();
	g_edit_active.store(g_edit.active, std::memory_order_release);
	record_maintenance_performance(performance_started, table_checks, read_bytes);
	return retired;
}

void end_edit()
{
	if (!g_edit_active.exchange(false, std::memory_order_acq_rel))
		return;
	std::lock_guard lock(g_mutex);
	g_edit.active = false;
	bool restored = true;
	for (active_target &target : g_active_targets)
	{
		if (target.profile_index >= g_profiles.size())
		{
			restored = false;
			continue;
		}
		const loaded_table_profile &profile = g_profiles[target.profile_index];
		std::vector<uint8_t> current(profile.t48.size());
		g_edit.restore_attempted = true;
		if (!read_process_bytes(target.address, current.data(), current.size()))
		{
			restored = false;
			continue;
		}
		const instance_lifecycle::table_state state = instance_lifecycle::classify(
			current.data(), profile.t48.data(), target.expected.data(), current.size());
		if (state == instance_lifecycle::table_state::pristine)
		{
			g_edit.targets_restored += static_cast<uint32_t>(target.slots.size());
			continue;
		}
		if (state != instance_lifecycle::table_state::our_override ||
			!write_table_slots(profile, target, target.expected, profile.t48, false) ||
			!read_process_bytes(target.address, current.data(), current.size()) || current != profile.t48)
		{
			restored = false;
			continue;
		}
		g_edit.targets_restored += static_cast<uint32_t>(target.slots.size());
	}
	g_edit.restore_succeeded = restored && g_edit.targets_written != 0 &&
		g_edit.targets_restored == g_edit.targets_written;
	if (!g_edit.restore_succeeded)
		g_edit.error = 3;
	g_active_targets.clear();
}

void toggle_shoulder_edit()
{
	++g_shoulder_toggles;
	++g_shoulder_intent_revision;
	const bool enabled = !g_shoulder_desired.load(std::memory_order_acquire);
	g_shoulder_desired.store(enabled, std::memory_order_release);
	if (!enabled)
	{
		end_edit();
		g_shoulder_active.store(false, std::memory_order_release);
		g_edit.completed = true;
		g_shoulder_error.clear();
		return;
	}
	if (g_edit_active.load(std::memory_order_acquire))
	{
		g_shoulder_error = "another edit is already active";
		return;
	}
	if (g_shoulder_targets.empty())
	{
		g_shoulder_desired.store(false, std::memory_order_release);
		return;
	}
	g_shoulder_error.clear();
	if (begin_edits(g_shoulder_targets))
	{
		g_shoulder_active.store(true, std::memory_order_release);
		g_shoulder_applied_generation = g_discovery_generation.load(std::memory_order_acquire);
		g_shoulder_instances_added += active_target_count();
	}
	else
		g_shoulder_applied_generation = g_discovery_generation.load(std::memory_order_acquire);
	request_discovery(true, scan_reason::enable);
	g_next_priority_scan = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	g_next_full_scan = std::chrono::steady_clock::now() + std::chrono::seconds(30);
}

void service_shoulder_edit()
{
	if (!g_shoulder_desired.load(std::memory_order_acquire))
		return;
	const uint64_t generation = g_discovery_generation.load(std::memory_order_acquire);
	if (g_edit_active.load(std::memory_order_acquire) &&
		!g_shoulder_active.load(std::memory_order_acquire))
	{
		g_shoulder_error = "another edit is already active";
		return;
	}
	if (g_shoulder_active.load(std::memory_order_acquire))
	{
		if (generation == g_shoulder_applied_generation)
			return;
		const size_t added = extend_edits(g_shoulder_targets);
		g_shoulder_instances_added += added;
		g_shoulder_applied_generation = generation;
		if (added != 0)
			g_shoulder_error.clear();
		return;
	}
	if (generation == g_shoulder_applied_generation)
		return;
	g_shoulder_applied_generation = generation;
	if (begin_edits(g_shoulder_targets))
	{
		g_shoulder_active.store(true, std::memory_order_release);
		g_shoulder_instances_added += active_target_count();
		g_shoulder_error.clear();
		return;
	}
	g_shoulder_error = "requested ON; waiting for a valid shoulder instance";
}

void schedule_shoulder_discovery(std::chrono::steady_clock::time_point now)
{
	if ((!g_shoulder_desired.load(std::memory_order_acquire) &&
		!g_custom_intent.load().desired) ||
		g_hunt_phase.load(std::memory_order_acquire) <= 1)
		return;
	const uint64_t resource_generation = g_resource_generation.load(std::memory_order_acquire);
	const uint64_t last_resource_tick = g_last_resource_tick.load(std::memory_order_acquire);
	if (resource_generation != g_observed_resource_generation &&
		GetTickCount64() - last_resource_tick >= 1000)
	{
		g_observed_resource_generation = resource_generation;
		g_next_full_scan = now + std::chrono::seconds(30);
		g_next_priority_scan = now + std::chrono::seconds(2);
		request_discovery(true, scan_reason::resource_change);
		return;
	}
	if (now >= g_next_priority_scan)
	{
		g_next_priority_scan = now + std::chrono::seconds(2);
		request_discovery(false, scan_reason::priority_refresh);
		return;
	}
	if (now >= g_next_full_scan)
	{
		g_next_full_scan = now + std::chrono::seconds(30);
		request_discovery(true, scan_reason::resource_change);
	}
}

void initialize_runtime_files()
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
	g_session_id = (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^ GetTickCount64();
	g_telemetry_path = directory + L"\\telemetry.json";
	g_resource_log_path = directory + L"\\resource-monitor-" +
		std::to_wstring(g_session_id) + L".csv";
	g_automation_path = directory + L"\\automation.request";
	g_monitor_started_tick_ms = GetTickCount64();
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

double percent_of_interval(uint64_t busy_us, uint64_t interval_us)
{
	return interval_us == 0 ? 0.0 :
		100.0 * static_cast<double>(busy_us) / static_cast<double>(interval_us);
}

double scan_session_cpu_percent()
{
	const uint64_t elapsed_ms = GetTickCount64() - g_monitor_started_tick_ms;
	return percent_of_interval(g_scan_total_cpu_us.load(std::memory_order_relaxed),
		elapsed_ms * 1000);
}

void update_resource_sample()
{
	const uint64_t now_tick_ms = GetTickCount64();
	FILETIME created = {}, exited = {}, kernel = {}, user = {};
	uint64_t process_cpu_100ns = 0;
	if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
		process_cpu_100ns = file_time_100ns(kernel) + file_time_100ns(user);

	PROCESS_MEMORY_COUNTERS_EX memory = {};
	memory.cb = sizeof(memory);
	if (GetProcessMemoryInfo(GetCurrentProcess(),
		reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory)))
	{
		g_resource_sample.process_working_set_bytes = memory.WorkingSetSize;
		g_resource_sample.process_private_bytes = memory.PrivateUsage;
	}

	const uint64_t scan_cpu_us = g_scan_total_cpu_us.load(std::memory_order_relaxed);
	const uint64_t maintenance_wall_us =
		g_maintenance_total_wall_us.load(std::memory_order_relaxed);
	const uint64_t rebind_wall_us = g_rebind_total_wall_us.load(std::memory_order_relaxed);
	const uint64_t custom_scan_wall_us =
		g_custom_scan_total_wall_us.load(std::memory_order_relaxed);
	if (g_previous_sample_tick_ms != 0 && now_tick_ms > g_previous_sample_tick_ms)
	{
		const uint64_t interval_us = (now_tick_ms - g_previous_sample_tick_ms) * 1000;
		g_resource_sample.interval_us = interval_us;
		g_resource_sample.scan_cpu_core_percent = percent_of_interval(
			scan_cpu_us - g_previous_scan_cpu_us, interval_us);
		g_resource_sample.maintenance_wall_percent = percent_of_interval(
			maintenance_wall_us - g_previous_maintenance_wall_us, interval_us);
		g_resource_sample.rebind_wall_percent = percent_of_interval(
			rebind_wall_us - g_previous_rebind_wall_us, interval_us);
		g_resource_sample.custom_scan_wall_percent = percent_of_interval(
			custom_scan_wall_us - g_previous_custom_scan_wall_us, interval_us);
		if (process_cpu_100ns >= g_previous_process_cpu_100ns)
		{
			const uint64_t process_cpu_us =
				(process_cpu_100ns - g_previous_process_cpu_100ns) / 10;
			const DWORD processors = std::max<DWORD>(1,
				GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
			g_resource_sample.process_cpu_percent = percent_of_interval(
				process_cpu_us, interval_us * processors);
		}
	}
	g_previous_sample_tick_ms = now_tick_ms;
	g_previous_process_cpu_100ns = process_cpu_100ns;
	g_previous_scan_cpu_us = scan_cpu_us;
	g_previous_maintenance_wall_us = maintenance_wall_us;
	g_previous_rebind_wall_us = rebind_wall_us;
	g_previous_custom_scan_wall_us = custom_scan_wall_us;
}

void append_resource_monitor_row()
{
	if (g_resource_log_path.empty())
		return;
	const custom_runtime_view custom = read_custom_view();
	HANDLE file = CreateFileW(g_resource_log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
		nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return;
	LARGE_INTEGER size = {};
	if (GetFileSizeEx(file, &size) && size.QuadPart == 0)
	{
		const char header[] =
			"unix_ms,frame,scan_active,scan_requests,scan_coalesced,scan_runs,full_scans,priority_scans,"
			"scan_total_mib,scan_last_mib,scan_last_wall_ms,scan_last_cpu_ms,scan_cpu_core_percent,"
			"scan_session_cpu_core_percent,maintenance_calls,maintenance_table_checks,maintenance_read_mib,"
			"maintenance_last_us,maintenance_max_us,maintenance_wall_percent,rebind_calls,rebind_candidates,"
			"rebind_instances_added,rebind_last_us,rebind_max_us,rebind_wall_percent,process_cpu_percent,"
			"custom_scan_calls,custom_scan_total_mib,custom_scan_last_us,custom_scan_max_us,"
			"custom_scan_wall_percent,custom_palette_candidates,custom_pose_samples,custom_plans,"
			"custom_publications,custom_restores,custom_worker_busy,custom_worker_cycles,"
			"custom_worker_last_us,custom_worker_max_us,custom_worker_total_ms,working_set_mib,private_mib\r\n";
		DWORD written = 0;
		WriteFile(file, header, static_cast<DWORD>(sizeof(header) - 1), &written, nullptr);
	}
	constexpr double mib = 1024.0 * 1024.0;
	std::ostringstream row;
	row << std::fixed << std::setprecision(3)
		<< unix_time_ms() << ',' << g_frame.load(std::memory_order_relaxed) << ','
		<< (g_hunt_phase.load(std::memory_order_relaxed) == 1 ? 1 : 0) << ','
		<< g_scan_requests.load(std::memory_order_relaxed) << ','
		<< g_scan_requests_coalesced.load(std::memory_order_relaxed) << ','
		<< g_scan_runs.load(std::memory_order_relaxed) << ','
		<< g_scan_full_runs.load(std::memory_order_relaxed) << ','
		<< g_scan_priority_runs.load(std::memory_order_relaxed) << ','
		<< g_scan_total_bytes.load(std::memory_order_relaxed) / mib << ','
		<< g_scan_last_bytes.load(std::memory_order_relaxed) / mib << ','
		<< g_scan_last_wall_us.load(std::memory_order_relaxed) / 1000.0 << ','
		<< g_scan_last_cpu_us.load(std::memory_order_relaxed) / 1000.0 << ','
		<< g_resource_sample.scan_cpu_core_percent << ',' << scan_session_cpu_percent() << ','
		<< g_maintenance_calls.load(std::memory_order_relaxed) << ','
		<< g_maintenance_table_checks.load(std::memory_order_relaxed) << ','
		<< g_maintenance_read_bytes.load(std::memory_order_relaxed) / mib << ','
		<< g_maintenance_last_wall_us.load(std::memory_order_relaxed) << ','
		<< g_maintenance_max_wall_us.load(std::memory_order_relaxed) << ','
		<< g_resource_sample.maintenance_wall_percent << ','
		<< g_rebind_calls.load(std::memory_order_relaxed) << ','
		<< g_rebind_candidates.load(std::memory_order_relaxed) << ','
		<< g_rebind_instances_added_metric.load(std::memory_order_relaxed) << ','
		<< g_rebind_last_wall_us.load(std::memory_order_relaxed) << ','
		<< g_rebind_max_wall_us.load(std::memory_order_relaxed) << ','
		<< g_resource_sample.rebind_wall_percent << ','
		<< g_resource_sample.process_cpu_percent << ','
		<< g_custom_scan_calls.load(std::memory_order_relaxed) << ','
		<< custom.metrics.mapped_bytes_scanned / mib << ','
		<< g_custom_scan_last_wall_us.load(std::memory_order_relaxed) << ','
		<< g_custom_scan_max_wall_us.load(std::memory_order_relaxed) << ','
		<< g_resource_sample.custom_scan_wall_percent << ','
		<< custom.metrics.palette_candidates << ','
		<< custom.metrics.pose_samples << ','
		<< custom.metrics.plans_built << ','
		<< custom.metrics.publications << ','
		<< custom.metrics.restores << ','
		<< (g_custom_worker_busy.load(std::memory_order_relaxed) ? 1 : 0) << ','
		<< g_custom_worker_cycles.load(std::memory_order_relaxed) << ','
		<< g_custom_worker_last_wall_us.load(std::memory_order_relaxed) << ','
		<< g_custom_worker_max_wall_us.load(std::memory_order_relaxed) << ','
		<< g_custom_worker_total_wall_us.load(std::memory_order_relaxed) / 1000.0 << ','
		<< g_resource_sample.process_working_set_bytes / mib << ','
		<< g_resource_sample.process_private_bytes / mib << "\r\n";
	const std::string bytes = row.str();
	DWORD written = 0;
	WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
	CloseHandle(file);
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

bool send_key(WORD key, bool down)
{
	INPUT input = {};
	input.type = INPUT_KEYBOARD;
	input.ki.wVk = key;
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

void release_keys()
{
	if (g_w_held)
		send_key('W', false);
	if (g_d_held)
		send_key('D', false);
	if (g_b_held)
		send_key('B', false);
	if (g_screenshot_key_held)
		send_key(VK_F12, false);
	g_w_held = false;
	g_d_held = false;
	g_b_held = false;
	g_screenshot_key_held = false;
}

bool read_automation_request()
{
	HANDLE file = CreateFileW(g_automation_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	char text[256] = {};
	DWORD read = 0;
	const bool ok = ReadFile(file, text, sizeof(text) - 1, &read, nullptr) != FALSE;
	CloseHandle(file);
	DeleteFileW(g_automation_path.c_str());
	unsigned delay_ms = 20000, skip_intro = 0, edit_test = 0, steam_capture = 0;
	unsigned reserved_capture = 0;
	unsigned long long unit_id = 0;
	unsigned slot = UINT32_MAX;
	float x = 0.0f, y = 0.0f, z = 0.0f;
	const int fields = ok ? sscanf_s(text, "%u %u %u %u %u %llx %u %f %f %f", &delay_ms,
		&skip_intro, &edit_test, &reserved_capture, &steam_capture, &unit_id, &slot, &x, &y, &z) : 0;
	if (fields < 1)
		return false;
	if (edit_test != 0 && (fields < 10 || unit_id == 0 || slot == UINT32_MAX ||
		!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
		(x == 0.0f && y == 0.0f && z == 0.0f) || std::fabs(x) > k_max_edit_translation ||
		std::fabs(y) > k_max_edit_translation || std::fabs(z) > k_max_edit_translation))
	{
		g_automation_error = 4;
		g_automation_stage = 9;
		return false;
	}
	g_automation_delay_ms = std::min(delay_ms, 120000u);
	g_skip_intro = skip_intro != 0;
	g_steam_capture = steam_capture != 0;
	g_edit_requested.store(edit_test != 0, std::memory_order_release);
	g_requested_edit = { static_cast<uint64_t>(unit_id), 0, slot, { x, y, z } };
	g_edit.requested = edit_test != 0;
	return true;
}

bool request_screenshot(bool edit_capture)
{
	if (!g_steam_capture || edit_capture != g_edit.requested)
		return true;
	if (g_screenshot_key_held || !send_key(VK_F12, true))
		return false;
	g_screenshot_key_held = true;
	g_screenshot_release = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
	++g_capture_count;
	return true;
}

void fail_automation(uint32_t error)
{
	release_keys();
	end_edit();
	g_automation_error = error;
	g_automation_stage = 9;
}

void begin_scene_delay(std::chrono::steady_clock::time_point now)
{
	release_keys();
	if (!request_screenshot(false))
		g_automation_error = 3;
	const uint32_t delay = g_edit.requested ? std::min(g_automation_delay_ms, 1000u) :
		g_automation_delay_ms;
	g_automation_deadline = now + std::chrono::milliseconds(delay);
	g_automation_stage = 3;
}

void run_automation(effect_runtime *runtime)
{
	const auto now = std::chrono::steady_clock::now();
	if (g_screenshot_key_held && now >= g_screenshot_release)
	{
		send_key(VK_F12, false);
		g_screenshot_key_held = false;
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
		if (const HWND console = GetConsoleWindow(); console != nullptr)
			ShowWindowAsync(console, SW_MINIMIZE);
		if (!focus_runtime_window(runtime))
		{
			if (!g_focus_click_attempted)
			{
				g_focus_click_attempted = true;
				click_runtime_window();
			}
			return;
		}
		if (!request_screenshot(false))
			g_automation_error = 3;
		if (g_skip_intro)
		{
			g_automation_deadline = now + std::chrono::seconds(25);
			g_automation_stage = 2;
		}
		else
			begin_scene_delay(now);
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
			begin_scene_delay(now);
		}
	}
	else if (g_automation_stage == 3 && now >= g_automation_deadline &&
		(g_edit.requested || g_last_draws.load(std::memory_order_relaxed) >= 20))
	{
		if (!focus_runtime_window(runtime))
			return;
		if (!request_screenshot(false))
			g_automation_error = 3;
		if (g_edit.requested)
		{
			if (has_requested_target())
			{
				g_automation_deadline = now;
				g_automation_stage = 13;
				return;
			}
			if (g_hunt_phase.load(std::memory_order_acquire) != 1)
				request_rescan();
			if (g_hunt_phase.load(std::memory_order_acquire) != 1 && !ensure_hunt_thread())
			{
				fail_automation(4);
				return;
			}
			g_input_started = true;
			g_automation_deadline = now + std::chrono::seconds(5);
			g_automation_stage = 13;
			return;
		}
		if (!send_key('W', true))
		{
			fail_automation(1);
			return;
		}
		g_w_held = true;
		g_input_started = true;
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
		release_keys();
		if (!request_screenshot(false))
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
		g_automation_deadline = now + std::chrono::seconds(2);
		g_automation_stage = 7;
	}
	else if (g_automation_stage == 7 && now >= g_automation_deadline)
	{
		if (!request_screenshot(false))
			g_automation_error = 3;
		g_automation_completed = true;
		g_automation_stage = 8;
	}
	else if (g_automation_stage == 13 && now >= g_automation_deadline)
	{
		const bool target_ready = has_requested_target();
		if (g_hunt_phase.load(std::memory_order_acquire) == 1 &&
			std::chrono::steady_clock::now() < g_hunt_deadline)
			return;
		if (!target_ready)
		{
			fail_automation(4);
			return;
		}
		if (!request_screenshot(true))
		{
			fail_automation(3);
			return;
		}
		g_automation_deadline = now + std::chrono::milliseconds(300);
		g_automation_stage = 15;
	}
	else if (g_automation_stage == 15 && now >= g_automation_deadline)
	{
		if (!begin_edit(now))
			fail_automation(4);
	}
	else if (g_automation_stage == 10 && now >= g_automation_deadline)
	{
		if (!request_screenshot(true))
			g_automation_error = 3;
		g_automation_deadline = now + std::chrono::milliseconds(300);
		g_automation_stage = 16;
	}
	else if (g_automation_stage == 16 && now >= g_automation_deadline)
	{
		end_edit();
		g_automation_deadline = now + std::chrono::milliseconds(500);
		g_automation_stage = 11;
	}
	else if (g_automation_stage == 11 && now >= g_automation_deadline)
	{
		if (!request_screenshot(true))
			g_automation_error = 3;
		g_edit.completed = true;
		g_automation_completed = true;
		g_automation_stage = 8;
	}
}

void open_console()
{
	if (GetConsoleWindow() == nullptr)
		AllocConsole();
	SetConsoleOutputCP(CP_UTF8);
	FILE *stream = nullptr;
	freopen_s(&stream, "CONOUT$", "w", stdout);
	g_console = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD mode = 0;
	if (g_console != INVALID_HANDLE_VALUE && GetConsoleMode(g_console, &mode))
		SetConsoleMode(g_console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

void console_write(const std::string &text)
{
	if (g_console == INVALID_HANDLE_VALUE)
		return;
	DWORD written = 0;
	WriteFile(g_console, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

void draw_console(uint32_t draws)
{
	const auto now = std::chrono::steady_clock::now();
	if (now - g_last_console < std::chrono::milliseconds(250))
		return;
	g_last_console = now;
	const custom_runtime_view custom = read_custom_view();
	const hd2aa::retarget::custom_intent_snapshot custom_intent = g_custom_intent.load();
	std::ostringstream out;
	std::lock_guard lock(g_mutex);
	size_t mapped = 0;
	for (const auto &[unused, buffer] : g_buffers)
		mapped += buffer.map_ptr != nullptr;
	const uint64_t maintenance_calls = g_maintenance_calls.load(std::memory_order_relaxed);
	const uint64_t rebind_calls = g_rebind_calls.load(std::memory_order_relaxed);
	const double maintenance_average_us = maintenance_calls == 0 ? 0.0 :
		static_cast<double>(g_maintenance_total_wall_us.load(std::memory_order_relaxed)) /
		maintenance_calls;
	const double rebind_average_us = rebind_calls == 0 ? 0.0 :
		static_cast<double>(g_rebind_total_wall_us.load(std::memory_order_relaxed)) /
		rebind_calls;
	constexpr double mib = 1024.0 * 1024.0;
	out << "\x1b[2J\x1b[H"
		<< "HD2 Armature Profile Runtime " << k_runtime_version << "  |  "
		<< experiment_mode_name(custom) << "  |  D3D12\n\n"
		<< "frame " << g_frame.load(std::memory_order_relaxed) << "  draws " << draws
		<< "  buffers " << g_buffers.size()
		<< " (mapped " << mapped << ")\n"
		<< "profiles " << g_profile_files << " files / " << g_profiles.size()
		<< " unique tables / " << g_profile_records << " records ("
		<< g_profile_duplicates << " duplicates)  load errors " << g_profile_errors.size() << '\n'
		<< "hunt phase " << g_hunt_phase.load(std::memory_order_relaxed)
		<< "  " << (g_hunt_full_current.load(std::memory_order_relaxed) ? "full" : "priority")
		<< '/' << scan_reason_name(static_cast<scan_reason>(
			g_hunt_reason_current.load(std::memory_order_relaxed)))
		<< "  passes " << g_hunt_passes.load(std::memory_order_relaxed)
		<< "  registry " << g_hits.size()
		<< "  last " << g_last_scan_hits.load(std::memory_order_relaxed)
		<< " (+" << g_last_scan_new_instances.load(std::memory_order_relaxed) << ')'
		<< "  scanned " << std::fixed << std::setprecision(1)
		<< static_cast<double>(g_hunt_bytes.load(std::memory_order_relaxed)) / (1024.0 * 1024.0)
		<< " MiB  partial " << g_partial_candidates.load(std::memory_order_relaxed)
		<< " (best " << g_best_partial_entries.load(std::memory_order_relaxed) << ")\n"
		<< "monitor process " << std::fixed << std::setprecision(1)
		<< g_resource_sample.process_cpu_percent << "% CPU  "
		<< g_resource_sample.process_working_set_bytes / mib << " MiB working  "
		<< g_resource_sample.process_private_bytes / mib << " MiB private\n"
		<< "scan cost " << g_resource_sample.scan_cpu_core_percent << "% of one core now / "
		<< scan_session_cpu_percent() << "% session  runs "
		<< g_scan_runs.load(std::memory_order_relaxed) << " ("
		<< g_scan_full_runs.load(std::memory_order_relaxed) << " full, "
		<< g_scan_priority_runs.load(std::memory_order_relaxed) << " priority)  last "
		<< g_scan_last_wall_us.load(std::memory_order_relaxed) / 1000.0 << " ms wall / "
		<< g_scan_last_cpu_us.load(std::memory_order_relaxed) / 1000.0 << " ms CPU\n"
		<< "maintain " << g_resource_sample.maintenance_wall_percent
		<< "% frame time  avg/max " << maintenance_average_us << '/'
		<< g_maintenance_max_wall_us.load(std::memory_order_relaxed) << " us  rebind avg/max "
		<< rebind_average_us << '/' << g_rebind_max_wall_us.load(std::memory_order_relaxed)
		<< " us  scan queue " << g_scan_requests_coalesced.load(std::memory_order_relaxed)
		<< '/' << g_scan_requests.load(std::memory_order_relaxed) << " coalesced\n"
		<< "F8 " << (custom.configured ? "custom armature " : "shoulder narrowing ")
		<< (custom.configured ?
			hd2aa::retarget::custom_status_name(custom.status) :
			(!g_shoulder_desired.load(std::memory_order_relaxed) ? "OFF" :
			 g_shoulder_active.load(std::memory_order_relaxed) ? "ACTIVE" : "WAITING"))
		<< "  |  F9 rescan\n";
	if (custom.configured)
		out << "custom rig " << custom.rig_id.substr(0, 12)
			<< "  requested " << (custom_intent.desired ? "ON" : "OFF")
			<< " rev " << g_custom_worker_intent_revision.load(std::memory_order_relaxed)
			<< '/' << custom_intent.revision
			<< "  worker " << (g_custom_worker_busy.load(std::memory_order_relaxed) ? "BUSY" : "idle")
			<< "\n  mapped scan " << custom.metrics.mapped_bytes_scanned / (1024.0 * 1024.0)
			<< " MiB @ " << g_resource_sample.custom_scan_wall_percent
			<< "% wall interval (last/max "
			<< g_custom_scan_last_wall_us.load(std::memory_order_relaxed) << '/'
			<< g_custom_scan_max_wall_us.load(std::memory_order_relaxed) << " us)  candidates "
			<< custom.metrics.palette_candidates
			<< "  samples " << custom.metrics.pose_samples << "  plans " << custom.metrics.plans_built
			<< "  publishes/restores " << custom.metrics.publications << '/'
			<< custom.metrics.restores << '\n';
	out
		<< "edit " << (g_edit.active ? "ACTIVE" : g_edit.completed ? "complete" :
			g_edit.requested ? "armed" : "off") << "  slot ";
	if (g_edit.slot == UINT32_MAX)
		out << "multi";
	else
		out << g_edit.slot;
	out
		<< "  targets " << g_edit.targets_written << '/' << g_edit.targets_selected
		<< "  writes " << g_edit.write_successes << '/' << g_edit.write_attempts
		<< "  refills " << g_edit.refills_observed
		<< "  restored " << (g_edit.restore_succeeded ? "yes" : "no") << "\n\n";
	for (size_t index = 0; index < std::min<size_t>(g_hits.size(), 12); ++index)
	{
		const instance_lifecycle::instance &hit = g_hits[index];
		if (hit.profile_index >= g_profiles.size())
			continue;
		const loaded_table_profile &profile = g_profiles[hit.profile_index];
		out << '[' << index << "] 0x" << std::hex << hit.address
			<< "  unit " << std::setw(16) << std::setfill('0') << profile.unit_id
			<< "  table " << std::setw(16) << profile.table_key
			<< "  lod-mask 0x" << std::setw(8) << profile.lod_mask << std::dec
			<< "  slots " << profile.entries << "  " << profile.patch_name << '\n';
	}
	for (const std::string &error : g_profile_errors)
		out << "PROFILE ERROR: " << error << '\n';
	if (!g_custom_error.empty())
		out << "CUSTOM RIG ERROR: " << g_custom_error << '\n';
	if (!g_shoulder_error.empty())
		out << "SHOULDER ERROR: " << g_shoulder_error << '\n';
	console_write(out.str());
}

void write_telemetry(uint32_t draws)
{
	const auto now = std::chrono::steady_clock::now();
	if (g_telemetry_path.empty() || now - g_last_telemetry < std::chrono::seconds(1))
		return;
	g_last_telemetry = now;
	update_resource_sample();
	append_resource_monitor_row();
	struct profile_summary
	{
		std::string patch_name;
		std::string profile_file;
		uint64_t unit_id;
		uint64_t table_key;
		uint32_t lod_mask;
		uint32_t entries;
		uint32_t source_records;
	};
	std::vector<instance_lifecycle::instance> hits;
	std::vector<profile_summary> profiles;
	std::vector<std::string> profile_errors;
	edit_state edit;
	size_t buffer_count = 0, mapped_count = 0, active_instances = 0,
		shoulder_target_count = 0;
	uint32_t profile_files = 0, profile_records = 0, profile_duplicates = 0;
	{
		std::lock_guard lock(g_mutex);
		hits = g_hits;
		for (const loaded_table_profile &profile : g_profiles)
			profiles.push_back({ profile.patch_name, profile.profile_file, profile.unit_id,
				profile.table_key, profile.lod_mask, profile.entries, profile.source_records });
		profile_errors = g_profile_errors;
		edit = g_edit;
		buffer_count = g_buffers.size();
		for (const auto &[unused, buffer] : g_buffers)
			mapped_count += buffer.map_ptr != nullptr;
		profile_files = g_profile_files;
		profile_records = g_profile_records;
		profile_duplicates = g_profile_duplicates;
		active_instances = g_active_targets.size();
		shoulder_target_count = g_shoulder_targets.size();
	}
	const bool shoulder_desired = g_shoulder_desired.load(std::memory_order_relaxed);
	const bool shoulder_active = g_shoulder_active.load(std::memory_order_relaxed);
	const custom_runtime_view custom = read_custom_view();
	const hd2aa::retarget::custom_intent_snapshot custom_intent = g_custom_intent.load();

	std::ostringstream json;
	json << "{\n"
		<< "  \"schema\": 9,\n"
		<< "  \"addon\": \"HD2 Armature Profile Runtime\",\n"
		<< "  \"addon_version\": \"" << k_runtime_version << "\",\n"
		<< "  \"experiment_mode\": \"" << experiment_mode_name(custom) << "\",\n"
		<< "  \"api_version\": " << RESHADE_API_VERSION << ",\n"
		<< "  \"process_id\": " << GetCurrentProcessId() << ",\n"
		<< "  \"window_handle\": " << reinterpret_cast<uintptr_t>(g_game_window) << ",\n"
		<< "  \"session_id\": " << g_session_id << ",\n"
		<< "  \"updated_unix_ms\": " << unix_time_ms() << ",\n"
		<< "  \"frame\": " << g_frame.load(std::memory_order_relaxed) << ",\n"
		<< "  \"draws_last_frame\": " << draws << ",\n"
		<< "  \"tracked_buffers\": " << buffer_count << ",\n"
		<< "  \"mapped_buffers\": " << mapped_count << ",\n"
		<< "  \"profile_directory\": \"" << json_escape(utf8(g_profile_directory.c_str())) << "\",\n"
		<< "  \"profile_files_loaded\": " << profile_files << ",\n"
		<< "  \"profile_table_records\": " << profile_records << ",\n"
		<< "  \"profile_duplicate_records\": " << profile_duplicates << ",\n"
		<< "  \"profile_tables_loaded\": " << profiles.size() << ",\n"
		<< "  \"profile_load_errors\": " << profile_errors.size() << ",\n"
		<< "  \"custom_rig_configured\": "
		<< (custom.configured ? "true" : "false") << ",\n"
		<< "  \"custom_rig_id\": \"" << json_escape(custom.rig_id) << "\",\n"
		<< "  \"custom_rig_file\": \"" << json_escape(utf8(g_active_rig_file.c_str())) << "\",\n"
		<< "  \"custom_source_file\": \"" << json_escape(utf8(g_source_reference_file.c_str())) << "\",\n"
		<< "  \"custom_status\": \""
		<< hd2aa::retarget::custom_status_name(custom.status) << "\",\n"
		<< "  \"custom_desired_enabled\": "
		<< (custom.desired ? "true" : "false") << ",\n"
		<< "  \"custom_intent_enabled\": " << (custom_intent.desired ? "true" : "false") << ",\n"
		<< "  \"custom_intent_revision\": " << custom_intent.revision << ",\n"
		<< "  \"custom_worker_intent_revision\": "
		<< g_custom_worker_intent_revision.load(std::memory_order_relaxed) << ",\n"
		<< "  \"custom_worker_busy\": "
		<< (g_custom_worker_busy.load(std::memory_order_relaxed) ? "true" : "false") << ",\n"
		<< "  \"custom_worker_cycles\": "
		<< g_custom_worker_cycles.load(std::memory_order_relaxed) << ",\n"
		<< "  \"custom_worker_wall_ms_total\": "
		<< g_custom_worker_total_wall_us.load(std::memory_order_relaxed) / 1000.0 << ",\n"
		<< "  \"custom_worker_wall_us_last\": "
		<< g_custom_worker_last_wall_us.load(std::memory_order_relaxed) << ",\n"
		<< "  \"custom_worker_wall_us_max\": "
		<< g_custom_worker_max_wall_us.load(std::memory_order_relaxed) << ",\n"
		<< "  \"custom_error\": \"" << json_escape(g_custom_error) << "\",\n"
		<< "  \"custom_mapped_bytes_scanned\": " << custom.metrics.mapped_bytes_scanned << ",\n"
		<< "  \"custom_palette_candidates\": " << custom.metrics.palette_candidates << ",\n"
		<< "  \"custom_pose_samples\": " << custom.metrics.pose_samples << ",\n"
		<< "  \"custom_plans_built\": " << custom.metrics.plans_built << ",\n"
		<< "  \"custom_publications\": " << custom.metrics.publications << ",\n"
		<< "  \"custom_restores\": " << custom.metrics.restores << ",\n"
		<< "  \"custom_rejected_instances\": " << custom.metrics.rejected_instances << ",\n"
		<< "  \"custom_scan_calls_total\": "
		<< g_custom_scan_calls.load(std::memory_order_relaxed) << ",\n"
		<< "  \"custom_scan_wall_ms_total\": "
		<< g_custom_scan_total_wall_us.load(std::memory_order_relaxed) / 1000.0 << ",\n"
		<< "  \"custom_scan_wall_us_last\": "
		<< g_custom_scan_last_wall_us.load(std::memory_order_relaxed) << ",\n"
		<< "  \"custom_scan_wall_us_max\": "
		<< g_custom_scan_max_wall_us.load(std::memory_order_relaxed) << ",\n"
		<< "  \"custom_scan_wall_percent_sample\": "
		<< g_resource_sample.custom_scan_wall_percent << ",\n"
		<< "  \"shoulder_config_targets\": " << shoulder_target_count << ",\n"
		<< "  \"shoulder_desired_enabled\": "
		<< (shoulder_desired ? "true" : "false") << ",\n"
		<< "  \"shoulder_applied\": " << (shoulder_active ? "true" : "false") << ",\n"
		<< "  \"shoulder_waiting\": "
		<< (shoulder_desired && !shoulder_active ? "true" : "false") << ",\n"
		<< "  \"shoulder_toggle_pending\": "
		<< (shoulder_desired && !shoulder_active ? "true" : "false") << ",\n"
		<< "  \"shoulder_narrow_active\": "
		<< (shoulder_active ? "true" : "false") << ",\n"
		<< "  \"shoulder_toggle_count\": " << g_shoulder_toggles << ",\n"
		<< "  \"shoulder_intent_revision\": " << g_shoulder_intent_revision << ",\n"
		<< "  \"shoulder_applied_generation\": " << g_shoulder_applied_generation << ",\n"
		<< "  \"shoulder_live_instances\": " << active_instances << ",\n"
		<< "  \"shoulder_instances_added\": " << g_shoulder_instances_added << ",\n"
		<< "  \"shoulder_instances_retired\": " << g_shoulder_instances_retired << ",\n"
		<< "  \"shoulder_error\": \"" << json_escape(g_shoulder_error) << "\",\n"
		<< "  \"ib_hunt_phase\": " << g_hunt_phase.load(std::memory_order_relaxed) << ",\n"
		<< "  \"ib_hunt_scope\": \""
		<< (g_hunt_full_current.load(std::memory_order_relaxed) ? "full" : "priority") << "\",\n"
		<< "  \"ib_hunt_reason\": \"" << scan_reason_name(static_cast<scan_reason>(
			g_hunt_reason_current.load(std::memory_order_relaxed))) << "\",\n"
		<< "  \"ib_hunt_passes\": " << g_hunt_passes.load(std::memory_order_relaxed) << ",\n"
		<< "  \"ib_hunt_bytes\": " << g_hunt_bytes.load(std::memory_order_relaxed) << ",\n"
		<< "  \"ib_discovery_generation\": "
		<< g_discovery_generation.load(std::memory_order_relaxed) << ",\n"
		<< "  \"ib_last_scan_hits\": " << g_last_scan_hits.load(std::memory_order_relaxed) << ",\n"
		<< "  \"ib_last_scan_new_instances\": "
		<< g_last_scan_new_instances.load(std::memory_order_relaxed) << ",\n"
		<< "  \"resource_monitor_path\": \""
		<< json_escape(utf8(g_resource_log_path.c_str())) << "\",\n"
		<< "  \"resource_sample_interval_ms\": "
		<< g_resource_sample.interval_us / 1000.0 << ",\n"
		<< "  \"process_cpu_percent\": " << std::fixed << std::setprecision(3)
		<< g_resource_sample.process_cpu_percent << ",\n"
		<< "  \"process_working_set_bytes\": "
		<< g_resource_sample.process_working_set_bytes << ",\n"
		<< "  \"process_private_bytes\": " << g_resource_sample.process_private_bytes << ",\n"
		<< "  \"scan_active\": "
		<< (g_hunt_phase.load(std::memory_order_relaxed) == 1 ? "true" : "false") << ",\n"
		<< "  \"scan_requests_total\": " << g_scan_requests.load(std::memory_order_relaxed) << ",\n"
		<< "  \"scan_requests_coalesced\": "
		<< g_scan_requests_coalesced.load(std::memory_order_relaxed) << ",\n"
		<< "  \"scan_runs_total\": " << g_scan_runs.load(std::memory_order_relaxed) << ",\n"
		<< "  \"scan_full_runs_total\": "
		<< g_scan_full_runs.load(std::memory_order_relaxed) << ",\n"
		<< "  \"scan_priority_runs_total\": "
		<< g_scan_priority_runs.load(std::memory_order_relaxed) << ",\n"
		<< "  \"scan_bytes_total\": "
		<< g_scan_total_bytes.load(std::memory_order_relaxed) << ",\n"
		<< "  \"scan_bytes_last\": " << g_scan_last_bytes.load(std::memory_order_relaxed) << ",\n"
		<< "  \"scan_wall_ms_total\": "
		<< g_scan_total_wall_us.load(std::memory_order_relaxed) / 1000.0 << ",\n"
		<< "  \"scan_wall_ms_last\": "
		<< g_scan_last_wall_us.load(std::memory_order_relaxed) / 1000.0 << ",\n"
		<< "  \"scan_wall_ms_max\": "
		<< g_scan_max_wall_us.load(std::memory_order_relaxed) / 1000.0 << ",\n"
		<< "  \"scan_cpu_ms_total\": "
		<< g_scan_total_cpu_us.load(std::memory_order_relaxed) / 1000.0 << ",\n"
		<< "  \"scan_cpu_ms_last\": "
		<< g_scan_last_cpu_us.load(std::memory_order_relaxed) / 1000.0 << ",\n"
		<< "  \"scan_cpu_core_percent_sample\": "
		<< g_resource_sample.scan_cpu_core_percent << ",\n"
		<< "  \"scan_cpu_core_percent_session\": " << scan_session_cpu_percent() << ",\n"
		<< "  \"maintenance_calls_total\": "
		<< g_maintenance_calls.load(std::memory_order_relaxed) << ",\n"
		<< "  \"maintenance_table_checks_total\": "
		<< g_maintenance_table_checks.load(std::memory_order_relaxed) << ",\n"
		<< "  \"maintenance_read_bytes_total\": "
		<< g_maintenance_read_bytes.load(std::memory_order_relaxed) << ",\n"
		<< "  \"maintenance_wall_ms_total\": "
		<< g_maintenance_total_wall_us.load(std::memory_order_relaxed) / 1000.0 << ",\n"
		<< "  \"maintenance_wall_us_last\": "
		<< g_maintenance_last_wall_us.load(std::memory_order_relaxed) << ",\n"
		<< "  \"maintenance_wall_us_max\": "
		<< g_maintenance_max_wall_us.load(std::memory_order_relaxed) << ",\n"
		<< "  \"maintenance_wall_percent_sample\": "
		<< g_resource_sample.maintenance_wall_percent << ",\n"
		<< "  \"rebind_calls_total\": " << g_rebind_calls.load(std::memory_order_relaxed) << ",\n"
		<< "  \"rebind_candidates_total\": "
		<< g_rebind_candidates.load(std::memory_order_relaxed) << ",\n"
		<< "  \"rebind_instances_added_total\": "
		<< g_rebind_instances_added_metric.load(std::memory_order_relaxed) << ",\n"
		<< "  \"rebind_wall_ms_total\": "
		<< g_rebind_total_wall_us.load(std::memory_order_relaxed) / 1000.0 << ",\n"
		<< "  \"rebind_wall_us_last\": "
		<< g_rebind_last_wall_us.load(std::memory_order_relaxed) << ",\n"
		<< "  \"rebind_wall_us_max\": "
		<< g_rebind_max_wall_us.load(std::memory_order_relaxed) << ",\n"
		<< "  \"rebind_wall_percent_sample\": "
		<< g_resource_sample.rebind_wall_percent << ",\n"
		<< "  \"resource_events_total\": "
		<< g_resource_generation.load(std::memory_order_relaxed) << ",\n"
		<< "  \"converted_ib_hits\": " << hits.size() << ",\n"
		<< "  \"converted_ib_partial_candidates\": " << g_partial_candidates.load(std::memory_order_relaxed) << ",\n"
		<< "  \"converted_ib_best_partial_entries\": " << g_best_partial_entries.load(std::memory_order_relaxed) << ",\n"
		<< "  \"converted_ib_tables\": [";
	for (size_t index = 0; index < hits.size(); ++index)
	{
		const instance_lifecycle::instance &hit = hits[index];
		if (index != 0)
			json << ',';
		json << "{\"address\":\"0x" << std::hex << hit.address
			<< "\",\"profile_index\":" << std::dec << hit.profile_index;
		if (hit.profile_index < profiles.size())
		{
			const profile_summary &profile = profiles[hit.profile_index];
			json << ",\"profile_file\":\"" << json_escape(profile.profile_file)
				<< "\",\"patch\":\"" << json_escape(profile.patch_name)
				<< "\",\"unit\":\"" << std::hex << std::setw(16) << std::setfill('0')
				<< profile.unit_id << "\",\"table_key\":\"" << std::setw(16)
				<< profile.table_key << "\",\"lod_mask\":\"0x" << std::setw(8)
				<< profile.lod_mask << "\",\"entries\":" << std::dec << profile.entries
				<< ",\"source_records\":" << profile.source_records;
		}
		json << ",\"region_base\":\"0x" << std::hex << hit.region_base << "\",\"region_size\":"
			<< std::dec << hit.region_size << ",\"protection\":" << hit.protection
			<< ",\"last_seen_generation\":" << hit.last_seen_generation << '}';
	}
	json << "],\n"
		<< "  \"automation_stage\": " << g_automation_stage << ",\n"
		<< "  \"automation_intro_attempts\": " << g_intro_attempts << ",\n"
		<< "  \"automation_input_started\": " << (g_input_started ? "true" : "false") << ",\n"
		<< "  \"automation_completed\": " << (g_automation_completed ? "true" : "false") << ",\n"
		<< "  \"automation_error\": " << g_automation_error << ",\n"
		<< "  \"captures\": " << g_capture_count << ",\n"
		<< "  \"edit_test_requested\": " << (edit.requested ? "true" : "false") << ",\n"
		<< "  \"edit_test_active\": " << (edit.active ? "true" : "false") << ",\n"
		<< "  \"edit_test_completed\": " << (edit.completed ? "true" : "false") << ",\n"
		<< "  \"edit_unit\": \"" << std::hex << std::setw(16) << std::setfill('0')
		<< g_requested_edit.unit_id << std::dec << "\",\n"
		<< "  \"edit_slot\": " << (edit.slot == UINT32_MAX ? g_requested_edit.slot : edit.slot)
		<< ",\n"
		<< "  \"edit_world_translation\": [" << g_requested_edit.world_translation[0] << ", "
		<< g_requested_edit.world_translation[1] << ", " << g_requested_edit.world_translation[2] << "],\n"
		<< "  \"edit_write_attempts\": " << edit.write_attempts << ",\n"
		<< "  \"edit_write_successes\": " << edit.write_successes << ",\n"
		<< "  \"edit_immediate_readbacks\": " << edit.immediate_readbacks << ",\n"
		<< "  \"edit_present_readbacks\": " << edit.present_readbacks << ",\n"
		<< "  \"edit_refills_observed\": " << edit.refills_observed << ",\n"
		<< "  \"edit_stale_skips\": " << edit.stale_skips << ",\n"
		<< "  \"edit_targets_selected\": " << edit.targets_selected << ",\n"
		<< "  \"edit_targets_written\": " << edit.targets_written << ",\n"
		<< "  \"edit_targets_restored\": " << edit.targets_restored << ",\n"
		<< "  \"edit_restore_attempted\": " << (edit.restore_attempted ? "true" : "false") << ",\n"
		<< "  \"edit_restore_succeeded\": " << (edit.restore_succeeded ? "true" : "false") << ",\n"
		<< "  \"edit_error\": " << edit.error << "\n"
		<< "}\n";
	const std::string bytes = json.str();
	const std::wstring temporary = g_telemetry_path + L".tmp";
	HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return;
	DWORD written = 0;
	const bool complete = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE &&
		written == static_cast<DWORD>(bytes.size());
	CloseHandle(file);
	if (complete)
		MoveFileExW(temporary.c_str(), g_telemetry_path.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void stop_hunt_thread()
{
	g_hunt_stop.store(true, std::memory_order_release);
	if (g_hunt_thread != nullptr)
	{
		WaitForSingleObject(g_hunt_thread, 1000);
		CloseHandle(g_hunt_thread);
		g_hunt_thread = nullptr;
	}
}

void on_init_device(device *device)
{
	if (device->get_api() != device_api::d3d12)
		return;
	{
		std::lock_guard lock(g_mutex);
		if (g_device != nullptr)
			return;
		g_device = device;
		const auto now = std::chrono::steady_clock::now();
		g_hunt_not_before = now + std::chrono::seconds(25);
		g_next_priority_scan = now + std::chrono::seconds(2);
		g_next_full_scan = now + std::chrono::seconds(30);
		g_hunt_full_next.store(true, std::memory_order_release);
		g_hunt_reason_next.store(static_cast<uint32_t>(scan_reason::initial), std::memory_order_release);
		initialize_runtime_files();
		load_profiles();
		load_custom_rig();
		load_shoulder_targets();
		open_console();
	}
	start_custom_worker();
}

void on_destroy_device(device *device)
{
	if (device != g_device)
		return;
	stop_custom_runtime();
	end_edit();
	stop_hunt_thread();
	std::lock_guard lock(g_mutex);
	g_buffers.clear();
	g_hits.clear();
	g_profiles.clear();
	g_custom_scan_buffer.clear();
	g_custom_pending_scan_buffer.clear();
	g_custom_active_scan_buffer.clear();
	g_custom_pending_intent_revision = 0;
	g_custom_active_intent_revision = 0;
	g_custom_active_cursor = 0;
	g_custom_capture_enabled.store(false, std::memory_order_release);
	g_active_targets.clear();
	g_shoulder_targets.clear();
	g_shoulder_desired.store(false, std::memory_order_release);
	g_shoulder_active.store(false, std::memory_order_release);
	g_hunt_again.store(false, std::memory_order_release);
	g_device = nullptr;
}

void on_init_resource(device *device, const resource_desc &desc, const subresource_data *,
	resource_usage, resource resource)
{
	if (device != g_device || desc.type != resource_type::buffer)
		return;
	{
		std::lock_guard lock(g_mutex);
		g_buffers[resource.handle] = { desc.buffer.size };
	}
	g_resource_generation.fetch_add(1, std::memory_order_relaxed);
	g_last_resource_tick.store(GetTickCount64(), std::memory_order_release);
}

void on_destroy_resource(device *device, resource resource)
{
	if (device != g_device)
		return;
	{
		std::lock_guard lock(g_mutex);
		g_buffers.erase(resource.handle);
		if (g_custom_scan_resource == resource.handle)
			g_custom_scan_resource = 0;
	}
	g_resource_generation.fetch_add(1, std::memory_order_relaxed);
	g_last_resource_tick.store(GetTickCount64(), std::memory_order_release);
}

void on_map_buffer(device *device, resource resource, uint64_t offset, uint64_t size,
	map_access, void **data)
{
	if (device != g_device || data == nullptr || *data == nullptr)
		return;
	{
		std::lock_guard lock(g_mutex);
		auto found = g_buffers.find(resource.handle);
		if (found == g_buffers.end())
			return;
		const uint64_t map_size = size == 0 || size == UINT64_MAX ?
			(offset < found->second.size ? found->second.size - offset : 0) : size;
		if (found->second.scan_map_offset != offset || found->second.scan_map_size != map_size)
		{
			found->second.scan_cursor = 0;
			found->second.capture_cursor = 0;
			found->second.scan_map_offset = offset;
			found->second.scan_map_size = map_size;
		}
		found->second.map_ptr = *data;
		found->second.map_offset = offset;
		found->second.map_size = map_size;
	}
	if (g_custom_capture_enabled.load(std::memory_order_acquire))
		queue_custom_pose_input(resource.handle);
}

void on_unmap_buffer(device *device, resource resource)
{
	if (device != g_device)
		return;
	std::lock_guard lock(g_mutex);
	auto found = g_buffers.find(resource.handle);
	if (found != g_buffers.end())
	{
		found->second.map_ptr = nullptr;
		found->second.map_offset = 0;
		found->second.map_size = 0;
		if (g_custom_scan_resource == resource.handle)
			g_custom_scan_resource = 0;
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
	g_frame.fetch_add(1, std::memory_order_relaxed);
	const uint32_t draws = g_draws.exchange(0, std::memory_order_relaxed);
	g_last_draws.store(draws, std::memory_order_relaxed);
	const custom_runtime_view custom = read_custom_view();
	if ((GetAsyncKeyState(VK_F8) & 1) && game_has_focus())
	{
		if (custom.configured)
		{
			const bool enabled = g_custom_intent.toggle();
			g_custom_capture_enabled.store(enabled, std::memory_order_release);
			wake_custom_worker();
			if (enabled)
				request_discovery(true, scan_reason::enable);
		}
		else
			toggle_shoulder_edit();
	}
	if (GetAsyncKeyState(VK_F9) & 1)
		request_discovery(true, scan_reason::manual);
	const auto now = std::chrono::steady_clock::now();
	schedule_shoulder_discovery(now);
	if (g_hunt_phase.load(std::memory_order_acquire) == 0 &&
		now >= g_hunt_not_before)
		ensure_hunt_thread();
	service_shoulder_edit();
	const size_t retired = maintain_edit();
	if (retired != 0 && g_shoulder_desired.load(std::memory_order_acquire))
	{
		g_shoulder_instances_retired += retired;
		g_shoulder_active.store(g_edit_active.load(std::memory_order_acquire),
			std::memory_order_release);
		request_discovery(true, scan_reason::stale_instance);
	}
	draw_console(draws);
	write_telemetry(draws);
}

void on_reshade_present(effect_runtime *runtime)
{
	if (runtime->get_device() == g_device)
	{
		g_game_window = static_cast<HWND>(runtime->get_hwnd());
		run_automation(runtime);
	}
}
}

extern "C"
{
__declspec(dllexport) const char *NAME = "HD2 Armature Profile Runtime";
__declspec(dllexport) const char *DESCRIPTION =
	"Loads extracted patch profiles, locates converted inverse-bind tables, and applies guarded edits.";
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		g_addon_module = module;
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
		release_keys();
		if (reserved == nullptr)
		{
			stop_custom_runtime();
			end_edit();
			stop_hunt_thread();
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
