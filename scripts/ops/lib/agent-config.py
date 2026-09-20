#!/usr/bin/env python3
"""巡检和备份共用的 Agent YAML 读取入口；输出 key<TAB>value。"""
import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    sys.exit("缺少 YAML 解析依赖，请安装 python3-yaml")


def read_config(path):
    # BaseLoader 保留标量原文，节点名 yes、数字编号也不会被转成布尔值或数字。
    data = yaml.load(Path(path).read_text(), Loader=yaml.BaseLoader)
    if not isinstance(data, dict):
        raise ValueError("配置顶层必须是映射")
    fields = {
        "agent": ("node_code", "server_url", "token_file"),
        "storage": ("queue_db", "work_dir", "max_pending_jobs"),
        "tasks": ("allowed_local_roots",),
    }
    rows = []
    for section, keys in fields.items():
        values = data.get(section, {})
        if not isinstance(values, dict):
            raise ValueError(f"{section} 必须是映射")
        for key in keys:
            value = values.get(key, "1000" if key == "max_pending_jobs" else "")
            if key == "allowed_local_roots":
                if not isinstance(value, list) or not value:
                    raise ValueError("tasks.allowed_local_roots 必须是非空列表")
            else:
                value = [value]
            for item in value:
                if not isinstance(item, str) or any(c in item for c in "\t\r\n\0"):
                    raise ValueError(f"{section}.{key} 必须是单行字符串")
                if key in ("queue_db", "work_dir", "allowed_local_roots", "token_file"):
                    if key == "allowed_local_roots" and not Path(item).is_absolute():
                        raise ValueError("tasks.allowed_local_roots 必须是绝对路径")
                    if item:
                        item = str((Path(path).resolve().parent / item).resolve())
                if key == "max_pending_jobs":
                    if not item.isdecimal() or not 1 <= int(item) <= 1000000:
                        raise ValueError("storage.max_pending_jobs 必须在 1～1000000 之间")
                    item = str(int(item))
                rows.append((f"{section}.{key}", item))
    for key in ("agent.node_code", "storage.queue_db", "storage.work_dir"):
        if not dict(rows)[key]:
            raise ValueError(f"缺少必填项 {key}")
    return rows


if __name__ == "__main__":
    try:
        rows = read_config(sys.argv[1])
    except (OSError, ValueError, yaml.YAMLError) as error:
        sys.exit(f"Agent 配置读取失败: {error}")
    for key, value in rows:
        print(f"{key}\t{value}")
