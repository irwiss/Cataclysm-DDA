#!/usr/bin/env python3

import argparse
import json
from math import ceil
import os
import shutil
from subprocess import PIPE, run

args_parser = argparse.ArgumentParser(formatter_class=argparse.ArgumentDefaultsHelpFormatter)
args_parser.add_argument("--base", help="base game json directory", default="./data")
args_parser.add_argument("--mods", help="list of mods to import, comma separated", default="")
args_parser.add_argument("--formatter", help="path to formatter executable", default="tools/format/json_formatter.exe")
args = vars(args_parser.parse_args())

formatter = args["formatter"]

if shutil.which(args["formatter"]) is None:
    print(f"formatter '{args['formatter']}' seems not existing or not runnable")
    exit(1)


def format_json(json_in):
    p = run([args["formatter"]], input=json_in, stdout=PIPE, stderr=PIPE, text=True, encoding='utf-8')
    return p.stdout


def object_to_json(obj):
    json_string = json.dumps(obj, ensure_ascii=False)
    return format_json(json_string)


def write_object_as_json_file(filename, json_in):
    with open(filename, "w", encoding="utf-8") as fp:
        json_formatted = object_to_json(json_in)
        fp.write(json_formatted)


def walk_json_files():
    res = []
    for root, directories, filenames in os.walk(args["base"]):
        if str(root).__contains__("Generic_Guns"):
            continue
        for filename in filenames:
            if filename.endswith(".json"):
                res.append(os.path.join(root, filename))
    return res


def get_all_objects():
    data = []

    for filename in walk_json_files():
        if not filename.endswith(".json"):
            continue

        with open(filename, "r", encoding="utf-8") as json_file:
            try:
                json_data = json.load(json_file)
            except json.JSONDecodeError:
                print(f"skipping {filename} due to JSONDecodeError")
                continue

        if type(json_data) is list:
            for elem in json_data:
                if type(elem) == dict and "type" in elem:
                    data.append(elem)
        elif type(json_data) is dict and "type" in json_data:
            data.append(json_data)
    
    return data

def id_for_item(x):
    if "id" in x: return x["id"]
    elif "abstract" in x: return x["abstract"]
    elif "copy-from" in x: return x["copy-from"]
    else: return None

def generic_loader(data, x):
    if x["type"] not in data:
        data[x["type"]] = dict()
    id = id_for_item(x)
    if id == None or type(id) is not str:
        # raise Exception(f"no 'id' or 'abstract' fields for {x}")
        return
    data[x["type"]][id] = x

def item_loader(data, x):
    if "item" not in data:
        data["item"] = dict()
    data["item"][id_for_item(x)] = x

def ignore_loader(data, x):
    pass

def parse_all_objects():
    loaders = {
        "AMMO": item_loader,
        "GUN": item_loader,
        "ARMOR": item_loader,
        "PET_ARMOR": item_loader,
        "TOOL": item_loader,
        "TOOLMOD": item_loader,
        "TOOL_ARMOR": item_loader,
        "BOOK": item_loader,
        "GENERIC": item_loader,
        "COMESTIBLE": item_loader,
        "ENGINE": item_loader,
        "WHEEL": item_loader,
        "GUNMOD": item_loader,
        "MAGAZINE": item_loader,
        "BATTERY": item_loader,
        "BIONIC_ITEM": item_loader,
    }
    for x in ["EXTERNAL_OPTION", "snippet", "help"]:
        loaders[x] = ignore_loader
    data = {"unknown": {}}
    for x in get_all_objects():
        if "type" not in x:
            continue
        if x["type"] not in loaders:
            generic_loader(data, x)
        else:
            loaders[x["type"]](data, x)

    #data["item"] = sorted(data["item"])
    return data