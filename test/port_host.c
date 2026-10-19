/**
 * @file    port_host.c
 * @brief   移植模板: 一个不依赖任何屏幕的 dice_port_t 实现(内存像素缓冲)
 *
 * 换真屏就照这个结构写 6 个成员: width / height / fill / flush_row / tick_ms / rgb。
 * 真屏的 flush_row 建议"首行开一次窗, 后续连续行直写"(省掉每行设光标), 见 README 第 3 节。
 * 这个文件只在宿主自测里用, 不参与单片机工程编译。
 */
#include <stdint.h>
#include "Dice.h"

#define HOST_W 240                                  /* 想测横屏就改成 320x240 */
#define HOST_H 320

static dice_pixel_t s_fb[HOST_H][HOST_W];           /* 真屏没有这块内存, 这里只为了宿主端能看到像素 */
static uint32_t     s_ms;
static uint32_t     s_painted;                      /* 非底色像素计数, 给自测断言用 */

/* 矩形填充: 库所有画点/清屏都走这里, 坐标是闭区间且已由库保证在屏内 */
static void host_fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, dice_pixel_t color)
{
    uint16_t x, y;

    for (y = y0; y <= y1; y++)
    {
        for (x = x0; x <= x1; x++)
        {
            s_fb[y][x] = color;
        }
    }
}

/* 整行上屏: 库逐行合成后一次交过来, 真屏在这里发一次 DMA/连续写 GRAM */
static void host_flush_row(uint16_t x, uint16_t y, uint16_t len, const dice_pixel_t *pixels)
{
    uint16_t i;

    for (i = 0; i < len; i++)
    {
        s_fb[y][x + i] = pixels[i];
    }
    s_painted++;
}

static uint32_t host_tick_ms(void)
{
    return s_ms;
}

/* RGB888 -> 屏像素: 565 模式打包, 888 模式原样(跟着 Dice.h 的 DICE_PIXEL_MODE 走) */
static dice_pixel_t host_rgb(uint32_t rgb888)
{
#if DICE_PIXEL_MODE == DICE_PIXEL_888
    return (dice_pixel_t)(rgb888 & 0xFFFFFFu);      /* 888: 原样, 24 位放进 32 位容器 */
#else
    return (dice_pixel_t)(((rgb888 >> 19) & 0x1Fu) << 11 |
                          ((rgb888 >> 10) & 0x3Fu) << 5 |
                          ((rgb888 >> 3) & 0x1Fu));
#endif
}

static const dice_port_t s_port =
{
    HOST_W, HOST_H, host_fill, host_flush_row, host_tick_ms, host_rgb
};

const dice_port_t *host_port(void)
{
    return &s_port;
}

void host_tick(uint32_t add_ms)
{
    s_ms += add_ms;
}

uint32_t host_flush_count(void)
{
    return s_painted;
}

/* 屏上有没有出现过非 0 像素(0 = 初始缓冲, 任何颜色打包后都不是 0) */
int host_has_ink(void)
{
    uint16_t x, y;

    for (y = 0; y < HOST_H; y++)
    {
        for (x = 0; x < HOST_W; x++)
        {
            if (s_fb[y][x] != 0)
            {
                return 1;
            }
        }
    }
    return 0;
}
