#include <reshade.hpp>

#include "ib_layout.hpp"
#include "profile_scan.hpp"
#include "runtime_profile.hpp"

#include <Windows.h>

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
constexpr size_t k_scan_chunk_bytes = 4 * 1024 * 1024;
constexpr size_t k_max_hits = 256;
constexpr uint32_t k_max_profile_tables = 4096;
constexpr float k_max_edit_translation = 10.0f;

struct buffer_info
{
	uint64_t size = 0;
	void *map_ptr = nullptr;
	uint64_t map_size = 0;
};

struct loaded_table_profile
{
	std::string patch_name;
	std::string profile_file;
	uint64_t unit_id = 0;
	uint32_t lod_mask = 0;
	uint32_t entries = 0;
	uint32_t first_lod = 0;
	uint32_t source_records = 1;
	std::vector<uint8_t> t48;
};

struct converted_hit
{
	uintptr_t address = 0;
	size_t profile_index = 0;
	uintptr_t region_base = 0;
	size_t region_size = 0;
	DWORD protection = 0;
};

struct active_target
{
	uintptr_t address = 0;
	std::array<uint8_t, armature_profile::transform_stride> original {};
	std::array<uint8_t, armature_profile::transform_stride> injected {};
};

struct edit_request
{
	uint64_t unit_id = 0;
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

std::mutex g_mutex;
device *g_device = nullptr;
HMODULE g_addon_module = nullptr;
std::unordered_map<uint64_t, buffer_info> g_buffers;
std::vector<loaded_table_profile> g_profiles;
std::vector<std::string> g_profile_errors;
std::vector<converted_hit> g_hits;
std::vector<active_target> g_active_targets;
std::wstring g_profile_directory;
uint32_t g_profile_files = 0;
uint32_t g_profile_records = 0;
uint32_t g_profile_duplicates = 0;

std::atomic<uint32_t> g_hunt_phase { 0 }; // 0 idle, 1 scanning, 2 found, 3 exhausted/error
std::atomic<uint32_t> g_hunt_passes { 0 };
std::atomic<uint64_t> g_hunt_bytes { 0 };
std::atomic<uint64_t> g_partial_candidates { 0 };
std::atomic<uint32_t> g_best_partial_entries { 0 };
std::atomic<bool> g_hunt_stop { false };
HANDLE g_hunt_thread = nullptr;
std::chrono::steady_clock::time_point g_hunt_deadline;
std::chrono::steady_clock::time_point g_hunt_not_before;

HANDLE g_console = INVALID_HANDLE_VALUE;
uint64_t g_frame = 0;
std::atomic<uint32_t> g_draws { 0 };
std::atomic<uint32_t> g_last_draws { 0 };
std::chrono::steady_clock::time_point g_last_console;
std::chrono::steady_clock::time_point g_last_telemetry;

std::wstring g_telemetry_path;
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
std::atomic<bool> g_shoulder_pending { false };
std::atomic<bool> g_shoulder_active { false };
uint64_t g_shoulder_toggles = 0;

const char *experiment_mode_name()
{
	if (g_shoulder_pending.load(std::memory_order_relaxed) ||
		g_shoulder_active.load(std::memory_order_relaxed))
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
			g_profiles.push_back({ package.patch_name, utf8(name.c_str()), table.unit_id,
				table.lod_mask, table.entries, table.first_lod, 1, std::move(table.t48) });
		}
		++g_profile_files;
	}
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
		std::string unit_text, extra;
		edit_request target;
		if (!(fields >> unit_text))
			continue;
		if (!(fields >> target.slot >> target.world_translation[0] >>
			target.world_translation[1] >> target.world_translation[2]) || fields >> extra ||
			unit_text.size() != 16)
		{
			g_shoulder_error = "invalid shoulder target on line " + std::to_string(line_number);
			g_shoulder_targets.clear();
			return;
		}
		const auto parsed = std::from_chars(unit_text.data(), unit_text.data() + unit_text.size(),
			target.unit_id, 16);
		if (parsed.ec != std::errc() || parsed.ptr != unit_text.data() + unit_text.size() ||
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
				return profile.unit_id == target.unit_id && target.slot < profile.entries;
			});
		const bool duplicate = std::any_of(g_shoulder_targets.begin(), g_shoulder_targets.end(),
			[&target](const edit_request &loaded) {
				return loaded.unit_id == target.unit_id && loaded.slot == target.slot;
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

DWORD WINAPI hunt_thread_proc(void *)
{
	std::vector<loaded_table_profile> profiles;
	{
		std::lock_guard lock(g_mutex);
		profiles = g_profiles;
	}
	if (profiles.empty())
	{
		g_hunt_phase.store(3, std::memory_order_release);
		return 0;
	}
	std::vector<armature_probe::profile_view> views;
	size_t largest_table = 0;
	for (const loaded_table_profile &profile : profiles)
	{
		views.push_back({ profile.t48.data(), profile.entries });
		largest_table = std::max(largest_table, profile.t48.size());
	}
	std::vector<uint8_t> scratch(k_scan_chunk_bytes + largest_table);
	SYSTEM_INFO system = {};
	GetSystemInfo(&system);
	const uintptr_t minimum = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
	const uintptr_t maximum = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);

	for (uint32_t pass = 0; pass < 40 && !g_hunt_stop.load(std::memory_order_acquire) &&
		std::chrono::steady_clock::now() < g_hunt_deadline; ++pass)
	{
		std::vector<converted_hit> found_hits;
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
		uintptr_t cursor = minimum;
		while (cursor < maximum && found_hits.size() < k_max_hits &&
			!g_hunt_stop.load(std::memory_order_acquire) &&
			std::chrono::steady_clock::now() < g_hunt_deadline)
		{
			MEMORY_BASIC_INFORMATION info = {};
			if (VirtualQuery(reinterpret_cast<const void *>(cursor), &info, sizeof(info)) == 0)
				break;
			const uintptr_t base = reinterpret_cast<uintptr_t>(info.BaseAddress);
			const size_t region_size = info.RegionSize;
			if (writable_private_page(info))
			{
				for (size_t region_offset = 0; region_offset < region_size &&
					found_hits.size() < k_max_hits &&
					std::chrono::steady_clock::now() < g_hunt_deadline;
					region_offset += k_scan_chunk_bytes)
				{
					const size_t wanted = std::min(scratch.size(), region_size - region_offset);
					SIZE_T got = 0;
					ReadProcessMemory(GetCurrentProcess(),
						reinterpret_cast<const void *>(base + region_offset), scratch.data(), wanted, &got);
					g_hunt_bytes.fetch_add(got, std::memory_order_relaxed);
					if (got < armature_profile::transform_stride)
						continue;
					auto scan = armature_probe::find_exact_profiles(scratch.data(), got,
						base + region_offset, views, excluded, k_max_hits - found_hits.size());
					partial += scan.partial_candidates;
					best_partial = std::max(best_partial, scan.best_partial_entries);
					const uintptr_t primary_end = base + region_offset +
						std::min(k_scan_chunk_bytes, static_cast<size_t>(got));
					for (const armature_probe::profile_hit &hit : scan.hits)
					{
						if (hit.address >= primary_end)
							continue;
						const bool duplicate = std::any_of(found_hits.begin(), found_hits.end(),
							[&hit](const converted_hit &known) {
								return known.address == hit.address && known.profile_index == hit.profile_index;
							});
						if (!duplicate)
							found_hits.push_back({ hit.address, hit.profile_index, base, region_size, info.Protect });
					}
				}
			}
			if (region_size == 0 || base > maximum - region_size)
				break;
			cursor = base + region_size;
		}
		g_partial_candidates.store(partial, std::memory_order_relaxed);
		g_best_partial_entries.store(best_partial, std::memory_order_relaxed);
		g_hunt_passes.fetch_add(1, std::memory_order_relaxed);
		if (!found_hits.empty())
		{
			std::lock_guard lock(g_mutex);
			g_hits = std::move(found_hits);
			g_hunt_phase.store(2, std::memory_order_release);
			return 0;
		}
		Sleep(500);
	}
	g_hunt_phase.store(3, std::memory_order_release);
	return 0;
}

bool ensure_hunt_thread()
{
	const uint32_t phase = g_hunt_phase.load(std::memory_order_acquire);
	if (phase == 1 || phase == 2)
		return true;
	{
		std::lock_guard lock(g_mutex);
		if (g_profiles.empty())
		{
			g_hunt_phase.store(3, std::memory_order_release);
			return false;
		}
		g_hits.clear();
	}
	if (g_hunt_thread != nullptr)
	{
		WaitForSingleObject(g_hunt_thread, 1000);
		CloseHandle(g_hunt_thread);
		g_hunt_thread = nullptr;
	}
	g_hunt_bytes.store(0, std::memory_order_relaxed);
	g_hunt_passes.store(0, std::memory_order_relaxed);
	g_partial_candidates.store(0, std::memory_order_relaxed);
	g_best_partial_entries.store(0, std::memory_order_relaxed);
	g_hunt_stop.store(false, std::memory_order_release);
	g_hunt_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	g_hunt_phase.store(1, std::memory_order_release);
	g_hunt_thread = CreateThread(nullptr, 0, hunt_thread_proc, nullptr, 0, nullptr);
	if (g_hunt_thread == nullptr)
	{
		g_hunt_phase.store(3, std::memory_order_release);
		return false;
	}
	return true;
}

void request_rescan()
{
	if (g_hunt_phase.load(std::memory_order_acquire) == 1)
		return;
	std::lock_guard lock(g_mutex);
	g_hits.clear();
	g_hunt_phase.store(0, std::memory_order_release);
	g_hunt_not_before = std::chrono::steady_clock::now();
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

bool has_requested_target()
{
	std::lock_guard lock(g_mutex);
	return std::any_of(g_hits.begin(), g_hits.end(), [](const converted_hit &hit) {
		return hit.profile_index < g_profiles.size() &&
			g_profiles[hit.profile_index].unit_id == g_requested_edit.unit_id &&
			g_requested_edit.slot < g_profiles[hit.profile_index].entries;
	});
}

bool begin_edits(const std::vector<edit_request> &requests)
{
	if (g_hunt_phase.load(std::memory_order_acquire) != 2 || requests.empty())
		return false;
	if (g_edit_active.load(std::memory_order_acquire))
		return false;
	std::lock_guard lock(g_mutex);
	g_active_targets.clear();
	g_edit = {};
	g_edit.requested = true;
	g_edit.slot = requests.size() == 1 ? requests.front().slot : UINT32_MAX;
	std::vector<uintptr_t> considered;

	for (const edit_request &request : requests)
	{
		const size_t slot_offset = static_cast<size_t>(request.slot) *
			armature_profile::transform_stride;
		for (const converted_hit &hit : g_hits)
		{
			if (hit.profile_index >= g_profiles.size())
				continue;
			const loaded_table_profile &profile = g_profiles[hit.profile_index];
			if (profile.unit_id != request.unit_id || request.slot >= profile.entries)
				continue;
			const uintptr_t address = hit.address + slot_offset;
			if (std::find(considered.begin(), considered.end(), address) != considered.end())
				continue;
			considered.push_back(address);
			++g_edit.targets_selected;
			active_target target;
			target.address = address;
			std::memcpy(target.original.data(), profile.t48.data() + slot_offset,
				target.original.size());
			armature_probe::translate_world_t48(target.original.data(),
				request.world_translation[0], request.world_translation[1],
				request.world_translation[2], target.injected.data());
			if (target.original == target.injected)
			{
				++g_edit.stale_skips;
				continue;
			}
			std::array<uint8_t, armature_profile::transform_stride> current {};
			if (!read_process_bytes(address, current.data(), current.size()) || current != target.original)
			{
				++g_edit.stale_skips;
				continue;
			}
			++g_edit.write_attempts;
			if (!write_process_bytes(address, target.injected.data(), target.injected.size()) ||
				!read_process_bytes(address, current.data(), current.size()) || current != target.injected)
			{
				write_process_bytes(address, target.original.data(), target.original.size());
				continue;
			}
			++g_edit.write_successes;
			++g_edit.immediate_readbacks;
			++g_edit.targets_written;
			g_active_targets.push_back(target);
		}
	}

	g_edit.active = !g_active_targets.empty();
	if (!g_edit.active)
	{
		g_edit.error = 2;
		return false;
	}
	g_edit_active.store(true, std::memory_order_release);
	return true;
}

bool begin_edit(std::chrono::steady_clock::time_point now)
{
	if (!begin_edits({ g_requested_edit }))
		return false;
	g_automation_deadline = now + std::chrono::seconds(1);
	g_automation_stage = 10;
	return true;
}

void maintain_edit()
{
	if (!g_edit_active.load(std::memory_order_acquire))
		return;
	std::unique_lock lock(g_mutex, std::try_to_lock);
	if (!lock.owns_lock() || !g_edit.active)
		return;
	for (active_target &target : g_active_targets)
	{
		std::array<uint8_t, armature_profile::transform_stride> current {};
		if (!read_process_bytes(target.address, current.data(), current.size()))
		{
			++g_edit.stale_skips;
			continue;
		}
		if (current == target.injected)
		{
			++g_edit.present_readbacks;
			continue;
		}
		if (current != target.original)
		{
			++g_edit.stale_skips;
			continue;
		}
		++g_edit.refills_observed;
		++g_edit.write_attempts;
		if (write_process_bytes(target.address, target.injected.data(), target.injected.size()) &&
			read_process_bytes(target.address, current.data(), current.size()) && current == target.injected)
		{
			++g_edit.write_successes;
			++g_edit.immediate_readbacks;
		}
	}
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
		std::array<uint8_t, armature_profile::transform_stride> current {};
		g_edit.restore_attempted = true;
		if (!read_process_bytes(target.address, current.data(), current.size()))
		{
			restored = false;
			continue;
		}
		if (current == target.original)
		{
			++g_edit.targets_restored;
			continue;
		}
		if (current != target.injected ||
			!write_process_bytes(target.address, target.original.data(), target.original.size()) ||
			!read_process_bytes(target.address, current.data(), current.size()) || current != target.original)
		{
			restored = false;
			continue;
		}
		++g_edit.targets_restored;
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
	if (g_shoulder_pending.exchange(false, std::memory_order_acq_rel))
	{
		g_shoulder_error.clear();
		return;
	}
	if (g_shoulder_active.load(std::memory_order_acquire))
	{
		end_edit();
		g_shoulder_active.store(false, std::memory_order_release);
		g_edit.completed = true;
		return;
	}
	if (g_edit_active.load(std::memory_order_acquire))
	{
		g_shoulder_error = "another edit is already active";
		return;
	}
	if (g_shoulder_targets.empty())
		return;
	g_shoulder_error.clear();
	g_shoulder_pending.store(true, std::memory_order_release);
	if (g_hunt_phase.load(std::memory_order_acquire) == 3)
		request_rescan();
}

void service_shoulder_edit()
{
	if (!g_shoulder_pending.load(std::memory_order_acquire))
		return;
	const uint32_t phase = g_hunt_phase.load(std::memory_order_acquire);
	if (phase == 0)
	{
		ensure_hunt_thread();
		return;
	}
	if (phase == 1)
		return;
	if (phase != 2 || !begin_edits(g_shoulder_targets))
	{
		g_shoulder_error = "no mapped shoulder table is currently resident";
		g_shoulder_pending.store(false, std::memory_order_release);
		return;
	}
	g_shoulder_active.store(true, std::memory_order_release);
	g_shoulder_pending.store(false, std::memory_order_release);
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
	g_telemetry_path = directory + L"\\telemetry.json";
	g_automation_path = directory + L"\\automation.request";
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
	g_requested_edit = { static_cast<uint64_t>(unit_id), slot, { x, y, z } };
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
			if (g_hunt_phase.load(std::memory_order_acquire) == 3)
				request_rescan();
			if (!ensure_hunt_thread())
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
	std::ostringstream out;
	std::lock_guard lock(g_mutex);
	size_t mapped = 0;
	for (const auto &[unused, buffer] : g_buffers)
		mapped += buffer.map_ptr != nullptr;
	out << "\x1b[2J\x1b[H"
		<< "HD2 Armature Profile Runtime 0.8  |  " << experiment_mode_name() << "  |  D3D12\n\n"
		<< "frame " << g_frame << "  draws " << draws << "  buffers " << g_buffers.size()
		<< " (mapped " << mapped << ")\n"
		<< "profiles " << g_profile_files << " files / " << g_profiles.size()
		<< " unique tables / " << g_profile_records << " records ("
		<< g_profile_duplicates << " duplicates)  load errors " << g_profile_errors.size() << '\n'
		<< "hunt phase " << g_hunt_phase.load(std::memory_order_relaxed)
		<< "  passes " << g_hunt_passes.load(std::memory_order_relaxed)
		<< "  hits " << g_hits.size()
		<< "  scanned " << std::fixed << std::setprecision(1)
		<< static_cast<double>(g_hunt_bytes.load(std::memory_order_relaxed)) / (1024.0 * 1024.0)
		<< " MiB  partial " << g_partial_candidates.load(std::memory_order_relaxed)
		<< " (best " << g_best_partial_entries.load(std::memory_order_relaxed) << ")\n"
		<< "F8 shoulder narrowing "
		<< (g_shoulder_active.load(std::memory_order_relaxed) ? "ACTIVE" :
			g_shoulder_pending.load(std::memory_order_relaxed) ? "pending" : "off")
		<< " (" << g_shoulder_targets.size() << " mapped slots)  |  F9 rescan\n"
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
		const converted_hit &hit = g_hits[index];
		if (hit.profile_index >= g_profiles.size())
			continue;
		const loaded_table_profile &profile = g_profiles[hit.profile_index];
		out << '[' << index << "] 0x" << std::hex << hit.address
			<< "  unit " << std::setw(16) << std::setfill('0') << profile.unit_id
			<< "  lod-mask 0x" << std::setw(8) << profile.lod_mask << std::dec
			<< "  slots " << profile.entries << "  " << profile.patch_name << '\n';
	}
	for (const std::string &error : g_profile_errors)
		out << "PROFILE ERROR: " << error << '\n';
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
	struct profile_summary
	{
		std::string patch_name;
		std::string profile_file;
		uint64_t unit_id;
		uint32_t lod_mask;
		uint32_t entries;
		uint32_t source_records;
	};
	std::vector<converted_hit> hits;
	std::vector<profile_summary> profiles;
	std::vector<std::string> profile_errors;
	edit_state edit;
	size_t buffer_count = 0, mapped_count = 0;
	uint32_t profile_files = 0, profile_records = 0, profile_duplicates = 0;
	{
		std::lock_guard lock(g_mutex);
		hits = g_hits;
		for (const loaded_table_profile &profile : g_profiles)
			profiles.push_back({ profile.patch_name, profile.profile_file, profile.unit_id,
				profile.lod_mask, profile.entries, profile.source_records });
		profile_errors = g_profile_errors;
		edit = g_edit;
		buffer_count = g_buffers.size();
		for (const auto &[unused, buffer] : g_buffers)
			mapped_count += buffer.map_ptr != nullptr;
		profile_files = g_profile_files;
		profile_records = g_profile_records;
		profile_duplicates = g_profile_duplicates;
	}

	std::ostringstream json;
	json << "{\n"
		<< "  \"schema\": 5,\n"
		<< "  \"addon\": \"HD2 Armature Profile Runtime\",\n"
		<< "  \"experiment_mode\": \"" << experiment_mode_name() << "\",\n"
		<< "  \"api_version\": " << RESHADE_API_VERSION << ",\n"
		<< "  \"process_id\": " << GetCurrentProcessId() << ",\n"
		<< "  \"window_handle\": " << reinterpret_cast<uintptr_t>(g_game_window) << ",\n"
		<< "  \"session_id\": " << g_session_id << ",\n"
		<< "  \"updated_unix_ms\": " << unix_time_ms() << ",\n"
		<< "  \"frame\": " << g_frame << ",\n"
		<< "  \"draws_last_frame\": " << draws << ",\n"
		<< "  \"tracked_buffers\": " << buffer_count << ",\n"
		<< "  \"mapped_buffers\": " << mapped_count << ",\n"
		<< "  \"profile_directory\": \"" << json_escape(utf8(g_profile_directory.c_str())) << "\",\n"
		<< "  \"profile_files_loaded\": " << profile_files << ",\n"
		<< "  \"profile_table_records\": " << profile_records << ",\n"
		<< "  \"profile_duplicate_records\": " << profile_duplicates << ",\n"
		<< "  \"profile_tables_loaded\": " << profiles.size() << ",\n"
		<< "  \"profile_load_errors\": " << profile_errors.size() << ",\n"
		<< "  \"shoulder_config_targets\": " << g_shoulder_targets.size() << ",\n"
		<< "  \"shoulder_toggle_pending\": "
		<< (g_shoulder_pending.load(std::memory_order_relaxed) ? "true" : "false") << ",\n"
		<< "  \"shoulder_narrow_active\": "
		<< (g_shoulder_active.load(std::memory_order_relaxed) ? "true" : "false") << ",\n"
		<< "  \"shoulder_toggle_count\": " << g_shoulder_toggles << ",\n"
		<< "  \"shoulder_error\": \"" << json_escape(g_shoulder_error) << "\",\n"
		<< "  \"ib_hunt_phase\": " << g_hunt_phase.load(std::memory_order_relaxed) << ",\n"
		<< "  \"ib_hunt_passes\": " << g_hunt_passes.load(std::memory_order_relaxed) << ",\n"
		<< "  \"ib_hunt_bytes\": " << g_hunt_bytes.load(std::memory_order_relaxed) << ",\n"
		<< "  \"converted_ib_hits\": " << hits.size() << ",\n"
		<< "  \"converted_ib_partial_candidates\": " << g_partial_candidates.load(std::memory_order_relaxed) << ",\n"
		<< "  \"converted_ib_best_partial_entries\": " << g_best_partial_entries.load(std::memory_order_relaxed) << ",\n"
		<< "  \"converted_ib_tables\": [";
	for (size_t index = 0; index < hits.size(); ++index)
	{
		const converted_hit &hit = hits[index];
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
				<< profile.unit_id << "\",\"lod_mask\":\"0x" << std::setw(8)
				<< profile.lod_mask << "\",\"entries\":" << std::dec << profile.entries
				<< ",\"source_records\":" << profile.source_records;
		}
		json << ",\"region_base\":\"0x" << std::hex << hit.region_base << "\",\"region_size\":"
			<< std::dec << hit.region_size << ",\"protection\":" << hit.protection << '}';
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
	std::lock_guard lock(g_mutex);
	if (g_device != nullptr)
		return;
	g_device = device;
	g_hunt_not_before = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	initialize_runtime_files();
	load_profiles();
	load_shoulder_targets();
	open_console();
}

void on_destroy_device(device *device)
{
	if (device != g_device)
		return;
	end_edit();
	stop_hunt_thread();
	std::lock_guard lock(g_mutex);
	g_buffers.clear();
	g_hits.clear();
	g_profiles.clear();
	g_active_targets.clear();
	g_shoulder_targets.clear();
	g_shoulder_pending.store(false, std::memory_order_release);
	g_shoulder_active.store(false, std::memory_order_release);
	g_device = nullptr;
}

void on_init_resource(device *device, const resource_desc &desc, const subresource_data *,
	resource_usage, resource resource)
{
	if (device != g_device || desc.type != resource_type::buffer)
		return;
	std::lock_guard lock(g_mutex);
	g_buffers[resource.handle] = { desc.buffer.size };
}

void on_destroy_resource(device *device, resource resource)
{
	if (device != g_device)
		return;
	std::lock_guard lock(g_mutex);
	g_buffers.erase(resource.handle);
}

void on_map_buffer(device *device, resource resource, uint64_t offset, uint64_t size,
	map_access, void **data)
{
	if (device != g_device || data == nullptr || *data == nullptr)
		return;
	std::lock_guard lock(g_mutex);
	auto found = g_buffers.find(resource.handle);
	if (found == g_buffers.end())
		return;
	found->second.map_ptr = *data;
	found->second.map_size = size == 0 || size == UINT64_MAX ?
		(offset < found->second.size ? found->second.size - offset : 0) : size;
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
		found->second.map_size = 0;
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
	if ((GetAsyncKeyState(VK_F8) & 1) && game_has_focus())
		toggle_shoulder_edit();
	if (GetAsyncKeyState(VK_F9) & 1)
	{
		if (g_shoulder_active.load(std::memory_order_acquire))
		{
			end_edit();
			g_shoulder_active.store(false, std::memory_order_release);
		}
		g_shoulder_pending.store(false, std::memory_order_release);
		request_rescan();
	}
	if (g_hunt_phase.load(std::memory_order_acquire) == 0 &&
		std::chrono::steady_clock::now() >= g_hunt_not_before)
		ensure_hunt_thread();
	service_shoulder_edit();
	maintain_edit();
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
