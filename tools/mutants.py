#!/usr/bin/env python3
"""Plant known storage-engine bugs one at a time and see what catches them.

Each mutant is applied to a scratch copy of the repo (the working tree is
never touched) and built in its own build dir. Then both the unit tests
(lsm_tests) and the crash sweep (lsm_crash) run against it, and the script
reports which of them noticed.

  python3 tools/mutants.py                   all mutants, 1000 sweep seeds
  python3 tools/mutants.py --seeds 300 wal-no-sync
  python3 tools/mutants.py --list

Exit code 0 if every mutant was caught by something, 1 otherwise.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# name -> (file, original text, mutated text, what the bug is)
MUTANTS = {
    "wal-no-sync": (
        "src/db.cpp",
        "  if (wo.sync) log_->sync();\n",
        "  (void)wo;\n",
        "a sync=true write returns without syncing the WAL",
    ),
    "manifest-no-sync": (
        "src/manifest.cpp",
        "  f->append(m.encode());\n  f->sync();\n",
        "  f->append(m.encode());\n",
        "MANIFEST.tmp is renamed into place without being synced first",
    ),
    "table-no-sync": (
        "src/sstable.cpp",
        "  file_->sync();\n  file_->close();\n",
        "  file_->close();\n",
        "a new table is added to the manifest without being synced",
    ),
    "log-deleted-early": (
        "src/db.cpp",
        "  const TableMeta tm = b.finish();\n\n  Manifest next = manifest_;\n"
        "  next.files.push_back(FileMeta{0, number, tm.file_size, tm.smallest, tm.largest});\n"
        "  next.log_number = new_log;",
        "  const TableMeta tm = b.finish();\n  fs_.remove(path(log_file_, \"log\"));\n\n"
        "  Manifest next = manifest_;\n"
        "  next.files.push_back(FileMeta{0, number, tm.file_size, tm.smallest, tm.largest});\n"
        "  next.log_number = new_log;",
        "flush deletes the old WAL before the manifest that replaces it is written",
    ),
    "flush-drops-deletes": (
        "src/db.cpp",
        "    // Deletions are kept: they must hide older values in other tables.\n",
        "    if (it->type() == ValueType::Deletion) continue;\n",
        "flush drops tombstones, so older values in L1 come back",
    ),
    "l0-oldest-first": (
        "src/db.cpp",
        "  for (auto it = manifest_.files.rbegin(); it != manifest_.files.rend(); ++it) {",
        "  for (auto it = manifest_.files.begin(); it != manifest_.files.end(); ++it) {",
        "get() searches level-0 tables oldest first",
    ),
    "batch-not-atomic": (
        "src/db.cpp",
        "  log_->add(batch.encode(first));\n",
        "  for (std::size_t i = 0; i < batch.size(); ++i) {\n"
        "    WriteBatch one;\n"
        "    const auto& op = batch.ops()[i];\n"
        "    if (op.type == ValueType::Value) one.put(op.key, op.value); else one.del(op.key);\n"
        "    log_->add(one.encode(first + i));\n"
        "  }\n",
        "a batch is logged as one record per op, so a crash can keep half of it",
    ),
    "wal-skips-crc": (
        "src/wal.cpp",
        "    if (crc32c(payload, crc32c(len_bytes)) != crc) {",
        "    if (false && crc32c(payload, crc32c(len_bytes)) != crc) {",
        "WAL replay accepts records whose checksum doesn't match",
    ),
    "block-skips-crc": (
        "src/sstable.cpp",
        "  if (crc32c(std::string_view(b).substr(0, h.size)) != decode_fixed32(b.data() + h.size))\n"
        "    throw Corruption(path_ + \": block checksum mismatch\");",
        "",
        "table data blocks are read without checking their checksum",
    ),
}


def run(cmd, cwd):
    return subprocess.run(cmd, cwd=cwd, text=True, capture_output=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="*")
    ap.add_argument("--seeds", type=int, default=1000)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    if args.list:
        for name, (_, _, _, what) in MUTANTS.items():
            print(f"{name:20} {what}")
        return 0
    names = args.names or list(MUTANTS)
    for n in names:
        if n not in MUTANTS:
            print(f"unknown mutant {n}", file=sys.stderr)
            return 2

    ignore = shutil.ignore_patterns("build*", ".git", "__pycache__")
    survivors = []
    print(f"{'mutant':20} {'unit tests':>14} {'crash sweep':>18}")
    with tempfile.TemporaryDirectory(prefix="lsm-mutants-") as tmp:
        for name in names:
            path, old, new, what = MUTANTS[name]
            src = os.path.join(tmp, name)
            shutil.copytree(ROOT, src, ignore=ignore)
            target = os.path.join(src, path)
            text = open(target).read()
            if text.count(old) != 1:
                print(f"{name}: anchor text not found exactly once in {path}; update tools/mutants.py")
                return 2
            open(target, "w").write(text.replace(old, new))
            build = os.path.join(src, "build")
            cfg = run(["cmake", "-S", src, "-B", build, "-DCMAKE_BUILD_TYPE=Release"], src)
            b = run(["cmake", "--build", build, "-j", str(args.jobs)], src)
            if cfg.returncode or b.returncode:
                print(f"{name}: does not compile\n{b.stdout[-3000:]}{b.stderr[-2000:]}")
                return 2

            t = run([os.path.join(build, "lsm_tests"), "--gtest_brief=1"], src)
            # --gtest_brief prints only failing tests, one "[  FAILED  ] Name" line each.
            failed_tests = sorted(set(re.findall(r"\[  FAILED  \] (\S+) \(", t.stdout)))
            test_fails = len(failed_tests) if failed_tests or t.returncode == 0 else -1
            tests_col = "pass" if test_fails == 0 else (f"{test_fails} fail" if test_fails > 0 else "crash")

            s = run([os.path.join(build, "lsm_crash"), "--seeds", str(args.seeds), "--quiet"], src)
            lines = s.stdout.strip().splitlines()
            m = re.match(r"(\d+)/(\d+) seeds passed", lines[-1] if lines else "")
            sweep_fails = int(m.group(2)) - int(m.group(1)) if m else -1
            sweep_col = f"{sweep_fails}/{args.seeds} fail" if sweep_fails >= 0 else "crash"

            caught = test_fails != 0 or sweep_fails != 0
            print(f"{name:20} {tests_col:>14} {sweep_col:>18}   {'' if caught else 'SURVIVED  '}{what}")
            first = next((l for l in lines if " FAIL: " in l), "")
            if failed_tests:
                print(f"{'':20} tests: {', '.join(failed_tests[:4])}{' ...' if len(failed_tests) > 4 else ''}")
            if first:
                print(f"{'':20} sweep: {first[:150]}")
            if not caught:
                survivors.append(name)
    if survivors:
        print(f"\n{len(survivors)} mutant(s) survived: {', '.join(survivors)}")
        return 1
    print("\nall mutants caught")
    return 0


if __name__ == "__main__":
    sys.exit(main())
