// Optional UI integration: NODE_PATH must resolve Playwright; uses a separate headless Edge.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const {chromium} = require('playwright');
const webRoot = path.resolve(__dirname, '../../resources/web');
const project = (name, extra = {}) => ({name, path: 'E:\\Projects\\Props\\' + name + '.3mf',
    folder: 'E:\\Projects\\Props', root: 'E:\\Projects', group: 'Props', kind: '3mf',
    state: 'prepped', mtime: 1000, colours: [], ...extra});
const data = {native: true, generated: 1000, roots: ['E:\\Projects', 'Z:\\Offline'],
    rootStatus: [{path: 'E:\\Projects', count: 3, error: ''}, {path: 'Z:\\Offline', count: 0, error: 'Folder unavailable'}],
    items: [project('Alpha'), project('Beta', {materials: ['PETG']}), project('Mesh', {kind: 'stl', state: 'mesh', path: 'E:\\Projects\\Props\\Mesh.stl'})]};
async function main() {
    const server = http.createServer((req, res) => {
        const relative = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
        if (relative === '/homepage/data/library.js' || relative === '/homepage/data/views.js') {
            res.writeHead(200, {'Content-Type': 'text/javascript'}); res.end(''); return;
        }
        const file = path.resolve(webRoot, '.' + relative);
        if (!file.startsWith(webRoot + path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
            res.writeHead(404); res.end(); return;
        }
        const types = {'.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml'};
        res.setHeader('Content-Type', types[path.extname(file)] || 'application/octet-stream');
        fs.createReadStream(file).pipe(res);
    });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    let browser;
    try {
        browser = await chromium.launch({channel: 'msedge', headless: true});
        const page = await browser.newPage({viewport: {width: 1200, height: 900}, userAgent: 'BBL-Slicer test'});
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        await page.addInitScript(data => {
            window.bridgeMessages = [];
            window.wx = {postMessage(raw) {
                const message = JSON.parse(raw); window.bridgeMessages.push(message);
                if (message.command === 'homepage_library_refresh') setTimeout(() => LibReceive(data), 0);
                if (message.command === 'get_recent_projects') setTimeout(() => HandleStudio({command: 'get_recent_projects',
                    response: [{path: data.items[0].path}, {path: 'D:\\Downloads\\Outside.3mf'}]}), 0);
            }};
        }, data);
        await page.goto('http://127.0.0.1:' + server.address().port + '/homepage/index.html');
        await page.waitForFunction(() => window.PROJECT_LIBRARY && PROJECT_LIBRARY.native);
        assert.equal(await page.locator('.LibCard').count(), 3);
        await page.locator('#LibSearch').pressSequentially('Alpha');
        assert.equal(await page.locator('#LibSearch').inputValue(), 'Alpha');
        assert.equal(await page.locator('.LibCard').count(), 1);
        await page.locator('#LibSearch').press('Control+A');
        await page.locator('#LibSearch').pressSequentially('PETG');
        assert.equal(await page.locator('.LibName').innerText(), 'Beta');
        await page.locator('#LibSearch').fill('');
        await page.locator('[cstate="recent"]').click();
        await page.locator('#LibSearch').pressSequentially('Outside');
        assert.equal(await page.locator('.LibCard').count(), 1);
        await page.locator('.LibFolderButton').filter({hasText: 'Open folder'}).click();
        const action = await page.evaluate(() => bridgeMessages.at(-1));
        assert.equal(action.command, 'homepage_explore_recentfile');
        assert.equal(action.data.path, 'D:\\Downloads\\Outside.3mf');
        assert.equal(await page.evaluate(() => bridgeMessages.filter(m => m.command === 'homepage_open_recentfile').length), 0);
        await page.locator('#LibSearch').fill('');
        await page.locator('[cstate="mesh"]').click();
        assert.equal(await page.locator('.LibName').innerText(), 'Mesh');
        await page.locator('#LibFolders summary').click();
        assert.match(await page.locator('#LibFoldersList').innerText(), /Folder unavailable/);
        await page.setViewportSize({width: 720, height: 900});
        assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth), 'Library must fit a narrow window');
        await page.evaluate(() => {
            LIB_WORKSPACE = {mine: [{path: 'E:\\Projects\\Tactile', title: 'Tactile work'},
                {path: 'E:\\Projects\\CAD only', title: 'CAD only'}], downloads: ['E:\\Models'], jobs: ['E:\\Projects']};
            LibNoteRecent([]);
            LibReceive({generated: 1000, roots: ['E:\\Projects', 'E:\\Models'], rootStatus: [], items: [
                {name: 'Tile', path: 'E:\\Projects\\Tactile\\tile.stl', root: 'E:\\Projects', folder: 'E:\\Projects\\Tactile', group: 'Tactile', kind: 'stl', state: 'mesh'},
                {name: 'Download', path: 'E:\\Models\\download.3mf', root: 'E:\\Models', folder: 'E:\\Models', group: 'Models', kind: '3mf', state: 'foreign'},
                {name: 'Print job', path: 'E:\\Projects\\Props\\job.3mf', root: 'E:\\Projects', folder: 'E:\\Projects\\Props', group: 'Props', kind: '3mf', state: 'prepped'}]});
            LibSetShelf('mine');
        });
        assert.equal(await page.locator('.LibDesignCard').count(), 2, 'CAD-only projects must be reachable');
        await page.locator('.LibDesignCard').first().getByText('Show folder', {exact: true}).click();
        assert.equal(await page.evaluate(() => bridgeMessages.at(-1).data.path), 'E:\\Projects\\Tactile');
        await page.locator('.LibDesignCard').first().getByText('Browse files', {exact: true}).click();
        assert.equal(await page.locator('.LibName').innerText(), 'Tile', 'Own-project browsing includes STL automatically');
        await page.locator('#LibShelves').getByText('Downloaded models', {exact: true}).click();
        assert.equal(await page.locator('.LibName').innerText(), 'Download');
        await page.locator('#LibShelves').getByText('Print jobs', {exact: true}).click();
        assert.equal(await page.locator('.LibName').innerText(), 'Print job');
        await page.locator('.LibCard').getByText('Add folder to My projects', {exact: true}).click();
        assert.equal(await page.locator('.LibDesignCard').count(), 3, 'Pinning an indexed subfolder creates a project shortcut');
        assert.equal(await page.evaluate(() => bridgeMessages.filter(m => m.command === 'homepage_open_recentfile').length), 0, 'Pinning must not open the file');
        await page.locator('.LibDesignCard').filter({hasText: 'Props'}).getByText('Remove shortcut', {exact: true}).click();
        await page.locator('#LibShelves').getByText('My projects', {exact: true}).click();
        await page.locator('#LibSearch').fill('CAD');
        assert.equal(await page.locator('.LibDesignCard').count(), 1);
        await page.locator('.LibDesignCard').getByText('Remove shortcut', {exact: true}).click();
        assert.equal(await page.locator('.LibDesignCard').count(), 1);
        assert.equal(await page.evaluate(() => PROJECT_LIBRARY.items.length), 3, 'Shortcut removal must not remove files');
        assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth), 'Project overview must fit a narrow window');
        assert.deepEqual(errors, [], 'Homepage must not raise JavaScript errors');
        await page.emulateMedia({colorScheme: 'dark'});
        await page.waitForFunction(() => getComputedStyle(document.querySelector('#LibraryArea')).color === 'rgb(38, 46, 48)');
        const darkPage = await browser.newPage({userAgent: 'BBL-Slicer dark test', colorScheme: 'light'});
        await darkPage.goto('http://127.0.0.1:' + server.address().port + '/homepage/index.html');
        await darkPage.waitForFunction(() => getComputedStyle(document.querySelector('#LibraryArea')).color === 'rgb(239, 239, 240)');
        await darkPage.close();
        console.log('Project library browser: keyboard, filters, reveal, unavailable folders, own-project navigation, CAD-only folders, download separation and narrow layout passed.');
    } finally {
        if (browser) await browser.close();
        await new Promise(resolve => server.close(resolve));
    }
}
main().catch(error => {console.error(error); process.exitCode = 1;});
