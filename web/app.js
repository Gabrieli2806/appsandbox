/* App Sandbox - WebView2 Frontend */
'use strict';

/* ---- State ---- */
let vms = [];
let coreStarting = true;   /* see setCoreStarting */
let selectedVm = -1;
let selectedSnap = new Map();
let editVmState = null;
let snapshotModal = null;
let pendingConfirm = null; /* {resolve} */
let minSizeReported = false;
let lastHostInfo = null;
let diskSpaceInfo = null;
let diskSpaceRequestId = 0;
let diskSpaceTimer = null;
let diskSpacePending = false;
let diskDirectoryBeforeEdit = null;
let rowCache = {};
let rowSigCache = {};
let createPending = false;   /* createVm sent, waiting for createResult (Windows host) */
let createOpener = null;     /* control that opened the create dialog */
let prereqState = null;      /* null | 'missing' | 'enabling' | 'reboot' | 'failed' */
let exitState = null;        /* {remaining, session} while App Sandbox shuts VMs down to exit */

/* ---- Collapsible sections ---- */
function toggleSection(id) {
    var section = document.getElementById(id);
    var collapsed = section.classList.toggle('collapsed');
    storePref('collapse_' + id, collapsed ? '1' : '0');
    if (!collapsed && id === 'log-section') {
        var panel = document.getElementById('log-panel');
        panel.scrollTop = panel.scrollHeight;
    }
}
(function restoreCollapse() {
    var defaults = { 'log-section': '1' };
    Object.keys(defaults).forEach(function(id) {
        if (storedPref('collapse_' + id, defaults[id]) === '1') document.getElementById(id).classList.add('collapsed');
    });
})();

const netNames = ['None', 'NAT', 'External', 'Internal'];

/* ---- Message bridge ----
 *
 * Two host environments are supported:
 *   - WebView2 on Windows  (window.chrome.webview)
 *   - WKWebView on macOS   (window.webkit.messageHandlers.host)
 *
 * Native code on both platforms calls window.onHostMessage(obj) with a
 * parsed message object; the JS side only sees one uniform surface. On
 * Windows we keep using the native chrome.webview event path because it
 * is the existing, tested route — onHostMessage is simply wired into the
 * same listener.
 */

var hostBridge = (function() {
    var isWebView2 = !!(window.chrome && window.chrome.webview);
    var isWKWebView = !!(window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.host);

    function send(action, data) {
        var msg = Object.assign({ action: action }, data || {});
        if (isWebView2) {
            window.chrome.webview.postMessage(msg);
        } else if (isWKWebView) {
            /* WKWebView only accepts JSON-serializable values; strings round-trip
             * most reliably so we hand the native side the raw JSON text. */
            window.webkit.messageHandlers.host.postMessage(JSON.stringify(msg));
        } else {
            console.warn('[hostBridge] no native host available; dropping', msg);
        }
    }

    return { send: send, isWebView2: isWebView2, isWKWebView: isWKWebView, isMac: isWKWebView };
})();

/* Returns false when the command was not sent (its VM or template is gone). */
function sendCmd(action, data) {
    if (data && data.vmId && vmIndexById(data.vmId) < 0) return false;
    if (data && data.templateId && !currentTemplates.some(function(t) { return t.id === data.templateId; })) return false;
    hostBridge.send(action, data);
    return true;
}

if (hostBridge.isMac) {
    var hide = document.querySelectorAll('.win-only, .windows-host-only, .needs-linux-version');
    for (var i = 0; i < hide.length; i++) hide[i].style.display = 'none';
}

/* OS Type dropdown: drop guest types that aren't available on this host.
 * Windows host: macOS unavailable (Apple Virtualization is Mac-only).
 * macOS host:   Linux unavailable (Windows IS supported — QEMU+ivshmem). */
{
    var unavailable = hostBridge.isMac ? ['Linux', 'Other'] : ['macOS'];
    unavailable.forEach(function(v) {
        var opt = document.querySelector('#os-type option[value="' + v + '"]');
        if (opt) opt.remove();
    });
}

function applyOsTypeUI() {
    var modal = document.getElementById('create-vm-overlay');
    var osType = document.getElementById('os-type').value;
    var isWindows = osType === 'Windows';
    var isLinux = osType === 'Linux';
    var isManual = osType === 'Other';
    modal.querySelectorAll('.needs-agent').forEach(function(el) { el.style.display = isManual ? 'none' : ''; });
    document.getElementById('manual-note').hidden = !isManual;
    if (isManual) {
        var net = document.getElementById('net-mode');
        if (net && net.value === '1') net.value = '3';   /* Internal: DHCP for any OS */
    }
    var selected = currentTemplates.find(function(t) { return t.name === document.getElementById('template-select').value; });
    if (selected && selected.osType !== osType) selectTemplate('', templateDefaultLabel());
    modal.querySelectorAll('.templates-supported').forEach(function(el) {
        el.style.display = (isWindows || osType === 'macOS') ? '' : 'none';
    });
    populateTemplates(currentTemplates);
    var winOnly = modal.querySelectorAll('.win-only');
    var needsIso = modal.querySelectorAll('.needs-iso');
    var needsWindows = modal.querySelectorAll('.needs-windows');
    var needsLinuxVersion = modal.querySelectorAll('.needs-linux-version');
    /* .win-only = template/snapshot features that exist only on a Windows *host*;
       never shown on a Mac host, even for a Windows guest. */
    for (var i = 0; i < winOnly.length; i++)
        winOnly[i].style.display = (!hostBridge.isMac && isWindows) ? '' : 'none';
    /* .needs-windows = Windows-*guest* options (Test Mode); shown for a Windows
       guest on EITHER host (a Windows-on-Mac VM uses it too), hidden otherwise. */
    for (var w = 0; w < needsWindows.length; w++) needsWindows[w].style.display = isWindows ? '' : 'none';
    /* ISO picker shows for both Windows and Linux now. */
    for (var j = 0; j < needsIso.length; j++) needsIso[j].style.display = (isWindows || isLinux || isManual || osType === 'macOS') ? '' : 'none';
    document.getElementById('image-path').placeholder = osType === 'macOS' ? 'Latest macOS restore image (or select IPSW)...' : 'Path to ISO...';
    /* Linux distribution dropdown is dormant — kept in the DOM but always
       hidden so the cloud-image code path can be revived without
       re-adding the markup. */
    for (var k = 0; k < needsLinuxVersion.length; k++) needsLinuxVersion[k].style.display = 'none';
    /* Swap the default VM name between OS conventions, but only when the
       field still holds the *other* OS's untouched default — never clobber a
       name the user typed. Linux hostnames must be lowercase. */
    var nameEl = document.getElementById('vm-name');
    if (isLinux && nameEl.value === 'MyAppSandbox') nameEl.value = 'myappsandbox';
    else if (!isLinux && nameEl.value === 'myappsandbox') nameEl.value = 'MyAppSandbox';
    revalidateVmName();
    revalidateUsername();
    revalidatePassword();
    updateCreateButtons();
}

/* Unified dispatch. Native code on either platform calls
 * window.onHostMessage(obj) with an already-parsed object. WebView2 also
 * delivers messages through chrome.webview.addEventListener('message'),
 * which we route into the same handler so both paths end up in one place. */
window.onHostMessage = function(msg) {
    if (!msg || typeof msg !== 'object') return;
    switch (msg.type) {
        case 'fullState':     onFullState(msg); break;
        case 'vmListChanged': updateVmList(msg.vms); renderVmTable(); updateHostInfo(msg.hostInfo); revalidateVmName(); break;
        case 'vmStateChanged': onVmStateChanged(msg); break;
        case 'snapListChanged': break;
        case 'log':           appendLog(msg.message); break;
        case 'hostInfo':      updateHostInfo(msg); break;
        case 'browseResult':  onBrowseResult(msg.path); break;
        case 'diskDirectoryBrowseResult': onDiskDirectoryBrowseResult(msg.path); break;
        case 'diskSpace':     onDiskSpace(msg); break;
        case 'confirmResult': if (pendingConfirm) pendingConfirm.resolve(msg.confirmed); break;
        case 'adapters':      populateAdapters(msg.adapters, msg.defaultIndex); break;
        case 'templates':     populateTemplates(msg.templates); break;
        case 'alert':         showAlert(msg); break;
        case 'notice':        showToast(msg.kind, msg.message); break;
        case 'createResult':  onCreateResult(msg); break;
        case 'exitProgress':  onExitProgress(msg); break;
        case 'document':      showDocument(msg); break;
        case 'prereqRequired': setPrereqState('missing'); setCoreStarting(false); onPrereqRequired(); break;
        case 'prereqReboot':   setPrereqState('reboot'); onPrereqReboot(); break;
        case 'prereqProgress': onPrereqProgress(msg); break;
        case 'prereqResult':   onPrereqResult(msg); break;
        case 'vmIcon':         onVmIcon(msg); break;
        case 'coreLoading':    setPrereqState(null); setCoreStarting(true); break;
        case 'coreReady':      setPrereqState(null); setCoreStarting(false); break;
    }
};

/* WebView2 delivers events as DOM CustomEvents; forward them into
 * window.onHostMessage so both transports converge on the same handler. */
if (hostBridge.isWebView2) {
    window.chrome.webview.addEventListener('message', function(event) {
        window.onHostMessage(event.data);
    });
}

/* ---- Initial state ---- */

/* The core starts in the background (prerequisites, GPUs, VM list); until
   it is ready the list shows a loading state and "New VM" waits. */
function setCoreStarting(on) {
    coreStarting = !!on;
    updateLibraryButtons();
    renderVmTable();
}

/* New VM and Import wait for the core, and for Virtual Machine Platform. */
function updateLibraryButtons() {
    var off = coreStarting || !!prereqState;
    ['btn-new-sandbox', 'btn-import-vm'].forEach(function(id) {
        var b = document.getElementById(id);
        if (!b) return;
        if (!b.hasAttribute('data-title')) b.setAttribute('data-title', b.title || '');
        b.disabled = off;
        b.title = prereqState ? 'Virtual Machine Platform must be on first' : b.getAttribute('data-title');
    });
}

function setPrereqState(state) {
    prereqState = state;
    updateLibraryButtons();
    renderVmTable();
}

function onFullState(msg) {
    coreStarting = false;
    prereqState = null;
    closePrereqDialog();
    updateLibraryButtons();
    updateVmList(msg.vms || []);
    renderVmTable();
    revalidateVmName();
    if (msg.hostInfo) updateHostInfo(msg.hostInfo);
    if (msg.adapters) populateAdapters(msg.adapters, msg.defaultAdapter);
    if (msg.templates) populateTemplates(msg.templates);
    if (!minSizeReported) {
        minSizeReported = true;
        setTimeout(reportMinSize, 50);
    }
}

/* Hyper-V/HCS requires VM memory aligned to 2 MB, so RAM (MB) must be even;
   round an odd value down by 1 (an odd value is rejected and the VM won't boot). */
function alignRamMb(mb) { return mb - (mb % 2); }

function applySmartDefaults(info) {
    var ram = Math.min(Math.floor(info.hostRamMb / 2), 16384);
    var cores = Math.min(Math.floor(info.hostCores / 2), 8);
    if (ram < 512) ram = 512;
    if (cores < 1) cores = 1;
    document.getElementById('ram-size').value = alignRamMb(ram);
    document.getElementById('cpu-cores').value = cores;
}

function onVmStateChanged(msg) {
    var index = vmIndexById(msg.id);
    if (index >= 0) {
        Object.assign(vms[index], msg);
    }
    renderVmTable();
    if (msg.hostInfo) updateHostInfo(msg.hostInfo);
}

/* ---- Host info ---- */

function updateHostInfo(info) {
    if (!info) return;
    var previousDefault = lastHostInfo && lastHostInfo.defaultDiskDirectory;
    if (!hostBridge.isMac && Array.isArray(info.gpus) &&
        JSON.stringify(info.gpus) !== JSON.stringify(lastHostInfo && lastHostInfo.gpus))
        populateGpus(info.gpus);
    lastHostInfo = info;
    updateHostMeters(info);
    renderSettingsView();
    var diskDirectory = document.getElementById('disk-directory');
    if (diskDirectoryBeforeEdit === null && info.defaultDiskDirectory &&
        (!diskDirectory.value || diskDirectory.value === previousDefault))
        diskDirectory.value = info.defaultDiskDirectory;
    var el;
    el = document.getElementById('host-cpu');
    if (el) el.textContent = 'Host: ' + info.hostCores + ' cores | VMs using: ' + info.vmCores;
    el = document.getElementById('host-ram');
    if (el) el.textContent = 'Host: ' + info.hostRamMb + ' MB | VMs using: ' + info.vmRamMb + ' MB';
    document.getElementById('edit-host-cpu').textContent = document.getElementById('host-cpu').textContent;
    document.getElementById('edit-host-ram').textContent = document.getElementById('host-ram').textContent;
    if (previousDefault !== info.defaultDiskDirectory) refreshDiskSpaceInfo();
    else {
        updateDiskSpaceInfo();
        if (document.getElementById('create-vm-overlay').classList.contains('active') && !diskSpacePending)
            refreshDiskSpaceInfo(true);
    }
}

function gpuSelectionValue(selection) {
    return selection.gpuMode === 1 && selection.gpuId ? 'gpu:' + selection.gpuId : String(selection.gpuMode);
}

function selectedGpu(id) {
    var value = document.getElementById(id).value;
    return value.indexOf('gpu:') === 0 ?
        { gpuMode: 1, gpuId: value.slice(4) } : { gpuMode: Number(value), gpuId: '' };
}

function updateGpuTitle(select) {
    var option = select.options[select.selectedIndex];
    select.title = option ? (option.title || option.textContent) : '';
}

function setGpuSelection(id, selection) {
    var select = document.getElementById(id);
    select.value = gpuSelectionValue(selection);
    if (select.selectedIndex < 0) select.value = '1';
    updateGpuTitle(select);
}

function populateGpus(gpus) {
    ['gpu-mode', 'edit-gpu-mode'].forEach(function(id) {
        var select = document.getElementById(id);
        var selection = selectedGpu(id);
        while (select.options.length > 2) select.remove(2);
        gpus.forEach(function(gpu) {
            if (!gpu.id) return;
            var option = document.createElement('option');
            option.value = 'gpu:' + gpu.id;
            option.textContent = gpu.name;
            option.title = gpu.location ? gpu.name + '\n' + gpu.location : gpu.name;
            select.appendChild(option);
        });
        setGpuSelection(id, selection);
    });
}

['gpu-mode', 'edit-gpu-mode'].forEach(function(id) {
    document.getElementById(id).addEventListener('change', function() { updateGpuTitle(this); });
});

function selectedDiskDirectory() {
    return document.getElementById('disk-directory').value.trim() ||
        (lastHostInfo && lastHostInfo.defaultDiskDirectory) || '';
}

function updateDiskSpaceInfo() {
    if (!lastHostInfo) return;
    var path = selectedDiskDirectory();
    var free = 'Checking...';
    if (!path || validateDiskDirectory(path)) free = 'Unavailable';
    else if (diskSpaceInfo && diskSpaceInfo.path === path)
        free = diskSpaceInfo.freeGb >= 0 ? diskSpaceInfo.freeGb + ' GB' : 'Unavailable';
    document.getElementById('host-hdd').textContent =
        'Free: ' + free + ' | VMs allocated: ' + lastHostInfo.vmHddGb + ' GB';
    document.getElementById('disk-location-space').textContent = 'Free: ' + free;
    document.getElementById('btn-disk-location').title = 'HDD Location: ' + path;
}

function refreshDiskSpaceInfo(keepPrevious) {
    clearTimeout(diskSpaceTimer);
    diskSpaceTimer = null;
    var path = selectedDiskDirectory();
    var requestId = ++diskSpaceRequestId;
    if (!keepPrevious) diskSpaceInfo = null;
    diskSpacePending = !!path && !validateDiskDirectory(path);
    updateDiskSpaceInfo();
    updateCreateButtons();
    if (!diskSpacePending) return;
    diskSpaceTimer = setTimeout(function() {
        diskSpaceTimer = null;
        sendCmd('getDiskSpace', { path: path, requestId: requestId });
    }, 200);
}

function onDiskSpace(msg) {
    if (msg.requestId !== diskSpaceRequestId || msg.path !== selectedDiskDirectory()) return;
    diskSpacePending = false;
    diskSpaceInfo = {
        path: msg.path,
        freeGb: typeof msg.freeGb === 'number' && Number.isFinite(msg.freeGb) ? msg.freeGb : -1
    };
    updateDiskSpaceInfo();
    updateCreateButtons();
}

/* ---- Adapters ---- */

var currentAdapters = [];

function populateAdapters(adapters, defaultIdx) {
    var sel = document.getElementById('net-adapter');
    sel.innerHTML = '<option value="">(Auto)</option>';
    currentAdapters = adapters || [];
    if (adapters) {
        adapters.forEach(function(a) {
            var opt = document.createElement('option');
            opt.value = a;
            opt.textContent = a;
            sel.appendChild(opt);
        });
    }
    if (typeof defaultIdx === 'number' && defaultIdx >= 0 && defaultIdx < sel.options.length) {
        sel.selectedIndex = defaultIdx;
    }
}

/* ---- Templates ---- */

var currentTemplates = [];
var selectedTemplateId = null;

function templatesForSelectedOS() {
    var osType = document.getElementById('os-type').value;
    return currentTemplates.filter(function(t) { return t.osType === osType; });
}

function templateDefaultLabel() {
    var n = templatesForSelectedOS().length;
    if (n === 0) return '(None)';
    return '(' + n + ' template' + (n === 1 ? '' : 's') + ' available)';
}

function populateTemplates(templates) {
    currentTemplates = templates || [];
    var list = document.getElementById('template-dropdown-list');
    var hidden = document.getElementById('template-select');
    list.textContent = '';

    /* One row per choice: a button that selects it, and for templates a
       button that deletes it. All reachable with Tab; Escape closes. */
    function addRow(value, label, template) {
        var row = document.createElement('div');
        row.className = 'template-dropdown-item';
        row.setAttribute('role', 'option');
        row.setAttribute('data-value', value);
        row.setAttribute('aria-selected', hidden.value === value ? 'true' : 'false');
        var pick = document.createElement('button');
        pick.type = 'button';
        pick.className = 'tpl-name';
        pick.textContent = label;
        pick.addEventListener('click', function() {
            selectTemplate(value, value ? label : templateDefaultLabel());
            document.getElementById('template-dropdown-selected').focus();
        });
        row.appendChild(pick);
        if (template) {
            var del = document.createElement('button');
            del.type = 'button';
            del.className = 'tpl-delete';
            del.setAttribute('aria-label', 'Delete template ' + template.name);
            del.title = 'Delete template';
            del.innerHTML = '<svg aria-hidden="true" width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"></path></svg>';
            del.addEventListener('click', function(e) {
                e.stopPropagation();
                closeTemplateDropdown();
                onDeleteTemplate(template.name, template.id);
            });
            row.appendChild(del);
        }
        list.appendChild(row);
    }

    addRow('', '(None)', null);
    templatesForSelectedOS().forEach(function(t) {
        addRow(t.name, t.name + ' [' + t.osType + ']', t);
    });

    /* If the currently selected template was deleted, reset */
    if (hidden.value !== '') {
        var found = currentTemplates.some(function(t) {
            return selectedTemplateId ? t.id === selectedTemplateId : t.name === hidden.value;
        });
        if (!found) selectTemplate('', templateDefaultLabel());
    } else {
        /* No template selected — update default label in case count changed */
        document.getElementById('template-dropdown-selected').textContent = templateDefaultLabel();
    }
    revalidateVmName();
}

function selectTemplate(value, label) {
    document.getElementById('template-select').value = value;
    document.getElementById('template-dropdown-selected').textContent = label;
    closeTemplateDropdown();
    if (value !== '') {
        document.getElementById('image-path').value = '';
    }
    var template = currentTemplates.find(function(t) { return t.name === value; });
    selectedTemplateId = template ? template.id || null : null;
    var disk = document.getElementById('hdd-size');
    if (template && template.hddGb) {
        disk.value = Math.max(Number(disk.value), template.hddGb);
    }
    document.querySelectorAll('#template-dropdown-list .template-dropdown-item').forEach(function(row) {
        row.setAttribute('aria-selected', String(row.getAttribute('data-value') === value));
    });
    updateCreateButtons();
}

function templateDropdownOpen() {
    return document.getElementById('template-dropdown').classList.contains('open');
}

function closeTemplateDropdown() {
    document.getElementById('template-dropdown').classList.remove('open');
    document.getElementById('template-dropdown-selected').setAttribute('aria-expanded', 'false');
}

function openTemplateDropdown(focusFirst) {
    document.getElementById('template-dropdown').classList.add('open');
    document.getElementById('template-dropdown-selected').setAttribute('aria-expanded', 'true');
    if (focusFirst) {
        var first = document.querySelector('#template-dropdown-list .tpl-name');
        if (first) first.focus();
    }
}

document.getElementById('template-dropdown-selected').addEventListener('click', function() {
    if (templateDropdownOpen()) closeTemplateDropdown(); else openTemplateDropdown(false);
});
document.getElementById('template-dropdown-selected').addEventListener('keydown', function(e) {
    if (e.key === 'ArrowDown') { e.preventDefault(); openTemplateDropdown(true); }
});
document.getElementById('template-dropdown-list').addEventListener('keydown', function(e) {
    if (e.key !== 'ArrowDown' && e.key !== 'ArrowUp') return;
    var picks = Array.prototype.slice.call(this.querySelectorAll('.tpl-name'));
    var i = picks.indexOf(document.activeElement);
    if (i < 0) return;
    e.preventDefault();
    var next = picks[Math.max(0, Math.min(picks.length - 1, i + (e.key === 'ArrowDown' ? 1 : -1)))];
    if (next) next.focus();
});

/* Close dropdown when clicking outside */
document.addEventListener('click', function(e) {
    if (!e.target.closest('#template-dropdown')) {
        closeTemplateDropdown();
    }
});

function onDeleteTemplate(name, id) {
    showModal(
        'Delete template?',
        'Delete template "' + name + '"?\n\nIts disk image is deleted permanently.',
        'Delete'
    ).then(function(confirmed) {
        if (confirmed) {
            sendCmd('deleteTemplate', { name: name, templateId: id });
        }
    });
}

/* ---- Browse result ---- */

function onBrowseResult(path) {
    if (path) {
        document.getElementById('image-path').value = path;
        selectTemplate('', templateDefaultLabel());
        updateCreateButtons();
    }
}

function onDiskDirectoryBrowseResult(path) {
    if (!path || diskDirectoryBeforeEdit === null) return;
    document.getElementById('disk-directory').value = path;
    revalidateDiskDirectory();
}

function validateDiskDirectory(path) {
    if (!path) return null;  /* Empty uses the host's default. */
    if (/[\x00-\x1f\x7f]/.test(path)) return 'Disk folder cannot contain control characters.';
    if (hostBridge.isMac) {
        if (path[0] !== '/') return 'Disk folder must be an absolute path.';
    } else {
        if (!/^(?:[a-zA-Z]:[\\/]|\\\\[^\\/]+[\\/][^\\/]+)/.test(path))
            return 'Disk folder must be an absolute path.';
        if (/["<>|?*]/.test(path) || /:/.test(path.slice(2)))
            return 'Disk folder contains invalid characters.';
    }
    return null;
}

function revalidateDiskDirectory() {
    var path = document.getElementById('disk-directory').value.trim();
    var error = validateDiskDirectory(path);
    document.getElementById('disk-directory-warn').textContent = error || '';
    document.getElementById('btn-save-disk-location').disabled = !!error;
    refreshDiskSpaceInfo();
}
document.getElementById('disk-directory').addEventListener('input', revalidateDiskDirectory);

function openDiskLocationModal() {
    var createOverlay = document.getElementById('create-vm-overlay');
    if (!createOverlay.classList.contains('active') || diskDirectoryBeforeEdit !== null) return;
    diskDirectoryBeforeEdit = document.getElementById('disk-directory').value;
    document.getElementById('disk-location-overlay').classList.add('active');
    createOverlay.inert = true;
    revalidateDiskDirectory();
    document.getElementById('disk-directory').focus();
    document.getElementById('disk-directory').select();
}

function closeDiskLocationModal(save) {
    if (diskDirectoryBeforeEdit === null) return;
    var input = document.getElementById('disk-directory');
    if (save && validateDiskDirectory(input.value.trim())) {
        revalidateDiskDirectory();
        input.focus();
        return;
    }
    var previousPath = selectedDiskDirectory();
    input.value = save ? input.value.trim() : diskDirectoryBeforeEdit;
    diskDirectoryBeforeEdit = null;
    document.getElementById('disk-location-overlay').classList.remove('active');
    document.getElementById('create-vm-overlay').inert = false;
    if (selectedDiskDirectory() !== previousPath) revalidateDiskDirectory();
    document.getElementById('btn-disk-location').focus();
}

let diskLocationBackdropPress = false;
document.getElementById('disk-location-overlay').addEventListener('mousedown', function(e) {
    diskLocationBackdropPress = (e.target === this);
});
document.getElementById('disk-location-overlay').addEventListener('click', function(e) {
    if (e.target === this && diskLocationBackdropPress) closeDiskLocationModal(false);
    diskLocationBackdropPress = false;
});

/* ---- Create buttons state ---- */

/* First problem in the form, checked in the order the fields appear, or null. */
function createValidationError(isTemplate) {
    var osType = document.getElementById('os-type').value;
    if (isTemplate && osType !== 'Windows' && osType !== 'macOS')
        return 'Templates require a Windows or macOS guest.';
    if (isTemplate && document.getElementById('template-select').value)
        return 'Select an OS image to build a new template.';
    if (osType === 'Other' && isTemplate) return 'Manual-install VMs cannot be templates.';
    var error = validateVmName(document.getElementById('vm-name').value.trim());
    if (error) return error;

    var hasImage = document.getElementById('image-path').value.trim() !== '';
    var hasTemplate = (osType === 'Windows' || osType === 'macOS') &&
        document.getElementById('template-select').value !== '';
    if (osType !== 'macOS' && !hasImage && (isTemplate || !hasTemplate))
        return (osType === 'Windows' && !isTemplate && !hostBridge.isMac)
            ? 'Choose an installer ISO (Browse\u2026) or a template to continue.'
            : 'Choose an installer ISO (Browse\u2026) to continue.';

    error = validateDiskDirectory(document.getElementById('disk-directory').value.trim());
    if (error) return error;
    var numericFields = [
        ['hdd-size', 'Disk size must be a whole number of at least 1 GB.'],
        ['ram-size', 'Memory must be an even number of at least 512 MB.'],
        ['cpu-cores', 'Processors must be a whole number of at least 1.']
    ];
    for (var i = 0; i < numericFields.length; i++) {
        var input = document.getElementById(numericFields[i][0]);
        if (input.value === '' || !input.validity.valid) return numericFields[i][1];
    }
    var path = selectedDiskDirectory();
    var diskKnown = diskSpaceInfo && diskSpaceInfo.path === path;
    if (diskKnown && diskSpaceInfo.freeGb < 0) return 'The disk location is unavailable. Choose another one.';
    if (diskSpacePending && !diskKnown) return 'Checking the disk location\u2026';

    if (osType !== 'Other') {
        error = validateUsername(document.getElementById('admin-user').value.trim(), isTemplate) ||
                validatePassword(document.getElementById('admin-pass').value);
        if (error) return error;
        if (document.getElementById('admin-pass').value !== document.getElementById('admin-confirm').value)
            return document.getElementById('admin-confirm').value
                ? 'The passwords don\u2019t match.' : 'Type the password again in Confirm.';
    }
    return null;
}

/* Error from the host for the last Create (kept until the form changes). */
let createHostError = '';

/* Create stays disabled until the form is complete; the line above the
   buttons says what is missing (or what the host refused). */
function updateCreateButtons() {
    var error = createValidationError(false);
    var templateError = createValidationError(true);
    var create = document.getElementById('btn-create');
    var template = document.getElementById('btn-create-template');
    create.disabled = createPending || !!error;
    create.title = error || '';
    create.textContent = createPending ? 'Creating\u2026' : 'Create VM';
    template.disabled = createPending || !!templateError;
    template.title = templateError || '';
    var hint = document.getElementById('create-hint');
    if (createHostError) {
        hint.textContent = createHostError;
        hint.className = 'form-hint error';
    } else {
        hint.textContent = createPending ? 'Creating the VM\u2026' : (error || '');
        hint.className = 'form-hint';
    }
}

/* Wire up change events */
function onCreateFormEdited() {
    createHostError = '';
    updateCreateButtons();
}
document.getElementById('create-vm-overlay').addEventListener('input', onCreateFormEdited);
document.getElementById('create-vm-overlay').addEventListener('change', onCreateFormEdited);
document.getElementById('image-path').addEventListener('input', function() {
    if (this.value.trim() !== '') {
        selectTemplate('', templateDefaultLabel());
    }
    updateCreateButtons();
});

/* RAM must be 2 MB-aligned: snap an odd entry down by 1 when the field is committed. */
document.getElementById('ram-size').addEventListener('change', function() {
    var mb = this.valueAsNumber;
    if (!isNaN(mb)) this.value = alignRamMb(mb);
});

function revalidateVmName() {
    var name = document.getElementById('vm-name').value.trim();
    document.getElementById('vm-name-warn').textContent = validateVmName(name) || '';
    updateCreateButtons();
}
document.getElementById('vm-name').addEventListener('input', function() {
    revalidateVmName();
    revalidateUsername();
});

function revalidateUsername() {
    var u = document.getElementById('admin-user').value.trim();
    document.getElementById('admin-user-warn').textContent = validateUsername(u) || '';
}
/* The password warning appears once the user typed or left the field, so an
   empty form does not open with errors; the hint line covers the rest. */
function revalidatePassword() {
    var passEl = document.getElementById('admin-pass');
    var confirm = document.getElementById('admin-confirm').value;
    var p = passEl.value;
    var error = (p || passEl.getAttribute('data-touched')) ? validatePassword(p) : null;
    if (!error && confirm && confirm !== p) error = 'The passwords don\u2019t match.';
    document.getElementById('admin-pass-warn').textContent = error || '';
}
document.getElementById('admin-user').addEventListener('input', revalidateUsername);
document.getElementById('admin-pass').addEventListener('blur', function() {
    this.setAttribute('data-touched', '1');
    revalidatePassword();
});

function checkPasswordMatch() {
    var pass = document.getElementById('admin-pass').value;
    var confirm = document.getElementById('admin-confirm');
    if (confirm.value === '') {
        confirm.classList.remove('pass-mismatch', 'pass-match');
        return;
    }
    if (confirm.value === pass) {
        confirm.classList.remove('pass-mismatch');
        confirm.classList.add('pass-match');
    } else {
        confirm.classList.remove('pass-match');
        confirm.classList.add('pass-mismatch');
    }
}
document.getElementById('admin-pass').addEventListener('input', function() {
    checkPasswordMatch();
    revalidatePassword();
});
document.getElementById('admin-confirm').addEventListener('input', function() {
    checkPasswordMatch();
    revalidatePassword();
});
checkPasswordMatch();

/* Show or hide both password fields (a toggle button, usable from the keyboard). */
function togglePasswordVisibility(show) {
    var btn = document.getElementById('toggle-pass');
    if (typeof show !== 'boolean') show = btn.getAttribute('aria-pressed') !== 'true';
    document.getElementById('admin-pass').type = show ? 'text' : 'password';
    document.getElementById('admin-confirm').type = show ? 'text' : 'password';
    btn.setAttribute('aria-pressed', show ? 'true' : 'false');
    btn.setAttribute('aria-label', show ? 'Hide password' : 'Show password');
    btn.title = show ? 'Hide password' : 'Show password';
}

function onNetModeChange() {
    /* Adapter dropdown only relevant for External */
    var mode = parseInt(document.getElementById('net-mode').value);
    var show = (!hostBridge.isMac && mode === 2) ? '' : 'none';
    document.getElementById('net-adapter').style.display = show;
    document.getElementById('net-adapter-label').style.display = show;
}
onNetModeChange();

/* ---- Create VM ---- */

function gatherConfig() {
    var osType = document.getElementById('os-type').value;
    var gpu = selectedGpu('gpu-mode');
    /* Same ISO-picker path for Windows and Linux. The cloud-image
       Linux-version dropdown is dormant (see applyOsTypeUI). */
    var imagePath = document.getElementById('image-path').value.trim();
    return {
        name:        document.getElementById('vm-name').value.trim(),
        osType:      osType,
        imagePath:   imagePath,
        diskDirectory: document.getElementById('disk-directory').value.trim() ===
            (lastHostInfo && lastHostInfo.defaultDiskDirectory) ? '' :
            document.getElementById('disk-directory').value.trim(),
        templateName: document.getElementById('template-select').value,
        templateId: selectedTemplateId || undefined,
        hddGb:       document.getElementById('hdd-size').valueAsNumber,
        ramMb:       alignRamMb(document.getElementById('ram-size').valueAsNumber),
        cpuCores:    document.getElementById('cpu-cores').valueAsNumber,
        gpuMode:     gpu.gpuMode,
        gpuId:       gpu.gpuId,
        networkMode: hostBridge.isMac ? 1 : parseInt(document.getElementById('net-mode').value),
        netAdapter:  hostBridge.isMac ? '' : document.getElementById('net-adapter').value,
        adminUser:   document.getElementById('admin-user').value.trim(),
        adminPass:   document.getElementById('admin-pass').value,
        testMode:    document.getElementById('test-mode').checked,
        sshEnabled:  document.getElementById('ssh-enabled').checked,
        sshDeployKey: document.getElementById('ssh-deploy-key').checked
    };
}

/* "Deploy SSH key" depends on "SSH Server": grey it out (and clear it) unless
   SSH is enabled. The core also gates deploy on ssh_enabled as a backstop. */
function onSshToggle() {
    var ssh = document.getElementById('ssh-enabled').checked;
    var dep = document.getElementById('ssh-deploy-key');
    dep.disabled = !ssh;
    if (!ssh) dep.checked = false;
}

function clearCreateForm() {
    document.getElementById('image-path').value = '';
    document.getElementById('admin-pass').value = '';
    document.getElementById('admin-confirm').value = '';
    selectTemplate('', templateDefaultLabel());
    updateCreateButtons();
}

/* VM name / hostname validation. Per-guest-OS rules, keyed off the
   selected OS Type (on a macOS host the dropdown is locked to 'macOS',
   so osType is an accurate guest discriminator on all hosts). */
function validateVmName(name) {
    if (!name) return 'VM name is required.';
    var osSelect = document.getElementById('os-type');
    var osType = osSelect ? osSelect.value : 'Windows';
    if (osType === 'macOS') {
        if (name.length > 63) return 'VM name cannot exceed 63 characters (macOS LocalHostName limit).';
    } else if (osType === 'Linux') {
        if (name.length > 63) return 'VM name cannot exceed 63 characters (Linux hostname limit).';
        if (/[A-Z]/.test(name)) return 'Linux hostname must be lowercase.';
    } else if (osType === 'Other') {
        if (name.length > 63) return 'VM name cannot exceed 63 characters.';
    } else { /* Windows */
        if (name.length > 15) return 'VM name cannot exceed 15 characters (NetBIOS limit).';
    }
    if (/[^a-zA-Z0-9-]/.test(name)) return 'VM name can only contain letters, digits, and hyphens.';
    if (/^\d+$/.test(name)) return 'VM name cannot be only digits.';
    if (name.startsWith('-') || name.endsWith('-')) return 'VM name cannot start or end with a hyphen.';
    var lower = name.toLowerCase();
    for (var i = 0; i < vms.length; i++) {
        if (vms[i].name.toLowerCase() === lower) return 'A VM with this name already exists.';
    }
    for (var j = 0; j < currentTemplates.length; j++) {
        if (currentTemplates[j].name.toLowerCase() === lower) return 'A template with this name already exists.';
    }
    return null;
}

/* Username validation. Per-guest-OS rules keyed off osType. Each branch
   is explicit so it's clear which OS's account rules apply. */
function validateUsername(name, isTemplate) {
    if (name === undefined) return 'Username is required.';
    if (typeof name !== 'string') return 'Username must be a string.';
    name = name.trim();
    if (!name) return 'Username is required.';
    if (name.indexOf('\u0000') >= 0) return 'Username cannot contain NUL characters.';
    var bytes;
    try {
        bytes = unescape(encodeURIComponent(name)).length;
    } catch (e) {
        return 'Username contains invalid Unicode.';
    }
    var osSelect = document.getElementById('os-type');
    var osType = osSelect ? osSelect.value : 'Windows';
    if (osType === 'Linux') {
        /* Ubuntu 26.04's installer grammar and reserved-usernames list.
           https://github.com/canonical/subiquity/tree/26.04 */
        if (name.length > 32) return 'Username cannot exceed 32 characters (Linux limit).';
        if (!/^[a-z_][a-z0-9_-]*$/.test(name))
            return 'Linux username: lowercase letters, digits, _ and - only; start with a letter or _.';
        var linuxReserved = (
            'root daemon bin sys sync games man lp mail news uucp proxy www-data backup list irc gnats nobody ' +
            'adm tty disk kmem dialout fax voice cdrom floppy tape sudo audio dip operator src shadow utmp video ' +
            'sasl plugdev staff users nogroup netplan ftn mysql tac-plus alias qmail qmaild qmails qmailr qmailq ' +
            'qmaill qmailp asterisk vpopmail vchkpw slurm hacluster haclient grsec-tpe grsec-sock-all grsec-sock-clt ' +
            'grsec-sock-srv grsec-proc ceph opensrf libvirt-qemu admin Debian-exim bind crontab cupsys dcc dhcp ' +
            'dictd dnsmasq dovecot fetchmail firebird ftp fuse gdm haldaemon hplilp identd input jwhois klog kvm ' +
            'lpadmin maas messagebus mythtv netdev powerdev radvd render saned sbuild scanner sgx slocate ssh ' +
            'sshd ssl-cert sslwrap statd syslog telnetd tftpd'
        ).split(' ');
        if (linuxReserved.indexOf(name) >= 0) return 'Username is a reserved name.';
        return null;
    }
    if (osType === 'macOS') {
        if (bytes > 63) return 'Username is too long (max 63 UTF-8 bytes in AppSandbox).';
        if (/\s/.test(name)) return 'Username cannot contain spaces (macOS short account name).';
        if (/[\x00-\x1f\x7f\ufffe\uffff/\\:]/.test(name)) return 'Username contains invalid characters.';
        if (name === '.' || name === '..') return 'Username cannot be . or .. (macOS short account name).';
        if (['root', 'daemon', 'nobody', 'guest', 'shared'].indexOf(name.toLowerCase()) >= 0)
            return 'Username is a reserved name.';
        return null;
    }
    if (name.length > 20) return 'Username cannot exceed 20 characters.';
    if (/[\x00-\x1f\ufffe\uffff"\\/\[\]:;|=,+*?<>%@]/.test(name)) return 'Username contains invalid characters.';
    if (/^[.\s]+$/.test(name)) return 'Username cannot be only dots or spaces.';
    if (name.endsWith('.')) return 'Username cannot end with a period.';
    var reserved = ['NONE','CON','PRN','AUX','NUL',
        'COM1','COM2','COM3','COM4','COM5','COM6','COM7','COM8','COM9',
        'LPT1','LPT2','LPT3','LPT4','LPT5','LPT6','LPT7','LPT8','LPT9'];
    if (reserved.indexOf(name.toUpperCase()) >= 0) return 'Username is a reserved name.';
    if (!isTemplate &&
        name.toLowerCase() === document.getElementById('vm-name').value.trim().toLowerCase())
        return 'Username cannot match the VM name (Windows computer name).';
    return null;
}

function validatePassword(pass) {
    if (!pass) return 'Choose a password for the VM\u2019s administrator account.';
    if (pass.indexOf('\u0000') >= 0) return 'Password cannot contain NUL characters.';
    var bytes;
    try {
        bytes = unescape(encodeURIComponent(pass)).length;
    } catch (e) {
        return 'Password contains invalid Unicode.';
    }
    var osSelect = document.getElementById('os-type');
    var osType = osSelect ? osSelect.value : 'Windows';
    if (osType === 'Linux') {
        if (Array.from(pass).length < 6)
            return 'Password must be at least 6 characters (Ubuntu minimum).';
        if (bytes > 255) return 'Password is too long (max 255 bytes).';
    } else if (osType === 'macOS') {
        if (Array.from(pass).length < 4)
            return 'Password must be at least 4 characters (macOS minimum).';
        if (bytes > 127) return 'Password is too long (max 127 UTF-8 bytes in AppSandbox).';
    } else if (pass.length > 127) {
        return 'Password is too long (max 127 characters for Windows).';
    }
    return null;
}

/* Windows host: the dialog stays open until the host answers with
   createResult, and shows the reason when the VM was not accepted. The Mac
   host reports problems with an alert, so the dialog closes at once there. */
function submitCreate(isTemplate) {
    if (createPending) return;
    var error = createValidationError(isTemplate);
    if (error) { updateCreateButtons(); return; }
    var cfg = gatherConfig();
    if (isTemplate) cfg.isTemplate = true;
    if (hostBridge.isMac) {
        sendCmd('createVm', cfg);
        clearCreateForm();
        closeCreateModal();
        return;
    }
    createHostError = '';
    createPending = true;
    updateCreateButtons();
    if (!sendCmd('createVm', cfg)) {
        createPending = false;
        createHostError = 'That template is no longer available. Choose another one.';
        updateCreateButtons();
    }
}

function onCreateVm() { submitCreate(false); }
function onCreateTemplate() { submitCreate(true); }

function onCreateResult(msg) {
    if (!createPending) return;
    createPending = false;
    var open = document.getElementById('create-vm-overlay').classList.contains('active');
    if (msg.kind === 'ok') {
        clearCreateForm();
        if (open) closeCreateModal();
        return;
    }
    if (!open) {
        showAlert({ kind: 'error', title: 'Could not create the VM', message: msg.message });
        return;
    }
    createHostError = msg.message || 'The VM could not be created. Details are in Activity.';
    updateCreateButtons();
}

/* ---- Create Sandbox modal ---- */

function openCreateModal() {
    if (coreStarting || prereqState) return;
    closeDiskLocationModal(false);
    createOpener = document.activeElement;
    createHostError = '';
    /* Reset to defaults every time the modal opens */
    document.getElementById('vm-name').value = 'MyAppSandbox';
    document.getElementById('image-path').value = '';
    document.getElementById('disk-directory').value = (lastHostInfo && lastHostInfo.defaultDiskDirectory) || '';
    revalidateDiskDirectory();
    selectTemplate('', templateDefaultLabel());
    document.getElementById('hdd-size').value = 64;
    setGpuSelection('gpu-mode', { gpuMode: 1 });
    document.getElementById('net-mode').value = '1';
    /* A common default name; never a known password (the account is an
       administrator, and reachable over SSH when that is turned on). */
    document.getElementById('admin-user').value = 'user';
    document.getElementById('admin-pass').value = '';
    document.getElementById('admin-confirm').value = '';
    document.getElementById('admin-pass').removeAttribute('data-touched');
    togglePasswordVisibility(false);
    document.getElementById('test-mode').checked = false;
    document.getElementById('ssh-enabled').checked = false;
    document.getElementById('ssh-deploy-key').checked = false;
    onSshToggle();   /* re-grey "Deploy SSH key" to match the cleared SSH checkbox */
    /* Reset OS type to Windows on each open. Valid on both hosts (a Mac host
       supports Windows via QEMU); the user can switch to macOS on a Mac. */
    document.getElementById('os-type').value = 'Windows';

    /* Smart defaults (RAM/cores) from latest host info */
    if (lastHostInfo) applySmartDefaults(lastHostInfo);

    /* Clear validation state */
    document.getElementById('vm-name-warn').textContent = '';
    document.getElementById('admin-user-warn').textContent = '';
    document.getElementById('admin-pass-warn').textContent = '';
    checkPasswordMatch();
    onNetModeChange();
    applyOsTypeUI();   /* fires updateCreateButtons + revalidateVmName */

    document.getElementById('create-vm-overlay').classList.add('active');
    setTimeout(function() {
        if (diskDirectoryBeforeEdit === null) document.getElementById('vm-name').focus();
    }, 0);
}

function closeCreateModal() {
    var overlay = document.getElementById('create-vm-overlay');
    var wasOpen = overlay.classList.contains('active');
    closeDiskLocationModal(false);
    closeTemplateDropdown();
    overlay.classList.remove('active');
    clearTimeout(diskSpaceTimer);
    diskSpaceTimer = null;
    diskSpacePending = false;
    ++diskSpaceRequestId;
    /* Passwords do not stay in the page once the dialog is closed. */
    document.getElementById('admin-pass').value = '';
    document.getElementById('admin-confirm').value = '';
    togglePasswordVisibility(false);
    if (wasOpen) {
        var back = createOpener && createOpener.isConnected && !createOpener.disabled
            ? createOpener : document.getElementById('btn-new-sandbox');
        createOpener = null;
        if (back && !back.disabled) back.focus();
    }
}

/* Close on backdrop click — but only when the press also STARTED on the backdrop.
   A click targets the common ancestor of the mousedown and mouseup, so pressing
   inside the modal (e.g. selecting text in a field) and releasing on the backdrop
   would otherwise close it. */
let createBackdropPress = false;
document.getElementById('create-vm-overlay').addEventListener('mousedown', function(e) {
    createBackdropPress = (e.target === this);
});
document.getElementById('create-vm-overlay').addEventListener('click', function(e) {
    if (e.target === this && createBackdropPress) closeCreateModal();
    createBackdropPress = false;
});

function trapModalFocus(event, overlay) {
    if (event.key !== 'Tab') return;
    var controls = Array.from(overlay.querySelectorAll('button, input, select, textarea, a[href], [tabindex]')).filter(function(el) {
        return !el.disabled && el.tabIndex >= 0 && el.getClientRects().length;
    });
    if (!controls.length) { event.preventDefault(); return; }
    var first = controls[0], last = controls[controls.length - 1];
    if (!overlay.contains(document.activeElement) ||
        (event.shiftKey && document.activeElement === first) ||
        (!event.shiftKey && document.activeElement === last)) {
        event.preventDefault();
        (event.shiftKey ? last : first).focus();
    }
}

/* Open dialogs in stacking order: later in the page is on top. */
function activeOverlays() {
    return Array.prototype.slice.call(document.querySelectorAll('.modal-overlay.active'));
}
function topOverlay() {
    var list = activeOverlays();
    return list.length ? list[list.length - 1] : null;
}

/* While a dialog is open, the page and any dialog under it are inert (no
   focus, no clicks, hidden from screen readers). */
function syncModalInert() {
    var top = topOverlay();
    var app = document.querySelector('.app');
    if (app) app.inert = !!top;
    document.querySelectorAll('.modal-overlay').forEach(function(o) {
        o.inert = o.classList.contains('active') && o !== top;
    });
}
(function() {
    var observer = new MutationObserver(syncModalInert);
    document.querySelectorAll('.modal-overlay').forEach(function(o) {
        observer.observe(o, { attributes: true, attributeFilter: ['class'] });
    });
})();

/* Escape closes the dialog on top, as its Cancel or Close button would. */
function closeOverlay(overlay) {
    switch (overlay.id) {
        case 'modal-overlay': modalResolve(false); break;
        case 'disk-location-overlay': closeDiskLocationModal(false); break;
        case 'edit-vm-overlay': closeEditVmModal(); break;
        case 'snapshot-overlay': closeSnapshotModal(); break;
        case 'create-vm-overlay': closeCreateModal(); break;
        case 'document-overlay': closeDocument(); break;
        case 'prereq-overlay': if (prereqState !== 'enabling') closePrereqDialog(); break;
    }
}

document.addEventListener('keydown', function(e) {
    var top = topOverlay();
    if (!top) return;
    if (e.key === 'Tab') { trapModalFocus(e, top); return; }
    if (e.key === 'Escape') {
        e.preventDefault();
        if (top.id === 'create-vm-overlay' && templateDropdownOpen()) {
            closeTemplateDropdown();
            document.getElementById('template-dropdown-selected').focus();
            return;
        }
        closeOverlay(top);
        return;
    }
    if (e.key === 'Enter') {
        if (top.id === 'disk-location-overlay' && e.target.id === 'disk-directory') {
            e.preventDefault();
            closeDiskLocationModal(true);
        } else if (top.id === 'edit-vm-overlay' && e.target.tagName === 'INPUT') {
            e.preventDefault();
            saveEditVm();
        }
    }
});

/* ---- VM library (cards) ---- */

let vmFilter = 'all';
let detailVmId = null;
let detailTab = 'overview';
let openMenu = null;

const ICONS = {
    monitor: '<svg aria-hidden="true" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="4" width="18" height="12" rx="2"></rect><path d="M8 20h8M12 16v4"></path></svg>',
    windows: '<svg aria-hidden="true" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linejoin="round"><path d="M3 5.5l7.5-1v7H3zM12.5 4.2L21 3v8.5h-8.5zM3 12.5h7.5v7L3 18.5zM12.5 12.5H21V21l-8.5-1.2z"></path></svg>',
    linux: '<svg aria-hidden="true" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M4 17l6-6-6-6M12 19h8"></path></svg>',
    apple: '<svg aria-hidden="true" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M16 3c-1.5.2-3 1.3-3.4 3M12 7.5c-1.4-.9-2.6-1-3.8-.6C5.6 7.8 4.5 11 5.4 14.6 6.3 18.4 8.5 21 10 21c1 0 1.3-.5 2-.5s1 .5 2 .5c1.6 0 3.4-2.6 4.2-5-1.8-.8-2.8-2.5-2.4-4.4.3-1.3 1.2-2.2 2.3-2.7-1.3-1.7-3.6-2.1-5.1-1.4z"></path></svg>',
    edit: '<svg aria-hidden="true" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><path d="M4 6h10M18 6h2M4 12h4M12 12h8M4 18h12"></path><circle cx="16" cy="6" r="2"></circle><circle cx="10" cy="12" r="2"></circle><circle cx="18" cy="18" r="2"></circle></svg>',
    more: '<svg aria-hidden="true" width="18" height="18" viewBox="0 0 24 24" fill="currentColor"><circle cx="5" cy="12" r="1.8"></circle><circle cx="12" cy="12" r="1.8"></circle><circle cx="19" cy="12" r="1.8"></circle></svg>',
    ssh: '<svg aria-hidden="true" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="4" width="18" height="16" rx="2"></rect><path d="M7 9l3 3-3 3M12 15h5"></path></svg>'
};

function osIcon(osType) {
    if (osType === 'Windows') return ICONS.windows;
    if (osType === 'Linux') return ICONS.linux;
    if (osType === 'macOS') return ICONS.apple;
    return ICONS.monitor;
}

function isBusy(vm) { return !!(vm.buildingVhdx || vm.starting || vm.updatingTools || vm.copyingDisk || vm.pendingOp); }

/* Status label + style for a VM (kind: running | busy | stopping | stopped). */
function vmStatus(vm) {
    if (vm.pendingOp === 'deleting') return { label: 'Deleting', kind: 'busy' };
    if (vm.pendingOp === 'stopping') return { label: 'Stopping', kind: 'stopping' };
    if (vm.pendingOp === 'shuttingDown') return { label: 'Shutting down', kind: 'stopping' };
    if (vm.copyingDisk)
        return { label: (vm.installComplete ? 'Exporting ' : 'Importing ') + (vm.copyProgress || 0) + '%', kind: 'busy' };
    if (vm.updatingTools) return { label: 'Updating components', kind: 'busy' };
    if (vm.starting) return { label: 'Starting', kind: 'busy' };
    if (vm.buildingVhdx)
        return { label: vm.vhdxStaging ? 'Staging files' : 'Building disk ' + (vm.vhdxProgress || 0) + '%', kind: 'busy' };
    if (vm.running && vm.shuttingDown) return { label: 'Shutting down', kind: 'stopping' };
    if (vm.running && vm.isTemplate) return { label: 'Building template', kind: 'busy' };
    if (vm.running && !vm.installComplete && !vm.isTemplate) {
        var def = vm.osType === 'macOS' ? 'Installing macOS' : vm.osType === 'Linux' ? 'Installing Linux' : 'Installing Windows';
        return { label: (vm.installStatus && vm.installStatus.length > 0) ? vm.installStatus : def, kind: 'busy' };
    }
    if (vm.running) return { label: 'Running', kind: 'running' };
    return { label: vm.isTemplate ? 'Template' : 'Stopped', kind: 'stopped' };
}

/* Fill a badge element; keeps an existing spinner so its animation runs on. */
function setBadge(el, vm) {
    var st = vmStatus(vm);
    el.className = 'badge' + (st.kind === 'stopped' ? '' : ' ' + st.kind);
    var spinner = el.querySelector('.spinner');
    if (st.kind === 'busy') {
        if (!spinner) { spinner = document.createElement('span'); spinner.className = 'spinner'; }
        el.textContent = '';
        el.appendChild(spinner);
        el.appendChild(document.createTextNode(st.label));
    } else {
        el.textContent = st.label;
    }
}

function gpuLabel(vm) {
    if (hostBridge.isMac && vm.osType === 'Windows') return 'Software rendering';
    return vm.gpuName || (vm.gpuMode === 1 ? 'Default GPU' : 'No GPU');
}

function ramLabel(mb) {
    return mb >= 1024 ? (Math.round(mb / 102.4) / 10) + ' GB' : mb + ' MB';
}

function vmSpecs(vm) {
    return [
        vm.cpuCores + ' vCPU',
        ramLabel(vm.ramMb),
        vm.hddGb + ' GB disk',
        hostBridge.isMac ? 'NAT' : (netNames[vm.networkMode] || 'None') + (vm.networkMode === 0 ? '' : ' network')
    ];
}

function setVmFilter(f) {
    vmFilter = f;
    renderVmTable();
}

function vmMatchesFilter(vm) {
    var q = (document.getElementById('vm-search').value || '').trim().toLowerCase();
    if (q && vm.name.toLowerCase().indexOf(q) < 0 && (vm.osType || '').toLowerCase().indexOf(q) < 0) return false;
    if (vmFilter === 'running') return !!vm.running || isBusy(vm);
    if (vmFilter === 'stopped') return !vm.running && !isBusy(vm);
    return true;
}

function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined && text !== null) e.textContent = text;
    return e;
}

function iconButton(cls, svg, label, handler, disabled) {
    var b = el('button', 'icon-square ' + cls);
    b.type = 'button';
    b.innerHTML = svg;
    b.setAttribute('aria-label', label);
    b.title = label;
    if (disabled) b.disabled = true;
    else b.onclick = function(e) { e.stopPropagation(); handler(e); };
    return b;
}

function sshTitle(vm) {
    if (vm.sshState === 1) return 'Installing OpenSSH in the guest...';
    if (vm.sshState === 4) return 'Open an SSH terminal (localhost:' + vm.sshPort + ', key deployed)';
    if (vm.sshState === 2) return 'Open an SSH terminal (localhost:' + vm.sshPort + ')';
    if (vm.sshState === 3) return 'SSH install failed';
    return 'SSH: waiting for the in-VM agent';
}

function sshReady(vm) {
    return vm.sshEnabled && (vm.sshState === 2 || vm.sshState === 4) && vm.running && !isBusy(vm);
}

/* Main action for a VM: Connect when running, Start when stopped. */
function primaryAction(vm) {
    var bld = isBusy(vm);
    if (vm.running) {
        return { label: 'Connect', disabled: bld || vm.isTemplate,
                 run: function() { sendCmd('connectIddVm', { vmId: vm.id }); } };
    }
    return { label: 'Start', disabled: bld || !!exitState || !!prereqState,
             title: exitState ? 'App Sandbox is shutting its VMs down to exit' : '',
             run: function() { onStartVm(vmIndexById(vm.id)); } };
}

/* The ... menu: secondary and destructive actions. */
function showVmMenu(vm, anchor) {
    closeVmMenu();
    var bld = isBusy(vm);
    var menu = el('div', 'menu-pop');
    menu.setAttribute('role', 'menu');
    menu.setAttribute('aria-label', vm.name);
    function item(label, enabled, fn, cls) {
        var b = el('button', cls || '', label);
        b.type = 'button';
        b.setAttribute('role', 'menuitem');
        if (!enabled) b.disabled = true;
        else b.onclick = function() { closeVmMenu(); fn(); };
        menu.appendChild(b);
    }
    item('Open settings', true, function() { openVmDetail(vm.id); });
    item('Shut down', vm.running && !bld, function() { sendCmd('shutdownVm', { vmId: vm.id }); });
    item('Force stop\u2026', vm.running && !bld, function() { onStopVm(vmIndexById(vm.id)); });
    if (!hostBridge.isMac) {
        item('Snapshots…', !bld, function() { openSnapshotModal(vmIndexById(vm.id)); });
        item('Export…', !vm.running && !bld && !vm.isTemplate, function() { sendCmd('exportVm', { vmId: vm.id }); });
        if (vm.osType === 'Other' && vm.hasIso)
            item('Remove installer ISO', !vm.running && !bld, function() { sendCmd('ejectIso', { vmId: vm.id }); });
        item('Change icon…', !vm.isTemplate, function() { sendCmd('setVmIcon', { vmId: vm.id }); });
        if (vm.iconVersion)
            item('Use default icon', true, function() { sendCmd('clearVmIcon', { vmId: vm.id }); });
        item('Create desktop shortcut', !vm.isTemplate, function() { sendCmd('createShortcut', { vmId: vm.id, where: 'desktop' }); });
        item('Add to Start menu', !vm.isTemplate, function() { sendCmd('createShortcut', { vmId: vm.id, where: 'start' }); });
        item('Open VM folder', !!vm.vmFolder && vm.pendingOp !== 'deleting', function() { sendCmd('openVmFolder', { vmId: vm.id }); });
        item('Copy VM folder path', !!vm.vmFolder, function() { sendCmd('copyVmPath', { vmId: vm.id }); });
    }
    if (vm.sshEnabled)
        item('Open SSH terminal', sshReady(vm), function() { sendCmd('sshConnect', { vmId: vm.id }); });
    menu.appendChild(document.createElement('hr'));
    item('Delete VM…', !bld, function() { onDeleteVm(vmIndexById(vm.id)); }, 'danger-item');

    /* Arrow keys, Home and End move between the enabled items. */
    menu.addEventListener('keydown', function(e) {
        var items = Array.prototype.slice.call(menu.querySelectorAll('button:not(:disabled)'));
        var i = items.indexOf(document.activeElement), next = null;
        if (!items.length) return;
        if (e.key === 'ArrowDown') next = items[(i + 1) % items.length];
        else if (e.key === 'ArrowUp') next = items[(i - 1 + items.length) % items.length];
        else if (e.key === 'Home') next = items[0];
        else if (e.key === 'End') next = items[items.length - 1];
        else if (e.key === 'Tab') { closeVmMenu(); return; }
        if (next) { e.preventDefault(); next.focus(); }
    });

    document.body.appendChild(menu);
    var r = anchor.getBoundingClientRect();
    var left = Math.min(r.right - menu.offsetWidth, window.innerWidth - menu.offsetWidth - 8);
    var top = r.bottom + 6;
    if (top + menu.offsetHeight > window.innerHeight - 8) top = r.top - menu.offsetHeight - 6;
    menu.style.left = Math.max(8, left) + 'px';
    menu.style.top = Math.max(8, top) + 'px';
    openMenu = { menu: menu, anchor: anchor };
    var first = menu.querySelector('button:not(:disabled)');
    if (first) first.focus();
}

function closeVmMenu() {
    if (!openMenu) return;
    var anchor = openMenu.anchor;
    if (openMenu.menu.parentNode) openMenu.menu.parentNode.removeChild(openMenu.menu);
    openMenu = null;
    if (anchor && anchor.isConnected && document.activeElement === document.body) anchor.focus();
}

document.addEventListener('mousedown', function(e) {
    if (openMenu && !openMenu.menu.contains(e.target) && e.target !== openMenu.anchor) closeVmMenu();
});
document.addEventListener('keydown', function(e) {
    if (openMenu && e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); closeVmMenu(); }
}, true);
window.addEventListener('resize', closeVmMenu);

function buildVmCard(vm) {
    var card = el('article', 'vm-card' + (vm.running ? ' running' : ''));

    var main = el('div', 'vm-card-main');
    main.setAttribute('role', 'button');
    main.tabIndex = 0;
    main.setAttribute('aria-label', 'Open ' + vm.name + ' settings');
    main.onclick = function() { openVmDetail(vm.id); };
    main.onkeydown = function(e) {
        if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); openVmDetail(vm.id); }
    };

    var top = el('div', 'vm-card-top');
    var tile = el('div', 'os-tile');
    tile.innerHTML = vmTileHtml(vm);
    if (vmIcons[vm.id] && vmIcons[vm.id].data) tile.classList.add('has-icon');
    var names = el('div', 'vm-card-names');
    names.appendChild(el('h2', '', vm.name));
    names.appendChild(el('span', '', vm.osType + ' · ' + gpuLabel(vm)));
    var badge = el('span', 'badge');
    badge.setAttribute('data-role', 'status');
    setBadge(badge, vm);
    top.appendChild(tile);
    top.appendChild(names);
    top.appendChild(badge);
    main.appendChild(top);

    var specs = el('ul', 'spec-list');
    vmSpecs(vm).forEach(function(s) { specs.appendChild(el('li', '', s)); });
    if (!hostBridge.isMac && vm.hasSnapshots)
        specs.appendChild(el('li', '', 'Disk: ' + snapshotPath(vm, selectedSnap.get(vm.id) || 'current')));
    main.appendChild(specs);

    var prog = el('div', 'vm-card-progress');
    prog.setAttribute('data-role', 'progress');
    prog.appendChild(el('div'));
    main.appendChild(prog);
    updateCardProgress(card, vm, prog);

    card.appendChild(main);

    if (vm.lastError) {
        var err = el('div', 'vm-card-error');
        err.setAttribute('role', 'alert');
        err.appendChild(el('span', '', vm.lastError));
        var x = el('button', 'notice-dismiss', '\u00d7');
        x.type = 'button';
        x.setAttribute('aria-label', 'Dismiss this message');
        x.onclick = function(e) { e.stopPropagation(); dismissVmError(vm.id); };
        err.appendChild(x);
        card.appendChild(err);
    }

    var actions = el('div', 'vm-card-actions');
    var pa = primaryAction(vm);
    var pb = el('button', 'primary grow', pa.label);
    pb.type = 'button';
    if (pa.title) pb.title = pa.title;
    if (pa.disabled) pb.disabled = true;
    else pb.onclick = pa.run;
    actions.appendChild(pb);
    if (vm.sshEnabled)
        actions.appendChild(iconButton('ssh', ICONS.ssh, sshTitle(vm),
            function() { sendCmd('sshConnect', { vmId: vm.id }); }, !sshReady(vm)));
    actions.appendChild(iconButton('edit', ICONS.edit, 'Edit hardware (VM must be stopped)',
        function() { openEditVmModal(vmIndexById(vm.id)); }, vm.running || isBusy(vm)));
    var more = iconButton('more-btn', ICONS.more, 'More actions', function(e) {
        if (openMenu && openMenu.anchor === more) closeVmMenu();
        else showVmMenu(vm, more);
    }, false);
    more.setAttribute('aria-haspopup', 'menu');
    actions.appendChild(more);
    card.appendChild(actions);
    return card;
}

function updateCardProgress(card, vm, prog) {
    prog = prog || card.querySelector('[data-role="progress"]');
    if (!prog) return;
    var copying = !!vm.copyingDisk;
    var show = (vm.buildingVhdx && !vm.vhdxStaging) || copying;
    prog.hidden = !show;
    if (show) prog.firstChild.style.width =
        Math.max(2, Math.min(100, (copying ? vm.copyProgress : vm.vhdxProgress) || 0)) + '%';
}

/* Name kept from the table UI: every state change calls it. Rebuilds a card
 * only when a field that changes its buttons changes, so install-progress
 * ticks never destroy a button mid-click. */
function renderVmTable() {
    updateEditVmModal();
    renderSnapshotModal();

    var grid = document.getElementById('vm-grid');
    var running = vms.filter(function(v) { return v.running; }).length;
    document.getElementById('nav-vm-count').textContent = vms.length;
    document.getElementById('vm-summary').textContent = coreStarting && !vms.length
        ? 'Starting…'
        : prereqState === 'reboot' && !vms.length ? 'Restart Windows to finish setup'
        : prereqState && !vms.length ? 'Virtual Machine Platform is off'
        : vms.length
            ? running + ' running · ' + (vms.length - running) + ' stopped'
            : 'No virtual machines yet';
    document.querySelectorAll('#vm-filters .chip').forEach(function(c) {
        var on = c.getAttribute('data-filter') === vmFilter;
        c.classList.toggle('active', on);
        c.setAttribute('aria-pressed', on ? 'true' : 'false');
    });

    var seen = {};
    vms.forEach(function(vm) { seen[vm.id] = true; });
    Object.keys(rowCache).forEach(function(id) {
        if (!seen[id]) {
            var stale = rowCache[id];
            if (stale.parentNode) stale.parentNode.removeChild(stale);
            delete rowCache[id];
            delete rowSigCache[id];
        }
    });
    Array.prototype.slice.call(grid.children).forEach(function(c) {
        if (!c.classList.contains('vm-card')) grid.removeChild(c);
    });

    if (vms.length === 0 && coreStarting) {
        var loading = el('div', 'empty-state loading-state');
        loading.setAttribute('role', 'status');
        loading.appendChild(el('span', 'spinner'));
        loading.appendChild(el('h2', '', 'Starting App Sandbox…'));
        loading.appendChild(el('p', 'muted', 'Checking Windows features and your GPUs, and finding your virtual machines.'));
        grid.appendChild(loading);
    } else if (vms.length === 0 && prereqState) {
        grid.appendChild(buildPrereqState());
    } else if (vms.length === 0) {
        var empty = el('div', 'empty-state');
        empty.appendChild(el('h2', '', 'Create your first virtual machine'));
        empty.appendChild(el('p', 'muted', hostBridge.isMac
            ? 'Windows 11 or macOS, set up automatically from an installer image.'
            : 'Windows 11 or Ubuntu, set up automatically from an installer image, or any other OS you install from its ISO.'));
        var btn = el('button', 'primary with-icon', 'New VM');
        btn.type = 'button';
        btn.onclick = openCreateModal;
        empty.appendChild(btn);
        grid.appendChild(empty);
    }

    var pos = 0, shown = 0;
    vms.forEach(function(vm, i) {
        var card = rowCache[vm.id];
        var sig = [
            i === selectedVm, vm.name, vm.running, vm.starting, vm.buildingVhdx, vm.vhdxStaging, vm.updatingTools,
            vm.copyingDisk, vm.pendingOp || '', vm.lastError || '', !!exitState, !!prereqState, !!vm.vmFolder,
            vm.shuttingDown, vm.installComplete, vm.isTemplate,
            vm.sshEnabled, vm.sshState, vm.sshPort, vm.osType, vm.ramMb, vm.hddGb, vm.cpuCores,
            vm.gpuMode, vm.gpuId, vm.gpuName, vm.networkMode, vm.hasSnapshots,
            vm.iconVersion || 0, (vmIcons[vm.id] && vmIcons[vm.id].data ? vmIcons[vm.id].data.length : 0),
            selectedSnap.get(vm.id) || 'current', vm.snapCurrent, vm.snapCurrentBranch,
            JSON.stringify(vm.snapshots || []), JSON.stringify(vm.baseBranches || [])
        ].join('|');

        if (!card || rowSigCache[vm.id] !== sig) {
            var fresh = buildVmCard(vm);
            if (card && card.parentNode) card.parentNode.replaceChild(fresh, card);
            card = fresh;
            rowCache[vm.id] = card;
            rowSigCache[vm.id] = sig;
        } else {
            var badge = card.querySelector('[data-role="status"]');
            if (badge) setBadge(badge, vm);
            updateCardProgress(card, vm);
        }
        card.classList.toggle('selected', i === selectedVm);

        var visible = vmMatchesFilter(vm);
        card.hidden = !visible;
        if (visible) shown++;
        if (grid.children[pos] !== card) grid.insertBefore(card, grid.children[pos] || null);
        pos++;
    });
    if (vms.length && !shown) {
        var none = el('div', 'empty-state');
        var q = (document.getElementById('vm-search').value || '').trim();
        none.appendChild(el('h2', '', q ? 'No VMs match \u201c' + q + '\u201d' : 'No VMs here'));
        var clear = el('button', '', 'Show all VMs');
        clear.type = 'button';
        clear.onclick = function() { document.getElementById('vm-search').value = ''; setVmFilter('all'); };
        none.appendChild(clear);
        grid.appendChild(none);
    }

    renderVmDetail();
    renderSettingsView();
}

/* ---- Views ---- */

function showView(name) {
    closeVmMenu();
    if (name !== 'vm') detailVmId = null;
    ['vms', 'vm', 'settings'].forEach(function(v) {
        document.getElementById('view-' + v).hidden = v !== name;
    });
    document.querySelectorAll('.nav-item').forEach(function(n) {
        var target = n.getAttribute('data-view');
        var on = target === name || (name === 'vm' && target === 'vms');
        n.classList.toggle('active', on);
        if (on) n.setAttribute('aria-current', 'page'); else n.removeAttribute('aria-current');
    });
    if (name === 'settings') renderSettingsView();
}

function openVmDetail(id) {
    var idx = vmIndexById(id);
    if (idx < 0) return;
    detailVmId = id;
    selectedVm = idx;
    sendCmd('selectVm', { vmId: id });
    showView('vm');
    detailVmId = id;
    renderVmDetail();
}

function showVmTab(tab) {
    detailTab = tab;
    renderVmDetail();
}

function detailRow(label, value) {
    var row = el('div', 'row');
    var text = el('div', 'row-text');
    text.appendChild(el('span', 'row-label', label));
    row.appendChild(text);
    row.appendChild(el('span', 'row-value', value));
    return row;
}

function fillRows(container, rows) {
    container.textContent = '';
    rows.forEach(function(r) { container.appendChild(detailRow(r[0], r[1])); });
}

function displaySupported(vm) {
    return !hostBridge.isMac && (vm.osType === 'Windows' || vm.osType === 'Linux') && !vm.isTemplate;
}

function renderVmDetail() {
    if (!detailVmId) return;
    var vm = vms[vmIndexById(detailVmId)];
    if (!vm) { showView('vms'); return; }

    document.getElementById('vmd-name').textContent = vm.name;
    setBadge(document.getElementById('vmd-status'), vm);
    document.getElementById('vmd-sub').textContent = vm.osType + ' · ' + gpuLabel(vm) +
        (vm.running && vm.agentOnline ? ' · agent connected' : '');

    /* Header actions: rebuilt only when they would change. */
    var actions = document.getElementById('vmd-actions');
    var bld = isBusy(vm);
    var sig = [vm.id, vm.running, bld, vm.isTemplate, vm.sshEnabled, sshReady(vm), !!exitState, !!prereqState].join('|');
    renderToolsCard(vm);
    if (actions.getAttribute('data-sig') !== sig) {
        actions.setAttribute('data-sig', sig);
        actions.textContent = '';
        var pa = primaryAction(vm);
        var pb = el('button', 'primary', pa.label);
        pb.type = 'button';
        if (pa.title) pb.title = pa.title;
        if (pa.disabled) pb.disabled = true; else pb.onclick = pa.run;
        actions.appendChild(pb);
        if (vm.sshEnabled) {
            var sb = el('button', '', 'SSH');
            sb.type = 'button';
            sb.title = sshTitle(vm);
            if (!sshReady(vm)) sb.disabled = true;
            else sb.onclick = function() { sendCmd('sshConnect', { vmId: vm.id }); };
            actions.appendChild(sb);
        }
        var sd = el('button', '', 'Shut down');
        sd.type = 'button';
        if (!vm.running || bld) sd.disabled = true;
        else sd.onclick = function() { sendCmd('shutdownVm', { vmId: vm.id }); };
        actions.appendChild(sd);
        var fs = el('button', 'danger-outline', 'Force stop\u2026');
        fs.type = 'button';
        if (!vm.running || bld) fs.disabled = true;
        else fs.onclick = function() { onStopVm(vmIndexById(vm.id)); };
        actions.appendChild(fs);
        var del = el('button', 'danger-outline', 'Delete\u2026');
        del.type = 'button';
        if (bld) del.disabled = true;
        else del.onclick = function() { onDeleteVm(vmIndexById(vm.id)); };
        actions.appendChild(del);
    }

    /* Tabs */
    var showDisplay = displaySupported(vm);
    var showSnaps = !hostBridge.isMac && !vm.isTemplate;
    if ((detailTab === 'display' && !showDisplay) || (detailTab === 'snapshots' && !showSnaps)) detailTab = 'overview';
    document.querySelectorAll('#vmd-tabs .tab').forEach(function(t) {
        var name = t.getAttribute('data-tab');
        if (name === 'display') t.hidden = !showDisplay;
        if (name === 'snapshots') t.hidden = !showSnaps;
        var on = name === detailTab;
        t.classList.toggle('active', on);
        if (on) t.setAttribute('aria-current', 'page'); else t.removeAttribute('aria-current');
    });
    document.querySelectorAll('#view-vm .tab-panel').forEach(function(p) {
        p.hidden = p.getAttribute('data-panel') !== detailTab;
    });

    var errBox = document.getElementById('vmd-error');
    errBox.hidden = !vm.lastError;
    document.getElementById('vmd-error-text').textContent = vm.lastError || '';

    var overview = [
        ['Status', vmStatus(vm).label],
        ['Operating system', vm.osType + (vm.isTemplate ? ' (template)' : '')],
        ['In-VM agent', vm.osType === 'Other' ? 'Not used (manual install)'
            : !vm.running ? 'VM is off' : vm.agentOnline ? 'Connected' : 'Not connected'],
        ['Network', hostBridge.isMac ? 'NAT' : (netNames[vm.networkMode] || 'None') + (vm.networkMode === 2 && vm.netAdapter ? ' · ' + vm.netAdapter : '')]
    ];
    /* NAT: the address the VM gets (a manual-install VM needs it set by hand;
       the gateway is the .1 of its /24). */
    if (vm.natIp) {
        var gw = vm.natIp.replace(/\.\d+$/, '.1');
        overview.push(['IP address', vm.natIp + '/24 · gateway ' + gw]);
    }
    if (vm.sshEnabled) overview.push(['SSH', 'localhost:' + vm.sshPort]);
    if (vm.diskDirectory) overview.push(['Disk folder', vm.diskDirectory]);
    fillRows(document.getElementById('vmd-overview'), overview);
    var files = document.getElementById('vmd-files-card');
    files.hidden = hostBridge.isMac || !vm.vmFolder;
    document.getElementById('vmd-folder').textContent = vm.vmFolder || '';

    fillRows(document.getElementById('vmd-hardware'), [
        ['Processors', vm.cpuCores + ' vCPU'],
        ['Memory', ramLabel(vm.ramMb) + (vm.ramMb >= 1024 ? ' (' + vm.ramMb + ' MB)' : '')],
        ['Disk', vm.hddGb + ' GB'],
        ['GPU', gpuLabel(vm)],
        ['Network', hostBridge.isMac ? 'NAT' : (netNames[vm.networkMode] || 'None')]
    ]);
    var editHw = document.getElementById('vmd-edit-hw');
    editHw.disabled = !!(vm.running || bld);
    editHw.title = editHw.disabled ? 'Stop the VM to change its hardware' : '';

    if (showDisplay) renderDisplayTab(vm);

    if (showSnaps) {
        var snaps = (vm.snapshots || []).filter(function(s) { return s && s.name; }).length;
        fillRows(document.getElementById('vmd-snapshots'), [
            ['Boots from', vm.hasSnapshots ? snapshotPath(vm, selectedSnap.get(vm.id) || 'current') : 'Current disk'],
            ['Snapshots', String(snaps)]
        ]);
        document.getElementById('vmd-open-snaps').disabled = bld;
    }
}

/* ---- Guest components ---- */

function renderToolsCard(vm) {
    var card = document.getElementById('vmd-tools-card');
    var applies = !hostBridge.isMac && vm.osType === 'Windows' && !vm.isTemplate;
    card.hidden = !applies;
    if (!applies) return;
    var auto = !lastHostInfo || lastHostInfo.autoUpdateGuestTools !== false;
    var text;
    if (vm.updatingTools) text = 'Copying this version\u2019s agent, helpers and display driver into the VM\u2019s disk\u2026';
    else if (!vm.installComplete) text = 'Installed with the VM. Available to update once Windows setup has finished.';
    else if (vm.guestToolsOutdated)
        text = 'This version of App Sandbox has newer components than this VM (agent, helpers, display driver). ' +
            (auto ? 'They are copied in automatically the next time the VM starts, or now: ' : 'Update them now: ') +
            'the VM must be stopped. The display driver installs during the next start.';
    else text = 'Up to date with this version of App Sandbox.';
    document.getElementById('vmd-tools-text').textContent = text;
    var btn = document.getElementById('vmd-tools-btn');
    btn.disabled = !!(vm.running || isBusy(vm) || !vm.installComplete);
    btn.className = vm.guestToolsOutdated ? 'primary' : '';
    btn.title = vm.running ? 'Shut the VM down first' : '';
}

function updateGuestTools(id) {
    var vm = vms[vmIndexById(id)];
    if (!vm || vm.running || isBusy(vm)) return;
    showModal('Update guest components',
        'App Sandbox will open the disk of "' + vm.name + '" and copy in this version\u2019s agent, helpers and display driver. ' +
        'The display driver installs during the next start (the VM screen may blink once).\n\n' +
        'Tip: take a snapshot first if you want a way back.', 'Update', { confirmClass: 'primary' })
        .then(function(ok) {
            if (ok) sendCmd('updateGuestTools', { vmId: id });
        });
}

function toggleAutoTools() {
    var on = !(lastHostInfo && lastHostInfo.autoUpdateGuestTools === false);
    sendCmd('setAutoUpdateGuestTools', { enabled: !on });
    if (lastHostInfo) lastHostInfo.autoUpdateGuestTools = !on;
    renderSettingsView();
}

/* ---- Display tab ---- */

const DISPLAY_PRESETS = [
    ['1280x720', 'HD'], ['1600x900', 'HD+'], ['1920x1080', 'Full HD'], ['1920x1200', '16:10'],
    ['2560x1440', 'QHD'], ['3440x1440', 'Ultrawide'], ['3840x2160', '4K']
];
const DISPLAY_RATES = [60, 120, 144];
let lastFixedResolution = {};

function renderDisplayTab(vm) {
    var res = vm.displayResolution || '1920x1080';
    var hz = vm.displayHz || 60;
    var adaptive = res === 'adaptive';

    var note = document.getElementById('vmd-display-note');
    var drv = typeof vm.displayDriver === 'number' ? vm.displayDriver : -1;
    var auto = !lastHostInfo || lastHostInfo.autoUpdateGuestTools !== false;
    var linux = vm.osType === 'Linux';
    document.getElementById('vmd-display-update').hidden = linux || !(drv === 0 && !vm.running && !isBusy(vm));
    if (drv === 0 && linux) {
        note.className = 'notice warn';
        note.innerHTML = '<strong>This Ubuntu VM has the older display components,</strong> so it stays at 1920 × 1080 at 60 Hz. ' +
            'Ubuntu VMs build their components when they are created: create a new Ubuntu VM with this version to change resolution and refresh rate.';
    } else if (drv === 0) {
        note.className = 'notice warn';
        note.innerHTML = '<strong>This VM has the older display driver,</strong> so it stays at 1920 × 1080 at 60 Hz. ' +
            (vm.running
                ? (auto ? 'Shut the VM down and start it again: App Sandbox updates its components before it starts.'
                        : 'Shut the VM down, then use Update guest components.')
                : 'Update its guest components (below), then start the VM.');
    } else if (drv >= 2) {
        note.className = 'notice ok';
        note.textContent = 'The display driver in this VM supports these settings. Changes apply in a few seconds.';
    } else {
        note.className = 'notice';
        note.textContent = vm.running
            ? 'Open the VM window to apply changes and check the VM\u2019s display driver.'
            : 'Saved now; applied when the VM window opens.';
    }
    if (!adaptive) lastFixedResolution[vm.id] = res;

    document.querySelectorAll('#view-vm [data-mode]').forEach(function(b) {
        var on = (b.getAttribute('data-mode') === 'adaptive') === adaptive;
        b.classList.toggle('active', on);
        b.setAttribute('aria-pressed', on ? 'true' : 'false');
    });

    var grid = document.getElementById('vmd-presets');
    grid.hidden = adaptive;
    var psig = res;
    if (grid.getAttribute('data-sig') !== psig) {
        grid.setAttribute('data-sig', psig);
        grid.textContent = '';
        DISPLAY_PRESETS.forEach(function(p) {
            var b = el('button', 'preset' + (p[0] === res ? ' active' : ''));
            b.type = 'button';
            b.setAttribute('aria-pressed', p[0] === res ? 'true' : 'false');
            b.appendChild(el('span', '', p[0].replace('x', ' × ')));
            b.appendChild(el('span', 'preset-note', p[1]));
            b.onclick = function() { setDisplayChoice('fixed', p[0]); };
            grid.appendChild(b);
        });
    }

    var perf = !!vm.displayPerformance;
    document.getElementById('vmd-stream-card').hidden = linux;
    document.querySelectorAll('#view-vm [data-stream]').forEach(function(b) {
        var on = (b.getAttribute('data-stream') === 'performance') === perf;
        b.classList.toggle('active', on);
        b.setAttribute('aria-pressed', on ? 'true' : 'false');
    });
    var snote = document.getElementById('vmd-stream-note');
    var video = typeof vm.displayVideo === 'number' ? vm.displayVideo : -1;
    if (perf && video === 1) {
        snote.textContent = 'Active: the VM\u2019s GPU is encoding the picture' + (vm.displayVideoCodec ? ' (' + vm.displayVideoCodec + ').' : '.');
    } else if (perf && drv >= 0 && drv < 3) {
        snote.textContent = 'Needs the updated display driver: shut the VM down and update its guest components.';
    } else if (perf && video === 2) {
        snote.textContent = 'Not active: the VM\u2019s GPU has no hardware video encoder (GPU partitioning off?) or this PC cannot decode H.264. Showing exact pixels instead; the VM window\u2019s log has details.';
    } else if (perf && video === 0) {
        snote.textContent = 'Starting\u2026';
    } else if (perf) {
        snote.textContent = 'Applied when the VM window is open. Needs GPU partitioning so the VM has a hardware video encoder.';
    } else {
        snote.textContent = '';
    }

    document.getElementById('vmd-stream-options').hidden = !perf;
    var codec = vm.displayCodec || 'auto';
    document.querySelectorAll('#view-vm [data-codec]').forEach(function(b) {
        var on = b.getAttribute('data-codec') === codec;
        b.classList.toggle('active', on);
        b.setAttribute('aria-pressed', on ? 'true' : 'false');
    });
    var br = document.getElementById('vmd-bitrate');
    var brv = String(vm.displayBitrate || 0);
    if (!br.querySelector('option[value="' + brv + '"]')) {
        var o = document.createElement('option');
        o.value = brv; o.textContent = brv + ' Mbit/s';
        br.appendChild(o);
    }
    if (document.activeElement !== br) br.value = brv;
    var cnote = document.getElementById('vmd-codec-note');
    var hevcHost = !lastHostInfo || lastHostInfo.hevcDecoder !== false;
    var inUse = vm.displayVideoCodec ? 'In use: ' + vm.displayVideoCodec + '. ' : '';
    if (codec !== 'h264' && !hevcHost)
        cnote.textContent = inUse + 'HEVC needs \u201cHEVC Video Extensions\u201d from the Microsoft Store on this PC; until then H.264 is used.';
    else if (codec === 'auto')
        cnote.textContent = inUse + 'Auto uses HEVC (sharper at the same bit rate) when this PC and the VM\u2019s GPU support it, otherwise H.264.';
    else
        cnote.textContent = inUse + 'More bit rate gives a sharper picture in fast motion; Moonlight users often pick 80\u2013150 Mbit/s at 4K.';

    var rates = document.getElementById('vmd-rates');
    if (rates.getAttribute('data-sig') !== String(hz)) {
        rates.setAttribute('data-sig', String(hz));
        rates.textContent = '';
        DISPLAY_RATES.forEach(function(r) {
            var b = el('button', r === hz ? 'active' : '', r + ' Hz');
            b.type = 'button';
            b.setAttribute('aria-pressed', r === hz ? 'true' : 'false');
            b.onclick = function() { setDisplayRate(r); };
            rates.appendChild(b);
        });
    }
}

function setDisplayChoice(mode, preset) {
    var vm = vms[vmIndexById(detailVmId)];
    if (!vm) return;
    var res = mode === 'adaptive' ? 'adaptive'
        : (preset || lastFixedResolution[vm.id] || '1920x1080');
    sendDisplayMode(vm, res, vm.displayHz || 60);
}

function setDisplayRate(hz) {
    var vm = vms[vmIndexById(detailVmId)];
    if (!vm) return;
    sendDisplayMode(vm, vm.displayResolution || '1920x1080', hz);
}

function sendDisplayMode(vm, res, hz) {
    vm.displayResolution = res;   /* optimistic; the host echoes the saved values */
    vm.displayHz = hz;
    renderVmDetail();
    sendCmd('setDisplayMode', { vmId: vm.id, resolution: res, hz: hz });
}

function setVideoCodec(codec) {
    var vm = vms[vmIndexById(detailVmId)];
    if (!vm) return;
    vm.displayCodec = codec;
    renderVmDetail();
    sendCmd('setVideoCodec', { vmId: vm.id, codec: codec });
}

function setVideoBitrate(value) {
    var vm = vms[vmIndexById(detailVmId)];
    if (!vm) return;
    vm.displayBitrate = parseInt(value, 10) || 0;
    sendCmd('setVideoBitrate', { vmId: vm.id, mbps: vm.displayBitrate });
}

function toggleViewerPref(which) {
    var info = lastHostInfo || {};
    var title = info.viewerTitleStats !== false, toolbar = info.viewerToolbar !== false;
    if (which === 'titleStats') title = !title; else toolbar = !toolbar;
    sendCmd('setViewerPrefs', { titleStats: title, toolbar: toolbar });
    if (lastHostInfo) { lastHostInfo.viewerTitleStats = title; lastHostInfo.viewerToolbar = toolbar; }
    renderSettingsView();
}

function setRenderer(r) {
    var info = lastHostInfo || {};
    sendCmd('setViewerPrefs', { titleStats: info.viewerTitleStats !== false,
                                toolbar: info.viewerToolbar !== false, renderer: r });
    if (lastHostInfo) lastHostInfo.viewerRenderer = r;
    renderSettingsView();
}

function setPerformanceMode(enabled) {
    var vm = vms[vmIndexById(detailVmId)];
    if (!vm) return;
    vm.displayPerformance = enabled;   /* optimistic */
    renderVmDetail();
    sendCmd('setPerformanceMode', { vmId: vm.id, enabled: enabled });
}

/* ---- Settings ---- */

const ACCENTS = [['Blue', '#4EA6F5'], ['Indigo', '#7C8CF8'], ['Green', '#3CCB9A'], ['Orange', '#F2A65A']];

function storedPref(key, fallback) {
    try { return localStorage.getItem(key) || fallback; } catch (e) { return fallback; }
}
function storePref(key, value) {
    try { localStorage.setItem(key, value); } catch (e) {}
}

function setTheme(theme) {
    storePref('ui_theme', theme);
    if (theme === 'system') document.documentElement.removeAttribute('data-theme');
    else document.documentElement.setAttribute('data-theme', theme);
    renderSettingsView();
}

function setAccent(hex) {
    storePref('ui_accent', hex);
    document.documentElement.style.setProperty('--accent', hex);
    renderSettingsView();
}

function setCloseBehavior(value) {
    sendCmd('setCloseBehavior', { toTray: value === 'tray' });
}

function renderSettingsView() {
    if (document.getElementById('view-settings').hidden) return;
    var theme = storedPref('ui_theme', 'dark');
    document.querySelectorAll('[data-theme-choice]').forEach(function(b) {
        var on = b.getAttribute('data-theme-choice') === theme;
        b.classList.toggle('active', on);
        b.setAttribute('aria-pressed', on ? 'true' : 'false');
    });

    var accent = storedPref('ui_accent', ACCENTS[0][1]);
    var sw = document.getElementById('set-accent');
    if (sw.getAttribute('data-sig') !== accent) {
        sw.setAttribute('data-sig', accent);
        sw.textContent = '';
        ACCENTS.forEach(function(a) {
            var b = el('button', 'swatch' + (a[1].toLowerCase() === accent.toLowerCase() ? ' active' : ''));
            b.type = 'button';
            b.style.background = a[1];
            b.setAttribute('aria-label', a[0]);
            b.setAttribute('aria-pressed', a[1].toLowerCase() === accent.toLowerCase() ? 'true' : 'false');
            b.onclick = function() { setAccent(a[1]); };
            sw.appendChild(b);
        });
    }

    if (lastHostInfo && !hostBridge.isMac)
        document.getElementById('set-close').value = lastHostInfo.closeToTrayWithoutAsking ? 'tray' : 'ask';
    document.getElementById('set-auto-tools').setAttribute('aria-checked',
        lastHostInfo && lastHostInfo.autoUpdateGuestTools === false ? 'false' : 'true');
    document.getElementById('set-title-stats').setAttribute('aria-checked',
        lastHostInfo && lastHostInfo.viewerTitleStats === false ? 'false' : 'true');
    document.getElementById('set-toolbar').setAttribute('aria-checked',
        lastHostInfo && lastHostInfo.viewerToolbar === false ? 'false' : 'true');
    var renderer = (lastHostInfo && lastHostInfo.viewerRenderer === 12) ? '12' : '11';
    document.querySelectorAll('#set-renderer [data-renderer]').forEach(function(b) {
        var on = b.getAttribute('data-renderer') === renderer;
        b.classList.toggle('active', on);
        b.setAttribute('aria-pressed', on ? 'true' : 'false');
    });

    var rows = [];
    if (lastHostInfo) {
        rows.push(['Processors', lastHostInfo.hostCores + ' cores']);
        rows.push(['Memory', ramLabel(lastHostInfo.hostRamMb)]);
        if (Array.isArray(lastHostInfo.gpus) && lastHostInfo.gpus.length)
            rows.push(['GPUs', lastHostInfo.gpus.map(function(g) { return g.name; }).join(', ')]);
        /* Windows host: the folder itself is under Folders and logs. */
        if (hostBridge.isMac && lastHostInfo.defaultDiskDirectory) rows.push(['Default VM folder', lastHostInfo.defaultDiskDirectory]);
        if (lastHostInfo.freeGb >= 0) rows.push(['Free space in the default VM folder', lastHostInfo.freeGb + ' GB']);
    }
    fillRows(document.getElementById('set-host'), rows);
    if (!hostBridge.isMac) renderFoldersAndAbout(lastHostInfo);
}

/* "0.1.9.123" -> "0.1.9 (build 123)". */
function formatVersion(v) {
    var parts = String(v || '').split('.');
    if (parts.length === 4 && parts[3] !== '0') return parts.slice(0, 3).join('.') + ' (build ' + parts[3] + ')';
    if (parts.length === 4) return parts.slice(0, 3).join('.');
    return v || 'unknown';
}

function setText(id, text) {
    var e = document.getElementById(id);
    if (e) e.textContent = text;
}

/* Settings > Folders and logs, and About (Windows host). */
function renderFoldersAndAbout(info) {
    if (!info) return;
    var vmDir = info.defaultDiskDirectory || '', dataDir = info.dataDirectory || '';
    var same = !!vmDir && vmDir.replace(/\\+$/, '').toLowerCase() === dataDir.replace(/\\+$/, '').toLowerCase();
    setText('set-folder-vms-path', vmDir);
    setText('set-folder-vms-label', same ? 'VM and app data folder' : 'Default VM folder');
    setText('set-folder-vms-help', same
        ? 'New VMs are created here unless you choose another disk location in New VM. It also holds the settings (vms.cfg), the local API\u2019s token, headless.log and caches.'
        : 'New VMs are created here unless you choose another disk location in New VM.');
    document.getElementById('set-folder-data').hidden = same;
    setText('set-folder-data-path', dataDir);
    setText('set-folder-log-path', info.logFile || '');
    setText('set-folder-app-path', info.packaged ? '' : (info.appDirectory || ''));
    setText('set-folder-app-help', info.packaged
        ? 'Installed from the Microsoft Store: Windows keeps the app\u2019s folder private.'
        : 'Where AppSandbox.exe and the guest components are.');
    document.getElementById('set-folder-app-btn').disabled = !!info.packaged;

    setText('about-version', 'Version ' + formatVersion(info.appVersion) +
        (info.packaged ? ' \u00b7 Microsoft Store' : ''));
    var project = info.projectRepo || '', upstream = info.upstreamRepo || '';
    setText('about-license', project && upstream && project.toLowerCase() !== upstream.toLowerCase()
        ? 'MIT License. ' + project + ' is a fork of ' + upstream + ' by James Stringer.'
        : 'MIT License, by James Stringer and contributors.');
    setText('about-project', project ? 'github.com/' + project : 'Source code, releases and documentation.');
}

/* Sidebar resource meters. */
function updateHostMeters(info) {
    if (!info) return;
    var cpuPct = info.hostCores ? Math.min(100, Math.round(100 * info.vmCores / info.hostCores)) : 0;
    var ramPct = info.hostRamMb ? Math.min(100, Math.round(100 * info.vmRamMb / info.hostRamMb)) : 0;
    document.getElementById('res-cpu').textContent = info.vmCores + ' / ' + info.hostCores + ' cores';
    document.getElementById('res-cpu-bar').style.width = cpuPct + '%';
    document.getElementById('res-ram').textContent = ramLabel(info.vmRamMb) + ' / ' + ramLabel(info.hostRamMb);
    document.getElementById('res-ram-bar').style.width = ramPct + '%';
    document.getElementById('res-disk').textContent = info.freeGb >= 0 ? info.freeGb + ' GB' : '–';
    var gpus = Array.isArray(info.gpus) ? info.gpus : [];
    var g = document.getElementById('res-gpu');
    g.textContent = gpus.length ? gpus[0].name + (gpus.length > 1 ? ' +' + (gpus.length - 1) : '') : 'None';
    g.title = gpus.map(function(x) { return x.name; }).join('\n');
}

/* ---- VM Selection ---- */

function selectVm(idx) {
    if (!vms[idx]) return;
    selectedVm = idx;
    renderVmTable();
    sendCmd('selectVm', { vmId: vms[idx].id });
}

function updateVmList(next) {
    var selectedId = vms[selectedVm] && vms[selectedVm].id;
    selectedSnap.forEach(function(value, id) {
        var before = vms.find(function(vm) { return vm.id === id; });
        var after = next.find(function(vm) { return vm.id === id; });
        if (!before || !after || snapshotTreeSignature(before) !== snapshotTreeSignature(after))
            selectedSnap.delete(id);
    });
    vms = next;
    selectedVm = selectedId ? vmIndexById(selectedId) : -1;
    requestVmIcons();
}

/* ---- Custom VM icons (icon.png / icon.ico beside the VM's disk) ---- */
let vmIcons = {};   /* id -> { version, data } */

function requestVmIcons() {
    vms.forEach(function(vm) {
        var v = vm.iconVersion || 0;
        var have = vmIcons[vm.id];
        if (!v) { if (have) delete vmIcons[vm.id]; return; }
        if (have && have.version === v) return;
        vmIcons[vm.id] = { version: v, data: have ? have.data : '' };
        sendCmd('getVmIcon', { vmId: vm.id });
    });
}

function onVmIcon(msg) {
    var entry = vmIcons[msg.vmId] || { version: 0, data: '' };
    entry.data = msg.data || '';
    vmIcons[msg.vmId] = entry;
    renderVmTable();
    if (detailVmId === msg.vmId) renderVmDetail();
}

/* The tile content for a VM: its custom icon, or the OS glyph. */
function vmTileHtml(vm) {
    var ic = vmIcons[vm.id];
    if (ic && ic.data && /^data:image\/(png|x-icon);base64,[A-Za-z0-9+\/=]+$/.test(ic.data))
        return '<img class="vm-custom-icon" alt="" src="' + ic.data + '">';
    return osIcon(vm.osType);
}

function vmIndexById(id) {
    return vms.findIndex(function(vm) { return vm.id === id; });
}

function openEditVmModal(idx) {
    var vm = vms[idx];
    if (!vm || vm.running || vm.buildingVhdx) return;
    editVmState = { id: vm.id, name: vm.name, initial: Object.assign({}, vm), previousFocus: modalOpener(vm.id) };
    document.getElementById('edit-vm-title').textContent = 'Edit ' + vm.name;
    document.getElementById('edit-ram-size').value = vm.ramMb;
    document.getElementById('edit-ram-size').min = hostBridge.isMac ? 512 : 4000;
    document.getElementById('edit-cpu-cores').value = vm.cpuCores;
    setGpuSelection('edit-gpu-mode', vm);
    document.getElementById('edit-net-mode').value = String(vm.networkMode);
    updateEditVmModal();
    document.getElementById('edit-vm-overlay').classList.add('active');
    document.getElementById('edit-ram-size').focus();
}

function closeEditVmModal() {
    document.getElementById('edit-vm-overlay').classList.remove('active');
    restoreVmModalFocus(editVmState, '.edit');
    editVmState = null;
}

/* The control that opened a dialog, to give focus back when it closes. */
function modalOpener(id) {
    var a = document.activeElement;
    if (a && a !== document.body && a.isConnected) return a;
    var card = rowCache[id];
    return card ? card.querySelector('.more-btn') : null;
}

function restoreVmModalFocus(state, selector) {
    if (!state) return;
    var target = state.previousFocus;
    if (!target || !target.isConnected || target.disabled) {
        var row = rowCache[state.id];
        target = row && row.querySelector(selector);
    }
    if (!target || !target.isConnected || target.disabled) target = document.getElementById('btn-new-sandbox');
    target.focus();
}

function editVmValues() {
    var values = {
        ramMb: document.getElementById('edit-ram-size').valueAsNumber,
        cpuCores: document.getElementById('edit-cpu-cores').valueAsNumber
    };
    if (!hostBridge.isMac) {
        Object.assign(values, selectedGpu('edit-gpu-mode'));
        values.networkMode = Number(document.getElementById('edit-net-mode').value);
    }
    return values;
}

function editVmValidationError(values) {
    if (!Number.isInteger(values.cpuCores) || values.cpuCores < 1 || values.cpuCores > 2147483647)
        return 'CPU cores must be a whole number of at least 1.';
    var minRam = hostBridge.isMac ? 512 : 4000;
    if (values.ramMb !== editVmState.initial.ramMb &&
        (!Number.isInteger(values.ramMb) || values.ramMb < minRam || values.ramMb > 2147483647))
        return 'RAM must be a whole number of at least ' + minRam + ' MB.';
    return '';
}

function updateEditVmModal() {
    if (!editVmState) return;
    var vm = vms[vmIndexById(editVmState.id)];
    if (!vm) { closeEditVmModal(); return; }
    document.getElementById("edit-vm-title").textContent = "Edit " + vm.name;
    var values = editVmValues();
    var conflict = false;
    var fields = { ramMb: 'edit-ram-size', cpuCores: 'edit-cpu-cores' };
    if (!hostBridge.isMac) fields.networkMode = 'edit-net-mode';
    Object.keys(fields).forEach(function(field) {
        if (vm[field] === editVmState.initial[field]) return;
        if (values[field] !== editVmState.initial[field] && values[field] !== vm[field]) {
            conflict = true;
            return;
        }
        document.getElementById(fields[field]).value = vm[field];
        editVmState.initial[field] = vm[field];
    });
    if (!hostBridge.isMac && gpuSelectionValue(vm) !== gpuSelectionValue(editVmState.initial)) {
        if (gpuSelectionValue(values) !== gpuSelectionValue(editVmState.initial) &&
            gpuSelectionValue(values) !== gpuSelectionValue(vm)) conflict = true;
        else {
            setGpuSelection('edit-gpu-mode', vm);
            editVmState.initial.gpuMode = vm.gpuMode;
            editVmState.initial.gpuId = vm.gpuId;
        }
    }
    var disabled = vm.running || vm.buildingVhdx || vm.starting;
    document.querySelectorAll('#edit-vm-overlay input, #edit-vm-overlay select').forEach(function(el) {
        el.disabled = !!disabled;
    });
    var values = editVmValues();
    var error = disabled ? 'Stop the VM before editing its configuration.' :
        conflict ? 'These settings changed in another interface. Reopen this editor to review the latest values.' : editVmValidationError(values);
    document.getElementById('edit-vm-warn').textContent = error;
    document.getElementById('btn-save-edit-vm').disabled = !!error;
}

function saveEditVm() {
    if (!editVmState) return;
    updateEditVmModal();
    if (!editVmState || document.getElementById('btn-save-edit-vm').disabled) return;
    var idx = vmIndexById(editVmState.id);
    var values = editVmValues();
    if (values.ramMb !== editVmState.initial.ramMb) values.ramMb = alignRamMb(values.ramMb);
    var gpuChanged = !hostBridge.isMac &&
        gpuSelectionValue(values) !== gpuSelectionValue(editVmState.initial) &&
        gpuSelectionValue(values) !== gpuSelectionValue(vms[idx]);
    var fields = Object.keys(values).filter(function(field) {
        return field !== 'gpuMode' && field !== 'gpuId' &&
            values[field] !== editVmState.initial[field] && values[field] !== vms[idx][field];
    });
    closeEditVmModal();
    fields.forEach(function(field) {
        sendCmd('editVm', { vmId: vms[idx].id, field: field, value: String(values[field]) });
    });
    if (gpuChanged)
        sendCmd('editVm', { vmId: vms[idx].id, field: 'gpuMode', value: String(values.gpuMode), gpuId: values.gpuId });
}

document.getElementById('edit-vm-overlay').addEventListener('input', updateEditVmModal);
document.getElementById('edit-vm-overlay').addEventListener('change', updateEditVmModal);
document.getElementById('edit-ram-size').addEventListener('change', function() {
    if (Number.isInteger(this.valueAsNumber)) this.value = alignRamMb(this.valueAsNumber);
    updateEditVmModal();
});

['edit-vm-overlay', 'snapshot-overlay', 'document-overlay'].forEach(function(id) {
    var backdropPress = false;
    var overlay = document.getElementById(id);
    overlay.addEventListener('mousedown', function(e) { backdropPress = e.target === overlay; });
    overlay.addEventListener('click', function(e) {
        if (backdropPress && e.target === overlay) {
            if (id === 'edit-vm-overlay') closeEditVmModal();
            else if (id === 'document-overlay') closeDocument();
            else closeSnapshotModal();
        }
        backdropPress = false;
    });
});

/* ---- Force Stop VM ---- */

function onStopVm(idx) {
    var vm = vms[idx];
    if (!vm) return;
    if (vm.isTemplate) {
        showModal(
            'Cancel the template build?',
            'Stopping a template build deletes the incomplete template "' + vm.name + '".',
            'Stop and delete'
        ).then(function(confirmed) {
            if (!confirmed) return;
            /* The Windows host turns a running VM off as part of deleting it. */
            if (hostBridge.isMac) sendCmd('stopVm', { vmId: vm.id });
            sendCmd('deleteVm', { vmId: vm.id });
        });
    } else {
        if (storedPref('suppress_force_stop_warn', '') === '1') {
            sendCmd('stopVm', { vmId: vm.id });
        } else {
            showForceStopModal(idx);
        }
    }
}

function showForceStopModal(idx) {
    var vm = vms[idx];
    if (!vm) return;
    showModal('Force stop?',
        'Force stop turns "' + vm.name + '" off at once, like pulling the plug. Unsaved work in it is lost and its disk can be damaged. Use Shut down when you can.',
        'Force stop', { dontShowAgain: true }
    ).then(function(result) {
        if (!result) return;
        if (result.dontShowAgain) storePref('suppress_force_stop_warn', '1');
        sendCmd('stopVm', { vmId: vm.id });
    });
}

/* ---- Delete VM ---- */

function onDeleteVm(idx) {
    var vm = vms[idx];
    if (!vm) return;
    showModal(
        'Delete VM?',
        'Delete "' + vm.name + '" with its disk and snapshots? This cannot be undone.' +
            (vm.running && !hostBridge.isMac ? '\n\nThe VM is running; it is turned off first.' : ''),
        'Delete'
    ).then(function(confirmed) {
        if (confirmed) {
            sendCmd('deleteVm', { vmId: vm.id });
        }
    });
}

function dismissVmError(id) {
    var vm = vms[vmIndexById(id)];
    if (!vm) return;
    vm.lastError = '';
    sendCmd('dismissVmError', { vmId: id });
    renderVmTable();
}

/* ---- Snapshots ---- */

/* Parse select value string into {snapIndex, branchIndex} */
function parseSnapValue(val) {
    if (!val || val === 'current') return {snapIndex: -1, branchIndex: -1};
    if (val === 'base') return {snapIndex: -2, branchIndex: -1};
    if (val.substring(0, 5) === 'base-') return {snapIndex: -2, branchIndex: parseInt(val.substring(5))};
    var parts = val.split('-');
    if (parts.length === 1) return {snapIndex: parseInt(parts[0]), branchIndex: -1};
    return {snapIndex: parseInt(parts[0]), branchIndex: parseInt(parts[1])};
}

/* Index of the snapshot `snap` was taken on, or -2 for the base (also for an
   unknown parent, so a snapshot is never hidden). */
function snapshotParent(snap) {
    return typeof snap.parent === 'number' && snap.parent >= 0 ? snap.parent : -2;
}

function snapshotTreeSignature(vm) {
    return JSON.stringify([vm.hasSnapshots, vm.baseBranches || [], vm.snapshots || []]);
}

function snapshotSelection(vm, value) {
    var p = value === 'current' ? { snapIndex: vm.snapCurrent, branchIndex: vm.snapCurrentBranch } : parseSnapValue(value);
    var snap = p.snapIndex >= 0 && (vm.snapshots || [])[p.snapIndex];
    var branches = snap ? snap.branches || [] : vm.baseBranches || [];
    var branch = p.branchIndex >= 0 && branches[p.branchIndex];
    return { snapIndex: p.snapIndex, branchIndex: p.branchIndex, snapshot: snap, branch: branch };
}

function snapshotRowValue(vm, value) {
    var p = snapshotSelection(vm, value);
    if (p.branch) return (p.snapIndex === -2 ? 'base-' : p.snapIndex + '-') + p.branchIndex;
    if (p.snapshot) return String(p.snapIndex);
    return 'base';
}

function snapshotPath(vm, value) {
    var p = snapshotSelection(vm, value);
    var path = 'Base';
    if (p.snapshot) {
        var chain = [], s = p.snapIndex, snaps = vm.snapshots || [];
        while (s >= 0 && snaps[s] && chain.length < snaps.length) {
            chain.unshift(snaps[s].name);
            s = snapshotParent(snaps[s]);
        }
        path += ' \u2192 ' + chain.join(' \u2192 ');
    }
    if (p.branch) path += ' \u2192 ' + (p.branch.name || 'branch ' + (p.branchIndex + 1));
    if (value !== 'current' && p.branchIndex < 0) path += ' [new branch]';
    return path;
}

function openSnapshotModal(idx) {
    var vm = vms[idx];
    if (!vm || hostBridge.isMac) return;
    var value = selectedSnap.get(vm.id) || 'current';
    snapshotModal = {
        id: vm.id, name: vm.name,
        currentRow: snapshotRowValue(vm, value),
        currentPath: snapshotPath(vm, value),
        selectedValue: null,
        treeSignature: snapshotTreeSignature(vm),
        previousFocus: modalOpener(vm.id),
        signature: null
    };
    document.getElementById('snapshot-title').textContent = 'Snapshots — ' + vm.name;
    document.getElementById('snapshot-warn').textContent = '';
    renderSnapshotModal();
    document.getElementById('snapshot-overlay').classList.add('active');
    var choice = document.querySelector('#snapshot-tree [aria-pressed="true"]:not(:disabled)');
    (choice || document.querySelector('#snapshot-overlay .modal-buttons button:last-child')).focus();
}

function closeSnapshotModal() {
    document.getElementById('snapshot-overlay').classList.remove('active');
    restoreVmModalFocus(snapshotModal, '.snap-open');
    snapshotModal = null;
}

function renderSnapshotModal() {
    if (!snapshotModal) return;
    var vm = vms[vmIndexById(snapshotModal.id)];
    if (!vm) { closeSnapshotModal(); return; }
    document.getElementById("snapshot-title").textContent = "Snapshots — " + vm.name;
    var value = selectedSnap.get(vm.id) || 'current';
    var disabled = !!(vm.running || vm.buildingVhdx);
    var treeSignature = snapshotTreeSignature(vm);
    if (treeSignature !== snapshotModal.treeSignature) {
        snapshotModal.currentRow = snapshotRowValue(vm, value);
        snapshotModal.currentPath = snapshotPath(vm, value);
        snapshotModal.selectedValue = null;
        snapshotModal.treeSignature = treeSignature;
    }
    var signature = [treeSignature, vm.snapCurrent, vm.snapCurrentBranch, value, snapshotModal.selectedValue, disabled].join('|');
    if (signature === snapshotModal.signature) return;
    snapshotModal.signature = signature;
    var tree = document.getElementById('snapshot-tree');
    var focusedValue = tree.contains(document.activeElement) ? document.activeElement.value : null;
    tree.textContent = '';
    var list = document.createElement('ul');
    tree.appendChild(list);
    var p = snapshotSelection(vm, value);
    var currentRow = snapshotModal.currentRow;
    var selectedRow = snapshotRowValue(vm, value);
    document.getElementById('snapshot-current').textContent = 'Current: ' + snapshotModal.currentPath;
    document.getElementById('snapshot-help').textContent = disabled
        ? 'Stop the VM to select a different disk or manage snapshots.'
        : !vm.hasSnapshots ? 'No snapshots. This VM uses its original disk.'
        : value === 'current' ? 'The next start resumes the current disk. A frozen disk will start in a new branch.'
        : p.branch ? 'The next start resumes this branch.'
        : 'The next start creates a new branch from this disk.';

    function addChoice(parent, val, name, meta) {
        var item = document.createElement('li');
        var choice = document.createElement('button');
        choice.type = 'button';
        choice.className = 'snapshot-choice';
        choice.value = val;
        choice.setAttribute('aria-pressed', String(selectedRow === val));
        choice.disabled = disabled;
        choice.onclick = function() {
            snapshotModal.selectedValue = val;
            selectedSnap.set(vm.id, vm.hasSnapshots ? val : 'current');
            document.getElementById('snapshot-warn').textContent = '';
            renderVmTable();
        };
        var text = document.createElement('span');
        text.className = 'snapshot-name';
        text.textContent = name + (currentRow === val ? ' (current)' : '') +
            (snapshotModal.selectedValue === val ? ' (selected)' : '');
        choice.appendChild(text);
        if (meta) {
            var info = document.createElement('span');
            info.className = 'snapshot-meta';
            info.textContent = meta;
            choice.appendChild(info);
        }
        item.appendChild(choice);
        parent.appendChild(item);
        return item;
    }
    function addBranches(parent, branches, prefix) {
        branches.forEach(function(branch, b) {
            var meta = branch.date ? 'Modified ' + branch.date : '';
            if (branch.sizeGb) meta += (meta ? ' · ' : '') + branch.sizeGb + ' GB including parent disks';
            addChoice(parent, prefix + b, branch.name || 'branch ' + (b + 1), meta);
        });
    }
    var base = addChoice(list, 'base', 'Base', vm.hasSnapshots ? 'New branch on start' : 'Original disk');
    if (vm.hasSnapshots) {
        var children = document.createElement('ul');
        base.appendChild(children);
        addBranches(children, vm.baseBranches || [], 'base-');
        /* A snapshot sits under the one it was taken on, after that one's branches */
        (function addSnapshots(parent, parentIndex) {
            (vm.snapshots || []).forEach(function(snap, i) {
                if (snapshotParent(snap) !== parentIndex) return;
                var item = addChoice(parent, String(i), snap.name,
                    (snap.date ? 'Created ' + snap.date + ' · ' : '') + 'New branch on start');
                var branches = document.createElement('ul');
                item.appendChild(branches);
                addBranches(branches, snap.branches || [], i + '-');
                addSnapshots(branches, i);
            });
        })(children, -2);
    }
    if (focusedValue !== null && !disabled) {
        var focusChoice = Array.from(tree.querySelectorAll('.snapshot-choice')).find(function(choice) { return choice.value === focusedValue; });
        (focusChoice || tree.querySelector('[aria-pressed="true"]')).focus();
    }
    var editable = !!(p.snapshot || p.branch);
    document.getElementById('btn-snapshot-create').disabled = disabled;
    document.getElementById('btn-snapshot-rename').disabled = disabled || !editable;
    document.getElementById('btn-snapshot-delete').disabled = disabled || !editable;
}

function snapshotActionContext() {
    if (!snapshotModal) return null;
    var vm = vms[vmIndexById(snapshotModal.id)];
    if (!vm || vm.running || vm.buildingVhdx) return null;
    return { modal: snapshotModal, id: vm.id, name: vm.name, value: selectedSnap.get(vm.id) || 'current',
        tree: snapshotTreeSignature(vm), vm: vm };
}

function snapshotActionIndex(context) {
    if (snapshotModal !== context.modal) return -1;
    var idx = vmIndexById(context.id);
    var vm = vms[idx];
    if (!vm || vm.running || vm.buildingVhdx) return -1;
    if (context.tree !== snapshotTreeSignature(vm)) {
        document.getElementById('snapshot-warn').textContent = 'Snapshots changed. Select the disk again.';
        return -1;
    }
    return idx;
}

function takeSnapshot() {
    var context = snapshotActionContext();
    if (!context) return;
    showModal('New snapshot', 'Freeze the current disk as a new snapshot; the VM continues on a new branch of it. Snapshots are frozen points in time that you can create independent branches from.', 'Create', {
        confirmClass: 'primary',
        input: { label: 'Snapshot name', value: 'Snapshot ' + ((context.vm.snapshots || []).length + 1) }
    }).then(function(result) {
        if (result === false) return;
        var idx = snapshotActionIndex(context);
        if (idx >= 0) sendCmd('snapTake', { vmId: vms[idx].id, name: result });
    });
}

function renameSnapshot() {
    var context = snapshotActionContext();
    if (!context) return;
    var p = snapshotSelection(context.vm, context.value);
    var target = p.branch || p.snapshot;
    if (!target) return;
    showModal('Rename', 'Enter a new name:', 'Rename', {
        confirmClass: 'primary',
        input: { label: 'Name', value: target.name || '' }
    }).then(function(result) {
        if (result === false || result === target.name) return;
        var idx = snapshotActionIndex(context);
        if (idx < 0) return;
        var cmd = { vmId: vms[idx].id, snapIndex: p.snapIndex, name: result };
        if (p.branchIndex >= 0) cmd.branchIndex = p.branchIndex;
        sendCmd('snapRename', cmd);
    });
}

function deleteSnapshot() {
    var context = snapshotActionContext();
    if (!context) return;
    var p = snapshotSelection(context.vm, context.value);
    var target = p.branch || p.snapshot;
    if (!target) return;
    if (!p.branch) {
        var child = (context.vm.snapshots || []).find(function(snap) {
            return snapshotParent(snap) === p.snapIndex;
        });
        if (child) {
            showModal('Snapshot in use', 'Snapshot "' + child.name + '" was taken on "' + target.name +
                '" and depends on its disk. Delete "' + child.name + '" first.', 'OK', { alert: true, confirmClass: 'primary' });
            return;
        }
    }
    showModal(p.branch ? 'Delete branch?' : 'Delete snapshot?',
        p.branch ? 'Delete branch "' + (target.name || '') + '"? Its parent disk will be kept.'
            : 'Delete snapshot "' + target.name + '" and all its branches?', 'Delete'
    ).then(function(confirmed) {
        if (!confirmed) return;
        var idx = snapshotActionIndex(context);
        if (idx < 0) return;
        selectedSnap.delete(context.id);
        if (p.branch) sendCmd('snapDeleteBranch', { vmId: vms[idx].id, snapIndex: p.snapIndex, branchIndex: p.branchIndex });
        else sendCmd('snapDelete', { vmId: vms[idx].id, snapIndex: p.snapIndex });
    });
}

function onStartVm(idx) {
    var vm = vms[idx];
    if (!vm || vm.running || vm.buildingVhdx) return;
    var id = vm.id;
    var p = parseSnapValue(selectedSnap.get(id) || 'current');
    if ((p.snapIndex >= 0 || p.snapIndex === -2) && p.branchIndex < 0) {
        var tree = snapshotTreeSignature(vm);
        var parentName = p.snapIndex === -2 ? 'Base' : vm.snapshots[p.snapIndex].name;
        var now = new Date();
        var pad = function(n) { return n < 10 ? '0' + n : '' + n; };
        var defaultName = now.getFullYear() + '-' + pad(now.getMonth()+1) + '-' + pad(now.getDate()) + ' ' + pad(now.getHours()) + ':' + pad(now.getMinutes()) + ':' + pad(now.getSeconds());
        showModal('New branch', 'A new branch will be created from ' + parentName + '. Branches are independent working copies \u2014 changes in one branch don\u2019t affect others or modify the base snapshot.', 'Boot', {
            confirmClass: 'primary', input: { label: 'Branch name', value: defaultName }
        }).then(function(result) {
            if (result === false) return;
            var currentIdx = vmIndexById(id);
            var current = vms[currentIdx];
            if (!current || current.running || current.buildingVhdx) return;
            if (tree !== snapshotTreeSignature(current)) {
                showModal('Snapshots changed', 'Select the disk again before starting the VM.', 'OK', { alert: true, confirmClass: 'primary' });
                return;
            }
            selectedSnap.delete(id);
            sendCmd('startVm', { vmId: current.id, snapIndex: p.snapIndex, branchIndex: p.branchIndex, branchName: result });
        });
    } else {
        sendCmd('startVm', { vmId: vms[idx].id, snapIndex: p.snapIndex, branchIndex: p.branchIndex });
    }
}

/* ---- Log ---- */

/* The panel keeps the newest lines only (appsandbox.log has them all), and
   follows new lines only while it is scrolled to the bottom. */
const LOG_MAX_LINES = 1000;

function logLineIsError(msg) {
    return /^\s*error\b/i.test(msg) || /\bfailed\b/i.test(msg);
}

function appendLog(msg) {
    var panel = document.getElementById('log-panel');
    var atBottom = panel.scrollHeight - panel.scrollTop - panel.clientHeight < 24;
    var div = document.createElement('div');
    div.className = 'log-line' + (logLineIsError(msg) ? ' log-error' : '');
    div.textContent = msg;
    panel.appendChild(div);
    while (panel.childElementCount > LOG_MAX_LINES) panel.removeChild(panel.firstChild);
    document.getElementById('log-last').textContent = msg;
    if (atBottom) panel.scrollTop = panel.scrollHeight;
}

function clearLog() {
    document.getElementById('log-panel').textContent = '';
    document.getElementById('log-last').textContent = '';
}

function copyText(text) {
    if (navigator.clipboard && navigator.clipboard.writeText)
        return navigator.clipboard.writeText(text);
    return new Promise(function(resolve, reject) {
        var area = document.createElement('textarea');
        area.value = text;
        area.style.position = 'fixed';
        area.style.opacity = '0';
        document.body.appendChild(area);
        area.select();
        var ok = false;
        try { ok = document.execCommand('copy'); } catch (e) {}
        document.body.removeChild(area);
        if (ok) resolve(); else reject();
    });
}

function copyLog() {
    var lines = Array.prototype.map.call(document.getElementById('log-panel').children,
        function(d) { return d.textContent; });
    copyText(lines.join('\n')).then(function() {
        showToast('info', 'Activity copied (' + lines.length + ' lines).');
    }, function() {
        showToast('error', 'Activity could not be copied.');
    });
}

/* ---- Notices (non-blocking) ---- */

/* kind: info | warning | error. Errors stay until dismissed; others go after
   a few seconds. opts.id reuses one notice (exit progress), opts.sticky has
   no close button and stays. */
function showToast(kind, message, opts) {
    opts = opts || {};
    var host = document.getElementById('toasts');
    var t = opts.id ? document.getElementById(opts.id) : null;
    if (!t) {
        t = el('div', 'toast');
        if (opts.id) t.id = opts.id;
        host.appendChild(t);
    }
    t.className = 'toast ' + (kind || 'info');
    t.setAttribute('role', kind === 'error' ? 'alert' : 'status');
    t.textContent = '';
    t.appendChild(el('span', 'toast-text', message || ''));
    (opts.actions || []).forEach(function(a) {
        var b = el('button', 'small-btn', a.label);
        b.type = 'button';
        b.onclick = a.run;
        t.appendChild(b);
    });
    if (!opts.sticky) {
        var x = el('button', 'notice-dismiss', '\u00d7');
        x.type = 'button';
        x.setAttribute('aria-label', 'Dismiss');
        x.onclick = function() { if (t.parentNode) t.parentNode.removeChild(t); };
        t.appendChild(x);
    }
    clearTimeout(t._timer);
    if (!opts.sticky && kind !== 'error')
        t._timer = setTimeout(function() { if (t.parentNode) t.parentNode.removeChild(t); }, 7000);
    while (host.children.length > 4) host.removeChild(host.firstChild);
    return t;
}

/* Exit after shutting VMs down: progress, and a way to change one's mind. */
function onExitProgress(msg) {
    var toast = document.getElementById('toast-exit');
    if (!msg.active) {
        exitState = null;
        if (toast && toast.parentNode) toast.parentNode.removeChild(toast);
        renderVmTable();
        return;
    }
    var changed = !exitState;
    exitState = { remaining: msg.remaining, session: !!msg.session };
    var text = msg.remaining > 0
        ? 'Shutting down ' + msg.remaining + (msg.remaining === 1 ? ' VM' : ' VMs') +
          '; App Sandbox exits when ' + (msg.remaining === 1 ? 'it has' : 'they have') + ' stopped.'
        : 'The VMs have stopped. App Sandbox is exiting\u2026';
    if (toast) {
        toast.querySelector('.toast-text').textContent = text;
    } else {
        showToast('info', text, { id: 'toast-exit', sticky: true,
            actions: msg.session ? [] : [{ label: 'Don\u2019t exit', run: function() { sendCmd('cancelExit'); } }] });
    }
    if (changed) renderVmTable();
}

/* ---- Documents (license, third-party notices) ---- */

let documentShown = null;

function showDocument(msg) {
    var overlay = document.getElementById('document-overlay');
    documentShown = msg.kind || null;
    document.getElementById('document-title').textContent = msg.title || 'Document';
    document.getElementById('document-text').textContent = msg.message || '';
    if (!overlay.classList.contains('active')) overlay._previousFocus = document.activeElement;
    overlay.classList.add('active');
    document.getElementById('document-text').scrollTop = 0;
    overlay.querySelector('.modal-buttons .primary').focus();
}

function closeDocument() {
    var overlay = document.getElementById('document-overlay');
    if (!overlay.classList.contains('active')) return;
    overlay.classList.remove('active');
    if (overlay._previousFocus && overlay._previousFocus.isConnected) overlay._previousFocus.focus();
    overlay._previousFocus = null;
}

function openDocumentOnline() {
    if (documentShown) sendCmd('openLink', { which: documentShown });
}

/* ---- Prerequisite check ---- */

/* The Virtual Machine Platform dialog. message is fixed text from this file. */
function setPrereqDialog(html, buttons) {
    var overlay = document.getElementById('prereq-overlay');
    var box = document.getElementById('prereq-buttons');
    document.getElementById('prereq-message').innerHTML = html;
    box.textContent = '';
    box.hidden = !buttons.length;
    buttons.forEach(function(b) {
        var btn = el('button', b.primary ? 'primary' : '', b.label);
        btn.type = 'button';
        btn.addEventListener('click', b.run);
        box.appendChild(btn);
    });
    if (!overlay.classList.contains('active')) {
        overlay._previousFocus = document.activeElement;
        overlay.classList.add('active');
    }
    var focus = box.querySelector('.primary') || box.querySelector('button');
    if (focus) focus.focus();
}

function closePrereqDialog() {
    var overlay = document.getElementById('prereq-overlay');
    if (!overlay.classList.contains('active')) return;
    overlay.classList.remove('active');
    if (overlay._previousFocus && overlay._previousFocus.isConnected) overlay._previousFocus.focus();
    overlay._previousFocus = null;
}

/* The library's empty state while Virtual Machine Platform is off or waits
   for a restart: say so, instead of offering to create a first VM. */
function buildPrereqState() {
    var box = el('div', 'empty-state');
    box.setAttribute('role', 'status');
    if (prereqState === 'enabling') {
        box.appendChild(el('span', 'spinner'));
        box.appendChild(el('h2', '', 'Turning on Virtual Machine Platform\u2026'));
        box.appendChild(el('p', 'muted', 'This can take a minute.'));
        return box;
    }
    var btn;
    if (prereqState === 'reboot') {
        box.appendChild(el('h2', '', 'Restart Windows to finish setup'));
        box.appendChild(el('p', 'muted', 'Virtual Machine Platform is on, but Windows must restart before VMs can run. Your VMs appear here afterwards.'));
        btn = el('button', 'primary', 'Restart now\u2026');
        btn.onclick = confirmRestart;
    } else {
        box.appendChild(el('h2', '', 'Turn on Virtual Machine Platform'));
        box.appendChild(el('p', 'muted', 'App Sandbox runs VMs with this Windows feature, and it is off on this PC. ' +
            'VMs you already have, and their disks, are not affected: they appear here once it is on.' +
            (prereqState === 'failed' ? ' Turning it on from App Sandbox did not work; turn it on in Settings > System > Optional features > More Windows features, then restart Windows.' : '')));
        btn = el('button', 'primary', 'Turn on\u2026');
        btn.onclick = onPrereqRequired;
    }
    btn.type = 'button';
    box.appendChild(btn);
    return box;
}

function onPrereqRequired() {
    setPrereqDialog('<p>App Sandbox needs the <strong>Virtual Machine Platform</strong> Windows feature to create and run VMs, and it is not on. ' +
        'VMs you already have, and their disks, are not affected.</p>', [
        { label: 'Not now', run: closePrereqDialog },
        { label: 'Turn on', primary: true, run: enableFeature }
    ]);
}

function onPrereqReboot() {
    setPrereqDialog('<p><strong>Virtual Machine Platform</strong> is on, but Windows must restart before VMs can be created or started.</p>', [
        { label: 'Later', run: closePrereqDialog },
        { label: 'Restart now\u2026', primary: true, run: confirmRestart }
    ]);
}

function confirmRestart() {
    closePrereqDialog();
    showModal('Restart Windows now?', 'Save your work in other apps first: Windows restarts right away.',
        'Restart now', { confirmClass: 'primary' }).then(function(ok) {
        if (ok) sendCmd('enableFeatureReboot');
    });
}

function enableFeature() {
    setPrereqState('enabling');
    setPrereqDialog('<p>Turning on <strong>Virtual Machine Platform</strong>. This can take a minute\u2026</p>' +
        '<div class="prereq-progress"><div class="prereq-progress-bar" id="prereq-bar"></div></div>' +
        '<div class="prereq-pct" id="prereq-pct">0%</div>', []);
    sendCmd('enableFeature');
}

function onPrereqProgress(msg) {
    var bar = document.getElementById('prereq-bar');
    var pctEl = document.getElementById('prereq-pct');
    if (bar) bar.style.width = msg.pct + '%';
    if (pctEl) pctEl.textContent = msg.pct + '%';
}

function onPrereqResult(msg) {
    if (msg.ok && !msg.reboot) {
        closePrereqDialog();
        setPrereqState(null);
    } else if (msg.ok && msg.reboot) {
        setPrereqState('reboot');
        setPrereqDialog('<p><strong>Virtual Machine Platform</strong> is on. Windows must restart for the change to take effect.</p>', [
            { label: 'Later', run: closePrereqDialog },
            { label: 'Restart now\u2026', primary: true, run: confirmRestart }
        ]);
    } else {
        setPrereqState('failed');
        setPrereqDialog('<p><strong>Virtual Machine Platform</strong> could not be turned on.</p>' +
            '<p>Turn it on yourself: Settings &gt; System &gt; Optional features &gt; More Windows features &gt; Virtual Machine Platform, then restart Windows.</p>', [
            { label: 'Close', primary: true, run: closePrereqDialog }
        ]);
    }
}

/* ---- Modal ---- */

/* Alerts from the host wait while another dialog is open, instead of
   replacing it (its answer would be lost). */
let alertQueue = [];

function showAlert(msg) {
    var kind = msg.kind || 'error';
    var title = msg.title || (kind === 'error' ? 'Something went wrong'
        : kind === 'warning' ? 'Check this first' : 'App Sandbox');
    if (pendingConfirm || document.getElementById('modal-overlay').classList.contains('active')) {
        alertQueue.push({ title: title, message: msg.message || '' });
        return;
    }
    showModal(title, msg.message || '', 'OK', { alert: true, confirmClass: 'primary' });
}

function showNextAlert() {
    if (!alertQueue.length || pendingConfirm) return;
    var next = alertQueue.shift();
    showModal(next.title, next.message, 'OK', { alert: true, confirmClass: 'primary' });
}

/* One dialog for confirmations, questions with a text field and alerts.
   opts: confirmClass, alert (only an OK button), input {label, value},
   dontShowAgain (resolves {dontShowAgain} when confirmed). */
function showModal(title, message, confirmText, opts) {
    opts = opts || {};
    var previousFocus = pendingConfirm ? pendingConfirm.previousFocus : document.activeElement;
    document.getElementById('modal-title').textContent = title;
    document.getElementById('modal-message').textContent = message;
    var confirmBtn = document.getElementById('modal-confirm-btn');
    confirmBtn.textContent = confirmText || 'Confirm';
    confirmBtn.className = opts.confirmClass || 'danger';
    document.getElementById('modal-cancel-btn').hidden = !!opts.alert;
    document.getElementById('modal-dont-show-row').hidden = !opts.dontShowAgain;
    document.getElementById('modal-dont-show').checked = false;
    var inputRow = document.getElementById('modal-input-row');
    var inputEl = document.getElementById('modal-input');
    if (opts.input) {
        inputRow.hidden = false;
        inputEl.value = opts.input.value || '';
        document.getElementById('modal-input-label').textContent = opts.input.label || 'Name';
        inputEl.onkeydown = function(e) { if (e.key === 'Enter') { e.preventDefault(); modalResolve(true); } };
        inputEl.oninput = function() {
            /* Strip characters that would break the INI-style .dat file */
            var clean = inputEl.value.replace(/[\n\r\t\[\]\\]/g, '');
            if (clean !== inputEl.value) inputEl.value = clean;
        };
        inputEl.maxLength = 127;
        setTimeout(function() { inputEl.select(); inputEl.focus(); }, 50);
    } else {
        inputRow.hidden = true;
        inputEl.onkeydown = null;
        inputEl.oninput = null;
    }
    document.getElementById('modal-overlay').classList.add('active');
    if (!opts.input) confirmBtn.focus();

    return new Promise(function(resolve) {
        pendingConfirm = { resolve: resolve, hasInput: !!opts.input, dontShowAgain: !!opts.dontShowAgain,
                           previousFocus: previousFocus };
    });
}

function modalResolve(result) {
    document.getElementById('modal-overlay').classList.remove('active');
    if (pendingConfirm) {
        var done = pendingConfirm;
        pendingConfirm = null;
        if (done.previousFocus && done.previousFocus.isConnected && !done.previousFocus.disabled) {
            done.previousFocus.focus();
        } else if (snapshotModal) {
            document.querySelector('#snapshot-overlay .modal-buttons button:last-child').focus();
        } else if (editVmState) {
            document.getElementById('btn-save-edit-vm').focus();
        }
        if (result && done.hasInput) {
            done.resolve(document.getElementById('modal-input').value);
        } else if (result && done.dontShowAgain) {
            done.resolve({ dontShowAgain: document.getElementById('modal-dont-show').checked });
        } else {
            done.resolve(result);
        }
    }
    if (alertQueue.length) setTimeout(showNextAlert, 0);
}

/* ---- Minimum size reporting ---- */

function reportMinSize() {
    /* The layout reflows down to a narrow window (collapsed sidebar), so a
       fixed floor is enough. */
    sendCmd('setMinSize', { width: 760, height: 560 });
}

/* ---- Init ---- */
showView('vms');

/* Signal to C that the UI is ready */
sendCmd('uiReady');

/* Report min size once layout is complete (covers case with no VMs) */
setTimeout(function() {
    if (!minSizeReported) {
        minSizeReported = true;
        reportMinSize();
    }
}, 300);
