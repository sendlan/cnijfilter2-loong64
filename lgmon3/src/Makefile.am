## Process this file with automake to produce Makefile.in

#cnijlgmon2_bindir = /usr/bin
bin_PROGRAMS = cnijlgmon3
transform = 

cnijlgmon3_SOURCES = cnijcomif.c cnijifusb.c cnijlgmon3.c cnijifnet.c keytext.c cnijifnet2.c cnijutil.c

# -fcommon：本代码依赖 tentative definition 合并（GET_PROTOCOL 等全局变量在
# cnijlgmon3.c 与 cnijifnet2.c 中各写了一次）。gcc 10 起默认 -fno-common，
# 会导致链接期 "multiple definition of `GET_PROTOCOL'"。显式回到 -fcommon。
# 注：官方 6.20/6.90 均无此开关（其目标发行版 gcc 较老，默认仍是 -fcommon）。
CFLAGS = -O2 -Wall -fcommon -I/usr/include/libusb-1.0 -I./common @XML2_CFLAGS@

# 不再链接 -lxml2：keytext.c 已改为运行时 dlopen 探测 soname
# （libxml2.so.2 / .so.16），硬链接会在 soname 分叉的发行版上加载失败。
cnijlgmon3_LDADD = -ldl -lusb-1.0 -lcnnet2 -lcnbpcnclapicom2 -lcnbpnet30
