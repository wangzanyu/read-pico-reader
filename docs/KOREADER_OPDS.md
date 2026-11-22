# KOReader 同步与 OPDS 书库 / KOReader sync and OPDS library

中文：本页说明设置 → 网络服务下的两个联网功能：KOReader 进度同步（kosync）与 OPDS 书库。
English: This page documents the two networked features under Settings → Network services:
KOReader progress sync (kosync) and the OPDS library.

中文：受 5 MB 应用分区限制，S31（Metalio）固件暂不编入这两个功能；Pico 固件包含全部功能。
English: Because of the 5 MB app partition, the S31 (Metalio) build currently leaves these
features out; the Pico build includes both.

## 入口 / Where to find it

- 设置 → 网络服务 → **KOReader 同步**
- 设置 → 网络服务 → **OPDS 书库**
- Settings → Network services → **KOReader sync** / **OPDS library**

设置页两行右侧显示当前状态：KOReader 同步显示已配置服务器域名或“未配置”；OPDS 显示服务器台数或“未配置”。
The two Settings rows show a summary: the configured kosync host (or 未配置/unconfigured), and the
number of OPDS servers (or unconfigured).

## KOReader 同步 / KOReader sync

- 作用：把本机 EPUB 的阅读位置与 kosync 服务器上的进度双向同步。仅支持 EPUB；TXT 不参与。
  Purpose: keep an EPUB's reading position in sync with a kosync server. EPUB only; TXT is not synced.
- 配置（页内“编辑配置”）：服务器地址、用户名、密码、文档匹配方式（内容 partial MD5 / 文件名 MD5）。
  公共默认服务器为 `https://sync.koreader.rocks`，可改为自建服务器。
  Config (Edit config): server URL, user, password, and document matching (partial MD5 of the file
  content, or MD5 of the file name). The default public server is `https://sync.koreader.rocks`;
  a self-hosted server is accepted.
- 书籍列表：列出最近有阅读进度的 EPUB（最多 20 本，按最近阅读排序），显示书名与进度百分比。
  Book list: up to 20 recently-read EPUBs sorted by recency, with title and progress percentage.
- 操作：
  - **同步**：拉取云端进度并与本机比较（`kosync_start`），不修改本地进度。
  - **上传本机**：把本机进度写到服务器（`kosync_upload`）。
  - **应用云端**：当云端更新（REMOTE_AHEAD）时可用，把云端位置写入本地进度库，下次开书生效
    （`kosync_apply_remote`）。
  - **验证账号**：请求 `/users/auth` 校验服务器与账号（`kosync_verify`）。
  Actions: **Sync** fetches and compares the remote record without touching local progress;
  **Upload local** writes local progress to the server; **Apply remote** appears when the remote is
  ahead and stores it locally (takes effect on the next open); **Verify account** checks the server
  and credentials via `/users/auth`.
- 状态区显示 `kosync_status_t.message`、本机/云端百分比、远端设备名，并按结果提示
  （REMOTE_AHEAD / LOCAL_AHEAD / SYNCED / REMOTE_MISSING）。
  The status card shows the message, local/remote percentage and remote device name, plus the
  outcome (remote ahead / local ahead / synchronized / no remote record).

## OPDS 书库 / OPDS library

- 服务器列表：最多 6 台，显示名称与地址；点选进入浏览，长按或“编辑”按钮修改，“新增”添加，
  “删除”需确认。每台可填名称、地址、可选用户名/密码（HTTP Basic）。
  Server list: up to six servers with name and URL. Tap to browse; long-press or Edit to modify;
  Add creates a new one; Delete asks for confirmation. Each server has a name, URL and optional
  HTTP Basic credentials.
- 浏览：显示 feed 标题与条目。NAVIGATION 条目（带 ›）进入下一级；BOOK 条目显示作者/大小，点按
  弹出确认后下载 EPUB。底部提供“返回上一级”（导航栈非空时）、“刷新”和“服务器”。
  Browse: shows the feed title and entries. Navigation entries (with ›) open the next level; book
  entries show author/size and ask for confirmation before downloading. Back (when the navigation
  stack is non-empty), Refresh and Servers are at the bottom.
- 下载：EPUB 下载到书籍目录（设置中的书籍根目录，默认 `/sdcard/books`），临时文件校验通过后
  才原子改名；完成后通知书架重扫，并在状态行显示保存的文件名。
  Downloads go into the configured book root (default `/sdcard/books`); a temporary file is
  validated and atomically renamed before it appears, the shelf is rescanned and the saved file
  name is shown.
- 进度：下载中显示百分比与已下载/总字节。
  While downloading the page shows percentage and downloaded/total bytes.

## 配置与凭据存哪里 / Where configuration lives

- 只保存在书籍目录下的 `.readpico/net/`（Pico 默认 `TF:/books/.readpico/net/`）：`kosync.json`、`opds.json`。
  Book-directory only (Pico default `TF:/books/.readpico/net/`): `kosync.json` and `opds.json`.
- **不写入 NVS，也不进入 TF 设置备份**。密码落盘前用设备派生密钥做 XOR+hex 混淆，运行时明文只在
  内存；页面离页/锁屏/卸卡会清零编辑缓冲。
  Never written to NVS or the settings backup. Passwords are XOR+hex obfuscated with a device-derived
  key before writing; plaintext exists in RAM only, and edit buffers are cleared on leave/lock/media loss.
- 因此换卡或删掉 `/.readpico` 目录后需重新配置；导出设置备份不会带走这两个功能。
  Removing the card or the `/.readpico` folder therefore loses both configurations; the settings
  backup does not carry them.

## 网络行为 / Networking

- 两者都按需拉起共享的 WiFi STA 连接（`read_pico_transfer` 的 network-only 会话），HTTPS 前自动
  联网对时；退出页面、锁屏、卸卡会取消并汇合后台任务（`cancel_join`），停止其持有的网络。
  Both raise the shared WiFi station link on demand (the transfer component's network-only session)
  and synchronise the clock before HTTPS. Leaving the page, locking or losing the card cancels and
  joins the worker and releases the network it owned.
- WiFi 会话期间蓝牙翻页器可能暂停；任务结束后释放。
  The Bluetooth page-turner may pause while WiFi is active and is released afterwards.
- 服务器当前需要 HTTPS 证书校验；不提供关闭校验的开关。
  Server connections use the built-in HTTPS certificate bundle; there is no switch to disable checks.

## 限制 / Limitations

- KOReader 同步仅 EPUB；文档匹配方式在换版本/改文件名后可能导致无法对上，可切换匹配方式重试。
  KOReader sync is EPUB-only. Changing editions or renaming a file may break matching; switch the
  matching mode and retry.
- OPDS 只支持 OPDS 1.2 Atom 与 HTTP Basic；不支持 OAuth/OAuth2、LCP/DRM、OPDS 2.0 JSON、
  OpenSearch 搜索、间接获取（indirect acquisition）与封面抓取。
  OPDS supports OPDS 1.2 Atom with HTTP Basic only: no OAuth/OAuth2, LCP/DRM, OPDS 2.0 JSON,
  OpenSearch, indirect acquisition or cover fetching.
- 服务器证书错误、时间未同步、服务器需要登录等错误会在状态区显示；401 提示检查用户名与密码。
  Certificate errors, unsynchronised time and login-required servers are reported in the status
  line; HTTP 401 asks to check the username and password.
- 同步与下载在后台任务里执行，页面只轮询状态；同一时刻只能有一个任务。
  Sync and download run in background workers while the page polls status; one task at a time.
- 支持设备：Pico 与 S31 两份固件；两板共用同一套页面与后台模块。
  Supported devices: both the Pico and S31 builds share the same pages and background modules.

## 故障排查 / Troubleshooting

- “请先配置 KOReader 同步账号”：进入编辑配置填写服务器、用户名、密码后保存。
  "Configure the KOReader sync account first": fill server, user and password in Edit config, save.
- “WiFi 连接超时/启动失败”：到 设置 → WiFi 重新配网，确认路由器 2.4 GHz 可用。
  "WiFi timeout/start failed": reconfigure WiFi under Settings → WiFi and check the 2.4 GHz router.
- “HTTPS证书校验失败/连接失败”：检查设备时间（联网对时）、服务器域名与网络；不要使用自签证书
  服务器（当前不支持导入自定义根证书）。
  Certificate/connection failures: check the clock (online time sync), server host and network;
  self-signed servers are not supported because custom root certificates cannot be imported.
- OPDS 空目录：确认服务器返回 Atom feed 且条目带 acquisition 链接；未知条目会被安全跳过。
  Empty OPDS feed: confirm the server returns an Atom feed with acquisition links; unknown entries
  are skipped safely.
- 下载失败/文件不出现：检查 TF 卡剩余空间与书籍目录权限；校验失败的文件不会落盘。
  Download failures or missing files: check free space and the book directory; files that fail
  validation are never published.
