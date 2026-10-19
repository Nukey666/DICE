/**
 ****************************************************************************************************
 * @file        Dice.c
 * @brief       骰子库框架: 骰子数组 + 按时间积分移动 + 撞边/互撞反弹 + 限帧
 * @note        这里没有任何骰子的几何/配色; 换骰子只是 dice_spawn() 传进来的 model 不同。
 *              一帧里只画一遍: model->frame() 重画"本帧的圆 ∪ 上一帧的圆", 旧位置用同一行的
 *              底色写掉, 所以移动时不会出现底色横条; 互撞先分开再画, 所以两颗不会互相擦。
 ****************************************************************************************************
 */

#include <math.h>
#include "Dice.h"

static const dice_port_t *s_port;
static dice_t             s_die[DICE_MAX];
static uint8_t            s_ndie;
static uint32_t           s_last_tick;
static float              s_rest;               /* 恢复系数, 装屏时夹到 [0,1] */
static uint8_t            s_err;                /* 上一次 init/spawn 的结果 */

/**
 * @brief       装屏
 * @note        必须在 LCD 初始化之后调用; port 的生命周期要覆盖整个运行期
 */
void dice_init(const dice_port_t *port)
{
    if ((port == 0) || (port->width == 0) || (port->height == 0))
    {
        s_port = 0;
        s_err  = DICE_ERR_PORT;
        return;
    }

    s_port      = port;
    s_ndie      = 0;
    s_last_tick = 0;
    s_err       = (port->width > DICE_ROW_MAX) ? DICE_ERR_ROW : DICE_OK;

    s_rest = DICE_RESTITUTION;
    if (s_rest < 0.0f)s_rest = 0.0f;
    if (s_rest > 1.0f)s_rest = 1.0f;
}

/**
 * @brief       上一次 dice_init()/dice_spawn() 的结果(见 Dice.h 的 DICE_ERR_*)
 */
uint8_t dice_last_error(void)
{
    return s_err;
}

/**
 * @brief       投放一颗骰子
 * @param       model: 型号; cx,cy: 中心像素; dia: 直径; color: 面色 RGB888
 * @param       spin_x,spin_y,spin_z: 三轴角速度(弧度/秒), 符号决定转向, 0 = 该轴不转
 * @retval      句柄(可继续改 vx/vy/color/spin[]); 参数非法或投满了返回 NULL
 */
dice_t *dice_spawn(const dice_model_t *model, uint16_t cx, uint16_t cy,
                   uint16_t dia, uint32_t color,
                   float spin_x, float spin_y, float spin_z, uint16_t flags)
{
    dice_t *d;

    if (s_port == 0)                        { s_err = DICE_ERR_PORT;  return 0; }
    if (model == 0)                         { s_err = DICE_ERR_MODEL; return 0; }
    if (s_ndie >= DICE_MAX)                 { s_err = DICE_ERR_FULL;  return 0; }
    if ((cx >= s_port->width) || (cy >= s_port->height)) { s_err = DICE_ERR_POS; return 0; }
    if ((dia < 24) || (dia > s_port->width) || (dia > s_port->height))
                                            { s_err = DICE_ERR_SIZE;  return 0; }

    s_err = (s_port->width > DICE_ROW_MAX) ? DICE_ERR_ROW : DICE_OK;

    d = &s_die[s_ndie++];

    d->model = model;
    d->dia   = dia;
    d->color = color;
    d->flags = flags;
    d->contact_k = 0.0f;                            /* 型号在 spawn 里改成自己的值 */
    d->friction  = 0.0f;                            /* 同上, 0 = 用全局 DICE_FRICTION */

    d->spin[0] = spin_x;
    d->spin[1] = spin_y;
    d->spin[2] = spin_z;

    d->x  = (float)cx;
    d->y  = (float)cy;
    d->vx = 0.0f;
    d->vy = 0.0f;

    d->px = (int16_t)cx;
    d->py = (int16_t)cy;

    d->bx0 = d->by0 = d->bx1 = d->by1 = 0;          /* 还没画过 */
    d->alive = 1;

    d->priv[0] = d->priv[1] = d->priv[2] = d->priv[3] = 0.0f;

    model->init(s_port);                            /* 几何只算一次, 重复调用是幂等的 */
    if (model->spawn != 0)model->spawn(d);

    return d;
}

/**
 * @brief       把一颗骰子上次画过的矩形刷回它型号的底色
 * @note        底色只有一个来源: 型号自己的 bg(); 库不再另存一份
 */
static void dice_erase(const dice_t *d)
{
    if ((d->bx1 <= d->bx0) || (d->model->bg == 0))return;

    s_port->fill((uint16_t)d->bx0, (uint16_t)d->by0,
                 (uint16_t)d->bx1, (uint16_t)d->by1, s_port->rgb(d->model->bg()));
}

/**
 * @brief       收回一颗骰子
 * @note        后面的骰子补上槽位, 数组中间不留洞; 不属于本库的指针忽略
 */
void dice_kill(dice_t *d)
{
    uint8_t i;

    if ((s_port == 0) || (d == 0) || (d < s_die) || (d >= (s_die + s_ndie)))return;

    i = (uint8_t)(d - s_die);
    dice_erase(d);
    s_die[i] = s_die[s_ndie - 1];
    s_ndie--;
    s_die[s_ndie].alive = 0;                /* 空出来的槽也标死: 应用手里的旧句柄不会以为还活着 */
}

/**
 * @brief       取某一颗骰子此刻的"值"
 * @param       d: dice_spawn() 返回的句柄
 * @note        型号没提供 value 回调, 或者 d 为空/不属于本库时返回 0。
 *              纯计算, 不画屏、不改状态, 主循环里任何时刻都能调
 */
uint8_t dice_value(const dice_t *d)
{
    if ((d == 0) || (d->model == 0) || (d->model->value == 0))return 0;

    return d->model->value(d);
}

/**
 * @brief       清空所有骰子, 把它们占过的矩形刷回型号底色
 * @note        只擦骰子自己画过的矩形, 屏幕上其它内容(标题等)不动
 */
void dice_clear(void)
{
    uint8_t i;

    for (i = 0; i < s_ndie; i++)
    {
        dice_erase(&s_die[i]);
        if (s_die[i].model->reset != 0)s_die[i].model->reset();
        s_die[i].alive = 0;                 /* 应用手里的句柄全部作废 */
    }

    s_ndie = 0;
}

/**
 * @brief       按速度移动一颗, 碰到屏幕边缘就反弹
 * @note        半径用直径算; 骰子比屏幕还大时没有反弹余地, 直接钉在中间停住
 */
static void dice_move(dice_t *d, float dt)
{
    float r = (float)d->dia * 0.5f;
    float w = (float)s_port->width;
    float h = (float)s_port->height;

    if ((d->vx == 0.0f) && (d->vy == 0.0f))return;

    d->x += d->vx * dt;
    d->y += d->vy * dt;

    if ((2.0f * r) >= w)                            /* 比屏幕还宽: 停中间 */
    {
        d->x  = w * 0.5f;
        d->vx = 0.0f;
    }
    else if (d->x < r)                              /* 左边 */
    {
        d->x  = r;
        d->vx = -s_rest * d->vx;
    }
    else if (d->x > (w - r))                        /* 右边 */
    {
        d->x  = w - r;
        d->vx = -s_rest * d->vx;
    }

    if ((2.0f * r) >= h)
    {
        d->y  = h * 0.5f;
        d->vy = 0.0f;
    }
    else if (d->y < r)
    {
        d->y  = r;
        d->vy = -s_rest * d->vy;
    }
    else if (d->y > (h - r))
    {
        d->y  = h - r;
        d->vy = -s_rest * d->vy;
    }
}

/* 质量 = 直径^DICE_MASS_EXP: 形状相似、密度相同时三次方才对 */
static float dice_mass(const dice_t *d)
{
    return powf((float)d->dia, DICE_MASS_EXP);
}

/**
 * @brief       被撞动之后重新转起来
 * @note        停住时 dice_damp() 把 spin 乘成了精确 0, 再被撞只会平移; 这里按"滚动"关系补一个角速度
 *              ω = (vy, -vx) × DICE_HIT_SPIN。只补原来不转的那颗, 免得越撞越疯。
 */
static void dice_spin_hit(dice_t *d)
{
    if ((d->spin[0] != 0.0f) || (d->spin[1] != 0.0f) || (d->spin[2] != 0.0f))return;

    d->spin[0] =  d->vy * DICE_HIT_SPIN;
    d->spin[1] = -d->vx * DICE_HIT_SPIN;
}

/**
 * @brief       两颗骰子相撞: 先分开, 再按动量守恒 + 恢复系数算冲量
 * @note        分开是必须的: 画的时候每颗只重画自己的圆/椭圆范围, 两颗重叠就会互相擦
 */
static void dice_bounce(dice_t *a, dice_t *b)
{
    float dx = b->x - a->x;
    float dy = b->y - a->y;
    float ka = (a->contact_k > 0.0f) ? a->contact_k : DICE_BOUNCE_K;  /* 型号没写就用全局兜底 */
    float kb = (b->contact_k > 0.0f) ? b->contact_k : DICE_BOUNCE_K;
    float rr = ((float)a->dia * ka + (float)b->dia * kb) * 0.5f;      /* 各自缩一圈再相加 */
    float d2 = dx * dx + dy * dy;
    float d, nx, ny, vn, push, ma, mb, j;

    if ((d2 >= rr * rr) || (d2 < 0.0001f))return;

    d  = sqrtf(d2);
    nx = dx / d;
    ny = dy / d;

    push = (rr - d) * 0.5f + 0.5f;                  /* 各退一半, 多退半像素防止下一帧还判成相撞 */
    a->x -= nx * push;
    a->y -= ny * push;
    b->x += nx * push;
    b->y += ny * push;

    vn = (b->vx - a->vx) * nx + (b->vy - a->vy) * ny;
    if (vn < 0.0f)                                  /* 正在靠近才反弹, 已经分开就别再加速 */
    {
        ma = dice_mass(a);
        mb = dice_mass(b);
        j  = -(1.0f + s_rest) * vn / (1.0f / ma + 1.0f / mb);   /* 等质量时化简成交换法向分量 */

        a->vx -= (j / ma) * nx;
        a->vy -= (j / ma) * ny;
        b->vx += (j / mb) * nx;
        b->vy += (j / mb) * ny;

        dice_spin_hit(a);                           /* 被撞前已经停转的, 撞完要重新转起来 */
        dice_spin_hit(b);
    }
}

/**
 * @brief       两两检测碰撞(骰子最多 4 颗, O(n²) 足够)
 */
static void dice_collide(void)
{
    uint8_t i, j;

    for (i = 0; i < s_ndie; i++)
    {
        for (j = (uint8_t)(i + 1); j < s_ndie; j++)dice_bounce(&s_die[i], &s_die[j]);
    }
}

#if DICE_REALISTIC
/**
 * @brief       地板摩擦 + 自转衰减
 * @note        自转按线速度的同一比例缩小: 速度停, 转也就停了, 不然"停下还在转"太假
 */
static void dice_damp(dice_t *d, float dt)
{
    float sp = sqrtf(d->vx * d->vx + d->vy * d->vy);
    float a  = (d->friction > 0.0f) ? d->friction : DICE_FRICTION;   /* 型号自己的阻尼优先 */
    float ns, k;

    if (sp <= 0.0f)return;

    ns = sp - a * dt;
    if (ns < 0.0f)ns = 0.0f;

    k = ns / sp;
    d->vx *= k;
    d->vy *= k;
    d->spin[0] *= k;
    d->spin[1] *= k;
    d->spin[2] *= k;
}
#endif

/**
 * @brief       推进一帧: dt 由调用方给(秒), 例如固定 1/60 或自己算的真实间隔
 * @note        不碰时间源、不限帧, 谁调用谁负责节奏; 库不睡
 */
void dice_update(float dt)
{
    uint8_t i;

    if ((s_port == 0) || (s_ndie == 0))return;
    if (dt <= 0.0f)return;
    if (dt > 0.1f)dt = 0.1f;                        /* 断点/长顿之后别一帧跳很远 */

    /* 1. 移动 */
    for (i = 0; i < s_ndie; i++)dice_move(&s_die[i], dt);

    /* 2. 碰撞: 撞开后再画, 保证每颗的重画范围不和别人重叠 */
    dice_collide();

    /* 3. 摩擦/自转衰减(真实模拟开关控制) */
#if DICE_REALISTIC
    for (i = 0; i < s_ndie; i++)dice_damp(&s_die[i], dt);
#endif

    /* 4. 画: 每颗把自己"本帧的圆 ∪ 上一帧的圆"重画一遍, 旧位置用底色覆盖 */
    for (i = 0; i < s_ndie; i++)
    {
        dice_t *d = &s_die[i];

        if (d->alive == 0)continue;

        /* 完全静止(不移动也不转)就不必重画, 图上已经是它现在的样子; 还没画过的那帧照画 */
        if ((d->bx1 > d->bx0) &&
            (d->vx == 0.0f) && (d->vy == 0.0f) &&
            (d->spin[0] == 0.0f) && (d->spin[1] == 0.0f) && (d->spin[2] == 0.0f))continue;

        d->model->frame(d, dt);
    }
}

/**
 * @brief       事件更新(便捷版): 用端口的 tick_ms 算 dt, 并按 DICE_FRAME_DELAY_MS 限帧
 * @note        主循环里反复调, 不阻塞; 想自己控节奏(固定 60Hz / RTOS 任务)就直接调 dice_update(dt)
 */
void dice_event_update(void)
{
    uint32_t now, dt_ms;

    if ((s_port == 0) || (s_ndie == 0))return;

    now    = s_port->tick_ms();
    dt_ms  = now - s_last_tick;
    if (dt_ms < DICE_FRAME_DELAY_MS)return;
    s_last_tick = now;

    dice_update((float)dt_ms * 0.001f);
}


/******************************************************************************************/
/* 多面体骰子通用画法: 投影/着光/扫描行/三角形/点阵。型号只提供几何和每个面的点数 */

/* 面上的点数只需要 '0'~'9', 12x6 点阵(每列 2 字节共 12 行, 字节内 MSB 在上), 不依赖外部字库 */
static const uint8_t s_digit_1206[10][12] =
{
    {0x1F,0x80,0x20,0x40,0x20,0x40,0x20,0x40,0x1F,0x80,0x00,0x00}, /* "0" */
    {0x00,0x00,0x10,0x40,0x3F,0xC0,0x00,0x40,0x00,0x00,0x00,0x00}, /* "1" */
    {0x18,0xC0,0x21,0x40,0x22,0x40,0x24,0x40,0x18,0x40,0x00,0x00}, /* "2" */
    {0x10,0x80,0x20,0x40,0x24,0x40,0x24,0x40,0x1B,0x80,0x00,0x00}, /* "3" */
    {0x02,0x00,0x0D,0x00,0x11,0x00,0x3F,0xC0,0x01,0x40,0x00,0x00}, /* "4" */
    {0x3C,0x80,0x24,0x40,0x24,0x40,0x24,0x40,0x23,0x80,0x00,0x00}, /* "5" */
    {0x1F,0x80,0x24,0x40,0x24,0x40,0x34,0x40,0x03,0x80,0x00,0x00}, /* "6" */
    {0x30,0x00,0x20,0x00,0x27,0xC0,0x38,0x00,0x20,0x00,0x00,0x00}, /* "7" */
    {0x1B,0x80,0x24,0x40,0x24,0x40,0x24,0x40,0x1B,0x80,0x00,0x00}, /* "8" */
    {0x1C,0x00,0x22,0xC0,0x22,0x40,0x22,0x40,0x1F,0x80,0x00,0x00}  /* "9" */
};

static dice_pixel_t s_row[DICE_ROW_MAX];        /* 一行的像素全部在这里合成好, 再整行刷进LCD */
static int          s_rx0, s_rx1;               /* 当前行实际要重绘的 x 范围 */

static int dice_round(float v)
{
    return (int)((v >= 0.0f) ? (v + 0.5f) : (v - 0.5f));
}

void dice_rot_m(float m[3][3], float ax, float ay, float az)
{
    float sa = sinf(ax), ca = cosf(ax);
    float sb = sinf(ay), cb = cosf(ay);
    float sg = sinf(az), cg = cosf(az);

    /* R = Rz * Ry * Rx */
    m[0][0] = cg * cb;
    m[0][1] = cg * sb * sa - sg * ca;
    m[0][2] = cg * sb * ca + sg * sa;
    m[1][0] = sg * cb;
    m[1][1] = sg * sb * sa + cg * ca;
    m[1][2] = sg * sb * ca - cg * sa;
    m[2][0] = -sb;
    m[2][1] = cb * sa;
    m[2][2] = cb * ca;
}

dice_pixel_t dice_shade(uint32_t rgb888, float diff, float spec)
{
    float k = DICE_AMBIENT + (1.0f - DICE_AMBIENT) * diff;
    int r = dice_round((float)((rgb888 >> 16) & 0xFF) * k + spec * 255.0f);
    int g = dice_round((float)((rgb888 >> 8) & 0xFF) * k + spec * 255.0f);
    int b = dice_round((float)(rgb888 & 0xFF) * k + spec * 255.0f);

    if (r < 0)r = 0;
    if (g < 0)g = 0;
    if (b < 0)b = 0;
    if (r > 255)r = 255;
    if (g > 255)g = 255;
    if (b > 255)b = 255;

    /* 只在"颜色"这一层转换, 每个面/棱每帧一次, 不参与逐像素运算 */
    return s_port->rgb((uint32_t)((r << 16) | (g << 8) | b));
}

uint32_t dice_lighten(uint32_t rgb888)
{
    int r = (int)((rgb888 >> 16) & 0xFF);
    int g = (int)((rgb888 >> 8) & 0xFF);
    int b = (int)(rgb888 & 0xFF);

    r += (255 - r) / 2;
    g += (255 - g) / 2;
    b += (255 - b) / 2;

    return (uint32_t)((r << 16) | (g << 8) | b);
}

int dice_halfw(int cx, int cy, int a, int b, int y)
{
    int dy = y - cy;

    (void)cx;                               /* 只按 y 求半宽, cx 是调用方保持对称的写法 */
    if ((a <= 0) || (b <= 0) || (dy < -b) || (dy > b))return -1;

    return (int)((float)a * sqrtf(1.0f - (float)(dy * dy) / (float)(b * b)) + 1.0f);
}

float dice_num_scale(float radius)
{
    float s = radius / DICE_NUM_K;

    return (s < 1.0f) ? 1.0f : s;
}

void dice_canvas_row(int x0, int x1)
{
    if (x0 < 0)x0 = 0;
    if (x1 > DICE_ROW_MAX - 1)x1 = DICE_ROW_MAX - 1;

    s_rx0 = x0;
    s_rx1 = x1;
}

void dice_canvas_put(int x0, int x1, dice_pixel_t color)
{
    dice_pixel_t *p;
    int n;

    if (x0 < s_rx0)x0 = s_rx0;
    if (x1 > s_rx1)x1 = s_rx1;

    n = x1 - x0 + 1;
    if (n <= 0)return;

    p = &s_row[x0];
    while (n--)*p++ = color;
}

void dice_canvas_flush(int y)
{
    if ((s_port == 0) || (s_rx1 < s_rx0))return;

    s_port->flush_row((uint16_t)s_rx0, (uint16_t)y,
                      (uint16_t)(s_rx1 - s_rx0 + 1), &s_row[s_rx0]);
}

int dice_tri_span(const int *ex, const int *ey, int y, int *xa, int *xb)
{
    int i, n = 0, v;

    *xa = 0;
    *xb = 0;

    for (i = 0; i < 3; i++)
    {
        int a = i;
        int b = (i + 1) % 3;

        if (ey[a] == ey[b])continue;                            /* 水平边不参与求交 */

        if (((y >= ey[a]) && (y < ey[b])) || ((y >= ey[b]) && (y < ey[a])))
        {
            v = ex[a] + (int)(((long)(ex[b] - ex[a]) * (y - ey[a])) / (ey[b] - ey[a]));

            if (n == 0)
            {
                *xa = v;
                n = 1;
            }
            else if (n == 1)
            {
                *xb = v;
                n = 2;
            }
            else                                        /* 极端情况下交点多于2个, 取最外两个 */
            {
                if (v < *xa)*xa = v;
                else if (v > *xb)*xb = v;
            }
        }
    }

    if (n == 2)
    {
        if (*xa > *xb)
        {
            v = *xa; *xa = *xb; *xb = v;
        }
        return 1;
    }

    if (n == 1)
    {
        *xb = *xa;
        return 1;
    }

    return 0;
}

void dice_tri_edges(const int *ex, const int *ey, int y, dice_pixel_t color)
{
    dice_poly_edges(ex, ey, 3, y, color);
}

void dice_poly_edges(const int *ex, const int *ey, int n, int y, dice_pixel_t color)
{
    int i;

    for (i = 0; i < n; i++)
    {
        int a = i;
        int b = (i + 1) % n;

        if (ey[a] == ey[b])                                     /* 水平边: 整段都要描 */
        {
            if (y == ey[a])
            {
                if (ex[a] < ex[b])dice_canvas_put(ex[a], ex[b], color);
                else dice_canvas_put(ex[b], ex[a], color);
            }
        }
        else if (((y >= ey[a]) && (y <= ey[b])) || ((y >= ey[b]) && (y <= ey[a])))
        {
            int x = ex[a] + (int)(((long)(ex[b] - ex[a]) * (y - ey[a])) / (ey[b] - ey[a]));
            dice_canvas_put(x, x, color);
        }
    }
}

void dice_canvas_digit(int digit, int x, int r, float scale, dice_pixel_t color)
{
    const unsigned char *pf;
    int col, run = -1;
    int gr = (int)((float)r / scale);                /* 屏幕行 -> 字库行(任意倍率) */

    if ((digit < 0) || (digit > 9) || (gr > 11))return;

    pf = s_digit_1206[digit];

    for (col = 0; col <= 6; col++)
    {
        int on = 0;

        if (col < 6)
        {
            on = (pf[col * 2 + (gr >> 3)] >> (7 - (gr & 7))) & 1;
        }

        if (on)
        {
            if (run < 0)run = col;
        }
        else if (run >= 0)
        {
            dice_canvas_put(x + (int)((float)run * scale),
                            x + (int)((float)col * scale) - 1, color);
            run = -1;
        }
    }
}
