#!/usr/bin/env python3

from __future__ import annotations

import json
import re
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Any, Callable


RUN_KEY_PATTERN = re.compile(r"^[a-z0-9][a-z0-9-]{7,47}$")


class DemoError(RuntimeError):
    pass


def require_run_key(value: str) -> str:
    if not RUN_KEY_PATTERN.fullmatch(value):
        raise DemoError(
            "demo_run_key must contain 8-48 lowercase letters, digits or hyphens"
        )
    return value


@dataclass(frozen=True)
class Evidence:
    detail: dict[str, Any]
    raw_files: list[dict[str, Any]]
    parsed_records: list[dict[str, Any]]
    qc_results: list[dict[str, Any]]
    alerts: list[dict[str, Any]]


class ApiClient:
    def __init__(self, base_url: str, timeout_seconds: float = 10.0) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout_seconds = timeout_seconds

    def get(self, path: str, params: dict[str, str] | None = None) -> Any:
        suffix = path
        if params:
            suffix += "?" + urllib.parse.urlencode(params)
        return self._request("GET", suffix)

    def post(self, path: str, payload: dict[str, Any]) -> Any:
        return self._request("POST", path, payload)

    def patch(self, path: str, payload: dict[str, Any]) -> Any:
        return self._request("PATCH", path, payload)

    def list_all(
        self, path: str, params: dict[str, str], max_pages: int = 20
    ) -> list[dict[str, Any]]:
        items: list[dict[str, Any]] = []
        cursor: str | None = None
        for _ in range(max_pages):
            query = dict(params)
            if cursor:
                query["cursor"] = cursor
            page = self.get(path, query)
            items.extend(page["items"])
            cursor = page.get("next_cursor")
            if not page.get("has_more"):
                return items
            if not cursor:
                raise DemoError(f"{path} reports has_more without next_cursor")
        raise DemoError(f"{path} exceeded {max_pages} pages")

    def _request(
        self, method: str, path: str, payload: dict[str, Any] | None = None
    ) -> Any:
        body = None if payload is None else json.dumps(payload).encode("utf-8")
        request = urllib.request.Request(
            self.base_url + path,
            data=body,
            method=method,
            headers={"Accept": "application/json", "Content-Type": "application/json"},
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout_seconds) as response:
                decoded = json.load(response)
        except urllib.error.HTTPError as error:
            error_body = error.read().decode("utf-8", errors="replace")
            try:
                parsed = json.loads(error_body).get("error", {})
                detail = (
                    f"{parsed.get('code', 'unknown')}: "
                    f"{parsed.get('message', error_body)}"
                )
            except json.JSONDecodeError:
                detail = error_body or error.reason
            raise DemoError(
                f"{method} {path} returned HTTP {error.code}: {detail}"
            ) from error
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
            raise DemoError(f"{method} {path} failed: {error}") from error

        if not isinstance(decoded, dict) or decoded.get("ok") is not True:
            raise DemoError(f"{method} {path} returned an invalid ok/data envelope")
        return decoded.get("data")


def wait_until(
    description: str,
    deadline: float,
    operation: Callable[[], Any],
    accept: Callable[[Any], bool],
    interval_seconds: float = 2.0,
) -> Any:
    last_observation = "not observed"
    while time.monotonic() < deadline:
        try:
            value = operation()
            last_observation = json.dumps(
                value, ensure_ascii=False, separators=(",", ":")
            )
            if accept(value):
                return value
        except DemoError as error:
            last_observation = str(error)
        time.sleep(min(interval_seconds, max(0.0, deadline - time.monotonic())))
    raise DemoError(
        f"timed out waiting for {description}; last={last_observation}"
    )


def find_business_run(items: list[dict[str, Any]]) -> dict[str, Any] | None:
    for item in items:
        if item.get("status") == "succeeded" and item.get("items_total", 0) > 0:
            return item
    return None


def task_is_projected(config: dict[str, Any], task_id: str) -> bool:
    return any(task.get("id") == task_id for task in config.get("tasks", []))


def validate_evidence(
    evidence: Evidence,
    *,
    node_code: str,
    task_id: str,
    run_id: str,
    raw_name: str,
    required_rule_id: str,
    timestamp_rule_id: str,
) -> None:
    detail = evidence.detail
    expected_run = {
        "id": run_id,
        "task_id": task_id,
        "node_code": node_code,
        "status": "succeeded",
        "items_total": 2,
        "items_success": 2,
        "items_failed": 0,
    }
    for field, expected in expected_run.items():
        if detail.get(field) != expected:
            raise DemoError(
                f"run {run_id} field {field} expected {expected!r}, "
                f"got {detail.get(field)!r}"
            )

    counts = (
        len(evidence.raw_files),
        len(evidence.parsed_records),
        len(evidence.qc_results),
        len(evidence.alerts),
    )
    if counts != (1, 2, 4, 1):
        raise DemoError(
            "evidence counts expected raw=1 parsed=2 qc=4 alerts=1, "
            f"got raw={counts[0]} parsed={counts[1]} "
            f"qc={counts[2]} alerts={counts[3]}"
        )

    raw = evidence.raw_files[0]
    if raw.get("task_run_id") != run_id or raw.get("original_name") != raw_name:
        raise DemoError("raw file is not associated with this demo run and fixture")
    raw_id = raw.get("id")
    parsed_ids = {record.get("id") for record in evidence.parsed_records}
    if None in parsed_ids or len(parsed_ids) != 2:
        raise DemoError("parsed record IDs are missing or duplicated")
    for record in evidence.parsed_records:
        if record.get("task_run_id") != run_id or record.get("raw_file_id") != raw_id:
            raise DemoError("parsed record evidence is not linked to the demo raw file")

    expected_rules = {required_rule_id, timestamp_rule_id}
    rule_counts = {rule_id: 0 for rule_id in expected_rules}
    record_counts = {record_id: 0 for record_id in parsed_ids}
    failed_results: list[dict[str, Any]] = []
    for result in evidence.qc_results:
        if result.get("task_run_id") != run_id:
            raise DemoError("QC result belongs to another task run")
        rule_id = result.get("qc_rule_id")
        record_id = result.get("parsed_record_id")
        if rule_id not in rule_counts or record_id not in record_counts:
            raise DemoError("QC result is not linked to this demo rule and record set")
        rule_counts[rule_id] += 1
        record_counts[record_id] += 1
        if result.get("result") == "failed":
            failed_results.append(result)
    if set(rule_counts.values()) != {2} or set(record_counts.values()) != {2}:
        raise DemoError("each demo record must have both QC rules exactly once")
    if len(failed_results) != 1:
        raise DemoError(f"expected one failed QC result, got {len(failed_results)}")
    passed_results = [
        result
        for result in evidence.qc_results
        if result.get("level") == "pass" and result.get("result") == "passed"
    ]
    if len(passed_results) != 3:
        raise DemoError(f"expected three passed QC results, got {len(passed_results)}")
    failed = failed_results[0]
    if failed.get("qc_rule_id") != timestamp_rule_id or failed.get("level") != "failed":
        raise DemoError("the only failed QC result must be the timestamp rule")

    alert = evidence.alerts[0]
    if (
        alert.get("node_code") != node_code
        or alert.get("task_run_id") != run_id
        or alert.get("status") != "open"
        or alert.get("severity") != "failed"
    ):
        raise DemoError("alert must be the open failed alert derived from this demo run")


def load_evidence(client: ApiClient, node_code: str, run_id: str) -> Evidence:
    return Evidence(
        detail=client.get(f"/task-runs/{run_id}", {"node_code": node_code}),
        raw_files=client.list_all(
            "/raw-files", {"task_run_id": run_id, "limit": "20"}
        ),
        parsed_records=client.list_all(
            "/parsed-records", {"task_run_id": run_id, "limit": "20"}
        ),
        qc_results=client.list_all(
            "/qc-results", {"task_run_id": run_id, "limit": "20"}
        ),
        alerts=client.list_all(
            "/alerts",
            {"node_code": node_code, "task_run_id": run_id, "limit": "20"},
        ),
    )
