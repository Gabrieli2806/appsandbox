#ifndef VM_DISPLAY_IDD_H
#define VM_DISPLAY_IDD_H

#include <windows.h>
#include "hcs_vm.h"

typedef struct VmDisplayIdd VmDisplayIdd;

/* Create IDD display window for VM. Connects to VM's AF_HYPERV channels
   (:0002 frames, :0003 input, :0005 clipboard writer, :0006 clipboard reader)
   and renders received frames via D3D11.
   main_hwnd receives WM_VM_DISPLAY_CLOSED when closed. */
VmDisplayIdd *vm_display_idd_create(VmInstance *vm, HINSTANCE hInstance, HWND main_hwnd);

void vm_display_idd_destroy(VmDisplayIdd *display);
BOOL vm_display_idd_is_active(VmDisplayIdd *display);
BOOL vm_display_idd_is_open(VmDisplayIdd *display);
UINT_PTR vm_display_idd_token(VmDisplayIdd *display);

/* Bring an already-open display window to the foreground/focus.
   Safe to call from any thread; the work is marshaled to the window thread. */
void vm_display_idd_focus(VmDisplayIdd *display);

/* The display mode saved for a VM (display_settings.json beside its disk):
   res receives "adaptive" or "WxH", hz the refresh rate. Defaults to
   1920x1080 @ 60 Hz when nothing is saved. */
void vm_display_idd_saved_mode(const wchar_t *vhdx_path, wchar_t *res, size_t res_chars,
                               UINT *hz);

/* Change a VM's display mode from the management UI. res is "adaptive" or
   one of the offered "WxH" presets; hz 60, 120 or 144. When the VM's display
   window is open (display != NULL) the change is applied live by that window,
   which also saves it; otherwise it is saved for the next time. Returns FALSE
   for a mode that is not offered. Call from the UI thread. */
BOOL vm_display_idd_set_mode(VmDisplayIdd *display, const wchar_t *vhdx_path,
                             const wchar_t *res, UINT hz);

/* What the guest's display driver supports, as seen on the open window's
   frame channel: -1 = not known yet (window closed or no frame yet),
   0 = older driver (fixed 1920x1080, mode changes do nothing),
   >= 2 = driver that accepts resolution / refresh changes. */
int vm_display_idd_guest_driver(VmDisplayIdd *display);

#endif /* VM_DISPLAY_IDD_H */
