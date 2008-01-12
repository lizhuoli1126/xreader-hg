/* vim:set ts=4 sw=4 cindent ignorecase enc=gbk: */

#ifndef DBG_H
#define DBG_H

#include <assert.h>

#define TODO	assert("TODO" && 0)

typedef void (*dbg_func)(void *arg, const char* str);
typedef struct _dbg_handle {
	void (*init)(void *arg);
	dbg_func write;
	void (*cleanup)(void *arg);
	void *arg;
} dbg_handle;

typedef struct _Dbg {
	dbg_handle* ot;
	size_t      otsize;
	short on;
} DBG, *PDBG;

/** 初始化 */
extern DBG* dbg_init();
/** 关闭 */
extern int  dbg_close(DBG* d);
/** 关闭某一句柄 */
int dbg_close_handle(DBG *d, size_t index);
/** 输出到文件 */
extern int  dbg_open_file(DBG *d, const char* fn);
/** 输出到文件流 */
extern int  dbg_open_stream(DBG *d, FILE *stream);
#ifdef WIN32
/** 输出到控制台 */
extern int  dbg_open_console(DBG *d, HANDLE console);
#endif
/** 输出到黑洞 */
extern int  dbg_open_dummy(DBG *d);
#ifdef WIN32
/** 输出到网络 */
extern int  dbg_open_net(DBG *d, const char* hostname, int port);
#endif
/** 输出到PSP英文终端 */
int dbg_open_psp(DBG *d);
/** 输出到PSP汉字终端 */
int dbg_open_psp_hz(DBG *d);
/** 输出到PSP记录文件 */
int dbg_open_psp_logfile(DBG *d, const char *logfile);
/** 输出到自定义函数 */
extern int  dbg_open_custom(DBG *d, void (*writer)(const char*));
/** 带时间格式化输出 */
extern int  dbg_printf(DBG *d, const char* fmt, ...);
/** 格式化输出 */
extern int  dbg_printf_raw(DBG *d, const char* fmt, ...);
/** 十六进制转储,无ASCII版本 */
extern int  dbg_hexdump(DBG *d, const unsigned char* data, size_t len);
/** 十六进制转储,ASCII版本 */
extern int  dbg_hexdump_ascii(DBG *d, const unsigned char* data, size_t len);
/** 设置输出开关 */
extern void dbg_switch(DBG *d, short on);

extern int dbg_open_memorylog(DBG *d);
extern const char *dbg_get_memorylog(void);

void dbg_assert(DBG* d, char *info, int test, const char *func, const char* file, int line);
#define DBG_ASSERT(d, info, test) dbg_assert(d, info, test, __FUNCTION__, __FILE__, __LINE__)

enum { DBG_BUFSIZE = 800 };

extern DBG *d;
double pspDiffTime(u64 *t1, u64 *t2);

#endif
