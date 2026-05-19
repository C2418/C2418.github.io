---
title: Authentik + Cloudflare Access 架构演进报告：从反向代理故障到零信任 OIDC 终局
published: 2026-05-19
description: 深度复盘自托管安全网络重构历程。对比 v2 版反向代理的“死锁”故障与 v3 终局版的 OIDC 零信任架构，打造高价值实战标准文档。
tags: [Authentik, Cloudflare Access, OIDC, 零信任架构, Vaultwarden, 架构演进]
category: 项目日志
draft: false
---

# Authentik + Cloudflare Access 架构演进报告

> **导语**：本报告综合了项目早期的本地代理尝试（v2 版）与最终落地的云端边缘拦截方案（v3 终局版）。通过梳理历史架构中遭遇的死锁问题，对比最终采用的 **Cloudflare Access (门卫) + Authentik (发证局)** 架构，形成了一份具备高度实战意义的安全部署标准。

---

## 架构对比：演进前后的范式转移

| 特性 | v2 历史版本 (本地拦截) | v3 终局版本 (边缘拦截 + OIDC) |
| :--- | :--- | :--- |
| **核心逻辑** | Authentik 既当门卫又当发证局 | **Cloudflare Access 门卫** + **Authentik 发证局** |
| **流量路径** | Tunnel ➡️ Authentik Proxy ➡️ 服务 | Tunnel ➡️ 服务 (由 CF Access 边缘拦截) |
| **协议处理** | 容易产生 HTTPS/HTTP 认知偏差 | **协议全链路自动适配** |
| **稳定性** | 容易陷入重定向死循环 | **金融级稳定性，毫秒级响应** |

---

## 一、 历史架构痛点深挖 (v2 回顾)

在早期尝试中，由于将 Authentik 作为直接的反向代理网关，我们踩到了两个极具代表性的“深坑”：

### 1. 域名回退 Localhost 隔离陷阱
*   **现象描述**：外网新设备访问时，地址栏瞬间被篡改为 `http://localhost:9000/...`。
*   **成因定位**：Authentik Proxy 在触发拦截时，由于未能正确识别原始 `Host` 标头，导致拼装重定向链接时使用了缺省的内网回环地址，将外部用户彻底“隔离”。

### 2. 无限重定向 (ERR_TOO_MANY_REDIRECTS) 踢皮球
*   **技术本质**：
    1. 用户通过 **HTTPS** 访问 Cloudflare。
    2. Cloudflare 通过隧道以 **HTTP** 转发。
    3. Authentik 强制要求 HTTPS，本能抛出 **302** 重定向。
    4. 浏览器收到指令再次发起请求，循环往复。
*   **复现方法**：开启 F12 勾选 `Preserve log`，直接输入 HTTP 端口地址即可复现成百上千个重定向请求。

---

## 破局方案：向 OIDC 联邦认证架构转型

为了彻底解决上述死锁，我们弃用了内建代理功能，采用业界标准的 **OIDC (OpenID Connect)** 协议。

::important
**核心逻辑变更**：剥离拦截权，将“门禁”上移至 Cloudflare 全球边缘网络，Authentik 退居大后方作为纯粹的 **身份提供商 (IdP)**。
::

---

## 二、 终极零信任架构配置全流程

### 1. 隧道重构与物理隔离 (打地基)
在 Cloudflare Zero Trust 的 Tunnels 中，彻底分离流量路径：
- **Vaultwarden 通道**：`vault.example.com` ➡️ `http://192.168.x.x:8000`
- **Authentik 通道**：`auth.example.com` ➡️ `http://192.168.x.x:9000`
*(关键：清除之前配置的所有额外 HTTP Host 标头补丁)*

### 2. Authentik 配置查岗接口 (建发证局)
在 Authentik 后台中建立专门供给 Cloudflare 的 OIDC 提供商：
- **核心参数**：
  - 流程：隐式同意 (`default-provider-authorization-implicit-consent`)
  - 客户端类型：`机密 (Confidential)`
- **重定向 URI**：`https://your-team.cloudflareaccess.com/cdn-cgi/access/callback`

### 3. Cloudflare 登记身份源 (搭桥梁)
在 Cloudflare Zero Trust 平台，新增 **OpenID Connect** 身份提供程序：
- **API 终结点**：
  - `Auth URL`: `https://auth.example.com/application/o/authorize/`
  - `Token URL`: `https://auth.example.com/application/o/token/`
  - `Certificate URL`: `https://auth.example.com/application/o/cloudflare-zero-trust/jwks/`
- **OIDC 声明**：务必手动添加 `openid`、`email`、`profile`。

---

## 三、 最终系统表现与日常维护

### 1. 无缝穿透体验
外部匿名请求访问时，边缘网络会瞬间重定向至 Authentik，验证通过后自动放行直达本地。

::note
**彻底告别了**：Localhost 迷航与无限重定向烦恼。
::

### 2. 垃圾清理与瘦身
强烈建议在 Authentik 中删除早期旧版的应用及 Proxy Provider，保持后台配置的纯净和高效。

### 3. 管理员紧急救援
若遗忘主控密码，需在宿主机执行：
```bash
docker exec -it authentik-server ak create_recovery_key 10 admin_user
```
并通过本地路径 `http://localhost:9000/recovery/use-token/...` 进行安全重置。

---

> **结语**：通过将“门卫”与“发证”职责分离，我们不仅跑通了零信任架构，更获得了一套稳如泰山的自托管安全方案。
