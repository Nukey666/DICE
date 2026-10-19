/**
 ****************************************************************************************************
 * @file        D4.c
 * @brief       正四面体骰子(D4): 只有顶点表/面表/参数, 画法见 Poly.c
 * @note        四面体不中心对称, 没有"对面"(opp_sum = 0), 点数按面表顺序 1~4
 ****************************************************************************************************
 */

#include "D4.h"

/* 4 个顶点(同一球面, 模长初始化时统一归一化) */
static const float d4_vsrc[4][3] =
{
    { 0.57735f,  0.57735f,  0.57735f}, { 0.57735f, -0.57735f, -0.57735f},
    {-0.57735f,  0.57735f, -0.57735f}, {-0.57735f, -0.57735f,  0.57735f}
};

/* 4 个三角面, 绕序朝外; 不足 POLY_MAX_VF 列的补首顶点 */
static const uint8_t d4_fsrc[4][POLY_MAX_VF] =
{
    {2, 1, 3, 2, 2}, {3, 0, 2, 3, 3}, {1, 0, 3, 1, 1}, {2, 0, 1, 2, 2}
};

/* 顶点表 / 面表 / 顶点数 / 面数 / 每面顶点数 / 对面之和 / 面积门限 / 接触系数 / 摩擦 */
static const poly_geo_t s_geo =
{
    d4_vsrc, d4_fsrc, 4, 4, 3, 0, 0.19f, 0.95f, 40.0f
};

static poly_inst_t s_inst;

static void d4_init(const dice_port_t *port) { poly_model_init(&s_inst, &s_geo, port); }
static void d4_spawn(dice_t *d)              { poly_model_spawn(&s_inst, d); }
static void d4_frame(dice_t *d, float dt)    { poly_model_frame(&s_inst, d, dt); }
static void d4_reset(void)                   { poly_model_reset(&s_inst); }
static uint8_t d4_value(const dice_t *d)     { return poly_model_value(&s_inst, d); }

const dice_model_t dice_d4 =
{
    d4_init, d4_spawn, d4_frame, poly_bg, d4_reset, d4_value
};
