/**
 ****************************************************************************************************
 * @file        D20.c
 * @author      ALIENTEK
 * @version     V1.1
 * @date        2026-10-13
 * @brief       D20 模型: 正二十面体(1~20 点), 可以投放任意多颗, 每颗的位置/大小/颜色/转速独立
 * @note        每颗的参数由 Dice.c 的 dice_t 保存(本文件只把它的 priv[] 当三个旋转角用);
 *              渲染时逐颗把参数装进下面的全局量(单线程, 这些全局量只当一颗骰子的临时寄存器用),
 *              所以同一份几何代码可以画任意多颗。限帧/移动由 Dice.c 做
 * @note        渲染流程(每帧):
 *                1. 由三个欧拉角合成旋转矩阵 R = Rz * Ry * Rx
 *                2. 12个顶点做旋转 -> 透视投影 -> 屏幕坐标
 *                3. 20个三角面依次求外法线
 *                4. 背面剔除(凸多面体, 正面必然完整可见, 无需深度排序)
 *                5. 按 环境光 + 漫反射 + 高光 计算面颜色
 *                6. 按"扫描行"合成: 逐行把 底色(舞台/阴影) -> 可见面 -> 棱边 -> 点数
 *                   合成到一行内存缓冲, 最后整行一次性写进LCD
 *
 *              之所以采用"逐行合成"而不是"先清舞台再画骰子", 是为了消除撕裂:
 *              清屏会把整块舞台刷成底色, 而骰子要再过几毫秒才画上去; LCD面板在这段
 *              时间里正在逐行扫描输出, 拍到的是"已清掉但还没画上骰子"的中间状态,
 *              屏上就会看到一条背景色的横条盖在骰子上(且随相位漂移时有时无)。
 *              逐行合成保证每个像素一帧内只被写一次、写下去就是最终颜色, 中间状态
 *              根本不存在, 横条随之消失; 顺带把LCD总线写入量从"整块舞台+骰子"降到
 *              "骰子包围圆", 帧率也更高。
 ****************************************************************************************************
 */

#include "D20.h"
#include <math.h>

/* 移植接口: 由 dice_init() 传入, 所有绘制和时钟都从这里出去 */
static const dice_port_t *s_port;


/* 黄金比例 (1 + sqrt(5)) / 2; 其余渲染常量(相机距离/光照/点数)在 Dice.h 的 DICE_* 里 */
#define D20_PHI         1.6180340f


/******************************************************************************************/
/* 正二十面体原始数据: 12个顶点 + 20个三角面 */

static const float d20_vsrc[12][3] =
{
    {-1.0f,      D20_PHI,  0.0f}, { 1.0f,      D20_PHI,  0.0f},
    {-1.0f,     -D20_PHI,  0.0f}, { 1.0f,     -D20_PHI,  0.0f},
    { 0.0f,     -1.0f,      D20_PHI}, { 0.0f,      1.0f,      D20_PHI},
    { 0.0f,     -1.0f,     -D20_PHI}, { 0.0f,      1.0f,     -D20_PHI},
    { D20_PHI,   0.0f,     -1.0f}, { D20_PHI,   0.0f,      1.0f},
    {-D20_PHI,   0.0f,     -1.0f}, {-D20_PHI,   0.0f,      1.0f}
};

static const uint8_t d20_fsrc[20][3] =
{
    {0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10},  {0, 10, 11},
    {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6},  {7, 1, 8},
    {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},   {3, 8, 9},
    {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},   {9, 8, 1}
};


/******************************************************************************************/
/* 模块状态 */

static float    d20_vtx[12][3];                 /* 归一化到单位球的模型顶点 */
static uint8_t  d20_face[20][3];                /* 绕序已统一(叉积指向外侧)的面索引 */

static float    d20_tx[12];                     /* 旋转后顶点 X */
static float    d20_ty[12];                     /* 旋转后顶点 Y */
static float    d20_tz[12];                     /* 旋转后顶点 Z(朝向观察者为正) */
static int      d20_sx[12];                     /* 投影后的屏幕 X */
static int      d20_sy[12];                     /* 投影后的屏幕 Y */

static uint16_t d20_sw, d20_sh;                 /* 屏幕宽高 */
static int      d20_cx, d20_cy;                 /* 舞台中心 */
static float    d20_scale;                      /* 单位模型半径对应的像素数 */
static int      d20_stage[4];                   /* 本帧逐行循环的上下界 x0,x1 是粗界, 精确左右界按形状算 */
static int      d20_bbox[4];                    /* 本帧自己的包围矩形, 画完写回 d->bx0..by1 */
static int      d20_pcx, d20_pcy;               /* 上一帧的整数中心(擦掉旧位置用) */
static uint8_t  d20_prev_ok;                    /* 上一帧画过没有 */

static int      d20_rr;                         /* 骰子投影半径上界(像素) */
static int      d20_shy;                        /* 地面阴影中心 y */
static int      d20_sha;                        /* 地面阴影横半轴 */
static int      d20_shb;                        /* 地面阴影纵半轴 */
static int      d20_shadow;                     /* 当前这颗要不要画阴影(来自 dice_t.flags) */

static float    d20_ang[3];                     /* 当前这颗骰子的三个旋转角(渲染用) */

static uint8_t      s_geo_ready = 0;            /* 顶点/面只归一化一次 */
static uint32_t     s_col_face;                 /* 当前这颗骰子的面色 RGB888 */
static uint32_t     s_col_edge;                 /* 棱边色(由面色提亮得到) */
static uint32_t     s_col_num;                  /* 点数色(按面色亮度取黑/白) */
static dice_pixel_t s_px_bg;                    /* 背景色的像素缓存 */
static dice_pixel_t s_px_sho;                   /* 阴影外圈 */
static dice_pixel_t s_px_shi;                   /* 阴影内圈 */

/* 面的点数: 标准 D20 对面点数之和恒为 21, 编号在初始化时按几何对映关系分配 */
static uint8_t d20_num[20];                     /* 各面的点数(1~20) */
static int8_t  d20_opp[20];                     /* 各面的对映面下标 */
static float   d20_fc[20][3];                   /* 各面重心(初始化时算一次) */

/* 已经投放的骰子 */
static uint8_t   s_spawned;                       /* 已投放几颗, 用来把起始姿态错开 */

/* ---- 每帧的可见面列表 ---- */
static int      d20_vx[20][3];                  /* 可见面 3 个顶点的屏幕 X */
static int      d20_vy[20][3];                  /* 可见面 3 个顶点的屏幕 Y */
static dice_pixel_t d20_vc[20];                  /* 可见面填充色(本屏像素) */
static dice_pixel_t d20_ve[20];                  /* 可见面棱边色 */
#if D20_SHOW_NUM
static int      d20_vn[20][4];                  /* 可见面点数框: {x, y, 位数, 数值}, 位数为0表示不画 */
static dice_pixel_t d20_vnc[20];                 /* 可见面点数颜色 */
static float    d20_nscale = 1.0f;               /* 点数放大倍数(浮点), 随骰子直径连续变 */
#endif
static int      d20_nvis;                       /* 本帧可见面数量 */

/* ---- 扫描行缓冲在 Dice.c 的 canvas 里, 本文件只管往它上面摆几何 ---- */

#if D20_SHOW_NUM
/* 10 的幂, 用于取十进制各位数字 */
static const uint32_t d20_pow10[3] = { 1, 10, 100 };
#endif


static int d20_iround(float v)
{
    return (int)((v >= 0.0f) ? (v + 0.5f) : (v - 0.5f));
}

/* 三角形的求交/填充跨度/描棱、点阵数字都在 Dice.c(dice_tri_span / dice_tri_edges / dice_canvas_digit) */

#if D20_SHOW_NUM
/**
 * @brief       把本帧所有可见面的点数中, 落在 y 行的那些画到本行缓冲
 */
static void d20_number_row(int y)
{
    int k, t;

    for (k = 0; k < d20_nvis; k++)
    {
        const int *nb = d20_vn[k];
        int r = y - nb[1];
        int nd = nb[2];
        float s = d20_nscale;

        if ((nd == 0) || (r < 0) || (r >= (int)(12.0f * s)))continue;

        for (t = 0; t < nd; t++)
        {
            int ch = (int)((nb[3] / d20_pow10[nd - t - 1]) % 10);
            dice_canvas_digit(ch, nb[0] + (int)((float)t * 6.0f * s), r, s, d20_vnc[k]);
        }
    }
}
#endif


static void d20_number_faces(void)
{
    int f, g, k;

    /* 1. 各面重心(顶点已归一化) */
    for (f = 0; f < 20; f++)
    {
        d20_fc[f][0] = 0.0f;
        d20_fc[f][1] = 0.0f;
        d20_fc[f][2] = 0.0f;

        for (k = 0; k < 3; k++)
        {
            int v = d20_face[f][k];

            d20_fc[f][0] += d20_vtx[v][0];
            d20_fc[f][1] += d20_vtx[v][1];
            d20_fc[f][2] += d20_vtx[v][2];
        }
    }

    /* 2. 对映面: 正二十面体中心对称, 两面的重心互为相反数, 有且只有一个 */
    for (f = 0; f < 20; f++)
    {
        d20_opp[f] = -1;

        for (g = 0; g < 20; g++)
        {
            float dx = d20_fc[f][0] + d20_fc[g][0];
            float dy = d20_fc[f][1] + d20_fc[g][1];
            float dz = d20_fc[f][2] + d20_fc[g][2];

            if ((dx * dx + dy * dy + dz * dz) < 0.0001f)
            {
                d20_opp[f] = (int8_t)g;
                break;
            }
        }

        if (d20_opp[f] < 0)return;                  /* 几何表有问题就不画, 免得画出错的点数 */
    }

    /* 3. 成对编号: 一对对面分到 n 和 21-n */
    for (f = 0, k = 1; f < 20; f++)
    {
        if (d20_num[f] != 0)continue;

        d20_num[f] = (uint8_t)k;
        d20_num[d20_opp[f]] = (uint8_t)(21 - k);
        k++;
    }

    /* 4. 自检: 1~20 各出现一次, 且每对对面之和都是 21(改几何表时立刻能发现) */
    for (f = 0; f < 20; f++)
    {
        if ((d20_num[f] == 0) || ((int)d20_num[f] + (int)d20_num[d20_opp[f]] != 21))return;

        for (g = f + 1; g < 20; g++)
        {
            if (d20_num[g] == d20_num[f])return;
        }
    }
}


/******************************************************************************************/
/* 布局 */

/**
 * @brief       按这颗骰子的中心/直径算出投影比例, 阴影参数和每帧重绘的上下界
 */
static void d20_layout(const dice_t *d)
{
    int b0, shadow, yoff;

    shadow = (d->flags & DICE_FLAG_SHADOW) != 0;

    d20_sw = s_port->width;
    d20_sh = s_port->height;

    d20_cx = d20_iround(d->x);
    d20_cy = d20_iround(d->y);

    /* 直径 -> 像素比例: 投影半径峰值 = scale * D20_PROJ_PEAK, 让它正好等于骰子半径 */
    d20_scale = (float)d->dia * 0.5f / D20_PROJ_PEAK;

    d20_rr  = (int)(d20_scale * D20_PROJ_PEAK) + 2;     /* 骰子投影半径上界 */

#if D20_SHOW_NUM
    /* 点数大小跟着直径连续变: 字高 ≈ 12 * nscale, 约骰子直径的 24%; 下限 1 保证 12px 字能看清 */
    d20_nscale = dice_num_scale((float)d20_rr);
#endif


    yoff    = (int)(d20_scale * 0.95f);                 /* 阴影中心相对骰子中心的下移量 */
    d20_shy = d20_cy + yoff;
    d20_sha = (int)(d20_scale * 0.80f);                 /* 地面阴影横半轴 */
    d20_shb = (int)(d20_scale * 0.12f);                 /* 地面阴影纵半轴 */
    if (d20_shb < 1)d20_shb = 1;

    d20_pcx        = d->px;                             /* 上一帧中心: rr/sha/shb 只跟直径有关, 不随帧变 */
    d20_pcy        = d->py;
    d20_prev_ok    = (uint8_t)(d->bx1 > d->bx0);        /* 上一帧真的画过 */

    /* 本帧自己的矩形 = 投影圆 ∪ 阴影, 画完写回 d->bx*(dice_clear 用它擦) */
    d20_bbox[0] = d20_cx - d20_rr - 1;
    d20_bbox[1] = d20_cy - d20_rr - 1;
    d20_bbox[2] = d20_cx + d20_rr + 1;
    d20_bbox[3] = d20_cy + d20_rr + 1;
    if (shadow && ((d20_shy + d20_shb + 1) > d20_bbox[3]))d20_bbox[3] = d20_shy + d20_shb + 1;

    d20_stage[0] = d20_bbox[0];
    d20_stage[1] = d20_bbox[1];
    d20_stage[2] = d20_bbox[2];
    d20_stage[3] = d20_bbox[3];

    /* 逐行循环的上下界还要并上上一帧的圆和阴影(左右界在 d20_render 里按形状算) */
    if (d20_prev_ok)
    {
        b0 = d20_pcx - d20_rr - 1;
        if (b0 < d20_stage[0])d20_stage[0] = b0;

        b0 = d20_pcy - d20_rr - 1;
        if (b0 < d20_stage[1])d20_stage[1] = b0;

        b0 = d20_pcx + d20_rr + 1;
        if (b0 > d20_stage[2])d20_stage[2] = b0;

        b0 = d20_pcy + d20_rr + 1;
        if (shadow && ((d20_pcy + yoff + d20_shb + 1) > b0))b0 = d20_pcy + yoff + d20_shb + 1;
        if (b0 > d20_stage[3])d20_stage[3] = b0;
    }

    if (d20_stage[0] < 0)d20_stage[0] = 0;
    if (d20_stage[1] < 0)d20_stage[1] = 0;
    if (d20_stage[2] > (int)d20_sw - 1)d20_stage[2] = (int)d20_sw - 1;
    if (d20_stage[3] > (int)d20_sh - 1)d20_stage[3] = (int)d20_sh - 1;
}

/******************************************************************************************/
/* 初始化 */

/**
 * @brief       初始化骰子库
 * @note        必须在 LCD 初始化之后调用; port 的生命周期要覆盖整个运行期
 */
static void d20_model_init(const dice_port_t *port)
{
    int i, a, b, c;
    float len, e1x, e1y, e1z, e2x, e2y, e2z;
    float nx, ny, nz, px, py, pz, dot;
    float fx, fy, fz;

    s_port = port;
    if (!s_port || (s_port->width == 0) || (s_port->height == 0))return;

    /* 1. 顶点归一化到单位球面 */
    for (i = 0; i < 12; i++)
    {
        px = d20_vsrc[i][0];
        py = d20_vsrc[i][1];
        pz = d20_vsrc[i][2];
        len = sqrtf(px * px + py * py + pz * pz);

        d20_vtx[i][0] = px / len;
        d20_vtx[i][1] = py / len;
        d20_vtx[i][2] = pz / len;
    }

    /* 2. 统一每个面的绕序, 使 (v1-v0)x(v2-v0) 始终指向球外。
          正二十面体的重心就在原点, 所以"面的重心方向"就是外法线方向。 */
    for (i = 0; i < 20; i++)
    {
        a = d20_fsrc[i][0];
        b = d20_fsrc[i][1];
        c = d20_fsrc[i][2];

        e1x = d20_vtx[b][0] - d20_vtx[a][0];
        e1y = d20_vtx[b][1] - d20_vtx[a][1];
        e1z = d20_vtx[b][2] - d20_vtx[a][2];
        e2x = d20_vtx[c][0] - d20_vtx[a][0];
        e2y = d20_vtx[c][1] - d20_vtx[a][1];
        e2z = d20_vtx[c][2] - d20_vtx[a][2];

        nx = e1y * e2z - e1z * e2y;
        ny = e1z * e2x - e1x * e2z;
        nz = e1x * e2y - e1y * e2x;

        fx = (d20_vtx[a][0] + d20_vtx[b][0] + d20_vtx[c][0]) / 3.0f;
        fy = (d20_vtx[a][1] + d20_vtx[b][1] + d20_vtx[c][1]) / 3.0f;
        fz = (d20_vtx[a][2] + d20_vtx[b][2] + d20_vtx[c][2]) / 3.0f;

        dot = nx * fx + ny * fy + nz * fz;

        if (dot < 0.0f)                             /* 法线朝内, 交换两个顶点即可翻转绕序 */
        {
            d20_face[i][0] = (uint8_t)a;
            d20_face[i][1] = (uint8_t)c;
            d20_face[i][2] = (uint8_t)b;
        }
        else
        {
            d20_face[i][0] = (uint8_t)a;
            d20_face[i][1] = (uint8_t)b;
            d20_face[i][2] = (uint8_t)c;
        }
    }

    /* 3. 分配点数: 对面之和恒为 21 */
    d20_number_faces();

    s_geo_ready = 1;
}


/******************************************************************************************/
/* 渲染 */

/* 旋转矩阵 R = Rz*Ry*Rx 在 Dice.c(dice_rot_m), 渲染和"取值"都用它, 保证同一姿态 */

/**
 * @brief       渲染一帧画面
 */
static void d20_render(void)
{
    float m[3][3];
    float px, py, pz, persp;
    float e1x, e1y, e1z, e2x, e2y, e2z;
    float nx, ny, nz, nlen;
    float fx, fy, fz, vx, vy, vz;
    float ndl, s2, s4, spec;
    int i, f, k, nvis, y, ytop, ybot;
    int dy;

    /* ---- 1. 合成旋转矩阵 R = Rz * Ry * Rx ---- */
    dice_rot_m(m, d20_ang[0], d20_ang[1], d20_ang[2]);

    /* ---- 2. 顶点旋转 + 透视投影 ---- */
    for (i = 0; i < 12; i++)
    {
        px = d20_vtx[i][0];
        py = d20_vtx[i][1];
        pz = d20_vtx[i][2];

        vx = m[0][0] * px + m[0][1] * py + m[0][2] * pz;
        vy = m[1][0] * px + m[1][1] * py + m[1][2] * pz;
        vz = m[2][0] * px + m[2][1] * py + m[2][2] * pz;

        d20_tx[i] = vx;
        d20_ty[i] = vy;
        d20_tz[i] = vz;

        persp = DICE_CAM_DIST / (DICE_CAM_DIST - vz);     /* 透视缩放, z=0 时为 1 */

        d20_sx[i] = d20_cx + d20_iround(vx * persp * d20_scale);
        d20_sy[i] = d20_cy - d20_iround(vy * persp * d20_scale);    /* 屏幕Y轴向下, 需要取反 */
    }

    /* ---- 3. 逐面: 剔除 + 光照, 把可见面收集起来 ---- */
    nvis = 0;

    for (f = 0; f < 20; f++)
    {
        int i0 = d20_face[f][0];
        int i1 = d20_face[f][1];
        int i2 = d20_face[f][2];

        /* 外法线(顶点已旋转, 叉积结果仍指向球外) */
        e1x = d20_tx[i1] - d20_tx[i0];
        e1y = d20_ty[i1] - d20_ty[i0];
        e1z = d20_tz[i1] - d20_tz[i0];
        e2x = d20_tx[i2] - d20_tx[i0];
        e2y = d20_ty[i2] - d20_ty[i0];
        e2z = d20_tz[i2] - d20_tz[i0];

        nx = e1y * e2z - e1z * e2y;
        ny = e1z * e2x - e1x * e2z;
        nz = e1x * e2y - e1y * e2x;

        nlen = sqrtf(nx * nx + ny * ny + nz * nz);
        if (nlen < 0.000001f)continue;                  /* 退化面 */

        nx /= nlen;
        ny /= nlen;
        nz /= nlen;

        /* 面重心 */
        fx = (d20_tx[i0] + d20_tx[i1] + d20_tx[i2]) / 3.0f;
        fy = (d20_ty[i0] + d20_ty[i1] + d20_ty[i2]) / 3.0f;
        fz = (d20_tz[i0] + d20_tz[i1] + d20_tz[i2]) / 3.0f;

        /* 背面剔除: 摄像机位于(0, 0, CAM), 法线与"重心->摄像机"夹角小于90度才算正面。
           骰子是凸多面体, 所有正面都完整可见, 因此不需要按深度排序, 也不存在相互遮挡。 */
        vx = -fx;
        vy = -fy;
        vz = DICE_CAM_DIST - fz;

        if ((nx * vx + ny * vy + nz * vz) <= 0.0f)continue;

        /* 漫反射 */
        ndl = nx * DICE_LIGHT_X + ny * DICE_LIGHT_Y + nz * DICE_LIGHT_Z;
        if (ndl < 0.0f)ndl = 0.0f;

        /* 高光(用 ndl^8 近似镜面反射的锐利高光) */
        s2 = ndl * ndl;
        s4 = s2 * s2;
        spec = s4 * s4 * DICE_SPECULAR;

        d20_vc[nvis] = dice_shade(s_col_face, ndl, spec);
        d20_ve[nvis] = dice_shade(s_col_edge, ndl, spec * 0.5f);

        d20_vx[nvis][0] = d20_sx[i0]; d20_vy[nvis][0] = d20_sy[i0];
        d20_vx[nvis][1] = d20_sx[i1]; d20_vy[nvis][1] = d20_sy[i1];
        d20_vx[nvis][2] = d20_sx[i2]; d20_vy[nvis][2] = d20_sy[i2];

#if D20_SHOW_NUM
        /* 点数框: 只记下位置, 具体像素在扫描到对应行时才画 */
        {
            long area2 = (long)(d20_vx[nvis][1] - d20_vx[nvis][0]) * (long)(d20_vy[nvis][2] - d20_vy[nvis][0])
                       - (long)(d20_vx[nvis][2] - d20_vx[nvis][0]) * (long)(d20_vy[nvis][1] - d20_vy[nvis][0]);

            if (area2 < 0)area2 = -area2;

            d20_vn[nvis][2] = 0;                        /* 默认不画 */

            if (area2 > (long)(d20_scale * d20_scale * DICE_NUM_AREA_K))    /* 门限随骰子大小缩放 */
            {
                int nd = (f >= 9) ? 2 : 1;              /* 10~20 是两位数 */
                float s = d20_nscale;                   /* 点数框跟着骰子大小连续缩放 */
                int tx = (d20_vx[nvis][0] + d20_vx[nvis][1] + d20_vx[nvis][2]) / 3 - (int)((float)nd * 3.0f * s);
                int ty = (d20_vy[nvis][0] + d20_vy[nvis][1] + d20_vy[nvis][2]) / 3 - (int)(6.0f * s);

                if ((tx >= 2) && (ty >= 2) &&
                    ((tx + (int)((float)nd * 6.0f * s)) < (int)d20_sw - 2) &&
                    ((ty + (int)(12.0f * s)) < (int)d20_sh - 2))
                {
                    d20_vn[nvis][0] = tx;
                    d20_vn[nvis][1] = ty;
                    d20_vn[nvis][2] = nd;
                    d20_vn[nvis][3] = d20_num[f];
                    d20_vnc[nvis]  = dice_shade(s_col_num, ndl, spec);
                }
            }
        }
#endif

        nvis++;
    }

    d20_nvis = nvis;

    /* ---- 4. 逐行合成 ----
       每行只碰"本帧骰子圆/阴影 ∪ 上一帧骰子圆/阴影"真正覆盖的像素, 矩形四角永远不写:
       方框保护会把骰子方框的四角(别人的地盘)每帧擦一遍, 圆/椭圆保护不会, 重叠时才不闪。 */
    ytop = d20_stage[1];
    ybot = d20_stage[3];

    for (y = ytop; y <= ybot; y++)
    {
        int l = (int)d20_sw, r = -1;                    /* 本行要重绘的左右边界 */
        int w;

        w = dice_halfw(d20_cx, d20_cy, d20_rr, d20_rr, y);
        if (w >= 0)
        {
            l = d20_cx - w;
            r = d20_cx + w;
        }

        if (d20_prev_ok)
        {
            w = dice_halfw(d20_pcx, d20_pcy, d20_rr, d20_rr, y);
            if (w >= 0)
            {
                if ((d20_pcx - w) < l)l = d20_pcx - w;
                if ((d20_pcx + w) > r)r = d20_pcx + w;
            }
        }

        if (d20_shadow)
        {
            w = dice_halfw(d20_cx, d20_shy, d20_sha, d20_shb, y);
            if (w >= 0)
            {
                if ((d20_cx - w) < l)l = d20_cx - w;
                if ((d20_cx + w) > r)r = d20_cx + w;
            }

            if (d20_prev_ok)
            {
                w = dice_halfw(d20_pcx, d20_pcy + (d20_shy - d20_cy), d20_sha, d20_shb, y);
                if (w >= 0)
                {
                    if ((d20_pcx - w) < l)l = d20_pcx - w;
                    if ((d20_pcx + w) > r)r = d20_pcx + w;
                }
            }
        }

        if (r < l)continue;

        if (l < d20_stage[0])l = d20_stage[0];
        if (r > d20_stage[2])r = d20_stage[2];
        if (r > DICE_ROW_MAX - 1)r = DICE_ROW_MAX - 1;
        if (l < 0)l = 0;
        if (r < l)continue;

        dice_canvas_row(l, r);

        /* 4.2 底色: 先铺舞台色, 再用阴影色覆盖 */
        dice_canvas_put(l, r, s_px_bg);

        if (d20_shadow)
        {
            dy = y - d20_shy;
            if ((dy >= -d20_shb) && (dy <= d20_shb))
            {
                float t = 1.0f - (float)(dy * dy) / (float)(d20_shb * d20_shb);
                if (t > 0.0f)
                {
                    int so = (int)((float)d20_sha * sqrtf(t) + 0.5f);
                    int ia = (d20_sha * 3) / 5;
                    int ib = (d20_shb * 3) / 5;

                    dice_canvas_put(d20_cx - so, d20_cx + so, s_px_sho);

                    if ((ib > 0) && (dy >= -ib) && (dy <= ib))
                    {
                        t = 1.0f - (float)(dy * dy) / (float)(ib * ib);
                        if (t > 0.0f)
                        {
                            int si = (int)((float)ia * sqrtf(t) + 0.5f);
                            dice_canvas_put(d20_cx - si, d20_cx + si, s_px_shi);
                        }
                    }
                }
            }
        }

        /* 4.3 可见面填充(凸多面体的正面互不重叠, 直接叠加即可) */
        for (k = 0; k < nvis; k++)
        {
            int xa, xb;

            if (dice_tri_span(d20_vx[k], d20_vy[k], y, &xa, &xb))
            {
                dice_canvas_put(xa, xb, d20_vc[k]);
            }
        }

        /* 4.4 棱边(画在面之后, 保证棱线清晰) */
        for (k = 0; k < nvis; k++)
        {
            dice_tri_edges(d20_vx[k], d20_vy[k], y, d20_ve[k]);
        }

        /* 4.5 点数 */
#if D20_SHOW_NUM
        d20_number_row(y);
#endif

        /* 4.6 整行一次性写进LCD: 每帧每个像素只被写一次, 不存在中间状态 */
        dice_canvas_flush(y);
    }
}


/******************************************************************************************/
/* 对外接口 */

/**
 * @brief       面色 -> 点数色(亮面用黑点, 暗面用白点)
 */
static uint32_t d20_readable(uint32_t rgb)
{
    int lum = ((rgb >> 16) & 0xFF) + ((rgb >> 8) & 0xFF) + (rgb & 0xFF);

    return (uint32_t)((lum > 3 * 255 / 2) ? 0x000000 : 0xFFFFFF);
}

/**
 * @brief       本型号的面板底色
 * @note        Dice 库擦骰子/清屏时用这个色, 所以面板色在 D20 里只有 D20_BG_COLOR 一个定义
 */
static uint32_t d20_model_bg(void)
{
    return D20_BG_COLOR;
}

/**
 * @brief       复位型号内部状态
 * @note        dice_clear() 时被调用: 起始姿态的错开计数归零, 清空后重新投放从第一颗开始
 */
static void d20_model_reset(void)
{
    s_spawned = 0;
}

/**
 * @brief       这一颗此刻的"值" = 屏幕上投影面积最大的那个面的点数
 * @param       d: 这一颗(姿态在 d->priv[0..2])
 * @note        正二十面体 20 个面全等, 投影面积 ∝ 面法线朝镜头的分量 × 透视缩放²;
 *              正多面体的面法线方向就是面重心方向, 所以不用旋转顶点, 20 次点积就够。
 *              拿不到姿态(几何没初始化 / 点数自检没过)时返回 0
 */
static uint8_t d20_model_value(const dice_t *d)
{
    float m[3][3];
    float best = -1.0f;
    int bf = 0, f;

    if (d == 0)return 0;

    dice_rot_m(m, d->priv[0], d->priv[1], d->priv[2]);

    for (f = 0; f < 20; f++)
    {
        /* 重心旋转后的 z 分量 = 这个面朝向镜头的程度(两面对映时一正一负) */
        float nz = m[2][0] * d20_fc[f][0] + m[2][1] * d20_fc[f][1] + m[2][2] * d20_fc[f][2];
        float persp = DICE_CAM_DIST / (DICE_CAM_DIST - nz);
        float area  = nz * persp * persp;                  /* 越大 = 屏幕上这块面越大 */

        if (area > best)
        {
            best = area;
            bf = f;
        }
    }

    return d20_num[bf];
}

/**
 * @brief       投放一颗 D20 时的补充初始化
 * @param       d: 库已经填好 位置/直径/颜色/转速
 * @note        这里只管本型号自己的姿态(priv 里三个旋转角)和接触系数; 起始姿态错开, 几颗骰子不会同步转
 */
static void d20_model_spawn(dice_t *d)
{
    s_spawned++;

    d->contact_k = D20_CONTACT_K;

    d->priv[0] = 0.35f * (float)s_spawned;
    d->priv[1] = 0.55f * (float)s_spawned;
    d->priv[2] = 0.0f;
    d->priv[3] = 0.0f;
}

/**
 * @brief       画这一颗骰子, 并按真实时间推进它的姿态
 * @param       d : 这颗骰子(位置已由库移动好)
 * @param       dt: 距上一帧的秒数
 * @note        由 Dice.c 的 dice_event_update() 在限帧后逐颗调用
 */
static void d20_model_frame(dice_t *d, float dt)
{
    int k;

    if (s_geo_ready == 0)return;

    d20_shadow = (d->flags & DICE_FLAG_SHADOW) != 0;

    /* 背景/阴影色每帧只算一次像素值(它们不随骰子变) */
    s_px_bg  = s_port->rgb(d20_model_bg());
    if (d20_shadow)
    {
        s_px_sho = s_port->rgb(D20_SHADOW_OUT);
        s_px_shi = s_port->rgb(D20_SHADOW_IN);
    }

    /* 这颗骰子的颜色/姿态装进渲染用的全局量 */
    s_col_face = d->color;
    s_col_edge = dice_lighten(d->color);
    s_col_num  = d20_readable(d->color);

    d20_ang[0] = d->priv[0];
    d20_ang[1] = d->priv[1];
    d20_ang[2] = d->priv[2];

    d20_layout(d);

    /* 记下本帧的矩形和中心: 下一帧要按圆/椭圆把这块(加上这一帧走过的位置)重画掉 */
    d->bx0 = (int16_t)d20_bbox[0];
    d->by0 = (int16_t)d20_bbox[1];
    d->bx1 = (int16_t)d20_bbox[2];
    d->by1 = (int16_t)d20_bbox[3];

    d20_render();

    d->px = (int16_t)d20_cx;
    d->py = (int16_t)d20_cy;

    /* 更新姿态: 三个轴速度不同, 得到翻滚效果 */
    for (k = 0; k < 3; k++)
    {
        d->priv[k] += d->spin[k] * dt;                     /* 三轴各自角速度, 符号=转向 */

        if (d->priv[k] > DICE_2PI)d->priv[k] -= DICE_2PI;     /* 及时回绕, 避免浮点精度损失 */
        else if (d->priv[k] < 0.0f)d->priv[k] += DICE_2PI;
    }
}


/******************************************************************************************/
/* 模型描述: 由 Dice 库选用(main.c: dice_init(port) 之后 dice_spawn(&dice_d20, ...)) */

const dice_model_t dice_d20 =
{
    d20_model_init,     /* 端口 + 几何(只算一次) */
    d20_model_spawn,    /* 投放时的初始姿态 */
    d20_model_frame,    /* 画一颗 + 推进姿态 */
    d20_model_bg,       /* 面板底色(Dice 库擦除用) */
    d20_model_reset,    /* dice_clear() 时复位起始姿态计数 */
    d20_model_value     /* 此刻的值: 屏幕上看最大那一面的点数 */
};
