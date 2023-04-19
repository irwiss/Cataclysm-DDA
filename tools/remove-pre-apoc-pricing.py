#!/usr/bin/env python3

import argparse
import json
import os
import shutil
from subprocess import PIPE, run

args = argparse.ArgumentParser(formatter_class=argparse.ArgumentDefaultsHelpFormatter)
args.add_argument("dir", help="specify json directory")
args.add_argument("--formatter", help="path to formatter executable", default="tools/format/json_formatter.exe")
args_dict = vars(args.parse_args())

formatter = args_dict["formatter"]

if shutil.which(formatter) is None:
    print(f"formatter '{formatter}' seems not existing or not runnable")
    exit(1)

def format_json(json_in):
    p = run([args_dict["formatter"]], input=json_in, stdout=PIPE, stderr=PIPE, text=True, encoding='utf-8')
    return p.stdout


def object_to_json(obj):
    json_string = json.dumps(obj, ensure_ascii=False)
    return format_json(json_string)


def write_object_as_json_file(filename, json_in):
    with open(filename, "w", encoding="utf-8") as fp:
        json_formatted = object_to_json(json_in)
        fp.write(json_formatted)


for root, directories, filenames in os.walk(args_dict["dir"]):
    for filename in filenames:
        if not filename.endswith(".json"):
            continue

        path = os.path.join(root, filename)

        with open(path, "r", encoding="utf-8") as json_file:
            try:
                json_data = json.load(json_file)
            except json.JSONDecodeError:
                print(f"skipping {path} due to JSONDecodeError")
                continue

        if type(json_data ) is not list:
            print(f"skipping non-array json in {path}")
            continue

        write_out = False
        for jo in json_data:
            if type(jo) is not dict:
                print(f"skipping non-dict object in {path}")
                continue

            if ("type" not in jo or "price_postapoc" not in jo):
                continue

            jo["price"] = jo["price_postapoc"]
            del jo["price_postapoc"]
            write_out = True

        if write_out:
            write_object_as_json_file(path, json_data)
