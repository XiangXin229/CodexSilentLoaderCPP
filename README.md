# CodexSilentLoader

一个用 C++ / Win32 写的静默启动器，用来启动**已安装的** OpenAI Codex（ChatGPT 桌面版），并在运行时开启中文界面。

它不修改 `app.asar`、不修改 `ChatGPT.exe`，而是在启动时通过本地 CDP 注入一小段脚本，让应用的 i18n 开关生效，并把界面语言设为中文。

## 特性

- 自动查找当前用户已安装的 `OpenAI.Codex`，选择最新版本
- 自动解析 `app.asar`，不写死 i18n layer id
- 使用 `IApplicationActivationManager` 启动 MSIX 应用
- 通过本地 WebSocket + CDP 注入
- 无窗口启动，注入完成后自动退出
- 不调用 `Page.reload`，不会打断应用启动
- 单文件 EXE，Visual Studio 工程

## 构建

要求：

- Visual Studio 2022，安装 **使用 C++ 的桌面开发** 工作负载
- Windows 10/11 SDK

打开：

```
CodexSilentLoader.sln
```

选择 `Release | x64`，生成即可。

输出：

```
dist\Release\CodexSilentLoader.exe
```

也可以用命令行：

```bat
build.bat
```

## 使用

只有一个参数：

```bat
CodexSilentLoader.exe        :: 静默启动、注入、退出
CodexSilentLoader.exe -log   :: 写日志到当前目录的 CodexSilentLoader.log
```

### 默认行为

1. 查找已安装的 `OpenAI.Codex`；
2. 如果软件**没有运行**：
   - 用 AUMID `OpenAI.Codex_2p2nqsd0c76g0!App` 启动；
   - 等待 DevTools 页面；
   - 注入脚本；
   - 注入成功后自动退出。
3. 如果软件**已经在运行**：
   - 弹出提示，要求彻底退出（包括托盘和后台进程）后重试；
   - 不会附加到已有实例；
   - 不会自动关闭或重启。

### 如果界面不是中文

用 `-log` 运行一次：

```bat
CodexSilentLoader.exe -log
```

日志写到当前目录：

```
CodexSilentLoader.log
```

如果当前目录不可写，会回退到 EXE 所在目录，再回退到：

```
%LOCALAPPDATA%\CodexSilentLoaderCPP\CodexSilentLoader.log
```

日志会记录：

- 安装包、版本、AUMID
- asar 扫描结果和自动发现的 i18n layer id
- DevTools 端口和 CDP 页面
- 注入状态：`patchedClients`、`localeForced`、`queryClient`、`language`、`error`
- 失败原因

说明：

- 每次运行覆盖旧日志；
- 每行立即 flush；
- `-log` 不会新开控制台窗口，如果从终端启动会同时打印到终端；
- 不加 `-log` 时完全静默，不产生日志文件。

## 工作原理

1. **定位安装包**
   - `GetPackagesByPackageFamily`
   - `GetPackagePathByFullName`

2. **自动解析 ASAR**
   - 读取 `app.asar` 头部 JSON
   - 扫描 `webview/assets` 中的 JS
   - 查找 `enable_i18n`
   - 提取 Statsig layer id

3. **启动应用**
   - `IApplicationActivationManager::ActivateApplication`
   - 参数：
     ```
     --remote-debugging-port=<随机端口>
     --remote-debugging-address=127.0.0.1
     --lang=zh-CN
     ```

4. **CDP 注入**
   - `Page.addScriptToEvaluateOnNewDocument` 让以后的新页面自动生效
   - `Runtime.evaluate` 在当前页面立即执行
   - 包装 Statsig 的 `getLayer`，强制 `enable_i18n = true`
   - 清理 React `memoCache` 中缓存的旧 layer 值
   - 找到应用的 QueryClient，把 `["vscode","locale-info"]` 写为中文，让首次渲染就是中文
   - 不调用 `Page.reload`

5. **退出**
   - 注入成功后继续观察一小段时间，处理新出现的页面
   - 然后退出
   - 应用继续运行，注入脚本继续生效

## 运行要求

- Windows 10 / Windows 11 x64
- 目标机器已安装 OpenAI Codex（MSIX 包）
- 当前 Release 配置使用 **MultiThreadedDLL (`/MD`)**，目标机器需要 Microsoft Visual C++ 2015-2022 Redistributable (x64)。
  - 如果希望不依赖 VC++ 运行库，可以在 Visual Studio 中把 **C/C++ → 代码生成 → 运行库** 改为 **多线程 (`/MT`)** 后重新生成。
  - 或在发布时附带 `VC_redist.x64.exe`。

## 说明与限制

- DevTools 端口只绑定 `127.0.0.1`，但应用运行期间本机其他进程仍可连接，不要在不可信环境中长期开放。
- 依赖应用内部实现（Statsig layer、QueryClient、`vscode/locale-info`），应用大版本更新后可能需要适配。
- 应用自身启动/渲染速度取决于 app-server 连接，加载器无法加速；但首次出现的界面就是中文。
- 软件已在运行时只弹窗提示，不会关闭或重启，避免丢失未保存的对话。
- EXE 未代码签名，首次运行可能出现 SmartScreen 提示。
- 本项目只用于本机已安装应用的界面语言调整，请自行确认符合相关软件的使用条款。

## 目录结构

```
CodexSilentLoaderCPP\
├─ CodexSilentLoader.sln
├─ CodexSilentLoader.vcxproj
├─ CodexSilentLoader.vcxproj.filters
├─ loader.cpp
├─ resource.rc
├─ resource.h
├─ App.ico
└─ README.md
```

## 图标

`App.ico` 已通过 `resource.rc` 嵌入 EXE。
