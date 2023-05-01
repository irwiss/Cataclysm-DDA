#!/usr/bin/env python3

import _cata_json_base as jj

for k, v in jj.parse_all_objects().items():
    print(f"{k}: {len(v)}")

exit(0)