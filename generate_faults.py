#!/usr/bin/env python3

import json
import os
import pathlib
from subprocess import PIPE, run


def format_json(json_in):
    p = run(['tools\\format\\json_formatter.exe'], input=json_in, stdout=PIPE, stderr=PIPE, text=True, encoding='utf-8')
    return p.stdout


def object_to_json(obj):
    json_string = json.dumps(obj, ensure_ascii=False)
    return format_json(json_string)


def write_object_as_json_file(filename, json_in):
    with open(filename, "w", encoding="utf-8", newline='\n') as fp:
        json_formatted = object_to_json(json_in)
        fp.write(json_formatted)


def find_sources():
    sources = {"base": "data/json"}
    for d in next(os.walk('data/mods'))[1]:
        if d == 'TEST_DATA':
            continue
        sources[d] = os.path.normpath(os.path.join("data/mods", d))
    return sources


def process_mod_dir(d):
    res = []
    for root, directories, filenames in os.walk(d):
        for filename in filenames:
            if not filename.endswith(".json"):
                continue
            p = os.path.join(root, filename)
            try:
                with open(p, "r", encoding="utf-8") as json_file:
                    json_data = json.load(json_file)
                    for jo in json_data:
                        if (type(jo) is not dict) or ("type" not in jo) or ("id" not in jo):
                            continue
                        if jo["type"] == "material" and "repaired_with" in jo and jo["repaired_with"] != "null":
                            res.append(jo)
                            continue
                        use_actions = []
                        if "use_action" in jo:
                            if type(jo["use_action"]) == list and len(jo["use_action"]) > 0:
                                for a in jo["use_action"]:
                                    if type(a) == dict and a.get("type", "") == "repair_item":
                                        use_actions.append(a)
                            elif type(jo["use_action"]) == dict:
                                if jo["use_action"].get("type", "") == "repair_item":
                                    use_actions.append(jo["use_action"])
                        if len(use_actions) > 0:
                            jo["use_action"] = use_actions
                            res.append(jo)
                            continue
            except json.JSONDecodeError:
                print(f"Json Decode Error at {p}")
    return res


name_for_mat = {}  # maps material_id to name
fault_for_mat = {}  # maps material_id to its fault object
spare_for_mat = {}  # maps material_id to it's patching item e.g. cotton -> cotton_patchwork


def deduce_tool_ammo_type(jo, base_data, mod_data):
    ammo_types = []
    if jo['id'] == "toolset":
        return ["battery"]
    for at in jo.get("ammo", []):
        if at not in ammo_types:
            ammo_types.append(at)
    if "pocket_data" in jo:
        for p in jo["pocket_data"]:
            if p.get("pocket_type", "") != "MAGAZINE":
                continue
            ammo_rest = p.get("ammo_restriction", {})
            for at in ammo_rest.keys():
                if at not in ammo_types:
                    ammo_types.append(at)

    if len(ammo_types) == 0:
        base_tool = [x for x in base_data if x['id'] == jo['id']]
        if any(base_tool):
            return deduce_tool_ammo_type(base_tool[0], base_data, mod_data)

    return ammo_types


special_materials = {
    "sewing_standard": [ "leather", "lycra", "nylon", "fur", "faux_fur", "neoprene", "gutskin", "denim", "fae_fur", ],
    "sewing_aramids": ["kevlar", "kevlar_layered", "nomex", "demon_chitin", "black_dragon_hide", ],
    "sewing_simple": ["cotton", "wool", "canvas", ],
    "welding_standard": [ "iron", "steel", "copper", "bronze", "fancy_bronze", "lc_steel", "mc_steel", "hc_steel", "ch_steel", ],
    "welding_high_grade": ["qt_steel", "qt_steel_chain", ],
    "chainmail_standard": [ "budget_steel_chain", "lc_steel_chain", "mc_steel_chain", "hc_steel_chain", "ch_steel_chain" ],
    "welding_alloys": [ "aluminum", "superalloy", "platinum", "mithril_metal", "orichalcum_metal", "titanium", ],
    "repair_misc_standard": [ "bone", "chitin", "paper", "cardboard", "wood", "kevlar_rigid", "cardboard"],
    "soldering_standard": ["tin", "zinc", "silver", "gold", "lead", "plastic"],
    "bronzesmithing_tools": ["bronze"],
#    "xe_oneiric_repairs": ["moon_tears", "forged_dreamstuff", "moon_tears_fine_chain"]
}
# non-special: alien_liquid


def process_tools(faults_path, reqs_path, fixes_path, base_data, mod_data):
    new_faults = []
    new_reqs = []
    new_fixes = []
    fault_for_mat_in_mod = {}

    # fill in faults, component requirements and tool requirement stubs
    for jo in mod_data:
        if jo["type"] != "material":
            continue
        mat_id = jo['id']
        mat_name = jo['name'].lower()
        name_for_mat[mat_id] = mat_name
        new_fault = {
            "type": "fault",
            "id": f"damaged_{mat_id}",
            "name": {"str": f"Damaged {mat_name}"},
            "description": f"This item's {mat_name} parts are damaged.",
            "material_damage": mat_id,
            "mod_damage": 1000
        }
        fault_for_mat[mat_id] = new_fault
        fault_for_mat_in_mod[mat_id] = new_fault
        spare_for_mat[mat_id] = jo.get("repaired_with", "null")
        if spare_for_mat[mat_id] == "null":
            continue
        new_faults.append(new_fault)

    # fill in mending methods
    for jo in mod_data:
        if jo["type"] == "material":
            continue
        tool_id = jo["id"]
        ammo_type = deduce_tool_ammo_type(jo, base_data, mod_data)
        if type(ammo_type) != list or len(ammo_type) < 1:
            print(f"{reqs_path}: {tool_id} has weird ammo")
        ammo_count = jo.get("charges_per_use", 1)
        for u in jo["use_action"]:
            tool_skill = u.get("skill", "fabrication")
            tool_skill_level = max(1, 5 - u.get("tool_quality", 0))

            for mat_id in u["materials"]:
                if mat_id not in fault_for_mat:
                    print(f"skipping repair method for {mat_id}")
                    continue
                if mat_id not in fault_for_mat_in_mod:
                    # material isn't based in this mod - skip it
                    continue

                special_case = None
                for k, v in special_materials.items():
                    if mat_id in v:
                        special_case = k
                if special_case:
                    mend_method_id = f"fix_{mat_id}"
                    if any(x['id'] == mend_method_id for x in new_fixes):
                        continue
                    new_fixes.append({
                        "type": "fault_fix",
                        "id": mend_method_id,
                        "name": f"Repair {name_for_mat[mat_id]}",
                        "success_msg": f"You repair your %s.  ( %s → %s )",
                        "time": "30 m",
                        "faults_removed": [ "damaged_" + mat_id ],
                        "skills": { tool_skill: tool_skill_level },
                        "mod_damage": -1000,
                        "requirements": [
                            [special_case, 1],
                            { "components": [[[spare_for_mat[mat_id], 1]]] },
                        ]
                    })
                else:
                    fix = {
                        "type": "fault_fix",
                        "id": f"fix_{mat_id}",
                        "name": f"Repair {name_for_mat[mat_id]}",
                        "success_msg": f"You repair your %s.  ( %s → %s )",
                        "time": "30 m",
                        "faults_removed": [ "damaged_" + mat_id ],
                        "skills": { tool_skill: tool_skill_level },
                        "mod_damage": -1000,
                        "requirements": [],
                    }
                    reqs = fix["requirements"]
                    new_fixes.append(fix)
                    if len(ammo_type) == 1 and ammo_type[0] == "battery":
                        reqs.append({
                            "components": [[[spare_for_mat[mat_id], 1]]],
                            "tools": [[[tool_id, ammo_count]]],
                        })
                    else:
                        ammo_type_variants = [[x, ammo_count] for x in ammo_type]
                        req = {
                            "components": [[[spare_for_mat[mat_id], 1]] + ammo_type_variants],
                            "tools": [[[tool_id, -1]]],
                        }
                        reqs.append(req)

    if len(new_faults) > 0:
        new_faults.sort(key=lambda x: x["name"]["str"])
        write_object_as_json_file(faults_path, new_faults)
    if len(new_reqs) > 0:
        write_object_as_json_file(reqs_path, new_reqs)
    if len(new_fixes) > 0:
        new_fixes.sort(key=lambda x: x["name"])
        write_object_as_json_file(fixes_path, new_fixes)


def process():
    data = {}  # json data filtered for relevancy (materials, items with repair_item use_action etc.)
    materials = {}  # map of mod id to { "material_id" : "repair_tool" } in the mod
    sources = find_sources()

    for mod, path in sources.items():
        mod_data = process_mod_dir(path)
        data[mod] = mod_data

    for mod, mod_data in data.items():
        if mod not in materials:
            materials[mod] = {}
        mats = materials[mod]
        for jo in mod_data:
            if jo["type"] == "material":
                mats[jo["id"]] = jo["repaired_with"]
    print(object_to_json({f"{k} [{len(v)}]": v for k, v in materials.items() if len(v) > 0}))

    for mod, path in sources.items():
        faults_dir = os.path.join(path, "faults")
        pathlib.Path.mkdir(pathlib.Path(faults_dir), parents=True, exist_ok=True)
        faults_path = os.path.join(faults_dir, "faults_materials.json")
        reqs_path = os.path.join(faults_dir, "reqs_materials.json")
        fixes_path = os.path.join(faults_dir, "fixes_materials.json")
        process_tools(faults_path, reqs_path, fixes_path, data["base"], data[mod])


process()
