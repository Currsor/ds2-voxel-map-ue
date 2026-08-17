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

## 使用烘焙地图

1. 在内容浏览器中创建 `Miscellaneous > Data Asset`，类型选择 `VoxelMapDataAsset`。
2. 在关卡中放置 `VoxelMapCollector`，设置生成参数和 `Output Data Asset`，点击 `Bake Voxel Map`。普通默认规模为 `256×128×256`。
3. 性能测试时点击 `Bake Performance Test`，默认生成 `512×128×512` 数据；输出日志会记录 block 数、占用体素数、数据体积、生成和验证耗时。烘焙后保存数据资产。
4. 在关卡中放置或更新 `DS2VoxelMapRenderer`，把数据资产设置给 `Voxel Map Asset`，把后处理材质设置给 `Display Post Process Material`。
5. 后处理材质必须是 `Post Process` 域，并包含名为 `VoxelMapRT` 的 Texture Parameter。
6. 运行游戏后，显示器只加载已烘焙数据，并由第一个玩家控制器自动 Possess；代码会切换到 `Game Only` 输入并禁用观察 Pawn 碰撞。
7. 相机控制使用 Unreal Engine 的 `ADefaultPawn` 输入：`W/A/S/D` 平移，`Q/E` 或 `C/Space` 下降/上升，鼠标直接转向，不需要按住右键。
8. `Display Field Of View` 默认是 `60` 度，可在 Renderer 或蓝图中调整；体素投影会自动按当前玩家视口宽高比补偿，方形 RenderTarget 铺满宽屏时不会横向拉伸。
9. 速度、加速度、减速度和转向响应可在 `MovementComponent` 的 `FloatingPawnMovement` 分类中调整。若由其他 Pawn 管理控制器，关闭 `Auto Possess Display Camera`。



采集完成后可从运行时关卡删除 `VoxelMapCollector`；该 Actor 标记为 Editor Only，不会进入打包游戏。

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
