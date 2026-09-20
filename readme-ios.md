# ARMSX iOS 说明与编译指南

本文档说明 ARMSX 当前 iOS 版本的运行方式、目录结构、已知限制和完整编译流程。

## 目标平台

- 架构：arm64 真机。
- 最低系统：iOS 14.0。
- 界面方向：横屏，全屏。
- 目标安装环境：TrollStore，或 Dopamine + AppSync 等可安装 fakesigned IPA 的越狱环境。
- 输出：单一 `dist/ARMSX-iOS14-JIT.ipa`。
- iPhone 和 iPad 使用同一目标。

当前 IPA 不是 App Store/开发者证书签名包。打包脚本使用 `ldid` 进行 fakesign，并写入以下权限：

- `com.apple.security.cs.allow-jit`
- `com.apple.security.cs.allow-unsigned-executable-memory`
- `get-task-allow`

`dynamic-codesigning` 不写入最终包；`ios/build-ipa.sh` 会把它视为 TrollStore 不兼容 entitlement 并直接拒绝打包。上述 JIT 权限为后续动态代码执行保留条件，但当前 ARMSX 的 `cached`/`interpreter` CPU 执行引擎都不会生成本机代码；权限本身不会把当前 CPU 核心变成动态重编译器。设置页中的“JIT 状态”是运行时可执行内存能力探测，不代表当前 CPU 核心已经启用了 JIT。

## iOS 前端现状

iOS 端使用轻量 UIKit Host 启动 SDL2 和原生 FSUI，不包含 React Native、CocoaPods 或 JavaScript bundle。

主要行为：

- 使用 SDL2 Metal renderer 显示画面。
- 使用 FSUI 作为游戏库、设置和暂停界面。
- iOS 触摸同时保留 SDL 原生多点触摸和主触点到鼠标事件的兼容桥，因此菜单点击与虚拟按键可以同时工作。
- 虚拟手柄支持多点触控，可同时操作摇杆/方向和右侧功能键。
- 设置中可在 `Classic` 与 `DualShock` 两套触控布局间切换。Classic 保留旧的圆形数字方向控件；DualShock 提供上/下/左/右四个独立方向键，以及连续模拟输入的左、右双摇杆。
- 触控布局编辑器允许各元素独立拖动；选中元素可双指捏合或使用 `28–220 pt` 的尺寸滑杆调整大小。普通元素显示蓝色编辑框，当前选中元素显示黄色框，触控安全区域发生重叠时相关元素显示红框并给出冲突组数。
- 编辑器提供 `0–100%` 的全局“控件透明度”，同时作用于方向键、双摇杆、面键、肩键、SELECT/START 和暂停键；透明度只影响显示，不改变触控命中。仍支持恢复默认、导出和导入布局；当前不提供任意命名的多 profile 管理。
- DualShock 触控布局会以模拟手柄方式向核心提供四个模拟轴，并保留数字方向键；SIO0/PAD 实现支持游戏进行 DualShock 配置探测和模式切换。
- iOS 支持 CoreMotion 重力感应左摇杆。该功能只控制左模拟摇杆 X 轴，适合赛车游戏，支持死区和灵敏度设置；触摸左侧摇杆时触摸输入优先。
- Advanced 中的重力感应开关属于全局持久设置；游戏内暂停菜单另有“重力感应摇杆 已开启/已关闭”临时开关，只覆盖当前游戏，不写入全局配置。
- 暂停菜单可继续游戏、进入设置、即时保存、即时读取、更换光盘、截图、重置、临时切换重力感应和返回游戏库。
- 即时存档为每个游戏一个 quick slot，保存到 Documents/Saves。读取时会恢复 PSX 核心状态、VRAM、SPU RAM、CD-ROM 队列、MDEC 等当前实现覆盖的设备状态。
- Advanced 中的“调试日志”开关控制 iOS 统一日志文件。核心诊断开关立即生效，完整 stdout/stderr 文件重定向在下次启动时按开关状态生效。
- Advanced 中的“JIT 状态”会安全探测普通 RWX、RW→RX 权限切换和 MAP_JIT。任一可用即显示“可用”；如果都无法确认则显示“未确认”，不会把单一路径失败直接等价成系统没有 JIT。
- “重置”会销毁当前模拟器 session，并按当前 BIOS、游戏路径和设置完整重新创建 session；不使用仅重置 CPU 的旧 soft reset。
- “更换光盘”只更换 CD-ROM，不会额外把 CPU 重置回 BIOS，适用于游戏自身要求换盘的场景。
- Fast Boot 通过修改内存中的 BIOS shell/bootstrap 实现；不会修改用户的 BIOS 文件。如果 BIOS 布局不受支持，会自动回退到正常 BIOS 启动。
- 游戏退出到游戏库时会把当前画面写入 `Documents/Screenshots/<game>.bmp` 作为稳定缩略图，并立即刷新 FSUI 图片缓存。手动截图同时保留带时间戳的 BMP，并更新同名稳定缩略图；FSUI 已启用 BMP 解码。
- iOS Documents container 变化后，旧的 Documents 绝对路径会在目标文件实际存在时迁移到当前 container。
- “退出 App”会先让 frontend 完成 session、FSUI 和 SDL 清理，再由 UIKit Host 正常结束进程，避免只销毁 SDL 窗口后留下黑屏进程。

## App Documents 目录

首次运行后会使用/创建以下目录：

```text
Documents/
├── BIOS/
├── Games/
├── Saves/
└── Screenshots/
```

另外：

- `Documents/Saves/<game>-<hash>.state`：每个游戏的单一即时存档槽。
- `Documents/Screenshots/<game>.bmp`：游戏库使用的稳定缩略图；退出游戏或成功手动截图后会更新。
- `Documents/Screenshots/<game>-<timestamp>.bmp`：手动截图生成的带时间戳历史文件。
- `Documents/ARMSX-TouchLayout.layout`：触控布局导入/导出的交换文件。
- `Documents/armsx_log.txt`：调试日志开关启用并重新启动 App 后生成的统一运行日志。关闭开关不会自动删除已有旧日志。
- `Documents/.armsx-debug-log-enabled`：iOS Host 使用的隐藏调试日志启用标记。
- 文件共享已开启，可通过系统 Files/文件 App 访问 Documents。
- 游戏目录也可以通过 iOS 文件选择器添加；外部目录使用 security-scoped bookmark 持久化授权。

建议将 BIOS 放在 `Documents/BIOS/`，游戏放在 `Documents/Games/`。项目不会默认提交受版权保护的 BIOS 文件。

支持的常见光盘镜像包括 `.cue`、`.bin`、`.iso`、`.img`，启用 CHD 构建时也支持 `.chd`。

## 源码布局

与 iOS 直接相关的主要文件：

```text
ios/
├── Frameworks/
│   ├── SDL2.xcframework/
│   └── libarmsx.dylib       由 `./build.sh ios` 生成，不纳入 Git
├── HostApp/
│   ├── Sources/
│   │   ├── AppDelegate.mm
│   │   ├── EmulatorRunner.mm
│   │   ├── armsx_bridge.h
│   │   ├── ARMSX.entitlements
│   │   ├── Info.plist
│   │   └── LaunchScreen.storyboard
│   └── project.yml
└── build-ipa.sh

third_party/fuse-lib/   FSUI 子模块
frontend/main.cpp       主前端、iOS 触摸/菜单/session 管理
psx/state.c             PSX 即时存档序列化/恢复
build.sh                libarmsx iOS 构建入口
```

`AppDelegate.mm` 负责 iOS 文件授权、统一日志和 CoreMotion 重力数据采集；`EmulatorRunner.mm` 负责启动 frontend，并在 frontend 完整退出后结束 iOS 进程。CoreMotion 使用系统 framework，不需要额外第三方依赖。

`ios/HostApp/ARMSX.xcodeproj` 可以用于查看/调试 Host 工程，但 iOS 14 TrollStore/Dopamine 包推荐使用 `ios/build-ipa.sh`。新版 Xcode 的工程构建系统可能不再接受 iOS 14 deployment target，而脚本直接调用 iPhoneOS SDK 的 clang/linker，可以继续生成 `minos 14.0` 的 Mach-O。

## 编译环境

推荐在 macOS 上编译。需要：

1. Xcode，并确保 `xcrun --sdk iphoneos` 可以找到 iPhoneOS SDK。
2. CMake。
3. `ldid`。
4. Python 3。
5. Python 包 `jinja2`，FSUI 的 Glad 代码生成需要它。
6. `ios/Frameworks/SDL2.xcframework/ios-arm64/SDL2.framework`。
7. Git submodule 已初始化。

Homebrew 环境可安装基础工具：

```sh
brew install cmake ldid python
```

初始化子模块：

```sh
git submodule update --init --recursive
```

为 FSUI 建议创建仓库内、被 Git 忽略的 Python venv：

```sh
python3 -m venv build/fsui-python
build/fsui-python/bin/python -m pip install --upgrade pip jinja2
```

如果已有其他能成功执行 `import jinja2` 的 Python，也可以直接把它设置为 `FSUI_PYTHON_EXECUTABLE`。

## 一键生成 IPA

在仓库根目录执行：

```sh
PATH="/opt/homebrew/bin:/usr/local/bin:/opt/local/bin:/usr/bin:/bin:/usr/sbin:/sbin" \
FSUI_PYTHON_EXECUTABLE="$PWD/build/fsui-python/bin/python" \
IOS_DEPLOYMENT_TARGET=14.0 \
./ios/build-ipa.sh
```

脚本会依次：

1. 调用 `./build.sh ios` 构建 iOS arm64 `libarmsx.dylib`。
2. 用 CMake 为 iOS 构建 FSUI 静态库。
3. 使用 iPhoneOS SDK 直接编译 UIKit Host。
4. 复制 SDL2 和 `libarmsx.dylib` 到 App bundle。
5. 编译 LaunchScreen 和图标资源。
6. 用 `ldid` 对 SDL2、`libarmsx.dylib` 和主程序进行 fakesign。
7. 校验 Bundle Identifier、Mach-O deployment target 和 JIT entitlement，并确认最终签名不包含 `dynamic-codesigning`。
8. 生成 IPA，并打印 SHA-256。

成功后输出：

```text
dist/ARMSX-iOS14-JIT.ipa
```

## 只编译 libarmsx

如果只需要重新生成 iOS 动态库：

```sh
PATH="/opt/homebrew/bin:/usr/local/bin:/opt/local/bin:/usr/bin:/bin:/usr/sbin:/sbin" \
FSUI_PYTHON_EXECUTABLE="$PWD/build/fsui-python/bin/python" \
IOS_DEPLOYMENT_TARGET=14.0 \
./build.sh ios
```

输出：

```text
bin/libarmsx.dylib
ios/Frameworks/libarmsx.dylib
```

## 可用环境变量

常用覆盖项：

| 变量 | 默认值/作用 |
| --- | --- |
| `IOS_DEPLOYMENT_TARGET` | `14.0` |
| `IOS_SDK` | `iphoneos` |
| `IOS_SDL_FRAMEWORK` | `ios/Frameworks/SDL2.xcframework` |
| `FSUI_PYTHON_EXECUTABLE` | 自动寻找可导入 `jinja2` 的 `python3`/`python` |
| `BUILD_JOBS` | `4` |
| `IOS_BUNDLE_IDENTIFIER` | `com.nanodata.armsx` |
| `IPA_OUTPUT` | `dist/ARMSX-iOS14-JIT.ipa` |
| `SKIP_NATIVE_BUILD` | 设为 `1` 时打包脚本复用已有 `ios/Frameworks/libarmsx.dylib` |

通常不建议改变 `IOS_DEPLOYMENT_TARGET` 和 Bundle Identifier，除非同时清楚对应的安装、数据 container 和兼容性影响。

## 干净重编

需要验证 iOS 构建不是复用旧的 FSUI/libchdr/object 输出时，可以删除被 Git 忽略的构建目录后重新执行上一节的一键命令：

```sh
rm -rf build/fsui/ios build/fsui/ios-sdl-package build/libchdr/ios build/ios-ipa bin
rm -f dist/ARMSX-iOS14-JIT.ipa
```

保留 `build/fsui-python`，否则需要重新创建 Python venv 和安装 `jinja2`。

随后再次执行：

```sh
PATH="/opt/homebrew/bin:/usr/local/bin:/opt/local/bin:/usr/bin:/bin:/usr/sbin:/sbin" \
FSUI_PYTHON_EXECUTABLE="$PWD/build/fsui-python/bin/python" \
IOS_DEPLOYMENT_TARGET=14.0 \
./ios/build-ipa.sh
```

## 构建结果检查

`ios/build-ipa.sh` 已内置关键检查。还可以手工检查 IPA：

```sh
unzip -t dist/ARMSX-iOS14-JIT.ipa
```

检查 staging 主程序的 entitlement：

```sh
ldid -e build/ios-ipa/stage/Payload/ARMSX.app/ARMSX
```

应能看到前文列出的三项 entitlement，并且不应出现 `dynamic-codesigning`。

## BIOS 与测试资源

打包脚本会按顺序检查以下可选开发 BIOS：

```text
ios/bios.bin
ios/Contents/bios.bin
bin/bios.bin
```

若存在，会复制为 App bundle 内的 `bios.bin`；不存在也不会导致 IPA 构建失败。正式使用建议通过 App Documents 提供 BIOS，不要把版权 BIOS 提交进仓库。

## 同步上游策略

当前仓库的 iOS 修改是建立在上游 `origin/main` 之上的一组本地分类 commit。同步前应先执行 `git fetch origin`，再检查：

```sh
git status --short
git branch -vv
git log --oneline main..origin/main
git log --oneline origin/main..main
```

### 默认选择：rebase

如果这些 iOS commit 主要由自己维护，或者远端尚没有其他人基于这些 commit 开发，推荐：

```sh
git fetch origin
git rebase origin/main
```

原因：

- iOS 适配本质上是一组叠在上游之上的补丁，rebase 后仍保持“上游最新代码 + iOS 补丁栈”的直线历史。
- 冲突会按本地 commit 的功能边界逐组出现，通常比一个长期累积的大 merge conflict 更容易判断。
- `git log`、`git bisect` 和以后再次同步都更清楚，不会不断增加“merge upstream”节点。
- 目前 commit 已按 core、frontend、ios、docs 等职责分类，正适合长期作为可重放补丁维护。

同步前必须保证根仓库和 `third_party/fuse-lib` 都没有未提交修改。建议先建立一个仅用于回退的备份分支，例如：

```sh
git branch backup/pre-upstream-sync
git fetch origin
git rebase origin/main
```

解决冲突时不要整文件直接选 `ours` 或 `theirs`。应先理解上游改动，再把 iOS 功能按新的上游结构重新落入。尤其注意：

- `frontend/main.cpp` 的 FSUI、session、输入和 SDL 生命周期变化。
- `psx/` 设备结构体发生变化时，`psx/state.c` 的状态版本、结构尺寸检查、指针保留和动态缓冲序列化也必须重新审查。
- `third_party/fuse-lib` 是独立 Git 仓库。父仓库若更新 gitlink，要分别确认本地 FSUI 修复是否已经存在于目标 fuse-lib commit，不能只解决父仓库表面的 submodule 冲突。
- `ios/HostApp` 或 SDL 初始化变化时，要重新确认 UIKit Host、CoreMotion、日志重定向、文件共享和退出路径。

rebase 完成后建议使用 `git range-diff` 对比重放前后的补丁栈，并执行 CPU/GPU 测试及完整 iOS clean build。不要仅以“冲突全部解决”作为同步成功标准。

### 什么时候改用 merge

如果当前 iOS 分支已经公开推送，并且其他人已经基于这些具体 commit SHA 开发，不应为了保持线性历史而强制改写公共历史。这种情况下使用：

```sh
git fetch origin
git merge --no-ff origin/main
```

因此原则是：**私有/单人维护的补丁栈优先 rebase；已经共享且不能安全 force-push 的公共分支使用 merge。**

如果准备长期同时维护“纯上游”和“iOS 适配”，更推荐以后把 iOS 改动放在独立的 `ios-port` 分支，让 `main` 只跟踪上游；每次把 `ios-port` rebase 到最新 `main`。这样职责最清晰，也降低误把本地 iOS 历史当成上游历史的风险。

### 给 ChatGPT 的上游同步 Prompt

日常同步可以直接使用下面这段：

```text
请在当前 ARMSX 仓库同步最新上游。先阅读 readme-ios.md 和 docs/history-session 中最近与 iOS 相关的记录，再检查根仓库和 third_party/fuse-lib 的 Git 状态、当前 branch、remote 和与 origin/main 的分叉情况。先 fetch，不要直接 pull。

当前 iOS 改动按补丁栈维护。只要这些 commit 没有被其他人依赖，默认使用 rebase 到最新 origin/main，不要用 merge 制造同步节点。rebase 前创建 backup/pre-upstream-sync 一类的本地备份分支。遇到冲突时逐项理解上游改动和本地 iOS 意图后手工合并，不要整文件使用 ours/theirs，也不要丢掉已有 iOS 功能。

重点检查 frontend/main.cpp 的 FSUI/session/input 生命周期、psx/state.c 与上游 PSX 设备结构变化、iOS Host 的 CoreMotion/日志/JIT/文件访问/退出流程，以及 third_party/fuse-lib 的独立仓库状态。若上游已经实现了本地某项功能，优先采用上游实现并删除重复补丁，而不是机械保留两套逻辑。

rebase 完成后用 range-diff 或等效方式核对本地补丁语义是否保持，运行相关 CPU/GPU 测试，然后按 readme-ios.md 从干净构建目录完整生成 IPA。核对 deployment target、JIT entitlement、Bundle 信息和 Git clean 状态。不要 push 或 force-push，除非我明确要求。最后按功能说明解决了哪些冲突、哪些本地补丁因上游变化而调整，并列出最终 commit 序列。
```

如果分支已经公开共享、不能改写历史，则把上面 prompt 中的 rebase 要求替换为：

```text
当前分支已经被其他人基于现有 commit SHA 使用，不允许改写历史。fetch 后使用 merge --no-ff origin/main，同样逐项解决冲突、完整测试和 clean build，不要 force-push。
```

## 已知限制

- 当前 CPU 后端仍以解释器为主，JIT entitlement 只是运行权限准备。
- JIT 状态只探测安全的可执行内存权限路径，不会通过实际跳转执行动态生成的机器码来冒险测试；“未确认”不等于越狱环境一定没有其它 JIT 绕过路径。
- 即时存档是当前新增实现，格式带版本和结构尺寸校验，但仍属于核心状态序列化功能；上游设备结构变化后必须同步审查状态格式。
- 重力感应摇杆目前是 iOS CoreMotion 功能，只映射左模拟摇杆 X 轴，不是通用陀螺仪到所有轴/按键的映射系统。
- Fast Boot 依赖 BIOS 布局。受支持布局会应用内存补丁；不支持时会正常回退，不应修改 BIOS 原文件。
- Xcode GUI 工程主要用于开发调试；要得到 iOS 14 TrollStore/Dopamine 包，应以 `ios/build-ipa.sh` 的输出为准。
- `ldid` fakesign 包与 Apple 正式 codesign 包的验证方式不同。项目的打包脚本会直接使用 `ldid -e` 校验 entitlement。
