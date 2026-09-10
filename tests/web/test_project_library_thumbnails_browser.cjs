// NODE_PATH must resolve Playwright. Runs a separate headless Edge with a mocked native bridge.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const {chromium} = require('playwright');
const webRoot = path.resolve(__dirname, '../../resources/web');
const item = (name, extra = {}) => ({name, path: `E:\\Projects\\${name}.stl`, folder: 'E:\\Projects',
    root: 'E:\\Projects', group: 'Projects', kind: 'stl', state: 'mesh', mtime: 100,
    revision: '1', size: 100, sizeMB: 0.1, thumbState: 'pending', ...extra});
const models = Array.from({length: 28}, (_, i) => item(`Model ${String(i).padStart(2, '0')}`));
const alpha = models[0], beta = models[1];
async function main() {
    let regenerated = false;
    const server = http.createServer((req, res) => {
        const relative = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
        if (/^\/test-preview\/(alpha|beta|back|recent)\.svg$/.test(relative) || (regenerated && relative === '/missing-cache.svg')) {
            res.writeHead(200, {'Content-Type': 'image/svg+xml'});
            const color = /alpha|back/.test(relative) ? '#297ac5' : '#ce7325';
            const shape = relative.includes('back') ? '<rect x="20" y="20" width="80" height="80"/>' : '<path d="M15 100 L60 15 L105 100 Z"/>';
            res.end(`<svg xmlns="http://www.w3.org/2000/svg" width="120" height="120"><g fill="${color}">${shape}</g></svg>`);
            return;
        }
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
        const page = await browser.newPage({viewport: {width: 1100, height: 700}, userAgent: 'BBL-Slicer test'});
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        await page.addInitScript(() => {
            window.bridgeMessages = [];
            window.wx = {postMessage(raw) { bridgeMessages.push(JSON.parse(raw)); }};
        });
        await page.goto(`http://127.0.0.1:${server.address().port}/homepage/index.html`);
        assert.equal(await page.evaluate(() => bridgeMessages.filter(m => m.command === 'homepage_library_thumbnails').length), 0,
            'Warm catalogue cannot request thumbnails before native metadata arrives');
        await page.evaluate(items => {
            LIB.kind = 'all'; LIB.shelf = 'all';
            LibReceive({native: true, generated: 1, roots: ['E:\\Projects'], items});
        }, models);
        const requests = await page.evaluate(() => bridgeMessages.filter(m => m.command === 'homepage_library_thumbnails'));
        assert.equal(requests.length, 1);
        assert.equal(new Set(requests[0].paths).size, models.length, 'Each rendered path is requested once');
        assert.equal(requests[0].paths[0], alpha.path, 'Visible first card is queued before cards below the viewport');
        assert.equal(await page.locator('.LibThumbnailPending').count(), models.length);
        assert.equal(await page.locator('#LibGrid img').count(), 0, 'Pending models must not show a stock image');
        await page.evaluate(() => {
            window.retainedCard = document.querySelector('.LibCard');
            retainedCard.focus();
            document.querySelector('#ContentBoard').scrollTop = 150;
            window.retainedScroll = document.querySelector('#ContentBoard').scrollTop;
            LIB.picked[retainedCard.getAttribute('fpath')] = true;
        });
        await page.evaluate(update => LibThumbnailReceive(update), {...alpha, thumbState: 'ready',
            thumb: '/test-preview/alpha.svg', views: [{src: '/test-preview/alpha.svg', label: 'Front'}, {src: '/test-preview/back.svg', label: 'Back'}]});
        assert.ok(await page.evaluate(() => retainedCard === document.querySelector('.LibCard')), 'Arrival must not replace the card');
        assert.ok(await page.evaluate(() => document.activeElement === retainedCard), 'Arrival preserves focus');
        assert.ok(await page.evaluate(() => document.querySelector('#ContentBoard').scrollTop === retainedScroll && retainedScroll > 0), 'Arrival preserves nonzero scroll');
        assert.ok(await page.evaluate(() => LIB.picked[retainedCard.getAttribute('fpath')]), 'Arrival preserves selection');
        await page.evaluate(() => { document.querySelector('#ContentBoard').scrollTop = 0; });
        const alphaCard = page.locator('.LibCard').filter({has: page.locator('.LibName', {hasText: alpha.name})});
        await alphaCard.locator('img').waitFor();
        await page.waitForFunction(() => document.querySelector('.LibCard img').naturalWidth > 0);
        assert.match(await alphaCard.locator('img').getAttribute('alt'), /Model 00/);
        await alphaCard.locator('.SlideRight').click();
        assert.equal(await alphaCard.locator('img').getAttribute('src'), '/test-preview/back.svg', 'Incremental views become usable immediately');
        assert.equal(await page.evaluate(() => bridgeMessages.filter(m => m.command === 'homepage_open_recentfile').length), 0);

        await page.locator('#LibSearch').fill('Model 00');
        await page.evaluate(update => LibThumbnailReceive(update), {...beta, thumbState: 'ready', thumb: '/test-preview/beta.svg'});
        assert.equal(await page.locator('#LibSearch').inputValue(), 'Model 00', 'Hidden-file update must not reset search');
        assert.equal(await page.locator('.LibCard').count(), 1);
        await page.locator('#LibSearch').fill('');
        const betaCard = page.locator('.LibCard').filter({has: page.locator('.LibName', {hasText: beta.name})});
        assert.equal(await betaCard.locator('img').getAttribute('src'), '/test-preview/beta.svg', 'Hidden update survives filter changes');
        assert.notEqual(await alphaCard.locator('img').getAttribute('src'), await betaCard.locator('img').getAttribute('src'), 'Models retain distinct previews');
        if (process.env.PODSLICER_TEST_SCREENSHOT) await page.screenshot({path: process.env.PODSLICER_TEST_SCREENSHOT});
        await page.evaluate(() => LibRender());
        assert.equal(await page.evaluate(() => bridgeMessages.filter(m => m.command === 'homepage_library_thumbnails' && !m.force).length), 1,
            'Re-rendering must not duplicate pending work');

        await page.evaluate(update => LibThumbnailReceive(update), {...beta, thumbState: 'ready', thumb: '/missing-cache.svg', views: []});
        await page.waitForFunction(() => bridgeMessages.some(m => m.command === 'homepage_library_thumbnails' && m.force));
        assert.match(await betaCard.innerText(), /Generating preview/);
        await page.evaluate(() => LibRender());
        assert.equal(await page.evaluate(() => bridgeMessages.filter(m => m.force).length), 1);
        regenerated = true;
        await page.evaluate(update => LibThumbnailReceive(update), {...beta, thumbState: 'ready', thumb: '/missing-cache.svg', views: []});
        await page.waitForFunction(path => {
            const card = [...document.querySelectorAll('.LibCard')].find(card => card.getAttribute('fpath') === path);
            return card.querySelector('img') && card.querySelector('img').naturalWidth > 0;
        }, beta.path);
        assert.equal(await betaCard.locator('img').getAttribute('src'), '/missing-cache.svg', 'Regenerated cache is displayed even at the same URL');
        await page.evaluate(update => LibThumbnailReceive(update), {...beta, thumbState: 'ready', thumb: '/missing-cache-again.svg', views: []});
        await betaCard.locator('.LibThumbnailUnavailable').waitFor();
        assert.equal(await page.evaluate(() => bridgeMessages.filter(m => m.force).length), 1, 'Second failure must not trigger another regeneration');
        assert.equal(await betaCard.locator('img').count(), 0);
        assert.match(await betaCard.innerText(), /could not be loaded/);

        await page.evaluate(update => LibThumbnailReceive(update), {...models[2], thumbState: 'unavailable', thumb: '', views: [], thumbError: 'The mesh cannot be read'});
        assert.equal(await page.locator('.LibThumbnailUnavailable').count(), 2);
        assert.equal(await page.locator('#LibGrid img[src*="img/d.png"]').count(), 0);
        await page.evaluate(update => LibThumbnailReceive(update), {path: 'D:\\Outside.stl', thumbState: 'ready', thumb: '/test-preview/recent.svg',
            views: [{src: '/test-preview/recent.svg', label: 'Front'}, {src: '/test-preview/back.svg', label: 'Back'}], revision: '1', size: 100});
        await page.evaluate(() => { LibNoteRecent([{path: 'D:\\Outside.stl'}]); LibSetState('recent'); LibNoteRecent([{path: 'D:\\Outside.stl'}]); });
        assert.equal(await page.locator('.LibCard').count(), 1);
        assert.equal(await page.locator('.LibCard img').getAttribute('src'), '/test-preview/recent.svg');
        assert.equal(await page.locator('.SlideRight').count(), 1, 'Unindexed recents keep their received multi-view previews');
        const recentRequestsBefore = await page.evaluate(() => bridgeMessages.filter(m => m.command === 'homepage_library_thumbnails').length);
        await page.evaluate(() => LibNoteRecent([{path: 'D:\\Outside.stl', revision: '1', size: 100, image: '/stale-startup.png', thumbState: 'pending'}]));
        assert.equal(await page.locator('.LibCard img').getAttribute('src'), '/test-preview/recent.svg', 'Unchanged unindexed metadata keeps its generated preview');
        assert.equal(await page.evaluate(() => bridgeMessages.filter(m => m.command === 'homepage_library_thumbnails').length), recentRequestsBefore);
        await page.evaluate(() => LibNoteRecent([{path: 'D:\\Outside.stl', revision: '2', size: 100, image: '/stale-startup.png', thumbState: 'pending'}]));
        assert.equal(await page.locator('.LibThumbnailPending').count(), 1);
        assert.equal(await page.locator('.LibCard img').count(), 0, 'Changed recent hides the startup-cached image');
        assert.equal(await page.evaluate(() => bridgeMessages.filter(m => m.command === 'homepage_library_thumbnails').length), recentRequestsBefore + 1, 'Changed unindexed recent queues fresh work');
        await page.evaluate(() => LibThumbnailReceive({path: 'D:\\Outside.stl', revision: '1', size: 100, thumbState: 'ready', thumb: '/test-preview/recent.svg'}));
        assert.equal(await page.locator('.LibThumbnailPending').count(), 1, 'Late old recent image is ignored');
        await page.evaluate(() => LibThumbnailReceive({path: 'D:\\Outside.stl', revision: '2', size: 100, thumbState: 'ready', thumb: '/test-preview/back.svg'}));
        assert.equal(await page.locator('.LibCard img').getAttribute('src'), '/test-preview/back.svg', 'Fresh generated result replaces the changed recent preview');
        await page.evaluate(() => LibNoteRecent([{path: 'D:\\Outside.stl', revision: '2', size: 101, image: '/stale-startup.png', thumbState: 'pending'}]));
        assert.equal(await page.locator('.LibThumbnailPending').count(), 1, 'Equal timestamp with changed size invalidates recent preview');
        assert.equal(await page.evaluate(() => bridgeMessages.filter(m => m.command === 'homepage_library_thumbnails').length), recentRequestsBefore + 2);
        await page.evaluate(() => LibNoteRecent([{path: 'Z:\\Missing.stl', missing: true}]));
        assert.match(await page.locator('.LibThumbnailUnavailable').innerText(), /File unavailable/);
        assert.equal(await page.locator('#LibGrid img').count(), 0);
        assert.equal(await page.evaluate(() => bridgeMessages.filter(m => m.command === 'homepage_library_thumbnails')
            .flatMap(m => m.paths).includes('Z:\\Missing.stl')), false, 'Known missing files are not queued for rendering');
        assert.deepEqual(errors, []);
        console.log('Library thumbnail browser: incremental images, previews, retained DOM/focus/scroll/search, dedupe, one retry, unavailable states and recents passed.');
    } finally {
        if (browser) await browser.close();
        await new Promise(resolve => server.close(resolve));
    }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
