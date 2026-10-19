/**
 ****************************************************************************************************
 * @file        Dice.h
 * @brief       骰子库(最高实体): 像素格式 + 移植端口 + 骰子结构 + 事件更新
 * @note        颜色统一用 RGB888(0xRRGGBB) 传, 由移植层转成本屏像素格式; 换屏/换色深只改移植层。
 * @attention
 *
 * 一颗骰子 = dice_t: 型号 + 颜色 + 转速 + 位置 + 速度。库自己按时间积分位置、撞边反弹,
 * 型号(dice_model_t)只负责"把这一颗画出来"。本库不认识任何具体形状: 加 D6 就是再写一个
 * dice_model_t 导出来, 库和移植层一行都不用改。
 *
 * 用法:  屏初始化之后(移植层把自己的 dice_port_t 准备好)
 *          dice_init(my_port());                            // 装屏; 底色由型号自己报告(见 dice_model_t)
 *          dice_t *p = dice_spawn(&dice_d20, 120, 134, 120, 0x8A5CFF,
 *                                 1.7f, 2.5f, 0.9f, DICE_FLAG_NONE);
 *          p->vx = 50.0f; p->vy = -30.0f;                   // 像素/秒, 0 = 原地转
 *          while (1) { dice_event_update(); }               // 每调一次推进一帧, 不阻塞
 *          // 想自己控节奏(定时器/RTOS)就直接调 dice_update(dt), 库不读时钟、不限帧
 ****************************************************************************************************
 */

#ifndef __DICE_H
#define __DICE_H

#include <stdint.h>

/******************************************************************************************/
/* 像素格式切换: 只改 DICE_PIXEL_MODE 这一行, 所有型号文件一行都不用动
 *   DICE_PIXEL_565: 16 位 RGB565, ST7789/ILI9341 这类 MCU 屏(每像素 2 字节)
 *   DICE_PIXEL_888: 24 位 RGB888, LTDC/外部大屏(每像素 4 字节容器)
 * 颜色参数永远是 RGB888(0xRRGGBB); 只有行缓冲/上屏用的像素类型跟着变,
 * 移植层的 rgb() 按这个宏决定压不压缩。 */

#define DICE_PIXEL_565  1
#define DICE_PIXEL_888  2

#ifndef DICE_PIXEL_MODE
#define DICE_PIXEL_MODE DICE_PIXEL_565           /* ← 换屏时只改这里; 也可以编译时 -DDICE_PIXEL_MODE=2 */
#endif

#if DICE_PIXEL_MODE == DICE_PIXEL_565
typedef uint16_t dice_pixel_t;                   /* 16 位/像素 */
#elif DICE_PIXEL_MODE == DICE_PIXEL_888
typedef uint32_t dice_pixel_t;                   /* 32 位容器装 24 位色(高 8 位留空) */
#else
#error "DICE_PIXEL_MODE 只能是 DICE_PIXEL_565 或 DICE_PIXEL_888"
#endif

/******************************************************************************************/
/* 移植接口: 横竖分辨率 + 绘制 + 时钟 + 颜色转换。换屏只需要填满这一个结构。
   注意 width/height 必须填**这块屏当前的横、竖分辨率**(竖屏 240x320 就 width=240, height=320),
   不是写死的 320x240/480x272; 库的撞边、布局、刷窗都按这两个值算, 填反了骰子会撞错边。 */

typedef struct
{
    uint16_t width;                             /* 横: 一行多少个像素 */
    uint16_t height;                            /* 竖: 一列多少个像素 */

    /* 矩形填充(闭区间), 画一个点就是 fill(x, y, x, y, c) */
    void (*fill)(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, dice_pixel_t color);

    /* 从 (x,y) 起连续写 len 个像素: 逐行合成后整行上屏, 这一项不能省 */
    void (*flush_row)(uint16_t x, uint16_t y, uint16_t len, const dice_pixel_t *pixels);

    /* 单调递增的毫秒时钟, 库用它限帧和算 dt(库不睡) */
    uint32_t (*tick_ms)(void);

    /* RGB888 -> 本屏像素。只在颜色变化时调用(每帧几十次), 不参与逐像素运算 */
    dice_pixel_t (*rgb)(uint32_t rgb888);
} dice_port_t;

/******************************************************************************************/
/* 骰子: 应用只需要填 位置/速度/颜色/转速, 其余交给库和型号 */

#ifndef DICE_MAX
#define DICE_MAX 6                              /* 最多同时投放几颗 */
#endif

/* dice_last_error() 的返回码: 上一次 dice_init()/dice_spawn() 为什么没成功(成功就是 DICE_OK) */
#define DICE_OK       0                         /* 上一次调用成功 */
#define DICE_ERR_PORT 1                         /* 端口为空或分辨率是 0: 先调 dice_init() */
#define DICE_ERR_MODEL 2                        /* 型号指针为空 */
#define DICE_ERR_POS  3                         /* 中心点不在屏幕内 */
#define DICE_ERR_SIZE 4                         /* 直径小于 24 或大于屏幕 */
#define DICE_ERR_FULL 5                         /* DICE_MAX 颗都占满了 */
#define DICE_ERR_ROW  6                         /* 屏宽超过 DICE_ROW_MAX: 骰子右半边会被裁掉(仍能跑) */

typedef struct dice_s       dice_t;
typedef struct dice_model_s dice_model_t;

/* dice_t.flags: 外观开关, 由型号自己解释; 在 dice_spawn() 里作为参数传入 */
#define DICE_FLAG_NONE      0x0000
#define DICE_FLAG_SHADOW    0x0001              /* 型号自定义: 画地面阴影(会多占一圈擦除区) */

struct dice_s
{
    const dice_model_t *model;                  /* 型号: 调用方传进来的那张函数表 */

    uint16_t dia;                               /* 直径(像素) */
    uint32_t color;                             /* 面色 RGB888 */
    float    spin[3];                           /* 三轴角速度(弧度/秒): 符号=转向, 0=该轴不转 */
    uint16_t flags;                             /* 外观开关 DICE_FLAG_*: 应用设, 型号解释 */
    float    contact_k;                         /* 型号写(应用只读): 接触半径系数, 0 = 用 DICE_BOUNCE_K */
    float    friction;                          /* 型号写(应用只读): 地面减速度(px/s²), 0 = 用 DICE_FRICTION */

    float    x, y;                              /* 中心(像素): 初始化时给, 之后由库积分 */
    float    vx, vy;                            /* 速度(像素/秒): 应用设, 库负责撞边反弹 */

    /* 下面由库/型号维护, 应用不要动 */
    int16_t  px, py;                            /* 上次绘制时的整数中心, 型号用它算擦除范围 */
    int16_t  bx0, by0, bx1, by1;                /* 上次绘制占的矩形(型号填), dice_clear 用它擦 */
    uint8_t  alive;
    float    priv[4];                           /* 型号私有状态(例如三个旋转角), 库不解释 */
};

/* 一种骰子 = 这样一个函数表。加 D6 就在 Dice/D6/ 里实现并导出自己的 dice_model_t */
struct dice_model_s
{
    void (*init)(const dice_port_t *port);      /* 端口 + 几何(只算一次) */
    void (*spawn)(dice_t *d);                   /* 补充本型号的初始状态(姿态等) */
    void (*frame)(dice_t *d, float dt);         /* 画这一颗: 把本帧矩形和 d->bx*(上一帧)一起重画, 画完更新 bx* */

    /* 本型号画的面板底色(RGB888)。库擦骰子/清空时刷这个色, 所以底色只有型号这一个定义 */
    uint32_t (*bg)(void);

    /* 可空: dice_clear() 时复位本型号的内部状态(比如起始姿态计数器), 不需要就填 0 */
    void (*reset)(void);

    /* 可空: 这一颗此刻的"值", 怎么算由型号定义(例如"屏幕投影面积最大的那一面的点数");
     * 库只转发 dice_value(), 不认识点数/面这些概念。返回 0 表示型号不支持或还算不出 */
    uint8_t (*value)(const dice_t *d);
};

/******************************************************************************************/
/* 可调参数: 全部 #ifndef, 所以可以用 -DDICE_MAX=8 这类命令行宏覆盖, 也可在工程里的
   一个 config 头里先定义再 include 本文件, 不必改库源码 */

#ifndef DICE_FRAME_DELAY_MS
#define DICE_FRAME_DELAY_MS 8                   /* 限帧间隔(ms), 越小转得越快 */
#endif

/* 互撞判定半径的**兜底**系数: 直径/2 × 这个系数, 两边相加。
 * 型号会在 spawn 里把自己的 dice_t.contact_k 写进来, 那边优先; 这颗填 0 才用这里的值。
 * 1.0 = 按保护圆(直径的一半)算, 绝不重叠、绝不被对方擦掉, 但骰子轮廓只在棱/顶点朝向对方时
 *       才是外接圆, 多数时候更小, 所以看着"没碰到就弹开"。
 * 0.85 左右看起来才是贴着弹; 代价是两颗都拿棱/顶点朝向对方的那一瞬轮廓超过判定半径,
 *       可能出现一两帧的小片闪烁。按屏上手感在这两个值之间调。 */
#ifndef DICE_BOUNCE_K
#define DICE_BOUNCE_K 0.88f
#endif

/* 恢复系数 e ∈ [0,1]: 1 = 完全弹性(碰撞前后动能不变), 0 = 撞完不再弹开。
 * 骰子互撞和撞屏幕边缘都用它; 真树脂骰子落在硬桌面上大约 0.6~0.7。
 * 屏幕边框当成质量无穷大的刚体, 所以撞墙退化成"法向速度 ×e 反向", 切向不变(库没有摩擦)。 */
#ifndef DICE_RESTITUTION
#define DICE_RESTITUTION 0.9f
#endif

/* 质量 = 直径 ^ DICE_MASS_EXP。形状相似、密度相同时 3 次方才对(直径 2 倍 = 质量 8 倍);
 * 2 对应薄片(质量 ∝ 面积)。觉得小骰子被大骰子撞得太飞就往下调。 */
#ifndef DICE_MASS_EXP
#define DICE_MASS_EXP 3.0f
#endif

/* "真实模拟"总开关: 1 = 地板摩擦 + 自转跟着线速度一起衰减(骰子会停, 停下也不再转);
 * 0 = 真空台球(速度只被撞墙/互撞的恢复系数削一点, 永不停止, 自转恒定)。
 * 只控制持续的阻尼项, 碰撞用的 DICE_RESTITUTION 不受它影响。 */
#ifndef DICE_REALISTIC
#define DICE_REALISTIC 1
#endif

/* 地板摩擦减速度(像素/秒²): 恒定减速(库仑摩擦)才停得下来, 指数衰减永远停不住。
 * DICE_FRICTION=18 时 80px/s 约 4.4 秒停下(滑得动, 但一定会停); 大屏/小屏手感不同, 按屏调。 */
#ifndef DICE_FRICTION
#define DICE_FRICTION 18.0f
#endif

/* 停下后又被撞动, 重新转起来的角速度 = 线速度 × 这个系数(弧度/秒 每 像素/秒)。
 * 停住时 spin 已经被削成精确 0, 不给这一下, 被撞的骰子就只会平移、不转, 看着像在冰上滑。
 * 800px/s → 3.2rad/s, 和正常自转一个量级; 0 = 撞了只平移, 想看转快点就加大。 */
#ifndef DICE_HIT_SPIN
#define DICE_HIT_SPIN 0.004f
#endif

/******************************************************************************************/
/* 多面体骰子通用的渲染常量: 换面数(D4/D6/D8/D12)时只有几何表和面数变, 这些不用改。
   现在的数值是给 240x320 屏调的, 大屏可以整体放大 */

#ifndef DICE_CAM_DIST
#define DICE_CAM_DIST        4.0f       /* 摄像机距离(模型半径为1), 越小透视越强 */
#endif
#ifndef DICE_AMBIENT
#define DICE_AMBIENT         0.28f      /* 环境光强度(0~1), 越大暗面越亮 */
#endif
#ifndef DICE_SPECULAR
#define DICE_SPECULAR        0.45f      /* 高光强度(0~1), 0为哑光 */
#endif
#ifndef DICE_LIGHT_X
#define DICE_LIGHT_X         (-0.45f)   /* 光照方向(观察坐标系, 单位向量): 左上前方 */
#define DICE_LIGHT_Y         0.60f
#define DICE_LIGHT_Z         0.66f
#endif
#ifndef DICE_NUM_K
#define DICE_NUM_K           25.0f      /* 点数放大倍数 = 投影半径 / K, 字高约骰子直径的 24% */
#endif
#ifndef DICE_NUM_AREA_K
#define DICE_NUM_AREA_K      0.19f      /* 面积门限 = K * scale^2(2倍三角形面积), 太小的面不写点数 */
#endif
#ifndef DICE_ROW_MAX
#define DICE_ROW_MAX         1024       /* 扫描行缓冲最大宽度(像素); 屏比这宽时右边被裁掉并置 DICE_ERR_ROW */
#endif
#ifndef DICE_2PI
#define DICE_2PI             6.2831853f /* 2π, 姿态角回绕用 */
#endif

/******************************************************************************************/
/* 函数声明 */

/* 移植层: 本库不自带任何屏幕代码。在你的移植层里提供一个返回 dice_port_t* 的函数
   (参考 test/port_host.c), 屏初始化之后调 dice_init() 传进来即可。 */

/* 装屏。port 必须在 LCD 初始化之后给, 生命周期覆盖整个运行期 */
void dice_init(const dice_port_t *port);

/* 投放一颗: 型号 / 中心 / 直径 / 颜色(RGB888) / 三轴角速度(弧度/秒, 符号=转向) / 外观开关;
   返回句柄(之后还能改 vx,vy,color,spin[],flags), 失败 NULL。
   角速度想按"型号默认的几倍"给, 就传 型号头文件里的默认角速度 * 倍率 */
dice_t *dice_spawn(const dice_model_t *model, uint16_t cx, uint16_t cy,
                   uint16_t dia, uint32_t color,
                   float spin_x, float spin_y, float spin_z, uint16_t flags);

/* 推进一帧: dt 由调用方给(秒), 例如固定 1/60 或自己算的真实间隔。库不用时间源、不限帧。
   想自己控节奏(固定帧率 / RTOS 任务)就用这个; 想省事用下面的 dice_event_update() */
void dice_update(float dt);

/* 便捷版事件更新: 主循环里反复调用。用端口 tick_ms 算 dt, 并按 DICE_FRAME_DELAY_MS 限帧, 未到下一帧立即返回 */
void dice_event_update(void);

/* 收回一颗: 擦掉它占过的位置, 后面的骰子补上这个槽位; d 不属于本库则什么都不做 */
void dice_kill(dice_t *d);

/* 取某一颗此刻的"值"(型号自己定义, 例如"屏幕投影面积最大的那一面的点数");
   型号不支持或 d 为空时返回 0, 主循环里任何时刻都能调 */
uint8_t dice_value(const dice_t *d);

/* 旧名字的兼容宏(拼错的 Vaule); 新代码用 dice_value() */
#define GetDiceVaule(d) dice_value(d)

/* 上一次 dice_init()/dice_spawn() 的结果: DICE_OK 或 DICE_ERR_*; 调试/日志用, 正常流程不用查 */
uint8_t dice_last_error(void);

/* 清空所有骰子: 把它们占过的矩形刷回型号底色, 屏幕其它内容不动 */
void dice_clear(void);

/******************************************************************************************/
/* 多面体骰子的画法工具: 型号只用写"几何表 + 每个面怎么摆点数", 投影/着光/扫描行/点阵都在这
   一套里。加 D6/D8/D12 就是把顶点表和面表换掉, 画法照下面的顺序拼。
   画一颗的顺序(型号的 render 照这个来):
     dice_rot_m() 出旋转矩阵 -> 顶点旋转+透视 -> 剔背面 -> dice_shade() 算颜色
     -> 逐行: dice_canvas_row() 定本行范围, 铺底色/阴影/面(dice_canvas_put、dice_tri_span)
        -> dice_tri_edges() 描棱 -> dice_canvas_digit() 点数字 -> dice_canvas_flush() 上屏 */

/* 欧拉角 -> 3x3 旋转矩阵, R = Rz * Ry * Rx(角度单位: 弧度) */
void dice_rot_m(float m[3][3], float ax, float ay, float az);

/* 按 环境光+漫反射+高光 调一个 RGB888 颜色, 返回本屏像素(颜色只在每个面/棱上算一次) */
dice_pixel_t dice_shade(uint32_t rgb888, float diff, float spec);

/* 把 RGB888 提亮一档(向白色混一半), 用来从面色推出棱边色 */
uint32_t dice_lighten(uint32_t rgb888);

/* 椭圆(圆是 a==b 的特例)在 y 行的半宽; 返回 -1 表示这一行不在形状内。
   圆形/椭圆保护用它算"本行真正要重绘的左右界", 比矩形保护少擦别人的地盘 */
int dice_halfw(int cx, int cy, int a, int b, int y);

/* 点数的放大倍数: max(1.0f, 投影半径 / DICE_NUM_K), 保证小骰子也用 12px 字 */
float dice_num_scale(float radius);

/* 设定本行要重绘的 x 区间(会自动夹进行缓冲), 之后所有 canvas 操作都只写这一行 */
void dice_canvas_row(int x0, int x1);

/* 往当前行缓冲的 x0~x1 涂色(闭区间, 自动裁剪到 dice_canvas_row 给的范围) */
void dice_canvas_put(int x0, int x1, dice_pixel_t color);

/* 把当前行一次性写进 LCD(每个像素一帧只写一次, 这就是不撕裂的原因) */
void dice_canvas_flush(int y);

/* 三角形在 y 行的填充跨度; 返回 1 表示这行有跨度, 0 表示没有 */
int dice_tri_span(const int *ex, const int *ey, int y, int *xa, int *xb);

/* 描三角形在 y 行的三条棱(闭区间, 顶点行也会描到) */
void dice_tri_edges(const int *ex, const int *ey, int y, dice_pixel_t color);

/* 描任意凸多边形在 y 行的各条棱: 四边形面用它描 4 条外边, 不会多出拆三角形的那条对角线 */
void dice_poly_edges(const int *ex, const int *ey, int n, int y, dice_pixel_t color);

/* 在 y 行的 x 处画一个 0~9 数字的一行: 12x6 点阵按 scale(浮点)放大, 一列画成 scale 列宽 */
void dice_canvas_digit(int digit, int x, int r, float scale, dice_pixel_t color);

#endif
