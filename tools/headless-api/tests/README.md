# headless-api tests

Self-contained integration tests for the AppSandbox headless daemon. They drive
the daemon over its HTTP/JSON API through the [`asb.py`](../asb.py) client — no
test framework, just stdlib Python. Every test creates the throwaway VMs it needs
(named `brk-*`) and deletes them at the end, so the suite **assumes no existing
configuration and never touches your VMs, templates, or snapshots**. Snapshots
are gated on `version()["capabilities"]`.

## Running

Start the daemon (elevated; `AppSandbox.exe` is the installed app's execution
alias), point the suite at your Windows install ISO, then run the harness from
this folder's parent:

```
AppSandbox.exe --headless
set ASB_ISO_WINDOWS=C:\path\to\Win11_x64.iso
python tests\run_all.py
```

The ISO defaults to this `tests/` folder if the env var is unset. A full run
builds real guests — it is long (a Windows install + sysprep can take 20–45 min)
and uses real disk.

## Files

| File | What it covers |
| --- | --- |
| `run_all.py` | Master harness. Sweeps leftover `brk-*` VMs, runs the static checks once, then one full per-VM lifecycle per spec (concurrently when there are several), then the Windows template lifecycle. Prints a per-thread summary and a coverage table. This is the entry point. |
| `vm_lifecycle.py` | The full per-VM sequence, parametrized by a create-spec. Covers create / status / edit / start / graceful shutdown / force-stop / SSE / delete, the during-build guards, **SSH key auto-deploy** (build with `sshDeployKey`, assert `keyDeployed`/`sshState 4`, then a key-only login), a quick **display open/close** once online (skipped in non-interactive daemon sessions), and — where `capabilities.snapshots` — snapshots + branches + base branches. |
| `test_host_and_validation.py` | No-VM checks: host cores/RAM cross-checked against the exact Win32 calls the daemon makes, capability gates, and the create-input-validation rejections matching the GUI's guards. Builds nothing. |
| `test_windows_template.py` | Template lifecycle: create a template (`isTemplate`), wait for sysprep finalization, create a VM from it, then delete the template. |
| `helpers.py` | Shared helpers: paths (`vm_dir`, disk name), `destroy_vm` (guard-aware teardown), `wait_progress` (stall-based waiting driven by the daemon's live progress reporting — no fixed deadlines on phases that report progress), and `ssh_key_login` (key-only BatchMode SSH login). |

## Notes

- **No arbitrary deadlines on progress-reporting phases.** Builds and installs
  are waited on with `helpers.wait_progress`: any change in the
  daemon-reported `state`/`progress`/`installStatus` resets a stall clock, and
  the wait fails only after a long stretch of zero observable movement. Fixed
  timeouts remain only for signal-less single transitions (boot→online,
  graceful shutdown).
- **Lifecycle discipline mirrors the GUI.** Every VM start boots all the way to
  *online* (guest agent connected) before the VM is used; the only exceptions are
  the delete/edit-during-build guards. Every shutdown is the graceful,
  agent-mediated one — force-stop is exercised in exactly one dedicated test.
- **Guest agent provenance.** The guest agent is built locally and staged into
  the guest at create time, so the key-deploy checks pass with a local build.
- Default guest credentials are `user` / `test123`.
