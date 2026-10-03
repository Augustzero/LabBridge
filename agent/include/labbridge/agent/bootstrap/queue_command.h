#pragma once

namespace labbridge::agent {

// labbridge_agent queue 子命令入口：解析 list/show/retry 参数并执行。
// 返回进程退出码：0 成功，1 执行失败，2 参数用法错误。
// 这里只做参数分派和结果输出，队列操作都在 storage 层。
int run_queue_command(int argc, char* argv[]);

}  // namespace labbridge::agent
