#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import json
import os
import sqlite3
import subprocess
from pathlib import Path

from demo_support import (
    ApiClient,
    DemoError,
    load_evidence,
    require_run_key,
    validate_evidence,
)


def psql(query: str) -> list[str]:
    process = subprocess.run(
        [
            "psql",
            "-X",
            "-v",
            "ON_ERROR_STOP=1",
            "-At",
            "-F",
            "|",
            "-c",
            query,
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if process.returncode != 0:
        raise DemoError(
            f"PostgreSQL verification failed: {process.stderr.strip()}"
        )
    return [line for line in process.stdout.splitlines() if line]


def scalar(query: str) -> str:
    rows = psql(query)
    if len(rows) != 1:
        raise DemoError(f"expected one PostgreSQL row, got {len(rows)}")
    return rows[0]


def main() -> int:
    run_key = require_run_key(os.environ["DEMO_RUN_KEY"])
    result_path = Path("/demo/inbox") / run_key / "result.json"
    if not result_path.is_file():
        raise DemoError(f"completed demo metadata not found: {result_path}")
    result = json.loads(result_path.read_text(encoding="utf-8"))
    task_id = str(result["task_id"])
    run_id = str(result["run_id"])
    if not task_id.isdigit() or not run_id.isdigit():
        raise DemoError("stored task_id and run_id must be decimal strings")

    client = ApiClient(
        os.getenv("DEMO_API_BASE_URL", "http://server:18080/api/v1")
    )
    evidence = load_evidence(client, result["node_code"], run_id)
    validate_evidence(
        evidence,
        node_code=result["node_code"],
        task_id=task_id,
        run_id=run_id,
        raw_name=result["raw_file_name"],
        required_rule_id=result["required_rule_id"],
        timestamp_rule_id=result["timestamp_rule_id"],
    )
    tasks = client.list_all(
        "/tasks", {"node_code": result["node_code"], "limit": "20"}
    )
    task = next(
        (candidate for candidate in tasks if candidate.get("id") == task_id),
        None,
    )
    if task is None:
        raise DemoError(f"task {task_id} is missing from management queries")
    if task.get("enabled") is not False:
        raise DemoError(f"task {task_id} is not disabled")
    print(
        "HTTP evidence: "
        f"task={task_id} disabled=true run={run_id} succeeded "
        "raw=1 parsed=2 qc=4 failed_qc=1 alerts=1(open)"
    )

    task_row = scalar(
        "SELECT t.enabled,ds.config_json->>'root_path',"
        "array_to_string(array_agg(tqr.qc_rule_id ORDER BY tqr.sort_order), ',') "
        "FROM tasks t JOIN data_sources ds ON ds.id=t.data_source_id "
        "JOIN task_qc_rules tqr ON tqr.task_id=t.id "
        f"WHERE t.id={task_id} GROUP BY t.enabled,ds.config_json"
    )
    enabled, root_path, rule_order = task_row.split("|", 2)
    expected_root = f"/demo/inbox/{run_key}"
    expected_order = (
        f"{result['required_rule_id']},{result['timestamp_rule_id']}"
    )
    if enabled != "f" or root_path != expected_root or rule_order != expected_order:
        raise DemoError(f"task configuration mismatch: {task_row}")

    counts = scalar(
        "SELECT count(DISTINCT rf.id),count(DISTINCT pr.id),"
        "count(DISTINCT qr.id),count(DISTINCT a.id) "
        "FROM task_runs tr LEFT JOIN raw_files rf ON rf.task_run_id=tr.id "
        "LEFT JOIN parsed_records pr ON pr.task_run_id=tr.id "
        "LEFT JOIN qc_results qr ON qr.parsed_record_id=pr.id "
        "LEFT JOIN alerts a ON a.task_run_id=tr.id "
        f"WHERE tr.id={run_id}"
    )
    if counts != "1|2|4|1":
        raise DemoError(f"PostgreSQL evidence counts mismatch: {counts}")
    receipts = psql(
        "SELECT request_type,count(*),count(completed_at) "
        "FROM agent_report_receipts "
        f"WHERE task_run_id={run_id} "
        "GROUP BY request_type ORDER BY request_type"
    )
    if receipts != ["raw_file_manifest|1|1", "task_run_report|1|1"]:
        raise DemoError(f"receipt state mismatch: {receipts}")
    print(
        "PostgreSQL evidence: disabled task with ordered QC bindings; "
        "raw=1 parsed=2 qc=4 alerts=1; manifest/report receipts=1/1 completed"
    )

    queue_path = Path("/var/lib/labbridge/agent.db")
    connection = sqlite3.connect(f"file:{queue_path}?mode=ro", uri=True)
    try:
        pending = connection.execute(
            "SELECT count(*) FROM pending_jobs"
        ).fetchone()[0]
        attention = connection.execute(
            "SELECT count(*) FROM pending_jobs WHERE stage='requires_attention'"
        ).fetchone()[0]
        processed = connection.execute(
            "SELECT count(*) FROM processed_files WHERE task_id=?",
            (task_id,),
        ).fetchone()[0]
    finally:
        connection.close()
    if (pending, attention, processed) != (0, 0, 1):
        raise DemoError(
            "SQLite state expected pending=0 attention=0 processed=1, "
            f"got pending={pending} attention={attention} processed={processed}"
        )
    print("SQLite evidence: pending=0 attention=0 processed_fingerprint=1")

    archive_row = scalar(
        f"SELECT storage_path,file_hash FROM raw_files WHERE task_run_id={run_id}"
    )
    archive_path_text, database_hash = archive_row.split("|", 1)
    archive_path = Path(archive_path_text)
    if not archive_path.is_file():
        raise DemoError(f"archive file is missing from agent_state: {archive_path}")
    digest = hashlib.sha256(archive_path.read_bytes()).hexdigest()
    if digest != database_hash:
        raise DemoError(
            f"archive hash mismatch: database={database_hash} actual={digest}"
        )
    print(
        f"Archive evidence: path={archive_path} "
        f"sha256={digest} (matches PostgreSQL)"
    )
    print(
        "Phase 026-02 engineering verification passed for "
        f"demo_run_key={run_key}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (DemoError, KeyError, OSError, json.JSONDecodeError) as error:
        raise SystemExit(f"verification failed: {error}")
