import test from 'node:test';
import assert from 'node:assert/strict';
import { WorkbenchAPI } from '../../assets/api.js';

test('preview releases continuation and marks its partial result', async () => {
    const requests = [];
    const api = new WorkbenchAPI('http://localhost', 'secret', async (url, options) => {
        requests.push([url, JSON.parse(options.body)]);
        return new Response(
            JSON.stringify(
                url.endsWith('/cancel')
                    ? { cancelled: true }
                    : { rows: [{ key: 'a' }], next_cursor: 'next' },
            ),
            { headers: { 'Content-Type': 'application/json' } },
        );
    });
    const result = await api.scanPreview('/collections/id/scan', { method: 'POST', body: {} });
    assert.equal(result.next_cursor, 'next');
    assert.deepEqual(requests[1], [
        'http://localhost/collections/id/scan/cancel',
        { cursor: 'next' },
    ]);
});

test('single-file Arrow export refuses a truncated paged result and releases cursor', async () => {
    let cancelled = false;
    const api = new WorkbenchAPI('http://localhost', 'secret', async (url) => {
        if (url.endsWith('/cancel')) {
            cancelled = true;
            return new Response('{}');
        }
        return new Response(new Uint8Array([1, 2]), {
            headers: { 'X-GlamDB-Next-Cursor': 'next' },
        });
    });
    await assert.rejects(api.arrow('/collections/id/scan', {}), /multiple pages/);
    assert.equal(cancelled, true);
});
