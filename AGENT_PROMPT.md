# 给 AI Agent 的移植提示词（整段复制给模型）

下面的内容可以直接作为一条 prompt 发给任意编码 Agent。它假定 Agent 能读仓库文件、能跑 gcc。

---

你在帮我把一个**便携 C99 骰子渲染库**移植到我的硬件上。库在我当前的仓库里，目录结构如下：

```
Dice.h  Dice.c          库本体：端口抽象 + 物理（移动/撞边/互撞/阻尼）+ 逐行渲染工具 + 点阵数字
models/D20.c/.h         正二十面体（对面点数和 21）
models/D6.c/.h          立方体（对面点数和 7）
models/Poly.c/.h        通用多面体引擎（D4/D8/D10/D12 共用）
models/D4.c/.h D8 D10 D12
test/port_host.c        端口模板：用内存像素数组当屏幕（**你写移植层时照它抄**）
test/dice_smoke.c       宿主自测：验证库的契约，不需要单片机
test/run.sh run.cmd     一条命令编译 + 跑自测
README.md               接口/配置/型号参数（第 3 节是端口定义，第 5 节是配置宏）
```

## 目标

写出**一个移植层文件**（比如 `port_myboard.c`），让库在我的 MCU + 屏上跑起来。
除此之外**不要改库的任何文件**。

## 硬性约束（违反任何一条都算做错）

1. **只新增移植层文件**，不要改 `Dice.c` / `Dice.h` / `models/*`。真觉得库有缺陷，先说清楚是缺陷，再等我同意。
2. 端口就是 `dice_port_t` 的 6 个成员：`width`、`height`、`fill`、`flush_row`、`tick_ms`、`rgb`。
   - `width`/`height` 是**屏的真实横竖像素数**，在屏初始化之后从驱动里读，不许写死、不许填反。
   - `fill(x0,y0,x1,y1,c)` 是**闭区间**矩形填充；画单点就是 `fill(x,y,x,y,c)`。
   - `flush_row(x,y,len,pixels)` 必须实现成**从 (x,y) 起连续写 len 个像素**。
     绝不能在它内部逐点设光标/CASET-RASET —— 那是这个库唯一要求你别做的事，否则每帧开销爆炸。
   - `tick_ms()` 返回毫秒（`HAL_GetTick()` 之类）。`rgb(R,G,B)` 收 RGB888、返回本屏像素
     （RGB565 就用位运算打包；RGB888 屏原样返回）。
3. 颜色参数一律 **RGB888**（`0xFF0000` = 红）。库内部不认任何屏的像素格式。
4. 主循环里调 `dice_event_update()`（库自己读 `tick_ms` 并限帧）或固定节奏调 `dice_update(dt)`。
   **两者都不阻塞、不睡**，不要在外面再套 `HAL_Delay`。
5. 不要引入 `malloc`、不要开线程/中断、不要把 HAL 头文件塞进库。
6. 屏比 `DICE_ROW_MAX`（默认 1024）宽时库会报 `DICE_ERR_ROW` 并裁掉右边；不够就 `-DDICE_ROW_MAX=你的屏宽`。
7. 内存预算：库静态占用约 2KB（行缓冲）+ 每颗骰子约 100B + 每个型号几百字节；
   不要为它加帧缓存（除非你确实要遮挡顺序/半透明，那是另一个话题）。

## 步骤

1. 先读 `README.md` 第 2、3、5、7 节，再读 `test/port_host.c`（这是范例，40 行左右）。
2. **先跑通基线**：`sh test/run.sh`（Windows `test\run.cmd`）。必须看到 `PASS: 0 check(s) failed`。
   跑不过就别往下走，先把环境（gcc、`-lm`）修好。
3. 复制 `test/port_host.c` 成你的 `port_myboard.c`，只改四个函数体，把内存数组换成你的屏写函数。
4. 把它加进你的工程（Keil/Make/CMake 都行），在 `main()` 里：屏初始化 → `dice_init(my_port())` → 投一颗 →
   主循环 `dice_event_update()`。
5. **分两步上硬件**：
   a. 先投一颗**静止**的骰子（`p->vx = p->vy = 0`，自转可以给 0），确认屏幕出现一个正二十面体。
      这一步只验证 `fill`/`flush_row`/分辨率/像素格式，别急着看动画。
   b. 再给速度和自转，确认它在动、撞边反弹、静止后不再刷屏。
6. 报错先看 `dice_last_error()`。

## 验收标准（逐条回答，不要只说"好了"）

1. `sh test/run.sh` 输出 `PASS: 0 check(s) failed`。
2. 你的工程编译 **0 warning**（至少库和移植层是 0 warning）。
3. 静态一颗 D20：形状正确、点数 1..20 各一次、棱线是提亮的、背景干净。
4. 运动中：骰子后面**没有底色横条/残影**；撞边能反弹；停下后（`DICE_REALISTIC=1`）不再刷新。
5. 两颗以上时它们**不重叠**（库用圆形保护 + 动量互撞保证）。
6. `dice_value(p)` 返回的值等于"屏上占据投影面积最大的那个面"的点数。
7. 给出实测：Flash/RAM 增量、每帧大致耗时（或帧率）。

## 常见故障 → 原因

| 现象 | 原因 |
| --- | --- |
| 全屏黑/花的，什么都没有 | FMC/SPI 时序；或 `rgb()` 返回格式和屏不符；或骰子画在屏外 |
| 骰子后面有底色横条 | `flush_row` 没真的连续写，或者坐标按半开区间处理了 |
| 骰子一闪一闪/几秒后越擦越大 | `fill`/`flush_row` 越界写；或你自己在库里加了"每帧擦旧矩形"的逻辑 |
| 只有轮廓、没有面 | 你的 `fill` 只写了点；或者 `rgb()` 把颜色压成了同色 |
| 数字是方块/乱码 | 点阵数字走的是 `dice_pixel_t`，检查 `DICE_PIXEL_MODE` 和 `dice_pixel_t` 位宽是否一致 |
| 越跑越慢 | `flush_row` 里逐点设光标；或每帧自己 `lcd_clear()` 整屏 |
| 静止后还在刷 | 你多调了什么重绘；库对全静止的骰子不画画 |
| `dice_spawn()` 返回 NULL | 看 `dice_last_error()`：`DICE_ERR_FULL`（`DICE_MAX` 满了）/ `DICE_ERR_SIZE`（直径 <24 或 >600）/ `DICE_ERR_POS`（中心在屏外） |

## 不要做的事

- 不要为了"更好看"改库的渲染逻辑、加阴影开关、加帧缓存、加第二套 API。
- 不要复制 `models/*.c` 里的渲染代码到自己文件里 —— 要新骰子就新写一个 `models/MyDie.c`。
- 不要用 `HAL_Delay` 控制帧率。
- 不要假设屏是 240x320；一切按 `width`/`height` 算，布局不要写死坐标。

## 给你的硬件生成自测（照 `test\dice_smoke.c` 抄）

`test/` 里那份自测只证明了库在 PC 上自洽，**没有证明你的屏**。所以上硬件的第一件事，是生成一份
`test_hw.c`，结构、检查项、输出格式都照 `test/dice_smoke.c` 走 —— 只把端口从 `host_port()`
换成你自己的 `my_port()`，再加几条只有真屏能测的检查。

**必须保持一致的部分**（这样结果可以直接和 PC 上那份对比）：

- 同样的 `CHECK(cond)` 宏：失败打印 `FAIL 文件:行 表达式`，计数 `s_fail`；
- 同样的编号顺序和检查项（见下表 1~7），一个都不要删；
- 最后一行 `printf("%s: %d check(s) failed\n", s_fail ? "FAIL" : "PASS", s_fail);`
- `return (s_fail != 0);` —— 让构建脚本/CI 能靠返回值判断成败。

**沿用 PC 版的 7 组契约检查**（同一份 `dice_smoke.c` 里就这么写的）：

| # | 检查 | 期望 |
| --- | --- | --- |
| 1 | `dice_init(0)` 后 `dice_spawn(...)` | 返回 0，`dice_last_error() == DICE_ERR_PORT` |
| 2 | `dice_init(my_port())` | `DICE_OK` |
| 2b | `my_port()->rgb(0xFF0000)` | 565 模式 `0xF800` 且 `sizeof(dice_pixel_t)==2`；888 模式 `0xFF0000` 且 `==4` |
| 3 | 直径 10 / 中心在屏外的 spawn | `DICE_ERR_SIZE` / `DICE_ERR_POS` |
| 4 | 六种型号各投一颗（直径 46，给速度） | 都非 0 |
| 5 | 再投一颗 | `DICE_ERR_FULL` |
| 5' | 跑 900 帧 `dice_update(1/60)` | 无 NaN、坐标在屏内、`dice_value()` 落在 1..型号面数 |
| 6 | `dice_kill()` / `dice_clear()` | `alive == 0`，清屏后还能重新投满 |
| 7 | 60 帧 `dice_event_update()` | 不崩、屏上有东西 |

**再加 6 条只有真屏能测的检查**（编号 8~13，请逐条打印数字，不要只说"通过"）：

| # | 检查 | 做法 / 期望 |
| --- | --- | --- |
| 8 | 分辨率与坐标方向 | `my_port()->width/height` 与屏驱动报告一致；在 (0,0)、(w-1,0)、(0,h-1)、(w-1,h-1) 各画一个红点，四个角都要对（错了就是 width/height 填反或坐标原点不同） |
| 9 | 颜色通道 | `fill` 三条横带：纯红、纯绿、纯蓝。屏上是红/绿/蓝而不是红蓝互换 → `rgb()` 打包正确 |
| 10 | `flush_row` 连续性 | 画一条 64px 宽的横向条纹（每 4px 交替两色），不能错位/跳行/撕裂 → 说明真的是连续写，而不是逐点设光标 |
| 11 | 行缓冲上限 | 屏宽 ≤ `DICE_ROW_MAX`；否则 `dice_last_error() == DICE_ERR_ROW`（那就 `-DDICE_ROW_MAX=<屏宽>`） |
| 12 | 一帧耗时 | 用 `my_port()->tick_ms()` 量：单颗一次 `dice_update(1/60)` 的毫秒数、六颗再一次；打印出来（顺带得到实际帧率上限） |
| 13 | 无残影 | 一颗骰子沿对角线走一段后停下，屏上只剩骰子本身：背景没有拖影/横条（有横条就是 `flush_row` 不连续或坐标半开区间） |

**输出方式**：有串口就 `printf`（重定向到 UART，115200 起）；没有串口就把结果画在屏上（先 `lcd_clear`，
再 `lcd_show_string`），或者用两个 LED 表示 PASS/FAIL；两者都没有，就把 `s_fail` 放进全局变量用调试器看。

**骨架**（照这个填，端口换掉即可）：

```c
#include <stdio.h>
#include <stdint.h>
#include "Dice.h"
#include "D20.h"
#include "D6.h"
#include "Poly.h"
#include "D4.h"
#include "D8.h"
#include "D10.h"
#include "D12.h"

extern const dice_port_t *my_port(void);        /* 你的移植层 */
extern void my_flush_now(void);                 /* 需要的话：把本帧推给屏 */

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); s_fail++; } } while (0)

static const dice_model_t *const s_model[6] = { &dice_d20, &dice_d6, &dice_d4, &dice_d8, &dice_d10, &dice_d12 };
static const int s_faces[6] = { 20, 6, 4, 8, 10, 12 };

int test_hw(void)
{
    dice_t *d[6] = { 0 };
    uint32_t t0, t1;
    int i, f;

    /* 1~7: 直接照 test/dice_smoke.c 抄, 只把 host_port() 换成 my_port() */
    ...

    /* 8: 四角红点 */
    my_port()->fill(0, 0, 0, 0, my_port()->rgb(0xFF0000));
    my_port()->fill(my_port()->width - 1, my_port()->height - 1,
                    my_port()->width - 1, my_port()->height - 1, my_port()->rgb(0xFF0000));
    my_flush_now();
    delay_ms(800);                              /* 用你屏的延时, 肉眼确认 */

    /* 12: 单颗一帧耗时 */
    t0 = my_port()->tick_ms();
    dice_update(1.0f / 60.0f);
    t1 = my_port()->tick_ms();
    printf("single die: %lu ms/frame\n", (unsigned long)(t1 - t0));

    printf("%s: %d check(s) failed\n", s_fail ? "FAIL" : "PASS", s_fail);
    return (s_fail != 0);
}
```

**顺便给出的结论也算交付**：Flash/RAM 增量（`size`/map 文件）、第 12 条测到的 ms、第 13 条有没有残影。

## 交付

1. `port_myboard.c`（+ 必要时 `.h`），带一行注释说明适配的 MCU 和屏型号。
2. 你的工程里新增/修改了哪些文件、怎么编译。
3. 上面 7 条验收标准的逐条结果，包括失败项的原因。
