#ifndef CNB_TOCNPWG_XMLWRAP_H
#define CNB_TOCNPWG_XMLWRAP_H
/* ==================================================================
 *  xmlwrap.h —— tocnpwg 的 libxml2 运行时加载层
 *
 *  为什么不用普通动态链接：
 *    libxml2 在不同发行版/不同版本上的 soname 不一致（2.12 及以前
 *    是 libxml2.so.2，2.13+ 变成 libxml2.so.16，以后还会变）。硬编
 *    NEEDED 到某个 soname，换个发行版就加载失败——表现为 tocnpwg
 *    起不来、rastertocanonij 管道 EPIPE、打印机静默不出纸。
 *
 *    本层改为运行时 dlopen：
 *      1) 环境变量 LIBXML2_SO 可强制指定库路径（发行版/调试出口）
 *      2) 按裸 soname（libxml2.so.16、.so.2 …）交给 ld.so.cache 解析
 *         —— Debian 系 multiarch 把库放在 /usr/lib/<triplet>/ 下，
 *            走缓存才不用硬编码目录名
 *         这只是快速路径，表里的版本号只是常见值；将来 libxml2 升到
 *         .so.17/.18/... 用不着改这里——第 3 步会按目录里的实际文件兜住
 *      3) 兜底：扫描常见库目录（含 multiarch 子目录）下所有 libxml2.so.*，
 *         按版本号从新到旧尝试（数值比较，100 > 17 > 10 > 2）
 *      4) 任一候选加载成功且 8 个符号齐全即用；全部失败才报错
 *    于是 NEEDED 里不再有 libxml2，同一份二进制在 AOSC / Debian /
 *    Loongnix / Fedora / Arch 上都能直接工作。
 *
 *  使用方法（mkpset.c）：
 *      #include <libxml/xmlwriter.h>   // 保留：提供类型定义
 *      #include "xmlwrap.h"            // 之后代码写法完全不变
 *      在每个使用 XML 的函数入口调用 xmlw_init()，非 0 即失败返回。
 *
 *  注意：所有函数名宏定义必须放在本文件末尾，避免展开内部实现。
 * ================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <limits.h>
#include <dlfcn.h>
#include <libxml/xmlwriter.h>

typedef struct {
    xmlBufferPtr     (*xmlBufferCreate)(void);
    void             (*xmlBufferFree)(xmlBufferPtr);
    xmlTextWriterPtr (*xmlNewTextWriterMemory)(xmlBufferPtr, int);
    void             (*xmlFreeTextWriter)(xmlTextWriterPtr);
    int              (*xmlTextWriterStartDocument)(xmlTextWriterPtr,
                        const char *, const char *, const char *);
    int              (*xmlTextWriterStartElement)(xmlTextWriterPtr,
                        const xmlChar *);
    int              (*xmlTextWriterEndElement)(xmlTextWriterPtr);
    int              (*xmlTextWriterWriteFormatAttribute)(xmlTextWriterPtr,
                        const xmlChar *, const char *, ...);
} XmlApi;

static XmlApi xmlw;   /* mkpset.c 是唯一使用方，单实例即可 */

/* 尝试加载单个候选；成功（且符号齐全）返回 0 */
static int xmlw_try( const char *path )
{
	static const char *syms[] = {
		"xmlBufferCreate", "xmlBufferFree", "xmlNewTextWriterMemory",
		"xmlFreeTextWriter", "xmlTextWriterStartDocument",
		"xmlTextWriterStartElement", "xmlTextWriterEndElement",
		"xmlTextWriterWriteFormatAttribute",
	};
	void **slots[8];
	void *h;
	int i;

	h = dlopen( path, RTLD_NOW | RTLD_LOCAL );
	if ( h == NULL ) return -1;

	slots[0] = (void **)&xmlw.xmlBufferCreate;
	slots[1] = (void **)&xmlw.xmlBufferFree;
	slots[2] = (void **)&xmlw.xmlNewTextWriterMemory;
	slots[3] = (void **)&xmlw.xmlFreeTextWriter;
	slots[4] = (void **)&xmlw.xmlTextWriterStartDocument;
	slots[5] = (void **)&xmlw.xmlTextWriterStartElement;
	slots[6] = (void **)&xmlw.xmlTextWriterEndElement;
	slots[7] = (void **)&xmlw.xmlTextWriterWriteFormatAttribute;

	for ( i = 0; i < 8; i++ ) {
		*slots[i] = dlsym( h, syms[i] );
		if ( *slots[i] == NULL ) {
			dlclose( h );
			return -1;
		}
	}
	return 0;
}

/* 解析 "libxml2.so.X.Y.Z" 尾部的版本号为数值 key（供降序排序） */
static void xmlw_verkey( const char *tail, unsigned long long key[3] )
{
	int i;
	for ( i = 0; i < 3; i++ ) key[i] = 0;
	for ( i = 0; i < 3 && *tail; i++ ) {
		unsigned long long v = 0;
		while ( *tail >= '0' && *tail <= '9' )
			v = v * 10 + (unsigned long long)(*tail++ - '0');
		key[i] = v;
		if ( *tail == '.' ) tail++;
		else break;
	}
}

typedef struct {
	char path[PATH_MAX];
	unsigned long long key[3];
} XmlCand;

static int xmlw_cmp( const void *a, const void *b )
{
	const XmlCand *x = a, *y = b;
	int i;
	for ( i = 0; i < 3; i++ ) {
		if ( x->key[i] != y->key[i] )
			return ( y->key[i] > x->key[i] ) ? 1 : -1;   /* 降序 */
	}
	return 0;
}

/* 收集一个目录里的 libxml2.so.*（depth>0 时下探一层 multiarch 子目录） */
static void xmlw_scan_dir( XmlCand *cands, int *ncand, int max,
                           const char *dir, int depth )
{
	DIR *dp = opendir( dir );
	struct dirent *de;

	if ( dp == NULL ) return;
	while ( (de = readdir( dp )) != NULL && *ncand < max ) {
		if ( strncmp( de->d_name, "libxml2.so.", 11 ) == 0 ) {
			XmlCand *c = &cands[*ncand];
			if ( (int)sizeof(c->path) <= snprintf( c->path, sizeof(c->path),
					"%s/%s", dir, de->d_name ) )
				continue;
			xmlw_verkey( de->d_name + 11, c->key );
			(*ncand)++;
		} else if ( depth > 0 && de->d_name[0] != '.' &&
		            strstr( de->d_name, "-linux-" ) != NULL ) {
			/* multiarch：/usr/lib/loongarch64-linux-gnu/libxml2.so.2 */
			char sub[PATH_MAX];
			if ( (int)sizeof(sub) > snprintf( sub, sizeof(sub), "%s/%s",
					dir, de->d_name ) )
				xmlw_scan_dir( cands, ncand, max, sub, depth - 1 );
		}
	}
	closedir( dp );
}

/* 懒加载入口：幂等。0=已就绪，-1=找不到可用的 libxml2 */
static int xmlw_init( void )
{
	static int done = 0;
	static const char *sonames[] = {
		"libxml2.so.16", "libxml2.so.15", "libxml2.so.14", "libxml2.so.13",
		"libxml2.so.12", "libxml2.so.2", "libxml2.so", NULL,
	};
	static const char *dirs[] = {
		"/usr/lib64", "/usr/lib",
		"/usr/local/lib64", "/usr/local/lib",
		"/lib64", "/lib", NULL,
	};
	XmlCand cands[64];
	int ncand = 0, i, d;
	char tried[1024];

	if ( done ) return ( xmlw.xmlBufferCreate != NULL ) ? 0 : -1;
	done = 1;

	/* 1) 显式出口：LIBXML2_SO=/path/to/libxml2.so.X */
	{
		const char *env = getenv( "LIBXML2_SO" );
		if ( env != NULL && env[0] != '\0' ) {
			if ( xmlw_try( env ) == 0 ) return 0;
			fprintf( stderr, "tocnpwg: LIBXML2_SO=%s 加载失败\n", env );
		}
	}

	/* 2) 裸 soname 交给 ld.so.cache —— 覆盖 multiarch / 非标准 prefix，
	 *    不依赖下面那张硬编码目录表 */
	for ( i = 0; sonames[i] != NULL; i++ ) {
		if ( xmlw_try( sonames[i] ) == 0 ) return 0;
	}

	/* 3) 兜底：扫目录（含一层 multiarch 子目录），按版本降序逐个试 */
	for ( d = 0; dirs[d] != NULL; d++ )
		xmlw_scan_dir( cands, &ncand, 64, dirs[d], 1 );
	if ( ncand > 1 ) qsort( cands, ncand, sizeof(XmlCand), xmlw_cmp );

	tried[0] = '\0';
	for ( i = 0; i < ncand; i++ ) {
		if ( xmlw_try( cands[i].path ) == 0 ) return 0;
		if ( strlen( tried ) < 800 )
			snprintf( tried + strlen( tried ), sizeof(tried) - strlen(tried),
				"%s%.400s", i ? " " : "", cands[i].path );
	}
	fprintf( stderr, "tocnpwg: 找不到可用的 libxml2（尝试: %s%s）\n",
		tried, ncand ? "" : "无候选" );
	return -1;
}

/* ------------------------------------------------------------------
 * 函数名宏：让 mkpset.c 里的调用保持上游原样。
 * 必须放在最后——避免展开上面的内部实现。
 * ------------------------------------------------------------------ */
#define xmlBufferCreate                   xmlw.xmlBufferCreate
#define xmlBufferFree                     xmlw.xmlBufferFree
#define xmlNewTextWriterMemory            xmlw.xmlNewTextWriterMemory
#define xmlFreeTextWriter                 xmlw.xmlFreeTextWriter
#define xmlTextWriterStartDocument        xmlw.xmlTextWriterStartDocument
#define xmlTextWriterStartElement         xmlw.xmlTextWriterStartElement
#define xmlTextWriterEndElement           xmlw.xmlTextWriterEndElement
#define xmlTextWriterWriteFormatAttribute xmlw.xmlTextWriterWriteFormatAttribute

#endif /* CNB_TOCNPWG_XMLWRAP_H */
