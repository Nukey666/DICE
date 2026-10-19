/**
 ****************************************************************************************************
 * @file        D4.h
 * @brief       正四面体骰子(D4): 几何表在 D4.c, 画法全部用 Poly.c 的引擎
 ****************************************************************************************************
 */

#ifndef __D4_H
#define __D4_H

#include "Poly.h"

/* 三轴角速度默认值(弧度/秒): 应用乘倍率、传负数反向 */
#define D4_SPIN_X   POLY_SPIN_X
#define D4_SPIN_Y   POLY_SPIN_Y
#define D4_SPIN_Z   POLY_SPIN_Z

extern const dice_model_t dice_d4;

#endif
