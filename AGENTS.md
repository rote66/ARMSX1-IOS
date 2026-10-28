# ARMSX iOS 维护约定

## 阅读入口与工作区

- 修改或打包 iOS 前先阅读 `readme-ios.md`，其中保留运行方式、目录结构、已知限制和编译命令。
- 若本机存在 `docs/history-session/`，阅读最近与本次任务相关的 iOS 记录；该目录是被 Git 忽略的本地资料，不是构建依赖。
- 开始前检查根仓库和 `third_party/fuse-lib` 的状态。保留用户已有修改，不要为了 clean 状态回滚、删除或提交无关改动。
- `third_party/fuse-lib` 是独立 Git 仓库。本地 FSUI 修复需要先在子模块提交，再由父仓库记录 gitlink；不要只提交父仓库而遗漏子模块改动。
- 未经用户明确要求，不执行 push 或 force-push。若将来需要发布，先确认本地 FSUI commit 在其远端可获取。
- 文档沿用中文，提交消息沿用已有的 conventional-commit 风格。

## 同步上游策略

当前 iOS 修改是一组建立在 `origin/main` 之上的分类补丁。先检查状态、分支和 remote，再 fetch，不要直接 pull：

```sh
git status --short
git -C third_party/fuse-lib status --short
git branch -vv
git remote -v
git fetch origin
git ls-remote --symref origin HEAD refs/heads/main refs/heads/android-compose
git log --oneline main..origin/main
git log --oneline origin/main..main
```

### 确认同步目标

- 默认同步目标为 `origin/main`，不要因为其它分支有更新就擅自更换目标。
- `origin/android-compose` 是另一条开发线，可能与当前 iOS 历史没有共同祖先；不能直接把 iOS 补丁栈 rebase 到它。
- 如果 fetch 后 `origin/main` 已被当前分支包含，应明确报告主线上游没有新提交，而不是制造空的同步 commit。
- 若用户确实要求迁移到其它上游开发线，先确认目标和共同祖先，单独评估核心、状态格式、构建系统及 FSUI 迁移范围。

### 默认选择：rebase

如果这些 iOS commit 主要由自己维护，或者远端尚没有其他人基于这些 commit 开发，默认 rebase。它保留“上游最新代码 + iOS 补丁栈”的直线历史，冲突可按 core、frontend、ios、docs 等功能边界逐组处理，也便于以后再次同步、bisect 和审查。

同步前必须保证根仓库和 `third_party/fuse-lib` 没有未提交修改，并创建仅用于回退的本地备份分支。示例中的分支名若已存在，使用带日期或序号的新名字，不要覆盖旧备份：

```sh
git branch backup/pre-upstream-sync
git rebase origin/main
```

解决冲突时不要整文件直接选 `ours` 或 `theirs`。先理解上游改动，再把 iOS 功能按新的上游结构重新落入；如果上游已实现某项功能，优先使用上游实现并移除重复补丁。

重点检查：

- `frontend/main.cpp` 的 FSUI、session、输入和 SDL 生命周期；保留 Classic/DualShock 布局、多点触控、布局编辑和全控件透明度。
- `psx/` 设备结构变化时，重新审查 `psx/state.c` 的状态版本、结构尺寸检查、指针保留、MDEC 动态缓冲和其它设备序列化。
- DualShock/SIO0/PAD 的配置探测、模式切换、RXNE/ACK/IRQ 和最后字节 no-ACK 行为；不要带回已移除的 NFS4 调试 trace。
- 父仓库更新 FSUI gitlink 时，分别确认本地触摸导航、暂停动作、BMP 解码、缩略图缓存刷新等修复仍存在于目标子模块 commit。
- `ios/HostApp` 和 SDL 初始化变化时，检查 UIKit Host、CoreMotion、日志重定向、JIT 状态探测、文件共享、持久文件授权和完整退出路径。

同步后用备份分支核对补丁栈语义：

```sh
git range-diff origin/main..backup/pre-upstream-sync origin/main..main
```

这里只比较本地补丁，不应把“冲突已全部解决”当成同步成功标准；还必须完成下文测试和 IPA 检查。

### 什么时候改用 merge

如果当前 iOS 分支已经公开推送，并且其他人已基于这些具体 commit SHA 开发，不应为了线性历史强制改写公共历史。这种情况下使用：

```sh
git fetch origin
git merge --no-ff origin/main
```

原则是：私有/单人维护的补丁栈优先 rebase；已经共享且不能安全 force-push 的公共分支使用 merge。需要长期同时维护纯上游和 iOS 适配时，可以在用户同意后采用独立的 `ios-port` 分支，让 `main` 只跟踪上游；不要自动更改现有分支布局。

## 验证与 IPA 交付

至少运行源码检查、CPU differential、GPU parity 和音频时序测试：

```sh
python3 tests/validate_sources.py
make test-cpu
make test-gpu SDL_CFLAGS=-Ithird_party/SDL/include
make test-audio
```

涉及输入、状态格式、MDEC 等改动时，补充对应的针对性回归。CPU 测试可能打印既有 opcode matrix diagnostic，最终以退出码和 `CPU_DIFFERENTIAL all cases passed` 为准。

完整 iOS 构建、环境变量、干净重编及手工校验步骤以 `readme-ios.md` 为准：

- 从干净的 FSUI/libchdr/object/staging 目录完整构建；保留可复用的 `build/fsui-python` venv，不要设置 `SKIP_NATIVE_BUILD=1` 冒充完整重编。
- 只清理 README 列出的生成物，不删除 BIOS、ROM、参考源码、用户资料或本地历史记录。
- 默认目标为 iOS 14.0 arm64；包使用 `ldid` fakesign，面向 TrollStore 或支持此类 IPA 的越狱环境，不是开发者证书/App Store 签名。
- 校验主程序和依赖的 deployment target、Bundle Identifier、IPA 完整性及 SHA-256。
- 最终主程序须含 `com.apple.security.cs.allow-jit`、`com.apple.security.cs.allow-unsigned-executable-memory`、`get-task-allow`，不得含 `dynamic-codesigning`。
- 若用户要求 clean 状态，验证后按功能提交本次相关改动，再检查根仓库和所有子模块；被忽略的 IPA 和构建目录不影响 Git clean。
- 交付时说明真实同步目标、冲突及补丁调整、测试结果、最终提交和 IPA 路径；没有真机验证时明确说明，不要把构建成功当成运行时验证。

## 日常同步请求

可直接使用：

```text
请按 AGENTS.md 的维护约定同步当前 ARMSX iOS 仓库的最新 origin/main。
先阅读 readme-ios.md 和本机存在的近期 iOS 历史，检查根仓库与 FSUI 子模块，fetch 后确认真实目标和分叉情况。
满足单人补丁栈条件时创建备份并 rebase；不要擅自改用 Android 开发分支，不要丢失已有 iOS 功能。
逐项处理冲突并核对 range-diff，运行相关测试，从干净构建目录完整生成 IPA。
校验 iOS 14 deployment target、Bundle 信息、JIT entitlement、IPA 完整性和 SHA-256。
提交本次相关修改至根仓库与子模块均 clean，报告最终提交和 IPA 路径；不要 push 或 force-push。
```

若分支已被其他人基于现有 SHA 使用，在请求中明确补充“禁止改写历史，使用 merge --no-ff origin/main”。
