import json

import httpx
import pytest

from glamdb_sdk import Client, ProtocolError

CID = '11111111-2222-4333-8444-555555555555'
SCHEMA = {'fields': [{'name': 'key', 'type': 'string', 'nullable': False, 'field_id': 1}]}


def page(key, cursor, snapshot='9'):
    return {'schema': SCHEMA, 'snapshot_id': snapshot, 'identity': {'fixed': True},
            'rows': [{'key': key}], 'next_cursor': cursor, 'profile': {'rows_returned': '1'}}


def test_scan_json_drains_pages_without_changing_snapshot():
    seen = []
    def handle(request):
        seen.append(json.loads(request.content))
        return httpx.Response(200, json=page('duplicate', 'next' if len(seen) == 1 else None))
    with Client('http://example.test', 'secret', transport=httpx.MockTransport(handle)) as client:
        result = client.scan_json(CID)
        assert result.rows == [{'key': 'duplicate'}] * 2
        assert result.profile['rows_returned'] == '2'
    assert seen[1] == {'cursor': 'next'}


def test_scan_pages_retries_lost_first_page_and_cancels_early():
    seen = []
    lost = True
    def handle(request):
        nonlocal lost
        seen.append((request.url.path, json.loads(request.content)))
        if request.url.path.endswith('/open'):
            return httpx.Response(200, json={'cursor': 'first', 'schema': SCHEMA, 'snapshot_id': '9', 'identity': {'fixed': True}})
        if request.url.path.endswith('/cancel'):
            return httpx.Response(200, json={'cancelled': True})
        if lost:
            lost = False
            raise httpx.ReadError('response lost', request=request)
        return httpx.Response(200, json=page('duplicate', 'second'))
    with Client('http://example.test', 'secret', transport=httpx.MockTransport(handle)) as client:
        iterator = client.scan_pages(CID)
        assert next(iterator).rows == [{'key': 'duplicate'}]
        iterator.close()
    assert seen[1][1] == seen[2][1] == {'cursor': 'first'}
    assert seen[-1][0].endswith('/cancel')
    assert seen[-1][1] == {'cursor': 'second'}


def test_scan_json_rejects_view_change_between_pages():
    calls = 0
    def handle(request):
        nonlocal calls
        calls += 1
        return httpx.Response(200, json=page('a', 'next' if calls == 1 else None, str(calls)))
    with Client('http://example.test', 'secret', transport=httpx.MockTransport(handle)) as client:
        with pytest.raises(ProtocolError, match='view'):
            client.scan_json(CID)


def test_arrow_scan_follows_pages_and_validates_every_page():
    import hashlib
    import pyarrow as pa
    bodies = []
    def handle(request):
        bodies.append(json.loads(request.content))
        sink = pa.BufferOutputStream()
        batch = pa.record_batch([pa.array(['same'])], schema=pa.schema([pa.field('key', pa.string(), nullable=False)]))
        with pa.ipc.new_stream(sink, batch.schema) as writer:
            writer.write_batch(batch)
        body = sink.getvalue().to_pybytes()
        headers = {'content-type': 'application/vnd.apache.arrow.stream',
                   'x-glamdb-snapshot-id': '9', 'x-content-sha256': hashlib.sha256(body).hexdigest()}
        if len(bodies) == 1:
            headers['x-glamdb-next-cursor'] = 'second'
        headers['content-length'] = str(len(body))
        return httpx.Response(200, stream=httpx.ByteStream(body), headers=headers)
    with Client('http://example.test', 'secret', transport=httpx.MockTransport(handle)) as client:
        with client.scan_batches(CID) as scan:
            assert sum(batch.num_rows for batch in scan) == 2
            assert scan.integrity == 'verified'
    assert bodies[1] == {'cursor': 'second'}


def test_client_close_cancels_an_unfinished_json_page_iterator():
    seen = []
    def handle(request):
        seen.append(request.url.path)
        if request.url.path.endswith('/open'):
            return httpx.Response(200, json={'cursor': 'first', 'schema': SCHEMA, 'snapshot_id': '9', 'identity': {'fixed': True}})
        if request.url.path.endswith('/cancel'):
            return httpx.Response(200, json={'cancelled': True})
        return httpx.Response(200, json=page('a', 'next'))
    with Client('http://example.test', 'secret', transport=httpx.MockTransport(handle)) as client:
        pages = client.scan_pages(CID)
        next(pages)
    assert seen[-1].endswith('/scan/cancel')
