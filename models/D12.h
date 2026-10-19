/**
 ****************************************************************************************************
 * @file        D12.h
 * @brief       正十二面体骰子(D12): 几何表在 D12.c, 画法全部用 Poly.c 的引擎
 ****************************************************************************************************
 */

#ifndef __D12_H
#define __D12_H

#include "Poly.h"

#define D12_SPIN_X  POLY_SPIN_X
#define D12_SPIN_Y  POLY_SPIN_Y
#define D12_SPIN_Z  POLY_SPIN_Z

extern const dice_model_t dice_d12;

#endif
