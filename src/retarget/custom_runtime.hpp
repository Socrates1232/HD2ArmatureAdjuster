#pragma once

#include "live_palette.hpp"
#include "publisher.hpp"
#include "runtime_adapter.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hd2aa::retarget
{
struct runtime_profile_data
{
	runtime_profile_identity identity;
	std::vector<uint8_t> pristine;
};

struct discovered_bind_instance
{
	uintptr_t address = 0;
	size_t profile_index = 0;
	uint64_t discovery_generation = 0;
};

enum class custom_status
{
	disabled,
	waiting_for_bind_instance,
	waiting_for_live_pose,
	live_layout_unverified,
	subject_ambiguous,
	applied,
	restore_pending,
	dirty_unknown,
	configuration_error,
};

inline const char *custom_status_name(custom_status value)
{
	switch (value)
	{
	case custom_status::disabled: return "DISABLED";
	case custom_status::waiting_for_bind_instance: return "WAITING_FOR_BIND_INSTANCE";
	case custom_status::waiting_for_live_pose: return "WAITING_FOR_LIVE_POSE";
	case custom_status::live_layout_unverified: return "LIVE_LAYOUT_UNVERIFIED";
	case custom_status::subject_ambiguous: return "SUBJECT_AMBIGUOUS";
	case custom_status::applied: return "APPLIED";
	case custom_status::restore_pending: return "RESTORE_PENDING";
	case custom_status::dirty_unknown: return "DIRTY_UNKNOWN";
	case custom_status::configuration_error: return "CONFIGURATION_ERROR";
	default: return "UNKNOWN";
	}
}

struct custom_metrics
{
	uint64_t mapped_bytes_scanned = 0;
	uint64_t palette_candidates = 0;
	uint64_t pose_samples = 0;
	uint64_t plans_built = 0;
	uint64_t publications = 0;
	uint64_t restores = 0;
	uint64_t rejected_instances = 0;
};

class custom_runtime
{
public:
	bool configure(rig_package rig, std::vector<runtime_profile_data> profiles,
		std::string &error)
	{
		clear();
		if (!rig.linear_capability)
		{
			error = "Stage 1 runtime currently requires linear_rest_translation";
			_status = custom_status::configuration_error;
			return false;
		}
		std::vector<runtime_profile_identity> identities;
		for (const runtime_profile_data &profile : profiles)
			identities.push_back(profile.identity);
		const association_result association = associate_tables(rig, identities);
		if (!association.valid)
		{
			error = association.errors.empty() ? "profile association failed" : association.errors.front();
			_status = custom_status::configuration_error;
			return false;
		}
		_rig = std::move(rig);
		_profiles = std::move(profiles);
		for (const table_association &item : association.tables)
		{
			if (!table_has_affected_slots(_rig, _rig.tables[item.rig_table_index]))
				continue;
			binding state;
			state.rig_table_index = item.rig_table_index;
			state.profile_index = item.profile_index;
			state.current_bind = _profiles[item.profile_index].pristine;
			_bindings.push_back(std::move(state));
		}
		_configured = true;
		_status = custom_status::disabled;
		return true;
	}

	void clear()
	{
		_configured = false;
		_desired = false;
		_status = custom_status::disabled;
		_rig = {};
		_profiles.clear();
		_bindings.clear();
		_instances.clear();
		_metrics = {};
	}

	bool configured() const { return _configured; }
	bool desired() const { return _desired; }
	custom_status status() const { return _status; }
	const custom_metrics &metrics() const { return _metrics; }
	const rig_package &rig() const { return _rig; }
	bool latest_pose_location(uint64_t &resource, uint64_t &offset, uint64_t &frame) const
	{
		const binding *latest = nullptr;
		for (const binding &item : _bindings)
			if (item.pose && (latest == nullptr || item.pose_frame > latest->pose_frame))
				latest = &item;
		if (latest == nullptr)
			return false;
		resource = latest->pose_resource;
		offset = latest->pose_offset;
		frame = latest->pose_frame;
		return true;
	}

	bool toggle()
	{
		if (!_configured)
		{
			_status = custom_status::configuration_error;
			return false;
		}
		_desired = !_desired;
		_status = _desired ? custom_status::waiting_for_bind_instance : custom_status::restore_pending;
		return _desired;
	}

	void observe_window(const uint8_t *data, size_t size, uint64_t resource,
		uint64_t absolute_offset, uint64_t frame)
	{
		if (!_configured || !_desired || data == nullptr || size == 0)
			return;
		_metrics.mapped_bytes_scanned += size;
		std::vector<table_binding_view> views;
		views.reserve(_bindings.size());
		for (const binding &item : _bindings)
			views.push_back({ item.rig_table_index, &item.current_bind });
		std::vector<palette_candidate> found = find_latest_live_palettes(data, size, _rig, views);
		for (size_t index = 0; index < _bindings.size(); ++index)
		{
			if (!found[index].found)
				continue;
			binding &item = _bindings[index];
			++_metrics.palette_candidates;
			item.pose = std::move(found[index].pose);
			item.pose_frame = frame;
			item.pose_resource = resource;
			item.pose_offset = absolute_offset + found[index].offset;
			item.source_sample_id = (frame << 24) ^ (resource << 3) ^ item.pose_offset;
			++_metrics.pose_samples;
		}
	}

	template <typename Read, typename Write>
	void service(const std::vector<discovered_bind_instance> &discovered,
		uint64_t discovery_generation, uint64_t frame, Read read, Write write)
	{
		if (!_configured)
			return;
		if (!_desired)
		{
			restore_all(read, write);
			return;
		}
		bool any_instance = false;
		bool any_pose = false;
		bool any_applied = false;
		for (binding &item : _bindings)
		{
			std::vector<discovered_bind_instance> candidates;
			uint64_t newest_generation = 0;
			for (const auto &candidate : discovered)
				if (candidate.profile_index == item.profile_index)
					newest_generation = std::max(newest_generation, candidate.discovery_generation);
			for (const auto &candidate : discovered)
				if (candidate.profile_index == item.profile_index &&
					candidate.discovery_generation == newest_generation)
					candidates.push_back(candidate);
			if (candidates.empty())
			{
				for (const instance &owned : _instances)
					if (owned.profile_index == item.profile_index)
						candidates.push_back({ owned.address, owned.profile_index, discovery_generation });
			}
			if (candidates.empty())
				continue;
			std::sort(candidates.begin(), candidates.end(),
				[](const discovered_bind_instance &left, const discovered_bind_instance &right) {
					return left.address < right.address;
				});
			candidates.erase(std::unique(candidates.begin(), candidates.end(),
				[](const discovered_bind_instance &left, const discovered_bind_instance &right) {
					return left.address == right.address;
				}), candidates.end());
			any_instance = true;
			if (!item.pose || frame > item.pose_frame + 3)
				continue;
			any_pose = true;
			const rig_table &table = _rig.tables[item.rig_table_index];
			const live_plan plan = build_live_translation_plan(_rig, table, item.pose,
				_profiles[item.profile_index].pristine);
			if (!plan)
			{
				_status = plan.failure == pose_error::missing_required_pose ?
					custom_status::live_layout_unverified : custom_status::configuration_error;
				continue;
			}
			++_metrics.plans_built;
			bool binding_applied = false;
			for (const discovered_bind_instance &candidate : candidates)
			{
				instance *target = find_instance(candidate.address, item.profile_index);
				if (target == nullptr)
				{
					instance created;
					created.address = candidate.address;
					created.profile_index = item.profile_index;
					created.rig_table_index = item.rig_table_index;
					created.publication_state.expected = _profiles[item.profile_index].pristine;
					_instances.push_back(std::move(created));
					target = &_instances.back();
				}
				publish_result published = publish_table(target->address,
					target->publication_state, plan.table.bytes, plan.table.changed_slots,
					item.source_sample_id, read, write);
				if (published.status == publish_status::stale_expected)
				{
					std::vector<uint8_t> current(_profiles[item.profile_index].pristine.size());
					if (read(target->address, current.data(), current.size()) &&
						current == _profiles[item.profile_index].pristine)
					{
						target->publication_state.expected = current;
						published = publish_table(target->address, target->publication_state,
							plan.table.bytes, plan.table.changed_slots,
							item.source_sample_id, read, write);
					}
				}
				if (published.status == publish_status::applied ||
					published.status == publish_status::no_change)
				{
					target->last_frame = frame;
					binding_applied = true;
					if (published.status == publish_status::applied) ++_metrics.publications;
				}
				else if (published.status == publish_status::dirty_unknown)
				{
					_status = custom_status::dirty_unknown;
					return;
				}
			}
			if (binding_applied)
			{
				item.current_bind = plan.table.bytes;
				any_applied = true;
			}
		}
		_status = any_applied ? custom_status::applied :
			(any_pose ? custom_status::live_layout_unverified :
			(any_instance ? custom_status::waiting_for_live_pose :
			 custom_status::waiting_for_bind_instance));
	}

private:
	struct binding
	{
		size_t rig_table_index = 0;
		size_t profile_index = 0;
		std::vector<uint8_t> current_bind;
		pose_sample pose;
		uint64_t pose_frame = 0;
		uint64_t pose_resource = 0;
		uint64_t pose_offset = 0;
		uint64_t source_sample_id = 0;
	};

	struct instance
	{
		uintptr_t address = 0;
		size_t profile_index = 0;
		size_t rig_table_index = 0;
		publication publication_state;
		uint64_t last_frame = 0;
	};

	bool _configured = false;
	bool _desired = false;
	custom_status _status = custom_status::disabled;
	rig_package _rig;
	std::vector<runtime_profile_data> _profiles;
	std::vector<binding> _bindings;
	std::vector<instance> _instances;
	custom_metrics _metrics;

	instance *find_instance(uintptr_t address, size_t profile_index)
	{
		for (instance &item : _instances)
			if (item.address == address && item.profile_index == profile_index)
				return &item;
		return nullptr;
	}

	template <typename Read, typename Write>
	bool retire_profile_instance(size_t profile_index, Read read, Write write)
	{
		bool complete = true;
		for (auto item = _instances.begin(); item != _instances.end();)
		{
			if (item->profile_index != profile_index)
			{
				++item;
				continue;
			}
			const auto &pristine = _profiles[profile_index].pristine;
			std::vector<size_t> slots;
			for (size_t slot = 0; slot < pristine.size() / 48; ++slot)
				if (std::memcmp(pristine.data() + slot * 48,
					item->publication_state.expected.data() + slot * 48, 48) != 0)
					slots.push_back(slot);
			const publish_result restored = publish_table(item->address,
				item->publication_state, pristine, slots, 0, read, write);
			if (restored.status == publish_status::applied ||
				restored.status == publish_status::no_change)
			{
				++_metrics.restores;
				item = _instances.erase(item);
				continue;
			}
			if (restored.status == publish_status::stale_expected)
			{
				std::vector<uint8_t> current(pristine.size());
				if (!read(item->address, current.data(), current.size()) || current == pristine)
				{
					item = _instances.erase(item);
					continue;
				}
			}
			if (restored.status == publish_status::dirty_unknown)
				_status = custom_status::dirty_unknown;
			else
				_status = custom_status::restore_pending;
			complete = false;
			++item;
		}
		return complete;
	}

	template <typename Read, typename Write>
	void restore_all(Read read, Write write)
	{
		bool complete = true;
		for (size_t profile = 0; profile < _profiles.size(); ++profile)
			complete = retire_profile_instance(profile, read, write) && complete;
		if (!complete)
			return;
		for (binding &item : _bindings)
			item.current_bind = _profiles[item.profile_index].pristine;
		if (_status != custom_status::dirty_unknown)
			_status = custom_status::disabled;
	}
};
}
