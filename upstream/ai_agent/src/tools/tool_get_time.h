/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * This file contains code derived from MimiClaw (https://github.com/memovai/mimiclaw)
 * Copyright (c) 2026 Ziboyan Wang, licensed under the MIT License.
 * See NOTICE file for the original MIT License terms.
 */

#pragma once

#include "agent_compat.h"
#include <stddef.h>

/**
 * Execute get_current_time tool.
 * Fetches current UTC time via HTTP Date response header,
 * sets the system clock, and returns a formatted local time string.
 */
int tool_get_time_execute(const char *input_json, char *output, size_t output_size);

/**
 * Fetch the time from a network source and set the system clock.
 * Blocking (one HTTPS HEAD, ~10s worst case). Safe to call from any task.
 *
 * @param out       Optional buffer for the formatted local time string.
 * @param out_size  Size of @p out. Ignored when @p out is NULL.
 * @return OK on success (clock now set and verified), ERROR otherwise.
 */
int agent_time_sync_now(char *out, size_t out_size);

/**
 * Whether the system clock has ever been set from an authoritative source.
 *
 * The board has no RTC, so time(NULL) at boot returns a seeded or placeholder
 * value. Anything that schedules a future absolute action must require this
 * to be true rather than trusting that the timestamp merely looks plausible.
 */
bool agent_clock_is_verified(void);
