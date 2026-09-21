// 「禁止包含」占位文件，不是真正的系统头。理由见同目录 windows.h。
#error "核心层与界面层不得包含 <arpa/inet.h>：地址格式转换只能出现在 src/platform/ 下。"
