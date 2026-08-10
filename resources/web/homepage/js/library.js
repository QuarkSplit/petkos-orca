// Project library for the home page.
//
// Replaces "recently opened", which sorted by the one fact that does not help: when a file
// was last touched. What matters before opening a project is what state it is in, because
// that is what decides the next action. A raw mesh needs orientation and colour. A
// MakerWorld project still carrying its Bambu printer needs re-targeting before it will
// slice for this farm at all. A prepped job needs slicing. A sliced one needs sending.
//
// The index is built by E:\3D-Printing\Scripts\build_library.py and loaded as a script,
// because the page cannot fetch a file:// path. Opening reuses the existing
// homepage_open_recentfile bridge, which takes any path, so this needs no C++ change.

var LIB_PAGE = 120;

var LIB = {
    state: 'all',
    group: 'all',
    search: '',
    kind: '3mf',      // most of the library is loose STL; the pipeline is 3MF
    // One card per model by default. A model that has become a project does not also need
    // the raw download it came from, and sliced output is not a thing you open to work on.
    versions: 'best',
    sort: 'recent',
    shown: LIB_PAGE,
    recentPaths: {},  // filled from the real recent-files list, for the Recent chip
    // Expansion and ticks are keyed by path rather than held in the DOM, because the grid
    // is re-rendered whole on every filter change and anything living only in the markup
    // would silently reset mid-selection.
    expanded: {},
    picked: {},
    pickupOpen: false
};

// Order matters: it is the pipeline, and the chips read as a progression.
var LIB_STATES = [
    { id: 'all',          label: 'All' },
    { id: 'recent',       label: 'Recently opened' },
    { id: 'mesh',         label: 'Raw mesh' },
    { id: 'foreign',      label: 'Bambu preset' },
    { id: 'needs-colour', label: 'Needs colour' },
    { id: 'prepped',      label: 'Prepped' },
    { id: 'sliced',       label: 'Sliced' },
    // Listed so a corrupt archive is findable. Without a chip it appears only under All,
    // which is the one view nobody scans.
    { id: 'unreadable',   label: 'Broken' }
];

var LIB_BADGE = {
    'mesh':         { text: 'raw mesh',    cls: 'LibBadgeMesh' },
    'foreign':      { text: 'bambu preset', cls: 'LibBadgeForeign' },
    'needs-colour': { text: 'needs colour', cls: 'LibBadgeColour' },
    'prepped':      { text: 'prepped',     cls: 'LibBadgePrepped' },
    'sliced':       { text: 'sliced',      cls: 'LibBadgeSliced' },
    'sent':         { text: 'sent',        cls: 'LibBadgeSent' },
    'unreadable':   { text: 'unreadable',  cls: 'LibBadgeBad' }
};

function LibItems() {
    return (typeof PROJECT_LIBRARY !== 'undefined' && PROJECT_LIBRARY.items) || [];
}

function LibEscape(s) {
    return String(s == null ? '' : s)
        .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;');
}

function LibMatches(it) {
    if (LIB.kind === '3mf' && it.kind !== '3mf') return false;
    if (LIB.versions === 'best' && it.superseded) return false;
    if (LIB.state === 'recent') {
        if (!LIB.recentPaths[it.path]) return false;
    } else if (LIB.state !== 'all' && it.state !== LIB.state) {
        return false;
    }
    if (LIB.group !== 'all' && it.group !== LIB.group) return false;
    if (LIB.search) {
        var hay = (it.name + ' ' + (it.title || '') + ' ' + it.group + ' ' +
                   (it.designer || '')).toLowerCase();
        var words = LIB.search.toLowerCase().split(/\s+/);
        for (var i = 0; i < words.length; i++) {
            if (words[i] && hay.indexOf(words[i]) < 0) return false;
        }
    }
    return true;
}

function LibFiltered() {
    var out = LibItems().filter(LibMatches);
    if (LIB.sort === 'name') {
        out.sort(function (a, b) { return a.name.localeCompare(b.name); });
    } else {
        out.sort(function (a, b) { return b.mtime - a.mtime; });
    }
    return out;
}

function LibDate(ts) {
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
    var img = it.thumb ? LibEscape(it.thumb) : 'img/d.png';

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
            it.machines.map(function (m) { return m.split(' ')[0]; }).join(' ')) + '</span>';
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
             '" fpath="' + LibEscape(it.path) + '" onClick="LibOpen(this)">' +
             '<a class="FileTip" title="' + LibEscape(it.path) + '"></a>' +
             '<div class="LibThumb"><img src="' + img +
               '" onerror="this.onerror=null;this.src=\'img/d.png\';" alt="" />' +
               '<span class="LibBadge ' + badge.cls + '">' + badge.text + '</span>' +
               tick +
             '</div>' +
             '<div class="LibName TextS1">' + LibEscape(it.name) + '</div>' +
             '<div class="LibMeta">' + LibEscape(it.group) + ' &middot; ' +
                LibDate(it.mtime) + '</div>' +
             '<div class="LibMeta LibFacts">' + LibFilamentStrip(it) +
                (facts.length ? '<span>' + facts.join(' &middot; ') + '</span>' : '') +
             '</div>' +
             LibFilamentRows(it) +
             (tags ? '<div class="LibTags">' + tags + '</div>' : '') +
           '</div>';
}

function LibRender() {
    var all = LibFiltered();
    var page = all.slice(0, LIB.shown);
    var html = '';
    for (var i = 0; i < page.length; i++) html += LibCard(page[i]);
    if (!page.length) {
        html = '<div class="LibEmpty">Nothing matches. ' +
               (LIB.kind === '3mf' ? 'STL files are hidden; use the STL chip to include them.'
                                   : '') + '</div>';
    }
    $('#LibGrid').html(html);

    if (all.length > page.length) {
        $('#LibMore').show().text('Show ' +
            Math.min(LIB_PAGE, all.length - page.length) + ' more of ' +
            (all.length - page.length));
    } else {
        $('#LibMore').hide();
    }
    $('#LibCount').text(all.length + (all.length === 1 ? ' project' : ' projects'));
    LibPickupSync();
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
    LIB.shown = LIB_PAGE;
    $('#LibChips .LibChip').removeClass('LibChipOn');
    $('#LibChips .LibChip[cstate="' + id + '"]').addClass('LibChipOn');
    LibRender();
}

function LibToggleVersions() {
    LIB.versions = (LIB.versions === 'best') ? 'all' : 'best';
    LIB.shown = LIB_PAGE;
    $('#LibVersionChip').toggleClass('LibChipOn', LIB.versions === 'all');
    LibBuildChips();
    LibBuildGroups();
    $('#LibGroup').val(LIB.group);
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

function LibSetGroup(v) { LIB.group = v; LIB.shown = LIB_PAGE; LibRender(); }
function LibSetSort(v) { LIB.sort = v; LibRender(); }
function LibSearch(v) { LIB.search = v; LIB.shown = LIB_PAGE; LibRender(); }
function LibMore() { LIB.shown += LIB_PAGE; LibRender(); }

function LibOpen(el) {
    var p = $(el).attr('fpath');
    if (p) OnOpenRecentFile(encodeURI(p));
}

// Counts sit on the chips because the useful question is usually "how much of the
// backlog is in this state", and that should not need a click to answer.
function LibBuildChips() {
    var counts = {};
    var items = LibItems();
    for (var i = 0; i < items.length; i++) {
        if (LIB.kind === '3mf' && items[i].kind !== '3mf') continue;
        if (LIB.versions === 'best' && items[i].superseded) continue;
        counts[items[i].state] = (counts[items[i].state] || 0) + 1;
    }
    var html = '';
    for (var s = 0; s < LIB_STATES.length; s++) {
        var st = LIB_STATES[s];
        var n = st.id === 'all' ? Object.keys(counts).reduce(
                    function (a, k) { return a + counts[k]; }, 0)
              : (st.id === 'recent' ? null : (counts[st.id] || 0));
        if (n === 0) continue;
        html += '<div class="LibChip' + (st.id === LIB.state ? ' LibChipOn' : '') +
                '" cstate="' + st.id + '" onClick="LibSetState(\'' + st.id + '\')">' +
                st.label + (n === null ? '' : ' <b>' + n + '</b>') + '</div>';
    }
    $('#LibChips').html(html);
}

function LibBuildGroups() {
    var groups = {};
    var items = LibItems();
    for (var i = 0; i < items.length; i++) {
        if (LIB.kind === '3mf' && items[i].kind !== '3mf') continue;
        if (LIB.versions === 'best' && items[i].superseded) continue;
        groups[items[i].group] = (groups[items[i].group] || 0) + 1;
    }
    var keys = Object.keys(groups).sort();
    var html = '<option value="all">All projects</option>';
    for (var k = 0; k < keys.length; k++) {
        html += '<option value="' + LibEscape(keys[k]) + '">' +
                LibEscape(keys[k]) + ' (' + groups[keys[k]] + ')</option>';
    }
    $('#LibGroup').html(html);
}

// The real recent list still arrives from C++. It is kept as a filter rather than a
// separate view, so there is one grid and one set of controls.
function LibNoteRecent(pList) {
    LIB.recentPaths = {};
    for (var i = 0; i < (pList || []).length; i++) {
        if (pList[i] && pList[i].path) LIB.recentPaths[pList[i].path] = true;
    }
    if (LIB.state === 'recent') LibRender();
}

function LibInit() {
    if (typeof PROJECT_LIBRARY === 'undefined') {
        $('#LibGrid').html('<div class="LibEmpty">No library index found. Run ' +
            '<code>python E:\\3D-Printing\\Scripts\\build_library.py</code> to build it.</div>');
        return;
    }
    LibBuildChips();
    LibBuildGroups();
    LibRender();
}
