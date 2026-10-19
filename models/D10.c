/**
 ****************************************************************************************************
 * @file        D10.c
 * @brief       正五角偏方面体骰子(D10): 只有顶点表/面表/参数, 画法见 Poly.c
 * @note        12 顶点(上下两个锥顶 + 10 个腰), 10 个风筝面, 对面点数之和 11。
 *              腰的 z 只有 ±0.357, 所以轮廓是"两个浅锥", 和真骰子一样宽高比 ~1:1
 ****************************************************************************************************
 */

#include "D10.h"

/* 12 个顶点(同一球面): 0/1 上下锥顶, 2~6 上腰圈(0°/72°/...), 7~11 下腰圈(+36°) */
static const float d10_vsrc[12][3] =
{
    { 0.00000f,  0.00000f,  1.00000f}, { 0.00000f,  0.00000f, -1.00000f},
    { 0.99441f,  0.00000f,  0.10557f}, { 0.30729f,  0.94574f,  0.10557f},
    {-0.80450f,  0.58450f,  0.10557f}, {-0.80450f, -0.58450f,  0.10557f},
    { 0.30729f, -0.94574f,  0.10557f},
    { 0.80450f,  0.58450f, -0.10557f}, {-0.30729f,  0.94574f, -0.10557f},
    {-0.99441f,  0.00000f, -0.10557f}, {-0.30729f, -0.94574f, -0.10557f},
    { 0.80450f, -0.58450f, -0.10557f}
};

/* 10 个风筝面(4 顶点), 5 个朝上锥顶 + 5 个朝下锥顶; 不足 POLY_MAX_VF 列的补首顶点 */
static const uint8_t d10_fsrc[10][POLY_MAX_VF] =
{
    { 0,  2,  7,  3,  0}, { 0,  3,  8,  4,  0}, { 0,  4,  9,  5,  0},
    { 0,  5, 10,  6,  0}, { 0,  6, 11,  2,  0},
    { 1,  7,  3,  8,  1}, { 1,  8,  4,  9,  1}, { 1,  9,  5, 10,  1},
    { 1, 10,  6, 11,  1}, { 1, 11,  2,  7,  1}
};

/* 顶点表 / 面表 / 顶点数 / 面数 / 每面顶点数 / 对面之和 / 面积门限 / 接触系数 / 摩擦 */
static const poly_geo_t s_geo =
{
    d10_vsrc, d10_fsrc, 12, 10, 4, 11, 1.20f, 0.85f, 24.0f
};

static poly_inst_t s_inst;

static void d10_init(const dice_port_t *port) { poly_model_init(&s_inst, &s_geo, port); }
static void d10_spawn(dice_t *d)              { poly_model_spawn(&s_inst, d); }
static void d10_frame(dice_t *d, float dt)    { poly_model_frame(&s_inst, d, dt); }
static void d10_reset(void)                   { poly_model_reset(&s_inst); }
static uint8_t d10_value(const dice_t *d)     { return poly_model_value(&s_inst, d); }

const dice_model_t dice_d10 =
{
    d10_init, d10_spawn, d10_frame, poly_bg, d10_reset, d10_value
};
