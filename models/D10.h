/**
 ****************************************************************************************************
 * @file        D10.h
 * @brief       正五角偏方面体骰子(D10): 几何表在 D10.c, 画法全部用 Poly.c 的引擎
 ****************************************************************************************************
 */

#ifndef __D10_H
#define __D10_H

#include "Poly.h"

#define D10_SPIN_X  POLY_SPIN_X
#define D10_SPIN_Y  POLY_SPIN_Y
#define D10_SPIN_Z  POLY_SPIN_Z

extern const dice_model_t dice_d10;

#endif
