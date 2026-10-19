/**
 ****************************************************************************************************
 * @file        Poly.c
 * @brief       多面体骰子的通用画法引擎: 一份画法, 型号各一个文件只填几何表
 * @note        这里的画法是从 D20/D6 抽出来的同一套: 旋转 -> 透视投影 -> 按面剔背面 -> 着光 ->
 *              逐行合成(底色/阴影/面/棱/点数) -> 整行一次上屏。D4 和 D12 只差几何表里的几行。
 * @note        面法线在初始化时由"面顶点叉积"算出并朝外(不假设重心就是法线, 偏方面体的面不是正多边形);
 *              背面剔除用 n_rot·摄像机 > 面平面距离, 对任何凸多面体都成立。
 ****************************************************************************************************
 */

#include "Poly.h"
#include <math.h>

/* 移植接口: 由 dice_init() 传入 */
static const dice_port_t *s_port;


/******************************************************************************************/
/* 几何表在各自的型号文件里(D4.c / D8.c / D10.c / D12.c), 顶点都在同一球面,
   面表绕序朝外、不足 vf 列补首顶点; 本文件只管画法 */

/* 当前这一帧正在画的型号 */
static const poly_geo_t *s_g;
static poly_inst_t      *s_in;

/* ---- 每帧的即时状态(一颗画完再画下一颗, 所以可以共享) ---- */
static uint16_t s_sw, s_sh;             /* 屏幕宽高 */
static int      s_cx, s_cy;             /* 这颗的中心 */
static float    s_scale;                /* 单位模型半径对应的像素数 */
static int      s_rr;                   /* 投影半径上界(擦除用) */
static float    s_nscale = 1.0f;        /* 点数放大倍数 */
static int      s_stage[4];             /* 逐行循环上下界(本帧 ∪ 上一帧) */
static int      s_bbox[4];              /* 本帧自己的矩形, 写回 d->bx* */
static int      s_pcx, s_pcy;           /* 上一帧的中心 */
static uint8_t  s_prev_ok;
static int      s_shy, s_sha, s_shb;    /* 地面阴影 */
static uint8_t  s_shadow;
static float    s_ang[3];               /* 三个旋转角 */

static float    s_tx[POLY_MAX_V], s_ty[POLY_MAX_V], s_tz[POLY_MAX_V];
static int      s_sx[POLY_MAX_V], s_sy[POLY_MAX_V];

/* 可见面: 屏幕多边形 / 颜色 / 点数框 */
static uint8_t       s_vis[POLY_MAX_F];         /* 本帧可见的面下标 */
static int           s_nvis;
static int           s_pex[POLY_MAX_F][POLY_MAX_VF];
static int           s_pey[POLY_MAX_F][POLY_MAX_VF];
static dice_pixel_t  s_fcol[POLY_MAX_F];        /* 面填充色 */
static dice_pixel_t  s_fecol[POLY_MAX_F];       /* 棱色 */
static dice_pixel_t  s_fdcol[POLY_MAX_F];       /* 点数色 */
static int           s_fn[POLY_MAX_F][4];       /* {x, y, 位数, 数值} 位数为 0 不画 */

static uint32_t      s_col_face, s_col_edge, s_col_num;
static dice_pixel_t  s_px_bg, s_px_sho, s_px_shi;

static const uint32_t s_pow10[2] = {1u, 10u};


static int poly_iround(float v)
{
    return (int)((v >= 0.0f) ? (v + 0.5f) : (v - 0.5f));
}

/**
 * @brief       分配点数: 有对面的形状按"对面之和 = opp_sum"成对编, 没有的按顺序 1~nf
 * @retval      1 = 自检通过, 0 = 几何表有问题(调用方据此不画)
 */
static uint8_t poly_number_faces(poly_inst_t *in)
{
    const poly_geo_t *g = in->geo_ptr;
    int f, g2, k, nf = g->nf;

    if (g->opp_sum == 0)                        /* 四面体: 没有对面, 顺序编号 */
    {
        for (f = 0; f < nf; f++)in->num[f] = (uint8_t)(f + 1);
        return 1;
    }

    /* 1. 成对编号: 一对互为反向的法线分到 n 和 opp_sum-n */
    for (f = 0; f < nf; f++)in->num[f] = 0;

    for (f = 0, k = 1; f < nf; f++)
    {
        int opp = -1;

        if (in->num[f] != 0)continue;

        for (g2 = 0; g2 < nf; g2++)
        {
            float dx = in->fn[f][0] + in->fn[g2][0];
            float dy = in->fn[f][1] + in->fn[g2][1];
            float dz = in->fn[f][2] + in->fn[g2][2];

            if ((dx * dx + dy * dy + dz * dz) < 0.001f)
            {
                opp = g2;
                break;
            }
        }

        if (opp < 0)return 0;                   /* 找不到对面: 表错了 */

        in->num[f]  = (uint8_t)k;
        in->num[opp] = (uint8_t)((int)g->opp_sum - k);
        k++;
    }

    /* 2. 自检: 1~nf 各出现一次, 每对对面之和都是 opp_sum */
    for (f = 0; f < nf; f++)
    {
        if ((in->num[f] == 0) || (in->num[f] > nf))return 0;

        for (g2 = f + 1; g2 < nf; g2++)
        {
            if (in->num[g2] == in->num[f])return 0;
        }
    }

    return 1;
}

/**
 * @brief       初始化一种型号: 顶点归一化 -> 面法线/平面距离 -> 分配点数
 */
void poly_model_init(poly_inst_t *in, const poly_geo_t *g, const dice_port_t *port)
{
    int i, f, a, b, c, t;
    float len, maxlen = 0.0f;
    float e1x, e1y, e1z, e2x, e2y, e2z;
    float nx, ny, nz, nl;
    float cx, cy, cz;

    s_port = port;
    in->geo_ptr = g;

    if (!s_port || (s_port->width == 0) || (s_port->height == 0))return;

    /* 1. 统一归一化: 用同一个比例, 形状不变形(否则偏方面体的面会变不共面) */
    for (i = 0; i < g->nv; i++)
    {
        len = sqrtf(g->vsrc[i][0] * g->vsrc[i][0] + g->vsrc[i][1] * g->vsrc[i][1] +
                    g->vsrc[i][2] * g->vsrc[i][2]);

        if (len > maxlen)maxlen = len;
    }

    if (maxlen < 0.000001f)return;

    for (i = 0; i < g->nv; i++)
    {
        in->vtx[i][0] = g->vsrc[i][0] / maxlen;
        in->vtx[i][1] = g->vsrc[i][1] / maxlen;
        in->vtx[i][2] = g->vsrc[i][2] / maxlen;
    }

    /* 2. 每个面的外法线(前三个顶点叉积, 朝外)和平面距离 */
    for (f = 0; f < g->nf; f++)
    {
        a = g->fsrc[f][0];
        b = g->fsrc[f][1];
        c = g->fsrc[f][2];

        e1x = in->vtx[b][0] - in->vtx[a][0];
        e1y = in->vtx[b][1] - in->vtx[a][1];
        e1z = in->vtx[b][2] - in->vtx[a][2];
        e2x = in->vtx[c][0] - in->vtx[a][0];
        e2y = in->vtx[c][1] - in->vtx[a][1];
        e2z = in->vtx[c][2] - in->vtx[a][2];

        nx = e1y * e2z - e1z * e2y;
        ny = e1z * e2x - e1x * e2z;
        nz = e1x * e2y - e1y * e2x;

        nl = sqrtf(nx * nx + ny * ny + nz * nz);

        if (nl < 0.000001f)return;              /* 退化面: 表错了, 不画 */

        in->fn[f][0] = nx / nl;
        in->fn[f][1] = ny / nl;
        in->fn[f][2] = nz / nl;

        /* 面重心, 用来判断法线朝里还是朝外 */
        cx = 0.0f; cy = 0.0f; cz = 0.0f;
        for (t = 0; t < g->vf; t++)
        {
            int v = g->fsrc[f][t];

            cx += in->vtx[v][0];
            cy += in->vtx[v][1];
            cz += in->vtx[v][2];
        }
        cx /= (float)g->vf; cy /= (float)g->vf; cz /= (float)g->vf;

        if ((in->fn[f][0] * cx + in->fn[f][1] * cy + in->fn[f][2] * cz) < 0.0f)
        {
            in->fn[f][0] = -in->fn[f][0];
            in->fn[f][1] = -in->fn[f][1];
            in->fn[f][2] = -in->fn[f][2];
        }

        in->fd[f] = in->fn[f][0] * in->vtx[a][0] + in->fn[f][1] * in->vtx[a][1] +
                    in->fn[f][2] * in->vtx[a][2];
    }

    /* 3. 点数 */
    in->num_ok = poly_number_faces(in);
    in->spawned = 0;
}


/******************************************************************************************/
/* 布局: 和 D20/D6 一样, 只是半径/点数比例都从这里取 */

static void poly_layout(const dice_t *d)
{
    int b0, yoff;

    s_sw = s_port->width;
    s_sh = s_port->height;

    s_cx = poly_iround(d->x);
    s_cy = poly_iround(d->y);

    s_scale = (float)d->dia * 0.5f / POLY_PROJ_PEAK;
    s_rr    = (int)(s_scale * POLY_PROJ_PEAK) + 2;
    s_nscale = dice_num_scale((float)s_rr);

    yoff   = (int)(s_scale * 0.95f);
    s_shy  = s_cy + yoff;
    s_sha  = (int)(s_scale * 0.80f);
    s_shb  = (int)(s_scale * 0.12f);
    if (s_shb < 1)s_shb = 1;

    s_pcx     = d->px;
    s_pcy     = d->py;
    s_prev_ok = (uint8_t)(d->bx1 > d->bx0);

    s_bbox[0] = s_cx - s_rr - 1;
    s_bbox[1] = s_cy - s_rr - 1;
    s_bbox[2] = s_cx + s_rr + 1;
    s_bbox[3] = s_cy + s_rr + 1;
    if (s_shadow && ((s_shy + s_shb + 1) > s_bbox[3]))s_bbox[3] = s_shy + s_shb + 1;

    s_stage[0] = s_bbox[0];
    s_stage[1] = s_bbox[1];
    s_stage[2] = s_bbox[2];
    s_stage[3] = s_bbox[3];

    /* 并上上一帧: 只进 stage, 绝不写回 d->bx*(写回去每帧并一次, 很快扩到整屏) */
    if (s_prev_ok)
    {
        b0 = s_pcx - s_rr - 1;
        if (b0 < s_stage[0])s_stage[0] = b0;

        b0 = s_pcy - s_rr - 1;
        if (b0 < s_stage[1])s_stage[1] = b0;

        b0 = s_pcx + s_rr + 1;
        if (b0 > s_stage[2])s_stage[2] = b0;

        b0 = s_pcy + s_rr + 1;
        if (s_shadow && ((s_pcy + yoff + s_shb + 1) > b0))b0 = s_pcy + yoff + s_shb + 1;
        if (b0 > s_stage[3])s_stage[3] = b0;
    }

    if (s_stage[0] < 0)s_stage[0] = 0;
    if (s_stage[1] < 0)s_stage[1] = 0;
    if (s_stage[2] > (int)s_sw - 1)s_stage[2] = (int)s_sw - 1;
    if (s_stage[3] > (int)s_sh - 1)s_stage[3] = (int)s_sh - 1;
}


/******************************************************************************************/
/* 渲染 */

static void poly_number_row(int y)
{
    int f, t;

    for (f = 0; f < (int)s_g->nf; f++)
    {
        const int *nb = s_fn[f];
        int r = y - nb[1];
        float s = s_nscale;

        if ((nb[2] == 0) || (r < 0) || (r >= (int)(12.0f * s)))continue;

        for (t = 0; t < nb[2]; t++)
        {
            int digit = (int)((nb[3] / s_pow10[nb[2] - t - 1]) % 10u);

            dice_canvas_digit(digit, nb[0] + (int)((float)t * 6.0f * s), r, s, s_fdcol[f]);
        }
    }
}

/**
 * @brief       画这一颗: 投影 -> 剔背面 -> 逐行合成 -> 上屏
 */
static void poly_render(void)
{
    float m[3][3];
    float vx, vy, vz, persp;
    float nx, ny, nz, nl;
    float ndl, s2, s4, spec;
    float cx, cy;
    int i, f, k, v, y, dy, nv = s_g->nv, nf = s_g->nf, vf = s_g->vf;

    dice_rot_m(m, s_ang[0], s_ang[1], s_ang[2]);

    /* ---- 1. 顶点旋转 + 透视 ---- */
    for (i = 0; i < nv; i++)
    {
        vx = s_in->vtx[i][0];
        vy = s_in->vtx[i][1];
        vz = s_in->vtx[i][2];

        s_tx[i] = m[0][0] * vx + m[0][1] * vy + m[0][2] * vz;
        s_ty[i] = m[1][0] * vx + m[1][1] * vy + m[1][2] * vz;
        s_tz[i] = m[2][0] * vx + m[2][1] * vy + m[2][2] * vz;

        persp = DICE_CAM_DIST / (DICE_CAM_DIST - s_tz[i]);

        s_sx[i] = s_cx + poly_iround(s_tx[i] * persp * s_scale);
        s_sy[i] = s_cy - poly_iround(s_ty[i] * persp * s_scale);
    }

    /* ---- 2. 逐面: 剔背面 + 着光 + 屏幕多边形 + 点数框 ---- */
    s_nvis = 0;

    for (f = 0; f < nf; f++)
    {
        s_fn[f][2] = 0;

        /* 外法线旋转到观察坐标系 */
        nx = m[0][0] * s_in->fn[f][0] + m[0][1] * s_in->fn[f][1] + m[0][2] * s_in->fn[f][2];
        ny = m[1][0] * s_in->fn[f][0] + m[1][1] * s_in->fn[f][1] + m[1][2] * s_in->fn[f][2];
        nz = m[2][0] * s_in->fn[f][0] + m[2][1] * s_in->fn[f][1] + m[2][2] * s_in->fn[f][2];

        nl = sqrtf(nx * nx + ny * ny + nz * nz);
        if (nl < 0.000001f)continue;

        nx /= nl; ny /= nl; nz /= nl;

        /* 背面剔除: 摄像机在 (0,0,CAM), 面所在平面 n·p = fd, 可见 <=> nz*CAM > fd */
        if ((nz * DICE_CAM_DIST) <= s_in->fd[f])continue;

        /* 漫反射 + 高光(ndl^8) */
        ndl = nx * DICE_LIGHT_X + ny * DICE_LIGHT_Y + nz * DICE_LIGHT_Z;
        if (ndl < 0.0f)ndl = 0.0f;

        s2 = ndl * ndl;
        s4 = s2 * s2;
        spec = s4 * s4 * DICE_SPECULAR;

        s_fcol[f]  = dice_shade(s_col_face, ndl, spec);
        s_fecol[f] = dice_shade(s_col_edge, ndl, spec * 0.5f);
        s_fdcol[f] = dice_shade(s_col_num, ndl, spec);

        /* 屏幕多边形 + 屏幕重心 */
        cx = 0.0f; cy = 0.0f;
        for (v = 0; v < vf; v++)
        {
            int idx = s_g->fsrc[f][v];

            s_pex[f][v] = s_sx[idx];
            s_pey[f][v] = s_sy[idx];
            cx += (float)s_sx[idx];
            cy += (float)s_sy[idx];
        }
        cx /= (float)vf;
        cy /= (float)vf;

        s_vis[s_nvis++] = (uint8_t)f;

        /* 点数框: 面积(2 倍)门限按整个面算, 太小的面不写(写了会跑到轮廓外面) */
        {
            long area2 = 0;

            for (v = 1; v + 1 < vf; v++)
            {
                long a2 = (long)(s_pex[f][v] - s_pex[f][0]) * (long)(s_pey[f][v + 1] - s_pey[f][0])
                        - (long)(s_pex[f][v + 1] - s_pex[f][0]) * (long)(s_pey[f][v] - s_pey[f][0]);

                area2 += (a2 < 0) ? -a2 : a2;
            }

            if (area2 > (long)(s_scale * s_scale * s_g->num_area_k))
            {
                int nd = (s_in->num[f] >= 10) ? 2 : 1;
                int tx = poly_iround(cx) - (int)((float)nd * 3.0f * s_nscale);  /* 半边宽 = nd*6*nscale/2 */
                int ty = poly_iround(cy) - (int)(6.0f * s_nscale);

                if ((tx >= 2) && (ty >= 2) &&
                    ((tx + (int)((float)(nd * 6) * s_nscale)) < (int)s_sw - 2) &&
                    ((ty + (int)(12.0f * s_nscale)) < (int)s_sh - 2))
                {
                    s_fn[f][0] = tx;
                    s_fn[f][1] = ty;
                    s_fn[f][2] = nd;
                    s_fn[f][3] = s_in->num[f];
                }
            }
        }
    }

    /* ---- 3. 逐行合成: 每行只碰 本帧骰子圆/阴影 ∪ 上一帧 的像素, 矩形四角永远不写 ---- */
    for (y = s_stage[1]; y <= s_stage[3]; y++)
    {
        int l = (int)s_sw, r = -1, w;

        w = dice_halfw(s_cx, s_cy, s_rr, s_rr, y);
        if (w >= 0) { l = s_cx - w; r = s_cx + w; }

        if (s_prev_ok)
        {
            w = dice_halfw(s_pcx, s_pcy, s_rr, s_rr, y);
            if (w >= 0)
            {
                if ((s_pcx - w) < l)l = s_pcx - w;
                if ((s_pcx + w) > r)r = s_pcx + w;
            }
        }

        if (s_shadow)
        {
            w = dice_halfw(s_cx, s_shy, s_sha, s_shb, y);
            if (w >= 0)
            {
                if ((s_cx - w) < l)l = s_cx - w;
                if ((s_cx + w) > r)r = s_cx + w;
            }

            if (s_prev_ok)
            {
                w = dice_halfw(s_pcx, s_pcy + (s_shy - s_cy), s_sha, s_shb, y);
                if (w >= 0)
                {
                    if ((s_pcx - w) < l)l = s_pcx - w;
                    if ((s_pcx + w) > r)r = s_pcx + w;
                }
            }
        }

        if (r < l)continue;

        if (l < s_stage[0])l = s_stage[0];
        if (r > s_stage[2])r = s_stage[2];
        if (r > DICE_ROW_MAX - 1)r = DICE_ROW_MAX - 1;
        if (l < 0)l = 0;
        if (r < l)continue;

        dice_canvas_row(l, r);

        /* 3.1 底色 */
        dice_canvas_put(l, r, s_px_bg);

        /* 3.2 地面阴影(两个椭圆) */
        if (s_shadow)
        {
            dy = y - s_shy;
            if ((dy >= -s_shb) && (dy <= s_shb))
            {
                float tt = 1.0f - (float)(dy * dy) / (float)(s_shb * s_shb);

                if (tt > 0.0f)
                {
                    int so = (int)((float)s_sha * sqrtf(tt) + 0.5f);
                    int ia = (s_sha * 3) / 5;
                    int ib = (s_shb * 3) / 5;

                    dice_canvas_put(s_cx - so, s_cx + so, s_px_sho);

                    if ((ib > 0) && (dy >= -ib) && (dy <= ib))
                    {
                        tt = 1.0f - (float)(dy * dy) / (float)(ib * ib);
                        if (tt > 0.0f)
                        {
                            int si = (int)((float)ia * sqrtf(tt) + 0.5f);

                            dice_canvas_put(s_cx - si, s_cx + si, s_px_shi);
                        }
                    }
                }
            }
        }

        /* 3.3 可见面: 扇形拆三角形填充(凸面, 正面互不重叠), 再按面描外边 */
        for (k = 0; k < s_nvis; k++)
        {
            int f = s_vis[k];

            for (v = 1; v + 1 < vf; v++)
            {
                int tx[3], ty[3], xa, xb;

                tx[0] = s_pex[f][0];     ty[0] = s_pey[f][0];
                tx[1] = s_pex[f][v];     ty[1] = s_pey[f][v];
                tx[2] = s_pex[f][v + 1]; ty[2] = s_pey[f][v + 1];

                if (dice_tri_span(tx, ty, y, &xa, &xb))dice_canvas_put(xa, xb, s_fcol[f]);
            }

            dice_poly_edges(s_pex[f], s_pey[f], vf, y, s_fecol[f]);
        }

        /* 3.4 点数 */
        poly_number_row(y);

        /* 3.5 整行一次上屏 */
        dice_canvas_flush(y);
    }
}


/******************************************************************************************/
/* 对外接口: 四个型号共用这几个函数, 只在 init/spawn 时按型号取表 */

uint32_t poly_bg(void)
{
    return POLY_BG_COLOR;
}

static uint32_t poly_readable(uint32_t rgb)
{
    int lum = (int)((rgb >> 16) & 0xFF) + (int)((rgb >> 8) & 0xFF) + (int)(rgb & 0xFF);

    return (uint32_t)((lum > 3 * 255 / 2) ? 0x000000 : 0xFFFFFF);
}

/**
 * @brief       这一颗此刻的值 = 屏幕上投影面积最大的那个面的点数
 */
uint8_t poly_model_value(poly_inst_t *in, const dice_t *d)
{
    float m[3][3];
    float best = -1.0f;
    int bf = 0, f;

    if (d == 0)return 0;

    if (in->num_ok == 0)return 0;

    dice_rot_m(m, d->priv[0], d->priv[1], d->priv[2]);

    for (f = 0; f < (int)in->geo_ptr->nf; f++)
    {
        float nz = m[2][0] * in->fn[f][0] + m[2][1] * in->fn[f][1] + m[2][2] * in->fn[f][2];
        float persp = DICE_CAM_DIST / (DICE_CAM_DIST - nz);
        float area  = nz * persp * persp;

        if (area > best)
        {
            best = area;
            bf = f;
        }
    }

    return in->num[bf];
}

void poly_model_spawn(poly_inst_t *in, dice_t *d)
{
    in->spawned++;

    d->contact_k = in->geo_ptr->contact_k;
    d->friction  = in->geo_ptr->friction;

    d->priv[0] = 0.35f * (float)in->spawned;
    d->priv[1] = 0.55f * (float)in->spawned;
    d->priv[2] = 0.0f;
    d->priv[3] = 0.0f;
}

void poly_model_frame(poly_inst_t *in, dice_t *d, float dt)
{
    int k;

    s_in = in;
    s_g  = in->geo_ptr;

    if (s_in->num_ok == 0)return;

    s_shadow = (uint8_t)((d->flags & DICE_FLAG_SHADOW) != 0);

    s_px_bg = s_port->rgb(POLY_BG_COLOR);
    if (s_shadow)
    {
        s_px_sho = s_port->rgb(POLY_SHADOW_OUT);
        s_px_shi = s_port->rgb(POLY_SHADOW_IN);
    }

    s_col_face = d->color;
    s_col_edge = dice_lighten(d->color);
    s_col_num  = poly_readable(d->color);

    s_ang[0] = d->priv[0];
    s_ang[1] = d->priv[1];
    s_ang[2] = d->priv[2];

    poly_layout(d);

    d->bx0 = (int16_t)s_bbox[0];
    d->by0 = (int16_t)s_bbox[1];
    d->bx1 = (int16_t)s_bbox[2];
    d->by1 = (int16_t)s_bbox[3];

    poly_render();

    d->px = (int16_t)s_cx;
    d->py = (int16_t)s_cy;

    for (k = 0; k < 3; k++)
    {
        d->priv[k] += d->spin[k] * dt;

        if (d->priv[k] > DICE_2PI)d->priv[k] -= DICE_2PI;
        else if (d->priv[k] < 0.0f)d->priv[k] += DICE_2PI;
    }
}

void poly_model_reset(poly_inst_t *in)
{
    in->spawned = 0;
}


/******************************************************************************************/
/* 型号文件(D4.c / D8.c / ...)负责把上面这些接口和一张几何表绑成自己的 dice_model_t */
