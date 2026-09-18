# ARMSX iOS 说明与编译指南

本文档说明 ARMSX 当前 iOS 版本的运行方式、目录结构、已知限制和完整编译流程。文档中的命令全部使用仓库相对路径，不依赖任何个人目录。

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
- `dynamic-codesigning`
- `get-task-allow`

这些权限为后续 JIT/动态代码执行保留条件，但当前 ARMSX 的 CPU 执行引擎仍是解释器；权限本身不会把解释器变成动态重编译器。

## iOS 前端现状

iOS 端使用轻量 UIKit Host 启动 SDL2 和原生 FSUI，不包含 React Native、CocoaPods 或 JavaScript bundle。

主要行为：

- 使用 SDL2 Metal renderer 显示画面。
- 使用 FSUI 作为游戏库、设置和暂停界面。
- iOS 触摸同时保留 SDL 原生多点触摸和主触点到鼠标事件的兼容桥，因此菜单点击与虚拟按键可以同时工作。
- 虚拟手柄支持多点触控，可同时操作摇杆/方向和右侧功能键。
- 暂停菜单可继续游戏、进入设置、更换光盘、截图、重置和返回游戏库。
- “重置”会销毁当前模拟器 session，并按当前 BIOS、游戏路径和设置完整重新创建 session；不使用仅重置 CPU 的旧 soft reset。
- “更换光盘”只更换 CD-ROM，不会额外把 CPU 重置回 BIOS，适用于游戏自身要求换盘的场景。
- Fast Boot 通过修改内存中的 BIOS shell/bootstrap 实现；不会修改用户的 BIOS 文件。如果 BIOS 布局不受支持，会自动回退到正常 BIOS 启动。
- iOS Documents container 变化后，旧的 Documents 绝对路径会在目标文件实际存在时迁移到当前 container。

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

- `Documents/armsx_log.txt`：每次启动重新创建的统一运行日志。
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
│   └── libarmsx.dylib
├── HostApp/
│   ├── Sources/
│   │   ├── AppDelegate.mm
│   │   ├── EmulatorRunner.mm
│   │   ├── ARMSX.entitlements
│   │   ├── Info.plist
│   │   └── LaunchScreen.storyboard
│   └── project.yml
└── build-ipa.sh

third_party/fuse-lib/   FSUI 子模块
frontend/main.cpp       主前端、iOS 触摸/菜单/session 管理
build.sh                libarmsx iOS 构建入口
```

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
7. 校验 Bundle Identifier、Mach-O deployment target 和 JIT entitlement。
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

应能看到前文列出的四项 entitlement。

## BIOS 与测试资源

打包脚本会按顺序检查以下可选开发 BIOS：

```text
ios/bios.bin
ios/Contents/bios.bin
bin/bios.bin
```

若存在，会复制为 App bundle 内的 `bios.bin`；不存在也不会导致 IPA 构建失败。正式使用建议通过 App Documents 提供 BIOS，不要把版权 BIOS 提交进仓库。

## 已知限制

- 当前 CPU 后端仍以解释器为主，JIT entitlement 只是运行权限准备。
- Fast Boot 依赖 BIOS 布局。受支持布局会应用内存补丁；不支持时会正常回退，不应修改 BIOS 原文件。
- Xcode GUI 工程主要用于开发调试；要得到 iOS 14 TrollStore/Dopamine 包，应以 `ios/build-ipa.sh` 的输出为准。
- `ldid` fakesign 包与 Apple 正式 codesign 包的验证方式不同。项目的打包脚本会直接使用 `ldid -e` 校验 entitlement。
