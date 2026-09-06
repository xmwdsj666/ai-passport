// main/page_home.h —— 主页对外接口(供外壳在数据变化时通知重绘)。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void page_home_notify_data(void);

#ifdef __cplusplus
}
#endif
