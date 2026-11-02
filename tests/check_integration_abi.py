#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Freeze the public Lertick/Lerwatt ABI independently of model releases.

No network, BPF load, peer checkout or third-party Python package is required.
Only the shared header is copied into the probe include tree: model/controller
headers must not become dependencies of this contract. CC is parsed with
shlex, never through a shell. --peer optionally checks another checkout's
compiled contract, ignoring comments and unrelated source/model changes.

ABI 2 meanings: CLOCK_MONOTONIC nanoseconds, capacity 0..1024 (zero ceiling
means no cap), milli shares/prices 0..1000, CPU-set identity and attachment
generation, 300 ms limit TTL and 500 ms demand age. A model-only update must
keep these values. A deliberate ABI change needs an explicit migration and
reviewed golden update in both repositories, not a silent test regeneration.
"""

import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


CONSTANTS = {
    "LW_CONTRACT_VERSION": 2,
    "LW_CONTRACT_MAX_CLUSTERS": 8,
    "LW_CAP_ONE": 1024,
    "LW_MILLI_ONE": 1000,
    "LW_CONTRACT_PUBLISH_NS": 100_000_000,
    "LW_CONTRACT_DEMAND_MAX_AGE_NS": 500_000_000,
    "LW_CONTRACT_TTL_NS": 300_000_000,
    "LW_CONTRACT_RETRY_NS": 1_000_000_000,
    "LW_CONTRACT_STALE_NS": 2_000_000_000,
}
OFFSETS = {
    "lw_contract_rec": {
        "seq": 0,
        "version": 4,
        "ceiling_cap": 8,
        "floor_cap": 12,
        "energy_price_milli": 16,
        "thermal_headroom_mc": 20,
        "valid_until_ns": 24,
        "topology_id": 32,
        "session_id": 40,
    },
    "lt_demand_rec": {
        "seq": 0,
        "version": 4,
        "pred_util": 8,
        "cpuperf_max": 12,
        "cpuperf_mean": 16,
        "lat_crit_milli": 20,
        "p99_wake_ns": 24,
        "updated_ns": 32,
        "reserved": 40,
        "topology_id": 48,
        "session_id": 56,
    },
}
SIZES = {"lw_contract_rec": 48, "lt_demand_rec": 64}
PIN_PATHS = {
    "LW_CONTRACT_PIN_DIR": "/sys/fs/bpf/lertick",
    "LW_CONTRACT_PIN_LW": "/sys/fs/bpf/lertick/lw_contract",
    "LW_CONTRACT_PIN_LT": "/sys/fs/bpf/lertick/lt_demand",
}
# Independent frozen FNV-1a vectors for domain LWTOP2, sorted LE32 CPUs/count.
HASH_VECTORS = (
    ((), 0),
    ((0,), 0x7162AA771D26622E),
    ((0, 1, 2, 3), 0x8D5E629BCC626C8B),
    ((4, 5), 0x8983A3CF8E7630AC),
    ((6, 7), 0xDCF22AA0EEB8538C),
    ((0, 256, 1024), 0xA01D8BA9443BD4DB),
)
CONSTRAINT_CASES = (
    ("fresh", "", 1),
    ("expires_now", "r.valid_until_ns = now;", 1),
    ("maximum_ttl", "r.valid_until_ns = now + 300000000ULL;", 1),
    ("expired", "r.valid_until_ns = now - 1;", 0),
    ("excess_ttl", "r.valid_until_ns = now + 300000001ULL;", 0),
    ("old_version", "r.version = 1;", 0),
    ("wrong_topology", "r.topology_id++;", 0),
    ("wrong_session", "r.session_id++;", 0),
    ("missing_topology", "r.topology_id = 0;", 0),
    ("missing_session", "r.session_id = 0;", 0),
    ("uncapped", "r.ceiling_cap = 0;", 1),
    ("ceiling_overflow", "r.ceiling_cap = 1025;", 0),
    ("floor_overflow", "r.ceiling_cap = 0; r.floor_cap = 1025;", 0),
    ("inverted_bounds", "r.floor_cap = r.ceiling_cap + 1;", 0),
    ("price_overflow", "r.energy_price_milli = 1001;", 0),
)


def golden():
    result = dict(CONSTANTS)
    result.update(PIN_PATHS)
    for record, fields in OFFSETS.items():
        result[f"sizeof.{record}"] = SIZES[record]
        for field, offset in fields.items():
            result[f"offsetof.{record}.{field}"] = offset
    result["topology.seed"] = 0xF3FB15F721510CDF
    for cpus, expected in HASH_VECTORS:
        result["topology." + ",".join(map(str, cpus))] = expected
    for name, _mutation, expected in CONSTRAINT_CASES:
        result[f"constraint.{name}"] = expected
    return result


def locate_header(root):
    root = root.resolve()
    if root.is_file():
        return root
    candidates = [
        root / "include/lerwatt/contract.h",
        root / "vendor/lerwatt/contract.h",
    ]
    found = [path for path in candidates if path.is_file()]
    if len(found) != 1:
        raise ValueError(f"expected one canonical/vendored contract header in {root}")
    return found[0]


def c_probe():
    lines = [
        "#include <stddef.h>",
        "#include <stdint.h>",
        "#include <stdio.h>",
        "#include <inttypes.h>",
        '#include "lerwatt/contract.h"',
        "int main(void) {",
        '  puts("{");',
    ]

    def emit_number(key, expression):
        lines.append(
            f'  printf("\\\"{key}\\\":%" PRIu64 ",\\n", (uint64_t)({expression}));'
        )

    for name in CONSTANTS:
        emit_number(name, name)
    for name in PIN_PATHS:
        lines.append(f'  printf("\\\"{name}\\\":\\\"%s\\\",\\n", {name});')
    for record, fields in OFFSETS.items():
        emit_number(f"sizeof.{record}", f"sizeof(struct {record})")
        for field in fields:
            emit_number(f"offsetof.{record}.{field}", f"offsetof(struct {record}, {field})")
    emit_number("topology.seed", "lw_contract_topology_seed()")
    for index, (cpus, _expected) in enumerate(HASH_VECTORS):
        lines.append(f"  uint64_t hash{index} = lw_contract_topology_seed();")
        for cpu in cpus:
            lines.append(f"  hash{index} = lw_contract_topology_add_cpu(hash{index}, {cpu});")
        emit_number(
            "topology." + ",".join(map(str, cpus)),
            f"lw_contract_topology_finish(hash{index}, {len(cpus)})",
        )
    lines.extend([
        "  const uint64_t now = 5000000000ULL, topology = 23, session = 47;",
        "  const struct lw_contract_rec base = { .seq = 2, .version = 2,",
        "    .ceiling_cap = 512, .floor_cap = 256, .energy_price_milli = 500,",
        "    .thermal_headroom_mc = 5000, .valid_until_ns = now + 100000000ULL,",
        "    .topology_id = topology, .session_id = session };",
    ])
    for name, mutation, _expected in CONSTRAINT_CASES:
        lines.extend(["  {", "    struct lw_contract_rec r = base;", f"    {mutation}"])
        emit_number(f"constraint.{name}", "lw_contract_constraint_valid(&r, topology, session, now)")
        lines.append("  }")
    lines.extend(['  puts("\\\"end\\\":0}");', "  return 0;", "}"])
    return "\n".join(lines) + "\n"


def run_checked(command, label):
    try:
        result = subprocess.run(command, text=True, capture_output=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ValueError(f"{label}: {error}") from error
    if result.returncode:
        detail = (result.stderr or result.stdout).strip()
        raise ValueError(f"{label} failed ({result.returncode}): {detail}")
    return result.stdout


def probe(header, compiler):
    with tempfile.TemporaryDirectory(prefix="lerwatt-abi-") as directory:
        temporary = Path(directory)
        include = temporary / "include/lerwatt"
        include.mkdir(parents=True)
        shutil.copyfile(header, include / "contract.h")
        source = temporary / "probe.c"
        binary = temporary / "probe"
        source.write_text(c_probe(), encoding="utf-8")
        run_checked(
            [*compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", str(temporary / "include"), str(source), "-o", str(binary)],
            f"standalone header compile ({header})",
        )
        output = run_checked([str(binary)], f"ABI probe ({header})")
    try:
        values = json.loads(output)
    except json.JSONDecodeError as error:
        raise ValueError(f"invalid ABI probe output for {header}: {error}") from error
    if values.pop("end", None) != 0:
        raise ValueError(f"incomplete ABI probe output for {header}")
    expected = golden()
    mismatches = [
        f"{key}: expected {expected.get(key)!r}, got {values.get(key)!r}"
        for key in sorted(expected.keys() | values.keys())
        if expected.get(key) != values.get(key)
    ]
    if mismatches:
        raise ValueError(f"ABI 2 drift in {header}:\n  " + "\n  ".join(mismatches))
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--peer", type=Path, help="optional peer checkout root or contract header")
    args = parser.parse_args()
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        parser.error("CC must name a C compiler")
    try:
        root = Path(__file__).resolve().parents[1]
        local = probe(locate_header(root), compiler)
        if args.peer:
            peer = probe(locate_header(args.peer), compiler)
            if local != peer:
                raise ValueError("local and peer compiled ABI probes differ")
    except (OSError, ValueError) as error:
        print(f"Integration ABI check failed: {error}", file=sys.stderr)
        return 1
    suffix = "; peer compiled ABI matches" if args.peer else ""
    print(f"Integration ABI 2 golden check passed ({len(local)} values){suffix}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
