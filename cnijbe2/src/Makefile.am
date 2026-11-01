## Process this file with automake to produce Makefile.in

backend_bindir = $(libdir)/cups/backend
backend_bin_PROGRAMS = cnijbe2
transform = 


cnijbe2_SOURCES = cnijbe.c

# cnijbe.c 源码级包含 ../cnnet/cnnet_native.c（设备枚举用），
# 它需要 libcnnet2_type.h（在 lgmon3 的 common 头目录里）
CFLAGS = -O2 -Wall -I$(top_srcdir)/../lgmon3/src/common

cnijbe2_LDADD = -lcups

