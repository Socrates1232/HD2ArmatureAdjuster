"""HD2 Armature Adapter: author and export Stage-1 custom armature rigs."""

from __future__ import annotations

import os
import pathlib

import bpy
from bpy.props import BoolProperty, EnumProperty, PointerProperty, StringProperty
from bpy.types import Operator, Panel, PropertyGroup
from mathutils import Matrix

from .core import bone_name_hash, build_rig, parent_matches, read_source, write_rig


bl_info = {
    "name": "HD2 Armature Adapter",
    "author": "HD2ArmatureAdjuster contributors",
    "version": (1, 1, 1),
    "blender": (4, 2, 0),
    "location": "View3D > Sidebar > HD2AA",
    "description": "Port compatible custom rest armatures to HD2RIG1",
    "category": "Rigging",
}


STABLE_ID = "hd2_stable_id"
SOURCE_PATH = "hd2_source_reference"
EXACT_NAME_MAPPING = "hd2_exact_name_mapping"


def _matrix(values):
    return Matrix((values[0:4], values[4:8], values[8:12], values[12:16]))


def _flat(matrix):
    return [float(matrix[row][column]) for row in range(4) for column in range(4)]


def _unique_name(existing, desired):
    if desired not in existing:
        return desired
    index = 1
    while f"{desired}.{index:03d}" in existing:
        index += 1
    return f"{desired}.{index:03d}"


def resolve_source_path(settings):
    candidates = []
    target = settings.target_object
    if target is not None and target.get(SOURCE_PATH):
        candidates.append(target.get(SOURCE_PATH))
    if settings.source_path:
        candidates.append(bpy.path.abspath(settings.source_path))
    if bpy.data.filepath:
        beside_blend = list(pathlib.Path(bpy.data.filepath).parent.glob("*.hd2source.json"))
        if len(beside_blend) == 1:
            candidates.append(str(beside_blend[0]))
    for candidate in candidates:
        path = os.path.abspath(candidate)
        if path.endswith(".hd2source.json") and os.path.isfile(path):
            read_source(path)
            return path
    raise ValueError("this armature has no HD2 source contract; select a generated "
                     ".hd2source.json (not a .patch_N file) once")


def create_source_armature(context, source_path):
    source = read_source(source_path)
    armature = bpy.data.armatures.new("HD2AA_Source")
    obj = bpy.data.objects.new("HD2AA_Source", armature)
    context.collection.objects.link(obj)
    context.view_layer.objects.active = obj
    obj.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    names = {}
    edit_by_id = {}
    name_by_id = {}
    for record in source["bones"]:
        name = _unique_name(names, record.get("display_name") or record["stable_id"])
        names[name] = True
        bone = armature.edit_bones.new(name)
        bone.matrix = _matrix(record["source_rest_global"])
        bone.length = 0.05
        edit_by_id[record["stable_id"]] = bone
        name_by_id[record["stable_id"]] = name
    for record in source["bones"]:
        parent_id = record["parent_id"]
        if parent_id is not None:
            edit_by_id[record["stable_id"]].parent = edit_by_id[parent_id]
    bpy.ops.object.mode_set(mode="OBJECT")
    for stable_id, name in name_by_id.items():
        armature.bones[name][STABLE_ID] = stable_id
    obj[SOURCE_PATH] = os.path.abspath(source_path)
    obj["hd2_source_definition_id"] = source["source_definition_id"]
    return obj


def collect_target(obj, source_path=None):
    if obj is None or obj.type != "ARMATURE":
        raise ValueError("select a target armature")
    if max(abs(float(obj.matrix_world[row][column]) - (1.0 if row == column else 0.0))
           for row in range(4) for column in range(4)) > 1e-6:
        raise ValueError("apply the target armature object's transforms before export")
    source = read_source(source_path or obj.get(SOURCE_PATH, ""))
    source_by_id = {bone["stable_id"]: bone for bone in source["bones"]}
    local = {bone["stable_id"]: list(bone["source_rest_local"])
             for bone in source["bones"]}
    parents = {bone["stable_id"]: bone["parent_id"] for bone in source["bones"]}
    stable_values = [bone.get(STABLE_ID) for bone in obj.data.bones if bone.get(STABLE_ID)]
    if len(stable_values) != len(set(stable_values)):
        raise ValueError("target contains duplicate stable IDs")
    by_stable_id = {bone.get(STABLE_ID): bone for bone in obj.data.bones
                    if bone.get(STABLE_ID)}
    by_name_hash = {}
    for bone in obj.data.bones:
        by_name_hash.setdefault(f"{bone_name_hash(bone.name):08x}", []).append(bone)
    mapped = 0
    for record in source["bones"]:
        bone = by_stable_id.get(record["stable_id"])
        if bone is None:
            candidates = by_name_hash.get(record["name_hash"].lower(), [])
            if len(candidates) > 1:
                raise ValueError(f"{record['display_name']}: multiple target bones have the "
                                 "same HD2 name hash")
            bone = candidates[0] if candidates else None
        if bone is None:
            continue
        parent_id = record["parent_id"]
        actual_parent = bone.parent
        actual_parent_name = None if actual_parent is None else actual_parent.name
        actual_parent_id = None if actual_parent is None else actual_parent.get(STABLE_ID)
        if not parent_matches(source_by_id, parent_id, actual_parent_name, actual_parent_id):
            expected = None if parent_id is None else source_by_id[parent_id]["name_hash"]
            raise ValueError(f"{bone.name}: target parent {actual_parent_name!r} differs from "
                             f"source parent hash {expected!r}")
        matrix = bone.matrix_local if bone.parent is None else bone.parent.matrix_local.inverted() @ bone.matrix_local
        local[record["stable_id"]] = _flat(matrix)
        parents[record["stable_id"]] = parent_id
        mapped += 1
    if mapped == 0:
        raise ValueError("the selected armature has no HD2 bone-name matches with the source")
    return local, parents


def mapping_summary(obj, source_path):
    source = read_source(source_path)
    source_ids = {bone["stable_id"] for bone in source["bones"]}
    stable_ids = {bone.get(STABLE_ID) for bone in obj.data.bones if bone.get(STABLE_ID)}
    if source_ids <= stable_ids:
        return "stable IDs", len(source_ids), len(source_ids)
    name_hashes = {f"{bone_name_hash(bone.name):08x}" for bone in obj.data.bones}
    mapped = sum(bone["stable_id"] in stable_ids or bone["name_hash"].lower() in name_hashes
                 for bone in source["bones"])
    return "stable IDs/name hashes", mapped, len(source["bones"])


def package_from_scene(settings):
    if settings.target_object is None:
        raise ValueError("select the modified armature first")
    source_path = resolve_source_path(settings)
    settings.source_path = source_path
    settings.target_object[SOURCE_PATH] = source_path
    local, parents = collect_target(settings.target_object, source_path)
    return build_rig(source_path, local, parents, settings.basis_mode,
                     settings.capability, settings.notes)


class HD2AASettings(PropertyGroup):
    source_path: StringProperty(name="Source contract", subtype="FILE_PATH")
    output_path: StringProperty(name="Rig output", subtype="FILE_PATH",
                                default="//target.hd2rig.json")
    target_object: PointerProperty(name="Modified armature", type=bpy.types.Object,
                                   poll=lambda _, obj: obj.type == "ARMATURE")
    basis_mode: EnumProperty(name="Basis mode", items=(
        ("preserved", "Preserved", "Require target rest bases to remain compatible"),
        ("position_normalized", "Position normalized", "Use edited positions with source bases"),
        ("exact_full", "Exact full", "Export exact target bases; requires full pose input")),
        default="preserved")
    capability: EnumProperty(name="Capability", items=(
        ("auto", "Automatic", "Choose the least demanding valid runtime path"),
        ("linear_rest_translation", "Linear translation", "Require compatible rest bases"),
        ("full_native_pose", "Full native pose", "Require full live matrices")), default="auto")
    notes: StringProperty(name="Notes")
    overwrite: BoolProperty(name="Overwrite", default=False)
    status: StringProperty(name="Status", default="Select a modified armature")


class HD2AA_OT_use_selected(Operator):
    bl_idname = "hd2aa.use_selected"
    bl_label = "Use Selected Armature"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        target = context.active_object
        if target is None or target.type != "ARMATURE":
            self.report({"ERROR"}, "select an armature object first")
            return {"CANCELLED"}
        settings = context.scene.hd2aa
        settings.target_object = target
        if target.get(SOURCE_PATH):
            settings.source_path = target.get(SOURCE_PATH)
        settings.status = "Target selected; choose the source contract if it was not embedded"
        self.report({"INFO"}, "Using selected armature: " + target.name)
        return {"FINISHED"}


class HD2AA_OT_import_source(Operator):
    bl_idname = "hd2aa.import_source"
    bl_label = "Import Source Armature"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            obj = create_source_armature(context, bpy.path.abspath(context.scene.hd2aa.source_path))
            context.scene.hd2aa.target_object = obj
            context.scene.hd2aa.status = "Source imported; duplicate it before editing"
            self.report({"INFO"}, f"Imported {len(obj.data.bones)} stable bones")
            return {"FINISHED"}
        except (OSError, ValueError, KeyError) as error:
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}


class HD2AA_OT_duplicate_target(Operator):
    bl_idname = "hd2aa.duplicate_target"
    bl_label = "Duplicate Source as Target"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        source = context.scene.hd2aa.target_object or context.active_object
        if source is None or source.type != "ARMATURE":
            self.report({"ERROR"}, "Import or select a source armature first")
            return {"CANCELLED"}
        target = source.copy()
        target.data = source.data.copy()
        target.name = "HD2AA_Target"
        context.collection.objects.link(target)
        context.scene.hd2aa.target_object = target
        context.scene.hd2aa.status = "Target duplicate ready for Edit Mode changes"
        context.view_layer.objects.active = target
        source.select_set(False)
        target.select_set(True)
        return {"FINISHED"}


class HD2AA_OT_map_exact_names(Operator):
    bl_idname = "hd2aa.map_exact_names"
    bl_label = "Map Stable IDs by Exact Name"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        settings = context.scene.hd2aa
        target = settings.target_object
        try:
            source_path = resolve_source_path(settings)
            mode, mapped, total = mapping_summary(target, source_path)
            target[EXACT_NAME_MAPPING] = True
            target[SOURCE_PATH] = source_path
            settings.source_path = source_path
            settings.status = f"Mapping: {mapped}/{total} source records by {mode}"
            self.report({"INFO"}, f"{mode}: {mapped}/{total}; unmatched source-only bones stay unchanged")
            return {"FINISHED"}
        except (AttributeError, OSError, ValueError, KeyError) as error:
            settings.status = "MAPPING FAILED: " + str(error)
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}


class HD2AA_OT_validate(Operator):
    bl_idname = "hd2aa.validate"
    bl_label = "Validate Target"

    def execute(self, context):
        settings = context.scene.hd2aa
        try:
            rig = package_from_scene(settings)
            mode, mapped, total = mapping_summary(settings.target_object,
                                                  resolve_source_path(settings))
            changed = sum(max(abs(a - b) for a, b in zip(
                bone["source_rest_local"], bone["target_rest_local"])) > 1e-7
                for bone in rig["bones"])
            settings.status = f"VALID: {changed} changed; {mapped}/{total} mapped by {mode}"
            self.report({"INFO"}, f"Valid: {changed} changed, {mapped}/{total} by {mode}; "
                                   f"{rig['requested_capability']}")
            return {"FINISHED"}
        except (OSError, ValueError, KeyError) as error:
            settings.status = "INVALID: " + str(error)
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}


class HD2AA_OT_export(Operator):
    bl_idname = "hd2aa.export"
    bl_label = "Validate & Export Port"

    def execute(self, context):
        settings = context.scene.hd2aa
        try:
            rig = package_from_scene(settings)
            digest = write_rig(bpy.path.abspath(settings.output_path), rig, settings.overwrite)
            settings.status = "EXPORTED: " + digest[:12]
            self.report({"INFO"}, "Exported " + digest[:12])
            return {"FINISHED"}
        except (OSError, ValueError, KeyError) as error:
            settings.status = "EXPORT FAILED: " + str(error)
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}


class HD2AA_PT_panel(Panel):
    bl_label = "HD2 Armature Adapter"
    bl_idname = "HD2AA_PT_panel"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "HD2AA"

    def draw(self, context):
        layout = self.layout
        settings = context.scene.hd2aa
        layout.label(text="1. Select your modified avatar armature")
        layout.prop(settings, "target_object")
        layout.operator("hd2aa.use_selected")
        layout.label(text="Exports rest bones; Pose Mode changes are ignored", icon="INFO")
        layout.separator()
        layout.label(text="2. Binding contract (resolved once)")
        layout.prop(settings, "source_path")
        layout.label(text="Use *.hd2source.json, not *.patch_N", icon="INFO")
        layout.operator("hd2aa.map_exact_names", text="Check Automatic Mapping")
        row = layout.row(align=True)
        row.operator("hd2aa.import_source", text="Import Source")
        row.operator("hd2aa.duplicate_target", text="Duplicate Imported Source")
        layout.separator()
        layout.label(text="3. Validate and port")
        layout.prop(settings, "basis_mode")
        layout.prop(settings, "capability")
        layout.prop(settings, "notes")
        layout.operator("hd2aa.validate")
        layout.prop(settings, "output_path")
        layout.prop(settings, "overwrite")
        layout.operator("hd2aa.export")
        box = layout.box()
        box.label(text=settings.status)


CLASSES = (HD2AASettings, HD2AA_OT_use_selected, HD2AA_OT_import_source, HD2AA_OT_duplicate_target,
           HD2AA_OT_map_exact_names, HD2AA_OT_validate, HD2AA_OT_export, HD2AA_PT_panel)


def register():
    for value in CLASSES:
        bpy.utils.register_class(value)
    bpy.types.Scene.hd2aa = PointerProperty(type=HD2AASettings)


def unregister():
    del bpy.types.Scene.hd2aa
    for value in reversed(CLASSES):
        bpy.utils.unregister_class(value)
