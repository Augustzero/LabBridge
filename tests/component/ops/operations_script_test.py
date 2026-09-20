#!/usr/bin/env python3
"""Phase 28 故障回归；保留临时现场，不接触真实 Docker/systemd，不批量清理。"""
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import tarfile
import unittest

REPO = Path(__file__).resolve().parents[3]
DOCKER = r"""#!/usr/bin/env python3
import datetime, json, os, sys
args = sys.argv[1:]
case = os.environ.get("OPS_CASE", "healthy")
base = os.environ["OPS_FIXTURE"]
now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
if args == ["info"]: pass
elif "config" in args:
 print(json.dumps({"services": {
  "server": {"image": "labbridge/server:test", "volumes": [{"target": "/etc/labbridge/auth/management.token", "source": base+"/config/auth/management.token"}]},
  "postgres": {"image": "postgres:16"}}}))
elif "ps" in args:
 if "--format" not in args: print("postgres")
 elif case == "bad-json": print("invalid")
 else:
  names = ["postgres", "server"] if case == "missing-web" else ["postgres", "server", "web"]
  print(json.dumps([dict(Service=n, State="running", Health="healthy") for n in names]))
elif "port" in args: print("127.0.0.1:8080")
elif "web" in args and "sh" in args:
 path = args[-1]
 if case == "broken-api": print("not-json"); sys.exit()
 if "/task-runs?" in path:
  data = {"items": [] if case in ("never-succeeded", "new-node") else [{"finished_at": now}]}
 elif "/nodes/" in path:
  data = dict(enabled_task_count=1, open_alert_count=0, created_at=now,
   latest_task_run=None if case == "new-node" else dict(started_at="2020-01-01T00:00:00Z",status="failed"))
 else:
  data = {"items": [dict(node_code="station-001",effective_status="online",last_heartbeat_at=now)],"next_cursor":None}
 print(json.dumps({"data":data}))
elif "df" in args: print("Available\n99999999")
elif "pg_dump" in args: print("dummy dump")
elif "psql" in args:
 if "pg_database_size" in args[-1]: print("100")
 elif "server_version" in args[-1]: print("16.15")
 else: print("nodes=1")
"""


class OperationsTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="labbridge-ops-regression-"))
        self.bin = self.root / "bin"
        self.bin.mkdir()
        (self.root / "config/auth").mkdir(parents=True)
        (self.root / "config/auth/management.token").write_text("a"*64+"\n")
        self.env_file = self.root / "config/production.env"
        self.env_file.write_text(f'LABBRIDGE_AUTH_DIR="{self.root}/config/auth"\n')
        self.executable("docker", DOCKER)
        self.executable("curl", '#!/bin/sh\n[ "$OPS_CASE" != unreachable ]\n')
        self.executable("pgrep", "#!/bin/sh\nexit 1\n")
        self.executable("systemctl", "#!/bin/sh\necho inactive\nexit 3\n")
        self.executable("sync", "#!/bin/sh\nexit 0\n")
        self.env = dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}",
                        OPS_FIXTURE=str(self.root), OPS_CASE="healthy")

    def executable(self, name, text):
        path = self.bin / name
        path.write_text(text)
        path.chmod(0o700)

    def command(self, *args, case="healthy"):
        return subprocess.run(args, cwd=REPO, env=dict(self.env, OPS_CASE=case),
                              text=True, capture_output=True, timeout=20)

    def center(self, case):
        return self.command("bash", "scripts/ops/check.sh", "center", "--env-file",
                            str(self.env_file), "--max-run-age-seconds", "60", case=case)

    def test_center_does_not_accept_missing_or_broken_services(self):
        for case in ("missing-web", "broken-api", "bad-json", "unreachable", "never-succeeded"):
            with self.subTest(case=case):
                result = self.center(case)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("[FAIL]", result.stdout)
        for case in ("healthy", "new-node"):
            result = self.center(case)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_credential_reserved_name_and_failed_publication(self):
        target = self.root / "credentials"
        result = self.command("bash", "deploy/production/generate-credentials.sh", str(target), "management")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(target.exists())
        self.executable("chgrp", "#!/bin/sh\nexit 41\n")
        result = self.command("bash", "deploy/production/generate-credentials.sh", str(target), "station-001")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(target.exists())
        self.assertIn("staging directory retained", result.stderr)

    def test_credential_success_and_no_overwrite(self):
        target = self.root / "credentials"
        result = self.command("bash", "deploy/production/generate-credentials.sh", str(target), "station-001")
        self.assertEqual(result.returncode, 0, result.stderr)
        management = (target / "management.token").read_text()
        self.assertNotEqual(management, (target / "station-001.token").read_text())
        self.assertEqual(target.stat().st_mode & 0o777, 0o700)
        self.assertEqual((target / "management.token").stat().st_mode & 0o777, 0o640)
        result = self.command("bash", "deploy/production/generate-credentials.sh", str(target), "station-002")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((target / "management.token").read_text(), management)

    def test_failed_backup_does_not_publish_completion(self):
        self.executable("sed", '#!/bin/sh\n[ "$1" != "s/^/  /" ] || exit 42\nexec /usr/bin/sed "$@"\n')
        result = self.command("bash", "scripts/ops/backup-center.sh", "--env-file", str(self.env_file),
                              "--backup-id", "failed", "--output-root", str(self.root / "backups"))
        self.assertEqual(result.returncode, 42, result.stdout+result.stderr)
        self.assertTrue((self.root / "backups/failed/manifest.pending").exists())
        self.assertFalse((self.root / "backups/failed/manifest.txt").exists())

    def test_successful_backup_publishes_checksums(self):
        result = self.command("bash", "scripts/ops/backup-center.sh", "--env-file", str(self.env_file),
                              "--backup-id", "ok", "--output-root", str(self.root / "backups"))
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertTrue((self.root / "backups/ok/manifest.txt").exists())
        self.assertFalse((self.root / "backups/ok/manifest.pending").exists())

    def test_backup_inspection_uses_published_timestamp(self):
        backups = self.root / "backups"
        batch = backups / "old"
        batch.mkdir(parents=True)
        (batch / "manifest.pending").write_text("incomplete")
        shell = 'source scripts/ops/lib/check-common.sh; report_backup center "$1" 1; exit "$failures"'
        result = self.command("bash", "-c", shell, "check", str(backups))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("没有找到完整备份", result.stdout)
        (batch / "manifest.txt").write_text("created_at_utc: 2020-01-01T00:00:00Z\n")
        (batch / "SHA256SUMS").write_text("fixture checksum\n")
        result = self.command("bash", "-c", shell, "check", str(backups))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("最近备份已", result.stdout)

    def test_yaml_quoted_paths_and_agent_backup(self):
        state = self.root / "node state"
        work = state / "work # evidence"
        work.mkdir(parents=True)
        inbox = self.root / "input files"
        inbox.mkdir()
        (inbox / "pending.csv").write_text("sample\n")
        db = state / "agent.db"
        connection = sqlite3.connect(db)
        self.addCleanup(connection.close)
        connection.execute("PRAGMA journal_mode=WAL")
        connection.execute("PRAGMA wal_autocheckpoint=0")
        connection.executescript("""CREATE TABLE queue_metadata(singleton_id INTEGER, node_code TEXT);
            INSERT INTO queue_metadata VALUES(1, 'yes');
            CREATE TABLE pending_jobs(stage TEXT);
            INSERT INTO pending_jobs VALUES('retry_wait');
            CREATE TABLE pending_deliveries(id INTEGER);""")
        config = self.root / "config/agent.yaml"
        config.write_text(f'agent:\n  node_code: yes\n  token_file: "auth/management.token"\n'
                          f'storage:\n  queue_db: "{db}"\n  work_dir: "{work}"\n'
                          f'tasks:\n  allowed_local_roots: ["{inbox}"]\n')
        result = self.command("bash", "scripts/ops/backup-agent.sh", "--config", str(config),
                              "--backup-id", "quoted", "--output-root", str(self.root / "backups"))
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        manifest = (self.root / "backups/quoted/manifest.txt").read_text()
        self.assertIn("node_code: yes", manifest)
        self.assertIn("pending_jobs: 1", manifest)
        # 主库和未 checkpoint 的 WAL 都必须进入备份，恢复后仍有这条待处理作业。
        restored = self.root / "restored"
        restored.mkdir()
        with tarfile.open(self.root / "backups/quoted/agent-state.tar") as archive:
            self.assertTrue(any(name.endswith("agent.db-wal") for name in archive.getnames()))
            archive.extractall(restored, filter="data")
        with sqlite3.connect(restored / str(db).lstrip("/")) as restored_db:
            self.assertEqual(restored_db.execute("PRAGMA integrity_check").fetchone()[0], "ok")
            self.assertEqual(restored_db.execute("SELECT stage FROM pending_jobs").fetchall(), [("retry_wait",)])


if __name__ == "__main__":
    unittest.main()
