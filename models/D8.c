/**
 ****************************************************************************************************
 * @file        D8.c
 * @brief       正八面体骰子(D8): 只有顶点表/面表/参数, 画法见 Poly.c
 * @note        6 顶点(±X/±Y/±Z), 8 个三角面, 对面点数之和 9
 ****************************************************************************************************
 */

#include "D8.h"

/* 6 个顶点(同一球面) */
static const float d8_vsrc[6][3] =
{
    { 1.0f,  0.0f,  0.0f}, {-1.0f,  0.0f,  0.0f},
    { 0.0f,  1.0f,  0.0f}, { 0.0f, -1.0f,  0.0f},
    { 0.0f,  0.0f,  1.0f}, { 0.0f,  0.0f, -1.0f}
};

/* 8 个三角面, 绕序朝外; 不足 POLY_MAX_VF 列的补首顶点 */
static const uint8_t d8_fsrc[8][POLY_MAX_VF] =
{
    {0, 2, 4, 0, 0}, {2, 1, 4, 2, 2}, {1, 3, 4, 1, 1}, {3, 0, 4, 3, 3},
    {2, 0, 5, 2, 2}, {1, 2, 5, 1, 1}, {3, 1, 5, 3, 3}, {0, 3, 5, 0, 0}
};

/* 顶点表 / 面表 / 顶点数 / 面数 / 每面顶点数 / 对面之和 / 面积门限 / 接触系数 / 摩擦 */
static const poly_geo_t s_geo =
{
    d8_vsrc, d8_fsrc, 6, 8, 3, 9, 0.19f, 0.90f, 26.0f
};

static poly_inst_t s_inst;

static void d8_init(const dice_port_t *port) { poly_model_init(&s_inst, &s_geo, port); }
static void d8_spawn(dice_t *d)              { poly_model_spawn(&s_inst, d); }
static void d8_frame(dice_t *d, float dt)    { poly_model_frame(&s_inst, d, dt); }
static void d8_reset(void)                   { poly_model_reset(&s_inst); }
static uint8_t d8_value(const dice_t *d)     { return poly_model_value(&s_inst, d); }

const dice_model_t dice_d8 =
{
    d8_init, d8_spawn, d8_frame, poly_bg, d8_reset, d8_value
};
