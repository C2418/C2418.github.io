---
title: 自托管密码管理：Vaultwarden Docker 部署与自动化运维保姆级教程
published: 2026-05-18
description: 从零开始教你如何在 Docker 环境下部署 Vaultwarden，并配合 PowerShell 实现自动化数据备份，构建稳固的个人密码管理基石。
tags: [Vaultwarden, Docker, 实战教程, 数据备份, 运维]
category: 项目日志
draft: false
---

# 自托管密码管理：Vaultwarden Docker 部署与自动化运维保姆级教程

在构建复杂的零信任架构之前，我们需要先搭建一个稳固的后端服务。本文将以 **Vaultwarden** 为例，详细手把手教你如何通过 Docker 进行部署，并实现一套完整的自动化运维体系。

---

## 🛠️ 第一步：环境准备与目录结构

建议在数据盘（如 `D:\Vaultwarden`）创建一个独立的文件夹，以便进行数据持久化和管理。

**推荐目录结构：**
```text
D:\Vaultwarden
├── data/               # 核心数据库与密钥存储
├── .env                # 环境配置文件
└── docker-compose.yml  # 容器编排文件
```

---

## 🐳 第二步：编写 Docker 编排文件

在根目录下创建 `docker-compose.yml`，这是定义容器如何运行的核心。

```yaml
services:
  vaultwarden:
    image: vaultwarden/server:latest
    container_name: vaultwarden
    restart: unless-stopped
    environment:
      - DOMAIN=https://vault.example.com  # 替换为你的域名
    volumes:
      - ./data:/data                      # 挂载宿主机目录实现数据持久化
    ports:
      - "8000:80"                         # 将容器 80 端口映射到宿主机的 8000
    networks:
      - my_network                        # 加入预先定义的网络

networks:
  my_network:
    external: true                        # 建议使用外部网桥以便后期扩展
```

---

## 🔐 第三步：精细化环境变量配置 (.env)

在目录下创建 `.env` 文件，这决定了服务的安全性。以下是实战中最核心的参数解析：

```bash
# 1. 基础配置
DOMAIN=https://vault.example.com
ROCKET_PORT=8000

# 2. 核心安全项（强烈建议这样设置）
SIGNUPS_ALLOWED=false       # 关闭注册：防止外人注册你的密码库
INVITATIONS_ALLOWED=false   # 禁用邀请：进一步收紧权限
ADMIN_TOKEN=your_secure_token_here  # 管理员面板访问 Token

# 3. 存储与功能
DATA_FOLDER=D:/Vaultwarden/data
WEB_VAULT_ENABLED=true      # 启用网页端面板
```

---

## 🚀 第四步：启动服务

打开终端（PowerShell 或 CMD），进入该目录执行：
```powershell
docker compose up -d
```
现在，你就可以通过 `http://localhost:8000` 访问你的私人密码库了！

---

## 💾 第五步：自动化备份实战 (PowerShell)

自托管最重要的就是**备份**。我编写了一套 PowerShell 脚本，可以每天自动冷备份核心数据。

### 1. 编写备份脚本 `backup_vault.ps1`：
```powershell
$source = 'D:\Vaultwarden\data'         # 源数据目录
$destRoot = 'E:\Vaultwarden_Backup'    # 备份存储目录
$date = Get-Date -Format 'yyyy-MM-dd_HHmm'
$destFolder = Join-Path $destRoot $date

# 创建备份文件夹
if (!(Test-Path $destRoot)) { New-Item -ItemType Directory -Path $destRoot }
New-Item -ItemType Directory -Path $destFolder

# 拷贝核心文件
Copy-Item -Path $source\db.sqlite3 -Destination $destFolder
Copy-Item -Path $source\config.json -Destination $destFolder -ErrorAction SilentlyContinue
Copy-Item -Path $source\rsa_key* -Destination $destFolder -ErrorAction SilentlyContinue

# 自动清理：仅保留最近 30 天的备份
Get-ChildItem $destRoot | Where-Object { $_.CreationTime -lt (Get-Date).AddDays(-30) } | Remove-Item -Recurse -Force
```

### 2. 自动化执行
在 Windows **任务计划程序**中创建一个新任务：
- **触发器**：每日凌晨 3:00。
- **操作**：启动程序 `powershell.exe`。
- **参数**：`-ExecutionPolicy Bypass -File D:\Vaultwarden\backup_vault.ps1`。

---

## 💡 总结与避坑

1.  **数据挂载**：务必确认 `./data` 映射到了物理磁盘，否则容器删除后数据会丢失。
2.  **注册控制**：建议先开启注册创建完自己的账号，然后立即将 `SIGNUPS_ALLOWED` 改为 `false` 并重启。
3.  **内网互联**：后期如果配合 Cloudflare Tunnel，只需将隧道容器加入 `my_network`，即可直接通过 `http://vaultwarden:80` 访问。

通过这套流程，你不仅获得了一个功能完整的密码库，更建立起了一套可追溯、可恢复的运维体系。
