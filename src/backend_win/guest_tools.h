#ifndef GUEST_TOOLS_H
#define GUEST_TOOLS_H

#include <windows.h>
#include "hcs_vm.h"

/* TRUE for VMs whose guest components this module can refresh (Windows
   guests, not templates). */
BOOL guest_tools_applies(const VmInstance *vm);

/* TRUE when the app has newer guest components than this VM last received
   (or the VM never received any through this module). Only for VMs whose
   installation finished. Cheap: hashes the host files, no disk access. */
BOOL guest_tools_outdated(const VmInstance *vm);

/* Copy the current guest components into the stopped VM's disk and schedule
   the display driver update for its next boot. The VM must be stopped and
   its disk not in use. On failure err holds a user-facing reason. Blocks for
   a few seconds (attach + copy + detach): call off the UI thread. */
HRESULT guest_tools_update(VmInstance *vm, wchar_t *err, size_t err_cap);

/* GPU driver pre-sync: TRUE when the host's GPU driver folders changed since
   this VM last received them offline. */
BOOL guest_gpu_presync_needed(const VmInstance *vm, const GpuDriverShareList *shares);

/* Copy the host's GPU driver folders (AppSandbox.Drv.* shares) into the
   stopped VM's HostDriverStore before it boots, so a host driver update
   never boots the guest on mismatched files. Skips files already there. */
HRESULT guest_gpu_presync(VmInstance *vm, const GpuDriverShareList *shares, int *files_out,
                          wchar_t *err, size_t err_cap);

#endif /* GUEST_TOOLS_H */
