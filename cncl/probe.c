#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>

int CNCL_GetStringWithTagFromFile(const char*, const char*, int, uint8_t**);
int CNCL_ParseCapabilityResponsePrint_HostEnv(void*, unsigned int);
int CNCL_ParseCapabilityResponsePrint_DateTime(void*, int);
int CNCL_GetProtocol(const char*, size_t);

static void probe(const char *tag)
{
    uint8_t *b = NULL;
    int n = CNCL_GetStringWithTagFromFile("/usr/share/cups/model/canong3010.ppd", tag, 1, &b);
    printf("=== %s ===\n  解码长度 = %d\n", tag, n);
    if (!b) { printf("  空\n"); return; }
    printf("  头 60 : %.60s\n", (char*)b);
    printf("  尾 60 : %.60s\n", (char*)b + (n > 60 ? n - 60 : 0));
    char *p = (char*)memmem(b, n < 0 ? 0 : n, "host_environment", 16);
    printf("  memmem(host_environment) 偏移 = %ld\n", p ? (long)(p - (char*)b) : -1L);
    char *q = (char*)memmem(b, n < 0 ? 0 : n, "datetime", 8);
    printf("  memmem(datetime) 偏移 = %ld\n", q ? (long)(q - (char*)b) : -1L);
    if (strcmp(tag, "CNIJ-DEVCE-INFO") == 0) {
        printf("  GetProtocol = %d\n", CNCL_GetProtocol((const char*)b, (size_t)n));
    } else {
        printf("  HostEnv = %d\n", CNCL_ParseCapabilityResponsePrint_HostEnv(b, (unsigned)n));
        printf("  DateTime = %d\n", CNCL_ParseCapabilityResponsePrint_DateTime(b, n));
    }
    printf("\n");
}

int main(void)
{
    probe("CNIJ-DEVCE-INFO");
    probe("CNIJ-IVEC-CAPABILITY");
    probe("CNIJ-IVEC-CAPABILITY-MAINTENANCE");
    return 0;
}
