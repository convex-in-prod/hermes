# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import hashlib
import json
import pathlib
import re
import subprocess
import sys


compiler = pathlib.Path(sys.argv[1]).resolve()
root = pathlib.Path(sys.argv[2]).resolve()
root.mkdir(parents=True, exist_ok=True)
assert len(sys.argv) in (3, 8)
native = sys.argv[3:]
compile_count = 0
execute_count = 0
source = '''
function read(value) {
  "noinline";
  return value.stableProperty;
}
print(read({stableProperty: "kept"}));
print("\\ud800");
'''


def compile_source(name, text, retained=None, succeeds=True, emit_layout=True,
                   shard_size=None, typed=False):
    global compile_count
    compile_count += 1
    directory = root / name
    directory.mkdir()
    (directory / "input.js").write_text(text)
    command = [
        str(compiler), "-O", "-emit-c", "-Xemit-c-bundle",
        "-o", "unit.c.json",
    ]
    if emit_layout:
        command.append("-Xemit-c-layout")
    if typed:
        command.append("-typed")
    if shard_size is not None:
        command.append("-Xemit-c-shard-size=" + str(shard_size))
    if retained is not None:
        path = directory / "retained.json"
        path.write_text(json.dumps(retained))
        command.append("-Xc-layout-input=" + str(path))
    result = subprocess.run(
        command + ["input.js"], cwd=directory, capture_output=True,
        text=True, timeout=60,
    )
    if not succeeds:
        assert result.returncode != 0, name
        assert "Invalid retained Static Hermes C layout" in result.stderr, result.stderr
        assert not (directory / "unit.c.json").exists(), name
        return None
    assert result.returncode == 0, result.stderr
    layout_path = directory / "sh_this_unit_layout.json"
    if emit_layout:
        layout = json.loads(layout_path.read_text())
    else:
        assert not layout_path.exists()
        layout = None
    manifest = json.loads((directory / "unit.c.json").read_text())
    if emit_layout:
        layout_bytes = layout_path.read_bytes()
        assert manifest["layout"] == {
            "path": layout_path.name,
            "sha256": hashlib.sha256(layout_bytes).hexdigest(),
            "size": len(layout_bytes),
        }
    else:
        assert "layout" not in manifest
    members = manifest["translationUnits"]
    next_function = 0
    previous = None
    for member in members:
        if member["role"] != "function":
            continue
        assert member["functionCount"] == member["lastFunctionId"] - member["firstFunctionId"] + 1
        if member.get("functionFragmentIndex", 0) == 0:
            if previous and "functionFragmentIndex" in previous:
                assert previous["functionFragmentIndex"] + 1 == previous["functionFragmentCount"]
            assert member["firstFunctionId"] == next_function
            next_function = member["lastFunctionId"] + 1
        else:
            assert previous is not None
            assert member["firstFunctionId"] == previous["firstFunctionId"]
            assert member["functionFragmentIndex"] == previous["functionFragmentIndex"] + 1
            assert member["functionFragmentCount"] == previous["functionFragmentCount"]
        previous = member
    if previous and "functionFragmentIndex" in previous:
        assert previous["functionFragmentIndex"] + 1 == previous["functionFragmentCount"]
    text = "\n".join((directory / member["path"]).read_text() for member in members)
    return layout, text


cold, cold_c = compile_source("cold", source)
_, default_c = compile_source("default", source, emit_layout=False)
assert "sh_this_unit_f_1_read" in default_c
assert "sh_this_unit_f_1_read" not in cold_c
assert cold["kind"] == "static-hermes-c-layout-v1"
assert cold["unitName"] == "this_unit"
strings = [bytes.fromhex(value) for value in cold["strings"]]
assert b"\xed\xa0\x80" in strings
assert len(strings) == len(set(strings))
property_index = strings.index(b"stableProperty")
assert "symbols[{}] /*stableProperty*/".format(property_index) in cold_c

edited, edited_c = compile_source("edited", 'print("added-before-existing");\n' + source, cold)
assert edited["strings"][:len(cold["strings"])] == cold["strings"]
assert "symbols[{}] /*stableProperty*/".format(property_index) in edited_c
assert b"added-before-existing".hex() in edited["strings"]
unchanged, unchanged_c = compile_source("unchanged", 'print("added-before-existing");\n' + source, edited)
assert unchanged == edited
assert unchanged_c == edited_c

removed, _ = compile_source("removed", source, edited)
assert removed["strings"] == edited["strings"]
for name, values in [
    ("slot-reset", ["retired-{}".format(i).encode().hex() for i in range(1100)]),
    ("byte-reset", [("x" * (65 * 1024)).encode().hex()]),
]:
    seed = {**cold, "strings": cold["strings"] + values}
    reset, reset_c = compile_source(name, source, seed)
    assert reset["strings"] == []
    assert "symbols[{}] /*stableProperty*/".format(property_index) in reset_c
    compacted, compacted_c = compile_source(name + "-next", source, reset)
    assert compacted == cold
    assert compacted_c == cold_c

for name, seed in [
    ("duplicate", {**cold, "strings": ["61", "61"]}),
    ("invalid-hex", {**cold, "strings": ["6g"]}),
    ("odd-hex", {**cold, "strings": ["6"]}),
    ("invalid-utf8", {**cold, "strings": ["ff"]}),
    ("truncated-utf8", {**cold, "strings": ["e080"]}),
    ("overlong-utf8", {**cold, "strings": ["c0af"]}),
    ("outside-unicode", {**cold, "strings": ["f4908080"]}),
    ("slot-bound", {**cold, "strings": ["61"] * (256 * 1024 + 1)}),
    ("byte-bound", {**cold, "strings": ["61" * (8 * 1024 * 1024 + 1)]}),
    ("wrong-unit", {**cold, "unitName": "other"}),
    ("unknown-field", {**cold, "extra": True}),
    ("wrong-version", {**cold, "kind": "unknown"}),
    ("wrong-shape", {**cold, "strings": {}}),
]:
    compile_source(name, source, seed, succeeds=False)

def bodies(text):
    return dict(re.findall(
        r"SH_BUNDLE_HIDDEN SHLegacyValue (sh_this_unit_f_[0-9]+)"
        r"\(SHRuntime \*shr\) \{(.*?)^}", text, re.M | re.S,
    ))


cold_bodies = bodies(cold_c)
read_label = next(name for name, body in cold_bodies.items() if "stableProperty" in body)
inserted_source = '''
function inserted(value, key) {
  "noinline";
  value.extraProperty = 17;
  return value[key];
}
print(inserted({first: 11}, "first"));
''' + source.replace("read", "renamed")
inserted, inserted_c = compile_source("inserted-function", inserted_source, cold)
assert inserted["functions"][1:len(cold["functions"])] == cold["functions"][1:]
assert bodies(inserted_c)[read_label] == cold_bodies[read_label]
again, again_c = compile_source("inserted-repeat", inserted_source, inserted)
assert again == inserted and again_c == inserted_c
restored, restored_c = compile_source("removed-function", source, inserted)
assert restored["functions"] == inserted["functions"]
assert bodies(restored_c)[read_label] == cold_bodies[read_label]

# Identical local shapes must receive distinct runtime slots and preserve the
# original names in metadata. Returning current C keeps a changed caller safe.
duplicate_source = '''
function first(value) { "noinline"; return value.entry; }
function second(value) { "noinline"; return value.entry; }
print(first({entry: 10}), second({entry: 20}), first.name, second.name);
'''
duplicates, duplicate_c = compile_source("duplicates", duplicate_source)
keys = [slot[0] for slot in duplicates["functions"]]
assert len(keys) == len(set(keys))
assert len(bodies(duplicate_c)) == len(keys)
changed_duplicate = duplicate_source.replace("return value.entry;", "return value.entry + 1;", 1)
compile_source("duplicate-edited", changed_duplicate, duplicates)

# Identical closures in unrelated parents must not shift all caller references
# when a different parent's closure is removed. Names are deliberately changed.
caller_source = '''
function wrapLeft() {
  "noinline";
  var result = {};
  result.left = function(value) { "noinline"; return value.entry; };
  return result;
}
function wrapRight() {
  "noinline";
  var result = {};
  result.right = function(value) { "noinline"; return value.entry; };
  return result;
}
function wrapLast() {
  "noinline";
  var result = {};
  result.last = function(value) { "noinline"; return value.entry; };
  return result;
}
print(wrapLeft().left({entry: 10}), wrapRight().right({entry: 20}), wrapLast().last({entry: 30}));
'''
callers, caller_c = compile_source("duplicate-callers", caller_source)
caller_bodies = bodies(caller_c)
right_label = next(name for name, body in caller_bodies.items()
                   if "/*right*/" in body and "/*last*/" not in body
                   and "_sh_ljs_create_closure" in body)
caller_edit = caller_source[caller_source.index("function wrapRight"):]
caller_edit = caller_edit.replace("wrapLeft().left({entry: 10}), ", "")
caller_edit = caller_edit.replace("wrapRight", "renamedRight")
_, caller_edit_c = compile_source("duplicate-callers-removed", caller_edit, callers)
assert bodies(caller_edit_c)[right_label] == caller_bodies[right_label]

# A deleted getter shifts identical getter occurrences, but the unique consumer
# still identifies its captured binding. Keep the creating initializer large so
# its changed shape cannot serve as a closure identity.
weighted_source = '''
function touch(box, value) { "noinline"; box.count = value; }
function factory(a, b, added) {
  "noinline";
  var box = {};
''' + "\n".join(f"  touch(box, {i});" for i in range(300)) + '''
  return [function() { return a; }, function() { return b; },
          function() { "noinline"; return b + 101; }];
}
var values = factory(10, 20, 30);
print(values[1](), values[2]());
'''
weighted, weighted_c = compile_source("weighted-environment", weighted_source)
weighted_bodies = bodies(weighted_c)
unique_label = next(name for name, body in weighted_bodies.items()
                    if "101" in body and "_sh_ljs_load_from_env" in body
                    and "_sh_ljs_create_closure" not in body)
weighted_edit_source = weighted_source.replace(
    "function() { return a; }, function() { return b; },",
    "function() { return b; },",
).replace("return b + 101; }];", "return b + 101; }, function() { return added; }];")
weighted_edit_source = weighted_edit_source.replace(
    "print(values[1](), values[2]());", "print(values[0](), values[1](), values[2]());",
)
_, weighted_edit_c = compile_source("weighted-environment-edited", weighted_edit_source, weighted)
assert bodies(weighted_edit_c)[unique_label] == weighted_bodies[unique_label]

# All four cache kinds and closure/class/generator metadata references execute
# from the current source after function insertion and name changes.
semantic_source = '''
class Counter {
  #value = 3;
  read(key) { "noinline"; this.publicValue = 8; return this.#value + this[key]; }
}
function *items() { yield 5; }
function capture(value) { return function inner() { return value + 1; }; }
const counter = new Counter();
print(counter.read("publicValue"), items().next().value, capture(9)());
print(Counter.name, Counter.length, capture.name, capture.length);
'''
semantic, _ = compile_source("semantic", semantic_source)
compile_source("semantic-default", semantic_source, emit_layout=False)
compile_source("semantic-edited", inserted_source + semantic_source, semantic)
assert all(any(slot[kind] for slot in semantic["functions"]) for kind in range(1, 5))

environment_source = '''
function factory(a, b) {
  "noinline";
  function reader(value) { "noinline"; return a + b + value; }
  function change(value) { "noinline"; a = value; }
  return {reader: reader, change: change};
}
var x = factory(3, 4);
print(x.reader(5));
x.change(8);
print(x.reader(5));
var y = factory(10, 20);
print(y.reader(5));
'''
environment, environment_c = compile_source("environment", environment_source)
environment_bodies = bodies(environment_c)
reader_label = next(name for name, body in environment_bodies.items()
                    if body.count("_sh_ljs_load_from_env(") == 2)
assert environment["scopes"]
edited_environment_source = re.sub(r"\ba\b", "renamedA", environment_source)
edited_environment_source = re.sub(r"\bb\b", "renamedB", edited_environment_source)
edited_environment_source = edited_environment_source.replace(
    "factory(renamedA, renamedB)", "factory(added, renamedA, renamedB)",
).replace(
    "  function reader", '  function extra() { "noinline"; return added; }\n  function reader',
).replace(
    "return {reader: reader, change: change};", "return {reader: reader, change: change, extra: extra};",
).replace("factory(3, 4)", "factory(99, 3, 4)").replace(
    "factory(10, 20)", "factory(100, 10, 20)",
) + "print(x.extra());\n"
environment_edit, environment_edit_c = compile_source(
    "environment-edited", edited_environment_source, environment,
)
assert bodies(environment_edit_c)[reader_label] == environment_bodies[reader_label]
environment_removed, environment_removed_c = compile_source(
    "environment-removed", environment_source, environment_edit,
)
assert bodies(environment_removed_c)[reader_label] == environment_bodies[reader_label]

# A sparse hint must not increase allocation size or the environment scanned by GC.
sparse_environment = json.loads(json.dumps(environment))
sparse_environment["scopes"] = [[[]] * 1100 + scope for scope in environment["scopes"]]
compacted_environment, compacted_environment_c = compile_source(
    "environment-compacted", environment_source, sparse_environment,
)
assert compacted_environment["scopes"] == environment["scopes"]
assert bodies(compacted_environment_c)[reader_label] == environment_bodies[reader_label]

scope_anchor = next(anchor for scope in environment["scopes"] for slot in scope for anchor in slot)
for name, scopes in [
    ("scope-shape", [True]),
    ("empty-scope", [[]]),
    ("scope-slot-shape", [[False]]),
    ("scope-negative-anchor", [[[-1]]]),
    ("scope-fractional-anchor", [[[0.5]]]),
    ("scope-missing-function", [[[len(environment["functions"]) << 32]]]),
    ("scope-duplicate-anchor", [[[scope_anchor], [scope_anchor]]]),
    ("scope-anchor-bound", [[[scope_anchor] * 5]]),
    ("scope-slot-bound", [[[]] * (256 * 1024 + 1)]),
]:
    compile_source(name, environment_source, {**environment, "scopes": scopes}, succeeds=False)

# Invalid seeds must not overlap sites, overflow indices, or alias functions.
read_slot = next(slot for slot in cold["functions"] if slot[2])
read_range = read_slot[2][0]
for name, functions in [
    ("duplicate-function-key", cold["functions"] + [cold["functions"][-1]]),
    ("missing-global-slot", cold["functions"][1:]),
    ("bad-function-key", [["wrong", [], [], [], []]]),
    ("wrong-function-shape", [["global"]]),
    ("overlapping-cache", [["global", [], [read_range, read_range], [], []]]),
    ("negative-cache", [["global", [], [[-1, 1]], [], []]]),
    ("zero-cache", [["global", [], [[0, 0]], [], []]]),
    ("fractional-cache", [["global", [], [[0.5, 1]], [], []]]),
    ("cache-bound", [["global", [], [[1024 * 1024, 1]], [], []]]),
    ("cache-extent", [["global", [[1024 * 1024 - 1, 1]], [[1, 1]], [], []]]),
    ("write-width", [["global", [[0, 2]], [], [], []]]),
]:
    compile_source(name, source, {**cold, "functions": functions}, succeeds=False)

retired_functions = [
    ["{:040x}:0".format(i), [], [], [], []] for i in range(1100)
]
function_reset, function_reset_c = compile_source(
    "function-reset", source,
    {**cold, "functions": cold["functions"] + retired_functions},
)
assert function_reset["functions"] == []
assert bodies(function_reset_c)[read_label] == cold_bodies[read_label]
function_next, _ = compile_source("function-reset-next", source, function_reset)
assert function_next == cold
cache_seed = json.loads(json.dumps(cold))
cache_seed["functions"].append(["f" * 40 + ":0", [], [[2000, 1]], [], []])
cache_reset, _ = compile_source("cache-reset", source, cache_seed)
assert cache_reset["functions"] == []
cache_next, _ = compile_source("cache-reset-next", source, cache_reset)
assert cache_next == cold

# Removing one packed group must not rename or repack the following groups.
# New shapes retain nearby native inlining opportunities; a smaller target
# still splits only the affected group, including outlined functions.
def shard_source(indices):
    return "\n".join(
        'function f{0}(x) {{ "noinline"; return x.p{0}; }}'.format(i)
        for i in indices
    ) + "\n" + "\n".join(
        'print(f{0}({{p{0}: {0}}}));'.format(i) for i in indices
    )


def function_members(name):
    directory = root / name
    manifest = json.loads((directory / "unit.c.json").read_text())
    return {m["path"]: (directory / m["path"]).read_bytes()
            for m in manifest["translationUnits"] if m["role"] == "function"}


shard_indices = list(range(32))
sharded, sharded_c = compile_source(
    "shards", shard_source(shard_indices), shard_size=6000,
)
assert len(sharded["shards"]) >= 4
removed_group = next(i for i, group in enumerate(sharded["shards"])
                     if len(group) > 1 and 0 not in group)
slot_to_source = {
    int(label.rsplit("_", 1)[1]): int(re.search(r"/\*p([0-9]+)\*/", body)[1])
    for label, body in bodies(sharded_c).items() if "/*p" in body and not label.endswith("_0")
}
removed_indices = {slot_to_source[slot] for slot in sharded["shards"][removed_group]}
remaining_indices = [i for i in shard_indices if i not in removed_indices]
deleted, _ = compile_source(
    "shards-deleted", shard_source(remaining_indices), sharded, shard_size=6000,
)
assert deleted["shards"][removed_group] == []
original_members = function_members("shards")
deleted_members = function_members("shards-deleted")
for path, body in original_members.items():
    group = int(re.search(r"_functions_([0-9]+)_", path)[1])
    if group != removed_group and 0 not in sharded["shards"][group]:
        assert deleted_members[path] == body, path
inserted_indices = [-1] + remaining_indices
# Use a valid identifier for the inserted function.
inserted_shard_source = shard_source(inserted_indices).replace("f-1", "added").replace("p-1", "added")
shard_inserted, shard_inserted_c = compile_source(
    "shards-inserted", inserted_shard_source, deleted, shard_size=6000,
)
shard_repeat, shard_repeat_c = compile_source(
    "shards-repeat", inserted_shard_source, shard_inserted, shard_size=6000,
)
assert shard_repeat == shard_inserted and shard_repeat_c == shard_inserted_c
unrelated_members = {path: body for path, body in deleted_members.items()
                     if "_functions_00000_" not in path}
inserted_members = function_members("shards-inserted")
assert sum(body == inserted_members.get(path)
           for path, body in unrelated_members.items()) >= len(unrelated_members) - 1
split_shards, _ = compile_source(
    "shards-split", inserted_shard_source, shard_inserted, shard_size=3000,
)
assert len(function_members("shards-split")) > len(function_members("shards-inserted"))
assert split_shards["shards"] == shard_inserted["shards"]

for name, shards in [
    ("shards-wrong-shape", [False]),
    ("shards-missing-slot", [[len(cold["functions"])]]),
    ("shards-negative-slot", [[-1]]),
    ("shards-fractional-slot", [[0.5]]),
    ("shards-duplicate-slot", [[0], [0]]),
    ("shards-group-bound", [[]] * 65534),
]:
    compile_source(name, source, {**cold, "shards": shards}, succeeds=False)
shard_reset, _ = compile_source(
    "shards-reset", source, {**cold, "shards": [[]] * 1100 + cold["shards"]},
)
assert shard_reset["shards"] == []
shard_reset_next, _ = compile_source("shards-reset-next", source, shard_reset)
assert shard_reset_next == cold

# Adding unrelated literals must not shift existing serialized offsets or shape
# cache slots. Changed literals still serialize from current source.
literal_source = '''
function array() { "noinline"; return [101, 203, 307]; }
function object() { "noinline"; return {stableKey: 701, otherKey: "kept"}; }
print(JSON.stringify(array()), JSON.stringify(object()));
'''
literal_prefix = '''
function extra() { "noinline"; return [401, 503, 607, 809, 911, 1013]; }
function extraObject() { "noinline"; return {inserted: 1201, text: "new"}; }
print(JSON.stringify(extra()), JSON.stringify(extraObject()));
'''
literals, literal_c = compile_source("literals", literal_source)
literal_bodies = bodies(literal_c)
array_label = next(name for name, body in literal_bodies.items()
                   if "_sh_ljs_new_array_with_buffer" in body)
object_label = next(name for name, body in literal_bodies.items()
                    if "_sh_ljs_new_object_with_buffer" in body)
literal_inserted, literal_inserted_c = compile_source(
    "literals-inserted", literal_prefix + literal_source, literals,
)
for label in (array_label, object_label):
    assert bodies(literal_inserted_c)[label] == literal_bodies[label]
literal_edited, literal_edited_c = compile_source(
    "literals-edited", literal_prefix + literal_source.replace("203", "211"),
    literal_inserted,
)
assert bodies(literal_edited_c)[object_label] == literal_bodies[object_label]
literal_restored, literal_restored_c = compile_source(
    "literals-restored", literal_source, literal_edited,
)
for label in (array_label, object_label):
    assert bodies(literal_restored_c)[label] == literal_bodies[label]
literal_repeat, literal_repeat_c = compile_source(
    "literals-repeat", literal_source, literal_restored,
)
assert literal_repeat == literal_restored and literal_repeat_c == literal_restored_c
legacy, legacy_c = compile_source(
    "literals-legacy", literal_source,
    {key: value for key, value in literals.items() if key != "literals"},
)
assert legacy == literals and legacy_c == literal_c

# Compressed byte segments can overlap. Seed bytes are allocation hints, never
# parsed as program data unless current serialization matches the exact segment.
empty_literals = {"values": "", "valueEntries": [], "keys": "", "keyEntries": [], "shapes": []}
for name, seed in [
    ("overlap", {**empty_literals, "values": "010203", "valueEntries": [[0, 2], [1, 2]]}),
    ("empty", {**empty_literals, "valueEntries": [[0, 0]], "keyEntries": [[0, 0]]}),
]:
    compile_source("literals-" + name, literal_source, {**literals, "literals": seed})

ordinary_kind = literals["literals"]["shapes"][0][2]
for name, seed in [
    ("wrong-type", []),
    ("unknown-field", {**empty_literals, "extra": 0}),
    ("missing-field", {key: value for key, value in empty_literals.items() if key != "values"}),
    ("invalid-hex", {**empty_literals, "values": "0g"}),
    ("odd-hex", {**empty_literals, "values": "0"}),
    ("uppercase-hex", {**empty_literals, "values": "AA"}),
    ("negative-offset", {**empty_literals, "valueEntries": [[-1, 0]]}),
    ("fractional-offset", {**empty_literals, "valueEntries": [[0.5, 0]]}),
    ("overflow-offset", {**empty_literals, "valueEntries": [[2**32, 0]]}),
    ("negative-length", {**empty_literals, "valueEntries": [[0, -1]]}),
    ("fractional-length", {**empty_literals, "valueEntries": [[0, 0.5]]}),
    ("overflow-length", {**empty_literals, "values": "00", "valueEntries": [[1, 1]]}),
    ("duplicate-entry", {**empty_literals, "values": "aaaa", "valueEntries": [[0, 1], [1, 1]]}),
    ("entry-bound", {**empty_literals, "valueEntries": [[0, 0]] * (256 * 1024 + 1)}),
    ("byte-bound", {**empty_literals, "values": "00" * (8 * 1024 * 1024 + 1)}),
    ("key-range", {**empty_literals, "keys": "00", "keyEntries": [[0, 2]]}),
    ("missing-key", {**empty_literals, "shapes": [[0, 0, ordinary_kind]]}),
    ("bad-kind", {**literals["literals"], "shapes": [[0, 1, 0]]}),
    ("negative-count", {**literals["literals"], "shapes": [[0, -1, ordinary_kind]]}),
    ("fractional-count", {**literals["literals"], "shapes": [[0, 0.5, ordinary_kind]]}),
    ("overflow-count", {**literals["literals"], "shapes": [[0, 2**32, ordinary_kind]]}),
    ("duplicate-shape", {**literals["literals"], "shapes": literals["literals"]["shapes"] * 2}),
    ("shape-bound", {**empty_literals, "shapes": [[0, 0, ordinary_kind]] * (64 * 1024 + 1)}),
]:
    compile_source("literals-reject-" + name, literal_source,
                   {**literals, "literals": seed}, succeeds=False)

retired_values = b"".join(i.to_bytes(2, "little") for i in range(1100))
for name, seed in [
    ("bytes", {**empty_literals, "values": "00" * (65 * 1024), "valueEntries": [[0, 1]]}),
    ("entries", {**empty_literals, "values": retired_values.hex(),
                 "valueEntries": [[i * 2, 2] for i in range(1100)]}),
    ("shapes", {**empty_literals, "keyEntries": [[0, 0]],
                "shapes": [[0, i, ordinary_kind] for i in range(1100)]}),
]:
    reset, _ = compile_source("literals-reset-" + name, literal_source,
                              {**literals, "literals": seed})
    assert reset["literals"] is None
    compacted, compacted_c = compile_source("literals-reset-" + name + "-next", literal_source, reset)
    assert compacted == literals and compacted_c == literal_c

# Identical property keys need distinct hidden-class caches for ordinary, typed,
# and non-enumerable typed allocations, including after retaining old shapes.
typed_literal_source = '''
'use strict';
class Item {
  x: number = 7;
  read(): number { return this.x; }
}
var typedItem = new Item();
var ordinaryItem = {x: 11};
print(typedItem.read(), ordinaryItem.x);
print(Object.keys(typedItem).join(","), Object.keys(ordinaryItem).join(","),
      Object.keys(Object.getPrototypeOf(typedItem)).join(","));
'''
typed_literals, _ = compile_source("literals-typed", typed_literal_source, typed=True)
shape_kinds = {shape[2] for shape in typed_literals["literals"]["shapes"]}
assert len(shape_kinds) == 3
typed_literal_edit = typed_literal_source.replace("'use strict';", "'use strict';\nprint({extra: 13}.extra);")
compile_source("literals-typed-edited", typed_literal_edit, typed_literals, typed=True)

if native:
    cc, config, include, vm, console = native

    def execute(name):
        global execute_count
        execute_count += 1
        directory = root / name
        manifest = json.loads((directory / "unit.c.json").read_text())
        units = manifest["translationUnits"]
        objects = []
        for unit in units:
            obj = unit["path"] + ".o"
            subprocess.run([
                cc, "-DNDEBUG", "-O0", "-I" + config, "-I" + include,
                "-c", unit["path"], "-o", obj,
            ], cwd=directory, check=True, timeout=60)
            objects.append(obj)
        subprocess.run([
            cc, *objects, "-L" + vm, "-L" + console,
            "-Wl,-rpath," + vm, "-Wl,-rpath," + console,
            "-lshermes_console", "-lhermesvm", "-lm", "-o", "executable",
        ], cwd=directory, check=True, timeout=60)
        result = subprocess.run(
            [str(directory / "executable")], check=True,
            capture_output=True, timeout=60,
        )
        return result.stdout

    baseline = execute("default")
    for name in ("cold", "removed-function", "function-reset", "cache-reset"):
        assert execute(name) == baseline, name
    assert execute("inserted-function") == b"11\n" + baseline
    assert execute("duplicates") == b"10 20 first second\n"
    assert execute("duplicate-edited") == b"11 20 first second\n"
    assert execute("duplicate-callers") == b"10 20 30\n"
    assert execute("duplicate-callers-removed") == b"20 30\n"
    assert execute("weighted-environment") == b"20 121\n"
    assert execute("weighted-environment-edited") == b"20 121 30\n"
    expected = execute("semantic-default")
    assert execute("semantic") == expected
    assert execute("semantic-edited") == b"11\n" + baseline + expected
    environment_expected = b"12\n17\n35\n"
    for name in ("environment", "environment-removed", "environment-compacted"):
        assert execute(name) == environment_expected
    assert execute("environment-edited") == environment_expected + b"99\n"
    for name, indices in [("shards", shard_indices),
                          ("shards-deleted", remaining_indices),
                          ("shards-inserted", inserted_indices),
                          ("shards-split", inserted_indices)]:
        assert execute(name) == "".join(str(i) + "\n" for i in indices).encode()
    literal_expected = b'[101,203,307] {"stableKey":701,"otherKey":"kept"}\n'
    inserted_expected = b'[401,503,607,809,911,1013] {"inserted":1201,"text":"new"}\n'
    for name in ("literals", "literals-restored", "literals-repeat", "literals-legacy",
                 "literals-overlap", "literals-empty", "literals-reset-bytes",
                 "literals-reset-entries", "literals-reset-shapes"):
        assert execute(name) == literal_expected, name
    assert execute("literals-inserted") == inserted_expected + literal_expected
    assert execute("literals-edited") == inserted_expected + literal_expected.replace(b"203", b"211")
    assert execute("literals-typed") == b"7 11\nx x \n"
    assert execute("literals-typed-edited") == b"13\n7 11\nx x \n"

print(f"{compile_count} retained-layout compiler cases passed; "
      f"{execute_count} native execution cases passed")
