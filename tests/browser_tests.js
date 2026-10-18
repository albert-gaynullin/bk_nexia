// Exercise the JavaScript embedded in the production portal, without a device.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

async function main() {
    const source = fs.readFileSync(path.join(__dirname, '../bk/DevicePortal.cpp'), 'utf8');
    const script = source.match(/<script>([\s\S]*?)<\/script>/)[1];
    const nodes = {clock: {}, result: {}, sync: {disabled: false}};
    const requests = [], alerts = [];
    const fixedDate = new Date(2026, 9, 18, 14, 30, 5);
    const context = vm.createContext({
        document: {getElementById: id => nodes[id]},
        Date: class { constructor() { return fixedDate; } },
        alert: message => alerts.push(message),
        fetch: async (url, options = {}) => {
            requests.push({url, options});
            return {ok: true, text: async () => url === '/time' && !options.method ? '2026-10-18 14:30:05' : 'OK'};
        }
    });
    vm.runInContext(script, context);
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(nodes.clock.textContent, '2026-10-18 14:30:05');
    assert.equal(context.stamp(fixedDate), '2026-10-18 14:30:05');
    await context.syncTime();
    const setTime = requests.find(r => r.url === '/time' && r.options.method === 'POST');
    assert.equal(decodeURIComponent(setTime.options.body), 'time=2026-10-18 14:30:05');
    assert.equal(nodes.result.textContent, 'OK');
    assert.equal(nodes.sync.disabled, false);

    function form(name, size = 1018976) {
        const button = {disabled: false};
        return {elements: {firmware: {files: name ? [{name, size}] : []}}, querySelector: () => button, button};
    }
    const upload = form('bk.ino.bin');
    assert.equal(context.prepare(upload), true);
    assert.equal(upload.action, '/update?size=1018976');
    assert.equal(upload.button.disabled, true);
    assert.equal(context.prepare(form('BK.INO.BIN')), true);
    for (const bad of ['bk.ino.merged.bin', 'bk.ino.bootloader.bin', 'bk.ino.partitions.bin', 'bk.ino.elf', 'source.zip']) {
        assert.equal(context.prepare(form(bad)), false);
    }
    assert.equal(context.prepare(form(null)), false);
    await context.restart();
    assert.ok(requests.some(r => r.url === '/restart' && r.options.method === 'POST'));

    context.fetch = async () => { throw new Error('connection lost'); };
    await context.syncTime();
    assert.equal(nodes.sync.disabled, false);
    assert.match(nodes.result.textContent, /Нет связи/);
    console.log('PASS: embedded JS syntax, local date/time, RTC display, POST encoding, upload size query, file filtering, restart, network error');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
