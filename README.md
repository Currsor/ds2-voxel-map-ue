# DS2 Voxel Map — Unreal Engine 复刻

复刻《死亡搁浅 2》(DEATH STRANDING 2) 的 3D 体素 UI 地图核心渲染技术。

本项目目标：在 **Unreal Engine（预编译版）** 中实现「体素数据 → 公告板 billboard 光追渲染 → 三级 LOD」的**核心渲染 demo**，不包含完整地图 UI（路线规划交互、相机系统、动画、地形变形等作为后续扩展）。

## 技术选型

| 项 | 选择 | 说明 |
|----|------|------|
| 引擎 | Unreal Engine 5.4 / 5.5（Epic 启动器预编译版） | 自定义全局 shader + RDG，无需源码构建 |
| 语言 | C++（模块）+ HLSL（`.usf` 着色器） | 纯蓝图工程无法自定义全局 shader |
| 渲染路径 | 自定义 Global Shader + RenderGraph (RDG) + StructuredBuffer + 实例化 quad | 100% 还原原方案，控制力最强 |
| 地形源 | 高度图 + 颜色图 / 程序化噪声（二选一） | demo 阶段先用简单源验证渲染管线 |

> 结论：**预编译版即可，不需要拉取 UE 源码。** 自定义 `.usf` 着色器、`FGlobalShader`、RDG、RHI StructuredBuffer 均为公开 API，无需修改引擎内部。

## 文档索引

- [01 · 技术路线](./docs/01-技术路线.md) — 两篇参考资料的完整技术拆解
- [02 · 复刻路线](./docs/02-复刻路线.md) — 里程碑 M0–M6 的分步实现计划
- [03 · 实现方案与踩坑](./docs/03-实现方案与踩坑.md) — 实际落地后的方案 + 完整踩坑记录

## 环境准备

- Epic Games Launcher 安装 UE 5.4 或 5.5（建议勾选 Editor symbols for debugging）
- Windows：Visual Studio + Windows SDK；macOS：Xcode + clang
- 新建 **C++ 工程**（非纯蓝图）

## 参考资料

- [GDC Vault — 'DEATH STRANDING 2': Making of Voxel 3D UI Map](https://gdcvault.com/play/1035737/-DEATH-STRANDING-2-Making)（演讲者 Ildar Valeev，KOJIMA PRODUCTIONS）
- [Doris Wu 博客 — Death Stranding 2 Voxel Map Learning](https://show50726.github.io/posts/Death-Stranding-2-Voxel-Map-Learning/)
- [参考 Unity demo 源码](https://github.com/show50726/Death-Stranding-Voxel-Map)
- [Voxel raycasting（3D DDA）](https://www.shadertoy.com/view/4dX3zl)

## 状态

- [x] 技术调研与路线整理
- [x] M0 工程骨架
- [x] M1 体素数据生成
- [x] M2 数据上传 GPU
- [x] M3 顶点着色器
- [x] M4 片元着色器（ray-box + DDA + LOD）
- [x] M5 集成与相机
- [x] M6 优化与打磨
