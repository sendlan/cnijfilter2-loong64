/*
 *  cncl_native.c —— Canon IVEC 命令层（libcnbpcnclapicom2.so.5.0.0）的 loong64 原生实现
 *
 *  契约来源（可复核）：
 *   1) 函数原型：源码树 4 份一致的 include/cncl/cnclcmdutils.h（extern 原型）
 *      + 各调用方 dlsym 的函数指针强转（维护组/状态组不在头文件里）
 *   2) XML 模板：闭源库 .rodata 中 52 条模板逐条提取（0x0152c8 ~ 0x019b40）
 *   3) SetConfiguration 模板选择规则：反汇编 0xe6c9~0xe85a
 *          r12 = Settings.colormode, r13 = Settings.duplexprint
 *          colormode==-1 ? (duplex==-1 -> 0x182b8 ; <=2 -> 0x183f8 ; else 0x18560)
 *                        : (duplex==-1 -> 0x17e10 ; <=2 -> 0x17f78 ; else 0x18108)
 *   4) 映射表：.rodata 字符串块 + 选项扫描实测（原始材料不随仓库分发）
 *   5) 逐字节验收：与上游官方实现对同一批输入的输出逐字节比对（见 tests/sweep/BASELINE-LOG.md）
 *
 *  授权：cnijfilter2 各源文件 NOTE 明确许可与二进制模块链接，本替代实现无授权障碍。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>

#include "tables.h"

#define OK       0
#define ERR_ARG  (-2)
#define ERR_GEN  (-1)

/* ---- XML 声明（闭源库中两种写法并存，必须原样保留） ---- */
#define XD_OLD  "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
#define XD_NEW  "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
/* 维护版 SetJobConfiguration 的 XML 头。注意 "job_description" 不是运行时残留，
 * 而是 Canon 自己写进 .rodata 模板的字面量：原库 @0x018d24 处模板串就是
 * 'encoding="job_descriptionutf-8" ?><cmd ...' —— 源码级 typo。
 * 照抄是为了与原始输出逐字节一致（差分验证见 sweep/mnt-regress.py）。 */
#define XD_NEW_MNT "<?xml version=\"1.0\" encoding=\"job_descriptionutf-8\" ?>"

#define NS_COMMON  "http://www.canon.com/ns/cmd/2008/07/common/"
#define NS_CANON   "http://www.canon.com/ns/cmd/2008/07/canon/"

/* <cmd 之后的命名空间属性串 */
#define ATTR_IVEC  " xmlns:ivec=\"" NS_COMMON "\""
#define ATTR_BOTH  ATTR_IVEC " xmlns:vcn=\"" NS_CANON "\""

/* 作业号常量（cnclcmdutils.h / cmdtocanonij.c） */
#define CN_START_JOBID   "00000001"
#define CN_START_JOBID2  "00000002"

/* ================================================================== */
/*  0) 小型格式化器：模板里只用 %s（数值也以字符串传入）              */
/* ================================================================== */

static int tpl_expand(char *out, long cap, const char *fmt, const char *const *args, int nargs)
{
    long w = 0;
    int ai = 0;
    const char *p = fmt;

    if (!out || cap <= 0 || !fmt) {
        return ERR_ARG;
    }
    while (*p) {
        if (p[0] == '%' && p[1] == 's') {
            const char *s = (ai < nargs && args[ai]) ? args[ai] : "";
            size_t n = strlen(s);
            if (w + (long)n >= cap) {
                return ERR_ARG;
            }
            memcpy(out + w, s, n);
            w += (long)n;
            ai++;
            p += 2;
        } else {
            if (w + 1 >= cap) {
                return ERR_ARG;
            }
            out[w++] = *p++;
        }
    }
    out[w] = '\0';
    return (int)w;
}

/* 表查询（ID 越界或空槽 -> NULL）已在 tables.h 内联实现 */

/* ================================================================== */
/*  1) 编解码                                                          */
/* ================================================================== */

/* 物证：闭源库 0x880c 处 `xorl $0x39, %edx` */
int CNCL_DecodeFromString(const char *src, size_t srcLen,
                          uint8_t *dst, size_t dstCap)
{
    size_t i, o = 0;

    if (!src || !dst) {
        return ERR_GEN;
    }
    for (i = 0; i + 1 < srcLen && o < dstCap; i += 2) {
        int hi = (unsigned char)src[i] - 'a';
        int lo = (unsigned char)src[i + 1] - 'a';
        if (hi < 0 || hi > 15 || lo < 0 || lo > 15) {
            break;                                  /* '=' 或非法字符即止 */
        }
        dst[o++] = (uint8_t)(((hi << 4) | lo) ^ 0x39);
    }
    return (int)o;
}

int CNCL_EncodeToString(const uint8_t *buf, size_t bufSize,
                        char *str, size_t strSize)
{
    size_t i, o = 0;

    if (!buf || !str) {
        return ERR_GEN;
    }
    for (i = 0; i < bufSize; i++) {
        uint8_t b = (uint8_t)(buf[i] ^ 0x39);
        if (o + 2 >= strSize) {
            return ERR_ARG;
        }
        str[o++] = (char)('a' + ((b >> 4) & 0x0f));
        str[o++] = (char)('a' + (b & 0x0f));
    }
    str[o] = '\0';
    return (int)o;
}

/* ================================================================== */
/*  2) 从 PPD 取编码块并解码                                           */
/*    块格式：  *% #<TAG>            （起）                            */
/*              *% <encoded...>      （内容，多行拼接）                */
/*              *% #<                （止）                            */
/*    返回：解码后字节数（decode!=0）或原始字符数（decode==0）         */
/* ================================================================== */

int CNCL_GetStringWithTagFromFile(const char *fileName, const char *tagName,
                                  int decode, uint8_t **resBuffer)
{
    FILE *fp;
    char line[8192];
    char start[256];
    int inside = 0, found = 0;
    size_t cap = 1 << 16, len = 0;
    char *enc;

    if (!fileName || !tagName || !resBuffer) {
        return ERR_GEN;
    }
    *resBuffer = NULL;

    snprintf(start, sizeof(start), "*%% #%s>", tagName);

    fp = fopen(fileName, "r");
    if (!fp) {
        return ERR_GEN;
    }
    enc = malloc(cap);
    if (!enc) {
        fclose(fp);
        return ERR_GEN;
    }

    while (fgets(line, sizeof(line), fp)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (!inside) {
            if (strncmp(line, "*% #", 4) == 0 && strcmp(line, start) == 0) {
                inside = 1;
                found = 1;
            }
            continue;
        }
        if (strncmp(line, "*% #", 4) == 0) {
            break;                                   /* 遇到下一个块标记 -> 结束 */
        }
        if (strncmp(line, "*% ", 3) == 0) {
            const char *p = line + 3;
            size_t add = strlen(p);
            if (len + add + 2 > cap) {
                char *t;
                cap *= 2;
                t = realloc(enc, cap);
                if (!t) {
                    free(enc);
                    fclose(fp);
                    return ERR_GEN;
                }
                enc = t;
            }
            memcpy(enc + len, p, add);
            len += add;
        }
    }
    fclose(fp);

    if (!found) {
        free(enc);
        return 0;
    }
    enc[len] = '\0';

    if (decode != 0) {
        char *eq = strchr(enc, '=');
        size_t mainLen = eq ? (size_t)(eq - enc) : len;
        uint8_t *dec = malloc(mainLen / 2 + 8);
        int n;
        if (!dec) {
            free(enc);
            return ERR_GEN;
        }
        n = CNCL_DecodeFromString(enc, mainLen, dec, mainLen / 2 + 8);
        free(enc);
        if (n < 0) {
            free(dec);
            return ERR_GEN;
        }
        dec[n] = '\0';
        *resBuffer = dec;
        return n;
    }

    *resBuffer = (uint8_t *)enc;
    return (int)len;
}

/* ================================================================== */
/*  3) 协议判定 / 能力解析                                             */
/* ================================================================== */

int CNCL_GetProtocol(const char *deviceID, size_t length_deviceID)
{
    size_t i;

    if (!deviceID) {
        return 1;
    }
    /* Device ID 前两字节是长度前缀，故按给定长度扫（不依赖 NUL） */
    for (i = 0; i + 4 <= length_deviceID; i++) {
        if (memcmp(deviceID + i, "IVEC", 4) == 0) {
            return 2;
        }
    }
    return 1;
}

/* 在 XML 中找首个含 ":<name" 的元素起点；返回指向 '>' 之后的指针，失败 NULL */
static const char *xml_elem_body(const char *xml, size_t size, const char *name,
                                 size_t *bodyLen)
{
    char pat[128];
    const char *p, *gt;
    size_t patLen;

    snprintf(pat, sizeof(pat), ":%s", name);
    patLen = strlen(pat);

    p = memmem(xml, size, pat, patLen);
    if (!p) {
        return NULL;
    }
    /* 必须紧跟 '>'、'/' 或空格（避免匹配到更长的名字） */
    {
        char c = p[patLen];
        if (c != '>' && c != '/' && c != ' ' && c != '\t') {
            return NULL;
        }
    }
    gt = memchr(p, '>', size - (size_t)(p - xml));
    if (!gt) {
        return NULL;
    }
    if (gt[-1] == '/') {                     /* 自闭合：内容为空 */
        if (bodyLen) {
            *bodyLen = 0;
        }
        return gt + 1;
    }
    if (bodyLen) {
        /* 闭合标签可能带命名空间前缀（</vcn:host_environment>）或不带
         * （</host_environment>），两种都要匹配 */
        char cl[128], cl2[128];
        const char *e1, *e2, *end;
        size_t rem = size - (size_t)(gt + 1 - xml);

        snprintf(cl, sizeof(cl), "</%s>", name);
        snprintf(cl2, sizeof(cl2), ":%s>", name);
        e1 = memmem(gt + 1, rem, cl, strlen(cl));
        e2 = memmem(gt + 1, rem, cl2, strlen(cl2));
        end = (e1 && e2) ? (e1 < e2 ? e1 : e2) : (e1 ? e1 : e2);
        *bodyLen = end ? (size_t)(end - (gt + 1)) : 0;
    }
    return gt + 1;
}

/*
 * host_environment 元素存在 -> 返回其中逗号分隔的条目数（G3010 为 7）
 * 不存在 -> 0。tocanonij 只判 !=0，用来决定 StartJob 是否带 host_environment。
 */
static int parse_host_env(const void *xmlBuf, int xmlSize)
{
    size_t len = 0, n = 0;
    const char *body;

    if (!xmlBuf || xmlSize <= 0) {
        return 0;
    }
    body = xml_elem_body((const char *)xmlBuf, (size_t)xmlSize, "host_environment", &len);
    if (!body || len == 0) {
        return 0;
    }
    n = 1;
    {
        size_t i;
        for (i = 0; i < len; i++) {
            if (body[i] == ',') {
                n++;
            }
        }
    }
    return (int)n;
}

/* datetime 元素存在 -> 2（由主机填时间）；不存在 -> 1 */
static int parse_datetime(const void *xmlBuf, int xmlSize)
{
    if (!xmlBuf || xmlSize <= 0) {
        return 1;
    }
    return xml_elem_body((const char *)xmlBuf, (size_t)xmlSize, "datetime", NULL)
           ? 2 : 1;
}

int CNCL_ParseCapabilityResponsePrint_HostEnv(void *xmlCapabilityPrint, unsigned int xmlSize)
{
    return parse_host_env(xmlCapabilityPrint, (int)xmlSize);
}

int CNCL_ParseCapabilityResponsePrint_DateTime(void *xmlCapabilityPrint, int xmlSize)
{
    return parse_datetime(xmlCapabilityPrint, xmlSize);
}

int CNCL_ParseCapabilityResponseMaintenance_HostEnv(void *xmlCapabilityPrint, int xmlSize)
{
    return parse_host_env(xmlCapabilityPrint, xmlSize);
}

int CNCL_ParseCapabilityResponseMaintenance_DateTime(void *xmlCapabilityPrint, int xmlSize)
{
    return parse_datetime(xmlCapabilityPrint, xmlSize);
}

/* ================================================================== */
/*  4) 打印路径                                                        */
/* ================================================================== */

/*
 * StartJob3 —— 模板（.rodata 0x017050 / 0x016a20）
 *   0x017050（带 host_environment）:
 *     XD_OLD<cmd%s><ivec:contents><ivec:operation>StartJob</ivec:operation>
 *       <ivec:param_set servicetype="%s"><ivec:jobID>%s</ivec:jobID>
 *       <ivec:bidi>%s</ivec:bidi><vcn:forcepmdetection>OFF</vcn:forcepmdetection>
 *       <ivec:jobname/><ivec:username/><ivec:computername/>
 *       <ivec:job_description><![CDATA[%s]]></ivec:job_description>
 *       <vcn:host_environment>%s</vcn:host_environment>
 *       </ivec:param_set></ivec:contents></cmd>
 *   hostEnv != 0 用 0x017050，否则用 0x016a20（同上的不带 host_environment 版）
 *   实参：attr, servicetype, jobID, bidi, jobDescription, [host_environment]
 */
int CNCL_MakeCommand_StartJob3(int hostEnvID, char *uuid, char jobID[],
                               void *cmdBuffer, int cmdBufferSize,
                               unsigned int *writtenSize)
{
    static const char TPL_HOST[] =
        XD_OLD "<cmd%s><ivec:contents>"
        "<ivec:operation>StartJob</ivec:operation>"
        "<ivec:param_set servicetype=\"%s\">"
        "<ivec:jobID>%s</ivec:jobID><ivec:bidi>%s</ivec:bidi>"
        "<vcn:forcepmdetection>OFF</vcn:forcepmdetection>"
        "<ivec:jobname/><ivec:username/><ivec:computername/>"
        "<ivec:job_description><![CDATA[%s]]></ivec:job_description>"
        "<vcn:host_environment>%s</vcn:host_environment>"
        "</ivec:param_set></ivec:contents></cmd>";
    static const char TPL_NOHOST[] =
        XD_OLD "<cmd%s><ivec:contents>"
        "<ivec:operation>StartJob</ivec:operation>"
        "<ivec:param_set servicetype=\"%s\">"
        "<ivec:jobID>%s</ivec:jobID><ivec:bidi>%s</ivec:bidi>"
        "<vcn:forcepmdetection>OFF</vcn:forcepmdetection>"
        "<ivec:jobname/><ivec:username/><ivec:computername/>"
        "<ivec:job_description><![CDATA[%s]]></ivec:job_description>"
        "</ivec:param_set></ivec:contents></cmd>";
    const char *a[6];
    int r;

    if (!cmdBuffer || cmdBufferSize <= 0) {
        return ERR_ARG;
    }
    a[0] = ATTR_BOTH;
    a[1] = "print";
    a[2] = jobID ? jobID : "";
    a[3] = "0";
    a[4] = uuid ? uuid : "";
    a[5] = "linux";

    r = (hostEnvID != 0)
            ? tpl_expand((char *)cmdBuffer, cmdBufferSize, TPL_HOST, a, 6)
            : tpl_expand((char *)cmdBuffer, cmdBufferSize, TPL_NOHOST, a, 5);
    if (r < 0) {
        return ERR_ARG;
    }
    if (writtenSize) {
        *writtenSize = (unsigned int)r;
    }
    return OK;
}

/*
 * SetJobConfiguration —— 模板 0x015af0（XD_NEW，仅 ivec 命名空间）
 *   注意此处模板内无 %s，闭源库是按 XML 路径改写 jobID / datetime 两处取值；
 *   本实现直接参数化，输出字节等价。
 */
int CNCL_MakeCommand_SetJobConfiguration(char jobID[], char datetime[],
                                         void *cmdBuffer, int cmdBufferSize,
                                         unsigned int *writtenSize)
{
    static const char TPL[] =
        XD_NEW "<cmd" ATTR_IVEC "><ivec:contents>"
        "<ivec:operation>SetJobConfiguration</ivec:operation>"
        "<ivec:param_set servicetype=\"print\">"
        "<ivec:jobID>%s</ivec:jobID>"
        "<ivec:mismatch_mode>none</ivec:mismatch_mode>"
        "<ivec:datetime>%s</ivec:datetime>"
        "</ivec:param_set></ivec:contents></cmd>";
    const char *a[2];
    int r;

    if (!cmdBuffer || cmdBufferSize <= 0) {
        return ERR_ARG;
    }
    a[0] = jobID ? jobID : "";
    a[1] = datetime ? datetime : "";
    r = tpl_expand((char *)cmdBuffer, cmdBufferSize, TPL, a, 2);
    if (r < 0) {
        return ERR_ARG;
    }
    if (writtenSize) {
        *writtenSize = (unsigned int)r;
    }
    return OK;
}

/*
 * SetConfiguration —— 六个模板，选择规则见文件头（反汇编 0xe6c9~0xe85a）。
 *   模板形如：XD_OLD<cmd%s><ivec:contents><ivec:operation>SetConfiguration</...>
 *     <ivec:param_set servicetype="print"><ivec:jobID>%s</ivec:jobID>
 *     <%s:papersize>%s</%s:papersize> ... [<%s:printcolormode>] [<%s:duplexprint>]
 *     [<ivec:stapleside>%s</ivec:stapleside>]
 *   元素前缀 = "ivec"，<cmd 后属性串 = ATTR_IVEC。
 */
int CNCL_GetSetConfigurationCommand(void *pSettings, char *jobID, long cmdBufferSize,
                                    void *xmlBuffer, long xmlBufferSize,
                                    char *cmdBuffer, long *writtenSize)
{
    /* tbl = {开头, papersize 前, papertype 前, borderless 前, colormode 前, duplex 前} */
    static const char T_BASE[] =                       /* 0x182b8 : 3 元素 */
        XD_OLD "<cmd%s><ivec:contents>"
        "<ivec:operation>SetConfiguration</ivec:operation>"
        "<ivec:param_set servicetype=\"print\">"
        "<ivec:jobID>%s</ivec:jobID>"
        "<%s:papersize>%s</%s:papersize>"
        "<%s:papertype>%s</%s:papertype>"
        "<%s:borderlessprint>%s</%s:borderlessprint>"
        "</ivec:param_set></ivec:contents></cmd>";
    static const char T_DUP[] =                        /* 0x183f8 : 3 元素 + duplex */
        XD_OLD "<cmd%s><ivec:contents>"
        "<ivec:operation>SetConfiguration</ivec:operation>"
        "<ivec:param_set servicetype=\"print\">"
        "<ivec:jobID>%s</ivec:jobID>"
        "<%s:papersize>%s</%s:papersize>"
        "<%s:papertype>%s</%s:papertype>"
        "<%s:borderlessprint>%s</%s:borderlessprint>"
        "<%s:duplexprint>%s</%s:duplexprint>"
        "</ivec:param_set></ivec:contents></cmd>";
    static const char T_DUP_STAPLE[] =                 /* 0x18560 : 3 元素 + duplex + staple */
        XD_OLD "<cmd%s><ivec:contents>"
        "<ivec:operation>SetConfiguration</ivec:operation>"
        "<ivec:param_set servicetype=\"print\">"
        "<ivec:jobID>%s</ivec:jobID>"
        "<%s:papersize>%s</%s:papersize>"
        "<%s:papertype>%s</%s:papertype>"
        "<%s:borderlessprint>%s</%s:borderlessprint>"
        "<%s:duplexprint>%s</%s:duplexprint>"
        "<ivec:stapleside>%s</ivec:stapleside>"
        "</ivec:param_set></ivec:contents></cmd>";
    static const char T_COL[] =                        /* 0x17e10 : 4 元素 */
        XD_OLD "<cmd%s><ivec:contents>"
        "<ivec:operation>SetConfiguration</ivec:operation>"
        "<ivec:param_set servicetype=\"print\">"
        "<ivec:jobID>%s</ivec:jobID>"
        "<%s:papersize>%s</%s:papersize>"
        "<%s:papertype>%s</%s:papertype>"
        "<%s:borderlessprint>%s</%s:borderlessprint>"
        "<%s:printcolormode>%s</%s:printcolormode>"
        "</ivec:param_set></ivec:contents></cmd>";
    static const char T_COL_DUP[] =                    /* 0x17f78 : 4 元素 + duplex */
        XD_OLD "<cmd%s><ivec:contents>"
        "<ivec:operation>SetConfiguration</ivec:operation>"
        "<ivec:param_set servicetype=\"print\">"
        "<ivec:jobID>%s</ivec:jobID>"
        "<%s:papersize>%s</%s:papersize>"
        "<%s:papertype>%s</%s:papertype>"
        "<%s:borderlessprint>%s</%s:borderlessprint>"
        "<%s:printcolormode>%s</%s:printcolormode>"
        "<%s:duplexprint>%s</%s:duplexprint>"
        "</ivec:param_set></ivec:contents></cmd>";
    static const char T_COL_DUP_STAPLE[] =             /* 0x18108 : 4 元素 + duplex + staple */
        XD_OLD "<cmd%s><ivec:contents>"
        "<ivec:operation>SetConfiguration</ivec:operation>"
        "<ivec:param_set servicetype=\"print\">"
        "<ivec:jobID>%s</ivec:jobID>"
        "<%s:papersize>%s</%s:papersize>"
        "<%s:papertype>%s</%s:papertype>"
        "<%s:borderlessprint>%s</%s:borderlessprint>"
        "<%s:printcolormode>%s</%s:printcolormode>"
        "<%s:duplexprint>%s</%s:duplexprint>"
        "<ivec:stapleside>%s</ivec:stapleside>"
        "</ivec:param_set></ivec:contents></cmd>";

    const unsigned char *s = (const unsigned char *)pSettings;
    short v_colormode, v_duplex;
    int ns_colormode, ns_duplex;
    const char *ps, *pt, *bl, *cm, *dx;
    const char *ns = "ivec";
    const char *a[20];
    const char *tpl;
    int na = 0, r;

    (void)xmlBuffer;
    (void)xmlBufferSize;

    if (!s || !cmdBuffer || cmdBufferSize <= 0) {
        return ERR_ARG;
    }

    /* Settings 结构（short × 6）：+0 version,+2 papersize,+4 mediatype,
     * +6 borderlessprint,+8 colormode,+10 duplexprint */
    memcpy(&v_colormode, s + 8, sizeof(short));
    memcpy(&v_duplex, s + 10, sizeof(short));

    ps = cncl_paper_tbl_get(s[2] | (s[3] << 8));
    pt = cncl_media_tbl_get(s[4] | (s[5] << 8));
    bl = cncl_onoff_tbl_get(s[6] | (s[7] << 8));
    cm = cncl_color_tbl_get(v_colormode);
    dx = cncl_onoff_tbl_get(v_duplex);

    if (!ps || !pt || !bl || !dx) {
        return ERR_GEN;
    }
    if (v_colormode != -1 && !cm) {
        return ERR_GEN;
    }

    ns_colormode = (v_colormode == -1) ? 0 : 1;     /* 0 -> colormode 段省略 */
    ns_duplex    = (v_duplex == -1) ? 0 : ((v_duplex <= 2) ? 1 : 2);

    if (!ns_colormode) {
        tpl = (ns_duplex == 0) ? T_BASE : ((ns_duplex == 1) ? T_DUP : T_DUP_STAPLE);
    } else {
        tpl = (ns_duplex == 0) ? T_COL : ((ns_duplex == 1) ? T_COL_DUP : T_COL_DUP_STAPLE);
    }

    a[na++] = ATTR_IVEC;
    a[na++] = jobID ? jobID : "";
    a[na++] = ns; a[na++] = ps; a[na++] = ns;
    a[na++] = ns; a[na++] = pt; a[na++] = ns;
    a[na++] = ns; a[na++] = bl; a[na++] = ns;
    if (ns_colormode) {
        a[na++] = ns; a[na++] = cm; a[na++] = ns;
    }
    if (ns_duplex) {
        a[na++] = ns; a[na++] = dx; a[na++] = ns;
    }
    if (ns_duplex == 2) {
        a[na++] = "none";                            /* stapleside：无装订机型占位 */
    }

    r = tpl_expand(cmdBuffer, cmdBufferSize, tpl, a, na);
    if (r < 0) {
        return ERR_ARG;
    }
    if (writtenSize) {
        *writtenSize = r;
    }
    return OK;
}

/*
 * SetPageConfiguration —— 模板 0x015fa8（XD_OLD，ivec+vcn 双命名空间）
 *   nextpage: 1 = OFF, 2 = ON（CNCL_PSET_NEXTPAGE_*）
 */
int CNCL_GetSetPageConfigurationCommand(const char *jobID, unsigned short nextpage,
                                        void *cmdBuffer, long cmdBufferSize,
                                        long *writtenSize)
{
    static const char TPL[] =
        XD_OLD "<cmd" ATTR_BOTH "><ivec:contents>"
        "<ivec:operation>VendorCmd</ivec:operation>"
        "<ivec:param_set servicetype=\"print\">"
        "<ivec:jobID>%s</ivec:jobID>"
        "<vcn:ijoperation>SetPageConfiguration</vcn:ijoperation>"
        "<vcn:nextpage>%s</vcn:nextpage>"
        "</ivec:param_set></ivec:contents></cmd>";
    const char *a[2];
    int r;

    if (!cmdBuffer || cmdBufferSize <= 0) {
        return ERR_ARG;
    }
    a[0] = jobID ? jobID : "";
    a[1] = (nextpage == 2) ? "ON" : "OFF";

    r = tpl_expand(cmdBuffer, cmdBufferSize, TPL, a, 2);
    if (r < 0) {
        return ERR_ARG;
    }
    if (writtenSize) {
        *writtenSize = r;
    }
    return OK;
}

/*
 * SendData（PWGRaster）—— 模板由 XML 路径（0x018778 datasize / 0x0187c0 format）
 * 动态构造；此处按实测输出定形，字节等价。
 */
int CNCL_GetSendDataPWGRasterCommand(char *jobID, long data_size, long cmdBufferSize,
                                     char *cmdBuffer, long *writtenSize)
{
    static const char TPL[] =
        XD_OLD "<cmd" ATTR_IVEC "><ivec:contents>"
        "<ivec:operation>SendData</ivec:operation>"
        "<ivec:param_set servicetype=\"print\">"
        "<ivec:jobID>%s</ivec:jobID>"
        "<ivec:format>PWGRaster</ivec:format>"
        "<ivec:datasize>%s</ivec:datasize>"
        "</ivec:param_set></ivec:contents></cmd>";
    char nbuf[32];
    const char *a[2];
    int r;

    if (!cmdBuffer || cmdBufferSize <= 0) {
        return ERR_ARG;
    }
    snprintf(nbuf, sizeof(nbuf), "%ld", data_size);
    a[0] = jobID ? jobID : "";
    a[1] = nbuf;

    r = tpl_expand(cmdBuffer, cmdBufferSize, TPL, a, 2);
    if (r < 0) {
        return ERR_ARG;
    }
    if (writtenSize) {
        *writtenSize = r;
    }
    return OK;
}

/*
 * GetPrintCommand —— op=3(END) 走 EndJob 模板 0x019b40；
 * op=1/2/4 属 CHMP(BJL) 旧协议路径，G3010 走不到，此处给等价 StartJob 形态。
 */
int CNCL_GetPrintCommand(char *cmdBuffer, long cmdBufferSize, long *writtenSize,
                         char *jobId, long opration_id)
{
    static const char T_END[] =
        XD_OLD "<cmd xmlns:ivec=\"" NS_COMMON "\"><ivec:contents>"
        "<ivec:operation>EndJob</ivec:operation>"
        "<ivec:param_set servicetype=\"%s\">"
        "<ivec:jobID>%s</ivec:jobID>"
        "</ivec:param_set></ivec:contents></cmd>";
    static const char T_START[] =
        XD_OLD "<cmd xmlns:ivec=\"" NS_COMMON "\"><ivec:contents>"
        "<ivec:operation>StartJob</ivec:operation>"
        "<ivec:param_set servicetype=\"%s\">"
        "<ivec:jobID>%s</ivec:jobID><ivec:bidi>%s</ivec:bidi>"
        "</ivec:param_set></ivec:contents></cmd>";
    const char *a[3];
    int r, na;

    if (!cmdBuffer || cmdBufferSize <= 0) {
        return ERR_ARG;
    }
    if (opration_id == 3) {                          /* CNCL_COMMAND_END */
        a[0] = "print";
        a[1] = jobId ? jobId : "";
        na = 2;
        r = tpl_expand(cmdBuffer, cmdBufferSize, T_END, a, na);
    } else {
        a[0] = "print";
        a[1] = jobId ? jobId : "";
        a[2] = "0";
        na = 3;
        r = tpl_expand(cmdBuffer, cmdBufferSize, T_START, a, na);
    }
    if (r < 0) {
        return ERR_ARG;
    }
    if (writtenSize) {
        *writtenSize = r;
    }
    return OK;
}

/*
 * BJL 设定时间（旧 CHMP 协议）。G3010 为 IVEC，不会走到。
 * 仍须导出，否则 tocanonij 的 dlsym 检查会让整条链失败。
 */
int CNCL_MakeBJLSetTimeJob(void *cmdBuffer, size_t cmdBufferSize, size_t *writtenSize)
{
    static const char TPL[] =
        XD_OLD "<cmd" ATTR_BOTH "><ivec:contents>"
        "<ivec:operation>SetJobConfiguration</ivec:operation>"
        "<ivec:param_set servicetype=\"print\">"
        "<ivec:mismatch_mode>none</ivec:mismatch_mode>"
        "<ivec:datetime>%s</ivec:datetime>"
        "</ivec:param_set></ivec:contents></cmd>";
    char dt[32];
    const char *a[1];
    int r;

    if (!cmdBuffer || cmdBufferSize == 0) {
        return ERR_ARG;
    }
    snprintf(dt, sizeof(dt), "%ld", (long)time(NULL));
    a[0] = dt;
    r = tpl_expand((char *)cmdBuffer, (long)cmdBufferSize, TPL, a, 1);
    if (r < 0) {
        return ERR_ARG;
    }
    if (writtenSize) {
        *writtenSize = (size_t)r;
    }
    return OK;
}

/*
 * GetCapability 命令（模板 0x0194a0 / 0x0192a0 / 0x0193a0）
 *   service_type: 1 = print, 2 = maintenance, 3 = device
 */
int CNCL_MakeGetCapabilityCommand(char *cmdBuffer, unsigned int cmdBufferSize,
                                  unsigned int *writtenSize, int service_type)
{
    static const char TPL[] =
        XD_OLD "<cmd" ATTR_IVEC "><ivec:contents>"
        "<ivec:operation>GetCapability</ivec:operation>"
        "<ivec:param_set servicetype=\"%s\">"
        "</ivec:param_set></ivec:contents></cmd>";
    const char *svc = (service_type == 2) ? "maintenance"
                    : (service_type == 3) ? "device" : "print";
    const char *a[1];
    int r;

    if (!cmdBuffer || cmdBufferSize == 0) {
        return ERR_ARG;
    }
    a[0] = svc;
    r = tpl_expand(cmdBuffer, (long)cmdBufferSize, TPL, a, 1);
    if (r < 0) {
        return ERR_ARG;
    }
    if (writtenSize) {
        *writtenSize = (unsigned int)r;
    }
    return OK;
}

/* ================================================================== */
/*  5) 维护路径（cmdtocanonij3）                                       */
/*     注意：签名取自调用方 dlsym 强转，与头文件不同（多一个 jobID）   */
/* ================================================================== */

/* 0x018b70：SetJobConfiguration（maintenance），jobID/datetime 按路径改写 */
int CLSS_MakeCommand_SetJobConfiguration_Maintenance(char jobID[], char datetime[],
                                                     void *cmdBuffer, int cmdBufferSize,
                                                     long *writtenSize)
{
    static const char TPL[] =
        XD_NEW_MNT "<cmd" ATTR_BOTH "><ivec:contents>"
        "<ivec:operation>SetJobConfiguration</ivec:operation>"
        "<ivec:param_set servicetype=\"maintenance\">"
        "<ivec:jobID>%s</ivec:jobID>"
        "<ivec:datetime>%s</ivec:datetime>"
        "</ivec:param_set></ivec:contents></cmd>";
    const char *a[2];
    int r;

    if (!cmdBuffer || cmdBufferSize <= 0) {
        return ERR_ARG;
    }
    a[0] = jobID ? jobID : "";
    a[1] = datetime ? datetime : "";
    r = tpl_expand((char *)cmdBuffer, cmdBufferSize, TPL, a, 2);
    if (r < 0) {
        return ERR_ARG;
    }
    if (writtenSize) {
        *writtenSize = r;
    }
    return OK;
}

/* 0x018808：Cleaning（%s = jobID 元素） */
int CNCL_MakeCommand_Cleaning(char jobID[], void *cmdBuffer, int cmdBufferSize,
                              long *writtenSize)
{
    static const char TPL[] =
        XD_NEW "<cmd" ATTR_IVEC "><ivec:contents>"
        "<ivec:operation>Cleaning</ivec:operation>"
        "<ivec:param_set servicetype=\"maintenance\">"
        "<ivec:inkgroup>all</ivec:inkgroup><ivec:type>regular</ivec:type>%s"
        "</ivec:param_set></ivec:contents></cmd>";
    char jb[64];
    const char *a[1];
    int r;

    if (!cmdBuffer || cmdBufferSize <= 0) {
        return ERR_ARG;
    }
    if (jobID && jobID[0]) {
        snprintf(jb, sizeof(jb), "<ivec:jobID>%s</ivec:jobID>", jobID);
    } else {
        jb[0] = '\0';
    }
    a[0] = jb;
    r = tpl_expand((char *)cmdBuffer, cmdBufferSize, TPL, a, 1);
    if (r < 0) {
        return ERR_ARG;
    }
    if (writtenSize) {
        *writtenSize = r;
    }
    return OK;
}

/* 0x018a58：TestPrint / nozzle_check */
int CNCL_MakeCommand_TestPrint(char jobID[], void *cmdBuffer, int cmdBufferSize,
                               long *writtenSize)
{
    static const char TPL[] =
        XD_NEW "<cmd" ATTR_IVEC "><ivec:contents>"
        "<ivec:operation>TestPrint</ivec:operation>"
        "<ivec:param_set servicetype=\"maintenance\">"
        "<ivec:type>nozzle_check</ivec:type>%s"
        "</ivec:param_set></ivec:contents></cmd>";
    char jb[64];
    const char *a[1];
    int r;

    if (!cmdBuffer || cmdBufferSize <= 0) {
        return ERR_ARG;
    }
    if (jobID && jobID[0]) {
        snprintf(jb, sizeof(jb), "<ivec:jobID>%s</ivec:jobID>", jobID);
    } else {
        jb[0] = '\0';
    }
    a[0] = jb;
    r = tpl_expand((char *)cmdBuffer, cmdBufferSize, TPL, a, 1);
    if (r < 0) {
        return ERR_ARG;
    }
    if (writtenSize) {
        *writtenSize = r;
    }
    return OK;
}

/* 0x018938：TestPrint / auto_registration */
int CNCL_MakeCommand_AutoAlignment(char jobID[], void *cmdBuffer, int cmdBufferSize,
                                   long *writtenSize)
{
    static const char TPL[] =
        XD_NEW "<cmd" ATTR_IVEC "><ivec:contents>"
        "<ivec:operation>TestPrint</ivec:operation>"
        "<ivec:param_set servicetype=\"maintenance\">"
        "<ivec:type>auto_registration</ivec:type>%s"
        "</ivec:param_set></ivec:contents></cmd>";
    char jb[64];
    const char *a[1];
    int r;

    if (!cmdBuffer || cmdBufferSize <= 0) {
        return ERR_ARG;
    }
    if (jobID && jobID[0]) {
        snprintf(jb, sizeof(jb), "<ivec:jobID>%s</ivec:jobID>", jobID);
    } else {
        jb[0] = '\0';
    }
    a[0] = jb;
    r = tpl_expand((char *)cmdBuffer, cmdBufferSize, TPL, a, 1);
    if (r < 0) {
        return ERR_ARG;
    }
    if (writtenSize) {
        *writtenSize = r;
    }
    return OK;
}

/*
 * 维护 StartJob3 —— 模板 0x0178b0（带 host_environment）/ 0x017728（不带）
 *   签名 (int hostEnv, char *uuid, char jobID[], void *buf, long bufSize, long *written)
 */
int CNCL_MakeCommand_StartJob3_Maintenance(int hostEnvID, char *uuid, char jobID[],
                                           void *cmdBuffer, long cmdBufferSize,
                                           long *writtenSize)
{
    static const char TPL_HOST[] =
        XD_OLD "<cmd%s><ivec:contents>"
        "<ivec:operation>StartJob</ivec:operation>"
        "<ivec:param_set servicetype=\"%s\">"
        "<ivec:jobID>%s</ivec:jobID><ivec:bidi>%s</ivec:bidi>"
        "<ivec:jobname/><ivec:username/><ivec:computername/>"
        "<ivec:job_description><![CDATA[%s]]></ivec:job_description>"
        "<vcn:host_environment>%s</vcn:host_environment>"
        "</ivec:param_set></ivec:contents></cmd>";
    static const char TPL_NOHOST[] =
        XD_OLD "<cmd%s><ivec:contents>"
        "<ivec:operation>StartJob</ivec:operation>"
        "<ivec:param_set servicetype=\"%s\">"
        "<ivec:jobID>%s</ivec:jobID><ivec:bidi>%s</ivec:bidi>"
        "<ivec:jobname/><ivec:username/><ivec:computername/>"
        "<ivec:job_description><![CDATA[%s]]></ivec:job_description>"
        "</ivec:param_set></ivec:contents></cmd>";
    const char *a[6];
    int r;

    if (!cmdBuffer || cmdBufferSize <= 0) {
        return ERR_ARG;
    }
    a[0] = ATTR_BOTH;
    a[1] = "maintenance";
    a[2] = jobID ? jobID : "";
    a[3] = "0";
    a[4] = uuid ? uuid : "";
    a[5] = "linux";

    r = (hostEnvID != 0)
            ? tpl_expand((char *)cmdBuffer, cmdBufferSize, TPL_HOST, a, 6)
            : tpl_expand((char *)cmdBuffer, cmdBufferSize, TPL_NOHOST, a, 5);
    if (r < 0) {
        return ERR_ARG;
    }
    if (writtenSize) {
        *writtenSize = r;
    }
    return OK;
}

/* 0x019b40：EndJob（servicetype = maintenance） */
int CNCL_MakeCommand_EndJob_Maintenance(char jobID[], void *cmdBuffer, long cmdBufferSize,
                                        long *writtenSize)
{
    static const char TPL[] =
        XD_OLD "<cmd" ATTR_IVEC "><ivec:contents>"
        "<ivec:operation>EndJob</ivec:operation>"
        "<ivec:param_set servicetype=\"%s\">"
        "<ivec:jobID>%s</ivec:jobID>"
        "</ivec:param_set></ivec:contents></cmd>";
    const char *a[2];
    int r;

    if (!cmdBuffer || cmdBufferSize <= 0) {
        return ERR_ARG;
    }
    a[0] = "maintenance";
    a[1] = jobID ? jobID : "";
    r = tpl_expand((char *)cmdBuffer, cmdBufferSize, TPL, a, 2);
    if (r < 0) {
        return ERR_ARG;
    }
    if (writtenSize) {
        *writtenSize = r;
    }
    return OK;
}

/* 0x019a20：CancelJob（servicetype 可变） */
static int cancel_job_common(const char *svc, char *jobID, char *cmdBuffer,
                             unsigned int cmdBufferSize, unsigned int *writtenSize)
{
    static const char TPL[] =
        XD_OLD "<cmd" ATTR_IVEC "><ivec:contents>"
        "<ivec:operation>CancelJob</ivec:operation>"
        "<ivec:param_set servicetype=\"%s\">"
        "<ivec:jobID>%s</ivec:jobID>"
        "</ivec:param_set></ivec:contents></cmd>";
    const char *a[2];
    int r;

    if (!cmdBuffer || cmdBufferSize == 0) {
        return ERR_ARG;
    }
    a[0] = svc;
    a[1] = jobID ? jobID : "";
    r = tpl_expand(cmdBuffer, (long)cmdBufferSize, TPL, a, 2);
    if (r < 0) {
        return ERR_ARG;
    }
    if (writtenSize) {
        *writtenSize = (unsigned int)r;
    }
    return OK;
}

int CNCL_MakeCommand_CancelJob(char *jobID, char *cmdBuffer,
                               unsigned int cmdBufferSize, unsigned int *writtenSize)
{
    return cancel_job_common("print", jobID, cmdBuffer, cmdBufferSize, writtenSize);
}

int CNCL_MakeCommand_CancelJob_Maintenance(char *jobID, char *cmdBuffer,
                                           unsigned int cmdBufferSize,
                                           unsigned int *writtenSize)
{
    return cancel_job_common("maintenance", jobID, cmdBuffer, cmdBufferSize, writtenSize);
}

/* ================================================================== */
/*  6) 状态查询命令（模板 0x018e20 / 0x018d00，字面 jobID 00000001）   */
/* ================================================================== */

int CNCL_MakeCommand_GetStatusPrint(char *cmdBuffer, unsigned int cmdBufferSize,
                                    unsigned int *writtenSize)
{
    static const char TPL[] =
        XD_OLD "<cmd" ATTR_IVEC "><ivec:contents>"
        "<ivec:operation>GetStatus</ivec:operation>"
        "<ivec:param_set servicetype=\"print\">"
        "<ivec:jobID>" CN_START_JOBID "</ivec:jobID>"
        "</ivec:param_set></ivec:contents></cmd>";
    int r;

    if (!cmdBuffer || cmdBufferSize == 0) {
        return ERR_ARG;
    }
    r = tpl_expand(cmdBuffer, (long)cmdBufferSize, TPL, NULL, 0);
    if (r < 0) {
        return ERR_ARG;
    }
    if (writtenSize) {
        *writtenSize = (unsigned int)r;
    }
    return OK;
}

int CNCL_MakeCommand_GetStatusMaintenance(char *cmdBuffer, unsigned int cmdBufferSize,
                                          unsigned int *writtenSize)
{
    static const char TPL[] =
        XD_OLD "<cmd" ATTR_IVEC "><ivec:contents>"
        "<ivec:operation>GetStatus</ivec:operation>"
        "<ivec:param_set servicetype=\"maintenance\">"
        "<ivec:jobID>" CN_START_JOBID "</ivec:jobID>"
        "</ivec:param_set></ivec:contents></cmd>";
    int r;

    if (!cmdBuffer || cmdBufferSize == 0) {
        return ERR_ARG;
    }
    r = tpl_expand(cmdBuffer, (long)cmdBufferSize, TPL, NULL, 0);
    if (r < 0) {
        return ERR_ARG;
    }
    if (writtenSize) {
        *writtenSize = (unsigned int)r;
    }
    return OK;
}

/* ================================================================== */
/*  7) 响应解析（cnijlgmon3 用）                                      */
/* ------------------------------------------------------------------ *
 *  逆向依据（全部可在官方 6.90 库上复核）：
 *    · 字典表：.data.rel.ro 里 4 张 {char* name; u64 val} 16 字节项表，
 *      索引即返回值。表地址与项数取自 GOT 槽位的 RELATIVE 重定位项：
 *        0x221298 -> OperationTbl     0x21f880  项数 27 (0x1c936)
 *        0x221138 -> ResponseTbl      0x21faa0  项数  3 (0x1c93a)
 *        0x221118 -> ResponseDetailTbl 0x21fae0 项数 12 (0x1c93c)
 *        0x221228 -> StatusTbl        0x21fba0
 *        0x221128 -> StatusDetailTbl  0x21fcc0
 *    · 解析流程：反汇编 CLSS_ParseResponseCommon @0x10480 /
 *      getOperation_ServiceType @0x12be0 / GetCanonID @0x12510 /
 *      parseCommonStatusResponse @0x12dd0 / CNCL_GetInfoResponse @0x9490 /
 *      CNCL_GetStatus @0x92c0。
 *    · 取值方式：GetCanonID 线性扫描表，命中后返回的是**下标**而不是
 *      条目里的 u64 字段（u64 字段全为 0xffff，不参与判定）。
 *    · 关键 XML 取值路径（.rodata 0x220d00 / 0x21f740 / 0x21f580）：
 *        cmd/ivec:contents/ivec:operation
 *        cmd/ivec:contents/ivec:param_set servicetype="print"/ivec:response
 *        .../ivec:response_detail        .../ivec:jobID
 *        .../ivec:jobstatus/ivec:jobID   .../vcn:ijoperation  .../vcn:ijresponse
 *        .../ivec:status                 .../ivec:status_detail
 *        .../ivec:current_support_code
 *
 *  ⚠ 返回类型说明：官方头文件写的是 unsigned short，但闭源库实际把**完整
 *    32 位**（如 0xfffffffe）留在返回寄存器里，而唯一的真实调用方
 *    cnijlgmon3 是用 `int (*)(...)` 通过 dlsym 调用的，并据此判 `err < 0`。
 *    若按 unsigned short 返回，0xfffe 会被零扩展成 65534 → 被误判为成功。
 *    故本实现把返回值声明为 int，与官方实际行为一致。
 * ================================================================== */

#define CN_ITEM_NONE      65535
#define CN_ERR_ARG        (-2)        /* 0xfffffffe 参数/查表失败 */
#define CN_ERR_SVC        (-3)        /* 0xfffffffd serviceType 非法 */
#define CN_ERR_NOMATCH    (-5)        /* 0xfffffffb 索引为 0xffff */
#define CN_ERR_NO_OP      (-9)        /* 0xfffffff7 操作名匹配数 != 1 */
#define CN_ERR_NO_RESP    (-10)       /* 0xfffffff6 response 节点匹配数 != 1 */
#define CN_ERR_NO_RESPID  (-15)       /* 0xfffffff1 response 未命中字典 */
#define CN_ERR_NO_DETAIL  (-16)       /* 0xfffffff0 response_detail 未命中字典 */

/* ---- 操作名表：索引 = opId（官方 glb_clssdicOperationTbl @0x21f880） ---- */
static const char *const CN_OP_TBL[] = {
    NULL,                                   /*  0 */
    "GetCapability",                        /*  1 */
    "GetCapabilityResponse",                /*  2 */
    "GetConfiguration",                     /*  3 */
    "GetConfigurationResponse",             /*  4 */
    "SetConfiguration",                     /*  5 */
    "SetConfigurationResponse",             /*  6 */
    "StartJob",                             /*  7 */
    "StartJobResponse",                     /*  8  CN_IVEC_START_RESPONSE */
    "EndJob",                               /*  9 */
    "EndJobResponse",                       /* 10  CN_IVEC_END_RESPONSE */
    "SendData",                             /* 11 */
    "SendDataResponse",                     /* 12 */
    "GetStatus",                            /* 13 */
    "GetStatusResponse",                    /* 14  CN_IVEC_STATUS_RESPONSE */
    NULL, NULL, NULL, NULL,                 /* 15..18 */
    "PowerOff",                             /* 19 */
    "PowerOffResponse",                     /* 20 */
    "VendorCmd",                            /* 21 */
    "VendorCmdResponse",                    /* 22  CN_IVEC_VENDER_RESPONSE */
    "SetJobConfiguration",                  /* 23 */
    "SetJobConfigurationResponse",          /* 24 */
    "CancelJob",                            /* 25 */
    "CancelJobResponse",                    /* 26 */
};
#define CN_OP_TBL_N       27                /* 官方扫描 0..26 */

/* 官方用 glb_OperationInfoTbl[i].+4 == 1 判定“必须是 Response 类操作”，
 * 该标志为 1 的恰好是下面这批下标（与上表偶/奇配对关系一致）。 */
static const unsigned char CN_OP_IS_RESPONSE[] = {
    0, /*0*/ 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, /*1..14*/
    0, 0, 0, 0,                                       /*15..18*/
    0, 1, 0, 1, 0, 1, 0, 1,                           /*19..26*/
};

/* ---- 响应码表（索引 = responseId，官方 @0x21faa0，项数 3） ---- */
static const char *const CN_RESP_TBL[] = { NULL, "OK", "NG" };
#define CN_RESP_TBL_N     3
#define CN_RESP_NG        2

/* ---- 响应明细表（官方 @0x21fae0，项数 12） ---- */
static const char *const CN_RESP_DETAIL_TBL[] = {
    "DeivceUseOtherJob", "ParameterError", "NotSupportService",
    "NotSupportOperation", "NotStart", "IllegalJobID", "IllegalOperation",
    "DataSizeOver", "Initializing", "Suspended", "ShuttingDown",
};
#define CN_RESP_DETAIL_TBL_N 11

/* ---- 状态表（索引 = statusId，官方 @0x21fba0） ---- */
static const char *const CN_STATUS_TBL[] = {
    NULL, "idle", "processing", "stopped", "notready", "busying", "canceling",
    NULL, NULL, "waitingrender", "rendering", "waitingprint", "printing",
    "held", "storing", "preparing", "busying",
};
#define CN_STATUS_TBL_N   17

/* ---- 状态明细表（官方 @0x21fcc0） ---- */
static const char *const CN_STATUS_DETAIL_TBL[] = {
    NULL, "AttentionRequired", "MediaJam", "DoorOpen", "MediaEmpty",
    "MarkerSupplyAttention", "DeviceBusy", "CUSTOM_MISMATCH", NULL,
    "processing", "stopped", "canceling", "busying",
};
#define CN_STATUS_DETAIL_TBL_N 13

/* ------------------------------------------------------------------ *
 *  大小写敏感的名字查表。命中返回下标，未命中返回 -1。
 *  对齐官方 GetCanonID：线性扫描 0..n-1，逐条 BJVSCompString 比较。
 * ------------------------------------------------------------------ */
static int cn_lookup(const char *const *tbl, int n, const char *s, int slen)
{
    int i;

    if (s == NULL || slen <= 0) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        const char *p = tbl[i];
        int j = 0;

        if (p == NULL) {
            continue;
        }
        while (p[j] != '\0' && j < slen && p[j] == s[j]) {
            j++;
        }
        if (p[j] == '\0' && j == slen) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ *
 *  取 <tag>…</tag> 的文本。
 *  返回： 1 = 命中，0 = 未命中，-1 = 只见到开标签没有闭标签（视为畸形）
 *  自闭合 <tag/> 视为命中且文本为空（长度 0）——设备确实会发 <ivec:status_detail/>。
 * ------------------------------------------------------------------ */
static int cn_xml_tag(const char *buf, int len, const char *tag,
                      const char **txt, int *tlen)
{
    int tl = (int)strlen(tag);
    int i;

    for (i = 0; i + tl + 2 <= len; i++) {
        int p;

        if (buf[i] != '<') {
            continue;
        }
        if (memcmp(buf + i + 1, tag, (size_t)tl) != 0) {
            continue;
        }
        p = i + 1 + tl;
        /* 标签名后必须紧跟 '>'、'/' 或空白，避免 <ivec:status> 命中
         * <ivec:status_detail> 之类的前缀串。 */
        if (!(buf[p] == '>' || buf[p] == '/' || buf[p] == ' ' ||
              buf[p] == '\t' || buf[p] == '\r' || buf[p] == '\n')) {
            continue;
        }
        while (p < len && buf[p] != '>') {   /* 跳过属性 */
            p++;
        }
        if (p >= len) {
            return 0;
        }
        if (buf[p - 1] == '/') {             /* <tag/> */
            *txt = buf + p;
            *tlen = 0;
            return 1;
        }
        p++;                                  /* 越过 '>' */
        {
            int q = p;
            while (q + tl + 3 <= len) {
                if (buf[q] == '<' && buf[q + 1] == '/' &&
                    memcmp(buf + q + 2, tag, (size_t)tl) == 0 &&
                    buf[q + 2 + tl] == '>') {
                    *txt = buf + p;
                    *tlen = q - p;
                    return 1;
                }
                q++;
            }
        }
        return -1;
    }
    return 0;
}

/* 取属性值 tag attr="..."（官方用于 ivec:jobinfo jobID="..."） */
static int cn_xml_attr(const char *buf, int len, const char *tag,
                       const char *attr, const char **txt, int *tlen)
{
    int tl = (int)strlen(tag), al = (int)strlen(attr);
    int i;

    for (i = 0; i + tl + 2 <= len; i++) {
        int p, q;
        if (buf[i] != '<' || memcmp(buf + i + 1, tag, (size_t)tl) != 0) {
            continue;
        }
        p = i + 1 + tl;
        while (p < len && buf[p] != '>') {
            if (buf[p] == attr[0] && p + al < len &&
                memcmp(buf + p, attr, (size_t)al) == 0 &&
                (buf[p + al] == '=' || buf[p + al] == ' ')) {
                q = p + al;
                while (q < len && buf[q] != '=') { q++; }
                if (q >= len) { return 0; }
                q++;
                while (q < len && (buf[q] == '"' || buf[q] == ' ')) { q++; }
                {
                    int r = q;
                    while (r < len && buf[r] != '"') { r++; }
                    *txt = buf + q;
                    *tlen = r - q;
                    return 1;
                }
            }
            p++;
        }
    }
    return 0;
}

/* 取作业号：优先 .../ivec:jobID，其次 .../ivec:jobstatus/ivec:jobID */
static int cn_jobid(const char *buf, int len, const char **txt, int *tlen)
{
    if (cn_xml_tag(buf, len, "ivec:jobID", txt, tlen) == 1) {
        return 1;
    }
    if (cn_xml_tag(buf, len, "ivec:jobstatus", txt, tlen) == 1) {
        /* jobstatus 是容器元素，其文本里再找一次 */
        if (*tlen > 0) {
            if (cn_xml_tag(*txt, *tlen, "ivec:jobID", txt, tlen) == 1) {
                return 1;
            }
        }
    }
    return 0;
}

static void cn_set_str(char *dst, int cap, const char *s, int slen)
{
    int n;

    if (cap <= 0) {
        return;
    }
    memset(dst, 0, (size_t)cap);
    if (s == NULL || slen <= 0) {
        return;
    }
    n = (slen < cap - 1) ? slen : cap - 1;
    memcpy(dst, s, (size_t)n);
}

/* ------------------------------------------------------------------ *
 *  服务类型：0=print 1=device 2=scan 3=maintenance
 *  官方 getOperation_ServiceType 依据 4 个 param_set 谁存在来定序，本实现
 *  直接读第一个 servicetype="…" 属性——对本机型两种写法等价。
 * ------------------------------------------------------------------ */
static int cn_service_type(const char *buf, int len)
{
    static const char *const svc[] = { "print", "device", "scan", "maintenance" };
    int i, k;

    for (i = 0; i + 5 <= len; i++) {
        if (memcmp(buf + i, "servicetype=\"", 13) != 0) {
            continue;
        }
        for (k = 0; k < 4; k++) {
            size_t sl = strlen(svc[k]);
            if ((size_t)(len - i - 13) >= sl + 1 &&
                memcmp(buf + i + 13, svc[k], sl) == 0 &&
                buf[i + 13 + sl] == '"') {
                return k;
            }
        }
    }
    return 0;                                   /* 缺省按 print */
}

/* ------------------------------------------------------------------ *
 *  CNCL_GetInfoResponse
 *    data/readed : 打印机返回的 XML 原文
 *    oprationId  : 输出，操作 ID（见 CN_OP_TBL）
 *    jobId       : 输出，9 字节作业号缓冲
 *    responseDetail : 双向，*responseDetail 指向调用方提供的 u16 缓冲
 *  成功返回 0，失败返回 <= -5 的负值（与官方一致）。
 * ------------------------------------------------------------------ */
int CNCL_GetInfoResponse(char *data, int readed_data,
                         unsigned short *oprationId, char *jobId,
                         unsigned short **responseDetail)
{
    const char *buf = data;
    const char *txt = NULL;
    int len = readed_data, tlen = 0;
    int op, resp;
    unsigned short detail = (unsigned short)CN_ITEM_NONE;

    if (buf == NULL || len <= 0 || oprationId == NULL || jobId == NULL ||
        responseDetail == NULL || *responseDetail == NULL) {
        return CN_ERR_ARG;
    }
    memset(jobId, 0, 9);

    /* 1) 操作名 -> opId */
    if (cn_xml_tag(buf, len, "ivec:operation", &txt, &tlen) != 1) {
        return CN_ERR_NO_RESP;
    }
    op = cn_lookup(CN_OP_TBL, CN_OP_TBL_N, txt, tlen);
    if (op < 0) {
        return CN_ERR_ARG;
    }
    if (!CN_OP_IS_RESPONSE[op]) {         /* 必须是 Response 类操作 */
        return CN_ERR_ARG;
    }

    /* 2) <ivec:response> 必须在，且必须命中字典 */
    if (cn_xml_tag(buf, len, "ivec:response", &txt, &tlen) != 1) {
        return CN_ERR_NO_RESP;
    }
    resp = cn_lookup(CN_RESP_TBL, CN_RESP_TBL_N, txt, tlen);
    if (resp < 0) {
        return CN_ERR_NO_RESPID;
    }

    if (resp == CN_RESP_NG) {
        /* 3a) NG -> 读 <ivec:response_detail>，未命中即报错 */
        if (cn_xml_tag(buf, len, "ivec:response_detail", &txt, &tlen) != 1) {
            return CN_ERR_NO_RESP;
        }
        resp = cn_lookup(CN_RESP_DETAIL_TBL, CN_RESP_DETAIL_TBL_N, txt, tlen);
        if (resp < 0) {
            return CN_ERR_NO_DETAIL;
        }
        detail = (unsigned short)resp;
    } else {
        /* 3b) OK -> 若存在 <vcn:ijoperation> 则连 <vcn:ijresponse> 一起校验
         *     （官方在服务类型为 print 且走 v3 方言时要求两者成对出现）。 */
        const char *t2 = NULL;
        int l2 = 0;
        if (cn_xml_tag(buf, len, "vcn:ijoperation", &t2, &l2) == 1) {
            if (cn_xml_tag(buf, len, "vcn:ijresponse", &t2, &l2) != 1) {
                return CN_ERR_NO_RESP;
            }
        }
        /* 4) 作业号 */
        if (cn_jobid(buf, len, &txt, &tlen) == 1) {
            cn_set_str(jobId, 9, txt, tlen);
        }
    }

    *oprationId = (unsigned short)op;
    *(unsigned short *)(*responseDetail) = detail;
    return 0;
}

/* ------------------------------------------------------------------ *
 *  公共状态解析：statusId / statusDetail / supportId
 *  tmp 布局（官方 parseCommonStatusResponse @0x12dd0，rbx = 调用方 tmp+2）：
 *      tmp+0 -> statusId        tmp+2 -> statusDetail
 *      tmp+0x10 -> 未知 ID      tmp+0x12 -> supportId(11 字节)
 *  这里不需要那份 tmp，直接按调用方参数输出。
 * ------------------------------------------------------------------ */
static int cn_parse_status_common(const char *buf, int len,
                                  int *statusId, int *statusDetail, char *supportId)
{
    const char *txt = NULL;
    int tlen = 0, i;

    if (statusId) {
        *statusId = CN_ITEM_NONE;
    }
    if (statusDetail) {
        *statusDetail = CN_ITEM_NONE;
    }
    if (supportId) {
        supportId[0] = '\0';
    }

    /* 服务类型解析与官方一致：先跑一遍 getOperation_ServiceType */
    (void)cn_service_type(buf, len);

    if (cn_xml_tag(buf, len, "ivec:status", &txt, &tlen) == 1 && tlen > 0) {
        i = cn_lookup(CN_STATUS_TBL, CN_STATUS_TBL_N, txt, tlen);
        if (statusId) {
            *statusId = (i < 0) ? CN_ITEM_NONE : i;
        }
    }
    if (cn_xml_tag(buf, len, "ivec:status_detail", &txt, &tlen) == 1 && tlen > 0) {
        i = cn_lookup(CN_STATUS_DETAIL_TBL, CN_STATUS_DETAIL_TBL_N, txt, tlen);
        if (statusDetail) {
            *statusDetail = (i < 0) ? CN_ITEM_NONE : i;
        }
    }
    if (supportId &&
        cn_xml_tag(buf, len, "ivec:current_support_code", &txt, &tlen) == 1) {
        cn_set_str(supportId, 11, txt, tlen);
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 *  CNCL_GetStatus —— USB 状态查询
 * ------------------------------------------------------------------ */
int CNCL_GetStatus(char *data, int readed_data, int *statusId, int *statusDetail,
                   char *supportId)
{
    if (data == NULL || readed_data <= 0 || statusId == NULL ||
        statusDetail == NULL || supportId == NULL) {
        return CN_ERR_ARG;
    }
    return cn_parse_status_common(data, readed_data, statusId, statusDetail,
                                  supportId);
}

/* ------------------------------------------------------------------ *
 *  CNCL_GetStatus2 / CNCL_GetStatus_Maintenance —— NET3 专用
 *
 *  ⚠ 本机是 USB 机型，这两条通路无法真机验证，按官方反汇编得到的
 *    输出映射实现（tmp+2=statusId、tmp+4=statusDetail、tmp+6=jobId、
 *    tmp+0x12=supportId、栈上第 7 个参数 = extra/job_description）：
 *      CNCL_GetStatus2            -> CLSS_ParseStatusResponsePrint2
 *      CNCL_GetStatus_Maintenance -> CLSS_ParseStatusResponseMaintenance
 *    返回 0=OK；解析器返回 1 时二者分别映射为 10 / 1。
 * ------------------------------------------------------------------ */
static int cn_parse_status2(const char *buf, int len, char *jobId,
                            int *statusId, int *statusDetail, char *supportId,
                            char *extra)
{
    const char *txt = NULL;
    int tlen = 0, found;

    if (cn_parse_status_common(buf, len, statusId, statusDetail, supportId) != 0) {
        return CN_ERR_ARG;
    }
    found = 0;
    if (jobId) {
        jobId[0] = '\0';
        found = cn_jobid(buf, len, &txt, &tlen);
        if (!found) {
            found = cn_xml_attr(buf, len, "ivec:jobinfo", "jobID", &txt, &tlen);
        }
        if (found) {
            cn_set_str(jobId, 9, txt, tlen);
        }
    }
    /* ⚠ 官方语义（lgmon3 收尾循环的退出条件反推）：
     *   响应里没有 jobinfo/jobID = 打印机上已没有我们的作业。
     *   此时 CLSS_ParseStatusResponsePrint2 返回 CLSS_NOT_SUPPORT(1)，
     *   CNCL_GetStatus2 映射为 CLSS_NOT_SUPPORT_CUSTOM(10)，
     *   lgmon3 据此判定「作业完成」退出收尾轮询；维护通路则直接以
     *   CLSS_NOT_SUPPORT(1) 退出。若这里恒返回 0(CLSS_OK)，
     *   lgmon3 会永远 sleep(4) 轮询下去（网络打印作业"永不结束"）。
     *   真机验证：作业完成后打印机只回 <ivec:status>idle，无 jobinfo。 */
    if (jobId && !found) {
        return 1;
    }
    if (extra) {
        extra[0] = '\0';
        if (cn_xml_tag(buf, len, "ivec:job_description", &txt, &tlen) == 1 ||
            cn_xml_attr(buf, len, "ivec:jobinfo", "jobID", &txt, &tlen) == 1) {
            cn_set_str(extra, 64, txt, tlen);
        }
    }
    return 0;
}

int CNCL_GetStatus2(char *data, int readed_data, char *jobId, int *statusId,
                    int *statusDetail, char *supportId, char *extra)
{
    int r;

    if (data == NULL || readed_data <= 0) {
        return CN_ERR_ARG;
    }
    r = cn_parse_status2(data, readed_data, jobId, statusId, statusDetail,
                         supportId, extra);
    /* 官方：解析器返回 1（CLSS_NOT_SUPPORT）时本函数返回 10 */
    return (r == 1) ? 10 : r;
}

int CNCL_GetStatus_Maintenance(char *data, int readed_data, char *jobId, int *statusId,
                               int *statusDetail, char *supportId, char *extra)
{
    if (data == NULL || readed_data <= 0) {
        return CN_ERR_ARG;
    }
    return cn_parse_status2(data, readed_data, jobId, statusId, statusDetail,
                            supportId, extra);
}

/*
 * 说明（与官方逐条对应的差异）：
 *   - 官方用自带的 CL_XML 引擎做 XPath 子集匹配（cmd/ivec:contents/…）。
 *     本实现按“标签名检索”代替：设备实际发出的响应里每个标签全局唯一，
 *     两者结果等价；已知响应样本 60+ 份（rsplog.bin）逐份比对一致。
 *   - CNCL_GetStatus2 / CNCL_GetStatus_Maintenance 只在 NET3 通路使用，
 *     本机为 USB 机型，无法真机交叉验证，按反汇编的输出映射实现。
 *   - CNCL_MakeBJLSetTimeJob 属旧 CHMP(BJL) 协议，G3010 走不到，未做逐字节验证。
 */
