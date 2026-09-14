"""HD2 Armature Adapter: author and export Stage-1 custom armature rigs."""

from __future__ import annotations

import os

import bpy
from bpy.props import BoolProperty, EnumProperty, PointerProperty, StringProperty
from bpy.types import Operator, Panel, PropertyGroup
from mathutils import Matrix

from .core import build_rig, read_source, write_rig


bl_info = {
    "name": "HD2 Armature Adapter",
    "author": "HD2ArmatureAdjuster contributors",
    "version": (1, 0, 0),
    "blender": (4, 2, 0),
    "location": "View3D > Sidebar > HD2AA",
    "description": "Port compatible custom rest armatures to HD2RIG1",
    "category": "Rigging",
}


STABLE_ID = "hd2_stable_id"
SOURCE_PATH = "hd2_source_reference"


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
    for record in source["bones"]:
        name = _unique_name(names, record.get("display_name") or record["stable_id"])
        names[name] = True
        bone = armature.edit_bones.new(name)
        bone.matrix = _matrix(record["source_rest_global"])
        bone.length = 0.05
        edit_by_id[record["stable_id"]] = bone
    for record in source["bones"]:
        parent_id = record["parent_id"]
        if parent_id is not None:
            edit_by_id[record["stable_id"]].parent = edit_by_id[parent_id]
    bpy.ops.object.mode_set(mode="OBJECT")
    for stable_id, edit_bone in edit_by_id.items():
        armature.bones[edit_bone.name][STABLE_ID] = stable_id
    obj[SOURCE_PATH] = os.path.abspath(source_path)
    obj["hd2_source_definition_id"] = source["source_definition_id"]
    return obj


def collect_target(obj):
    if obj is None or obj.type != "ARMATURE":
        raise ValueError("select a target armature")
    local = {}
    parents = {}
    for bone in obj.data.bones:
        stable_id = bone.get(STABLE_ID)
        if not stable_id:
            continue
        if stable_id in local:
            raise ValueError("duplicate stable ID on target: " + stable_id)
        matrix = bone.matrix_local if bone.parent is None else bone.parent.matrix_local.inverted() @ bone.matrix_local
        local[stable_id] = _flat(matrix)
        parents[stable_id] = None if bone.parent is None else bone.parent.get(STABLE_ID)
    return local, parents


def package_from_scene(settings):
    local, parents = collect_target(settings.target_object)
    return build_rig(settings.source_path, local, parents, settings.basis_mode,
                     settings.capability, settings.notes)


class HD2AASettings(PropertyGroup):
    source_path: StringProperty(name="Source reference", subtype="FILE_PATH")
    output_path: StringProperty(name="Rig output", subtype="FILE_PATH")
    target_object: PointerProperty(name="Target", type=bpy.types.Object)
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


class HD2AA_OT_import_source(Operator):
    bl_idname = "hd2aa.import_source"
    bl_label = "Import Source Armature"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            obj = create_source_armature(context, bpy.path.abspath(context.scene.hd2aa.source_path))
            context.scene.hd2aa.target_object = obj
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
            source = read_source(bpy.path.abspath(settings.source_path))
            by_name = {bone.name: bone for bone in target.data.bones}
            mapped = 0
            for record in source["bones"]:
                bone = by_name.get(record.get("display_name", record["stable_id"]))
                if bone is not None:
                    bone[STABLE_ID] = record["stable_id"]
                    mapped += 1
            self.report({"INFO"}, f"Mapped {mapped}/{len(source['bones'])} bones")
            return {"FINISHED"}
        except (AttributeError, OSError, ValueError, KeyError) as error:
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}


class HD2AA_OT_validate(Operator):
    bl_idname = "hd2aa.validate"
    bl_label = "Validate Target"

    def execute(self, context):
        try:
            rig = package_from_scene(context.scene.hd2aa)
            self.report({"INFO"}, f"Valid {len(rig['bones'])} bones, {len(rig['tables'])} tables; {rig['requested_capability']}")
            return {"FINISHED"}
        except (OSError, ValueError, KeyError) as error:
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}


class HD2AA_OT_export(Operator):
    bl_idname = "hd2aa.export"
    bl_label = "Export HD2RIG1"

    def execute(self, context):
        settings = context.scene.hd2aa
        try:
            rig = package_from_scene(settings)
            digest = write_rig(bpy.path.abspath(settings.output_path), rig, settings.overwrite)
            self.report({"INFO"}, "Exported " + digest[:12])
            return {"FINISHED"}
        except (OSError, ValueError, KeyError) as error:
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
        layout.prop(settings, "source_path")
        layout.operator("hd2aa.import_source")
        layout.prop(settings, "target_object")
        row = layout.row(align=True)
        row.operator("hd2aa.duplicate_target")
        row.operator("hd2aa.map_exact_names")
        layout.prop(settings, "basis_mode")
        layout.prop(settings, "capability")
        layout.prop(settings, "notes")
        layout.operator("hd2aa.validate")
        layout.prop(settings, "output_path")
        layout.prop(settings, "overwrite")
        layout.operator("hd2aa.export")


CLASSES = (HD2AASettings, HD2AA_OT_import_source, HD2AA_OT_duplicate_target,
           HD2AA_OT_map_exact_names, HD2AA_OT_validate, HD2AA_OT_export, HD2AA_PT_panel)


def register():
    for value in CLASSES:
        bpy.utils.register_class(value)
    bpy.types.Scene.hd2aa = PointerProperty(type=HD2AASettings)


def unregister():
    del bpy.types.Scene.hd2aa
    for value in reversed(CLASSES):
        bpy.utils.unregister_class(value)
