#!/bin/bash
# 提交脚本 - 在本地运行

set -e

echo "=== 提交 libswf 更改到 dev 分支 ==="

# 进入仓库目录
cd "$(dirname "$0")"

# 检查远程仓库
REMOTE_URL="https://github.com/tdyx87/libswf.git"
echo "远程仓库: $REMOTE_URL"

# 获取当前分支
CURRENT_BRANCH=$(git branch --show-current)
echo "当前分支: $CURRENT_BRANCH"

# 创建并切换到dev分支（如果不存在）
if ! git branch -a | grep -q "dev"; then
    echo "创建 dev 分支..."
    git checkout -b dev
else
    echo "切换到 dev 分支..."
    git checkout dev
fi

# 添加所有更改
echo "添加更改..."
git add -A

# 提交
echo "提交更改..."
git commit -m "修复Linux编译问题：完善renderer.cpp预处理器指令和添加软件渲染实现

- 修复第344行#ifdef RENDERER_GDIPLUS未正确关闭的问题
- 在第677行后添加缺失的#endif // _WIN32
- 将renderFrameSoftware移到RENDERER_GDIPLUS块外确保Linux编译
- 为所有DisplayObject派生类添加renderSoftware实现
- 添加Cairo渲染器和软件渲染支持
- 修复Frame数据结构访问(displayListOps替代objects)" || echo "没有新更改需要提交"

# 设置远程（如果没有）
if ! git remote | grep -q "origin"; then
    echo "添加远程仓库..."
    git remote add origin "$REMOTE_URL"
fi

# 推送到dev分支
echo "推送到 dev 分支..."
git push -u origin dev

echo "=== 完成 ==="
