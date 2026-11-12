"""JSON, CSV and readable load results with one common report stem."""

import csv
import io
from pathlib import Path

from .model import save_json


def validate_report(path, state_path, data, run_dir):
    path = Path(path).resolve()
    if (path.suffix != ".json" or path == Path(state_path).resolve() or path.is_relative_to(data.path) or
            path.is_relative_to(Path(run_dir).resolve())):
        raise ValueError("load report must be a separate .json path outside the dataset and journal")
    return path


def write_report(path, result):
    path = Path(path).resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    save_json(path, result)
    fields = ("status", "collection_mode", "load_complete", "snapshot_id", "confirmed_rows",
              "newly_confirmed_rows", "elapsed_seconds", "rows_per_second", "performance_eligible")
    stream = io.StringIO(newline="")
    writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
    writer.writeheader()
    writer.writerow(result)
    path.with_suffix(".csv").write_text(stream.getvalue())
    path.with_suffix(".md").write_text(
        "# ClickBench load\n\n| Metric | Value |\n|---|---|\n" +
        "\n".join(f"| {name} | {result.get(name, 'unavailable')} |" for name in fields) + "\n\n" +
        "Elapsed ingestion includes source part reads, encoding, journal syncs and network writes. "
        "Source preparation, capacity planning and final source/receipt/row verification are reported separately.\n")
