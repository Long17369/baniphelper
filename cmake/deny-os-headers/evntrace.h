// 「禁止包含」占位文件，不是真正的系统头。理由见同目录 windows.h。
#error "核心层与界面层不得包含平台专有头 <evntrace.h>：事件订阅只能出现在 src/platform/ 下。"
