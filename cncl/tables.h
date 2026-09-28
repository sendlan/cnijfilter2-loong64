/*
 *  tables.h —— 6.20 源码枚举 ID  ->  IVEC 协议字符串
 *
 *  数据来源（可复核）：
 *   字符串：闭源库 /opt/canonij2/usr/lib/libcnbpcnclapicom2.so.5.0.0
 *          .rodata 中的字符串块（纸张 0x1b908 起、介质 0x1bbd2 起）
 *   ID 编号：本源码树 tocanonij/include/cncl/cnclcmdutilsdef.h（6.20 编号）
 *           ——注意 6.90 的编号与此不同，但线上协议只传字符串，故按语义配对即可。
 *
 *  ⚠ 配对结论由“选项扫描实验”实证：用上游官方实现逐值运行
 *     rastertocanonij，读取输出的 <ivec:papersize>/<ivec:papertype>。
 *     该实证脚本与产物属一次性材料，不随仓库分发。标 [实测] 的行为逐值验证过；
 *     标 [推] 的行该机型不可达（PPD 未声明 -> 回退默认值），为语义推断。
 *
 *  ⚠ 闭源库只认“本机型已注册”的纸张/介质 ID，其余一律令
 *     CNCL_GetSetConfigurationCommand 失败。实证：给 PPD 补上 A3/B4/A3+/2L 等
 *     未声明 choice 后再跑上游官方实现，stderr 出现
 *     "Error in CNCL_GetSetConfigurationCommand"，且只吐出 StartJob +
 *     SetJobConfiguration 两条命令就中止。
 *     因此本表对不可达 ID 填 NULL，而 cncl_native.c 在任一字段查表失败时
 *     返回 ERR_GEN —— 与原库行为一致。
 */
#ifndef CNCL_TABLES_H
#define CNCL_TABLES_H

/* ------------------------------------------------------------------ */
/*  纸张尺寸：索引 = CNCL_PSET_SIZE_*（6.20 编号）                      */
/* ------------------------------------------------------------------ */
static const char *const CNCL_PAPER_TBL[] = {
    NULL,
    "na_letter_8.5x11in",             /*  1 LETTER        [实测] */
    "na_legal_8.5x14in",              /*  2 LEGAL         [实测] */
    "iso_a5_148x210mm",               /*  3 A5            [实测] */
    "iso_a4_210x297mm",               /*  4 A4            [实测] */
    "iso_a3_297x420mm",               /*  5 A3            [推] */
    "custom_canon_329x483mm",         /*  6 A3_PLUS       [推] */
    "jis_b5_182x257mm",               /*  7 B5            [实测] */
    "jis_b4_257x364mm",               /*  8 B4            [推] */
    "na_index-4x6_4x6in",             /*  9 4X6           [实测] */
    "na_5x7_5x7in",                   /* 10 5X7           [实测] */
    "custom_canon_203x254mm",         /* 11 6GIRI(8x10)   [实测] */
    "custom_canon_254x304mm",         /* 12 4GIRI(10x12)  [推] */
    NULL,                             /* 13 -- */
    "custom_canon_89x127mm",          /* 14 L             [实测] */
    NULL,                             /* 15 2L（本机型不可达；闭源库对该 ID 亦拒绝） */
    "jpn_hagaki_100x148mm",           /* 16 POST / 明信片   [实测 PageSize=Postcard] */
    NULL,                             /* 17 -- */
    "custom_canon_55x91mm",           /* 18 BUSINESSCARD  [实测] */
    NULL,                             /* 19 -- */
    "na_number-10_4.125x9.5in",       /* 20 ENV_10        [推] */
    "iso_dl_110x220mm",               /* 21 ENV_DL        [实测] */
    "na_executive_7.25x10.5in",       /* 22 EXECUTIVE     [推] */
    "iso_a6_105x148mm",               /* 23 A6            [推] */
    "na_oficio_8.5x13.4in",           /* 24 OFICIO        [推] */
    "custom_canon_215x345mm",         /* 25 B_OFICIO      [推] */
    "custom_canon_215.9x317.5mm",     /* 26 M_OFICIO      [推] */
    "na_foolscap_8.5x13in",           /* 27 FOOLSCAP      [推] */
    "custom_canon_216x355mm",         /* 28 LEGAL_INDIA   [推] */
    "custom_canon_127x127mm",         /* 29 SQUARE_127    [实测] */
};
/* 稀疏 ID（>29）用下面这张补充表 */
#define CNCL_PAPER_EXT_BASE 78
static const char *const CNCL_PAPER_EXT_TBL[] = {
    "custom_canon_7x10in",            /* 78 7X10          [推] */
    "custom_canon_4x4in",             /* 79 SQUARE_4IN    [推] */
    "custom_canon_12x12in",           /* 80 SQUARE_12IN   [推] */
    NULL,                             /* 81 -- */
    "custom_canon_89x89mm",           /* 82 SQUARE_89     [推] */
};
#define CNCL_PAPER_EXT_MAX 5

/* ------------------------------------------------------------------ */
/*  介质：索引 = CNCL_PSET_MEDIA_*（6.20 编号）                        */
/* ------------------------------------------------------------------ */
static const char *const CNCL_MEDIA_TBL[] = {
    NULL,
    "stationery",                     /*  1 PLAIN                     [实测] */
    "custom-media-type-canon-3",      /*  2 PHOTO_PAPER_PLUS_GLOSSY_II[实测] */
    "custom-media-type-canon-2",      /*  3 PHOTO_PAPER_PRO_II        [推] */
    NULL,                             /*  4 -- */
    "custom-media-type-canon-4",      /*  5 PHOTO_PAPER_PRO_PLATINUM  [推] */
    "custom-media-type-canon-1",      /*  6 PROPHOTO                  [推] */
    "custom-media-type-canon-5",      /*  7 SUPER_PHOTO_PAPER         [推] */
    "custom-media-type-canon-17",     /*  8 PHOTO_PAPER_PRO_LUSTER    [实测] */
    "custom-media-type-canon-6",      /*  9 PHOTO_PAPER_SG            [实测] */
    "custom-media-type-canon-14",     /* 10 GLOSSY_PAPER              [实测] */
    "custom-media-type-canon-15",     /* 11 MATTE_PAPER               [实测] */
    "photographic",                   /* 12 PHOTOPAPER                [实测] */
    "custom-media-type-canon-11",     /* 13 INKJET_HAGAKI             [实测] */
    "custom-media-type-canon-12",     /* 14 HAGAKI                    [实测] */
    "custom-media-type-canon-9",      /* 15 HIGHRES                   [实测] */
    "custom-media-type-canon-16",     /* 16 OTHER_PHOTO_PAPER         [推] */
    "custom-media-type-canon-18",     /* 17 ENVELOPE                  [实测] */
    "custom-media-type-canon-19",     /* 18 LABEL                     [推] */
    NULL,                             /* 19 -- */
    NULL,                             /* 20 -- */
    NULL,                             /* 21 -- */
    NULL,                             /* 22 -- */
    NULL,                             /* 23 -- */
    NULL,                             /* 24 -- */
    "custom-media-type-canon-43",     /* 25 GREETING_CARD             [推] */
    "custom-media-type-canon-45",     /* 26 CARDSTOCK                 [推] */
    NULL,                             /* 27 -- */
    NULL,                             /* 28 -- */
    "custom-media-type-canon-49",     /* 29 PHOTO_PAPER_PRO_CRYSTAL_GRADE [推] */
};

/* ------------------------------------------------------------------ */
/*  布尔 / 色彩（索引 = 枚举值，来源 cnclcmdutilsdef.h）               */
/* ------------------------------------------------------------------ */
static const char *const CNCL_ONOFF_TBL[] = { NULL, "OFF", "ON" };          /* 1/2 [实测] */
static const char *const CNCL_COLOR_TBL[] = { NULL, "color", "monochrome" }; /* 1/2 [实测] */

/* ------------------------------------------------------------------ */
/*  查表：ID 越界 / 空槽 -> NULL                                       */
/* ------------------------------------------------------------------ */
static inline const char *cncl_paper_tbl_get(int id)
{
    if (id < 0) {
        return NULL;
    }
    if (id >= CNCL_PAPER_EXT_BASE) {
        int k = id - CNCL_PAPER_EXT_BASE;
        if (k >= CNCL_PAPER_EXT_MAX) {
            return NULL;
        }
        return CNCL_PAPER_EXT_TBL[k];
    }
    if ((size_t)id >= sizeof(CNCL_PAPER_TBL) / sizeof(CNCL_PAPER_TBL[0])) {
        return NULL;
    }
    return CNCL_PAPER_TBL[id];
}

static inline const char *cncl_media_tbl_get(int id)
{
    if (id < 0 || (size_t)id >= sizeof(CNCL_MEDIA_TBL) / sizeof(CNCL_MEDIA_TBL[0])) {
        return NULL;
    }
    return CNCL_MEDIA_TBL[id];
}

static inline const char *cncl_onoff_tbl_get(int id)
{
    if (id < 1 || id > 2) {
        return NULL;
    }
    return CNCL_ONOFF_TBL[id];
}

static inline const char *cncl_color_tbl_get(int id)
{
    if (id < 1 || id > 2) {
        return NULL;
    }
    return CNCL_COLOR_TBL[id];
}

#endif /* CNCL_TABLES_H */
