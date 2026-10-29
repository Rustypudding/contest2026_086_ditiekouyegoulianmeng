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

#include "tools/tool_registry.h"
#ifdef CONFIG_AI_AGENT_MCP
#include "tools/mcp_client.h"
#include "tools/mcp_bridge.h"
#endif
#include "tools/tool_guard.h"
#include "tools/tool_control.h"
#include "tools/tool_cron.h"
#include "tools/tool_feishu_chat.h"
#include "tools/tool_feishu_doc.h"
#include "tools/tool_fetch_url.h"
#include "tools/tool_files.h"
#include "tools/tool_get_time.h"
#include "tools/tool_health.h"
#include "tools/tool_media.h"
#include "tools/tool_proxyquickapp.h"
#include "tools/tool_shell.h"
#include "tools/tool_system.h"
#include "tools/tool_vision.h"
#include "tools/tool_camera.h"
#include "tools/tool_web_search.h"
#include "agent_compat.h"
#include "agent_config.h"

#include "cJSON.h"
#include <pthread.h>
#include <string.h>

static const char* TAG = "tools";

#define MAX_TOOLS 48
#define MAX_PROVIDERS 4

/* ── External tool provider registry ───────────────────────── */

typedef struct {
    const char* name;
    tool_provider_fn get_tools;
    tool_executor_fn execute;
} tool_provider_t;

static tool_provider_t s_providers[MAX_PROVIDERS];
static int s_provider_count = 0;

static agent_tool_t s_tools[MAX_TOOLS];
static int s_tool_count;
static char* s_tools_json;
static bool s_tools_dirty = true;
static pthread_mutex_t s_tools_mtx = PTHREAD_MUTEX_INITIALIZER;

void tool_registry_register_provider(const char* name,
                                     tool_provider_fn get_tools,
                                     tool_executor_fn execute)
{
    if (s_provider_count >= MAX_PROVIDERS) {
        syslog(LOG_ERR, "[%s] Provider registry full, cannot add '%s'\n",
               TAG, name ? name : "(null)");
        return;
    }
    s_providers[s_provider_count].name = name;
    s_providers[s_provider_count].get_tools = get_tools;
    s_providers[s_provider_count].execute = execute;
    s_provider_count++;
    syslog(LOG_INFO, "[%s] Registered tool provider: %s\n", TAG, name);
}

static void register_tool(const agent_tool_t* tool)
{
    if (s_tool_count >= MAX_TOOLS) {
        syslog(LOG_ERR, "[%s] Tool registry full\n", TAG);
        return;
    }
    s_tools[s_tool_count++] = *tool;
    syslog(LOG_INFO, "[%s] Registered tool: %s\n", TAG, tool->name);
}

/* Rebuild tools JSON under lock. Only rebuilds when dirty flag is set. */
static void build_tools_json_locked(void)
{
    if (!s_tools_dirty) {
        return;
    }

    cJSON* arr = cJSON_CreateArray();

    for (int i = 0; i < s_tool_count; i++) {
        cJSON* tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", s_tools[i].name);
        cJSON_AddStringToObject(tool, "description", s_tools[i].description);
        cJSON* schema = cJSON_Parse(s_tools[i].input_schema_json);
        if (schema)
            cJSON_AddItemToObject(tool, "input_schema", schema);
        cJSON_AddItemToArray(arr, tool);
    }

#ifdef CONFIG_AI_AGENT_MCP
    char* mcp_json = mcp_bridge_get_tools_json();
    if (mcp_json) {
        cJSON* mcp_arr = cJSON_Parse(mcp_json);
        if (mcp_arr && cJSON_IsArray(mcp_arr)) {
            cJSON* item = NULL;
            cJSON_ArrayForEach(item, mcp_arr)
            {
                cJSON_AddItemToArray(arr, cJSON_Duplicate(item, 1));
            }
        }
        cJSON_Delete(mcp_arr);
        free(mcp_json);
    }
#endif

    /* Collect tools from all registered providers */
    for (int p = 0; p < s_provider_count; p++) {
        if (!s_providers[p].get_tools) {
            continue;
        }
        char* provider_json = s_providers[p].get_tools();
        if (provider_json) {
            cJSON* provider_arr = cJSON_Parse(provider_json);
            if (provider_arr && cJSON_IsArray(provider_arr)) {
                cJSON* item = NULL;
                cJSON_ArrayForEach(item, provider_arr)
                {
                    cJSON_AddItemToArray(arr, cJSON_Duplicate(item, 1));
                }
            }
            cJSON_Delete(provider_arr);
            free(provider_json);
        }
    }

    free(s_tools_json);
    s_tools_json = cJSON_PrintUnformatted(arr);
    s_tools_dirty = false;
    cJSON_Delete(arr);
    syslog(LOG_INFO, "[%s] Tools JSON built (%d builtin + %d providers)\n",
        TAG, s_tool_count, s_provider_count);
}

void tool_registry_rebuild_json(void) { build_tools_json_locked(); }

/* ── remember：板子记住孩子说过的事（决赛加） ────────────────────
 *
 * 工具表里多一条 remember。孩子在聊天里说到关于自己的事 —— 最喜欢什么、
 * 养了什么宠物、生日、怕什么、在期待什么 —— 模型调一次这个工具，
 * 那句话就存下来了；kid_buddy 每轮把最近几条注入提示词，下次提起时它接得上。
 *
 * **为什么由模型决定记什么**：写死在应用里的关键词表只能覆盖提前想到的说法，
 * 而「哪句值得记」本来就是语义判断。这也和这个作品一贯的做法一致 ——
 * 判断在模型、执行在板子。
 *
 * 存储就是一个纯文本文件，一行一条，FIFO 封顶。kid_buddy 那边读它。
 * 实现放在这里而不是新开一个文件，是为了不动 CMakeLists 的源文件列表。
 */

#define KID_LINE         96     /* 单条上限（字节），remember / remember_world 共用 */
#define KID_FACTS_PATH   AGENT_DATA_DIR "/kid_buddy_facts.txt"
#define KID_FACTS_MAX    32     /* 存放上限，超了丢最旧的 */

/* 星芽岛的世界状态（v2.85）。剧情里一旦发生不可逆的改变 —— 打开宝箱、
 * 拿走东西、救下谁、和居民立下约定 —— 模型调 remember_world 写一行。
 * kid_buddy 在故事模式下每轮读出来注入提示词，所以下一次、甚至重启之后
 * 它仍然知道“那个宝箱已经开过了”。
 *
 * 和 remember 分两个文件：孩子的事是长期档案，世界状态是剧情存档，
 * 生命周期和注入位置都不一样。 */
#define KID_WORLD_PATH   AGENT_DATA_DIR "/kid_buddy_world.txt"
#define KID_WORLD_MAX    24     /* 存放上限，超了丢最旧的 */

static int kid_lines_read(const char *path, char (*lines)[KID_LINE], int max)
{
    FILE *f = fopen(path, "r");
    int   n = 0;

    if (f == NULL) {
        return 0;
    }

    while (n < max && fgets(lines[n], KID_LINE, f) != NULL) {
        size_t l = strlen(lines[n]);

        while (l > 0 && (lines[n][l - 1] == '\n' || lines[n][l - 1] == '\r')) {
            lines[n][--l] = '\0';
        }

        if (l > 0) {
            n++;
        }
    }

    fclose(f);
    return n;
}

static int kid_lines_write(const char *path, char (*lines)[KID_LINE], int n)
{
    FILE *f = fopen(path, "w");
    int   i;

    if (f == NULL) {
        return ERROR;
    }

    for (i = 0; i < n; i++) {
        fprintf(f, "%s\n", lines[i]);
    }

    fclose(f);
    return OK;
}

/* 把模型给的一句话压成文件里的一行：换行/制表符换成空格、去尾空格、
 * 截到 KID_LINE-1 字节。截断可能正好落在 UTF-8 序列中间，这时退回到
 * 最后一个完整字符 —— 半个汉字喂给模型比少几个字难查得多。 */
static void kid_line_clean(const char *src, char *clean, size_t *out_len)
{
    size_t n = 0;
    size_t i;

    for (i = 0; src[i] != '\0' && n < KID_LINE - 1; i++) {
        char c = src[i];
        clean[n++] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }

    if (src[i] != '\0') {
        size_t cut = n;

        while (cut > 0 && ((unsigned char)clean[cut - 1] & 0xC0) == 0x80) {
            cut--;
        }

        if (cut > 0) {
            unsigned char lead = (unsigned char)clean[cut - 1];
            size_t need = 1;

            if ((lead & 0xE0) == 0xC0) {
                need = 2;
            } else if ((lead & 0xF0) == 0xE0) {
                need = 3;
            } else if ((lead & 0xF8) == 0xF0) {
                need = 4;
            }

            if (cut - 1 + need > n) {
                n = cut - 1;
            }
        }
    }

    while (n > 0 && clean[n - 1] == ' ') {
        n--;
    }

    clean[n] = '\0';
    *out_len = n;
}

int tool_remember_execute(const char *input_json, char *output,
                          size_t output_size)
{
    /* 只跑在 Agent 主循环这一个线程上，静态缓冲是安全的 ——
     * 换来的是不用在栈上放 3KB。 */
    static char s_lines[KID_FACTS_MAX][KID_LINE];

    cJSON      *root;
    const char *fact;
    char        clean[KID_LINE];
    size_t      n = 0;
    size_t      i;
    int         count;

    root = cJSON_Parse(input_json);

    if (root == NULL) {
        snprintf(output, output_size, "{\"error\":\"bad json\"}");
        return ERROR;
    }

    fact = cJSON_GetStringValue(cJSON_GetObjectItem(root, "fact"));
    cJSON_Delete(root);

    if (fact == NULL || fact[0] == '\0') {
        snprintf(output, output_size, "{\"error\":\"fact is empty\"}");
        return ERROR;
    }

    /* 一条事实必须压成一行：换行会把文件格式撑破 */
    kid_line_clean(fact, clean, &n);

    if (n == 0) {
        snprintf(output, output_size, "{\"error\":\"fact is empty\"}");
        return ERROR;
    }

    count = kid_lines_read(KID_FACTS_PATH, s_lines, KID_FACTS_MAX);

    /* 记过同一条就不重复 */
    for (i = 0; i < (size_t)count; i++) {
        if (strcmp(s_lines[i], clean) == 0) {
            snprintf(output, output_size,
                     "{\"ok\":true,\"note\":\"already known\",\"total\":%d}",
                     count);
            return OK;
        }
    }

    /* 满了丢最旧的一条 */
    if (count >= KID_FACTS_MAX) {
        memmove(s_lines[0], s_lines[1],
                (size_t)(KID_FACTS_MAX - 1) * KID_LINE);
        count = KID_FACTS_MAX - 1;
    }

    strncpy(s_lines[count], clean, KID_LINE - 1);
    s_lines[count][KID_LINE - 1] = '\0';
    count++;

    if (kid_lines_write(KID_FACTS_PATH, s_lines, count) != OK) {
        snprintf(output, output_size, "{\"error\":\"write failed\"}");
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] remember: %s（共 %d 条）\n", "tool", clean, count);

    snprintf(output, output_size, "{\"ok\":true,\"total\":%d}", count);
    return OK;
}

/* ── remember_world：记住故事世界里已经发生的改变（v2.85） ────────
 *
 * 和 remember 分开：孩子自己的喜好是长期档案，剧情里的“宝箱已经开过”
 * 是另一条时间线。写进 kid_buddy_world.txt，kid_buddy 只在故事模式下
 * 注入，不污染普通聊天。
 */
int tool_remember_world_execute(const char *input_json, char *output,
                                size_t output_size)
{
    static char s_lines[KID_WORLD_MAX][KID_LINE];

    cJSON      *root;
    const char *change;
    char        clean[KID_LINE];
    size_t      n = 0;
    size_t      i;
    int         count;

    root = cJSON_Parse(input_json);

    if (root == NULL) {
        snprintf(output, output_size, "{\"error\":\"bad json\"}");
        return ERROR;
    }

    change = cJSON_GetStringValue(cJSON_GetObjectItem(root, "change"));
    cJSON_Delete(root);

    if (change == NULL || change[0] == '\0') {
        snprintf(output, output_size, "{\"error\":\"change is empty\"}");
        return ERROR;
    }

    kid_line_clean(change, clean, &n);

    if (n == 0) {
        snprintf(output, output_size, "{\"error\":\"change is empty\"}");
        return ERROR;
    }

    count = kid_lines_read(KID_WORLD_PATH, s_lines, KID_WORLD_MAX);

    for (i = 0; i < (size_t)count; i++) {
        if (strcmp(s_lines[i], clean) == 0) {
            snprintf(output, output_size,
                     "{\"ok\":true,\"note\":\"already recorded\",\"total\":%d}",
                     count);
            return OK;
        }
    }

    if (count >= KID_WORLD_MAX) {
        memmove(s_lines[0], s_lines[1],
                (size_t)(KID_WORLD_MAX - 1) * KID_LINE);
        count = KID_WORLD_MAX - 1;
    }

    strncpy(s_lines[count], clean, KID_LINE - 1);
    s_lines[count][KID_LINE - 1] = '\0';
    count++;

    if (kid_lines_write(KID_WORLD_PATH, s_lines, count) != OK) {
        snprintf(output, output_size, "{\"error\":\"write failed\"}");
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] remember_world: %s（共 %d 条）\n",
           "tool", clean, count);

    snprintf(output, output_size, "{\"ok\":true,\"total\":%d}", count);
    return OK;
}

int tool_registry_init(void)
{

    s_tool_count = 0;

    /* 记住关于孩子的事（决赛加）。写进 kid_buddy_facts.txt，
     * kid_buddy 每轮读出来注入提示词。 */
    REGISTER_TOOL(
        "remember",
        "记住一件关于这个小朋友的事。当他说到自己的喜好、家人、宠物、生日、"
        "害怕的东西、正在期待的事，就调用一次。不要记无关的闲聊，"
        "也不要记你自己说过的话。一条一句话，用第三人称写。",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("fact", "要记住的那件事，一句话")
            TOOL_SCHEMA_END_REQUIRED("\"fact\""),
        tool_remember_execute);

    /* 记住故事世界里已经发生、以后不能反悔的改变（v2.85）。
     * 写进 kid_buddy_world.txt，只在故事模式下注入提示词。 */
    REGISTER_TOOL(
        "remember_world",
        "记录故事世界里已经发生的改变。打开了宝箱、拿走了某件东西、救下了谁、"
        "和居民立下了约定、某个地点被改变了——这种以后不能反悔、会影响后续剧情的事，"
        "发生时就调用一次。只记事实，不记对话过程；一件事一条，用第三人称，"
        "不超过 30 个字。孩子自己的喜好不要记在这里，用 remember。",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("change", "发生了什么改变，一句话")
            TOOL_SCHEMA_END_REQUIRED("\"change\""),
        tool_remember_world_execute);


#ifdef CONFIG_AI_AGENT_MCP
    if (mcp_bridge_init() != OK) {
        syslog(LOG_WARNING, "[%s] MCP bridge init failed; MCP tools disabled\n",
            TAG);
    }

    if (mcp_client_init() != OK) {
        syslog(LOG_WARNING, "[%s] MCP client init failed; remote tools disabled\n",
            TAG);
    }
#endif

    tool_web_search_init();

    /* Web / info tools */
    REGISTER_TOOL(
        "web_search",
        "Search the web for current info, news, or real-time data.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("query", "The search query")
            TOOL_SCHEMA_END_REQUIRED("\"query\""),
        tool_web_search_execute);

    REGISTER_TOOL(
        "news_search",
        "Search recent news. Set top_headlines=true for major stories.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("query", "News search query") "," TOOL_PARAM_BOOL(
                "top_headlines",
                "If true, fetch top headlines instead of everything")
                TOOL_SCHEMA_END_REQUIRED("\"query\""),
        tool_news_search_execute);

    REGISTER_TOOL(
        "get_weather",
        "Get current weather for a location.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR(
            "location", "City name or location, e.g. 'Shanghai' or 'New York'")
            TOOL_SCHEMA_END_REQUIRED("\"location\""),
        tool_get_weather_execute);

    REGISTER_TOOL_NO_PARAMS(
        "get_current_time",
        "Get current date, time, and UNIX epoch.",
        tool_get_time_execute);

    /* File tools */
    REGISTER_TOOL("read_file",
        "Read a file. Path must start with " AGENT_DATA_DIR "/.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("path", "Absolute file path")
            TOOL_SCHEMA_END_REQUIRED("\"path\""),
        tool_read_file_execute);

    REGISTER_TOOL(
        "write_file",
        "Write a file. Path must start with " AGENT_DATA_DIR "/.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("path", "Absolute file path") "," TOOL_PARAM_STR(
                "content", "File content")
                TOOL_SCHEMA_END_REQUIRED("\"path\",\"content\""),
        tool_write_file_execute);

    REGISTER_TOOL(
        "edit_file", "Find and replace text in a file.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("path", "Absolute file path") "," TOOL_PARAM_STR(
                "old_string",
                "Text to find") "," TOOL_PARAM_STR("new_string",
                "Replacement text")
                TOOL_SCHEMA_END_REQUIRED(
                    "\"path\",\"old_string\",\"new_string\""),
        tool_edit_file_execute);

    REGISTER_TOOL(
        "list_dir",
        "List files in data directory.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("prefix", "Optional path prefix")
            TOOL_SCHEMA_END(),
        tool_list_dir_execute);

    /* Cron tools */
    REGISTER_TOOL(
        "cron_add",
        "Schedule a reminder or task. "
        "Steps: get_current_time→compute at_epoch→call this. "
        "at_epoch must be plain integer. "
        "Recurring: schedule_type='every', interval_s=seconds. "
        "For a plain reminder, leave action and action_args EMPTY — "
        "the job just sends the message at trigger time.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("name", "Job name") "," TOOL_PARAM_ENUM("schedule_type", "Schedule type", "\"every\",\"at\"") "," TOOL_PARAM_NUM("interval_s", "Interval in seconds (for every)") "," TOOL_PARAM_NUM("at_epoch",
                "Target UNIX timestamp as a plain integer") "," TOOL_PARAM_STR("message",
                "Notification message sent to user when job fires") "," TOOL_PARAM_STR("channel", "Reply channel (default system)") "," TOOL_PARAM_STR("chat_id", "Reply chat_id") "," TOOL_PARAM_STR("action",
                "Tool name to execute when job fires (e.g. music_play)") "," TOOL_PARAM_STR("action_args",
                "JSON arguments for the action tool")
                TOOL_SCHEMA_END_REQUIRED("\"name\",\"schedule_type\",\"message\""),
        tool_cron_add_execute);

    REGISTER_TOOL_NO_PARAMS("cron_list", "List all scheduled cron jobs.",
        tool_cron_list_execute);

    REGISTER_TOOL("cron_remove", "Remove a scheduled cron job by its ID.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("job_id", "8-char hex job ID")
                TOOL_SCHEMA_END_REQUIRED("\"job_id\""),
        tool_cron_remove_execute);

    /* Fetch URL tool */
    REGISTER_TOOL(
        "fetch_url",
        "Fetch content from an HTTPS URL.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("url", "The HTTPS URL to fetch")
            TOOL_SCHEMA_END_REQUIRED("\"url\""),
        tool_fetch_url_execute);

    /* Vision tool */
    REGISTER_TOOL(
        "analyze_image",
        "Analyze a screen screenshot or image file with vision LLM. "
        "Without image_path: auto-captures screen. With image_path: reads file.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("image_path", "Optional: path to an existing image "
                                         "file. Omit to auto-capture screen.") "," TOOL_PARAM_STR("prompt", "Question or instruction about the image")
                TOOL_SCHEMA_END(),
        tool_analyze_image_execute);

    /* Camera capture tool */
#ifdef CONFIG_AI_AGENT_CAMERA
    REGISTER_TOOL(
        "camera_capture",
        "Take a photo with device camera and analyze it. "
        "For screen screenshots, use analyze_image instead.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("prompt",
                "Optional question about what the camera sees") ","
            TOOL_PARAM_ENUM("resolution",
                "Image resolution: low (320x180, faster) or high (1280x720)",
                "\"low\",\"high\"")
            TOOL_SCHEMA_END(),
        tool_camera_capture_execute);
#endif

    /* Shell tool */
#if AGENT_SHELL_SECURITY == AGENT_SHELL_SECURITY_FULL
    REGISTER_TOOL(
        "run_shell",
        "Execute a NuttX NSH command. FULL mode: pipes/redirects allowed. "
        "NuttX, not Linux — no Linux flags (use 'free' not 'free -h'). "
        "lvctl: 'lvctl snapshot take 1', 'lvctl snapshot save /data/screen.png'.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("command", "The NSH command to execute, e.g. 'free' "
                                      "or 'curl wttr.in/Beijing | head -7'")
                TOOL_SCHEMA_END_REQUIRED("\"command\""),
        tool_run_shell_execute);
#elif AGENT_SHELL_SECURITY == AGENT_SHELL_SECURITY_DENY
    /* Shell tool disabled in DENY mode */
#else
    REGISTER_TOOL(
        "run_shell",
        "Execute a NuttX NSH command. ALLOWLIST mode: only safe commands. "
        "NuttX, not Linux — no Linux flags. No pipes/redirects. "
        "Allowed: free, ps, df, cat, ls, curl, lvctl, nxplayer, etc.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR(
            "command", "The NSH command to execute, e.g. 'free' or 'echo hello'")
            TOOL_SCHEMA_END_REQUIRED("\"command\""),
        tool_run_shell_execute);
#endif

    /* System state tools */
    REGISTER_TOOL_NO_PARAMS(
        "get_battery",
        "Get battery level, charging status, voltage, temperature.",
        tool_get_battery_execute);

    REGISTER_TOOL_NO_PARAMS(
        "get_wear_state",
        "Check if device is being worn.",
        tool_get_wear_state_execute);

    REGISTER_TOOL_NO_PARAMS(
        "get_screen_state",
        "Check if screen is on or off.",
        tool_get_screen_state_execute);

    /* Health / fitness tools */
    REGISTER_TOOL_NO_PARAMS(
        "get_heartrate",
        "Get latest heart rate in BPM.",
        tool_get_heartrate_execute);

    REGISTER_TOOL_NO_PARAMS("get_steps",
        "Get step count and step frequency.",
        tool_get_steps_execute);

    /* Device control tools */
    REGISTER_TOOL(
        "vibrate",
        "Trigger vibration. duration_ms: 1-5000 (default 200), amplitude: 1-255.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_NUM(
            "duration_ms",
            "Duration in ms (1-5000)") "," TOOL_PARAM_NUM("amplitude",
            "Strength 1-255")
            TOOL_SCHEMA_END(),
        tool_vibrate_execute);

    /* Feishu document tools */
    REGISTER_TOOL(
        "feishu_doc_create",
        "Create a new Feishu cloud document. Returns document_id and URL.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("title", "Document title") "," TOOL_PARAM_STR(
                "folder_token", "Target folder token (optional, has default)")
                TOOL_SCHEMA_END_REQUIRED("\"title\""),
        tool_feishu_doc_create_execute);

    REGISTER_TOOL(
        "feishu_doc_write",
        "Write text into a Feishu document. Each line becomes a paragraph.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR(
            "document_id",
            "Feishu document ID") "," TOOL_PARAM_STR("content",
            "Text content (newlines "
            "create paragraphs)")
            TOOL_SCHEMA_END_REQUIRED("\"document_id\",\"content\""),
        tool_feishu_doc_write_execute);

    REGISTER_TOOL(
        "feishu_doc_read",
        "Read plain-text content of a Feishu document.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("document_id", "Feishu document ID")
            TOOL_SCHEMA_END_REQUIRED("\"document_id\""),
        tool_feishu_doc_read_execute);

    REGISTER_TOOL("feishu_doc_list",
        "List recent documents in a Feishu folder.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("folder_token", "Folder token (optional)")
                TOOL_SCHEMA_END(),
        tool_feishu_doc_list_execute);

    /* Feishu group chat tools */
    REGISTER_TOOL(
        "feishu_chat_members",
        "List members of a Feishu group chat with name and open_id.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("chat_id", "Feishu group chat ID (oc_xxx)")
                TOOL_SCHEMA_END_REQUIRED("\"chat_id\""),
        tool_feishu_chat_members_execute);

    REGISTER_TOOL(
        "feishu_send_mention",
        "Send @mention message in a Feishu group chat. "
        "Call ONCE per request. Use chat_id from current session.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("chat_id", "Feishu group chat ID "
                                                      "(oc_xxx)") "," TOOL_PARAM_STR("open_id",
            "Target user's open_id (ou_xxx)") "," TOOL_PARAM_STR("name",
            "Display name "
            "for the "
            "@mention") "," TOOL_PARAM_STR("text",
            "Message text after the @mention")
            TOOL_SCHEMA_END_REQUIRED("\"chat_id\",\"open_id\",\"text\""),
        tool_feishu_send_mention_execute);

    /* Music search */
    REGISTER_TOOL("music_search",
        "Search songs by keyword. Returns name, artist, URL. "
        "Does NOT play — call music_play with URL after.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("keyword", "Song name or artist to search")
            TOOL_SCHEMA_END_REQUIRED("\"keyword\""),
        tool_music_search_execute);

    /* Music playback tools (URL mode) */
    REGISTER_TOOL("music_play",
        "Play music from URL or local file. Stops current track first.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("url", "Music URL or local file path") "," TOOL_PARAM_BOOL("autostart", "Start immediately (default true)") "," TOOL_PARAM_NUM("start_position_ms", "Seek position before start (default 0)") "," TOOL_PARAM_STR(
                "options", "Media format options for raw files, e.g. "
                           "format=s16le:sample_rate=16000:ch_layout="
                           "mono") "," TOOL_PARAM_STR("format",
                "PCM sample "
                "format: s16le, "
                "s32le, f32le, "
                "etc. (default "
                "s16le)") "," TOOL_PARAM_NUM("sample_rate",
                "Sample rate in Hz, e.g. 16000, 44100, 48000") "," TOOL_PARAM_NUM("channels",
                "Number of audio channels, 1=mono, 2=stereo")
                TOOL_SCHEMA_END_REQUIRED("\"url\""),
        tool_music_play_execute);

    REGISTER_TOOL_NO_PARAMS("music_pause",
        "Pause the currently playing music track.",
        tool_music_pause_execute);

    REGISTER_TOOL_NO_PARAMS(
        "music_resume", "Resume a paused music track, or start a prepared track.",
        tool_music_resume_execute);

    REGISTER_TOOL_NO_PARAMS("music_stop",
        "Stop music playback and release all resources.",
        tool_music_stop_execute);

    REGISTER_TOOL(
        "music_seek", "Seek to a specific position in the current music track.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_NUM("position_ms", "Target position in milliseconds")
                TOOL_SCHEMA_END_REQUIRED("\"position_ms\""),
        tool_music_seek_execute);

    REGISTER_TOOL("music_set_volume",
        "Set the music stream volume. Range 0 (mute) to 100 (max).",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_NUM("volume", "Volume level 0-100")
                TOOL_SCHEMA_END_REQUIRED("\"volume\""),
        tool_music_set_volume_execute);

    REGISTER_TOOL_NO_PARAMS("music_status",
        "Get current music playback status including state, "
        "position, duration, volume, and track URL.",
        tool_music_status_execute);

    /* QuickApp launch tool */
    REGISTER_TOOL("launch_quickapp",
        "Launch a QuickApp by package name.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("package_name", "The QuickApp package name to launch")
                TOOL_SCHEMA_END_REQUIRED("\"package_name\""),
        tool_launch_quickapp_execute);

    /* QuickApp exit tool */
    REGISTER_TOOL_NO_PARAMS("exit_quickapp",
        "Exit current QuickApp, return to home.",
        tool_exit_quickapp_execute);

    build_tools_json_locked();
    syslog(LOG_INFO, "[%s] Tool registry initialized\n", TAG);
    return OK;
}

char* tool_registry_get_tools_json(void)
{
    char* copy;

    pthread_mutex_lock(&s_tools_mtx);
    build_tools_json_locked();
    copy = s_tools_json ? strdup(s_tools_json) : NULL;
    pthread_mutex_unlock(&s_tools_mtx);
    return copy;
}

void tool_registry_invalidate(void)
{
    pthread_mutex_lock(&s_tools_mtx);
    s_tools_dirty = true;
    pthread_mutex_unlock(&s_tools_mtx);
}

void tool_registry_cleanup(void)
{
    pthread_mutex_lock(&s_tools_mtx);
    free(s_tools_json);
    s_tools_json = NULL;
    s_tool_count = 0;
    pthread_mutex_unlock(&s_tools_mtx);
}

int tool_registry_execute(const char *name, const char *input_json,
    char *output, size_t output_size)
{
    /* Security guard check before any tool execution */
    size_t input_len = input_json ? strlen(input_json) : 0;
    tool_guard_result_t guard = tool_guard_check(name, input_json, input_len);

    if (guard != GUARD_ALLOW) {
        const char *reason = "unknown";
        switch (guard) {
        case GUARD_DENY_DISABLED:
            reason = "tool disabled by config";
            break;
        case GUARD_DENY_RATE_LIMIT:
            reason = "rate limit exceeded";
            break;
        case GUARD_DENY_INPUT_SIZE:
            reason = "input too large";
            break;
        case GUARD_DENY_INPUT_INVALID:
            reason = "invalid input";
            break;
        default:
            break;
        }
        syslog(LOG_WARNING, "[%s] Tool '%s' blocked: %s\n",
               TAG, name ? name : "(null)", reason);
        snprintf(output, output_size,
                 "Error: tool '%s' blocked — %s",
                 name ? name : "(null)", reason);
        return ERROR;
    }

    /* Try builtin tools first */
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            syslog(LOG_INFO, "[%s] Executing tool: %s\n", TAG, name);
            int ret = s_tools[i].execute(input_json, output, output_size);
            if (ret == OK) {
                tool_guard_record_call(name);
            }
            return ret;
        }
    }

    /* Try MCP bridge */
#ifdef CONFIG_AI_AGENT_MCP
    if (mcp_bridge_execute(name, input_json, output, output_size) == OK) {
        syslog(LOG_INFO, "[%s] Executed MCP tool: %s\n", TAG, name);
        tool_guard_record_call(name);
        return OK;
    }
#endif

    /* Try registered providers */
    for (int p = 0; p < s_provider_count; p++) {
        if (!s_providers[p].execute) {
            continue;
        }
        int ret = s_providers[p].execute(name, input_json, output, output_size);
        if (ret == OK) {
            syslog(LOG_INFO, "[%s] Executed %s tool: %s\n",
                   TAG, s_providers[p].name, name);
            tool_guard_record_call(name);
            return OK;
        }
    }

    syslog(LOG_WARNING, "[%s] Unknown tool: %s\n", TAG, name);
    snprintf(output, output_size, "Error: unknown tool '%s'", name);
    return ERROR;
}
