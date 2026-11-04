/****************************************************************************
 * vendor/allwinnertech/apps/kid_buddy/led_strip.h
 *
 * 外接单色呼吸灯带（PG13 / H4 第 1 脚 → PWM2 → 低边 MOS 管）。
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __APP_KID_BUDDY_LED_STRIP_H
#define __APP_KID_BUDDY_LED_STRIP_H

#ifdef __cplusplus
extern "C"
{
#endif

/* 灯带的「活动」位：只要有任何一位被置上，灯带就做 4 秒一轮的呼吸；
 * 全部清掉之后回到常亮。多个线程可以各自置自己那一位，互不覆盖。 */

#define LED_STRIP_ACT_THINKING  (1u << 0)  /* 等模型回复（LLM 轮次） */
#define LED_STRIP_ACT_BOOK      (1u << 1)  /* 给家长回传故事记录 */

/* 启动灯带：把 PG13 复用到 PWM2，配置 2 kHz，先给一个常亮亮度，然后
 * 起线程按活动位在「呼吸 / 常亮」之间切换。重复调用是安全的。
 * 返回 0 成功，负值失败（失败时灯带保持熄灭，不影响主功能）。 */

int led_strip_start(void);

/* 置上 / 清掉一个活动位（LED_STRIP_ACT_*）。on 非 0 表示置位。 */

void led_strip_set_activity(unsigned int activity, int on);

/* 手动设置亮度，0~100（%）。呼吸线程运行时会在每个周期覆盖这个值，
 * 调试时可以先调用 led_strip_stop()。 */

void led_strip_set_pct(int pct);

/* 停止呼吸线程并把灯带关掉。 */

void led_strip_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_KID_BUDDY_LED_STRIP_H */
