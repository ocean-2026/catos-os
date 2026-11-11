#!/usr/bin/env python3
"""test_app_list.py -- build/apps.list is the single source of truth.

Guards against the old failure mode where adding a CE app required editing
two hard-coded lists (build.sh and gen_builtin.py); forgetting the second
one silently produced an ISO without the app.
"""
import importlib.util
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
K = os.path.join(ROOT, "kernel_v25")

spec = importlib.util.spec_from_file_location(
    "gen_builtin", os.path.join(K, "build", "gen_builtin.py"))
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

apps = list(mod.app_names())
assert apps, "build/apps.list is empty"
assert len(apps) == len(set(apps)), "duplicate app names in build/apps.list"

for name in apps:
    src = os.path.join(K, "extapp", name + ".c")
    assert os.path.isfile(src), "missing source: " + src

paths = [p for p, _ in mod.FILES]
for name in apps:
    assert ("/bin/%s.ce" % name) in paths, "gen_builtin.py lost " + name

with open(os.path.join(K, "build.sh"), "r") as fh:
    build = fh.read()
assert "apps.list" in build, "build.sh does not read build/apps.list"
assert "for app in hello calc" not in build, "build.sh still has a hard-coded list"

print("PASS: %d apps, single source of truth verified" % len(apps))
