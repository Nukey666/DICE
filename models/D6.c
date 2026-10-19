/**
 ****************************************************************************************************
 * @file        D6.c
 * @brief       D6 模型: 立方体(1~6 点), 6 个正方形面, 每面拆成 2 个三角形
 * @note        投影/着光/扫描行/点阵数字都在 Dice.c 的通用画法里, 本文件只有几何和规则:
 *              8 个顶点 + 6 个正方形面(绕序朝外), 对面点数之和恒为 7。
 * @note        正方形拆成两个三角形只影响填充/求交; 点数必须按"面"画, 所以另有一张 quad 表,
 *              逐三角形的重心会被对角线拉偏, 一个面上就会出现两个数字
 ****************************************************************************************************
 */

#include "D6.h"
#include <math.h>

/* 移植接口: 由 dice_init() 传入, 所有绘制和时钟都从这里出去 */
static const dice_port_t *s_port;


/******************************************************************************************/
/* 立方体原始数据: 8 个顶点 + 6 个正方形面(每个面拆成 2 个三角形 = 12) */

static const float d6_vsrc[8][3] =
{
    {-1.0f, -1.0f, -1.0f}, { 1.0f, -1.0f, -1.0f},
    { 1.0f,  1.0f, -1.0f}, {-1.0f,  1.0f, -1.0f},
    {-1.0f, -1.0f,  1.0f}, { 1.0f, -1.0f,  1.0f},
    { 1.0f,  1.0f,  1.0f}, {-1.0f,  1.0f,  1.0f}
};

/* 6 个正方形面(从外面看逆时针); 第 f 面拆成 (0,1,2) 和 (0,2,3) 两个三角形 */
static const uint8_t d6_qsrc[6][4] =
{
    {4, 5, 6, 7},       /* +Z 前 */
    {1, 0, 3, 2},       /* -Z 后 */
    {5, 1, 2, 6},       /* +X 右 */
    {0, 4, 7, 3},       /* -X 左 */
    {7, 6, 2, 3},       /* +Y 上 */
    {0, 1, 5, 4}        /* -Y 下 */
};


/******************************************************************************************/
/* 模块状态 */

static float    d6_vtx[8][3];                   /* 归一化到单位球的模型顶点 */
static uint8_t  d6_quad[6][4];                  /* 面索引(四边形, 点数按面画) */
static uint8_t  d6_tri[12][3];                  /* 由 quad 拆出的三角形, 绕序已统一(叉积朝外) */

static float    d6_tx[8];                       /* 旋转后顶点 X */
static float    d6_ty[8];                       /* 旋转后顶点 Y */
static float    d6_tz[8];                       /* 旋转后顶点 Z(朝向观察者为正) */
static int      d6_sx[8];                       /* 投影后的屏幕 X */
static int      d6_sy[8];                       /* 投影后的屏幕 Y */

static uint16_t d6_sw, d6_sh;                   /* 屏幕宽高 */
static int      d6_cx, d6_cy;                   /* 舞台中心 */
static float    d6_scale;                       /* 单位模型半径对应的像素数 */
static int      d6_stage[4];                    /* 本帧逐行循环的上下界 x0,x1 是粗界, 精确左右界按形状算 */
static int      d6_bbox[4];                     /* 本帧自己的包围矩形, 画完写回 d->bx0..by1 */
static int      d6_pcx, d6_pcy;                 /* 上一帧的整数中心(擦掉旧位置用) */
static uint8_t  d6_prev_ok;                     /* 上一帧画过没有 */

static int      d6_rr;                          /* 骰子投影半径上界(像素) */
static int      d6_shy;                         /* 地面阴影中心 y */
static int      d6_sha;                         /* 地面阴影横半轴 */
static int      d6_shb;                         /* 地面阴影纵半轴 */
static int      d6_shadow;                      /* 当前这颗要不要画阴影(来自 dice_t.flags) */

static float    d6_ang[3];                      /* 当前这颗骰子的三个旋转角(渲染用) */

static uint8_t      s_geo_ready = 0;            /* 顶点/面只归一化一次, 点数自检不过就一直不画 */
static uint32_t     s_col_face;                 /* 当前这颗骰子的面色 RGB888 */
static uint32_t     s_col_edge;                 /* 棱边色(由面色提亮得到, 和 D20 一样) */
static uint32_t     s_col_num;                  /* 点数色(按面色亮度取黑/白) */
static dice_pixel_t s_px_bg;                    /* 背景色的像素缓存 */
static dice_pixel_t s_px_sho;                   /* 阴影外圈 */
static dice_pixel_t s_px_shi;                   /* 阴影内圈 */

/* 面的点数: 标准 D6 对面点数之和恒为 7, 编号在初始化时按几何对映关系分配 */
static uint8_t d6_num[6];                       /* 各面的点数(1~6) */
static int8_t  d6_opp[6];                       /* 各面的对映面下标 */
static float   d6_fc[6][3];                     /* 各面重心(初始化时算一次), 方向即面法线 */

/* 已经投放的骰子 */
static uint8_t   s_spawned;                     /* 已投放几颗, 用来把起始姿态错开 */

/* ---- 每帧的可见三角形 ---- */
static int      d6_vx[12][3];                   /* 可见面 3 个顶点的屏幕 X */
static int      d6_vy[12][3];                   /* 可见面 3 个顶点的屏幕 Y */
static dice_pixel_t d6_vc[12];                  /* 可见面填充色(本屏像素) */

/* ---- 每个面的点数框: 面可能由两个三角形组成, 所以按面记而不是按三角形 ---- */
static int      d6_fn[6][4];                    /* {x, y, 位数, 数值}, 位数为 0 表示不画 */
static dice_pixel_t d6_fnc[6];                  /* 各面点数颜色 */
static dice_pixel_t d6_fe[6];                   /* 各面棱边颜色(提亮色, 按面的光照再调一次) */
static int      d6_fx[6], d6_fy[6];             /* 各面投影中心(4 个顶点平均) */
static long     d6_farea[6];                    /* 各面本帧的 2x 屏幕面积(两个三角形之和) */
static uint8_t  d6_fvis[6];                     /* 这一面本帧有正面吗 */
static float    d6_nscale = 1.0f;               /* 点数放大倍数(浮点), 随骰子直径连续变 */


static int d6_iround(float v)
{
    return (int)((v >= 0.0f) ? (v + 0.5f) : (v - 0.5f));
}

/**
 * @brief       把本帧各面的点数中, 落在 y 行的那些画到本行缓冲
 */
static void d6_number_row(int y)
{
    int f, t;

    for (f = 0; f < 6; f++)
    {
        const int *nb = d6_fn[f];
        int r = y - nb[1];
        float s = d6_nscale;

        if ((nb[2] == 0) || (r < 0) || (r >= (int)(12.0f * s)))continue;

        for (t = 0; t < nb[2]; t++)
        {
            dice_canvas_digit(nb[3], nb[0] + (int)((float)t * 6.0f * s), r, s, d6_fnc[f]);
        }
    }
}

/**
 * @brief       分配点数: 对面点数之和恒为 7
 * @retval      1 = 编号通过自检(几何表没被改坏), 0 = 失败(调用方据此不画)
 */
static uint8_t d6_number_faces(void)
{
    int f, g, k;

    /* 1. 各面重心: 4 个顶点平均(立方体的面重心方向就是朝外的面法线) */
    for (f = 0; f < 6; f++)
    {
        d6_fc[f][0] = 0.0f;
        d6_fc[f][1] = 0.0f;
        d6_fc[f][2] = 0.0f;

        for (k = 0; k < 4; k++)
        {
            int v = d6_quad[f][k];

            d6_fc[f][0] += d6_vtx[v][0];
            d6_fc[f][1] += d6_vtx[v][1];
            d6_fc[f][2] += d6_vtx[v][2];
        }

        d6_fc[f][0] *= 0.25f;
        d6_fc[f][1] *= 0.25f;
        d6_fc[f][2] *= 0.25f;
    }

    /* 2. 对映面: 立方体中心对称, 两面的重心互为相反数, 有且只有一个 */
    for (f = 0; f < 6; f++)
    {
        d6_opp[f] = -1;

        for (g = 0; g < 6; g++)
        {
            float dx = d6_fc[f][0] + d6_fc[g][0];
            float dy = d6_fc[f][1] + d6_fc[g][1];
            float dz = d6_fc[f][2] + d6_fc[g][2];

            if ((dx * dx + dy * dy + dz * dz) < 0.0001f)
            {
                d6_opp[f] = (int8_t)g;
                break;
            }
        }

        if (d6_opp[f] < 0)return 0;
    }

    /* 3. 成对编号: 一对对面分到 n 和 7-n */
    for (f = 0, k = 1; f < 6; f++)
    {
        if (d6_num[f] != 0)continue;

        d6_num[f] = (uint8_t)k;
        d6_num[d6_opp[f]] = (uint8_t)(7 - k);
        k++;
    }

    /* 4. 自检: 1~6 各出现一次, 且每对对面之和都是 7 */
    for (f = 0; f < 6; f++)
    {
        if ((d6_num[f] == 0) || (d6_num[f] > 6))return 0;
        if (((int)d6_num[f] + (int)d6_num[d6_opp[f]]) != 7)return 0;

        for (g = f + 1; g < 6; g++)
        {
            if (d6_num[g] == d6_num[f])return 0;
        }
    }

    return 1;
}


/******************************************************************************************/
/* 布局 */

/**
 * @brief       按这颗骰子的中心/直径算出投影比例, 阴影参数和每帧重绘的上下界
 */
static void d6_layout(const dice_t *d)
{
    int b0, shadow, yoff;

    shadow = (d->flags & DICE_FLAG_SHADOW) != 0;

    d6_sw = s_port->width;
    d6_sh = s_port->height;

    d6_cx = d6_iround(d->x);
    d6_cy = d6_iround(d->y);

    /* 直径 -> 像素比例: 投影半径峰值 = scale * D6_PROJ_PEAK, 让它正好等于骰子半径 */
    d6_scale = (float)d->dia * 0.5f / D6_PROJ_PEAK;

    d6_rr = (int)(d6_scale * D6_PROJ_PEAK) + 2;     /* 骰子投影半径上界 */

    /* 点数大小跟着直径连续变: 字高 ≈ 12 * nscale, 约骰子直径的 24% */
    d6_nscale = dice_num_scale((float)d6_rr);

    yoff    = (int)(d6_scale * 0.95f);              /* 阴影中心相对骰子中心的下移量 */
    d6_shy = d6_cy + yoff;
    d6_sha = (int)(d6_scale * 0.80f);               /* 地面阴影横半轴 */
    d6_shb = (int)(d6_scale * 0.12f);               /* 地面阴影纵半轴 */
    if (d6_shb < 1)d6_shb = 1;

    d6_pcx     = d->px;                             /* 上一帧中心: rr/sha/shb 只跟直径有关, 不随帧变 */
    d6_pcy     = d->py;
    d6_prev_ok = (uint8_t)(d->bx1 > d->bx0);        /* 上一帧真的画过 */

    /* 本帧自己的矩形 = 投影圆 ∪ 阴影, 画完写回 d->bx*(dice_clear 用它擦) */
    d6_bbox[0] = d6_cx - d6_rr - 1;
    d6_bbox[1] = d6_cy - d6_rr - 1;
    d6_bbox[2] = d6_cx + d6_rr + 1;
    d6_bbox[3] = d6_cy + d6_rr + 1;
    if (shadow && ((d6_shy + d6_shb + 1) > d6_bbox[3]))d6_bbox[3] = d6_shy + d6_shb + 1;

    d6_stage[0] = d6_bbox[0];
    d6_stage[1] = d6_bbox[1];
    d6_stage[2] = d6_bbox[2];
    d6_stage[3] = d6_bbox[3];

    /* 逐行循环的上下界还要并上上一帧的圆和阴影(左右界在 d6_render 里按形状算)。
       注意: 并集只进 stage, 绝不能写回 d->bx*, 否则每帧并一次很快扩到整屏 */
    if (d6_prev_ok)
    {
        b0 = d6_pcx - d6_rr - 1;
        if (b0 < d6_stage[0])d6_stage[0] = b0;

        b0 = d6_pcy - d6_rr - 1;
        if (b0 < d6_stage[1])d6_stage[1] = b0;

        b0 = d6_pcx + d6_rr + 1;
        if (b0 > d6_stage[2])d6_stage[2] = b0;

        b0 = d6_pcy + d6_rr + 1;
        if (shadow && ((d6_pcy + yoff + d6_shb + 1) > b0))b0 = d6_pcy + yoff + d6_shb + 1;
        if (b0 > d6_stage[3])d6_stage[3] = b0;
    }

    if (d6_stage[0] < 0)d6_stage[0] = 0;
    if (d6_stage[1] < 0)d6_stage[1] = 0;
    if (d6_stage[2] > (int)d6_sw - 1)d6_stage[2] = (int)d6_sw - 1;
    if (d6_stage[3] > (int)d6_sh - 1)d6_stage[3] = (int)d6_sh - 1;
}

/******************************************************************************************/
/* 初始化 */

/**
 * @brief       初始化本型号: 顶点归一化 -> 拆三角形/统一绕序 -> 分配点数
 * @note        由 dice_spawn() 第一次投放时调用; 重复调用是幂等的
 */
static void d6_model_init(const dice_port_t *port)
{
    int i, t, f, a, b, c;
    float len, e1x, e1y, e1z, e2x, e2y, e2z;
    float nx, ny, nz, px, py, pz, dot;
    float fx, fy, fz;

    s_port = port;
    if (!s_port || (s_port->width == 0) || (s_port->height == 0))return;

    /* 1. 顶点归一化到单位球面(= 立方体的外接球) */
    for (i = 0; i < 8; i++)
    {
        px = d6_vsrc[i][0];
        py = d6_vsrc[i][1];
        pz = d6_vsrc[i][2];
        len = sqrtf(px * px + py * py + pz * pz);

        d6_vtx[i][0] = px / len;
        d6_vtx[i][1] = py / len;
        d6_vtx[i][2] = pz / len;
    }

    for (f = 0; f < 6; f++)
    {
        for (i = 0; i < 4; i++)d6_quad[f][i] = d6_qsrc[f][i];
    }

    /* 2. 每个面拆成两个三角形, 并统一绕序, 使 (v1-v0)x(v2-v0) 始终指向球外 */
    for (t = 0; t < 12; t++)
    {
        f = t >> 1;

        d6_tri[t][0] = d6_quad[f][0];
        d6_tri[t][1] = (t & 1) ? d6_quad[f][2] : d6_quad[f][1];
        d6_tri[t][2] = (t & 1) ? d6_quad[f][3] : d6_quad[f][2];

        a = d6_tri[t][0];
        b = d6_tri[t][1];
        c = d6_tri[t][2];

        e1x = d6_vtx[b][0] - d6_vtx[a][0];
        e1y = d6_vtx[b][1] - d6_vtx[a][1];
        e1z = d6_vtx[b][2] - d6_vtx[a][2];
        e2x = d6_vtx[c][0] - d6_vtx[a][0];
        e2y = d6_vtx[c][1] - d6_vtx[a][1];
        e2z = d6_vtx[c][2] - d6_vtx[a][2];

        nx = e1y * e2z - e1z * e2y;
        ny = e1z * e2x - e1x * e2z;
        nz = e1x * e2y - e1y * e2x;

        fx = (d6_vtx[a][0] + d6_vtx[b][0] + d6_vtx[c][0]) / 3.0f;
        fy = (d6_vtx[a][1] + d6_vtx[b][1] + d6_vtx[c][1]) / 3.0f;
        fz = (d6_vtx[a][2] + d6_vtx[b][2] + d6_vtx[c][2]) / 3.0f;

        dot = nx * fx + ny * fy + nz * fz;

        if (dot < 0.0f)                             /* 法线朝内, 交换两个顶点即可翻转绕序 */
        {
            d6_tri[t][1] = (uint8_t)c;
            d6_tri[t][2] = (uint8_t)b;
        }
    }

    /* 3. 分配点数: 对面之和恒为 7; 自检不过就一直不画(几何表打错时宁可不显示) */
    s_geo_ready = d6_number_faces();
}


/******************************************************************************************/
/* 渲染 */

/**
 * @brief       渲染一帧画面
 */
static void d6_render(void)
{
    float m[3][3];
    float px, py, pz, persp;
    float e1x, e1y, e1z, e2x, e2y, e2z;
    float nx, ny, nz, nlen;
    float fx, fy, fz, vx, vy, vz;
    float ndl, s2, s4, spec;
    int i, f, k, t, nvis, y, ytop, ybot;
    int dy;

    /* ---- 1. 合成旋转矩阵 R = Rz * Ry * Rx ---- */
    dice_rot_m(m, d6_ang[0], d6_ang[1], d6_ang[2]);

    /* ---- 2. 顶点旋转 + 透视投影 ---- */
    for (i = 0; i < 8; i++)
    {
        px = d6_vtx[i][0];
        py = d6_vtx[i][1];
        pz = d6_vtx[i][2];

        vx = m[0][0] * px + m[0][1] * py + m[0][2] * pz;
        vy = m[1][0] * px + m[1][1] * py + m[1][2] * pz;
        vz = m[2][0] * px + m[2][1] * py + m[2][2] * pz;

        d6_tx[i] = vx;
        d6_ty[i] = vy;
        d6_tz[i] = vz;

        persp = DICE_CAM_DIST / (DICE_CAM_DIST - vz);     /* 透视缩放, z=0 时为 1 */

        d6_sx[i] = d6_cx + d6_iround(vx * persp * d6_scale);
        d6_sy[i] = d6_cy - d6_iround(vy * persp * d6_scale);    /* 屏幕Y轴向下, 需要取反 */
    }

    /* ---- 3. 每个面的屏幕中心: 用四边形的 4 个顶点平均(按三角形平均会被对角线拉偏) ---- */
    for (f = 0; f < 6; f++)
    {
        int a0 = d6_quad[f][0];
        int a1 = d6_quad[f][1];
        int a2 = d6_quad[f][2];
        int a3 = d6_quad[f][3];

        d6_fx[f] = (d6_sx[a0] + d6_sx[a1] + d6_sx[a2] + d6_sx[a3]) / 4;
        d6_fy[f] = (d6_sy[a0] + d6_sy[a1] + d6_sy[a2] + d6_sy[a3]) / 4;
        d6_farea[f] = 0;
        d6_fvis[f]  = 0;
        d6_fn[f][2] = 0;
    }

    /* ---- 4. 逐三角形: 剔除 + 光照, 把可见的收集起来, 面积累加到所在面 ---- */
    nvis = 0;

    for (t = 0; t < 12; t++)
    {
        int i0 = d6_tri[t][0];
        int i1 = d6_tri[t][1];
        int i2 = d6_tri[t][2];

        /* 外法线(顶点已旋转, 叉积结果仍指向球外) */
        e1x = d6_tx[i1] - d6_tx[i0];
        e1y = d6_ty[i1] - d6_ty[i0];
        e1z = d6_tz[i1] - d6_tz[i0];
        e2x = d6_tx[i2] - d6_tx[i0];
        e2y = d6_ty[i2] - d6_ty[i0];
        e2z = d6_tz[i2] - d6_tz[i0];

        nx = e1y * e2z - e1z * e2y;
        ny = e1z * e2x - e1x * e2z;
        nz = e1x * e2y - e1y * e2x;

        nlen = sqrtf(nx * nx + ny * ny + nz * nz);
        if (nlen < 0.000001f)continue;                  /* 退化面 */

        nx /= nlen;
        ny /= nlen;
        nz /= nlen;

        /* 面重心(三角形) */
        fx = (d6_tx[i0] + d6_tx[i1] + d6_tx[i2]) / 3.0f;
        fy = (d6_ty[i0] + d6_ty[i1] + d6_ty[i2]) / 3.0f;
        fz = (d6_tz[i0] + d6_tz[i1] + d6_tz[i2]) / 3.0f;

        /* 背面剔除: 摄像机位于(0, 0, CAM), 法线与"重心->摄像机"夹角小于90度才算正面 */
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

        d6_vc[nvis] = dice_shade(s_col_face, ndl, spec);

        d6_vx[nvis][0] = d6_sx[i0]; d6_vy[nvis][0] = d6_sy[i0];
        d6_vx[nvis][1] = d6_sx[i1]; d6_vy[nvis][1] = d6_sy[i1];
        d6_vx[nvis][2] = d6_sx[i2]; d6_vy[nvis][2] = d6_sy[i2];

        /* 面积(2 倍)累加到所在的面: 点数按面画, 所以门限也按面算 */
        {
            long area2 = (long)(d6_vx[nvis][1] - d6_vx[nvis][0]) * (long)(d6_vy[nvis][2] - d6_vy[nvis][0])
                       - (long)(d6_vx[nvis][2] - d6_vx[nvis][0]) * (long)(d6_vy[nvis][1] - d6_vy[nvis][0]);

            if (area2 < 0)area2 = -area2;

            f = t >> 1;
            d6_farea[f] += area2;
            d6_fvis[f]   = 1;
            d6_fnc[f]    = dice_shade(s_col_num, ndl, spec);
            d6_fe[f]     = dice_shade(s_col_edge, ndl, spec * 0.5f);
        }

        nvis++;
    }

    /* ---- 5. 每个可见面记一个点数框: 位置在这里定, 像素在扫描到那几行时才画 ---- */
    for (f = 0; f < 6; f++)
    {
        float s = d6_nscale;
        int tx, ty;

        if ((d6_fvis[f] == 0) || (d6_farea[f] <= (long)(d6_scale * d6_scale * D6_NUM_AREA_K)))continue;

        tx = d6_fx[f] - (int)(3.0f * s);                /* 1 位数: 6*s 宽, 居中 */
        ty = d6_fy[f] - (int)(6.0f * s);                /* 12*s 高 */

        if ((tx >= 2) && (ty >= 2) &&
            ((tx + (int)(6.0f * s)) < (int)d6_sw - 2) &&
            ((ty + (int)(12.0f * s)) < (int)d6_sh - 2))
        {
            d6_fn[f][0] = tx;
            d6_fn[f][1] = ty;
            d6_fn[f][2] = 1;
            d6_fn[f][3] = d6_num[f];
        }
    }

    /* ---- 6. 逐行合成 ----
       每行只碰"本帧骰子圆/阴影 ∪ 上一帧骰子圆/阴影"真正覆盖的像素, 矩形四角永远不写:
       方框保护会把骰子方框的四角(别人的地盘)每帧擦一遍, 圆/椭圆保护不会, 重叠时才不闪。 */
    ytop = d6_stage[1];
    ybot = d6_stage[3];

    for (y = ytop; y <= ybot; y++)
    {
        int l = (int)d6_sw, r = -1;                     /* 本行要重绘的左右边界 */
        int w;

        w = dice_halfw(d6_cx, d6_cy, d6_rr, d6_rr, y);
        if (w >= 0)
        {
            l = d6_cx - w;
            r = d6_cx + w;
        }

        if (d6_prev_ok)
        {
            w = dice_halfw(d6_pcx, d6_pcy, d6_rr, d6_rr, y);
            if (w >= 0)
            {
                if ((d6_pcx - w) < l)l = d6_pcx - w;
                if ((d6_pcx + w) > r)r = d6_pcx + w;
            }
        }

        if (d6_shadow)
        {
            w = dice_halfw(d6_cx, d6_shy, d6_sha, d6_shb, y);
            if (w >= 0)
            {
                if ((d6_cx - w) < l)l = d6_cx - w;
                if ((d6_cx + w) > r)r = d6_cx + w;
            }

            if (d6_prev_ok)
            {
                w = dice_halfw(d6_pcx, d6_pcy + (d6_shy - d6_cy), d6_sha, d6_shb, y);
                if (w >= 0)
                {
                    if ((d6_pcx - w) < l)l = d6_pcx - w;
                    if ((d6_pcx + w) > r)r = d6_pcx + w;
                }
            }
        }

        if (r < l)continue;

        if (l < d6_stage[0])l = d6_stage[0];
        if (r > d6_stage[2])r = d6_stage[2];
        if (r > DICE_ROW_MAX - 1)r = DICE_ROW_MAX - 1;
        if (l < 0)l = 0;
        if (r < l)continue;

        dice_canvas_row(l, r);

        /* 6.1 底色: 先铺面板色, 再用阴影色覆盖 */
        dice_canvas_put(l, r, s_px_bg);

        if (d6_shadow)
        {
            dy = y - d6_shy;
            if ((dy >= -d6_shb) && (dy <= d6_shb))
            {
                float tt = 1.0f - (float)(dy * dy) / (float)(d6_shb * d6_shb);
                if (tt > 0.0f)
                {
                    int so = (int)((float)d6_sha * sqrtf(tt) + 0.5f);
                    int ia = (d6_sha * 3) / 5;
                    int ib = (d6_shb * 3) / 5;

                    dice_canvas_put(d6_cx - so, d6_cx + so, s_px_sho);

                    if ((ib > 0) && (dy >= -ib) && (dy <= ib))
                    {
                        tt = 1.0f - (float)(dy * dy) / (float)(ib * ib);
                        if (tt > 0.0f)
                        {
                            int si = (int)((float)ia * sqrtf(tt) + 0.5f);
                            dice_canvas_put(d6_cx - si, d6_cx + si, s_px_shi);
                        }
                    }
                }
            }
        }

        /* 6.2 可见三角形填充(凸多面体的正面互不重叠, 直接叠加即可) */
        for (k = 0; k < nvis; k++)
        {
            int xa, xb;

            if (dice_tri_span(d6_vx[k], d6_vy[k], y, &xa, &xb))
            {
                dice_canvas_put(xa, xb, d6_vc[k]);
            }
        }

        /* 6.3 棱: 和 D20 一样用提亮色描。正方形拆成了两个三角形, 直接描三角形会在每个面上
               多出一条对角线; 按 d6_quad[] 描四条外边就只有立方体真正的棱 */
        for (f = 0; f < 6; f++)
        {
            int ex[4], ey[4];

            if (d6_fvis[f] == 0)continue;

            for (k = 0; k < 4; k++)
            {
                ex[k] = d6_sx[d6_quad[f][k]];
                ey[k] = d6_sy[d6_quad[f][k]];
            }

            dice_poly_edges(ex, ey, 4, y, d6_fe[f]);
        }

        /* 6.4 点数 */
        d6_number_row(y);

        /* 6.5 整行一次性写进LCD: 每帧每个像素只被写一次, 不存在中间状态 */
        dice_canvas_flush(y);
    }
}


/******************************************************************************************/
/* 对外接口 */

/**
 * @brief       面色 -> 点数色(亮面用黑点, 暗面用白点)
 */
static uint32_t d6_readable(uint32_t rgb)
{
    int lum = ((rgb >> 16) & 0xFF) + ((rgb >> 8) & 0xFF) + (rgb & 0xFF);

    return (uint32_t)((lum > 3 * 255 / 2) ? 0x000000 : 0xFFFFFF);
}

/**
 * @brief       本型号的面板底色
 * @note        Dice 库擦骰子/清屏时用这个色; 整屏底色必须和 D20 一致(D6_BG_COLOR)
 */
static uint32_t d6_model_bg(void)
{
    return D6_BG_COLOR;
}

/**
 * @brief       复位型号内部状态
 * @note        dice_clear() 时被调用: 起始姿态的错开计数归零, 清空后重新投放从第一颗开始
 */
static void d6_model_reset(void)
{
    s_spawned = 0;
}

/**
 * @brief       这一颗此刻的"值" = 屏幕上投影面积最大的那个面的点数
 * @param       d: 这一颗(姿态在 d->priv[0..2])
 * @note        立方体 6 个面全等, 投影面积 ∝ 面法线朝镜头的分量 × 透视缩放²;
 *              面法线方向就是面重心方向, 所以不用旋转顶点, 6 次点积就够
 */
static uint8_t d6_model_value(const dice_t *d)
{
    float m[3][3];
    float best = -1.0f;
    int bf = 0, f;

    if (d == 0)return 0;

    dice_rot_m(m, d->priv[0], d->priv[1], d->priv[2]);

    for (f = 0; f < 6; f++)
    {
        float nz = m[2][0] * d6_fc[f][0] + m[2][1] * d6_fc[f][1] + m[2][2] * d6_fc[f][2];
        float persp = DICE_CAM_DIST / (DICE_CAM_DIST - nz);
        float area  = nz * persp * persp;                   /* 越大 = 屏幕上这块面越大 */

        if (area > best)
        {
            best = area;
            bf = f;
        }
    }

    return d6_num[bf];
}

/**
 * @brief       投放一颗 D6 时的补充初始化
 * @param       d: 库已经填好 位置/直径/颜色/转速
 * @note        这里只管本型号自己的姿态(priv 里三个旋转角)、接触系数和地面阻尼
 */
static void d6_model_spawn(dice_t *d)
{
    s_spawned++;

    d->contact_k = D6_CONTACT_K;
    d->friction  = D6_FRICTION;

    d->priv[0] = 0.35f * (float)s_spawned;
    d->priv[1] = 0.55f * (float)s_spawned;
    d->priv[2] = 0.0f;
    d->priv[3] = 0.0f;
}

/**
 * @brief       画这一颗骰子, 并按真实时间推进它的姿态
 * @param       d : 这颗骰子(位置已由库移动好)
 * @param       dt: 距上一帧的秒数
 */
static void d6_model_frame(dice_t *d, float dt)
{
    int k;

    if (s_geo_ready == 0)return;

    d6_shadow = (d->flags & DICE_FLAG_SHADOW) != 0;

    /* 背景/阴影色每帧只算一次像素值(它们不随骰子变) */
    s_px_bg = s_port->rgb(d6_model_bg());
    if (d6_shadow)
    {
        s_px_sho = s_port->rgb(D6_SHADOW_OUT);
        s_px_shi = s_port->rgb(D6_SHADOW_IN);
    }

    /* 这颗骰子的颜色/姿态装进渲染用的全局量 */
    s_col_face = d->color;
    s_col_edge = dice_lighten(d->color);
    s_col_num  = d6_readable(d->color);

    d6_ang[0] = d->priv[0];
    d6_ang[1] = d->priv[1];
    d6_ang[2] = d->priv[2];

    d6_layout(d);

    /* 先记矩形再画: 渲染中途出错时, 上一帧的矩形至少还留着可擦 */
    d->bx0 = (int16_t)d6_bbox[0];
    d->by0 = (int16_t)d6_bbox[1];
    d->bx1 = (int16_t)d6_bbox[2];
    d->by1 = (int16_t)d6_bbox[3];

    d6_render();

    d->px = (int16_t)d6_cx;
    d->py = (int16_t)d6_cy;

    /* 最后才推进姿态: 本帧画的是上一时刻的姿态, 和 value() 的取值口径一致 */
    for (k = 0; k < 3; k++)
    {
        d->priv[k] += d->spin[k] * dt;                     /* 三轴各自角速度, 符号=转向 */

        if (d->priv[k] > DICE_2PI)d->priv[k] -= DICE_2PI;  /* 及时回绕, 避免浮点精度损失 */
        else if (d->priv[k] < 0.0f)d->priv[k] += DICE_2PI;
    }
}


/******************************************************************************************/
/* 模型描述: 由 Dice 库选用(main.c: dice_init(port) 之后 dice_spawn(&dice_d6, ...)) */

const dice_model_t dice_d6 =
{
    d6_model_init,      /* 端口 + 几何(只算一次) */
    d6_model_spawn,     /* 投放时的初始姿态 + 接触系数/阻尼 */
    d6_model_frame,     /* 画一颗 + 推进姿态 */
    d6_model_bg,        /* 面板底色(Dice 库擦除用) */
    d6_model_reset,     /* dice_clear() 时复位起始姿态计数 */
    d6_model_value      /* 此刻的值: 屏幕上看最大那一面的点数 */
};
