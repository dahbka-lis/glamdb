"""Disposable real native store for browser acceptance."""
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import uuid

root = Path(__file__).resolve().parents[3]
server = Path(os.environ['GLAMDB_NATIVE_SERVER']).resolve()
cli = Path(os.environ['GLAMDB_NATIVE_CLI']).resolve()
with tempfile.TemporaryDirectory(prefix='glamdb-browser-', dir='/tmp') as directory:
    store = str(Path(directory) / 'store')
    identity = str(uuid.uuid4())
    subprocess.run([str(cli), 'create-store', '--root', store, '--store-id', identity], check=True)
    process = subprocess.Popen([str(server), '--root', store, '--store-id', identity,
        '--port', os.environ.get('GLAMDB_E2E_PORT', '8766'), '--ui-root', str(root / 'ui')],
        env=dict(os.environ, GLAMDB_TOKEN='e2e-only-local-token'))
    signal.signal(signal.SIGTERM, lambda *_: process.terminate())
    signal.signal(signal.SIGINT, lambda *_: process.terminate())
    try:
        raise SystemExit(process.wait())
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=30)
