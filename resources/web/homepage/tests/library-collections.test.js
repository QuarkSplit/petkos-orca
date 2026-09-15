// Regression checks for the library shelf's collection model. Run: node tests/library-collections.test.js
// The page script is loaded into a bare VM with a stub jQuery, so these assert the logic
// (membership, shelf routing, pin/hide deltas, migration) rather than the paint.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

const HOME = path.join(__dirname, '..');

function load(opts) {
    const store = Object.assign({}, opts.storage || {});
    const calls = [];
    const el = () => ({
        html: (h) => { if (h !== undefined) calls.push(['html', h]); return el(); },
        text: (t) => { if (t !== undefined) calls.push(['text', t]); return el(); },
        val: () => el(), attr: () => el(), toggleClass: () => el(), addClass: () => el(),
        removeClass: () => el(), hide: () => el(), show: () => el(), prop: () => el(),
    });
    const ctx = {
        window: {localStorage: {getItem: (k) => (k in store ? store[k] : null), setItem: (k, v) => { store[k] = v; }}},
        document: {},
        $: () => el(),
        SendWXMessage: (m) => calls.push(['wx', m]),
        PROJECT_LIBRARY: opts.library || {items: []},
        console,
    };
    if (opts.workspace) ctx.PROJECT_WORKSPACE = opts.workspace;
    vm.createContext(ctx);
    vm.runInContext(fs.readFileSync(path.join(HOME, 'js/library.js'), 'utf8'), ctx);
    ctx.__calls = calls; ctx.__store = store;
    return ctx;
}

const item = (p, extra) => Object.assign({path: p, name: path.win32.basename(p).replace(/\.[^.]+$/, ''),
    folder: path.win32.dirname(p), root: p.indexOf('Models') >= 0 ? 'E:\\3D-Printing\\Models' : 'E:\\3D-Printing\\Projects',
    group: 'g', mtime: 1, sizeMB: 1, kind: p.endsWith('.stl') ? 'stl' : '3mf', state: 'mesh', thumbState: 'pending'}, extra || {});

const ITEMS = [
    item('E:\\3D-Printing\\Projects\\Helldivers Helmet\\helldivershelmet.stl'),
    item('E:\\3D-Printing\\Models\\Fandom\\Helldivers\\500kg_keychain.3mf', {state: 'foreign'}),
    item('E:\\3D-Printing\\Projects\\Comic Con 2026\\01-source\\1607565-las-58-talon-v2-helldivers-2\\LAS-58.3mf', {state: 'prepped'}),
    item('E:\\3D-Printing\\Models\\Fandom\\Iron Man & Marvel\\Captain+America+V5.3mf'),
    item('E:\\3D-Printing\\Models\\Fandom\\Iron Man & Marvel\\Iron+Man+Mask.3mf', {state: 'prepped'}),
    item('E:\\3D-Printing\\Projects\\Skippy-Malorian-Farm-Run\\Skippy\\Skippy-Farm-Run.3mf', {state: 'prepped', perPlateMachines: true}),
    item('E:\\3D-Printing\\Projects\\3D WiPs\\LAS-16+Sickle.3mf', {state: 'sliced', perPlateMachines: true}),
    item('E:\\3D-Printing\\Models\\Functional & Home\\hook.stl'),
];

const WS = JSON.parse(fs.readFileSync('E:\\3D-Printing\\library-collections.json', 'utf8'));
WS.revision = 1;

let n = 0;
function ok(cond, msg) { n++; assert.ok(cond, msg); }

// 1. Generated collections load and membership follows folders, matches, excludes and rules.
{
    const c = load({workspace: WS, library: {items: ITEMS}});
    const by = (id) => c.LibCollection(id);
    ok(c.LibCollections(false).length === WS.collections.length, 'all generated collections present');
    ok(c.LibInCollection(ITEMS[0], by('helldivers')), 'folder membership');
    ok(c.LibInCollection(ITEMS[2], by('helldivers')), 'match membership through the pipeline folder');
    ok(c.LibInCollection(ITEMS[1], by('keychains')), 'a keychain is also a keychain');
    ok(c.LibInCollection(ITEMS[4], by('iron-man')), 'iron man mask is iron man');
    ok(!c.LibInCollection(ITEMS[3], by('iron-man')), 'captain america excluded from iron man');
    ok(c.LibInCollection(ITEMS[3], by('marvel')), 'captain america is marvel');
    ok(c.LibInCollection(ITEMS[6], by('farm-runs')), 'perPlateMachines rule puts the Sickle on the farm-runs card');
    ok(c.LibInCollection(ITEMS[5], by('farm-runs')) && c.LibInCollection(ITEMS[5], by('cyberpunk')), 'farm run is both farm-run and cyberpunk');
    ok(c.LibShelfFor(ITEMS[7]) === 'mine', 'workshop folder routes to the shelf');
    ok(c.LibShelfFor(item('E:\\3D-Printing\\Models\\somewhere\\odd.stl')) === 'downloads', 'uncollected model routes to downloads');
    const stats = c.LibCollectionStats(by('helldivers'), ITEMS);
    ok(stats.files.length === 4 && stats.byState.prepped === 1 && stats.byState.foreign === 1 && stats.byState.sliced === 1, 'stats count files and states (the Sickle is Helldivers too)');
    const card = c.LibCollectionCard(by('helldivers'), ITEMS);
    ok(card.indexOf('Helldivers 2') > 0 && card.indexOf('1 configured') > 0 && card.indexOf('LibMosaic') > 0, 'card renders title, pills and mosaic');
    c.LibInit();
    ok(c.LIB.shelf === 'mine', 'page starts on the shelf');
    ok(c.LibWorkspaceRender() === true, 'overview renders');
    const grid = c.__calls.filter((x) => x[0] === 'html').pop()[1];
    ok(grid.indexOf('LibCollectionCard') >= 0 && grid.split('LibCollectionCard').length - 1 === WS.collections.length, 'one card per collection');
}

// 2. Browsing scopes the grid to the collection; matching honours the scope.
{
    const c = load({workspace: WS, library: {items: ITEMS}});
    c.LibBrowseCollection({getAttribute: () => 'helldivers'});
    ok(c.LIB.collection === 'helldivers' && c.LIB.shelf === 'mine', 'scope set');
    ok(c.LibMatches(ITEMS[0]) && !c.LibMatches(ITEMS[4]), 'scoped matching');
    ok(c.LibFiltered().length === 4, 'four helldivers files in scope');
    c.LibSetShelf('mine');
    ok(c.LIB.collection === '', 'back to the overview clears the scope');
}

// 3. Pin, hide, restore: deltas live beside the generated set and survive a rebuild.
{
    const c = load({workspace: WS, library: {items: ITEMS}});
    c.LibPinDesign({getAttribute: () => 'E:\\3D-Printing\\Projects\\Odd Folder'});
    ok(c.LibCollections(false).length === WS.collections.length + 1, 'pin adds a card');
    const saved = JSON.parse(c.__store['podslicer.library.workspace.v2']);
    ok(saved.added.length === 1 && saved.added[0].user === true, 'pin persisted as a user delta');
    c.LibUnpinDesign({getAttribute: () => 'helldivers'});
    ok(c.LibCollections(false).length === WS.collections.length && c.LibCollection('helldivers'), 'hiding a generated card keeps it retrievable');
    ok(JSON.parse(c.__store['podslicer.library.workspace.v2']).removed[0] === 'helldivers', 'hide persisted');
    c.LibRestoreHidden();
    ok(c.LibCollections(false).length === WS.collections.length + 1, 'restore brings it back');
    c.LibUnpinDesign({getAttribute: () => saved.added[0].id});
    ok(c.LibCollections(true).length === WS.collections.length, 'removing a pin deletes it');
    // A fresh load with the same storage sees the same deltas.
    const c2 = load({workspace: WS, library: {items: ITEMS}, storage: c.__store});
    ok(c2.LibCollections(true).length === WS.collections.length, 'deltas reload');
}

// 4. Legacy single-folder pins migrate once, and only the ones no collection covers.
{
    const legacy = {mine: [
        {path: 'E:\\3D-Printing\\Projects\\UK-Crossings-Tactile-Tiles', title: 'Crossings'},
        {path: 'E:\\3D-Printing\\Projects\\Secret CAD', title: 'Secret CAD', description: 'cad only'},
    ], downloads: [], jobs: []};
    const c = load({workspace: WS, library: {items: ITEMS}, storage: {'podslicer.library.workspace': JSON.stringify(legacy)}});
    const added = c.LibCollections(true).filter((x) => x.user);
    ok(added.length === 1 && added[0].title === 'Secret CAD' && added[0].description === 'cad only', 'only the uncovered legacy pin becomes a card');
    ok(c.__store['podslicer.library.workspace.v2'], 'migration written');
}

// 5. Without a workspace file the page still works: legacy catalogue workspace or nothing.
{
    const c = load({library: {items: ITEMS, workspace: {mine: [{path: 'E:\\3D-Printing\\Projects\\Skippy', title: 'Skippy'}], downloads: [], jobs: []}}});
    ok(c.LibCollections(false).length === 1 && c.LibCollections(false)[0].folders[0].endsWith('Skippy'), 'catalogue workspace becomes one-folder cards');
    const d = load({library: {items: ITEMS}});
    ok(d.LibCollections(false).length === 0, 'no workspace, no cards');
    d.LibInit();
    ok(d.LIB.shelf === 'all', 'no cards, start on all files');
    ok(d.LibWorkspaceRender() === false, 'overview declines to render without a shelf');
}

// 6. Regex patterns and the kind of path spelling the recent list delivers.
{
    const ws = {revision: 1, collections: [{id: 'rx', title: 'Rx', match: ['/las-?58/i'], folders: []}], downloads: [], jobs: []};
    const c = load({workspace: ws, library: {items: ITEMS}});
    ok(c.LibInCollection(item('E:/3D-Printing/x/LAS58.3mf'), c.LibCollection('rx')), 'regex match, forward slashes');
    ok(!c.LibInCollection(item('E:\\x\\other.3mf'), c.LibCollection('rx')), 'regex non-match');
}

console.log('library-collections: %d assertions pass', n);
