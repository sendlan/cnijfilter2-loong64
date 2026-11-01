/*
 *  Canon Inkjet Printer Driver for Linux
 *  Copyright CANON INC. 2001-2024
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
 */

//
//  '2004.05.25
//  Using the original "usb.c" code of the CUPS 1.1.14 to create a new
//  backend for writing the printing data into a named pipe.
//

/*
 *
 *   USB port backend for the Common UNIX Printing System (CUPS).
 *
 *   Copyright 1997-2002 by Easy Software Products, all rights reserved.
 *
 *   These coded instructions, statements, and computer programs are the
 *   property of Easy Software Products and are protected by Federal
 *   copyright law.  Distribution and use rights are outlined in the file
 *   "LICENSE" which should have been included with this file.  If this
 *   file is missing or damaged please contact Easy Software Products
 *   at:
 *
 *       Attn: CUPS Licensing Information
 *       Easy Software Products
 *       44141 Airport View Drive, Suite 204
 *       Hollywood, Maryland 20636-3111 USA
 *
 *       Voice: (301) 373-9603
 *       EMail: cups-info@cups.org
 *         WWW: http://www.cups.org
 *
 * Contents:
 *
 *   main()         - Send a file to the specified USB port.
 *   list_devices() - List all USB devices.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

//#include <cups/cups.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "cnijbe.h"

/*
 * 设备枚举（argc==1）的网络发现：源码级复用 cnnet/cnnet_native.c 的
 * cn_snmp_discover（SNMP v1 单播扫描，community canon_admin）。
 * 直接 include 以便 cnijbe2 保持零外部库依赖（DT_NEEDED 仍只有 libc），
 * 构建时需 -I../lgmon3/src/common（libcnnet2_type.h）。
 * 发现: 打印 = TCP 9100 由 lgmon3 经 libcnnet2 完成，此处只负责枚举。
 */
#include "../../cnnet/cnnet_native.c"

#define CNIJ_LGMON2_PATH	PROG_PATH "/cnijlgmon3"
// #define _DEBUG_MODE_

/* 列出网内的 Canon 网络打印机，按 CUPS backend 协议输出设备行。
 * URI 约定与 lgmon3 的解析（cnijcomif.c）严格对齐：
 *   cnijbe2://Canon/?port=net&serial=<12位大写MAC>
 * serial 字段承载 MAC —— lgmon3 作业期拿它经 CNNL_GetIPAddressEx
 * （ARP -> 缓存）定位打印机 IP。 */
/* 发现结果缓存：GUI（print-manager）发起的 CUPS-Get-Devices 带有较短
 * 超时，冷扫描（约 4s）会被掐掉导致"添加打印机"里看不到网络设备。
 * URI 以 MAC 定位设备，打印机换 IP 不失效，故缓存可安全复用。 */
#define CNIJ_DISCOVER_CACHE	"/var/cache/cnijbe2-devices"
#define CNIJ_DISCOVER_TMO_MS	3000
#define CNIJ_CACHE_TTL		3600	/* 秒 */

/* 输出一台设备的 CUPS 设备行（stdout 与缓存文件共用）
 *
 * scheme 必须是 cnijbe://（全小写字母）：KDE print-manager 用正则
 * [a-z]+:// 判断"是否为可发现的网络设备"，带数字的 cnijbe2:// 会被
 * 归入 Manual 类别而在"添加打印机"默认视图里被过滤掉。
 * CUPS 按 scheme 找同名 backend，故包内另有 backend/cnijbe -> cnijbe2
 * 的符号链接；作业期 cnijlgmon3 对两种 scheme 都解析。 */
static void emit_dev(FILE *out, const cn_canon_dev *d)
{
	fprintf(out, "network cnijbe://Canon/?port=net&serial=%s "
		     "\"Canon %s\" \"Canon %s (network)\" "
		     "\"MFG:Canon;MDL:%s;CLS:PRINTER;DES:Canon IJ Printer;\"\n",
	       d->mac, d->model, d->model, d->model);
}

/* 把缓存文件原样搬到 stdout */
static void cache_dump(void)
{
	FILE *fp = fopen(CNIJ_DISCOVER_CACHE, "r");
	char buf[512];

	if (!fp)
		return;
	while (fgets(buf, sizeof(buf), fp))
		fputs(buf, stdout);
	fclose(fp);
}

static void list_devices(void)
{
	cn_canon_dev devs[8];
	int n, i, j;
	struct stat st;
	FILE *fp;

	/* 快速路径：1 小时内扫过 -> 立即输出，枚举零等待 */
	if (stat(CNIJ_DISCOVER_CACHE, &st) == 0 &&
	    time(NULL) - st.st_mtime < CNIJ_CACHE_TTL) {
		cache_dump();
		return;
	}

	/* 冷路径：单播 SNMP 扫描（窗口 3s，控制在上层超时内） */
	n = cn_snmp_discover(devs, 8, CNIJ_DISCOVER_TMO_MS);
	fp = (n > 0) ? fopen(CNIJ_DISCOVER_CACHE ".new", "w") : NULL;
	for (i = 0; i < n; i++) {
		if (devs[i].mac[0] == '\0')
			continue;
		/* 多网卡同网段时同一台打印机会被探测两遍，按 MAC 去重 */
		for (j = 0; j < i; j++) {
			if (!strcmp(devs[i].mac, devs[j].mac))
				break;
		}
		if (j < i)
			continue;
		emit_dev(stdout, &devs[i]);
		if (fp)
			emit_dev(fp, &devs[i]);
	}
	if (fp) {
		fclose(fp);
		rename(CNIJ_DISCOVER_CACHE ".new", CNIJ_DISCOVER_CACHE);
	} else if (n == 0) {
		/* 本次没扫到（打印机睡眠/离线）：把旧缓存兜底输出 */
		cache_dump();
	}
}

pid_t pid = -1;

/*
 * 'main()' - Send a file to the specified USB port.
 *
 * Usage:
 *
 *    printer-uri job-id user title copies options [file]
 */

int			/* O - Exit status */
main(int  argc,		/* I - Number of command-line arguments (6 or 7) */
     char *argv[])	/* I - Command-line arguments */
{
	//FILE *fp;
	//char	resource[1024],			/* Resource info (device and options) */
	//		*options;				/* Pointer to options 				*/
	char copies[2];					/* Number of copies to print 		*/
	char printer_uri[64];
	int status = 0;
	//char discover[2] = "0";

	CNIF_SetSignal(SIGTERM);
 /*
  * Make sure status messages are not buffered...
  */

	setbuf(stderr, NULL);
	memset(printer_uri, 0, sizeof(printer_uri));
	/*
	* Check command-line...
	*/
	if (argc == 1) {
		/* 枚举模式（CUPS cups-deviced 调用）：输出网内 Canon 网络打印机 */
		list_devices();
		return (0);
	} else if (argc < 6 || argc > 7){
		fputs("ERROR: Illegal argument.\n", stderr);
		fputs("Usage: cnijbe\n", stderr);
		return (1);
	} else if(strstr(argv[0], "port") == NULL) {
		fputs("ERROR: Illegal argument.\n", stderr);
		fputs("Usage: cnijbe\n", stderr);
		return (1);
	}
	
	strncpy(printer_uri, argv[0], sizeof(printer_uri));

	/*
	* If we have 7 arguments, print the file named on the command-line.
	* Otherwise, send stdin instead...
	*/

	if (argc == 6) {
		strncpy(copies, "1", sizeof(copies));
	} else if (argc == 7) {
		/*
		* Try to open the print file...
		*/

		if ((stdin = fopen(argv[6], "rb")) == NULL)
		{
#ifdef _DEBUG_MODE_
			perror("ERROR: unable to open print file\n");
#endif
			return (1);
		}

		strncpy(copies, argv[4], sizeof(copies));
	}
	
	int err = 0;
	/* Divid Process */
	pid = fork();
	
	if (pid < 0){
#ifdef _DEBUG_MODE_
		fprintf(stderr, "ERROR: cnjibe fork error\n");
#endif
		exit(1);
	}
	
	if (pid > 0){
		close(STDIN_FILENO);
		waitpid(pid, &status, 0);
#ifdef _DEBUG_MODE_
		fprintf(stderr, "DEBUG: -------------Write process end\n");
#endif
		if (WIFEXITED(status)){
			err = 0;
			err = WEXITSTATUS(status);
			if (err == 0) {
				
#ifdef _DEBUG_MODE_
				fprintf(stderr, "DEBUG: cnijlgmon3 normal exit\n");
#endif
			} else {
#ifdef _DEBUG_MODE_
				fprintf(stderr, "DEBUG: cnijlgmon3 exit error.(%d)\n", err);
#endif
			}
		} else if (WIFSIGNALED(status)){
#ifdef _DEBUG_MODE_
			fprintf(stderr, "DEBUG: cnijlgmon3 signal, sig = %d\n", WTERMSIG(status));
#endif
		} else {
#ifdef _DEBUG_MODE_
			fprintf(stderr, "DEBUG: cnijlgmon3 other err\n");
#endif
		}
	} else {
#ifdef _DEBUG_MODE_
		fprintf(stderr, "DEBUG: Write process start--------------\n");
#endif
		execl(CNIJ_LGMON2_PATH, "cnijlgmon3", printer_uri, copies, argv[5], argv[1], NULL);
	}
	
	return 0;

}

/* set signal */
void CNIF_SetSignal(int sign)
{
	
#if defined(HAVE_SIGACTION) && !defined(HAVE_SIGSET)
    struct  sigaction action;			/* Actions for POSIX signals        */
#endif /* HAVE_SIGACTION && !HAVE_SIGSET */

#ifdef HAVE_SIGSET
	//sigset
	sigset(SIGPIPE, SIG_IGN);
	sigset(sign, CNIF_SigCatch);
	
#elif defined(HAVE_SIGACTION)
	//sigaction
	memset(&action, 0, sizeof(action));
	action.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &action, NULL);
	
	sigemptyset(&action.sa_mask);
	sigaddset(&action.sa_mask, SIGTERM);
	
	action.sa_handler = CNIF_SigCatch;
	sigaction(sign, &action, NULL);
	
	//signal
#else
	signal(SIGPIPE, SIG_IGN);
	if (SIG_ERR == signal(sign, CNIF_SigCatch)) {        
		fprintf(stderr, "DEBUG: failed to set signal handler\n"); 
		exit(1); 
	}
#endif

}

//signal catch
void CNIF_SigCatch(int sign)
{
	fprintf(stderr, "DEBUG: cnijbe_process catch the interrupt signal %d\n", sign);//DEBUG_MSG
	if( pid > 0 ) {
		kill(pid, SIGTERM);
	}
}

