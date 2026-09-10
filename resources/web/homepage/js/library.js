// The native scanner owns filesystem access. Folder membership is organisation, never
// proof that two files are interchangeable or that a configured project is ready to print.

var LIB_PAGE = 120;

var LIB = {
    state: 'all',
    group: 'all',
    root: 'all',
    search: '',
    kind: '3mf',      // most of the library is loose STL; the pipeline is 3MF
    sort: 'recent',
    shown: LIB_PAGE,
    shelf: 'all',
    folderScope: '',
    recentPaths: {},  // normalised keys of the real recent-files list, for the Recent chip
    recentOrder: [],  // the same list in its own order, which is what "recent" means
    recentRecords: {},
    // Expansion and ticks are keyed by path rather than held in the DOM, because the grid
    // is re-rendered whole on every filter change and anything living only in the markup
    // would silently reset mid-selection.
    expanded: {},
    picked: {},
    pickupOpen: false,
    nativeReady: false,
    thumbnails: {},
    thumbFingerprints: {},
    thumbRequested: {},
    thumbRetried: {},
    thumbForce: {},
    thumbVersions: {}
};

var LIB_WORKSPACE = (typeof PROJECT_LIBRARY !== 'undefined' && PROJECT_LIBRARY.workspace) || {mine: [], downloads: [], jobs: []};
try {
    var savedWorkspace = JSON.parse(window.localStorage.getItem('podslicer.library.workspace'));
    if (savedWorkspace && Array.isArray(savedWorkspace.mine)) LIB_WORKSPACE = savedWorkspace;
} catch (_) { /* The library also works when webview storage is unavailable. */ }

function LibWithin(path, folder) {
    var p = LibKey(path), f = LibKey(folder);
    return !!f && (p === f || p.indexOf(f + '\\') === 0);
}

function LibShelfFor(it) {
    if ((LIB_WORKSPACE.mine || []).some(function (folder) { return LibWithin(it.path, folder.path); })) return 'mine';
    if ((LIB_WORKSPACE.downloads || []).some(function (folder) { return LibWithin(it.path, folder); })) return 'downloads';
    if ((LIB_WORKSPACE.jobs || []).some(function (folder) { return LibWithin(it.path, folder); })) return 'jobs';
    return /^(models|downloads)$/i.test(String(it.root || '').replace(/[\\/]+$/, '').split(/[\\/]/).pop()) ? 'downloads' : 'jobs';
}

function LibSetShelf(shelf) {
    LIB.shelf = shelf; LIB.folderScope = ''; LIB.root = 'all'; LIB.group = 'all';
    LIB.state = 'all'; LIB.search = ''; LIB.shown = LIB_PAGE;
    LIB.kind = shelf === 'jobs' ? '3mf' : 'all';
    $('#LibSearch').val(''); $('#LibRoot').val('all');
    $('#LibKindChip').toggleClass('LibChipOn', LIB.kind === 'all');
    LibBuildGroups(); LibBuildChips(); LibRender();
}

function LibBrowseDesign(el) {
    LIB.shelf = 'mine'; LIB.folderScope = el.getAttribute('data-path');
    LIB.kind = 'all'; LIB.state = 'all'; LIB.group = 'all'; LIB.search = ''; LIB.root = 'all';
    LIB.shown = LIB_PAGE;
    $('#LibSearch').val(''); $('#LibRoot').val('all'); $('#LibKindChip').addClass('LibChipOn');
    LibBuildGroups(); LibBuildChips(); LibRender();
}

function LibShowDesignFolder(el) {
    var path = el.getAttribute('data-path');
    if (path) SendWXMessage(JSON.stringify({command: 'homepage_explore_recentfile', data: {path: path}}));
}

function LibPinDesign(el) {
    var path = el.getAttribute('data-path');
    if (!path) return;
    var mine = LIB_WORKSPACE.mine || [];
    if (!mine.some(function (folder) { return LibKey(folder.path) === LibKey(path); })) {
        mine.push({path: path, title: path.replace(/[\\/]+$/, '').split(/[\\/]/).pop()});
    }
    LIB_WORKSPACE.mine = mine;
    try { window.localStorage.setItem('podslicer.library.workspace', JSON.stringify(LIB_WORKSPACE)); } catch (_) {}
    LibSetShelf('mine');
}

function LibUnpinDesign(el) {
    var path = el.getAttribute('data-path');
    LIB_WORKSPACE.mine = (LIB_WORKSPACE.mine || []).filter(function (folder) { return LibKey(folder.path) !== LibKey(path); });
    try { window.localStorage.setItem('podslicer.library.workspace', JSON.stringify(LIB_WORKSPACE)); } catch (_) {}
    LibSetShelf('mine');
}

function LibWorkspaceRender() {
    var labels = [['mine', 'My projects'], ['jobs', 'Print jobs'], ['downloads', 'Downloaded models'], ['all', 'All files']];
    $('#LibShelves').html(labels.map(function (entry) {
        return '<button class="LibShelf' + (LIB.shelf === entry[0] ? ' LibShelfOn' : '') +
            '" onclick="LibSetShelf(\'' + entry[0] + '\')">' + entry[1] + '</button>';
    }).join(''));
    var overview = LIB.shelf === 'mine' && !LIB.folderScope;
    $('#LibraryArea').toggleClass('LibOverview', overview);
    $('#LibSearch').attr('placeholder', overview ? 'Search your project folders' : 'Search names, folders, printers or materials');
    $('#LibLocation').html(LIB.folderScope ? '<button class="LibFolderButton" onclick="LibSetShelf(\'mine\')">My projects</button> / ' +
        LibEscape(LIB.folderScope.replace(/[\\/]+$/, '').split(/[\\/]/).pop()) : '');
    if (!overview) return false;
    var terms = LIB.search.toLowerCase().split(/\s+/).filter(Boolean);
    var folders = (LIB_WORKSPACE.mine || []).filter(function (folder) {
        var hay = [folder.title, folder.description, folder.path].join(' ').toLowerCase();
        return terms.every(function (term) { return hay.indexOf(term) >= 0; });
    });
    var items = LibItems();
    $('#LibGrid').html(folders.map(function (folder) {
        var files = items.filter(function (it) { return LibWithin(it.path, folder.path); });
        var cover = files[0] || {name: folder.title, thumbState: 'unavailable', thumbError: 'No model files in this folder'};
        return '<article class="LibDesignCard"><div class="LibThumb LibDesignThumb">' + LibThumbnailMarkup(cover) + '</div>' +
            '<h3>' + LibEscape(folder.title || folder.path.split(/[\\/]/).pop()) + '</h3>' +
            '<p>' + LibEscape(folder.description || 'Design files, editable projects and working material.') + '</p>' +
            '<div class="LibMeta">' + files.length + ' model files</div>' +
            '<div class="LibDesignActions"><button class="LibChip" data-path="' + LibEscape(folder.path) +
            '" onclick="LibBrowseDesign(this)">Browse files</button><button class="LibFolderButton" data-path="' +
            LibEscape(folder.path) + '" onclick="LibShowDesignFolder(this)">Show folder</button></div>' +
            '<button class="LibFolderButton" data-path="' + LibEscape(folder.path) +
            '" onclick="LibUnpinDesign(this)" title="Remove this shortcut; files stay where they are">Remove shortcut</button></article>';
    }).join('') || '<div class="LibEmpty">' + (terms.length ? 'No project folders match this search.' :
        'Add your project folder, then choose “My project” under Library folders. CAD-only folders can live here too.') + '</div>');
    $('#LibCount').text(folders.length + ' project folders'); $('#LibMore').hide();
    return true;
}

// Order matters: it is the pipeline, and the chips read as a progression.
var LIB_STATES = [
    { id: 'all',          label: 'All' },
    { id: 'recent',       label: 'Recently opened' },
    { id: 'mesh',         label: 'Raw mesh' },
    { id: 'foreign',      label: 'Bambu preset' },
    { id: 'needs-colour', label: 'Needs colour' },
    { id: 'prepped',      label: 'Configured' },
    { id: 'sliced',       label: 'Contains toolpaths' },
    { id: 'missing',      label: 'Missing files' },
    // Listed so a corrupt archive is findable. Without a chip it appears only under All,
    // which is the one view nobody scans.
    { id: 'unreadable',   label: 'Preview unavailable' }
];

var LIB_BADGE = {
    'mesh':         { text: 'raw mesh',    cls: 'LibBadgeMesh' },
    'foreign':      { text: 'bambu preset', cls: 'LibBadgeForeign' },
    'needs-colour': { text: 'needs colour', cls: 'LibBadgeColour' },
    'prepped':      { text: 'configured',  cls: 'LibBadgePrepped' },
    'sliced':       { text: 'toolpaths',   cls: 'LibBadgeSliced' },
    'sent':         { text: 'sent',        cls: 'LibBadgeSent' },
    'unreadable':   { text: 'no preview',  cls: 'LibBadgeBad' },
    // Not a pipeline state: it means the file is real and open-able but lives outside the
    // indexed roots, so nothing is known about it beyond its path.
    'unindexed':    { text: 'not in library', cls: 'LibBadgeBad' },
    'pending':      { text: 'reading metadata', cls: 'LibBadgeMesh' },
    'missing':      { text: 'file missing', cls: 'LibBadgeBad' }
};

function LibItems() {
    var items = ((typeof PROJECT_LIBRARY !== 'undefined' && PROJECT_LIBRARY.items) || []).slice();
    var seen = Object.create(null);
    items = items.filter(function (it) {
        var key = LibKey(it.path);
        if (seen[key]) return false;
        seen[key] = true;
        return true;
    });
    LIB.recentOrder.forEach(function (path) {
        if (!seen[LibKey(path)]) items.push(LibSynthRecent(path));
    });
    return items;
}

function LibEscape(s) {
    return String(s == null ? '' : s)
        .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;');
}

// A path is one identity however it was spelled. The recent list is written by whoever
// opened the file - the app writes backslashes, a command line or a drag-drop can deliver
// forward slashes - while the index always holds backslashes. Comparing the raw strings
// dropped every recent whose separators disagreed, silently, for a reason that has nothing
// to do with the question being asked.
function LibKey(p) {
    var value = String(p || '');
    if (/^file:/i.test(value)) {
        value = value.replace(/^file:\/\/\/([a-z]:)/i, '$1').replace(/^file:\/\//i, '\\\\');
        try { value = decodeURIComponent(value); } catch (_) { /* Preserve literal percent names. */ }
    }
    var parts = value.replace(/\//g, '\\').split('\\'), out = [];
    parts.forEach(function (part) {
        if (part === '.') return;
        if (part === '..' && out.length > 1 && out[out.length - 1] !== '..') out.pop();
        else out.push(part);
    });
    return out.join('\\').replace(/\\$/, '').toLowerCase();
}

function LibFindItem(path) {
    var key = LibKey(path), items = typeof PROJECT_LIBRARY !== 'undefined' ? PROJECT_LIBRARY.items || [] : [];
    for (var i = 0; i < items.length; i++) if (LibKey(items[i].path) === key) return items[i];
    return LibSynthRecent(path);
}

function LibThumbnailSource(src) {
    src = typeof src === 'string' ? src : '';
    return /^(?:\.\/)?img\/d\.png(?:[?#]|$)/i.test(src) || /\/homepage\/img\/d\.png(?:[?#]|$)/i.test(src) ? '' : src;
}

function LibThumbnailMetadata(it) {
    var key = LibKey(it.path), fingerprint = it.revision == null ? '' : String(it.revision) + ':' + it.size;
    if (fingerprint && LIB.thumbFingerprints[key] && LIB.thumbFingerprints[key] !== fingerprint) {
        delete LIB.thumbnails[key]; delete LIB.thumbRequested[key];
        delete LIB.thumbRetried[key]; delete LIB.thumbForce[key];
        LIB.thumbVersions[key] = (LIB.thumbVersions[key] || 0) + 1;
    }
    if (fingerprint) LIB.thumbFingerprints[key] = fingerprint;
}

function LibThumbnail(it) {
    var info = LIB.thumbnails[LibKey(it.path)] || it;
    var views = (Array.isArray(info.views) ? info.views : []).filter(function (view) {
        return view && LibThumbnailSource(view.src);
    });
    var thumb = LibThumbnailSource(info.thumb) || (views[0] && views[0].src) || '';
    var state = info.thumbState || (thumb ? 'ready' : 'pending');
    var error = info.thumbError || '';
    if (it.state === 'missing') { state = 'unavailable'; error = 'File unavailable'; }
    if (state === 'ready' && !thumb) state = 'pending';
    return {thumb: thumb, views: views, thumbState: state, thumbError: error};
}

function LibThumbnailContents(it) {
    var info = LibThumbnail(it), key = LibKey(it.path);
    if (info.thumbState === 'ready') {
        return '<img src="' + LibEscape(info.thumb) + '" loading="lazy" alt="Preview of ' + LibEscape(it.name || 'model') +
            '" data-thumb-path="' + LibEscape(it.path) + '" data-thumb-version="' + (LIB.thumbVersions[key] || 0) +
            '" onerror="LibThumbnailError(this)" />';
    }
    var unavailable = info.thumbState === 'unavailable';
    return '<span class="LibThumbnailState' + (unavailable ? ' LibThumbnailUnavailable' : ' LibThumbnailPending') +
        '" role="status">' + (unavailable ? 'Preview unavailable' : 'Generating preview…') +
        (unavailable && info.thumbError ? '<small>' + LibEscape(info.thumbError) + '</small>' : '') + '</span>';
}

function LibThumbnailMarkup(it) {
    return '<div class="LibThumbnailImage" data-thumb-path="' + LibEscape(it.path || '') + '">' +
        LibThumbnailContents(it) + '</div>';
}

// Patch only the image area: arriving previews must not replace focused cards, ticks or scroll.
function LibPatchThumbnail(path) {
    if (!document.querySelectorAll) return;
    var key = LibKey(path), it = LibFindItem(path);
    document.querySelectorAll('.LibThumbnailImage').forEach(function (node) {
        if (LibKey(node.getAttribute('data-thumb-path')) !== key) return;
        node.innerHTML = LibThumbnailContents(it);
        var card = node.closest('.LibCard');
        if (card && typeof SlideRefresh === 'function') SlideRefresh(card);
    });
}

function LibQueueThumbnails() {
    if (!LIB.nativeReady || !document.querySelectorAll) return;
    var candidates = [], seen = {}, items = {};
    LibItems().forEach(function (it) { items[LibKey(it.path)] = it; });
    document.querySelectorAll('.LibThumbnailImage').forEach(function (node) {
        var path = node.getAttribute('data-thumb-path'), key = LibKey(path);
        if (!path || seen[key] || LIB.thumbRequested[key]) return;
        seen[key] = true;
        if (LibThumbnail(items[key] || LibSynthRecent(path)).thumbState !== 'pending') return;
        var box = node.getBoundingClientRect();
        candidates.push({path: path, key: key, visible: box.bottom > 0 && box.top < window.innerHeight && box.width > 0});
    });
    candidates.sort(function (a, b) { return Number(b.visible) - Number(a.visible); });
    [true, false].forEach(function (force) {
        var paths = candidates.filter(function (entry) { return !!LIB.thumbForce[entry.key] === force; }).map(function (entry) {
            LIB.thumbRequested[entry.key] = true;
            return entry.path;
        });
        if (!paths.length) return;
        var message = {command: 'homepage_library_thumbnails', paths: paths};
        if (force) message.force = true;
        SendWXMessage(JSON.stringify(message));
    });
}

function LibThumbnailError(img) {
    img.onerror = null;
    var path = img.getAttribute('data-thumb-path'), key = LibKey(path);
    if (Number(img.getAttribute('data-thumb-version')) !== (LIB.thumbVersions[key] || 0)) return;
    var again = !!LIB.thumbRetried[key];
    LIB.thumbRetried[key] = true;
    LIB.thumbVersions[key] = (LIB.thumbVersions[key] || 0) + 1;
    LIB.thumbnails[key] = {thumb: '', views: [], thumbState: again ? 'unavailable' : 'pending',
        thumbError: again ? 'The preview image could not be loaded' : ''};
    if (!again) { LIB.thumbForce[key] = true; delete LIB.thumbRequested[key]; }
    LibPatchThumbnail(path);
    LibQueueThumbnails();
}

function LibThumbnailReceive(update) {
    if (!update || !update.path) return;
    var key = LibKey(update.path), it = LibFindItem(update.path);
    if (it.revision != null && update.revision != null &&
        (String(it.revision) !== String(update.revision) || Number(it.size) !== Number(update.size))) return;
    LIB.thumbnails[key] = Object.assign({}, LIB.thumbnails[key] || it, update);
    LIB.thumbRequested[key] = true;
    LIB.thumbVersions[key] = (LIB.thumbVersions[key] || 0) + 1;
    delete LIB.thumbForce[key];
    if (update.revision != null) LIB.thumbFingerprints[key] = String(update.revision) + ':' + update.size;
    LibPatchThumbnail(update.path);
}

// A project that is genuinely recent but sits outside the indexed roots has no record to
// find, and answering "recently opened" with silence is worse than answering it with a card
// that says where the file actually is. One is synthesised so this view can never be
// emptier than the truth.
function LibSynthRecent(path) {
    var norm = String(path).replace(/\//g, '\\');
    var cut  = norm.lastIndexOf('\\');
    var file = cut < 0 ? norm : norm.substr(cut + 1);
    var recent = LIB.recentRecords[LibKey(path)] || {};
    var missing = recent.missing === true || recent.missing === 'true';
    return {
        path: norm,
        name: recent.project_name || file.replace(/\.[^.]+$/, ''),
        root: '', group: '(not in the library)', folder: cut < 0 ? '' : norm.substr(0, cut),
        mtime: 0, sizeMB: 0, kind: (file.split('.').pop() || '').toLowerCase(), gcode: 0,
        plateCount: 0, machines: [], perPlateMachines: false,
        printer: '', process: '', colours: [], materials: [], filaments: [],
        state: missing ? 'missing' : 'unindexed', title: '', designer: '', parts: 0,
        note: missing ? 'Reconnect the drive or add the folder where this file now lives.' : '',
        thumb: recent.image || recent.thumb || '', views: recent.views || [], thumbState: recent.thumbState,
        thumbError: recent.thumbError, revision: recent.revision, size: recent.size,
        stage: '', superseded: false, unindexed: true
    };
}

function LibMatches(it) {
    if (LIB.shelf !== 'all' && LibShelfFor(it) !== LIB.shelf) return false;
    if (LIB.folderScope && !LibWithin(it.path, LIB.folderScope)) return false;
    if (LIB.kind === '3mf' && it.kind !== '3mf') return false;
    if (LIB.root !== 'all' && LibKey(it.root) !== LibKey(LIB.root)) return false;
    if (LIB.state === 'recent') {
        if (!LIB.recentPaths[LibKey(it.path)]) return false;
    } else if (LIB.state !== 'all' && it.state !== LIB.state) {
        return false;
    }
    if (LIB.group !== 'all' && it.group !== LIB.group) return false;
    if (LIB.search) {
        var hay = [it.name, it.title, it.group, it.folder, it.path, it.designer,
                   (it.machines || []).join(' '), (it.materials || []).join(' ')].join(' ').toLowerCase();
        var words = LIB.search.toLowerCase().split(/\s+/);
        for (var i = 0; i < words.length; i++) {
            if (words[i] && hay.indexOf(words[i]) < 0) return false;
        }
    }
    return true;
}

function LibFiltered() {
    var out = LibItems().filter(LibMatches);
    // In the recent view, every real recent gets a card - indexed or not. Sorted by the
    // recent list's own order, which is the order the question "what was I just working on"
    // is actually asking about; mtime cannot answer it for a synthesised record.
    if (LIB.state === 'recent') {
        var i, rank = {};
        for (i = 0; i < LIB.recentOrder.length; i++) rank[LibKey(LIB.recentOrder[i])] = i;
        out.sort(function (a, b) {
            var ra = rank[LibKey(a.path)], rb = rank[LibKey(b.path)];
            if (ra === undefined) ra = 1e9;
            if (rb === undefined) rb = 1e9;
            return ra - rb;
        });
        return out;
    }
    if (LIB.sort === 'name') {
        out.sort(function (a, b) { return a.name.localeCompare(b.name); });
    } else {
        out.sort(function (a, b) { return b.mtime - a.mtime; });
    }
    return out;
}

function LibDate(ts) {
    if (!ts) return '';
    var d = new Date(ts * 1000);
    function p(n) { return (n < 10 ? '0' : '') + n; }
    return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate());
}

function LibStop(ev) {
    ev = ev || window.event;
    if (ev && ev.stopPropagation) ev.stopPropagation();
    if (ev) ev.cancelBubble = true;
}

// ---------------------------------------------------------------------------------------
// Filaments
//
// What comes off the warehouse shelf is a spool, and a spool is chosen by material and
// colour. The profile name is what distinguishes two spools that share both, so it is
// carried alongside rather than folded into the key: matte and basic black are one trip to
// the same shelf bay and two different boxes.

// A slot the project declares but never prints is not stock. The archive records one as a
// fully transparent colour, and counting it puts a spool on the list no plate asks for.
function LibFilaments(it) {
    var out = [], f = it.filaments || [], i;
    for (i = 0; i < f.length; i++) {
        if (f[i].used && /^#[0-9a-f]{6}/i.test(f[i].colour || '')) out.push(f[i]);
    }
    return out;
}

function LibHex(c) {
    return String(c || '').substr(0, 7).toUpperCase();
}

// Grams are read off a shelf label and compared against a spool, so tenths matter only
// where the number is small enough for a tenth to be a real share of it.
function LibG(n) {
    return n >= 10 ? String(Math.round(n)) : (Math.round(n * 10) / 10).toFixed(1);
}

// Spools are the unit actually carried off the shelf, but only once more than one is
// needed. Below a kilo the count is always one, and printing it turns the number that
// matters into noise.
function LibSpools(g) {
    return g > 1000 ? Math.ceil(g / 1000) : 0;
}

// A hex is exact and unreadable. The pickup text has to survive being pasted into a stock
// declaration and read by someone standing at a rack, so every colour gets a plain name
// derived from it rather than a lookup table that would go stale the first time a vendor
// ships a new shade.
function LibColourName(hex) {
    var r = parseInt(hex.substr(1, 2), 16) / 255,
        g = parseInt(hex.substr(3, 2), 16) / 255,
        b = parseInt(hex.substr(5, 2), 16) / 255;
    if (isNaN(r) || isNaN(g) || isNaN(b)) return 'unknown';
    var mx = Math.max(r, g, b), mn = Math.min(r, g, b), d = mx - mn, l = (mx + mn) / 2;
    if (d < 0.09) {
        return l > 0.9 ? 'white' : l > 0.65 ? 'light grey'
             : l > 0.3 ? 'grey' : l > 0.12 ? 'dark grey' : 'black';
    }
    var h = mx === r ? 60 * (((g - b) / d) % 6)
          : mx === g ? 60 * ((b - r) / d + 2)
                     : 60 * ((r - g) / d + 4);
    if (h < 0) h += 360;
    var name = h < 15 ? 'red' : h < 40 ? 'orange' : h < 68 ? 'yellow'
             : h < 100 ? 'lime' : h < 160 ? 'green' : h < 195 ? 'teal'
             : h < 250 ? 'blue' : h < 290 ? 'purple' : h < 335 ? 'magenta' : 'red';
    // Brown is not a hue. It is a dark orange, and calling it orange points at the wrong
    // shelf.
    if (h < 45 && l < 0.4) name = 'brown';
    return (l > 0.78 ? 'light ' : l < 0.25 ? 'dark ' : '') + name;
}

function LibSwatches(cols) {
    if (!cols || !cols.length) return '';
    var h = '';
    for (var i = 0; i < cols.length && i < 8; i++) {
        var c = cols[i];
        // A fully transparent slot is a declared-but-unused filament, not a colour.
        if (!/^#[0-9a-f]{6}/i.test(c)) continue;
        h += '<i class="LibSwatch" style="background:' + LibEscape(c.substr(0, 7)) + '"></i>';
    }
    return h;
}

// The swatch row doubles as the control that opens the detail, so the thing you want to
// know more about is the thing you click. It must swallow the click: the card underneath
// opens the project in the slicer, which is a slow and unwanted answer to "what colour is
// that".
function LibFilamentStrip(it) {
    var fl = LibFilaments(it);
    if (!fl.length) return LibSwatches(it.colours);
    var h = '', i;
    for (i = 0; i < fl.length && i < 8; i++) {
        h += '<i class="LibSwatch" style="background:' +
             LibEscape(LibHex(fl[i].colour)) + '"></i>';
    }
    return '<span class="LibSwatchRow" onClick="LibToggleFilaments(event, this)">' + h +
           '<b class="LibCaret">' + (LIB.expanded[it.path] ? '&#9652;' : '&#9662;') +
           '</b></span>';
}

function LibFilamentRows(it) {
    var fl = LibFilaments(it);
    if (!fl.length || !LIB.expanded[it.path]) return '';
    var h = '<div class="LibFilRows" onClick="LibStop(event)">', i, f, hex;
    for (i = 0; i < fl.length; i++) {
        f = fl[i];
        hex = LibHex(f.colour);
        h += '<div class="LibFilRow">' +
               '<i class="LibSwatch" style="background:' + LibEscape(hex) + '"></i>' +
               '<span class="LibFilHex">' + LibEscape(hex) + '</span>' +
               '<span class="LibFilType">' + LibEscape(f.type || '?') + '</span>' +
               (f.plate ? '<span>Plate ' + f.plate + ' · slot ' + f.slot + '</span>' : '') +
               // Grams cannot be invented. An arranged-but-unsliced project has none, and
               // printing a zero there would read as "this uses no filament".
               (typeof f.grams === 'number'
                    ? '<span class="LibFilG">' + LibG(f.grams) + ' g</span>'
                    : '<span class="LibFilG LibFilNone">not weighed</span>') +
               '<span class="LibFilProfile">' +
                 LibEscape(f.profile || f.vendor || '') + '</span>' +
             '</div>';
    }
    return h + '</div>';
}

function LibToggleFilaments(ev, el) {
    LibStop(ev);
    var p = $(el).closest('.LibCard').attr('fpath');
    if (!p) return;
    if (LIB.expanded[p]) delete LIB.expanded[p]; else LIB.expanded[p] = true;
    LibRender();
}

function LibCard(it) {
    var badge = LIB_BADGE[it.state] || LIB_BADGE['mesh'];

    var facts = [];
    if (it.plateCount > 1) facts.push(it.plateCount + ' plates');
    if (it.parts) facts.push(it.parts + (it.parts === 1 ? ' part' : ' parts'));
    if (it.hours) facts.push(it.hours.toFixed(1) + ' h');
    if (it.grams) facts.push(Math.round(it.grams) + ' g');
    if (!it.hours && it.kind === 'stl') facts.push(it.sizeMB + ' MB');

    var tags = '';
    if (it.makerworld) tags += '<span class="LibTag">MakerWorld</span>';
    if (it.handPrepared) tags += '<span class="LibTag">hand-prepared</span>';
    if (it.kind === 'stl') tags += '<span class="LibTag">STL</span>';
    // The machines a project delegates to are the fastest read on the card: it says at a
    // glance whether this is a one-printer job or the whole farm.
    if (it.machines && it.machines.length) {
        tags += '<span class="LibTag LibTagMachine">' + LibEscape(
            it.machines.join(' / ')) + '</span>';
    }
    if (it.materials && it.materials.length &&
        !(it.materials.length === 1 && it.materials[0] === 'PLA')) {
        tags += '<span class="LibTag LibTagWarn">' +
                LibEscape(it.materials.join('/')) + '</span>';
    }

    var picked = !!LIB.picked[it.path];
    // Only a project with filaments can contribute to a pickup list, so only those offer
    // a tick. A checkbox on a raw mesh would promise a total it can never add to.
    var tick = LibFilaments(it).length
        ? '<span class="LibPick' + (picked ? ' LibPickOn' : '') +
          '" onClick="LibTogglePick(event, this)" title="Add to pickup list">' +
          (picked ? '&#10003;' : '') + '</span>'
        : '';

    return '<div class="LibCard' + (picked ? ' LibCardPicked' : '') +
             '" fpath="' + LibEscape(it.path) + '" tabindex="0" role="button"' +
             ' aria-label="Open ' + LibEscape(it.name) + '" title="' + LibEscape(it.path) +
             '" onkeydown="LibCardKey(event, this)" onClick="LibOpen(this)">' +
             '<div class="LibThumb">' + LibThumbnailMarkup(it) +
               '<span class="LibBadge ' + badge.cls + '">' + badge.text + '</span>' +
               tick +
             '</div>' +
             '<div class="LibName TextS1">' + LibEscape(it.name) + '</div>' +
             '<div class="LibMeta">' + LibEscape(it.group) + ' &middot; ' +
                LibDate(it.mtime) + '</div>' +
             '<div class="LibPath">' + LibEscape(it.folder) + '</div>' +
             '<div class="LibMeta LibFacts">' + LibFilamentStrip(it) +
                (facts.length ? '<span>' + facts.join(' &middot; ') + '</span>' : '') +
             '</div>' +
             LibFilamentRows(it) +
             (tags ? '<div class="LibTags">' + tags + '</div>' : '') +
             (it.note ? '<div class="LibProblem">' + LibEscape(it.note) + '</div>' : '') +
             '<button class="LibFolderButton" onclick="LibReveal(event, this)">Open folder</button>' +
             (it.folder && LibShelfFor(it) !== 'mine' ? ' <button class="LibFolderButton" data-path="' +
                LibEscape(it.folder) + '" onclick="LibStop(event); LibPinDesign(this)">Add folder to My projects</button>' : '') +
             (LIB.recentPaths[LibKey(it.path)] ?
                ' <button class="LibFolderButton" onclick="LibForgetRecent(event, this)">Remove from recent</button>' : '') +
           '</div>';
}

function LibRender() {
    if (LibWorkspaceRender()) { LibQueueThumbnails(); return; }
    var all = LibFiltered();
    var page = all.slice(0, LIB.shown);
    var html = '';
    for (var i = 0; i < page.length; i++) html += LibCard(page[i]);
    if (!page.length) {
        html = '<div class="LibEmpty">' + (LibItems().length ?
            'Nothing matches these filters. <button class="LibFolderButton" onclick="LibResetFilters()">Reset filters</button>' :
            'Add a folder to find your projects, or open a project to start a recent list.') + '</div>';
    }
    $('#LibGrid').html(html);

    if (all.length > page.length) {
        $('#LibMore').show().text('Show ' +
            Math.min(LIB_PAGE, all.length - page.length) + ' more of ' +
            (all.length - page.length));
    } else {
        $('#LibMore').hide();
    }
    $('#LibCount').text(all.length + (all.length === 1 ? ' file' : ' files'));
    LibPickupSync();
    LibQueueThumbnails();
}

// ---------------------------------------------------------------------------------------
// Pickup list
//
// The whole point of the panel: one trip to the warehouse instead of one per project. It
// totals across projects, so it must never quietly turn an unknown into a zero. A total
// that silently understates is worse than no total, because it is the number that gets
// declared and then found short halfway through a print run.

// Ticks win over the filter when there are any, because ticking is the more specific
// statement. With none, the filter is the statement, and the panel says which it used.
function LibPickupSource() {
    var picked = [], items = LibItems(), i;
    for (i = 0; i < items.length; i++) {
        if (LIB.picked[items[i].path]) picked.push(items[i]);
    }
    if (picked.length) {
        picked.sort(function (a, b) { return a.name.localeCompare(b.name); });
        return { items: picked, from: 'ticked' };
    }
    return { items: LibFiltered(), from: 'filter' };
}

// Material and colour decide which spool comes off the shelf; the profile decides which
// box on that shelf. Grouping on the first and listing the second keeps one line per trip
// without losing the distinction between matte and basic.
function LibPickupGroups(items) {
    var groups = [], byKey = {}, i, j, fl, f, hex, key, grp, pr;
    for (i = 0; i < items.length; i++) {
        fl = LibFilaments(items[i]);
        for (j = 0; j < fl.length; j++) {
            f = fl[j];
            hex = LibHex(f.colour);
            key = (f.type || '?') + ' ' + hex;
            grp = byKey[key];
            if (!grp) {
                grp = byKey[key] = {
                    key: key, hex: hex, type: f.type || '?', name: LibColourName(hex),
                    grams: 0, profiles: [], projects: [], byPath: {}, profileSeen: {}
                };
                groups.push(grp);
            }
            if (f.profile && !grp.profileSeen[f.profile]) {
                grp.profileSeen[f.profile] = true;
                grp.profiles.push(f.profile);
            }
            pr = grp.byPath[items[i].path];
            if (!pr) {
                pr = grp.byPath[items[i].path] =
                    { name: items[i].name, group: items[i].group, grams: 0, known: false };
                grp.projects.push(pr);
            }
            // Slots without a measurement are counted as projects, never as grams. That is
            // what stops the total from reading as complete when it is a floor.
            if (typeof f.grams === 'number') {
                grp.grams += f.grams;
                pr.grams += f.grams;
                pr.known = true;
            }
        }
    }
    for (i = 0; i < groups.length; i++) {
        grp = groups[i];
        grp.unweighed = 0;
        for (j = 0; j < grp.projects.length; j++) {
            if (!grp.projects[j].known) grp.unweighed++;
        }
        grp.projects.sort(function (a, b) { return b.grams - a.grams; });
    }
    // Heaviest first, so that a list read only from the top is still the useful part of
    // the trip.
    groups.sort(function (a, b) {
        return (b.grams - a.grams) || (b.projects.length - a.projects.length) ||
               a.key.localeCompare(b.key);
    });
    return groups;
}

// One pass, one set of numbers. Counting twice, once for the panel and once for the text
// pasted into a declaration, is how the two come to disagree.
//
// The gap is counted in slots rather than in projects. A project whose first slot was
// measured and whose other four were not is a weighed project by any per-project count,
// so such a count reports a complete total over four missing numbers. The slot is the
// thing that has or has not been measured, so it is the thing that gets counted.
function LibPickupData() {
    var src = LibPickupSource();
    var d = { items: src.items, from: src.from, groups: LibPickupGroups(src.items),
              total: 0, nProjects: 0, slots: 0, unweighedSlots: 0, unweighedProjects: 0 };
    var i, j, fl, any;
    for (i = 0; i < d.groups.length; i++) d.total += d.groups[i].grams;
    for (i = 0; i < d.items.length; i++) {
        fl = LibFilaments(d.items[i]);
        if (!fl.length) continue;
        d.nProjects++;
        any = false;
        for (j = 0; j < fl.length; j++) {
            d.slots++;
            if (typeof fl[j].grams === 'number') any = true; else d.unweighedSlots++;
        }
        if (!any) d.unweighedProjects++;
    }
    return d;
}

function LibPickupText(d) {
    var lines = [], i, j, grp, known, unknown, groups = d.groups;

    lines.push('FILAMENT PICKUP LIST');
    lines.push(d.nProjects + (d.nProjects === 1 ? ' project' : ' projects') +
               (d.from === 'ticked' ? ', ticked by hand'
                                    : ', everything matching the library filter'));
    lines.push('');

    for (i = 0; i < groups.length; i++) {
        grp = groups[i];
        known = [];
        unknown = [];
        for (j = 0; j < grp.projects.length; j++) {
            if (grp.projects[j].known) {
                known.push(grp.projects[j].name + ' ' + LibG(grp.projects[j].grams) + ' g');
            } else {
                unknown.push(grp.projects[j].name);
            }
        }
        lines.push(grp.name + ' ' + grp.type + '  ' + grp.hex + '  ' +
                   (grp.grams ? LibG(grp.grams) + ' g' : 'no weights yet') +
                   (LibSpools(grp.grams)
                        ? '  (at least ' + LibSpools(grp.grams) + ' x 1 kg spools)' : ''));
        if (grp.profiles.length) lines.push('  profile: ' + grp.profiles.join(', '));
        if (known.length) lines.push('  weighed: ' + known.join('; '));
        if (unknown.length) lines.push('  not weighed: ' + unknown.join('; '));
        lines.push('');
    }

    if (!groups.length) lines.push('Nothing selected carries filament data.');
    lines.push('TOTAL WEIGHED: ' + LibG(d.total) + ' g across ' + groups.length +
               (groups.length === 1 ? ' filament' : ' filaments'));
    if (d.unweighedSlots) {
        lines.push('NOT WEIGHED: ' + d.unweighedSlots + ' of ' + d.slots +
                   ' filament slots carry no measured weight' +
                   (d.unweighedProjects ? ' (' + d.unweighedProjects +
                    ' projects have none at all)' : '') +
                   ', so the real total is higher than this.');
    }
    return lines.join('\n');
}

function LibPickupRender() {
    var d = LibPickupData();
    var groups = d.groups, total = d.total, nProjects = d.nProjects;
    var i, j, grp;

    var h = '<div id="LibPickHead">' +
              '<b>Pickup list</b>' +
              '<span class="LibPickFrom">' + nProjects +
                (nProjects === 1 ? ' project' : ' projects') + ' &middot; ' +
                (d.from === 'ticked' ? 'ticked by hand'
                                     : 'everything matching the filter') + '</span>' +
              '<span class="LibPickSpacer"></span>' +
              '<div class="LibChip" onClick="LibPickAllShown()">Tick all shown</div>' +
              '<div class="LibChip" onClick="LibClearPicks()">Clear ticks</div>' +
              '<div class="LibChip" onClick="LibPickupCopy()">Copy as text</div>' +
              '<div class="LibChip" onClick="LibTogglePickup()">Close</div>' +
            '</div>';

    h += '<div id="LibPickTotal">' +
           '<b>' + LibG(total) + ' g</b> weighed across ' + groups.length +
           (groups.length === 1 ? ' filament' : ' filaments') +
           (d.unweighedSlots
              ? '<span class="LibPickWarn">' + d.unweighedSlots + ' of ' + d.slots +
                ' filament slots have no weight yet, so this is a floor</span>' : '') +
         '</div>';

    if (!groups.length) {
        h += '<div class="LibEmpty">Nothing selected carries filament data. Tick a ' +
             'project, or filter to prepped and sliced work.</div>';
    }

    for (i = 0; i < groups.length; i++) {
        grp = groups[i];
        var parts = [];
        for (j = 0; j < grp.projects.length; j++) {
            parts.push('<span class="' + (grp.projects[j].known ? '' : 'LibFilNone') +
                       '">' + LibEscape(grp.projects[j].name) +
                       (grp.projects[j].known
                            ? ' ' + LibG(grp.projects[j].grams) + ' g'
                            : ' (not weighed)') + '</span>');
        }
        h += '<div class="LibPickRow">' +
               '<i class="LibSwatchBig" style="background:' + LibEscape(grp.hex) +
                 '"></i>' +
               '<div class="LibPickMain">' +
                 '<div class="LibPickTitle">' + LibEscape(grp.name) + ' ' +
                   LibEscape(grp.type) +
                   '<span class="LibPickHex">' + LibEscape(grp.hex) + '</span></div>' +
                 (grp.profiles.length
                    ? '<div class="LibPickSub">' +
                      LibEscape(grp.profiles.join('  ·  ')) + '</div>' : '') +
                 '<div class="LibPickSub">' + parts.join(' &middot; ') + '</div>' +
               '</div>' +
               '<div class="LibPickAmt">' +
                 (grp.grams ? '<b>' + LibG(grp.grams) + ' g</b>' +
                              (LibSpools(grp.grams)
                                 ? '<span>at least ' + LibSpools(grp.grams) +
                                   ' &times; 1 kg</span>' : '')
                            : '<b class="LibFilNone">no weights</b>') +
                 (grp.unweighed ? '<span class="LibPickWarn">' + grp.unweighed +
                                  ' not weighed</span>' : '') +
               '</div>' +
             '</div>';
    }

    // The text is always on screen, not only behind the copy button. A webview can refuse
    // a clipboard write, and a pickup list that cannot leave the window is no use at the
    // warehouse door.
    h += '<textarea id="LibPickText" readonly onClick="this.select()"></textarea>';

    $('#LibPickup').html(h).show();
    $('#LibPickText').val(LibPickupText(d));
}

function LibPickupSync() {
    var n = 0, k;
    for (k in LIB.picked) { if (LIB.picked.hasOwnProperty(k)) n++; }
    $('#LibPickupChip').text('Pickup list' + (n ? ' (' + n + ')' : ''))
                       .toggleClass('LibChipOn', LIB.pickupOpen);
    if (LIB.pickupOpen) LibPickupRender();
}

function LibTogglePickup() {
    LIB.pickupOpen = !LIB.pickupOpen;
    if (!LIB.pickupOpen) $('#LibPickup').hide().empty();
    LibPickupSync();
}

function LibTogglePick(ev, el) {
    LibStop(ev);
    var p = $(el).closest('.LibCard').attr('fpath');
    if (!p) return;
    if (LIB.picked[p]) delete LIB.picked[p]; else LIB.picked[p] = true;
    LibRender();
}

function LibPickAllShown() {
    var all = LibFiltered(), i;
    for (i = 0; i < all.length && i < LIB.shown; i++) {
        if (LibFilaments(all[i]).length) LIB.picked[all[i].path] = true;
    }
    LibRender();
}

function LibClearPicks() { LIB.picked = {}; LibRender(); }

function LibCopyLabel(text) {
    $('#LibPickup .LibChip').filter(function () {
        return /^(Copy|Select)/.test($(this).text());
    }).text(text);
}

// Two ways to reach a clipboard and neither is guaranteed here: the async API needs a
// secure context and a focused document, and an embedded webview grants neither reliably.
// The label is only allowed to claim success once a path has actually reported it, because
// a button that says Copied over an empty clipboard sends the trip out with nothing.
function LibPickupCopy() {
    var el = document.getElementById('LibPickText');
    if (!el) return;
    el.focus();
    el.select();
    var ok = false;
    try { ok = document.execCommand('copy'); } catch (e) { ok = false; }
    if (ok) { LibCopyLabel('Copied'); return; }
    if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(el.value).then(
            function () { LibCopyLabel('Copied'); },
            function () { LibCopyLabel('Select the text below'); });
        return;
    }
    LibCopyLabel('Select the text below');
}

function LibSetState(id) {
    LIB.state = id;
    // The raw-mesh category must be able to show the files it names.
    if (id === 'mesh') {
        LIB.kind = 'all';
        $('#LibKindChip').addClass('LibChipOn');
    }
    LIB.shown = LIB_PAGE;
    LibBuildChips();
    LibBuildGroups();
    LibRender();
}

function LibToggleKind() {
    LIB.kind = (LIB.kind === '3mf') ? 'all' : '3mf';
    LIB.shown = LIB_PAGE;
    $('#LibKindChip').toggleClass('LibChipOn', LIB.kind === 'all');
    // Chip and group counts are counts of the visible set, so including STL changes both.
    // Leaving them stale would show "85 raw mesh" over a grid holding 1315.
    LibBuildChips();
    LibBuildGroups();
    $('#LibGroup').val(LIB.group);
    LibRender();
}

function LibSetGroup(v) { LIB.group = v; LIB.shown = LIB_PAGE; LibBuildChips(); LibRender(); }
function LibSetRoot(v) { LIB.root = v; LIB.group = 'all'; LibBuildGroups(); LibBuildChips(); LibRender(); }
function LibSetSort(v) { LIB.sort = v; LibRender(); }
function LibSearch(v) { LIB.search = v; LIB.shown = LIB_PAGE; LibBuildChips(); LibRender(); }
function LibMore() { LIB.shown += LIB_PAGE; LibRender(); }

function LibOpen(el) {
    var p = $(el).attr('fpath');
    if (p) OnOpenRecentFile(encodeURI(p));
}

function LibCardKey(ev, el) {
    if (ev.target !== el || (ev.key !== 'Enter' && ev.key !== ' ')) return;
    ev.preventDefault();
    LibOpen(el);
}

function LibReveal(ev, el) {
    LibStop(ev);
    var path = $(el).closest('.LibCard').attr('fpath');
    if (path) SendWXMessage(JSON.stringify({command: 'homepage_explore_recentfile', data: {path: path}}));
}

function LibForgetRecent(ev, el) {
    LibStop(ev);
    var path = $(el).closest('.LibCard').attr('fpath');
    if (path) SendWXMessage(JSON.stringify({command: 'homepage_delete_recentfile', data: {path: path}}));
}

function LibResetFilters() {
    LIB.state = 'all'; LIB.root = 'all'; LIB.group = 'all'; LIB.search = ''; LIB.kind = 'all';
    LIB.shown = LIB_PAGE;
    $('#LibSearch').val(''); $('#LibRoot').val('all'); $('#LibKindChip').addClass('LibChipOn');
    LibBuildGroups(); LibBuildChips(); LibRender();
}

// Text fields own their keyboard shortcuts. Forwarding Ctrl+A or suppressing every
// keydown here made search impossible even after the native focus fix.
function LibKeyboard(event) {
    var target = event.target;
    if (target && (/^(INPUT|TEXTAREA|SELECT)$/.test(target.tagName) || target.isContentEditable)) return;
    if (event.ctrlKey || event.metaKey) {
        OutputKey(event.keyCode, !!event.ctrlKey, !!event.shiftKey, !!event.metaKey);
        event.preventDefault();
    }
}

// Counts sit on the chips because the useful question is usually "how much of the
// backlog is in this state", and that should not need a click to answer.
function LibBuildChips() {
    var counts = {};
    var items = LibItems();
    var state = LIB.state;
    LIB.state = 'all';
    var recentCount = 0, allCount = 0;
    for (var i = 0; i < items.length; i++) {
        if (!LibMatches(items[i])) continue;
        allCount++;
        counts[items[i].state] = (counts[items[i].state] || 0) + 1;
        if (LIB.recentPaths[LibKey(items[i].path)]) recentCount++;
    }
    var kind = LIB.kind;
    LIB.kind = 'all'; LIB.state = 'mesh';
    counts.mesh = items.filter(LibMatches).length;
    LIB.kind = kind;
    LIB.state = state;
    var html = '';
    for (var s = 0; s < LIB_STATES.length; s++) {
        var st = LIB_STATES[s];
        var n = st.id === 'all' ? allCount
              : (st.id === 'recent' ? recentCount : (counts[st.id] || 0));
        if (n === 0 && st.id !== 'all' && st.id !== 'recent' && st.id !== LIB.state && st.id !== 'mesh') continue;
        html += '<button class="LibChip' + (st.id === LIB.state ? ' LibChipOn' : '') +
                '" cstate="' + st.id + '" onClick="LibSetState(\'' + st.id + '\')">' +
                st.label + ' <b>' + n + '</b></button>';
    }
    $('#LibChips').html(html);
}

function LibBuildGroups() {
    var groups = Object.create(null);
    var items = LibItems();
    for (var i = 0; i < items.length; i++) {
        if (LIB.shelf !== 'all' && LibShelfFor(items[i]) !== LIB.shelf) continue;
        if (LIB.folderScope && !LibWithin(items[i].path, LIB.folderScope)) continue;
        if (LIB.kind === '3mf' && items[i].kind !== '3mf') continue;
        if (LIB.root !== 'all' && LibKey(items[i].root) !== LibKey(LIB.root)) continue;
        groups[items[i].group] = (groups[items[i].group] || 0) + 1;
    }
    var keys = Object.keys(groups).sort();
    var html = '<option value="all">All projects</option>';
    for (var k = 0; k < keys.length; k++) {
        html += '<option value="' + LibEscape(keys[k]) + '">' +
                LibEscape(keys[k]) + ' (' + groups[keys[k]] + ')</option>';
    }
    $('#LibGroup').html(html);
    if (LIB.group !== 'all' && !groups[LIB.group]) LIB.group = 'all';
    $('#LibGroup').val(LIB.group);
}

// The real recent list still arrives from C++. It is kept as a filter rather than a
// separate view, so there is one grid and one set of controls.
function LibNoteRecent(pList) {
    LIB.recentPaths = {};
    LIB.recentOrder = [];
    LIB.recentRecords = {};
    for (var i = 0; i < (pList || []).length; i++) {
        if (pList[i] && pList[i].path) {
            var key = LibKey(pList[i].path);
            LibThumbnailMetadata(pList[i]);
            if (!LIB.recentPaths[key]) LIB.recentOrder.push(pList[i].path);
            LIB.recentPaths[key] = true;
            LIB.recentRecords[key] = pList[i];
        }
    }
    // The chip's count is only honest once the list has arrived, and it arrives after the
    // first render, so the chips are rebuilt rather than left showing the boot-time answer.
    LibBuildChips();
    LibBuildGroups();
    LibRender();
}

function LibInit() {
    if ((LIB_WORKSPACE.mine || []).length) {
        LIB.shelf = 'mine';
        try { window.localStorage.setItem('podslicer.library.workspace', JSON.stringify(LIB_WORKSPACE)); } catch (_) {}
    }
    LibBuildChips();
    LibBuildGroups();
    LibRender();
    LibRefresh();
}

function LibBusy(busy) {
    $('#LibRefresh').prop('disabled', busy).text(busy ? 'Scanning folders…' : 'Refresh');
    $('#LibStatus').text(busy ? 'Checking files and project metadata…' : '');
}

function LibRefresh() {
    LibBusy(true);
    SendWXMessage(JSON.stringify({command: 'homepage_library_refresh',
        roots: typeof PROJECT_LIBRARY !== 'undefined' ? PROJECT_LIBRARY.roots || [] : []}));
}

function LibAddFolder() {
    SendWXMessage(JSON.stringify({command: 'homepage_library_add_folder'}));
}

function LibRemoveFolder(el) {
    SendWXMessage(JSON.stringify({command: 'homepage_library_remove_folder', path: el.getAttribute('data-path')}));
}

function LibReceive(data) {
    LibBusy(false);
    if (data.error) {
        $('#LibStatus').text('Could not refresh the library: ' + data.error);
        return;
    }
    (data.items || []).forEach(LibThumbnailMetadata);
    PROJECT_LIBRARY = data;
    LIB.nativeReady = true;
    var roots = data.roots || [];
    if (roots.indexOf(LIB.root) < 0) LIB.root = 'all';
    var options = '<option value="all">All folders</option>';
    roots.forEach(function (root) { options += '<option value="' + LibEscape(root) + '">' + LibEscape(root) + '</option>'; });
    $('#LibRoot').html(options).val(LIB.root);
    var folders = '';
    (data.rootStatus || []).forEach(function (status) {
        folders += '<div class="LibRootRow"><span>' + LibEscape(status.path) + '</span><span class="' +
            (status.error ? 'LibProblem' : '') + '">' + LibEscape(status.error || status.count + ' files') + '</span>' +
            '<button class="LibFolderButton" data-path="' + LibEscape(status.path) +
            '" onclick="LibPinDesign(this)">My project</button>' +
            '<button class="LibFolderButton" data-path="' + LibEscape(status.path) +
            '" onclick="LibRemoveFolder(this)" title="Remove from library; files stay on disk">Remove folder</button></div>';
    });
    $('#LibFoldersList').html(folders || 'No folders added yet.');
    $('#LibStatus').text('Updated ' + new Date((data.generated || 0) * 1000).toLocaleString() + '. Files stay in their original folders.');
    LibBuildGroups(); LibBuildChips(); LibRender();
}
