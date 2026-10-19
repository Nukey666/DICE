/**
 ****************************************************************************************************
 * @file        Poly.h
 * @brief       多面体骰子的通用画法引擎: 旋转/投影/背面剔除/着光/逐行合成/描棱/点阵数字
 * @note        引擎不认识任何具体形状。每个型号自己一个文件(D4.c / D8.c / ...), 里面只放
 *              顶点表、面表和一张 poly_geo_t, 再加一个 static poly_inst_t 和六个转发函数,
 *              画法全部调这里的 poly_model_*()。加新型号不用改本文件。
 * @attention   顶点表必须**所有顶点在同一个球面上**(初始化时按同一个比例归一化), 面法线由面顶点
 *              叉积算出, 投影峰值 POLY_PROJ_PEAK 也按这个前提定。
 ****************************************************************************************************
 */

#ifndef __POLY_H
#define __POLY_H

#include "Dice.h"

#define POLY_MAX_V   20                 /* 顶点数上限(正十二面体 20 个) */
#define POLY_MAX_F   12                 /* 面数上限 */
#define POLY_MAX_VF  5                  /* 每面最多几个顶点(五边形) */

/* 面板/阴影色: 整屏底色全工程只有一个, 必须和 D20_BG_COLOR、D6_BG_COLOR 一致 */
#define POLY_BG_COLOR       0x0A0F1A
#define POLY_SHADOW_OUT     0x080818
#define POLY_SHADOW_IN      0x000810

/* 顶点都在同一球面上时, 透视投影半径峰值最大 1.0338(球廓半径 4/(4-z)·sqrt(1-z²) 在 z=0.25 处),
   取 1.05 对所有正多面体都够用; 它同时是"擦除半径"的基准, 别按轮廓最小值缩小 */
#define POLY_PROJ_PEAK      1.05f

/* 默认三轴角速度(弧度/秒), 和 D20/D6 同一档; 应用乘倍率、传负数反向 */
#define POLY_SPIN_X         1.6875f
#define POLY_SPIN_Y         2.5000f
#define POLY_SPIN_Z         0.9375f

/**
 * @brief 一种多面体骰子的几何描述(型号文件里填一份)
 * @note  画法不读这个结构以外的任何东西, 所以同屏可以混投不同面数
 */
typedef struct
{
    const float   (*vsrc)[3];           /* 顶点表(同一球面, 模长任意) */
    const uint8_t (*fsrc)[POLY_MAX_VF]; /* 面表: 每面 vf 个顶点, 绕序朝外, 多出的列补首顶点 */
    uint8_t nv;                         /* 顶点数 */
    uint8_t nf;                         /* 面数 */
    uint8_t vf;                         /* 每面顶点数(D4/D8 = 3, D10 = 4, D12 = 5) */
    uint8_t opp_sum;                    /* 对面点数之和; 0 = 这个形状没有对面(D4), 按顺序 1..nf 编号 */
    float   num_area_k;                 /* 面积门限系数, 大面写点数用的门限 = scale² × 它 */
    float   contact_k;                  /* 互撞判定半径系数(见 新骰子开发指南.md 第 6 节) */
    float   friction;                   /* 地面减速度 px/s² */
} poly_geo_t;

/**
 * @brief 一种型号的持久状态(归一化顶点/面法线/点数)
 * @note  每个型号文件 static 一份, 不能同屏共用; 每帧的临时量在 Poly.c 里共用
 *        (前提: model->frame() 一次只画一颗)
 */
typedef struct
{
    const poly_geo_t *geo_ptr;          /* 这一种型号的几何表 */
    float    vtx[POLY_MAX_V][3];        /* 归一化到半径 1 的顶点 */
    float    fn[POLY_MAX_F][3];         /* 面单位外法线 */
    float    fd[POLY_MAX_F];            /* 面平面到中心的距离(n·p = fd) */
    uint8_t  num[POLY_MAX_F];           /* 各面点数 */
    uint8_t  num_ok;                    /* 编号自检通过才画(几何表打错宁可不显示) */
    uint8_t  spawned;                   /* 已投放几颗, 用来错开起始姿态 */
} poly_inst_t;

/* 引擎接口: 都带型号自己的状态, 所以引擎不需要认识型号名 */
void     poly_model_init(poly_inst_t *in, const poly_geo_t *g, const dice_port_t *port);
void     poly_model_spawn(poly_inst_t *in, dice_t *d);
void     poly_model_frame(poly_inst_t *in, dice_t *d, float dt);
uint8_t  poly_model_value(poly_inst_t *in, const dice_t *d);
void     poly_model_reset(poly_inst_t *in);
uint32_t poly_bg(void);                 /* 面板底色(库擦除/清屏用) */

#endif
