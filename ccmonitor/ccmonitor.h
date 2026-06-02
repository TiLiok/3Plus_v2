#ifndef __CCMONITOR_H
#define __CCMONITOR_H
#include "manager/manager.h"

// Claude Code 状态监控页面：
//  - 收到状态时三屏显示对应表情包 (/cc/<state>/{1,2,3}.png)
//  - 空闲时三屏显示时钟 (时/分/秒)
// 状态由 Claude Code 的 hooks 通过 HTTP 推送： GET /cc?state=busy
extern page_t page_ccmonitor;

#endif
