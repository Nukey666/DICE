/**
 ****************************************************************************************************
 * @file        D8.h
 * @brief       正八面体骰子(D8): 几何表在 D8.c, 画法全部用 Poly.c 的引擎
 ****************************************************************************************************
 */

#ifndef __D8_H
#define __D8_H

#include "Poly.h"

#define D8_SPIN_X   POLY_SPIN_X
#define D8_SPIN_Y   POLY_SPIN_Y
#define D8_SPIN_Z   POLY_SPIN_Z

extern const dice_model_t dice_d8;

#endif
