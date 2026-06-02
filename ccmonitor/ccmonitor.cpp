#include "ccmonitor.h"
#include "board_def.h"
#include "app/app_key.h"
#include <ESPAsyncWebServer.h>

// board_def.cpp 里定义的全局 web 服务器（开机常驻，端口 80）
extern AsyncWebServer server;

// ---------------- 状态定义 ----------------
// transient = true 的状态会在 hold_ms 后自动回到时钟（idle）。
// 其余“工作中”状态会保持，直到下一次推送；并由 STALE_MS 兜底防止卡死。
enum
{
    ST_IDLE = 0, // 空闲 -> 显示时钟
    ST_OFF,      // 熄屏
    ST_THINKING, // 思考中
    ST_AI,       // 生成中
    ST_BUSY,     // 执行命令中
    ST_SUCCESS,  // 成功
    ST_ERROR,    // 失败
    ST_ALARM,    // 需要你确认 / 阻塞
    ST_DEMO,     // 开机/演示
    ST_COUNT
};

struct cc_def
{
    const char *name; // 同时也是 /cc/<name>/ 图片文件夹名
    bool transient;   // 是否自动回到时钟
    uint32_t hold_ms; // transient 状态保持时长
};

static const cc_def CC[ST_COUNT] = {
    {"idle", false, 0},
    {"off", false, 0},
    {"thinking", false, 0},
    {"ai", false, 0},
    {"busy", false, 0},
    {"success", true, 4000},
    {"error", true, 6000},
    {"alarm", false, 0},
    {"demo", true, 5000},
};

// 工作中状态若超过此时间没有新推送，自动回到时钟（防止漏掉 Stop 钩子时卡住）
static const uint32_t STALE_MS = 60000;

// 这两个变量由 web 回调（AsyncTCP 任务）写、页面 loop（主任务）读。
// 在 ESP32 上 32 位对齐读写是原子的，标记 volatile 即可。
volatile int g_cc_state = ST_IDLE;
volatile uint32_t g_cc_ts = 0;

static int cc_lookup(const char *s)
{
    for (int i = 0; i < ST_COUNT; i++)
        if (strcasecmp(s, CC[i].name) == 0)
            return i;
    return ST_IDLE; // 未知一律按空闲处理
}

// ---------------- 时钟数字配色 (RGB565) ----------------
static const uint16_t COL_H = 0x07FF; // 时：青
static const uint16_t COL_M = 0xFFFF; // 分：白
static const uint16_t COL_S = 0x07E0; // 秒：绿

static int last_e = -99;
static int last_hh = -1, last_mm = -1, last_ss = -1;
static uint32_t t_clk = 0;

static void drawField(uint8_t screen, int val, uint16_t color)
{
    char buf[4];
    sprintf(buf, "%02d", val);
    int16_t x1, y1;
    uint16_t w, h;
    gfx[screen]->fillScreen(BLACK);
    gfx[screen]->setFont(&Orbitron_Medium_48);
    gfx[screen]->setTextSize(1);
    gfx[screen]->getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
    gfx[screen]->setCursor((OLED_WIDTH - w) / 2 - x1, (OLED_HEIGHT - h) / 2 - y1);
    gfx[screen]->setTextColor(color);
    gfx[screen]->print(buf);
}

static void drawClock(bool force)
{
    if (!getLocalTime(&timeInfo))
        return;
    if (force || timeInfo.tm_hour != last_hh)
    {
        last_hh = timeInfo.tm_hour;
        drawField(0, last_hh, COL_H);
    }
    if (force || timeInfo.tm_min != last_mm)
    {
        last_mm = timeInfo.tm_min;
        drawField(1, last_mm, COL_M);
    }
    if (force || timeInfo.tm_sec != last_ss)
    {
        last_ss = timeInfo.tm_sec;
        drawField(2, last_ss, COL_S);
    }
}

static void drawExpression(int e)
{
    char path[48];
    for (uint8_t i = 0; i < 3; i++)
    {
        snprintf(path, sizeof(path), "/cc/%s/%d.png", CC[e].name, i + 1);
        myDrawPNG(0, 0, path, i); // 图片为 128x128，铺满整屏
    }
}

// 计算“当前实际应显示的状态”（处理自动回退与超时兜底）
static int effectiveState()
{
    int e = g_cc_state;
    uint32_t age = millis() - g_cc_ts;
    if (e == ST_IDLE || e == ST_OFF)
        return e;
    if (CC[e].transient && age > CC[e].hold_ms)
        return ST_IDLE;
    if (!CC[e].transient && age > STALE_MS)
        return ST_IDLE;
    return e;
}

// ---------------- 页面回调 ----------------
static void init(void *data)
{
    // 只注册一次 /cc 路由。board_init() 已经 server.begin()，
    // ESPAsyncWebServer 支持运行后再添加 handler。
    static bool route_done = false;
    if (!route_done)
    {
        route_done = true;
        server.on("/cc", HTTP_GET, [](AsyncWebServerRequest *req)
                  {
            if (req->hasParam("state")) {
                String s = req->getParam("state")->value();
                g_cc_state = cc_lookup(s.c_str());
                g_cc_ts = millis();
            }
            req->send(200, "text/plain", "ok"); });
    }
}

static void enter(void *data)
{
    gfx1->fillScreen(BLACK);
    gfx2->fillScreen(BLACK);
    gfx3->fillScreen(BLACK);
    last_e = -99;                          // 强制下一帧重绘
    last_hh = last_mm = last_ss = -1;
    manager_setBusy(false);
}

static void loop(void *data)
{
    int e = effectiveState();

    if (e != last_e)
    {
        last_e = e;
        if (e == ST_IDLE)
        {
            gfx1->fillScreen(BLACK);
            gfx2->fillScreen(BLACK);
            gfx3->fillScreen(BLACK);
            last_hh = last_mm = last_ss = -1;
            drawClock(true);
        }
        else if (e == ST_OFF)
        {
            gfx1->fillScreen(BLACK);
            gfx2->fillScreen(BLACK);
            gfx3->fillScreen(BLACK);
        }
        else
        {
            drawExpression(e);
        }
    }

    if (e == ST_IDLE && millis() - t_clk >= 250)
    {
        t_clk = millis();
        drawClock(false);
    }

    // 按键：长按 KEY4 返回菜单；KEY1/编码器 可在没连 Claude Code 时手动预览各状态
    static int preview[] = {ST_THINKING, ST_AI, ST_BUSY, ST_SUCCESS, ST_ERROR, ST_ALARM, ST_DEMO, ST_IDLE};
    static int pidx = 0;
    KEY_TYPE key = app_key_get();
    switch (key)
    {
    case KEY1_SHORT:
    case ENC_NEXT:
        pidx = (pidx + 1) % (sizeof(preview) / sizeof(preview[0]));
        g_cc_state = preview[pidx];
        g_cc_ts = millis();
        break;
    case ENC_PREV:
        pidx = (pidx - 1 + (int)(sizeof(preview) / sizeof(preview[0]))) % (int)(sizeof(preview) / sizeof(preview[0]));
        g_cc_state = preview[pidx];
        g_cc_ts = millis();
        break;
    case KEY4_LONG:
        manager_switchToParent();
        break;
    default:
        break;
    }
}

static void exit(void *data)
{
    manager_setBusy(true);
}

#include "img.h"
page_t page_ccmonitor = {
    .init = init,
    .enter = enter,
    .exit = exit,
    .loop = loop,
    .title_en = "Claude",
    .title_cn = "编码助手",
    .icon = img_bits,
    .icon_width = img_width,
    .icon_height = img_height,
    .sleep_enable = false,
    .wakeup_btn_effect_enable = false,
    .acc_enable = false,
};
