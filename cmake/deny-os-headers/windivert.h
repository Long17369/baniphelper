// 「禁止包含」占位文件，不是真正的系统头。理由见同目录 windows.h。
#error "核心层与界面层不得包含 <windivert.h>：这是第三方内核驱动方案，只在 src/platform/win/ 下按需引入。"
