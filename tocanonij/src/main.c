/*
 *  CUPS add-on module for Canon Inkjet Printer.
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
#include <stdint.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <dlfcn.h>
#include <errno.h>
#include <time.h>
//#include "cncl.h"
#include "cnclcmdutils.h"
#include "cnclcmdutilsdef.h"
#include "cndata_def.h"
#include "com_def.h"
#include "cncl_paramtbl.h"

#define CN_LIB_PATH_LEN 512
#define CN_CNCL_LIBNAME "libcnbpcnclapicom2.so"
#define TMP_BUF_SIZE 256
#define IS_NUMBER(c)	(c >= '0' && c <= '9')
// #define UUID_LEN	(37)

#define CNIJ_TEMP "/var/tmp/cnijcachetmpXXXXXX"
#define OPTION_TRUE "true"

#define COLOR_MODE_COUNT 2

// #include "ivec.h"
#include "cnijutil.h"

int (*CNCL_GetString)(const char*, const char*, int, uint8_t**);
int WriteHeader(int fd, char jobID[], char uuid[], CNCL_P_SETTINGSPTR Settings, CAPABILITY_DATA capability);
int WriteData(int in_fd, int out_fds[], char jobID[], enum ColorMode *jobColorMode);
int WritePages(int in_fd, int out_fd, char jobID[]);
int WriteTail(int out_fd, char jobID[]);
void CreateCacheFile(int out_fds[]);
int ReplayCacheFile(int in_fd, int out_fd);
int GetJobId(char jobID[], CAPABILITY_DATA capability);

enum {
	OPT_VERSION = 0,
	OPT_FILTERPATH,
	OPT_PAPERSIZE,
	OPT_MEDIATYPE,
	OPT_BORDERLESSPRINT,
	OPT_COLORMODE,
	OPT_DUPLEXPRINT,
	OPT_JOBID,
	OPT_UUID,
	OPT_ROTATE180,
	OPT_OPTIMIZATION
};

static int is_size_X(char *str)
{
	int is_size = 1;

	while( *str && is_size )
	{
		if( *str == '.' ) break;	/* Ver.2.90 */

		switch( is_size )
		{
		case 1:
			if( IS_NUMBER(*str) )
				is_size = 2;
			else
				is_size = 0;
			break;
		case 2:
			if( *str == 'X' )
				is_size = 3;
			else if( !IS_NUMBER(*str) )
				is_size = 0;
			break;
		case 3:
		case 4:
			if( IS_NUMBER(*str) )
				is_size = 4;
			else
				is_size = 0;
			break;

		}
		str++;
	}

	return (is_size == 4)? 1 : 0;
}

static void to_lower_except_size_X(char *str)
{
	if( !is_size_X(str) )
	{
		while( *str )
		{
			if( *str >= 'A' && *str <= 'Z' )
				*str = *str - 'A' + 'a';
			str++;
		}
	}
}

static long ConvertStrToID( const char *str, const MapTbl *tbl )
{
	int result = -1;
	const MapTbl *cur = tbl;
	char srcBuf[TMP_BUF_SIZE];
	char dstBuf[TMP_BUF_SIZE];

	if ( cur == NULL ) goto onErr;

	while( cur->optNum != -1 ){
		strncpy( srcBuf, str, TMP_BUF_SIZE ); srcBuf[TMP_BUF_SIZE-1] = '\0';
		to_lower_except_size_X(srcBuf);

		strncpy( dstBuf, cur->optName, TMP_BUF_SIZE ); dstBuf[TMP_BUF_SIZE-1] = '\0';
		to_lower_except_size_X(dstBuf);

		if ( !strcmp( dstBuf, srcBuf ) ){
			result = cur->optNum;
			break;
		}
		cur++;
	}

onErr:
	return result;
}

static int IsBorderless( const char *str )
{
	int result = 0;

	if ( str == NULL ) goto onErr;
	
	if ( strstr( str, ".bl" ) != NULL ) {
		result = 1;
	}

onErr:
	return result;
}



static void InitpSettings( CNCL_P_SETTINGS *pSettings )
{
	if ( pSettings == NULL ) return;

	pSettings->version 			= 0;
	pSettings->papersize		= -1;
	pSettings->mediatype		= -1;
	pSettings->borderlessprint	= -1;
	pSettings->colormode		= -1;
	pSettings->duplexprint		= -1;
}

static int DumpSettings( CNCL_P_SETTINGS *pSettings )
{
	int result = -1;

	DEBUG_PRINT( "[tocanonij] !!!----------------------!!!\n" );
	DEBUG_PRINT2( "[tocanonij] papersize : %d\n", pSettings->papersize );
	DEBUG_PRINT2( "[tocanonij] mediatype : %d\n", pSettings->mediatype );
	DEBUG_PRINT2( "[tocanonij] borderlessprint : %d\n", pSettings->borderlessprint );
	DEBUG_PRINT2( "[tocanonij] colormode : %d\n", pSettings->colormode );
	DEBUG_PRINT2( "[tocanonij] duplexprint : %d\n", pSettings->duplexprint );
	DEBUG_PRINT( "[tocanonij] !!!----------------------!!!\n" );

	result = 0;
	return result;
}

static int CheckSettings( CNCL_P_SETTINGS *pSettings )
{
	int result = -1;

	if ( (pSettings->papersize == -1) ||
		 (pSettings->mediatype == -1) ||
		 (pSettings->borderlessprint == -1) ||
		 (pSettings->colormode == -1) ||
		 (pSettings->duplexprint == -1)  ){
		goto onErr;
	}

	result = 0;
onErr:
	return result;
}


/* define CNCL API */
static int (*GETSETCONFIGURATIONCOMMAND)( CNCL_P_SETTINGSPTR, char *, long ,void *, long, char *, long * );
static int (*GETSENDDATAPWGRASTERCOMMAND)( char *, long, long, char *, long * );
static int (*GETPRINTCOMMAND)( char *, long, long *, char *, long );
static int (*GETSTRINGWITHTAGFROMFILE)( const char* , const char* , int* , uint8_t** );
static int (*GETSETPAGECONFIGUARTIONCOMMAND)( const char* , unsigned short , void * , long, long * );
static int (*MAKEBJLSETTIMEJOB)( void*, size_t, size_t* );
static int (*GetProtocol)(char *, size_t);
static int (*ParseCapabilityResponsePrint_HostEnv)(void *, int);
static int (*MakeCommand_StartJob3)(int, char *, char[], void *, int, int *);
static int (*ParseCapabilityResponsePrint_DateTime)(void *, int);
static int (*MakeCommand_SetJobConfiguration)(char[], char[], void *, int, int *);


/* CN_START_JOBID */
#define CN_BUFSIZE				(1024 * 256)
#define CN_START_JOBID			("00000001")
#define CN_START_JOBID2			("00000002")
#define CN_START_JOBID_LEN		(9)

// #define DEBUG_LOG

int OutputSetTime( int fd, char *jobID )
{
	long bufSize, retSize, writtenSize;
	char *bufTop = NULL;
	int result  = -1;

	/* Allocate Buffer */
	bufSize = sizeof(char) * CN_BUFSIZE;
	if ( (bufTop = malloc( bufSize )) == NULL ) goto onErr;

	/* StartJob1 */
	if ( GETPRINTCOMMAND == NULL ) goto onErr;
	if ( GETPRINTCOMMAND( bufTop, bufSize, &writtenSize, jobID, CNCL_COMMAND_START1 ) != 0 ) {
		fprintf( stderr, "Error in OutputSetTime\n" );
		goto onErr;
	}
	if ( (retSize = write( fd, bufTop, writtenSize )) != writtenSize ) goto onErr;

	/* StartJob2 */
	if ( GETPRINTCOMMAND( bufTop, bufSize, &writtenSize, jobID, CNCL_COMMAND_START2 ) != 0 ) {
		fprintf( stderr, "Error in OutputSetTime\n" );
		goto onErr;
	}
	if ( (retSize = write( fd, bufTop, writtenSize )) != writtenSize ) goto onErr;

	/* SetTime */
	if ( MAKEBJLSETTIMEJOB == NULL ) goto onErr;
	if ( MAKEBJLSETTIMEJOB( bufTop, (size_t)bufSize, (size_t *)&writtenSize ) != 0 ) {
		fprintf( stderr, "Error in OutputSetTime\n" );
		goto onErr;
	}
	if ( (retSize = write( fd, bufTop, writtenSize )) != writtenSize ) goto onErr;

	/* EndJob */
	if ( GETPRINTCOMMAND( bufTop, bufSize, &writtenSize, jobID, CNCL_COMMAND_END ) != 0 ) {
		fprintf( stderr, "Error in OutputSetTime\n" );
		goto onErr;
	}
	if ( (retSize = write( fd, bufTop, writtenSize )) != writtenSize ) goto onErr;

	result = 0;
onErr:
	if ( bufTop != NULL ) {
		free( bufTop );
	}
	return result;
}

int WriteHeader(int fd, char jobID[], char uuid[], CNCL_P_SETTINGSPTR Settings, CAPABILITY_DATA capability)
{
	DEBUG_PRINT( "[tocanonij] WriteHeader\n");
	uint8_t *xmlBuf = NULL;
	int xmlBufSize;
	int writtenSize = 0;
	long writtenSize_long = 0;
	const char *p_ppd_name = getenv("PPD");

	long bufSize = sizeof(char) * CN_BUFSIZE;
	char *bufTop = NULL;
	char *tmpBuf = NULL;

	if ( (bufTop = malloc( bufSize )) == NULL ){
		return -1;
	}

	int prot = GetProtocol( (char *)capability.deviceID, capability.deviceIDLength );

	if( prot == 2 ){
		xmlBufSize = GETSTRINGWITHTAGFROMFILE( p_ppd_name, CNCL_FILE_TAG_CAPABILITY, (int *)CNCL_DECODE_EXEC, &xmlBuf );

		unsigned short hostEnv = 0;
		hostEnv = ParseCapabilityResponsePrint_HostEnv( xmlBuf, xmlBufSize );

		/* Write StartJob Command */
		int ret = 0;
		ret = MakeCommand_StartJob3( hostEnv, uuid, jobID, bufTop, bufSize, &writtenSize );

		if ( ret != 0 ) {
			fprintf( stderr, "Error in CNCL_GetPrintCommand\n" );
			free(bufTop);
			return -1;
		}

		/* WriteData */
		if (  write( fd, bufTop, writtenSize ) != writtenSize ){
			free(bufTop);
			return -1;
		} 

		char dateTime[15];
		memset(dateTime, '\0', sizeof(dateTime));

		ret = ParseCapabilityResponsePrint_DateTime( xmlBuf, xmlBufSize );

		if( ret == 2 ){
			time_t timer = time(NULL);
			struct tm *date = localtime(&timer);

			sprintf(dateTime, "%d%02d%02d%02d%02d%02d",
				date->tm_year+1900, date->tm_mon+1, date->tm_mday,
				date->tm_hour, date->tm_min, date->tm_sec);

			if ( (tmpBuf = malloc( bufSize )) == NULL ){
				free(tmpBuf);
				return -1;
			}

			MakeCommand_SetJobConfiguration( jobID, dateTime, tmpBuf, bufSize, &writtenSize );

			/* WriteData */
			if ( write( fd, tmpBuf, writtenSize ) != writtenSize ){
				free(bufTop);
				free(tmpBuf);
				return -1;
			}
			
			free(tmpBuf);
			// writtenSize += tmpWrittenSize;
		}
	}
	else{
		/* OutputSetTime */
		if ( OutputSetTime( fd, jobID ) != 0 ){
			free(bufTop);
			return -1;
		}

		/* Write StartJob Command */
		if ( GETPRINTCOMMAND( bufTop, bufSize, &writtenSize_long, jobID, CNCL_COMMAND_START1 ) != 0 ) {
			fprintf( stderr, "Error in CNCL_GetPrintCommand\n" );
			free(bufTop);
			return -1;
		}

		/* WriteData */
		if ( write( fd, bufTop, writtenSize_long ) != writtenSize_long ){
			free(bufTop);
			return -1;
		}
	}

	/* Write SetConfiguration Command */
	if ( (xmlBufSize = GETSTRINGWITHTAGFROMFILE( p_ppd_name, CNCL_FILE_TAG_CAPABILITY, (int*)CNCL_DECODE_EXEC, &xmlBuf )) < 0 ){
		DEBUG_PRINT2( "[tocanonij] p_ppd_name : %s\n", p_ppd_name );
		DEBUG_PRINT2( "[tocanonij] xmlBufSize : %d\n", xmlBufSize );
		fprintf( stderr, "Error in CNCL_GetStringWithTagFromFile\n" );
		free(bufTop);
		return -1;
	}

	if ( GETSETCONFIGURATIONCOMMAND( Settings, jobID, bufSize, (void *)xmlBuf, xmlBufSize, bufTop, &writtenSize_long ) != 0 ){
		fprintf( stderr, "Error in CNCL_GetSetConfigurationCommand\n" );
		free(bufTop);
		return -1;
	}
	/* WriteData */
	write( fd, bufTop, writtenSize_long );
	free(bufTop);
	return 0;
}	

int WriteData(int in_fd, int out_fds[], char jobID[], enum ColorMode *jobColorMode)
{
	long bufSize = sizeof(char) * CN_BUFSIZE;
	char *bufTop = NULL;
	long writtenSize_long = 0;

	if ( (bufTop = malloc( bufSize )) == NULL ){
		return -1;
	}

	while ( 1 ) {
		int readBytes = 0;
		int writeBytes;
		CNDATA CNData;
		long readSize = 0;
		unsigned short next_page;
		int out_fd = -1;

		memset( &CNData, 0, sizeof(CNDATA) );

		/* read magic number */
		readBytes = read( in_fd, &CNData, sizeof(CNDATA) );
		if ( readBytes > 0 ){
			if ( CNData.magic_num != MAGIC_NUMBER_FOR_CNIJPWG ){
				fprintf( stderr, "Error illeagal MagicNumber\n" );
				free(bufTop);
				return -1;
			}
			if ( CNData.image_size < 0 ){
				fprintf( stderr, "Error illeagal dataSize\n" );
				free(bufTop);
				return -1;
			}
		}
		else if ( readBytes < 0 ){
			if ( errno == EINTR ) continue;
			fprintf( stderr, "DEBUG:[tocanonij] tocnij read error, %d\n", errno );
			free(bufTop);
			return -1;
		}
		else {
			DEBUG_PRINT( "DEBUG:[tocanonij] !!!DATA END!!!\n" );
			break; /* data end */
		}

		if(jobColorMode != NULL) {
			*jobColorMode = CNData.jobColorMode;
		}	


		if(CNData.pageColorMode == COLOR_MODE_COLOR) {
			out_fd = out_fds[0];
		} else if(CNData.pageColorMode == COLOR_MODE_GRAY) {
			out_fd = out_fds[1];
		}

		/* Write Next Page Info */
		if ( CNData.next_page ) {
			next_page = CNCL_PSET_NEXTPAGE_ON;
		}
		else {
			next_page = CNCL_PSET_NEXTPAGE_OFF; 
		}
		if ( GETSETPAGECONFIGUARTIONCOMMAND( jobID, next_page, bufTop, bufSize, &writtenSize_long ) != 0 ) {
			fprintf( stderr, "Error in CNCL_GetPrintCommand\n" );
			free(bufTop);
			return -1;
		}
		
		/* WriteData */
		if ( write( out_fd, bufTop, writtenSize_long) != writtenSize_long ){
			free(bufTop);
			return -1;
		} 

		DEBUG_PRINT( "[tocanonij] Write SendData Command\n");
		/* Write SendData Command */
		memset(	bufTop, 0x00, bufSize );
		readSize = CNData.image_size;
		if ( GETSENDDATAPWGRASTERCOMMAND( jobID, readSize, bufSize, bufTop, &writtenSize_long ) != 0 ) {
			DEBUG_PRINT( "Error in CNCL_GetSendDataJPEGCommand\n" );
			free(bufTop);
			return -1;
		}
		/* WriteData */
		write( out_fd, bufTop, writtenSize_long );

		while( readSize ){
			char *pCurrent = bufTop;

			if ( readSize - bufSize > 0 ){
				readBytes = read( in_fd, bufTop, bufSize );
				DEBUG_PRINT2( "[tocanonij] PASS tocanonij READ1<%d>\n", readBytes );
				if ( readBytes < 0 ) {
					if ( errno == EINTR ) continue;
				}
				readSize -= readBytes;
			}
			else {
				readBytes = read( in_fd, bufTop, readSize );
				DEBUG_PRINT2( "[tocanonij] PASS tocanonij READ2<%d>\n", readBytes );
				if ( readBytes < 0 ) {
					if ( errno == EINTR ) continue;
				}
				readSize -= readBytes;
			}

			do {
				writeBytes = write( out_fd, pCurrent, readBytes );
				DEBUG_PRINT2( "[tocanonij] PASS tocanonij WRITE<%d>\n", writeBytes );
				if( writeBytes < 0){
					if ( errno == EINTR ) continue;
					free(bufTop);
					return -1;
				}
				readBytes -= writeBytes;
				pCurrent += writeBytes;
			} while( writeBytes > 0 );
		}
	}

	free(bufTop);

	return 0;
}

int WritePages(int in_fd, int out_fd, char jobID[])
{
	DEBUG_PRINT( "[tocanonij] WritePages\n" );
	int out_fds[2] = { out_fd, out_fd };
	return WriteData(in_fd, out_fds, jobID, NULL);
}

int WriteCacheFile(int in_fd, int out_fds[], char jobID[], enum ColorMode *jobColorMode)
{
	DEBUG_PRINT( "[tocanonij] WriteCacheFile\n" );
	return WriteData(in_fd, out_fds, jobID, jobColorMode);
}

int WriteTail(int out_fd, char jobID[])
{
	DEBUG_PRINT( "[tocanonij] WriteTail\n");
	long bufSize = sizeof(char) * CN_BUFSIZE;
	char *bufTop = NULL;
	int retSize = 0;
	long writtenSize_long = 0;

	if ( (bufTop = malloc( bufSize )) == NULL ){
		DEBUG_PRINT( "Error in malloc\n" );
		return -1;
	}

	/* CNCL_GetPrintCommand */
	if ( GETPRINTCOMMAND( bufTop, bufSize, &writtenSize_long, jobID, CNCL_COMMAND_END ) != 0 ) {
		DEBUG_PRINT( "Error in CNCL_GetPrintCommand\n" );
		free(bufTop);
		return -1;
	}
	/* WriteData */
	retSize = write( out_fd, bufTop, writtenSize_long );
	if(retSize == 0){
		DEBUG_PRINT( "Error in WriteData\n" );
		free(bufTop);
		return -1;
	}
	DEBUG_PRINT( "[tocanonij] to_cnijf <end>\n" );

	free(bufTop);
	return 0;
}

void CreateCacheFile(int out_fds[])
{
	DEBUG_PRINT( "[tocanonij] CreateCacheFile\n" );
	for(int i = 0; i < COLOR_MODE_COUNT; i++){
		char tmpName[64];
		strncpy( tmpName, CNIJ_TEMP, 64 );
		out_fds[i] = mkstemp( tmpName );
		if(out_fds[i] != -1){
			unlink( tmpName );
		}
	}
}

int ReplayCacheFile(int in_fd, int out_fd)
{
	DEBUG_PRINT( "[tocanonij] ReplayCacheFile\n");
	long bufSize = sizeof(char) * CN_BUFSIZE;
	char *bufTop = NULL;

	if ( (bufTop = malloc( bufSize )) == NULL ){
		DEBUG_PRINT( "Error in malloc\n" );
		return -1;
	}

	lseek(in_fd, 0, SEEK_SET);
	while(1){
		int readBytes = read( in_fd, bufTop, bufSize );
		if(readBytes <= 0){
			break;
		}

		if(write(out_fd, bufTop, readBytes) == 0){
			DEBUG_PRINT( "Error in WriteData\n" );
			free( bufTop );
			return -1;
		}	
	}
	
	free( bufTop );

	DEBUG_PRINT( "ReplayCacheFile return 0\n" );
	return 0;
}

int GetJobId(char jobID[], CAPABILITY_DATA capability)
{
	//GetJobId
	int prot = GetProtocol( (char *)capability.deviceID, capability.deviceIDLength );
	if(prot == 2){
		/* Set JobID */
		strncpy( jobID, CN_START_JOBID2, CN_START_JOBID_LEN );
	}
	else{
		/* Set JobID */
		strncpy( jobID, CN_START_JOBID, CN_START_JOBID_LEN );
	}

	return 0;
}

int main( int argc, char *argv[] )
{
	int fd = 0;
	int opt, opt_index;
	int result = -1;
	CNCL_P_SETTINGS Settings;
	char jobID[CN_START_JOBID_LEN];
	char libPathBuf[CN_LIB_PATH_LEN];
	void *libclss = NULL;
	struct option long_opt[] = {
		{ "version", required_argument, NULL, OPT_VERSION }, 
		{ "filterpath", required_argument, NULL, OPT_FILTERPATH }, 
		{ "papersize", required_argument, NULL, OPT_PAPERSIZE }, 
		{ "mediatype", required_argument, NULL, OPT_MEDIATYPE }, 
		{ "grayscale", required_argument, NULL, OPT_COLORMODE }, 
		{ "duplexprint", required_argument, NULL, OPT_DUPLEXPRINT }, 
		{ "jobid", required_argument, NULL, OPT_JOBID }, 
		{ "uuid", required_argument, NULL, OPT_UUID }, 
		{ "rotate180", required_argument, NULL, OPT_ROTATE180 },
		{ "optimization", required_argument, NULL, OPT_OPTIMIZATION},
		/* 别名：本仓库 ppd/ 里的 *%CNPpdToOptKey 直接下发 --media / --duplex
		 * （见 ppd/canong3010.ppd 等），而官方 6.90 只认 --mediatype /
		 * --duplexprint。不补这两个别名，MediaType / Duplex 选择会被
		 * getopt_long 静默丢弃，选项根本不生效。
		 * 注：官方 6.90 无此别名。 */
		{ "media", required_argument, NULL, OPT_MEDIATYPE }, 
		{ "duplex", required_argument, NULL, OPT_DUPLEXPRINT }, 
		{ 0, 0, 0, 0 }, 
	};
	const char *p_ppd_name = getenv("PPD");
	CAPABILITY_DATA capability;
	char	uuid[UUID_LEN + 1];

	short optimization = 0;
	enum ColorMode jobColorMode = COLOR_MODE_GRAY;
	int cnijtmp_fd = -1;
	int cnijtmp_fds[2] = { -1, -1 };

	DEBUG_PRINT( "[tocanonij] start tocanonij\n" );

	/* init CNCL API */
	GETSETCONFIGURATIONCOMMAND = NULL;
	GETSENDDATAPWGRASTERCOMMAND = NULL;
	GETPRINTCOMMAND = NULL;
	GETSTRINGWITHTAGFROMFILE = NULL;
	GETSETPAGECONFIGUARTIONCOMMAND = NULL;
	MAKEBJLSETTIMEJOB = NULL;
	GetProtocol = NULL;
	ParseCapabilityResponsePrint_HostEnv=NULL;
	MakeCommand_StartJob3 = NULL;
	ParseCapabilityResponsePrint_DateTime = NULL;
	MakeCommand_SetJobConfiguration = NULL;

	/* Init Settings */
	memset( &Settings, 0x00, sizeof(CNCL_P_SETTINGS) );
	InitpSettings( &Settings );
	memset( uuid, '\0', sizeof(uuid) );

	while( (opt = getopt_long( argc, argv, "0:", long_opt, &opt_index )) != -1) {
		switch( opt ) {
			case OPT_VERSION:
				DEBUG_PRINT3( "[tocanonij] OPTION(%s):VALUE(%s)\n", long_opt[opt_index].name, optarg );
				break;
			case OPT_FILTERPATH:
				DEBUG_PRINT3( "[tocanonij] OPTION(%s):VALUE(%s)\n", long_opt[opt_index].name, optarg );
				snprintf( libPathBuf, CN_LIB_PATH_LEN, "%s%s", optarg, CN_CNCL_LIBNAME );
				break;
			case OPT_PAPERSIZE:
				DEBUG_PRINT3( "[tocanonij] OPTION(%s):VALUE(%s)\n", long_opt[opt_index].name, optarg );
				Settings.papersize = ConvertStrToID( optarg, papersizeTbl );
				if ( IsBorderless( optarg ) ){
					Settings.borderlessprint = CNCL_PSET_BORDERLESS_ON;
				}
				else {
					Settings.borderlessprint = CNCL_PSET_BORDERLESS_OFF;
				}
				break;
			case OPT_MEDIATYPE:
				DEBUG_PRINT3( "[tocanonij] OPTION(%s):VALUE(%s)\n", long_opt[opt_index].name, optarg );
				Settings.mediatype = ConvertStrToID( optarg, mediatypeTbl );
				DEBUG_PRINT2( "[tocanonij] media : %d\n", Settings.mediatype );
				break;
#if 0
			case OPT_BORDERLESSPRINT:
				DEBUG_PRINT3( "[tocanonij] OPTION(%s):VALUE(%s)\n", long_opt[opt_index].name, optarg );
				if ( IsBorderless( optarg ) ){
					Settings.borderlessprint = CNCL_PSET_BORDERLESS_ON;
				}
				else {
					Settings.borderlessprint = CNCL_PSET_BORDERLESS_OFF;
				}
				break;
#endif
			case OPT_COLORMODE:
				DEBUG_PRINT3( "[tocanonij] OPTION(%s):VALUE(%s)\n", long_opt[opt_index].name, optarg );
				Settings.colormode = ConvertStrToID( optarg, colormodeTbl );
				break;
			case OPT_DUPLEXPRINT:
				DEBUG_PRINT3( "[tocanonij] OPTION(%s):VALUE(%s)\n", long_opt[opt_index].name, optarg );
				//Settings.duplexprint = CNCL_PSET_DUPLEX_OFF;
				Settings.duplexprint = ConvertStrToID( optarg, duplexprintTbl);
				break;

			case OPT_UUID:
				strncpy( uuid, optarg, strlen(optarg) );
				break;
			case OPT_JOBID:
				if( strlen( uuid ) == 0 ){
					strncpy( uuid, optarg, strlen(optarg) );
				}
				break;
			case OPT_ROTATE180:  /* ignore this option */
				break;
			case OPT_OPTIMIZATION:
				DEBUG_PRINT3( "[tocanonij] OPTION(%s):VALUE(%s)\n", long_opt[opt_index].name, optarg );
				if(strcasecmp(optarg, OPTION_TRUE) == 0){
					optimization = 1;
				}
				break;
			case '?':
				fprintf( stderr, "Error: invalid option %c:\n", optopt);
				break;
			default:
				break;
		}
	}

	/* dlopen */
	/* Make progamname with path of execute progname. */
	//snprintf( libPathBuf, CN_LIB_PATH_LEN, "%s%s", GetExecProgPath(), CN_CNCL_LIBNAME );
	DEBUG_PRINT2( "[tocanonij] libPath : %s\n", libPathBuf );
	if ( access( libPathBuf, R_OK ) ){
		strncpy( libPathBuf, CN_CNCL_LIBNAME, CN_LIB_PATH_LEN );
	}
	DEBUG_PRINT2( "[tocanonij] libPath : %s\n", libPathBuf );


	libclss = dlopen( libPathBuf, RTLD_LAZY );
	if ( !libclss ) {
		fprintf( stderr, "Error in dlopen\n" );
		goto onErr;
	}

	GETSETCONFIGURATIONCOMMAND = dlsym( libclss, "CNCL_GetSetConfigurationCommand" );
	if ( dlerror() != NULL ) {
		fprintf( stderr, "Error in CNCL_GetSetConfigurationCommand. API not Found.\n" );
		goto onErr;
	}
	GETSENDDATAPWGRASTERCOMMAND = dlsym( libclss, "CNCL_GetSendDataPWGRasterCommand" );
	if ( dlerror() != NULL ) {
		fprintf( stderr, "Error in CNCL_GetSendDataPWGRasterCommand\n" );
		goto onErr;
	}
	GETPRINTCOMMAND = dlsym( libclss, "CNCL_GetPrintCommand" );
	if ( dlerror() != NULL ) {
		fprintf( stderr, "Error in CNCL_GetPrintCommand\n" );
		goto onErr;
	}
	GETSTRINGWITHTAGFROMFILE = dlsym( libclss, "CNCL_GetStringWithTagFromFile" );
	if ( dlerror() != NULL ) {
		fprintf( stderr, "Error in CNCL_GetStringWithTagFromFile\n" );
		goto onErr;
	}
	GETSETPAGECONFIGUARTIONCOMMAND = dlsym( libclss, "CNCL_GetSetPageConfigurationCommand" );
	if ( dlerror() != NULL ) {
		fprintf( stderr, "Load Error in CNCL_GetSetPageConfigurationCommand\n" );
		goto onErr;
	}
	MAKEBJLSETTIMEJOB = dlsym( libclss, "CNCL_MakeBJLSetTimeJob" );
	if ( dlerror() != NULL ) {
		fprintf( stderr, "Load Error in CNCL_MakeBJLSetTimeJob\n" );
		goto onErr;
	}
	GetProtocol = dlsym( libclss, "CNCL_GetProtocol" );
	if ( dlerror() != NULL ) {
		fprintf( stderr, "Load Error in CNCL_MakeBJLSetTimeJob\n" );
		goto onErr;
	}
	ParseCapabilityResponsePrint_HostEnv = dlsym( libclss, "CNCL_ParseCapabilityResponsePrint_HostEnv" );
	if ( dlerror() != NULL ) {
		fprintf( stderr, "Load Error in CNCL_ParseCapabilityResponsePrint_HostEnv\n" );
		goto onErr;
	}
	MakeCommand_StartJob3 = dlsym( libclss, "CNCL_MakeCommand_StartJob3" );
	if ( dlerror() != NULL ) {
		fprintf( stderr, "Load Error in CNCL_MakeCommand_StartJob3\n" );
		goto onErr;
	}
	ParseCapabilityResponsePrint_DateTime = dlsym( libclss, "CNCL_ParseCapabilityResponsePrint_DateTime" );
	if ( dlerror() != NULL ) {
		fprintf( stderr, "Load Error in CNCL_ParseCapabilityResponsePrint_DateTime\n" );
		goto onErr;
	}
	MakeCommand_SetJobConfiguration = dlsym( libclss, "CNCL_MakeCommand_SetJobConfiguration" );
	if ( dlerror() != NULL ) {
		fprintf( stderr, "Load Error in CNCL_MakeCommand_SetJobConfiguration\n" );
		goto onErr;
	}

	/* Check Settings */
	if ( CheckSettings( &Settings ) != 0 ) goto onErr;

#if 1
	/* Dump Settings */
	DumpSettings( &Settings );
#endif

	memset(&capability, '\0', sizeof(CAPABILITY_DATA));
	if( ! GetCapabilityFromPPDFile(p_ppd_name, &capability) ){
		goto onErr;
	}

	/* Get jobID */
	if(GetJobId(jobID, capability) != 0){
		goto onErr;
	}

	if((Settings.colormode == CNCL_PSET_COLORMODE_COLOR) && 
	   (optimization == 1 )){
		/* Create cache file */
		CreateCacheFile(cnijtmp_fds);
		if(cnijtmp_fds[0] == -1 || cnijtmp_fds[1] == -1){
			goto onErr;
		}

		/* Write data to cache file */
		if(WriteCacheFile(fd, cnijtmp_fds, jobID, &jobColorMode) != 0){
			goto onErr;
		}

		if(jobColorMode == COLOR_MODE_COLOR){
			cnijtmp_fd = cnijtmp_fds[0];
		} else if (jobColorMode == COLOR_MODE_GRAY){
			Settings.colormode = CNCL_PSET_COLORMODE_MONO;
			cnijtmp_fd = cnijtmp_fds[1];
		} else{
			goto onErr;
		}
		DEBUG_PRINT2( "[tocanonij] jobColorMode:%d\n", jobColorMode);

		/* Write header data to out port*/
		if(WriteHeader(1, jobID, uuid, &Settings, capability) != 0){
			goto onErr;
		}

		/* Get data from cache file and write it to out port */
		if(ReplayCacheFile(cnijtmp_fd, 1) != 0){
			goto onErr;
		}

		/*Write tail data to out port */
		if(WriteTail(1, jobID) != 0){
			goto onErr;
		}
		
	} else {
		/* Write header data */
		if(WriteHeader(1, jobID, uuid, &Settings, capability) != 0){
			goto onErr;
		}

		/* Write page Data */
		if(WritePages(fd, 1, jobID) != 0){
			goto onErr;
		}
		
		/* Write tail data */
		if(WriteTail(1, jobID) != 0){
			goto onErr;
		}
	}

	result = 0;

onErr:
	if ( libclss != NULL ) {
		dlclose( libclss );
	}

	if(cnijtmp_fds[0] != -1){
		close(cnijtmp_fds[0]);
	}

	if(cnijtmp_fds[1] != -1){
		close(cnijtmp_fds[1]);
	}

	return result;
}


