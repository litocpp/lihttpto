#!/usr/bin/env python3
"""Compare public Url observations with a supplied WPT urltestdata.json snapshot."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

FIELDS = ("href", "protocol", "username", "password", "host", "hostname", "port", "pathname", "search", "hash", "origin")


def usv_string(value):
    # WPT feeds JavaScript strings through the URL constructor's USVString conversion.
    return value.encode("utf-16-le", "surrogatepass").decode("utf-16-le", "replace")


def compare(expected, actual):
    want_failure = expected.get("failure", False)
    if want_failure != actual["failure"]:
        return {"failure": {"expected": want_failure, "actual": actual["failure"]}}
    if want_failure:
        return {}
    return {field: {"expected": expected[field], "actual": actual.get(field)}
            for field in FIELDS if field in expected and actual.get(field) != expected[field]}


def run(args):
    raw = args.data.read_bytes()
    entries = json.loads(raw)
    indexed = [(index, item) for index, item in enumerate(entries) if isinstance(item, dict)]
    inputs = [{"input": usv_string(item["input"]),
               "base": usv_string(item["base"]) if item.get("base") is not None else None}
              for _, item in indexed]
    with tempfile.TemporaryDirectory(prefix="lihttpto-url-baseline-") as directory:
        root = Path(directory)
        input_path, output_path = root / "input.json", root / "observed.json"
        input_path.write_text(json.dumps(inputs, ensure_ascii=False), encoding="utf-8")
        env = dict(os.environ, LIHTTPTO_URL_INPUT=str(input_path), LIHTTPTO_URL_OUTPUT=str(output_path))
        subprocess.run([str(args.binary.resolve()), "--gtest_filter=UrlBaseline.*"], env=env, check=True, timeout=60)
        observations = json.loads(output_path.read_text(encoding="utf-8"))
    assert len(observations) == len(indexed)
    failures, fields, categories = [], Counter(), Counter()
    accepted = rejected = 0
    for (index, expected), actual in zip(indexed, observations):
        differences = compare(expected, actual)
        if not differences:
            continue
        fields.update(differences.keys())
        scheme = expected.get("protocol") or "failure/no-protocol"
        categories.update([scheme])
        accepted += bool(expected.get("failure") and not actual["failure"])
        rejected += bool(not expected.get("failure") and actual["failure"])
        failures.append({"index": index, "input": expected["input"], "base": expected.get("base"), "differences": differences})
    report = {
        "source": str(args.data.resolve()), "revision": args.revision,
        "sha256": hashlib.sha256(raw).hexdigest(), "cases": len(indexed),
        "matched": len(indexed) - len(failures), "mismatched": len(failures),
        "unexpected_accept": accepted, "unexpected_reject": rejected,
        "fields": dict(fields), "categories": dict(categories),
        "coverage": list(FIELDS),
        "excluded": ["URL setters", "URLSearchParams", "JavaScript-only coercion tests"],
        "limitations": ["Input with a base uses Url::parse(input, base) through base.resolve(input).",
                        "IDNA host processing is not implemented yet.",
                        "Origin uses the standalone URL algorithm without a browser blob URL store.",
                        "Unsupported embedded blob hosts leave origin unreported, not assumed opaque.",
                        "Successful probe execution is not a conformance pass."],
        "failures": failures,
    }
    args.report.write_text(json.dumps(report, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if key != "failures"}, indent=2))
    if args.require_conformance and failures:
        raise SystemExit(1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("data", type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--require-conformance", action="store_true")
    run(parser.parse_args())
