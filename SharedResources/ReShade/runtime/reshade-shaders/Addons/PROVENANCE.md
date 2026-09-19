# RenoDX Genshin Add-on — 归档溯源与授权记录

> 本文件是 `renodx-genshin.addon64` 的**权威溯源与再分发授权记录**，
> 随二进制一同归档于 `SharedResources/ReShade/runtime/reshade-shaders/Addons/`。
>
> 2026-09-19 迁移说明：此前该记录位于仓库根目录 `RenoDX-Genshin/UPSTREAM.md`，
> 二进制同时存在根目录与 `SharedResources` 两份（内容相同）。
> 经确认 **`SharedResources` 内为归档副本**，根目录那份已删除，
> 溯源记录随二进制移入本目录，避免"授权记录与二进制分离"。

## 文件

- 文件：`renodx-genshin.addon64`
- 版本：支持原神 7.0 的上游构建（2026-08-13）
- SHA-256：`F4A1ECC739213A4DE34F26F38A5B22EA5F7C114276B6F6D0425F632ED603B760`

## 作者身份

- Bilibili UID：`3461582765951639`
- 授权时的显示名：卡文迪许爱吃香蕉
- 曾用显示名：剪刀妹丽丽
- 作者主页：<https://space.bilibili.com/3461582765951639>
- 原始发布参考：<https://www.bilibili.com/video/av116861345793770/>

## 再分发授权

- 授权日期：**2026-07-14**
- 授权内容：作者明确同意本项目的 GitHub Release 安装包可再分发**未经修改**的
  `renodx-genshin.addon64`，前提是保留作者署名、原始下载/主页参考，
  以及任何适用的许可信息。
- 证据：完整授权请求与作者的肯定答复保存在
  [`NOTICE-RenoDX-genshin-permission.png`](../NOTICE-RenoDX-genshin-permission.png)
- 证据图片 SHA-256：`9DFBD2D0837CC305637FDB4C2C79A013F480686332F334E7DFD926F4ADCDF672`

Bilibili 显示名可能变更；UID `3461582765951639` 是本次授权记录使用的**稳定身份标识**。

## 维护约定

- **保持二进制未经修改**。更新时替换为更新的上游构建，并同步更新上方版本与 SHA-256。
- 打包流程在构建时把本目录的归档文件复制到 ReShade payload
  （见 `Build-OnlineInstaller.ps1` 第 296/302 行、`tools/FpsUnlockInstaller/ReShadeResources.ps1` 第 216 行）。
- 本授权**不转让著作权**，也不授予修改 Add-on 的许可。
- 该授权仅适用于随本项目分发的**未经修改的二进制**，不替代作者另行声明的任何条款。
