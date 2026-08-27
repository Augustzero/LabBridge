#!/usr/bin/env python3

import io
import time
import unittest
from unittest.mock import patch

from demo_support import (
    ApiClient,
    DemoError,
    Evidence,
    find_business_run,
    require_run_key,
    validate_evidence,
    wait_until,
)


def valid_evidence() -> Evidence:
    return Evidence(
        detail={
            "id": "20",
            "task_id": "10",
            "node_code": "demo-node-001",
            "status": "succeeded",
            "items_total": 2,
            "items_success": 2,
            "items_failed": 0,
        },
        raw_files=[
            {"id": "30", "task_run_id": "20", "original_name": "demo.csv"}
        ],
        parsed_records=[
            {"id": "40", "task_run_id": "20", "raw_file_id": "30"},
            {"id": "41", "task_run_id": "20", "raw_file_id": "30"},
        ],
        qc_results=[
            {
                "task_run_id": "20",
                "parsed_record_id": "40",
                "qc_rule_id": "50",
                "level": "pass",
                "result": "passed",
            },
            {
                "task_run_id": "20",
                "parsed_record_id": "40",
                "qc_rule_id": "51",
                "level": "pass",
                "result": "passed",
            },
            {
                "task_run_id": "20",
                "parsed_record_id": "41",
                "qc_rule_id": "50",
                "level": "pass",
                "result": "passed",
            },
            {
                "task_run_id": "20",
                "parsed_record_id": "41",
                "qc_rule_id": "51",
                "level": "failed",
                "result": "failed",
            },
        ],
        alerts=[
            {
                "node_code": "demo-node-001",
                "task_run_id": "20",
                "status": "open",
                "severity": "failed",
            }
        ],
    )


class DemoSupportTest(unittest.TestCase):
    def test_api_client_accepts_production_ok_data_envelope(self) -> None:
        response = io.BytesIO(b'{"ok":true,"data":{"id":"10"}}')
        with patch("urllib.request.urlopen", return_value=response):
            self.assertEqual(
                {"id": "10"}, ApiClient("http://server").get("/nodes")
            )

    def test_api_client_rejects_wrong_success_field(self) -> None:
        response = io.BytesIO(b'{"success":true,"data":{}}')
        with patch("urllib.request.urlopen", return_value=response):
            with self.assertRaisesRegex(DemoError, "ok/data"):
                ApiClient("http://server").get("/nodes")

    def test_run_key_keeps_names_within_database_limits(self) -> None:
        self.assertEqual("demo-123", require_run_key("demo-123"))
        with self.assertRaises(DemoError):
            require_run_key("x" * 49)

    def test_timeout_reports_last_run_status(self) -> None:
        with self.assertRaisesRegex(DemoError, '"status":"failed"'):
            wait_until(
                "business run",
                time.monotonic() + 0.01,
                lambda: [{"id": "20", "status": "failed", "items_total": 2}],
                lambda _items: False,
                interval_seconds=0.001,
            )

    def test_selects_succeeded_run_with_business_items(self) -> None:
        selected = find_business_run(
            [
                {"id": "22", "status": "succeeded", "items_total": 0},
                {"id": "21", "status": "failed", "items_total": 2},
                {"id": "20", "status": "succeeded", "items_total": 2},
            ]
        )
        self.assertEqual("20", selected["id"])

    def test_accepts_complete_linked_evidence(self) -> None:
        validate_evidence(
            valid_evidence(),
            node_code="demo-node-001",
            task_id="10",
            run_id="20",
            raw_name="demo.csv",
            required_rule_id="50",
            timestamp_rule_id="51",
        )

    def test_rejects_failure_from_required_rule(self) -> None:
        evidence = valid_evidence()
        evidence.qc_results[-1]["qc_rule_id"] = "50"
        with self.assertRaisesRegex(DemoError, "exactly once|timestamp rule"):
            validate_evidence(
                evidence,
                node_code="demo-node-001",
                task_id="10",
                run_id="20",
                raw_name="demo.csv",
                required_rule_id="50",
                timestamp_rule_id="51",
            )

    def test_rejects_alert_from_another_run(self) -> None:
        evidence = valid_evidence()
        evidence.alerts[0]["task_run_id"] = "999"
        with self.assertRaisesRegex(DemoError, "alert"):
            validate_evidence(
                evidence,
                node_code="demo-node-001",
                task_id="10",
                run_id="20",
                raw_name="demo.csv",
                required_rule_id="50",
                timestamp_rule_id="51",
            )


if __name__ == "__main__":
    unittest.main()
