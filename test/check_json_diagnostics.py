#!/usr/bin/env python3
import json
import sys


with open(sys.argv[1], "r", encoding="utf-8") as f:
    records = [json.loads(line) for line in f if line.strip()]

assert records, "expected at least one diagnostic record"
first = records[0]
assert first["function"] == "rt_entry", first
assert first["kind"] == "transitive", first
assert first["constraint"] == "nonallocating", first
assert first["effect"] == "may_alloc", first
assert first["confidence"] == "high", first
assert first["chain"][0]["function"] == "rt_entry", first
assert first["chain"][0]["callee"] == "leaf_alloc", first
assert first["chain"][1]["callee"] == "malloc", first
assert "locations" in first and len(first["locations"]) == len(first["chain"]), first
