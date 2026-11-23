"""
Master test harness.

  1. Runs the no-VM static checks once (host vs Win32 + create-validation).
  2. Spawns ONE full per-VM suite thread per spec, CONCURRENTLY -- with more
     than one spec, multi-VM concurrency is tested by construction (the daemon
     juggles every spec's VM at once). There is no separate multi-VM test. Edit
     SPECS to change thread count / create params.
  3. Runs the Windows template lifecycle (separate -- sysprep is a different
     lifecycle from a normal VM).

Then prints a per-thread summary + a coverage table (function -> where tested).
Launch as a background task. Self-cleaning: every VM is a brk-* throwaway.
"""
import sys, os, subprocess, threading
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))   # parent: for asb.py
sys.path.insert(0, HERE)                     # this dir: for vm_lifecycle + sibling test modules
import asb
from vm_lifecycle import run_vm_lifecycle

# The harness chooses how many threads and each thread's create params.
# One uniform guest size everywhere: 4096 MB / 2 cores / 64 GB.
SPECS = [
    {"name": "brk-win", "os_type": "Windows", "ram": 4096, "hdd": 64, "cores": 2},
]
RUN_STATIC = True
RUN_TEMPLATE = True   # template create -> sysprep -> from-template

# function / endpoint -> where it's covered
COVERAGE = [
    ("GET /host (cores/ram vs Win32)",          "static"),
    ("GET /version, /templates",                "static, per-VM"),
    ("POST /vms create -- input validation",    "static (reject) + per-VM (duplicate)"),
    ("POST /vms create -- Windows",             "per-VM (one thread per spec)"),
    ("POST /vms create -- template / from-tpl", "template"),
    ("GET /vms (list), /vms/{n} status",        "per-VM (all threads)"),
    ("PUT /vms/{n} edit + validation",          "per-VM (config-at-start + reject)"),
    ("config applied at start (ram/cpu/gpu/net)","per-VM"),
    ("POST .../start (+ from snapshot/branch)", "per-VM"),
    ("POST .../shutdown (graceful)",            "per-VM"),
    ("POST .../stop (force) + idempotency",     "per-VM"),
    ("POST .../delete (+ no orphan)",           "per-VM"),
    ("delete-during-build guard (409)",         "per-VM (each thread, mid-build)"),
    ("GET .../sshInfo",                         "per-VM"),
    ("SSH key auto-deploy + key-only login",    "per-VM (sshEnabled+sshDeployKey)"),
    ("GET/POST/DELETE .../display open+close",   "per-VM when online; skip on no-desktop"),
    ("snapshots take/rename/delete + GET tree", "per-VM"),
    ("branches: start-from-snapshot + delete",  "per-VM"),
    ("GET /events (SSE) per VM",                "per-VM (each thread, concurrent SSE)"),
    ("POST /shutdown (+ active-VM guard 409)",  "per-VM (while its VM is running)"),
    ("DELETE /templates/{name}, /templates",    "template"),
    ("multi-VM concurrency (daemon juggles N)", "INHERENT: %d threads run at once" % len(SPECS)),
]

try:
    _c = asb.connect()
    v = _c.version()
    print("daemon reachable: %s %s (%s)" % (v.get("product"), v.get("version"), v.get("hostOs")))
except Exception as e:
    print("PREFLIGHT FAIL: daemon not reachable (%s).\nStart it with: %s"
          % (e, asb.daemon_launch_hint()))
    sys.exit(2)

# Sweep leftover brk-* throwaways from a previous aborted run so a rerun is
# self-healing (every spec creates its VM from scratch).
import helpers as _helpers
for _vm in list(_c.list()):
    if _vm["name"].startswith("brk-"):
        print("sweeping leftover %s..." % _vm["name"], flush=True)
        _helpers.destroy_vm(_c, _vm["name"])

results = {}   # label -> list of failures ([] == pass)

if RUN_STATIC:
    print("\n" + "=" * 70 + "\nSTATIC (no-VM) CHECKS\n" + "=" * 70, flush=True)
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_host_and_validation.py")], cwd=HERE)
    results["static"] = [] if p.returncode == 0 else ["test_host_and_validation.py exit %d" % p.returncode]

print("\n" + "=" * 70 + "\n%d CONCURRENT PER-VM SUITES: %s\n" % (len(SPECS), ", ".join(s["name"] + "/" + s["os_type"] for s in SPECS)) + "=" * 70, flush=True)
threads = []
def worker(spec):
    name, fails = run_vm_lifecycle(spec)
    results[name] = fails
for spec in SPECS:
    t = threading.Thread(target=worker, args=(spec,)); t.start(); threads.append(t)
for t in threads:
    t.join()

if RUN_TEMPLATE:
    print("\n" + "=" * 70 + "\nWINDOWS TEMPLATE LIFECYCLE\n" + "=" * 70, flush=True)
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_windows_template.py")], cwd=HERE)
    results["template"] = [] if p.returncode == 0 else ["test_windows_template.py exit %d" % p.returncode]

print("\n" + "=" * 70 + "\nSUMMARY\n" + "=" * 70)
for label in sorted(results):
    f = results[label]
    print("  %-12s %s" % (label, "PASS" if not f else "FAIL"))
    for x in f:
        print("        - %s" % x)

ran = set(results)
failed = {k for k, f in results.items() if f}
def status_for(where):
    toks = []
    if "static" in where: toks.append("static")
    if "per-VM" in where: toks.append("per-VM")
    if "template" in where: toks.append("template")
    if "INHERENT" in where: toks.append("per-VM")
    keys = set()
    for t in toks:
        if t == "static": keys.add("static")
        elif t == "template": keys.add("template")
        elif t == "per-VM": keys |= {s["name"] for s in SPECS}
    if failed & keys: return "FAIL"
    if keys & ran:    return "PASS"
    return "n/a"

print("\n" + "=" * 70 + "\nCOVERAGE  (function -> where tested)\n" + "=" * 70)
print("  %-44s %-6s %s" % ("function / endpoint", "status", "covered by"))
print("  " + "-" * 68)
for feature, where in COVERAGE:
    print("  %-44s %-6s %s" % (feature, status_for(where), where))

ok = not any(results.values())
print("\n%s" % ("ALL GREEN" if ok else "SOME FAILED"))
sys.exit(0 if ok else 1)
