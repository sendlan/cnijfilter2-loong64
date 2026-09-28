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
 *
 * NOTE:
 *  - As a special exception, this program is permissible to link with the
 *    libraries released as the binary modules.
 *  - If you write modifications of your own for these programs, it is your
 *    choice whether to permit this exception to apply to your modifications.
 *    If you do not wish that, delete this exception.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>

#include "cnijutil.h"
#include "cnijifnet2.h"

#define CN_CNCL_LIB_PATH		("libcnbpcnclapi%s.so")
#define CN_LIB_PATH_LEN 512
#define CN_CNCL_LIBNAME "libcnbpcnclapicom2.so"


static int (*GetStringWithTagFromFile)(const char*, const char*, int, uint8_t**);
// static int (*GetStringWithTagFromFile)(const char*, const char*, int*, uint8_t**);
// static int (*CNCL_GetStringWithTagFromFile)(const char*, const char*, size_t, uint8_t**);



int GetUUID( char *arg , char *uuid ){
	int		ret = CN_NET3_SUCCEEDED;
	char	*cnt = NULL;

	cnt = strstr( arg, UUID_PTN);

	if( cnt != NULL ){
		strncpy( uuid, cnt + sizeof(UUID_PTN) - 1, UUID_LEN );
	}
	else{
		ret = CN_NET3_FAILED;
	}

	return ret;
}


int GetCapabilityFromPPDFile(const char *ppdFileName, CAPABILITY_DATA *_data)
{
	char libPathBuf[CN_LIB_PATH_LEN];
	void *libclss = NULL;
	char model_number[] = "com2";

	// snprintf( libPathBuf, CN_LIB_PATH_LEN, "%s%s", optarg, CN_CNCL_LIBNAME );
	snprintf( libPathBuf, CN_LIB_PATH_LEN - 1, CN_CNCL_LIB_PATH, model_number );
	// snprintf( libPathBuf, CN_LIB_PATH_LEN, "%s", CN_CNCL_LIBNAME );

	libclss = dlopen( libPathBuf, RTLD_LAZY );
	if ( !libclss ) {
		fprintf( stderr, "Error in dlopen\n" );
		goto onErr;
	}

	GetStringWithTagFromFile = NULL;
	GetStringWithTagFromFile = dlsym( libclss, "CNCL_GetStringWithTagFromFile" );

	if ( dlerror() != NULL ) {
		fprintf( stderr, "Error in CNCL_GetSetConfigurationCommand. API not Found.\n" );
		goto onErr;
	}

	// char *buffer = NULL;
	uint8_t *buffer = NULL;

	/* 原版此处是 unsigned int：GetStringWithTagFromFile 失败时返回 -1，
	 * 赋给 unsigned 变成 4294967295，下面 "<= 0" 的守卫失效，随后
	 * for(i=2;i<bufferSize;i++) buffer[i] 会对 NULL 解引用 → SIGSEGV。
	 * 实测：不设 PPD 环境变量直接运行 cnijlgmon3 即崩溃
	 * （dmesg: SIGSEGV ... invalid read access from 0000000000000002）。
	 * 改成有符号类型即可让原有守卫真正生效。
	 * 注：官方 6.90 此处仍为 unsigned int，未修。 */
	int bufferSize = 0;

	bufferSize = GetStringWithTagFromFile(ppdFileName, CNCL_FILE_TAG_DEVICEID, 1, &buffer);

	if( bufferSize <= 0 ){
		return MAKEPPD_FAILED;
	}

	uint8_t *temp = NULL;
	temp = (uint8_t *) malloc(1024);

	if( temp == NULL ){
		goto onErr;
	}

	memset(temp, '\0', 1024);

	int i, j = 0;

	for( i = 2; i < bufferSize; i++ ){
		if( buffer[i] != '\0' ){
			temp[j] = buffer[i];
			j++;
		}
	}

	_data->deviceID = temp;
	_data->deviceIDLength = strlen( (char *)temp );

	return MAKEPPD_SUCCEEDED;

onErr:
	return -1;
}
