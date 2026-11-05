/*
 * appsandbox-displays.exe — Tiny helper that enumerates active monitors
 * and prints count + resolutions to stdout, then exits.
 *
 * Output format (single line):
 *   <count>,<W>x<H>,<W>x<H>,...
 *
 * Example:
 *   2,1920x1080,1024x768
 *
 * Must be launched in the interactive session (WinSta0\Default) so that
 * EnumDisplayDevices / EnumDisplaySettings see all displays.
 *
 *   appsandbox-displays.exe --set <W> <H> <HZ>
 *     Switch the AppSandbox virtual display to WxH@HZ. The driver may still
 *     be re-plugging the monitor with the new mode list, so this retries for
 *     a few seconds. Prints "ok:WxH@HZ" or "error:<reason>".
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Find the display device (\\.\DISPLAYn) driven by the AppSandbox VDD: its
   monitor's hardware ID carries the EDID vendor/product "ASB0001". Falls back
   to the primary display (Hyper-V Video is disabled once the VDD is up). */
static BOOL find_vdd_display(wchar_t *name, size_t name_chars)
{
    DISPLAY_DEVICEW dd, mon;
    DWORD i;
    BOOL have_primary = FALSE;
    wchar_t primary[32] = L"";

    for (i = 0; ; i++) {
        memset(&dd, 0, sizeof(dd));
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(NULL, i, &dd, 0)) break;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;

        memset(&mon, 0, sizeof(mon));
        mon.cb = sizeof(mon);
        if (EnumDisplayDevicesW(dd.DeviceName, 0, &mon, 0) &&
            wcsstr(mon.DeviceID, L"ASB0001")) {
            wcsncpy_s(name, name_chars, dd.DeviceName, _TRUNCATE);
            return TRUE;
        }
        if (wcsstr(dd.DeviceString, L"AppSandbox")) {
            wcsncpy_s(name, name_chars, dd.DeviceName, _TRUNCATE);
            return TRUE;
        }
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
            wcsncpy_s(primary, 32, dd.DeviceName, _TRUNCATE);
            have_primary = TRUE;
        }
    }
    if (have_primary) {
        wcsncpy_s(name, name_chars, primary, _TRUNCATE);
        return TRUE;
    }
    return FALSE;
}

static BOOL find_mode(const wchar_t *device, DWORD w, DWORD h, DWORD hz, DEVMODEW *out)
{
    DEVMODEW dm;
    DWORD i;
    for (i = 0; ; i++) {
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsExW(device, i, &dm, EDS_RAWMODE)) break;
        if (dm.dmPelsWidth == w && dm.dmPelsHeight == h &&
            (hz == 0 || dm.dmDisplayFrequency == hz)) {
            *out = dm;
            return TRUE;
        }
    }
    return FALSE;
}

static int set_mode(DWORD w, DWORD h, DWORD hz)
{
    wchar_t device[32];
    DEVMODEW dm, cur;
    int attempt;
    LONG rc = DISP_CHANGE_FAILED;

    if (w < 640 || h < 480 || w > 7680 || h > 4320 || hz < 30 || hz > 240) {
        printf("error:out_of_range");
        return 2;
    }

    /* After a monitor re-plug the device name and mode list take a moment
       to settle; poll for up to ~10 s. */
    for (attempt = 0; attempt < 40; attempt++) {
        if (find_vdd_display(device, 32)) {
            memset(&cur, 0, sizeof(cur));
            cur.dmSize = sizeof(cur);
            if (EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &cur) &&
                cur.dmPelsWidth == w && cur.dmPelsHeight == h &&
                cur.dmDisplayFrequency == hz) {
                printf("ok:%lux%lu@%lu", w, h, hz);
                return 0;
            }
            if (find_mode(device, w, h, hz, &dm)) {
                dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
                rc = ChangeDisplaySettingsExW(device, &dm, NULL,
                                              CDS_UPDATEREGISTRY | CDS_GLOBAL, NULL);
                if (rc == DISP_CHANGE_SUCCESSFUL) {
                    printf("ok:%lux%lu@%lu", w, h, hz);
                    return 0;
                }
            }
        }
        Sleep(250);
    }
    printf("error:mode_unavailable(%ld)", rc);
    return 1;
}

int main(int argc, char **argv)
{
    DISPLAY_DEVICEW dd;
    DEVMODEW dm;
    char buf[512];
    int count = 0;
    int off = 0;
    DWORD i;

    if (argc == 5 && strcmp(argv[1], "--set") == 0)
        return set_mode((DWORD)strtoul(argv[2], NULL, 10),
                        (DWORD)strtoul(argv[3], NULL, 10),
                        (DWORD)strtoul(argv[4], NULL, 10));

    buf[0] = '\0';

    memset(&dd, 0, sizeof(dd));
    dd.cb = sizeof(dd);

    for (i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); i++) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE)) {
            dd.cb = sizeof(dd);
            continue;
        }

        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);

        if (EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm) &&
            dm.dmPelsWidth > 0 && dm.dmPelsHeight > 0) {
            if (count > 0 && off < (int)sizeof(buf) - 1) {
                buf[off++] = ',';
                buf[off] = '\0';
            }
            if (off < (int)sizeof(buf) - 1) {
                int n = snprintf(buf + off, sizeof(buf) - (size_t)off, "%lux%lu",
                                 dm.dmPelsWidth, dm.dmPelsHeight);
                if (n < 0) break;
                off += n;
                if (off >= (int)sizeof(buf)) {
                    off = (int)sizeof(buf) - 1;
                    buf[off] = '\0';
                    break;
                }
            }
        }
        count++;

        dd.cb = sizeof(dd);
    }

    printf("%d,%s", count, buf);
    return 0;
}
