/* 验证原生实现：解码结果应与 Python 侧、以及原厂库一致 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int CNCL_GetStringWithTagFromFile(const char *, const char *, int, uint8_t **);
int CNCL_DecodeFromString(const char *, size_t, uint8_t *, size_t);
int CNCL_GetProtocol(const char *, size_t);

int main(int argc, char **argv)
{
    const char *ppd = (argc > 1) ? argv[1] : "/usr/share/cups/model/canong3010.ppd";

    const char *tags[] = { "CNIJ-DEVCE-INFO", "CNIJ-IVEC-CAPABILITY" };
    for (int t = 0; t < 2; t++) {
        uint8_t *buf = NULL;
        int n = CNCL_GetStringWithTagFromFile(ppd, tags[t], 1, &buf);
        printf("=== %s ===\n", tags[t]);
        printf("  返回(编码串长度) = %d\n", n);
        if (!buf) { printf("  !! 返回空\n"); continue; }
        size_t shown = (n > 2 && n < 300) ? (size_t)n : 260;
        printf("  前 %zu 字节(转义): ", shown);
        for (size_t i = 0; i < shown; i++) {
            unsigned char c = buf[i];
            if (i == 2 && t == 0) printf(" | ");
            if (c >= 32 && c < 127) putchar(c);
            else printf("\\x%02x", c);
        }
        printf("\n");

        /* 用解码结果识别协议 */
        printf("  CNCL_GetProtocol(前 2 字节之后) = %d\n",
               CNCL_GetProtocol((const char *)buf + 2, (size_t)(n - 2)));
        free(buf);
        printf("\n");
    }
    return 0;
}
