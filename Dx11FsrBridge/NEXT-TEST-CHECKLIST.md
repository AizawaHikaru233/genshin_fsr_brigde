# 下一测试会话验证清单（零 jitter 决定性二分，第三十五轮）

构建：SHA256 `0E2D80F59381BEC5B5E70EB94F052CAAF4ED2F105FD978E1E585546C4A5A0139`（DLL 未改动，含 JITTER_CANCELLATION）。
ini：**`Fsr2Sdk234JitterMode=5`（零 jitter）**；`Hdr=0`、`AutoExposure=0`、`MotionVectorScaleMode=1`、`ForceReset=0`。

## 本轮目的：涂抹源二分（累积存活状态下的决定性测试）
连续多轮配置微调（jitter 3/4、motion ±1、jitter 取消 flag）无效 → 用零 jitter 隔离：
- **涂抹（油画感）消失**（只剩锯齿/单帧感）→ 涂抹来自 **jitter 路径**（采样相位/取值错误）→ 下一轮深挖 jitter（cb0 值语义、相位）；
- **涂抹仍全画面存在** → 涂抹与 jitter 无关 → 锁定 **motion 重投影/历史资源/运行时**（RDNA4 历史描述符 or runtime bug）→ 下一轮：换 OPEN SDK FSR2 host 直连（源码在手，可自验历史），或精调 motion 量级（0.0187 UV≈36px 是否 2× 过大）。

## 观察点
1. 涂抹是否消失/减弱；画面是否恢复"仅锯齿"的清晰锐利感；
2. 若零 jitter 下画面稳定（无抖）→ 进一步说明 jitter 相位是唯一涂抹源；
3. 日志：`jm=5`、`jitter_px=0.000000,0.000000`、`reset=0`。

## 背景（已排除项）
- jitter 模式 3/4（±0.5）、motion 符号 ±1920、JITTER_CANCELLATION flag：均无好转；
- context 转储（512B）全零：相机参数不在 context（放弃该路径）。