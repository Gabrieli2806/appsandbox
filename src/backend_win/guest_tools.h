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

#endif /* GUEST_TOOLS_H */
