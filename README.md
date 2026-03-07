# Paper v1.1

把纸间书摘里收藏的书摘，推送到 M5Paper 墨水屏上慢慢回顾。

---

## 📦 你需要准备什么

- **M5Paper 墨水屏设备**（SD 卡中已预装好固件和字体文件）
- **纸间书摘 App**（v4.4.0 及以上版本）
- 手机和 M5Paper 连接到**同一个 WiFi 网络**

## 🛜 M5Paper 配网

第一次使用时，M5Paper 开机后会自动进入 WiFi 配置页面。

1. 屏幕上会列出附近的 WiFi 网络，选择你家里/办公室的 WiFi
2. 用屏幕上的触摸键盘输入密码，点击「连接」
3. 连接成功后自动跳转到主页，屏幕上会显示设备的 **IP 地址**——记住它，后面要用

> 💡 如果密码输错了，屏幕会提示「连接失败，请检查密码」，重新输入就好。
>
> 💡 之后想换 WiFi？在主页长按中间按键，可以重新进入配网。

## 📱 在纸间书摘 App 中设置

打开纸间书摘，进入 **设置 → API 集成**，在页面底部找到 **Paper v1.1** 卡片，点进去。

### 设备

| 设置项 | 说明 |
|-------|------|
| **设备地址** | 填入 M5Paper 主页上显示的 IP 地址，比如 `192.168.1.100` |

### 回顾

这里控制书摘推送到设备后，以什么样的方式来回顾：

| 设置项 | 选项 | 说明 |
|-------|------|------|
| **排序方式** | 随机 / 顺序 | 默认「随机」。选「顺序」后会多出一个排序方向的选项 |
| **排序方向** | 从旧到新 / 从新到旧 | 仅在排序方式为「顺序」时显示，默认「从旧到新」 |
| **书摘来源** | 全部书籍 / 选择特定书籍 | 默认推送所有书的书摘。也可以只选几本书 |
| **标签筛选** | 未设置 / 选择特定标签 | 可以按标签过滤，只推送带有指定标签的书摘 |
| **自动切换** | 5 分钟 / 10 分钟 / 30 分钟 / 1 小时 / 2 小时 | 墨水屏上自动翻页的间隔，默认 10 分钟 |

> 💡 书摘来源和标签筛选可以搭配使用，比如只推送某几本书中带「精华」标签的书摘。

## 🔄 同步书摘到设备

设置好之后，点击页面底部的 **「同步到设备」** 按钮。

同步过程中按钮会显示进度：

```
正在获取书摘... → 正在准备数据... → 正在同步 128/500 条书摘... → ✓ 已同步 500 条书摘
```

看到打勾和「已同步 xxx 条书摘」就说明搞定了。

> ⚠️ 同步前请确保手机与 Paper 设备处于同一 WiFi 网络，否则会连接失败。
>
> ⚠️ 每次同步会把筛选后的书摘**全量推送**到设备，不是增量更新。新增了书摘或者改了筛选条件，重新同步一次就行。

## 📖 在墨水屏上阅读

同步完成后，M5Paper 主页会显示书摘数量（比如「书摘: 500 条 · 3 本书」）。

点击 **「开始阅读」** 进入阅读模式：

- 每一屏显示一条书摘，底部会标注书名、作者和页码
- **翻到下一条**：向左滑动 或 按右侧按键
- **回到上一条**：向右滑动 或 按左侧按键
- **返回主页**：按中间按键

到了设定的自动切换时间，屏幕也会自己翻到下一条，不用一直盯着操作。

## 🔌 API 文档

M5Paper 设备在主页状态下会运行一个 HTTP 服务（端口 80），纸间书摘 App 就是通过这些接口来推送数据的。如果你想自己写脚本或做二次开发，可以直接调用。

> 所有接口的 Content-Type 均为 `application/json`，仅限局域网访问。
>
> 下文中 `{IP}` 代表设备 IP 地址，比如 `192.168.1.100`。

---

### GET /api/status

查询设备当前状态。

**请求示例**

```bash
curl http://{IP}/api/status
```

**响应**

```json
{
  "ip": "192.168.1.100",
  "excerptCount": 42,
  "bookCount": 8,
  "sdFreeKB": 15200,
  "version": "1.0.0"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `ip` | string | 设备 IP 地址 |
| `excerptCount` | int | 当前已同步的书摘条数 |
| `bookCount` | int | 当前已同步的书籍数 |
| `sdFreeKB` | int | SD 卡剩余空间（KB） |
| `version` | string | 固件版本号 |

---

### POST /api/sync

一次性同步全部数据（适合书摘数量不多的情况）。调用后设备上的旧数据会被完全替换。

**请求示例**

```bash
curl -X POST http://{IP}/api/sync \
  -H "Content-Type: application/json" \
  -d '{
    "books": [
      { "id": 1, "name": "小王子", "author": "圣埃克苏佩里" }
    ],
    "excerpts": [
      {
        "id": 101,
        "bookId": 1,
        "content": "所有的大人都曾经是小孩，虽然，只有少数的人记得。",
        "idea": "写在扉页上的话",
        "chapter": "作者献辞"
      }
    ],
    "reviewSettings": {
      "sortRule": 1,
      "sortOrder": 0,
      "autoSwitchMinutes": 10
    }
  }'
```

**请求体字段**

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `books` | array | 是 | 书籍列表 |
| `books[].id` | int | 是 | 书籍 ID |
| `books[].name` | string | 是 | 书名（最长 127 字符） |
| `books[].author` | string | 是 | 作者（最长 63 字符） |
| `excerpts` | array | 是 | 书摘列表 |
| `excerpts[].id` | int | 是 | 书摘 ID |
| `excerpts[].bookId` | int | 是 | 所属书籍 ID |
| `excerpts[].content` | string | 是 | 书摘正文（纯文本，不含 HTML） |
| `excerpts[].idea` | string | 否 | 个人想法/批注，默认空字符串 |
| `excerpts[].chapter` | string | 否 | 章节名（最长 63 字符） |
| `reviewSettings` | object | 否 | 回顾设置 |
| `reviewSettings.sortRule` | int | 否 | 排序方式：`0` 顺序，`1` 随机（默认 `1`） |
| `reviewSettings.sortOrder` | int | 否 | 排序方向：`0` 从旧到新，`1` 从新到旧（默认 `0`） |
| `reviewSettings.autoSwitchMinutes` | int | 否 | 自动切换间隔，单位分钟，范围 1-1440（默认 `10`） |

**成功响应** — `200 OK`

```json
{ "status": "ok" }
```

**错误响应**

| 状态码 | 响应 | 原因 |
|--------|------|------|
| 400 | `{"error": "empty body"}` | 请求体为空 |
| 400 | `{"error": "invalid JSON: ..."}` | JSON 格式错误 |
| 500 | `{"error": "SD write failed"}` | SD 卡写入失败 |

---

### 批量同步（适合大量书摘）

当书摘数量较多时，建议用批量同步的方式，分三步完成。每批最多发送 200 条书摘。

#### 第一步：POST /api/sync/begin

发送书籍信息和回顾设置，开始一次同步会话。

```bash
curl -X POST http://{IP}/api/sync/begin \
  -H "Content-Type: application/json" \
  -d '{
    "books": [
      { "id": 1, "name": "小王子", "author": "圣埃克苏佩里" },
      { "id": 2, "name": "月亮与六便士", "author": "毛姆" }
    ],
    "reviewSettings": {
      "sortRule": 1,
      "sortOrder": 0,
      "autoSwitchMinutes": 10
    }
  }'
```

**成功响应** — `200 OK`

```json
{ "status": "ok" }
```

**错误响应**

| 状态码 | 响应 | 原因 |
|--------|------|------|
| 400 | `{"error": "empty body"}` | 请求体为空 |
| 400 | `{"error": "invalid JSON: ..."}` | JSON 格式错误 |
| 500 | `{"error": "SD write meta failed"}` | 元数据写入 SD 卡失败 |
| 500 | `{"error": "SD write failed"}` | 无法创建书摘数据文件 |

#### 第二步：POST /api/sync/batch

分批发送书摘数据，可以调用多次。

```bash
curl -X POST http://{IP}/api/sync/batch \
  -H "Content-Type: application/json" \
  -d '{
    "excerpts": [
      {
        "id": 101,
        "bookId": 1,
        "content": "所有的大人都曾经是小孩，虽然，只有少数的人记得。",
        "idea": "",
        "chapter": "作者献辞"
      },
      {
        "id": 201,
        "bookId": 2,
        "content": "追逐梦想就是追逐自己的厄运，在满地都是六便士的街上，他抬起头看到了月光。",
        "idea": "全书的主题",
        "chapter": ""
      }
    ]
  }'
```

**成功响应** — `200 OK`

```json
{
  "status": "ok",
  "totalExcerpts": 200
}
```

`totalExcerpts` 是到目前为止累计接收的书摘总数。

**错误响应**

| 状态码 | 响应 | 原因 |
|--------|------|------|
| 400 | `{"error": "no sync in progress"}` | 没有先调用 `/api/sync/begin` |
| 400 | `{"error": "empty body"}` | 请求体为空 |
| 400 | `{"error": "invalid JSON: ..."}` | JSON 格式错误 |

#### 第三步：POST /api/sync/end

告诉设备所有数据已发完，完成同步。

```bash
curl -X POST http://{IP}/api/sync/end
```

**成功响应** — `200 OK`

```json
{
  "status": "ok",
  "totalExcerpts": 500
}
```

**错误响应**

| 状态码 | 响应 | 原因 |
|--------|------|------|
| 400 | `{"error": "no sync in progress"}` | 没有活跃的同步会话 |

---

### DELETE /api/data

清除设备上的所有书摘数据。

```bash
curl -X DELETE http://{IP}/api/data
```

**成功响应** — `200 OK`

```json
{ "status": "ok" }
```

---

### 404 兜底

访问不存在的路径会返回：

```json
{ "error": "not found" }
```

---

## ❓ 常见问题

**Q：同步时提示「请先填写设备地址」？**
回到设置页最上面，在「设备地址」里填入 M5Paper 显示的 IP 地址。

**Q：同步时提示「没有符合条件的书摘」？**
你当前的书摘来源和标签筛选条件组合下，没有匹配的书摘。试试把书摘来源改回「全部书籍」，或者去掉标签筛选。

**Q：同步失败，连不上设备？**
检查这几点：
- 手机和 M5Paper 是否在同一个 WiFi 网络下
- M5Paper 是否已经开机并停留在主页（主页上能看到 IP 地址）
- IP 地址是否填对了（在 M5Paper 主页上核实一下）

**Q：换了 WiFi 后 IP 地址变了怎么办？**
在 M5Paper 主页长按中间按键重新配网，拿到新的 IP 地址后，在 App 里更新一下设备地址，再重新同步。

**Q：能推送多少条书摘？**
没有硬性上限，取决于设备的 SD 卡存储空间。一般几千条书摘完全没问题。
