/* App Sandbox - WebView2 Frontend */
'use strict';

/* ---- State ---- */
let vms = [];
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

/* ---- Collapsible sections ---- */
function toggleSection(id) {
    var section = document.getElementById(id);
    var collapsed = section.classList.toggle('collapsed');
    localStorage.setItem('collapse_' + id, collapsed ? '1' : '0');
}
(function restoreCollapse() {
    var defaults = { 'log-section': '1' };
    Object.keys(defaults).forEach(function(id) {
        var val = localStorage.getItem('collapse_' + id);
        if (val === null) val = defaults[id];
        if (val === '1') document.getElementById(id).classList.add('collapsed');
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

function sendCmd(action, data) {
    if (data && data.vmId && vmIndexById(data.vmId) < 0) return;
    if (data && data.templateId && !currentTemplates.some(function(t) { return t.id === data.templateId; })) return;
    hostBridge.send(action, data);
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
        case 'alert':         showModal('Error', msg.message, 'OK'); break;
        case 'prereqRequired': onPrereqRequired(); break;
        case 'prereqReboot':   onPrereqReboot(); break;
        case 'prereqProgress': onPrereqProgress(msg); break;
        case 'prereqResult':   onPrereqResult(msg); break;
        case 'vmIcon':         onVmIcon(msg); break;
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

function onFullState(msg) {
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
    list.innerHTML = '';

    /* Default (None) item — always shows "None" inside the list */
    var noneItem = document.createElement('div');
    noneItem.className = 'template-dropdown-item';
    noneItem.innerHTML = '<span class="tpl-name">(None)</span>';
    noneItem.addEventListener('click', function() { selectTemplate('', templateDefaultLabel()); });
    list.appendChild(noneItem);

    templatesForSelectedOS().forEach(function(t) {
        var item = document.createElement('div');
        item.className = 'template-dropdown-item';

        var nameSpan = document.createElement('span');
        nameSpan.className = 'tpl-name';
        nameSpan.textContent = t.name + ' [' + t.osType + ']';
        item.appendChild(nameSpan);

        var delBtn = document.createElement('span');
        delBtn.className = 'tpl-delete';
        delBtn.textContent = '\uD83D\uDDD1\uFE0F';
        delBtn.title = 'Delete template';
        delBtn.addEventListener('click', function(e) {
            e.stopPropagation();
            closeTemplateDropdown();
            onDeleteTemplate(t.name, t.id);
        });
        item.appendChild(delBtn);

        item.addEventListener('click', function() {
            selectTemplate(t.name, t.name + ' [' + t.osType + ']');
        });
        list.appendChild(item);
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
    updateCreateButtons();
}

function closeTemplateDropdown() {
    document.getElementById('template-dropdown').classList.remove('open');
}

document.getElementById('template-dropdown-selected').addEventListener('click', function() {
    document.getElementById('template-dropdown').classList.toggle('open');
});

/* Close dropdown when clicking outside */
document.addEventListener('click', function(e) {
    if (!e.target.closest('#template-dropdown')) {
        closeTemplateDropdown();
    }
});

function onDeleteTemplate(name, id) {
    showModal(
        'Confirm Delete',
        'Are you sure you want to delete template "' + name + '"?\n\nThis will permanently delete the template disk image.',
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

function createValidationError(isTemplate) {
    var osType = document.getElementById('os-type').value;
    if (isTemplate && osType !== 'Windows' && osType !== 'macOS')
        return 'Templates require a Windows or macOS guest.';
    if (isTemplate && document.getElementById('template-select').value)
        return 'Select an OS image to build a new template.';
    if (osType === 'Other' && isTemplate) return 'Manual-install VMs cannot be templates.';
    var error = validateVmName(document.getElementById('vm-name').value.trim()) ||
        validateDiskDirectory(document.getElementById('disk-directory').value.trim());
    if (!error && osType !== 'Other')
        error = validateUsername(document.getElementById('admin-user').value.trim(), isTemplate) ||
                validatePassword(document.getElementById('admin-pass').value);
    if (error) return error;
    if (osType !== 'Other' &&
        document.getElementById('admin-pass').value !== document.getElementById('admin-confirm').value)
        return 'Passwords do not match.';

    var numericFields = [
        ['hdd-size', 'Disk size must be a whole number of at least 1 GB.'],
        ['ram-size', 'RAM must be an even number of at least 512 MB.'],
        ['cpu-cores', 'CPU cores must be a whole number of at least 1.']
    ];
    for (var i = 0; i < numericFields.length; i++) {
        var input = document.getElementById(numericFields[i][0]);
        if (input.value === '' || !input.validity.valid) return numericFields[i][1];
    }

    var path = selectedDiskDirectory();
    var diskKnown = diskSpaceInfo && diskSpaceInfo.path === path;
    if (diskKnown && diskSpaceInfo.freeGb < 0) return 'Disk folder is unavailable.';
    if (diskSpacePending && !diskKnown) return 'Checking disk folder...';

    var hasImage = document.getElementById('image-path').value.trim() !== '';
    var hasTemplate = (osType === 'Windows' || osType === 'macOS') &&
        document.getElementById('template-select').value !== '';
    if (osType !== 'macOS' && !hasImage && (isTemplate || !hasTemplate))
        return 'Select an OS image or an available template.';
    return null;
}

function updateCreateButtons() {
    document.getElementById('btn-create').disabled = !!createValidationError(false);
    document.getElementById('btn-create-template').disabled = !!createValidationError(true);
}

/* Wire up change events */
document.getElementById('create-vm-overlay').addEventListener('input', updateCreateButtons);
document.getElementById('create-vm-overlay').addEventListener('change', updateCreateButtons);
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
function revalidatePassword() {
    var p = document.getElementById('admin-pass').value;
    document.getElementById('admin-pass-warn').textContent = validatePassword(p) || '';
}
document.getElementById('admin-user').addEventListener('input', revalidateUsername);

function checkPasswordMatch() {
    var pass = document.getElementById('admin-pass').value;
    var confirm = document.getElementById('admin-confirm');
    if (confirm.value === '' && pass === '') {
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
document.getElementById('admin-confirm').addEventListener('input', checkPasswordMatch);
checkPasswordMatch();

function showPassword() {
    document.getElementById('admin-pass').type = 'text';
    document.getElementById('admin-confirm').type = 'text';
}
function hidePassword() {
    document.getElementById('admin-pass').type = 'password';
    document.getElementById('admin-confirm').type = 'password';
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
        adminConfirm: document.getElementById('admin-confirm').value,
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
    if (!pass) return 'Password is required.';
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

function onCreateVm() {
    var error = createValidationError(false);
    if (error) { sendCmd('log', { message: error }); updateCreateButtons(); return; }
    var cfg = gatherConfig();
    sendCmd('createVm', cfg);
    clearCreateForm();
    closeCreateModal();
}

function onCreateTemplate() {
    var error = createValidationError(true);
    if (error) { sendCmd('log', { message: error }); updateCreateButtons(); return; }
    var cfg = gatherConfig();
    cfg.isTemplate = true;
    sendCmd('createVm', cfg);
    clearCreateForm();
    closeCreateModal();
}

/* ---- Create Sandbox modal ---- */

function openCreateModal() {
    closeDiskLocationModal(false);
    /* Reset to defaults every time the modal opens */
    document.getElementById('vm-name').value = 'MyAppSandbox';
    document.getElementById('image-path').value = '';
    document.getElementById('disk-directory').value = (lastHostInfo && lastHostInfo.defaultDiskDirectory) || '';
    revalidateDiskDirectory();
    selectTemplate('', templateDefaultLabel());
    document.getElementById('hdd-size').value = 64;
    setGpuSelection('gpu-mode', { gpuMode: 1 });
    document.getElementById('net-mode').value = '1';
    document.getElementById('admin-user').value = 'user';
    document.getElementById('admin-pass').value = 'test123';
    document.getElementById('admin-confirm').value = 'test123';
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
    closeDiskLocationModal(false);
    document.getElementById('create-vm-overlay').classList.remove('active');
    clearTimeout(diskSpaceTimer);
    diskSpaceTimer = null;
    diskSpacePending = false;
    ++diskSpaceRequestId;
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
    var controls = Array.from(overlay.querySelectorAll('button, input, select, [tabindex]')).filter(function(el) {
        return !el.disabled && el.tabIndex >= 0 && el.getClientRects().length;
    });
    if (!controls.length) return;
    var first = controls[0], last = controls[controls.length - 1];
    if (!overlay.contains(document.activeElement) ||
        (event.shiftKey && document.activeElement === first) ||
        (!event.shiftKey && document.activeElement === last)) {
        event.preventDefault();
        (event.shiftKey ? last : first).focus();
    }
}

document.addEventListener('keydown', function(e) {
    var confirmOverlay = document.getElementById('modal-overlay');
    if (confirmOverlay.classList.contains('active')) {
        if (e.key === 'Escape') { e.preventDefault(); modalResolve(false); }
        else trapModalFocus(e, confirmOverlay);
        return;
    }
    var diskOverlay = document.getElementById('disk-location-overlay');
    if (diskOverlay.classList.contains('active')) {
        if (e.key === 'Escape') { e.preventDefault(); closeDiskLocationModal(false); }
        else if (e.key === 'Enter' && e.target.id === 'disk-directory') {
            e.preventDefault();
            closeDiskLocationModal(true);
        } else trapModalFocus(e, diskOverlay);
        return;
    }
    var editOverlay = document.getElementById('edit-vm-overlay');
    if (editOverlay.classList.contains('active')) {
        if (e.key === 'Escape') { e.preventDefault(); closeEditVmModal(); }
        else if (e.key === 'Enter' && e.target.tagName === 'INPUT') {
            e.preventDefault();
            saveEditVm();
        } else trapModalFocus(e, editOverlay);
        return;
    }
    var snapshotOverlay = document.getElementById('snapshot-overlay');
    if (snapshotOverlay.classList.contains('active')) {
        if (e.key === 'Escape') { e.preventDefault(); closeSnapshotModal(); }
        else trapModalFocus(e, snapshotOverlay);
        return;
    }
    if (e.key !== 'Escape') return;
    if (document.getElementById('create-vm-overlay').classList.contains('active')) {
        closeCreateModal();
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

function isBusy(vm) { return !!(vm.buildingVhdx || vm.starting || vm.updatingTools || vm.copyingDisk); }

/* Status label + style for a VM (kind: running | busy | stopping | stopped). */
function vmStatus(vm) {
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
    return { label: 'Start', disabled: bld,
             run: function() { onStartVm(vmIndexById(vm.id)); } };
}

/* The ... menu: secondary and destructive actions. */
function showVmMenu(vm, anchor) {
    closeVmMenu();
    var bld = isBusy(vm);
    var menu = el('div', 'menu-pop');
    menu.setAttribute('role', 'menu');
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
    item('Force stop', vm.running && !bld, function() { onStopVm(vmIndexById(vm.id)); });
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
    }
    if (vm.sshEnabled)
        item('Open SSH terminal', sshReady(vm), function() { sendCmd('sshConnect', { vmId: vm.id }); });
    menu.appendChild(document.createElement('hr'));
    item('Delete VM…', !bld, function() { onDeleteVm(vmIndexById(vm.id)); }, 'danger-item');

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

    var actions = el('div', 'vm-card-actions');
    var pa = primaryAction(vm);
    var pb = el('button', 'primary grow', pa.label);
    pb.type = 'button';
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
    document.getElementById('vm-summary').textContent = vms.length
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

    if (vms.length === 0) {
        var empty = el('div', 'empty-state');
        empty.appendChild(el('h2', '', 'Create your first virtual machine'));
        empty.appendChild(el('p', 'muted', 'Windows 11, Ubuntu or macOS, set up automatically from an installer image.'));
        var btn = el('button', 'primary with-icon', 'New VM');
        btn.type = 'button';
        btn.onclick = openCreateModal;
        empty.appendChild(btn);
        grid.appendChild(empty);
    }

    var pos = 0;
    vms.forEach(function(vm, i) {
        var card = rowCache[vm.id];
        var sig = [
            i === selectedVm, vm.name, vm.running, vm.starting, vm.buildingVhdx, vm.vhdxStaging, vm.updatingTools,
            vm.copyingDisk,
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
        if (grid.children[pos] !== card) grid.insertBefore(card, grid.children[pos] || null);
        pos++;
    });

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
    var sig = [vm.id, vm.running, bld, vm.isTemplate, vm.sshEnabled, sshReady(vm)].join('|');
    renderToolsCard(vm);
    if (actions.getAttribute('data-sig') !== sig) {
        actions.setAttribute('data-sig', sig);
        actions.textContent = '';
        var pa = primaryAction(vm);
        var pb = el('button', 'primary', pa.label);
        pb.type = 'button';
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
        var fs = el('button', 'danger-outline', 'Force stop');
        fs.type = 'button';
        if (!vm.running || bld) fs.disabled = true;
        else fs.onclick = function() { onStopVm(vmIndexById(vm.id)); };
        actions.appendChild(fs);
        var del = el('button', 'danger-outline', 'Delete');
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

    var overview = [
        ['Status', vmStatus(vm).label],
        ['Operating system', vm.osType + (vm.isTemplate ? ' (template)' : '')],
        ['In-VM agent', !vm.running ? 'VM is off' : vm.agentOnline ? 'Connected' : 'Not connected'],
        ['Network', hostBridge.isMac ? 'NAT' : (netNames[vm.networkMode] || 'None') + (vm.networkMode === 2 && vm.netAdapter ? ' · ' + vm.netAdapter : '')]
    ];
    if (vm.sshEnabled) overview.push(['SSH', 'localhost:' + vm.sshPort]);
    if (vm.diskDirectory) overview.push(['Disk folder', vm.diskDirectory]);
    fillRows(document.getElementById('vmd-overview'), overview);

    fillRows(document.getElementById('vmd-hardware'), [
        ['Processors', vm.cpuCores + ' vCPU'],
        ['Memory', vm.ramMb + ' MB'],
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
        if (lastHostInfo.defaultDiskDirectory) rows.push(['Default VM folder', lastHostInfo.defaultDiskDirectory]);
        if (lastHostInfo.freeGb >= 0) rows.push(['Free space there', lastHostInfo.freeGb + ' GB']);
    }
    fillRows(document.getElementById('set-host'), rows);
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

['edit-vm-overlay', 'snapshot-overlay'].forEach(function(id) {
    var backdropPress = false;
    var overlay = document.getElementById(id);
    overlay.addEventListener('mousedown', function(e) { backdropPress = e.target === overlay; });
    overlay.addEventListener('click', function(e) {
        if (backdropPress && e.target === overlay) {
            if (id === 'edit-vm-overlay') closeEditVmModal();
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
            'Cancel Template Build',
            'Stopping a template build will delete the incomplete template "' + vm.name + '".\n\nAre you sure?',
            'Stop & Delete'
        ).then(function(confirmed) {
            if (confirmed) {
                sendCmd('stopVm', { vmId: vm.id });
                sendCmd('deleteVm', { vmId: vm.id });
            }
        });
    } else {
        if (localStorage.getItem('suppress_force_stop_warn') === '1') {
            sendCmd('stopVm', { vmId: vm.id });
        } else {
            showForceStopModal(idx);
        }
    }
}

function showForceStopModal(idx) {
    var vm = vms[idx];
    if (!vm) return;
    document.getElementById('modal-title').textContent = 'Force Stop';
    document.getElementById('modal-message').textContent =
        'Force Stop will immediately power-off "' + vms[idx].name + '" which may result in corruption of its data.';
    document.getElementById('modal-confirm-btn').textContent = 'Force Stop';

    var cb = document.getElementById('modal-dont-show');
    if (cb) { cb.checked = false; cb.parentElement.style.display = ''; }

    document.getElementById('modal-overlay').classList.add('active');
    pendingConfirm = { resolve: function(confirmed) {
        if (confirmed) {
            if (cb && cb.checked) localStorage.setItem('suppress_force_stop_warn', '1');
            sendCmd('stopVm', { vmId: vm.id });
        }
        if (cb) cb.parentElement.style.display = 'none';
    }};
}

/* ---- Delete VM ---- */

function onDeleteVm(idx) {
    var vm = vms[idx];
    if (!vm) return;
    showModal(
        'Confirm Delete',
        'Are you sure you want to delete VM "' + vm.name + '"?\n\nThis will permanently delete all disk data and snapshots.',
        'Delete'
    ).then(function(confirmed) {
        if (confirmed) {
            sendCmd('deleteVm', { vmId: vm.id });
        }
    });
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
    showModal('New Snapshot', 'Freeze the current disk as a new snapshot; the VM continues on a new branch of it. Snapshots are frozen points in time that you can create independent branches from.', 'Create', {
        confirmClass: 'primary',
        input: { label: 'Snapshot name:', value: 'Snapshot ' + ((context.vm.snapshots || []).length + 1) }
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
        input: { label: 'Name:', value: target.name || '' }
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
            showModal('Cannot Delete Snapshot', 'Snapshot "' + child.name + '" was taken on "' + target.name +
                '" and depends on its disk. Delete "' + child.name + '" first.', 'OK', { confirmClass: 'primary' });
            return;
        }
    }
    showModal(p.branch ? 'Delete Branch' : 'Delete Snapshot',
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
        showModal('New Branch', 'A new branch will be created from ' + parentName + '. Branches are independent working copies \u2014 changes in one branch don\u2019t affect others or modify the base snapshot.', 'Boot', {
            confirmClass: 'primary', input: { label: 'Branch name:', value: defaultName }
        }).then(function(result) {
            if (result === false) return;
            var currentIdx = vmIndexById(id);
            var current = vms[currentIdx];
            if (!current || current.running || current.buildingVhdx) return;
            if (tree !== snapshotTreeSignature(current)) {
                showModal('Snapshots Changed', 'Select the disk again before starting the VM.', 'OK', { confirmClass: 'primary' });
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

function appendLog(msg) {
    var panel = document.getElementById('log-panel');
    var div = document.createElement('div');
    div.className = 'log-line';
    div.textContent = msg;
    panel.appendChild(div);
    document.getElementById('log-last').textContent = msg;
    panel.scrollTop = panel.scrollHeight;
}

/* ---- Prerequisite check ---- */

function onPrereqRequired() {
    document.getElementById('prereq-message').innerHTML =
        'App Sandbox requires the <strong>Virtual Machine Platform</strong> Windows feature to create and run VMs. This feature is not currently enabled.';
    document.getElementById('prereq-buttons').innerHTML =
        '<button onclick="document.getElementById(\'prereq-overlay\').classList.remove(\'active\')">Cancel</button>' +
        '<button class="primary" onclick="enableFeature()">Enable</button>';
    document.getElementById('prereq-buttons').style.display = '';
    document.getElementById('prereq-overlay').classList.add('active');
}

function onPrereqReboot() {
    document.getElementById('prereq-message').innerHTML =
        '<strong>Virtual Machine Platform</strong> has been enabled but a reboot is required before VMs can be created or started.';
    document.getElementById('prereq-buttons').innerHTML =
        '<button onclick="document.getElementById(\'prereq-overlay\').classList.remove(\'active\')">Later</button>' +
        '<button class="primary" onclick="sendCmd(\'enableFeatureReboot\')">Reboot Now</button>';
    document.getElementById('prereq-buttons').style.display = '';
    document.getElementById('prereq-overlay').classList.add('active');
}

function enableFeature() {
    document.getElementById('prereq-message').innerHTML =
        'Enabling <strong>Virtual Machine Platform</strong>. This may take a minute...' +
        '<div class="prereq-progress"><div class="prereq-progress-bar" id="prereq-bar"></div></div>' +
        '<div class="prereq-pct" id="prereq-pct">0%</div>';
    document.getElementById('prereq-buttons').style.display = 'none';
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
        document.getElementById('prereq-overlay').classList.remove('active');
    } else if (msg.ok && msg.reboot) {
        document.getElementById('prereq-message').innerHTML =
            '<strong>Virtual Machine Platform</strong> has been enabled. A reboot is required for the change to take effect.';
        document.getElementById('prereq-buttons').innerHTML =
            '<button onclick="document.getElementById(\'prereq-overlay\').classList.remove(\'active\')">Later</button>' +
            '<button class="primary" onclick="sendCmd(\'enableFeatureReboot\')">Reboot Now</button>';
        document.getElementById('prereq-buttons').style.display = '';
    } else {
        document.getElementById('prereq-message').innerHTML =
            'Failed to enable <strong>Virtual Machine Platform</strong>.<br><br>' +
            'Try enabling it manually:<br>' +
            'Settings &gt; System &gt; Optional Features &gt; More Windows Features &gt; Virtual Machine Platform';
        document.getElementById('prereq-buttons').innerHTML =
            '<button onclick="document.getElementById(\'prereq-overlay\').classList.remove(\'active\')">Close</button>';
        document.getElementById('prereq-buttons').style.display = '';
    }
}

/* ---- Modal ---- */

function showModal(title, message, confirmText, opts) {
    var previousFocus = document.activeElement;
    document.getElementById('modal-title').textContent = title;
    document.getElementById('modal-message').textContent = message;
    var confirmBtn = document.getElementById('modal-confirm-btn');
    confirmBtn.textContent = confirmText || 'Confirm';
    confirmBtn.className = (opts && opts.confirmClass) || 'danger';
    var cb = document.getElementById('modal-dont-show');
    if (cb) cb.parentElement.style.display = 'none';
    var inputRow = document.getElementById('modal-input-row');
    var inputEl = document.getElementById('modal-input');
    if (opts && opts.input) {
        inputRow.style.display = 'block';
        inputEl.value = opts.input.value || '';
        if (opts.input.label) document.getElementById('modal-input-label').textContent = opts.input.label;
        inputEl.onkeydown = function(e) { if (e.key === 'Enter') modalResolve(true); };
        inputEl.oninput = function() {
            /* Strip characters that would break the INI-style .dat file */
            var clean = inputEl.value.replace(/[\n\r\t\[\]\\]/g, '');
            if (clean !== inputEl.value) inputEl.value = clean;
        };
        inputEl.maxLength = 127;
        setTimeout(function() { inputEl.select(); inputEl.focus(); }, 50);
    } else {
        inputRow.style.display = 'none';
    }
    document.getElementById('modal-overlay').classList.add('active');
    if (!(opts && opts.input)) confirmBtn.focus();

    return new Promise(function(resolve) {
        pendingConfirm = { resolve: resolve, hasInput: !!(opts && opts.input), previousFocus: previousFocus };
    });
}

function modalResolve(result) {
    document.getElementById('modal-overlay').classList.remove('active');
    if (pendingConfirm) {
        if (pendingConfirm.previousFocus && pendingConfirm.previousFocus.isConnected) {
            pendingConfirm.previousFocus.focus();
        } else if (snapshotModal) {
            document.querySelector('#snapshot-overlay .modal-buttons button:last-child').focus();
        } else if (editVmState) {
            document.getElementById('btn-save-edit-vm').focus();
        }
        if (result && pendingConfirm.hasInput) {
            pendingConfirm.resolve(document.getElementById('modal-input').value);
        } else {
            pendingConfirm.resolve(result);
        }
        pendingConfirm = null;
    }
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
