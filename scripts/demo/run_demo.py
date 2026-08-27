#!/usr/bin/env python3

from __future__ import annotations

import json
import os
import secrets
import shutil
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

from demo_support import (
    ApiClient,
    DemoError,
    find_business_run,
    load_evidence,
    require_run_key,
    task_is_projected,
    validate_evidence,
    wait_until,
)


INBOX_ROOT = Path("/demo/inbox")
FIXTURE_PATH = Path("/opt/labbridge-demo/demo-observations.csv")


def generated_run_key() -> str:
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%d%H%M%S")
    return f"{timestamp}-{secrets.token_hex(3)}"


def created_id(client: ApiClient, path: str, payload: dict[str, object]) -> str:
    item = client.post(path, payload)
    resource_id = item.get("id") if isinstance(item, dict) else None
    if not isinstance(resource_id, str) or not resource_id.isdigit():
        raise DemoError(f"POST {path} did not return a string ID")
    return resource_id


def disable_task(client: ApiClient, task_id: str) -> None:
    updated = client.patch(f"/tasks/{task_id}", {"enabled": False})
    if updated.get("id") != task_id or updated.get("enabled") is not False:
        raise DemoError(f"task {task_id} disable response is inconsistent")


def write_result(run_dir: Path, result: dict[str, object]) -> None:
    target = run_dir / "result.json"
    temporary = run_dir / "result.json.tmp"
    temporary.write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    temporary.replace(target)


def main() -> int:
    run_key = require_run_key(os.getenv("DEMO_RUN_KEY", generated_run_key()))
    node_code = os.getenv("DEMO_NODE_CODE", "demo-node-001")
    timeout_seconds = int(os.getenv("DEMO_TIMEOUT_SECONDS", "150"))
    if timeout_seconds < 30 or timeout_seconds > 600:
        raise DemoError("DEMO_TIMEOUT_SECONDS must be between 30 and 600")
    client = ApiClient(
        os.getenv("DEMO_API_BASE_URL", "http://server:18080/api/v1")
    )
    deadline = time.monotonic() + timeout_seconds

    run_dir = INBOX_ROOT / run_key
    raw_name = f"observations-{run_key}.csv"
    task_id = ""
    print(f"demo_run_key={run_key}")
    print("[1/5] waiting for Server management API")

    try:
        wait_until(
            "Server management API",
            deadline,
            lambda: client.get("/nodes", {"limit": "1"}),
            lambda page: isinstance(page.get("items"), list),
        )

        print(f"[2/5] waiting for Agent {node_code} registration and heartbeat")
        node = wait_until(
            f"Agent {node_code} online heartbeat",
            deadline,
            lambda: client.get(f"/nodes/{node_code}"),
            lambda item: item.get("effective_status") == "online"
            and item.get("last_heartbeat_at") is not None,
        )
        print(f"agent online last_heartbeat_at={node['last_heartbeat_at']}")

        print("[3/5] creating isolated CSV source, QC rules and task")
        # 每次 task 只看自己的目录，旧 fixture 不会被新任务重新采集。
        run_dir.mkdir(mode=0o750, parents=False, exist_ok=False)
        shutil.copyfile(FIXTURE_PATH, run_dir / raw_name)

        source_id = created_id(
            client,
            "/data-sources",
            {
                "node_code": node_code,
                "source_type": "local_directory",
                "name": f"demo-source-{run_key}",
                "config": {"root_path": str(run_dir), "extension": ".csv"},
                "enabled": True,
            },
        )
        print(f"created data_source id={source_id}")

        required_rule_id = created_id(
            client,
            "/qc-rules",
            {
                "name": f"demo-required-{run_key}",
                "rule_type": "required_fields",
                "config": {},
                "enabled": True,
            },
        )
        timestamp_rule_id = created_id(
            client,
            "/qc-rules",
            {
                "name": f"demo-timestamp-{run_key}",
                "rule_type": "basic_timestamp_format",
                "config": {},
                "enabled": True,
            },
        )
        print(
            "created qc_rules "
            f"required={required_rule_id} timestamp={timestamp_rule_id}"
        )

        task_id = created_id(
            client,
            "/tasks",
            {
                "node_code": node_code,
                "data_source_id": source_id,
                "name": f"demo-task-{run_key}",
                "task_type": "local_file_import",
                "schedule_expr": "* * * * *",
                "parser_type": "csv_observation",
                "qc_profile": f"demo-{run_key}",
                "qc_rule_ids": [required_rule_id, timestamp_rule_id],
                "enabled": True,
            },
        )
        print(f"created task id={task_id}")

        # 先确认 Agent 真的拿到任务，再等调度结果，超时时才能分清是配置还是执行阻塞。
        wait_until(
            f"Agent config to include task {task_id}",
            deadline,
            lambda: client.get(f"/agents/{node_code}/config"),
            lambda config: task_is_projected(config, task_id),
        )
        print(f"agent config includes task={task_id}")

        print("[4/5] waiting for a succeeded run with two business records")
        runs = wait_until(
            f"task {task_id} succeeded business run",
            deadline,
            lambda: client.list_all(
                "/task-runs",
                {
                    "node_code": node_code,
                    "task_id": task_id,
                    "limit": "20",
                },
            ),
            lambda items: find_business_run(items) is not None,
        )
        business_run = find_business_run(runs)
        if business_run is None:
            raise DemoError("business run disappeared after it was observed")
        run_id = business_run["id"]
        print(f"observed business run id={run_id}")

        print("[5/5] verifying raw, parsed, QC and alert evidence")
        evidence = load_evidence(client, node_code, run_id)
        validate_evidence(
            evidence,
            node_code=node_code,
            task_id=task_id,
            run_id=run_id,
            raw_name=raw_name,
            required_rule_id=required_rule_id,
            timestamp_rule_id=timestamp_rule_id,
        )

        disable_task(client, task_id)
        wait_until(
            f"Agent config to remove disabled task {task_id}",
            deadline,
            lambda: client.get(f"/agents/{node_code}/config"),
            lambda config: not task_is_projected(config, task_id),
        )

        result = {
            "demo_run_key": run_key,
            "node_code": node_code,
            "data_source_id": source_id,
            "required_rule_id": required_rule_id,
            "timestamp_rule_id": timestamp_rule_id,
            "task_id": task_id,
            "run_id": run_id,
            "raw_file_name": raw_name,
            "raw_files": 1,
            "parsed_records": 2,
            "qc_results": 4,
            "failed_qc_results": 1,
            "alerts": 1,
            "task_enabled": False,
        }
        write_result(run_dir, result)
        print("\nLabBridge CSV demo succeeded")
        print(
            f"node={node_code} task={task_id} run={run_id} "
            "status=succeeded items=2/2/0"
        )
        print("evidence raw=1 parsed=2 qc=4 failed_qc=1 alerts=1(open)")
        print("task disabled=true; PostgreSQL, SQLite and archive evidence retained")
        print("LABBRIDGE_DEMO_RESULT=" + json.dumps(result, separators=(",", ":")))
        return 0
    except (DemoError, OSError, KeyboardInterrupt) as error:
        print(f"demo failed: {error}", file=sys.stderr)
        if task_id:
            try:
                # 只收尾自己创建的 task，失败证据和其他演示任务都保留。
                disable_task(client, task_id)
                print(f"disabled incomplete demo task={task_id}", file=sys.stderr)
            except DemoError as cleanup_error:
                print(
                    f"could not disable task={task_id}: {cleanup_error}",
                    file=sys.stderr,
                )
        print(f"diagnostic demo_run_key={run_key}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (DemoError, OSError, ValueError) as error:
        raise SystemExit(f"demo failed before execution: {error}")
