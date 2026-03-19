#!/bin/bash
# 测试脚本：验证代码审查中发现的问题

echo "=== 测试 1: 正常情况 ==="
echo -e "line1\nline2\nline3" | EVENT_NOEPOLL=1 ./mapred -m "cat" -c 2
echo "退出码: $?"
echo

echo "=== 测试 2: 子进程失败时的行为 ==="
echo -e "line1\nline2\nline3" | EVENT_NOEPOLL=1 ./mapred -m "exit 1" -c 2
echo "退出码: $?"
echo

echo "=== 测试 3: 空输入 ==="
echo -n "" | EVENT_NOEPOLL=1 ./mapred -m "cat" -c 2
echo "退出码: $?"
echo

echo "=== 测试 4: 大量数据（检查缓冲区扩展）==="
seq 1 10000 | EVENT_NOEPOLL=1 ./mapred -m "wc -l" -c 2
echo "退出码: $?"
echo

echo "=== 测试 5: 无效的 mapper 命令 ==="
echo "test" | EVENT_NOEPOLL=1 ./mapred -m "nonexistent_command_xyz" -c 2 2>&1
echo "退出码: $?"
echo

echo "=== 所有测试完成 ==="
