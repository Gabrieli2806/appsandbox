"""
Shared helpers for the headless-api test suite. Every test
prepares the throwaway VM(s) it needs and removes them afterward -- no test
assumes any pre-existing VM or configuration. Throwaways are named
``brk-<tag>``; ``destroy_vm`` removes both the VM and its on-disk folder.

Install image: drop the Windows ISO next to the tests or override it with the
env var ``ASB_ISO_WINDOWS``.
"""
import os, sys, time, shutil, subprocess, tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # for asb.py (parent dir)
import asb

_HERE = os.path.dirname(os.path.abspath(__file__))
ASB_DIR = asb._support_dir()
# Per-VM folders live directly under the support dir.
VMS_DIR = ASB_DIR
DISK_NAME = "disk.vhdx"
ISO_WIN = os.environ.get("ASB_ISO_WINDOWS", os.path.join(_HERE, "Win11_x64.iso"))


def vm_dir(name):
    """On-disk folder for a VM, per platform."""
    return os.path.join(VMS_DIR, name)


def status_or_none(c, name):
    """Status dict for ``name``, or None if the VM no longer exists (404)."""
    try:
        return c.status(name)
    except KeyError:
        return None


def wait_progress(c, name, done, stall=600, interval=5, label="", missing_ok=False):
    """Progress-aware wait: poll ``name`` until ``done(status)`` is true.

    No big arbitrary deadline: the budget is a STALL window. Every observable
    change -- state, progress %, installStatus text, agentOnline, sshState --
    resets the clock, so a 20-GB restore-image download or a long guest
    install never trips it while it is moving, and a genuinely hung one fails
    after ``stall`` seconds of zero movement. The daemon's status fields are
    live level signals, which is what makes this reliable.

    ``missing_ok``: tolerate the VM not existing yet (used as a start gate
    while another thread is still creating it). Raises TimeoutError on stall,
    RuntimeError if the VM vanishes mid-wait (missing_ok=False).
    """
    last_change = time.time()
    sig = None
    while True:
        s = status_or_none(c, name)
        if s is None:
            if not missing_ok and sig is not None:
                raise RuntimeError("%s vanished (404) while waiting" % name)
        else:
            if done(s):
                return s
            new_sig = (s["state"], s.get("progress"), s.get("installStatus"),
                       s.get("agentOnline"), s.get("sshState"))
            if new_sig != sig:
                sig = new_sig
                last_change = time.time()
                if label:
                    print("    [%s] %s state=%s progress=%s status=%r"
                          % (time.strftime("%H:%M:%S"), label, s["state"],
                             s.get("progress"), s.get("installStatus", "")),
                          flush=True)
        if time.time() - last_change > stall:
            raise TimeoutError("%s made no progress for %ds (last: %s)"
                               % (name, stall, sig))
        time.sleep(interval)


def destroy_vm(c, name):
    """Force-stop, delete, and rmtree a throwaway VM -- guard-aware (waits out any
    in-flight build/install, since delete is refused meanwhile). Safe to call on
    a name that no longer exists."""
    deadline = time.time() + 600
    while time.time() < deadline:
        s = status_or_none(c, name)
        if s is None:
            break
        if s["building"] or s["state"] == "installing":
            time.sleep(5); continue
        if s["running"]:
            c.stop(name); time.sleep(3)
        c.delete_vm(name); time.sleep(2)
        break
    d = vm_dir(name)
    if os.path.isdir(d):
        shutil.rmtree(d, ignore_errors=True)


def ssh_key_login(port, user, command="whoami", timeout=90):
    """Run ``command`` in the guest over SSH using the deployed AppSandbox key,
    with BatchMode (key auth only -- no password fallback, so success proves the
    key works). ssh refuses broad-permission keys, so the key is copied to a
    private temp file first (the source is admin-owned in %ProgramData%; icacls
    restricts the copy). Returns ``(returncode, stdout, stderr)``."""
    keysrc = asb.key_path()
    if not os.path.isfile(keysrc):
        return (1, "", "AppSandbox private key not found: %s" % keysrc)
    tmpkey = os.path.join(tempfile.gettempdir(), "asb_test_id")
    known = os.path.join(tempfile.gettempdir(), "asb_known_hosts")
    shutil.copyfile(keysrc, tmpkey)
    subprocess.run(["icacls", tmpkey, "/inheritance:r", "/grant:r",
                    "%s:F" % os.environ.get("USERNAME", "")], capture_output=True)
    try:
        r = subprocess.run(
            ["ssh", "-i", tmpkey, "-p", str(port),
             "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=" + known,
             "-o", "BatchMode=yes", "-o", "ConnectTimeout=20",
             "%s@127.0.0.1" % user, command],
            capture_output=True, text=True, timeout=timeout)
        return (r.returncode, r.stdout, r.stderr)
    except Exception as e:
        return (1, "", repr(e))
    finally:
        for junk in (tmpkey, known):
            try: os.remove(junk)
            except OSError: pass
