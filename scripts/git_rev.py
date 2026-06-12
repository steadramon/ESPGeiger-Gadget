#!/usr/bin/env python3
# PlatformIO build_flags shim: emit -DGIT_VERSION='"<rev>"' from the
# current git working tree, so the firmware can stamp build assets with
# a cache-bust token. Mirrors the parent ESPGeiger scripts/git_rev.py.

import subprocess

revision = ""
try:
    revision = (
        subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            stderr=subprocess.DEVNULL,
        )
        .strip()
        .decode("utf-8")
    )
except Exception:
    pass

# Tagging convention is "v0.0.1" but we want "0.0.1" shown; same shape as
# the auto-update artefacts the parent firmware ships.
if revision.startswith("v"):
    revision = revision[1:]

print("-DGIT_VERSION='\"%s\"'" % revision)
