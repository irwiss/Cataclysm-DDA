#!/usr/bin/env python3

from anytree import Node, RenderTree
import _cata_json_base as jj


guns = { k: v for k, v in jj.parse_all_objects()["item"].items() if v["type"] == "GUN" }
guncount = len(guns)

def get_gun_parent(x: dict):
    cpfrom = x.get('copy-from', None)
    id = x.get('id', x.get('abstract', None))
    if cpfrom is not None and cpfrom != id:
        return cpfrom
    else:
        return None

tree = Node("root")
fake_item = Node("fake_item", parent=tree)
added = {"root": tree, "fake_item": fake_item}
pending = guns.keys()

while pending:
    for k in pending:
        parent = get_gun_parent(guns[k])
        if parent in added:
            added[k] = Node(k, parent=added[parent])
        elif parent is None:
            added[k] = Node(k, parent=added["root"])
        else:
            continue
        guns.pop(k)
        break

max_depth: int = 0
pre: str
fill: str
node: Node
for pre, fill, node in RenderTree(tree):
    childcount = len(node.children)
    count = f" ({childcount})" if childcount > 0 else ''
    max_depth = max([max_depth, node.depth])
    print(f"{pre}{node.name}{count}")

print(f"total guns: {guncount}")
print(f"guns on depth 1: {len(tree.children)}")
print(f"max depth: {max_depth}")

exit(0)
