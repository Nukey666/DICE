/**
 * @file    dice_smoke.c
 * @brief   宿主自测: 不用单片机, PC 上跑一遍库的生命周期, 证明 Dice.c 不依赖 HAL/屏幕
 *
 * 在 DICE 目录下跑(GCC/MinGW 都行, 不需要改任何路径):
 *   sh test/run.sh         Windows: test\run.cmd
 *
 * 它测的是库的契约(初始化/错误码/边界/收回/清屏/点数范围), 不是画面好看不好看。
 */
#include <stdio.h>
#include <stdint.h>
#include "Dice.h"
#include "D20.h"
#include "D6.h"
#include "D4.h"
#include "D8.h"
#include "D10.h"
#include "D12.h"

/* port_host.c 提供 */
extern const dice_port_t *host_port(void);
extern void host_tick(uint32_t add_ms);
extern int  host_has_ink(void);

static int s_fail;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            s_fail++;                                                   \
        }                                                               \
    } while (0)

static const dice_model_t *const s_model[6] =
{
    &dice_d20, &dice_d6, &dice_d4, &dice_d8, &dice_d10, &dice_d12
};

static const int s_faces[6] = { 20, 6, 4, 8, 10, 12 };

int main(void)
{
    dice_t *d[6] = { 0 };
    int     i, f;

    /* 1. 没装端口: spawn 失败但不崩, 错误码说得清 */
    dice_init(0);
    CHECK(dice_spawn(&dice_d4, 100, 100, 50, 0xFF0000u, 0, 0, 0, DICE_FLAG_NONE) == 0);
    CHECK(dice_last_error() == DICE_ERR_PORT);

    /* 2. 装端口 */
    dice_init(host_port());
    CHECK(dice_last_error() == DICE_OK);

    /* 2b. 颜色格式: rgb() 必须按 DICE_PIXEL_MODE 转换, 这是切换像素格式的唯一接口
       用 -DDICE_PIXEL_MODE=2 编译一遍, 这条会一起验证 888 分支 */
#if DICE_PIXEL_MODE == DICE_PIXEL_888
    CHECK(host_port()->rgb(0xFF0000u) == 0xFF0000u);
    CHECK(sizeof(dice_pixel_t) == 4);
#else
    CHECK(host_port()->rgb(0xFF0000u) == 0xF800u);
    CHECK(sizeof(dice_pixel_t) == 2);
#endif

    /* 3. 参数错误各有错误码(此时一颗都还没投, 先排除 DICE_MAX 满的影响) */
    CHECK(dice_spawn(&dice_d4, 100, 100, 10, 0xFF0000u, 0, 0, 0, 0) == 0);
    CHECK(dice_last_error() == DICE_ERR_SIZE);
    CHECK(dice_spawn(&dice_d4, 999, 100, 50, 0xFF0000u, 0, 0, 0, 0) == 0);
    CHECK(dice_last_error() == DICE_ERR_POS);

    /* 4. 一颗一个型号, 都在屏内 */
    for (i = 0; i < 6; i++)
    {
        d[i] = dice_spawn(s_model[i], (uint16_t)(50 + 24 * i), (uint16_t)(90 + 18 * i),
                          46, 0x8A5CFFu, 1.7f, 2.5f, 0.9f, DICE_FLAG_SHADOW);
        CHECK(d[i] != 0);
        if (d[i] != 0)
        {
            d[i]->vx = 120.0f;
            d[i]->vy = -90.0f;
        }
    }

    /* 5. 满了再投: DICE_ERR_FULL */
    CHECK(dice_spawn(&dice_d4, 100, 100, 50, 0xFF0000u, 0, 0, 0, 0) == 0);
    CHECK(dice_last_error() == DICE_ERR_FULL);

    /* 5. 跑 900 帧(15 秒): 不能出屏、不能 NaN、点数在型号范围内 */
    for (f = 0; f < 900; f++)
    {
        dice_update(1.0f / 60.0f);
    }
    CHECK(host_has_ink());
    for (i = 0; i < 6; i++)
    {
        uint8_t v;

        if ((d[i] == 0) || (d[i]->alive == 0))continue;

        CHECK(d[i]->x == d[i]->x);                      /* NaN 自己不等于自己 */
        CHECK(d[i]->y == d[i]->y);
        CHECK((d[i]->x >= -1.0f) && (d[i]->x <= 241.0f));
        CHECK((d[i]->y >= -1.0f) && (d[i]->y <= 321.0f));

        v = dice_value(d[i]);
        CHECK((v >= 1) && (v <= (uint8_t)s_faces[i]));
    }

    /* 6. 收回一颗 -> 再投一颗 -> 清屏 */
    dice_kill(d[5]);
    CHECK(d[5]->alive == 0);
    d[5] = dice_spawn(&dice_d6, 120, 160, 40, 0xFFA23Cu, 0, 0, 0, 0);
    CHECK(d[5] != 0);
    dice_clear();
    for (i = 0; i < 6; i++)CHECK(d[i]->alive == 0);

    /* 7. 清屏后还能重新投满(型号的起始姿态计数被 reset 过, 不会越投越偏) */
    for (i = 0; i < 6; i++)
    {
        d[i] = dice_spawn(s_model[i], (uint16_t)(50 + 24 * i), (uint16_t)(90 + 18 * i),
                          40, 0x2BD9FFu, 2.0f, -1.5f, 0.8f, DICE_FLAG_NONE);
        CHECK(d[i] != 0);
    }
    for (f = 0; f < 60; f++)dice_event_update();        /* 便捷版: 用端口时钟 + 限帧 */
    CHECK(host_has_ink());

    printf("%s: %d check(s) failed\n", s_fail ? "FAIL" : "PASS", s_fail);
    return (s_fail != 0);
}
