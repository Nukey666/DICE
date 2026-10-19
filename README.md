# 多面体骰子库（便携 C99，无 HAL）

A tiny, portable C99 library that renders **spinning 3D platonic dice on a plain 2D framebuffer device**
(works with any MCU that has a fill / write-row API — tested on STM32H750 + FMC 8080 LCD).
No dynamic memory, no RTOS, no HAL, no framebuffer. You give it a screen port; it integrates motion,
handles wall bounces and die-on-die collisions, and draws only the pixels it owns.

- 支持型号：**D20 / D12 / D10 / D8 / D6 / D4**（都是可旋转的 3D 多面体，不是精灵图）
- 一颗骰子 = 一个结构体；位置/大小/颜色/三轴转速/速度全部逐颗可调
- 库负责：时间积分、撞边反弹、动量互撞、地面阻尼、限帧（可选）、擦除与重画
- 型号负责：几何表、点数、姿态、配色；加型号只写一个 `.c/.h`，库和移植层不用改
- 依赖：`<stdint.h>`、`<math.h>`（`-lm`）；C99；静态内存约 2KB（行缓冲）+ 每颗 ~100B

---

## 1. 目录

```
Dice.h  Dice.c          库本体（端口抽象 + 物理 + 逐行渲染工具 + 点阵数字）
models/
  D20.c/.h              正二十面体（对面点数和 21）
  D6.c/.h               立方体（对面点数和 7）
  Poly.c/.h             通用多面体引擎（投影/着光/扫描行/描棱/点数）
  D4.c/.h               正四面体（无对映面，点数 1~4）
  D8.c/.h               正八面体（对面点数和 9）
  D10.c/.h              五角反棱柱对偶 = 10 个风筝面（对面点数和 11）
  D12.c/.h              正十二面体（12 个五边面，对面点数和 13）
test/
  port_host.c           移植模板：内存像素缓冲的端口实现（换屏照它抄）
  dice_smoke.c          宿主自测：在 PC 上验证库的契约，不需要单片机
  run.sh  run.cmd       一条命令编译 + 跑自测
```

`Dice.h` / `Dice.c` 里**不出现任何型号名**，型号只出现在 `dice_spawn()` 的实参和型号自己的头文件里。
所以你可以只拷 `Dice.*` + 需要的 `models/*`。

## 2. 30 秒上手

```c
#include "Dice.h"
#include "D20.h"                    /* 或者 D6.h / Poly.h 里的 dice_d12 等 */

lcd_init();                         /* 你自己的屏初始化 */
dice_init(my_port());               /* 移植层提供的 dice_port_t*, 见第 3 节 */

dice_t *p = dice_spawn(&dice_d20,
                       120, 134,              /* 中心 x,y (像素) */
                       110,                   /* 直径 (像素), 最小 24 */
                       0x8A5CFF,              /* 面色 RGB888 */
                       D20_SPIN_X, D20_SPIN_Y, D20_SPIN_Z,   /* 三轴角速度 rad/s, 负数=反向 */
                       DICE_FLAG_SHADOW);     /* 或 DICE_FLAG_NONE */
if (p) { p->vx = 120.0f; p->vy = -80.0f; }    /* 像素/秒, 库负责撞边反弹 */

while (1)
{
    dice_event_update();           /* 库自己读端口时钟 + 限帧(DICE_FRAME_DELAY_MS), 不阻塞 */
}
```

想自己控节奏（定时器 / RTOS 任务 / 固定 60Hz）：

```c
while (1) { dice_update(1.0f / 60.0f); }      /* 库不读时钟、不限帧、不睡 */
```

## 3. 移植：填一个 `dice_port_t`

**换屏唯一要写的就是这个结构体**（6 个成员），库只通过它碰硬件：

```c
typedef struct
{
    uint16_t width;      /* 横: 一行多少像素（读屏后填，别写死） */
    uint16_t height;     /* 竖: 一列多少像素 */

    /* 矩形填充，闭区间；画一个点就是 fill(x, y, x, y, c) */
    void (*fill)(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, dice_pixel_t color);

    /* 从 (x,y) 起连续写 len 个像素；库逐行合成后整行交过来（等价 LVGL 的 flush） */
    void (*flush_row)(uint16_t x, uint16_t y, uint16_t len, const dice_pixel_t *pixels);

    /* 单调递增毫秒时钟（只在便捷版 dice_event_update() 里用） */
    uint32_t (*tick_ms)(void);

    /* RGB888 -> 本屏像素。只在颜色变化时调用，不参与逐像素运算 */
    dice_pixel_t (*rgb)(uint32_t rgb888);
} dice_port_t;
```

要点：

- `width`/`height` 是**这块屏当前的横、竖分辨率**（竖屏 240×320 就 `width=240, height=320`）。
  库的撞边、布局、刷窗全按它算，**填反了骰子会撞错边**。
- `flush_row` 不能省：库是"先在一行缓冲里合成，再整行交给你"，这样才能做到一个像素一帧只写一次（不闪）。
- 需要 ≥2 字节/像素；`DICE_PIXEL_MODE` 选 565（16 位）或 888（32 位容器），也可以用
  `-DDICE_PIXEL_MODE=2` 编译覆盖，库和型号文件一行都不用改。
- 888 模式下像素是低 24 位的 `0x00RRGGBB`。目标是 LTDC/ARGB8888 且开了 alpha 混合时 A=0 会变透明 ——
  在移植层 `rgb()` 里 `| 0xFF000000` 即可（库只在 `rgb()` 这一处产生颜色，补一次全局生效）。
- 屏宽超过 `DICE_ROW_MAX`（默认 1024）时右边会被裁，`dice_last_error()` 返回 `DICE_ERR_ROW`。

可以直接拿 `test/port_host.c` 当模板抄（它就是内存版），或者看本库原工程里的 FMC 屏实现思路：
首行开一次窗 `set_window(x, y, len, 屏高 - y)`，后续连续行直接连写，省掉每行 CASET/RASET。

## 4. API

| 函数 | 说明 |
| --- | --- |
| `void dice_init(const dice_port_t *port)` | 装屏。必须在屏初始化之后、只调一次 |
| `void dice_update(float dt)` | 推进一帧（秒）。库不读时钟、不限帧；`dt ≤ 0` 忽略，`dt` 上限 0.1s |
| `void dice_event_update(void)` | 便捷版：用端口 `tick_ms` 算 dt + 按 `DICE_FRAME_DELAY_MS` 限帧 |
| `dice_t *dice_spawn(model, cx, cy, dia, color, sx, sy, sz, flags)` | 投放一颗，失败返回 `NULL`（原因见 `dice_last_error()`） |
| `void dice_kill(dice_t *d)` | 收回一颗，后面的补上槽位；旧句柄 `alive` 立刻变 0 |
| `void dice_clear(void)` | 全部收回 + 刷回型号底色 + 复位型号内部计数 |
| `uint8_t dice_value(const dice_t *d)` | 这一颗此刻的"值"（型号定义，例如屏幕投影面积最大的面），不支持返回 0 |
| `uint8_t dice_last_error(void)` | 上一次 `init`/`spawn` 的结果：`DICE_OK` / `DICE_ERR_PORT`(没装端口) / `DICE_ERR_MODEL` / `DICE_ERR_POS`(中心在屏外) / `DICE_ERR_SIZE`(直径 <24 或大于屏) / `DICE_ERR_FULL`(`DICE_MAX` 满) / `DICE_ERR_ROW`(屏宽 > 行缓冲) |

应用可直接改的字段（其余由库/型号维护，别动）：

```c
p->vx, p->vy;        /* 速度, 像素/秒 */
p->spin[0..2];       /* 三轴角速度, rad/s; 符号=转向, 0=该轴不转 */
p->color;            /* 面色 RGB888 */
p->flags;            /* DICE_FLAG_SHADOW / DICE_FLAG_NONE */
p->dia; p->x; p->y;  /* 改尺寸/位置会导致擦除范围对不上, 不建议运行中改 */
```

## 5. 配置（全部 `#ifndef`，可 `-D` 覆盖）

| 宏 | 默认 | 作用 |
| --- | --- | --- |
| `DICE_MAX` | 6 | 最多同时几颗 |
| `DICE_FRAME_DELAY_MS` | 8 | 便捷版限帧间隔（ms） |
| `DICE_BOUNCE_K` | 0.88f | 互撞判定半径兜底系数（型号 `contact_k` 优先） |
| `DICE_RESTITUTION` | 0.9f | 恢复系数 e（互撞与撞墙） |
| `DICE_MASS_EXP` | 3.0f | 质量 = `dia ^ DICE_MASS_EXP` |
| `DICE_REALISTIC` | 1 | 1=开地面阻尼（骰子会停，自转同比衰减）；0=真空台球 |
| `DICE_FRICTION` | 18.0f | 地面减速度 px/s²（恒定减速，不是指数衰减） |
| `DICE_HIT_SPIN` | 0.004f | 停住的骰子被撞后补的角速度 = 线速度 × 它 |
| `DICE_CAM_DIST` | 4.0f | 摄像机距离（模型半径 1），越小透视越强 |
| `DICE_AMBIENT` / `DICE_SPECULAR` | 0.28 / 0.45 | 环境光 / 高光 |
| `DICE_LIGHT_X/Y/Z` | -0.45 / 0.60 / 0.66 | 光照方向（观察坐标系） |
| `DICE_NUM_K` | 25.0f | 点数大小 = 投影半径 / K（调大 = 字更小） |
| `DICE_NUM_AREA_K` | 0.19f | 面积阈值：太小的面不写点数 |
| `DICE_ROW_MAX` | 1024 | 行缓冲最大宽度（像素） |
| `DICE_PIXEL_MODE` | 565 | `DICE_PIXEL_565` / `DICE_PIXEL_888` |

## 6. 型号参数（每颗骰子自己的）

| 型号 | 面 | 对面点数和 | `contact_k` | `friction` | `num_area_k` |
| --- | --- | --- | --- | --- | --- |
| D20 | 20 三角形 | 21 | 0.88 | 18 | 0.19 |
| D12 | 12 五边形 | 13 | 0.89 | 22 | 1.60 |
| D10 | 10 风筝形 | 11 | 0.85 | 24 | 1.20 |
| D8 | 8 三角形 | 9 | 0.90 | 26 | 0.19 |
| D6 | 6 正方形 | 7 | 0.90 | 32 | 1.20 |
| D4 | 4 三角形 | 无对映面 | 0.95 | 40 | 0.19 |

- `contact_k`：互撞判定半径 = 直径/2 × 它。多面体轮廓在 0.79~1.0 外接半径之间变化，1.0 = 绝不重叠但"没碰到就弹开"，
  0.85 左右最贴；调小可能闪一两帧。
- `friction`：地面减速度。面越多越像球、滚得越久（D4 停得最快）。
- `num_area_k`：面小到一定程度就不写字，避免压扁的数字。

## 7. 自测（不开硬件，先在这上面跑通）

```bash
cd DICE
sh test/run.sh            # Windows: test\run.cmd
```

内容是 `gcc -std=c99 -Wall -Wextra -I. -Imodels -Itest test/port_host.c test/dice_smoke.c Dice.c models/*.c -lm`，
跑完打印 `PASS: 0 check(s) failed`。它验证：没装端口时 spawn 失败但不崩、各种错误码、900 帧后不出屏/无 NaN、
`dice_value()` 落在 1..面数、`dice_kill()`/`dice_clear()` 后句柄失效、清屏后能重新投满。

新写型号时把它的头文件加进 `test/dice_smoke.c` 的 `s_model[]` / `s_faces[]`，就能在移植到硬件之前先跑一遍。

## 8. 加一种新骰子（或改现有型号）

1. 新写一个 `models/MyDie.c/.h`，导出一个 `const dice_model_t dice_my`，六个回调见 `Dice.h` 的
   `struct dice_model_s`；几何表写顶点（统一按最大长度归一）和面（每个面几个顶点、绕序朝外）。
2. 面法线用**面内三点叉积**算，不能用"重心方向"当法线（非径向的面会算错，风筝面会变扭曲）。
3. 透视峰值取型号头文件里的 `PROJ_PEAK`（`D20_PROJ_PEAK` / `POLY_PROJ_PEAK`，都是 1.05；
   任意"顶点都在同一个球面上"的多面体通用峰值是 1.0338，取 1.05 留余量）。
4. **点数按面画，不按三角形画**：四边形/五边形面拆成多个三角形填充，但数字和描棱都按整面走，
   否则正方形面上会多出一条对角线、多出一个数字。
5. 点数编号可以自动算：找重心互为相反数的对映面，成对分配 `n` 和 `面数+1-n`，再自检一遍。
6. 擦除范围用"本帧轮廓 ∪ 上一帧轮廓"，**别把两者的并集写回 `d->bx*`**（会每帧扩大，几秒后擦整屏 → 看起来更闪）。

## 9. 给 AI Agent 的移植说明（复制这段给模型）

> 完整版（含约束、验收清单、故障对照表、**"给你的硬件生成自测"模板**）：[`AGENT_PROMPT.md`](AGENT_PROMPT.md)。
> 下面这段是精简版，够用但没那么多验收细节。

> 这是一个便携 C99 骰子渲染库。任务：把它移植到 `<你的 MCU + 屏>` 上。
> - **只写一个移植层文件**，实现第 3 节的 `dice_port_t`（`width/height/fill/flush_row/tick_ms/rgb`），
>   在屏初始化之后调 `dice_init(你的port)`。**不要改 `Dice.c`**；如果非要改，说明是库的缺陷。
> - 颜色参数一律 RGB888；`rgb()` 负责转成本屏像素（565 打包或原样返回）。
> - `flush_row` 必须实现成"连续写 len 个像素"，不要在里面逐点设光标；否则每帧 FMC/SPI 开销会爆炸。
> - 主循环调 `dice_event_update()`（简单）或固定节奏调 `dice_update(dt)`（自己控帧率）。两者都不要阻塞。
> - 先跑 `test/run.sh` 证明编译和逻辑没问题，再上硬件。
> - 报错先看 `dice_last_error()`；`DICE_ERR_ROW` 表示屏比 `DICE_ROW_MAX` 宽，加大这个宏。
> - 屏上出现"骰子后方的横条/闪烁"通常是 `flush_row` 没有真的整行连续写，或者 `fill`/`flush_row`
>   的坐标没按闭区间处理。

## 10. 已知限制

- **没有帧缓存**：每颗骰子只重画自己"本帧 ∪ 上一帧"的轮廓区域，所以两颗真重叠时后画的会把先画的盖掉。
  库用互撞（动量守恒 + 圆形保护半径）保证它们不重叠。要真正的遮挡顺序/半透明/拖尾，得自己做脏行合成。
- **单实例**：全局状态（端口、骰子数组）是静态的，一块屏一个库实例。
- 只做"骰子本体"：旋转、平移、撞边、互撞、阻尼。没有滚动/结算动画、没有音效、没有贴图。
- 数字是 12×6 点阵按浮点倍率放大（不是矢量字）；点数只支持 1~20（两位）。
- 六颗同时运动时每帧的像素量按各自轮廓算，240×320 屏上没量过帧率；卡就调大 `DICE_FRAME_DELAY_MS` 或减 `DICE_MAX`。
- `dt` 上限 0.1s（防断点跳帧），长时间暂停后不会瞬移。
