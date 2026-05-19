---
title: 2418 博客：从 Gmail 侧边栏到顶级域名部署的调试全记录
published: 2026-05-19
description: 记录一次深度的 Astro 博客重构之旅：侧边栏逻辑重写、TOC 性能优化、GitHub Pages 根域名迁移以及那些“血泪”调试瞬间。
tags: [Astro, TailwindCSS, GitHub Pages, Debugging, Refactor]
category: 技术笔记
draft: false
---

# 2418 博客：从 Gmail 侧边栏到顶级域名部署的调试全记录

就在今天，我们对 2418 博客进行了一次“脱胎换骨”的重构。作为一名有“开源精神”的博主，我决定不仅分享心路历程，更要把这次重构中最核心的代码逻辑贴出来供大家参考。

---

## 1. 核心挑战：Gmail 风格 A/B/C 状态侧边栏

我们想要一个既能固定展示、又能折叠省空间、还能在悬浮时灵动扩展的侧边栏。这涉及三个状态的精准切换：

- **状态 A (Pinned)**：宽度固定，内容常驻。
- **状态 B (Collapsed)**：宽度收缩至图标大小。
- **状态 C (Hover Expanded)**：折叠态下，鼠标悬停即浮起扩展。

### 侧边栏样式逻辑 (Tailwind + Vanilla CSS)

为了保证性能，我们大量使用了 `will-change` 和 `cubic-bezier` 贝塞尔曲线：

```css
/* 定义平滑的 Expo 曲线 */
.cubic-bezier-expo {
    transition-timing-function: cubic-bezier(0.16, 1, 0.3, 1) !important;
}

/* 状态 B：折叠态 */
.sidebar-collapsed #sidebar-placeholder,
.sidebar-collapsed #main-sidebar {
    width: 72px;
}

/* 状态 C：折叠下的悬停浮起态 */
.sidebar-collapsed #main-sidebar.is-hovered {
    width: 250px;
    box-shadow: 20px 0 50px -10px rgba(0,0,0,0.12); /* 悬浮阴影，增加层次感 */
    z-index: 50; /* 确保浮在内容之上 */
}
```

### 交互逻辑脚本 (Vanilla JS)

为了实现防抖和持久化，我们使用了简单的 DOM 操作和 `localStorage`：

```javascript
function initSidebar() {
    const toggleBtn = document.getElementById('sidebar-toggle');
    const sidebar = document.getElementById('main-sidebar');
    const container = document.body;

    let isPinned = localStorage.getItem('sidebar-pinned') !== 'false';
    let hoverTimer = null;

    const updateUI = () => {
        isPinned ? container.classList.remove('sidebar-collapsed') 
                 : container.classList.add('sidebar-collapsed');
    };

    // 初始状态恢复
    updateUI();

    // 切换逻辑
    toggleBtn.onclick = () => {
        isPinned = !isPinned;
        localStorage.setItem('sidebar-pinned', isPinned);
        updateUI();
    };

    // 状态 C 的悬停防抖逻辑
    sidebar.onmouseenter = () => {
        if (isPinned) return;
        hoverTimer = setTimeout(() => {
            sidebar.classList.add('is-hovered');
        }, 150); // 150ms 防误触
    };

    sidebar.onmouseleave = () => {
        clearTimeout(hoverTimer);
        sidebar.classList.remove('is-hovered');
    };
}
```

---

## 2. TOC 目录：告别掉帧，拥抱硬件加速

目录高亮指示器的平滑移动是提升博客质感的关键。我们放弃了修改 `top` 值的传统做法，全面转向硬件加速：

```javascript
// TOC 指标更新逻辑
updateActiveState(idx) {
    const activeItem = this.items[idx];
    if (activeItem && this.indicator) {
        const top = activeItem.offsetTop;
        const height = activeItem.offsetHeight;

        // 使用 translate3d 触发 GPU 加速，极其顺滑
        this.indicator.style.transform = `translate3d(0, ${top}px, 0)`;
        this.indicator.style.height = `${height}px`;
        this.indicator.style.opacity = "1";
    }
}
```

同时，我们通过 `#main-scroll-container` 实现了局部滚动监听，避免了污染全局 `window.scroll`。

---

## 3. 部署细节：顶级域名的“坑”与“填”

将博客迁移到 `https://c2418.github.io/` 时，最核心的改动是在 `astro.config.mjs`：

```javascript
// astro.config.mjs
export default defineConfig({
  site: "https://c2418.github.io",
  base: "/", // 从 "/C2418" 迁移回根路径
  // ...
});
```

以及在 GitHub Actions 中处理**时差问题**：

```javascript
// 在构建脚本中强制指定时区，确保“最新更新时间”准确无误
formattedTime = new Date().toLocaleString("zh-CN", {
    timeZone: "Asia/Shanghai", // 强制北京时间
    hour12: false,
    // ...
});
```

---

## 4. 结语

这次重构不仅是代码的更新，更是对 Web 交互细节的一次深入探索。代码本身是有生命力的，希望这些片段能给同样在折腾博客的你一点启发。

**2418 博客，代码全开，正式启航！** 🚀✨

