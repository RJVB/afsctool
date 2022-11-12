// kate: auto-insert-doxygen true; backspace-indents true; indent-width 4; keep-extra-spaces true; replace-tabs false; tab-indents true; tab-width 4;
/*
 * @file afsctool.c
 * Copyright "brkirch" (https://brkirch.wordpress.com/afsctool/) 
 * Parallel processing modifications and other tweaks (C) 2015-now René J.V. Bertin
 * This code is made available under the GPL3 License
 * (See License.txt)
 */

#ifndef __APPLE__
	#define __USE_BSD
	#ifndef _BSD_SOURCE
	#	define _BSD_SOURCE
	#endif
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <libgen.h>
#include <signal.h>

#include <zlib.h>
#ifdef HAS_LZVN
#	include "private/lzfse/src/lzfse_internal.h"
#endif
#ifdef HAS_LZFSE
#	include "private/lzfse/src/lzfse.h"
#endif

#include <sys/mman.h>

#ifdef __APPLE__
	#include <sys/attr.h>

	#include <CoreFoundation/CoreFoundation.h>
	#include <CoreServices/CoreServices.h>
	#define BlockMutable	__block
#else
	// for cross-platform debugging only!
	#include <bsd/stdlib.h>
	#include <endian.h>
	#include <sys/vfs.h>
	#include <sys/stat.h>
	#include <fcntl.h>
	#define O_EXLOCK 0
	#define HFSPlusAttrKey 0
	#define HFSPlusCatalogFile 0
	#define OSSwapHostToBigInt16(x)		htobe16(x)
	#define OSSwapHostToBigInt32(x)		htobe32(x)
	#define OSSwapHostToLittleInt32(x)	htole32(x)
	#define OSSwapHostToLittleInt64(x)	htole64(x)
	#define OSSwapLittleToHostInt32(x)	le32toh(x)
	#define MAP_NOCACHE 				0
	#define BlockMutable				/**/
#endif

#include "afsctool.h"
#ifdef SUPPORT_PARALLEL
#	include "ParallelProcess.h"
	static ParallelFileProcessor *PP = NULL;
	static bool exclusive_io = true;
#endif
#include "afsctool_fullversion.h"
#include "utils.h"

#define xfree(x)		if((x)){free((x)); (x)=NULL;}
#define xclose(x)		if((x)!=-1){close((x)); (x)=-1;}
#define xmunmap(x,s)	if((x)){munmap((x),(s)); (x)=NULL;}

#if MAC_OS_X_VERSION_MIN_REQUIRED <= MAC_OS_X_VERSION_10_5
	static bool legacy_output = true;
#else
	static bool legacy_output = false;
#endif

// some constants borrowed from libarchive
#define MAX_DECMPFS_XATTR_SIZE	3802	/* max size that can be stored in the xattr. */
#ifndef DECMPFS_MAGIC
#	define DECMPFS_MAGIC		'cmpf'
#endif
#ifndef DECMPFS_XATTR_NAME
#	define DECMPFS_XATTR_NAME	"com.apple.decmpfs"
#endif

// use a hard-coded count so all arrays are always sized equally (and the compiler can warn better)
#define sizeunits 6
const char *sizeunit10_short[sizeunits] = {"KB", "MB", "GB", "TB", "PB", "EB"};
const char *sizeunit10_long[sizeunits] = {"kilobytes", "megabytes", "gigabytes", "terabytes", "petabytes", "exabytes"};
const long long int sizeunit10[sizeunits] = {1000, 1000 * 1000, 1000 * 1000 * 1000, (long long int) 1000 * 1000 * 1000 * 1000,
	(long long int) 1000 * 1000 * 1000 * 1000 * 1000, (long long int) 1000 * 1000 * 1000 * 1000 * 1000 * 1000};
const char *sizeunit2_short[sizeunits] = {"KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
const char *sizeunit2_long[sizeunits] = {"kibibytes", "mebibytes", "gibibytes", "tebibytes", "pebibytes", "exbibytes"};
const long long int sizeunit2[sizeunits] = {1024, 1024 * 1024, 1024 * 1024 * 1024, (long long int) 1024 * 1024 * 1024 * 1024,
	(long long int) 1024 * 1024 * 1024 * 1024 * 1024, (long long int) 1024 * 1024 * 1024 * 1024 * 1024 * 1024};

int printVerbose = 0;
static size_t maxOutBufSize = 0;
void printFileInfo(const char *filepath, struct stat *fileinfo, bool appliedcomp, bool onAPFS);

#if !__has_builtin(__builtin_available)
#	warning "Please use clang 5 or newer if you can"
// determine the Darwin major version number
static int darwinMajor = 0;
#endif

char* getSizeStr(long long int size, long long int size_rounded, int likeFinder)
{
	static char sizeStr[128];
	static int len = sizeof(sizeStr)/sizeof(char);
	int unit2, unit10;
	
	for (unit2 = 0; unit2 + 1 < sizeunits && (size_rounded / sizeunit2[unit2 + 1]) > 0; unit2++);
	for (unit10 = 0; unit10 + 1 < sizeunits && (size_rounded / sizeunit10[unit10 + 1]) > 0; unit10++);

	int remLen = len - snprintf(sizeStr, len, "%lld bytes", size);
	char *cursor = &sizeStr[strlen(sizeStr)];

#ifdef PRINT_SI_SIZES
	int print_si_sizes = 1;
#else
	int print_si_sizes = likeFinder;
#endif
	if (print_si_sizes) {
		// the Finder will happily print "0 bytes on disk" so here we don't bother
		// determining if the human-readable value is > 0.
		switch (unit10)
		{
			case 0:
				snprintf(cursor, remLen, " / %0.0f %s (%s, base-10)",
						 (double) size_rounded / sizeunit10[unit10], sizeunit10_short[unit10], sizeunit10_long[unit10]);
				break;
			case 1:
				snprintf(cursor, remLen, " / %.12g %s (%s, base-10)",
						 (double) (((long long int) ((double) size_rounded / sizeunit10[unit10] * 100) + 5) / 10) / 10,
						 sizeunit10_short[unit10], sizeunit10_long[unit10]);
				break;
			default:
				snprintf(cursor, remLen, " / %0.12g %s (%s, base-10)",
						 (double) (((long long int) ((double) size_rounded / sizeunit10[unit10] * 1000) + 5) / 10) / 100,
						 sizeunit10_short[unit10], sizeunit10_long[unit10]);
				break;
		}
	}
	if (!likeFinder) {
		double humanReadable;
		switch (unit2)
		{
			case 0:
				// this should actually be the only case were we'd need
				// to check if the human readable value is sensical...
				humanReadable = (double) size_rounded / sizeunit2[unit2];
				if( humanReadable >= 1 ) {
					snprintf(cursor, remLen, " / %0.0f %s", humanReadable, sizeunit2_short[unit2]);
				}
				break;
			case 1:
				humanReadable = (double) (((long long int) ((double) size_rounded / sizeunit2[unit2] * 100) + 5) / 10) / 10;
				if( humanReadable > 0 ) {
					snprintf(cursor, remLen, " / %.12g %s", humanReadable, sizeunit2_short[unit2]);
				}
				break;
			default:
				humanReadable = (double) (((long long int) ((double) size_rounded / sizeunit2[unit2] * 1000) + 5) / 10) / 100;
				if( humanReadable > 0 ) {
					snprintf(cursor, remLen, " / %0.12g %s", humanReadable, sizeunit2_short[unit2]);
				}
				break;
		}
	}
	
	return sizeStr;
}

long long int roundToBlkSize(long long int size, struct stat *fileinfo)
{
	if (size <= 0) {
		return size;
	} else if (size < fileinfo->st_blksize) {
// 		fprintf( stderr, "size=%lld -> blksize %d\n", size, fileinfo->st_blksize );
		return fileinfo->st_blksize;
	} else {
		// round up to the next multiple of st_blksize:
		long long int remainder = size % fileinfo->st_blksize;
// 		fprintf( stderr, "size=%lld -> multiple of blksize %d: %lld (%g ; %d)\n", size,
// 				 fileinfo->st_blksize, (remainder != 0) ? size + (fileinfo->st_blksize - remainder) : size,
// 				 (double) ((remainder != 0) ? size + (fileinfo->st_blksize - remainder) : size) / fileinfo->st_blocks,
// 				 fileinfo->st_blksize);
		return (remainder != 0) ? size + (fileinfo->st_blksize - remainder) : size;
	}
}

static bool quitRequested = FALSE;

static void signal_handler(int sig)
{
	fprintf( stderr, "Received signal %d: " AFSCTOOL_PROG_NAME " will quit\n", sig );
#ifdef SUPPORT_PARALLEL
	stopParallelProcessor(PP);
#endif
}

bool fileIsCompressable(const char *inFile, struct stat *inFileInfo, int comptype, bool *isAPFS)
{
	struct statfs fsInfo;
	errno = 0;
	int ret = statfs(inFile, &fsInfo);
#ifdef __APPLE__
	// https://github.com/RJVB/afsctool/pull/1#issuecomment-352727426
	uint32_t MNTTYPE_ZFS_SUBTYPE = 'Z'<<24|'F'<<16|'S'<<8;
// // 	fprintf( stderr, "statfs=%d f_type=%u f_fssubtype=%u \"%s\" ISREG=%d UF_COMPRESSED=%d compressable=%d\n",
// // 			ret, fsInfo.f_type, fsInfo.f_fssubtype, fsInfo.f_fstypename,
// // 			S_ISREG(inFileInfo->st_mode), (inFileInfo->st_flags & UF_COMPRESSED),
// // 			ret >= 0 && fsInfo.f_type == 17
// // 				&& S_ISREG(inFileInfo->st_mode)
// // 				&& (inFileInfo->st_flags & UF_COMPRESSED) == 0 );
	bool _isAPFS = !strncasecmp(fsInfo.f_fstypename, "apfs", 4);
	bool _isZFS = (fsInfo.f_fssubtype == MNTTYPE_ZFS_SUBTYPE);
	if (isAPFS) {
		*isAPFS = ret >= 0 ? _isAPFS && !_isZFS : false;
	}
	if (ret < 0) {
		fprintf( stderr, "\"%s\": %s\n", inFile, strerror(errno) );
		return false;
	}
#ifndef HFSCOMPRESS_TO_ZFS
	if (_isZFS) {
		// ZFS doesn't do HFS/decmpfs compression. It may pretend to, but in
		// that case it will *de*compress the data before committing it. We
		// won't play that game, wasting cycles and rewriting data for nothing.
		return false;
	}
#endif
#ifdef HAS_LZVN
	// the LZVN compressor we use fails on buffers that are too small, so we need to verify
	// if the file gets to be split into chunks that are all large enough.
	if (comptype == LZVN){
		int lastChunkSize = inFileInfo->st_size % 0x10000;
		if (lastChunkSize > 0 && lastChunkSize < LZVN_ENCODE_MIN_SRC_SIZE) {
			if (printVerbose >= 2) {
				fprintf( stderr, "\"%s\": file too small or will contain a too small compression chunk (try ZLIB compression)\n",
						 inFile);
			}
			return false;
		}
	}
#endif
#ifdef VOL_CAP_FMT_DECMPFS_COMPRESSION
	// https://opensource.apple.com/source/copyfile/copyfile-146/copyfile.c.auto.html
	int rv;
	struct attrlist attrs;
	char volroot[MAXPATHLEN + 1];
	struct {
		uint32_t length;
		vol_capabilities_attr_t volAttrs;
	} volattrs;

	strlcpy(volroot, fsInfo.f_mntonname, sizeof(volroot));
	memset(&attrs, 0, sizeof(attrs));
	attrs.bitmapcount = ATTR_BIT_MAP_COUNT;
	attrs.volattr = ATTR_VOL_CAPABILITIES;

	errno = 0;
	rv = getattrlist(volroot, &attrs, &volattrs, sizeof(volattrs), 0);
// 	fprintf( stderr, "volattrs for \"%s\": rv=%d VOL_CAP_FMT_DECMPFS_COMPRESSION=%d:%d\n", volroot,
// 		rv,
// 		(bool)(volattrs.volAttrs.capabilities[VOL_CAPABILITIES_FORMAT] & VOL_CAP_FMT_DECMPFS_COMPRESSION),
// 		(bool)(volattrs.volAttrs.valid[VOL_CAPABILITIES_FORMAT] & VOL_CAP_FMT_DECMPFS_COMPRESSION)
// 	);
	if (errno) {
		fprintf( stderr, "Error getting volattrs for \"%s\": %s\n", volroot, strerror(errno) );
	}
	return (rv != -1 &&
		(volattrs.volAttrs.capabilities[VOL_CAPABILITIES_FORMAT] & VOL_CAP_FMT_DECMPFS_COMPRESSION) &&
		(volattrs.volAttrs.valid[VOL_CAPABILITIES_FORMAT] & VOL_CAP_FMT_DECMPFS_COMPRESSION)
		&& S_ISREG(inFileInfo->st_mode)
		&& (inFileInfo->st_flags & UF_COMPRESSED) == 0 );
#else
	return (ret >= 0
		&& (!strncasecmp(fsInfo.f_fstypename, "hfs", 3) || _isAPFS)
		&& S_ISREG(inFileInfo->st_mode)
		&& (inFileInfo->st_flags & UF_COMPRESSED) == 0);
#endif
#else // !APPLE
	return (ret >= 0 && S_ISREG(inFileInfo->st_mode));
#endif
}

/** Mac OS X basename() can modify the input string when not in 'legacy' mode on 10.6
 * and indeed it does. So we use our own which doesn't, and also doesn't require internal
 * storage.
 */
static const char *lbasename(const char *url)
{ const char *c = NULL;
	if (url)
	{
		if ((c =  strrchr( url, '/' )))
		{
			c++;
		}
		else
		{
			c = url;
		}
	}
	return c;
}

const char *compressionTypeName(int type)
{
	char *name = "";
	switch (type) {
#define STR(v)		#v
#define STRVAL(v)	STR(v)
		case CMP_ZLIB_XATTR:
			name = "ZLIB in decmpfs xattr (" STRVAL(CMP_ZLIB_XATTR) ")";
			break;
		case CMP_ZLIB_RESOURCE_FORK:
			name = "ZLIB in resource fork (" STRVAL(CMP_ZLIB_RESOURCE_FORK) ")";
			break;
		case CMP_LZVN_XATTR:
			name = "LZVN in decmpfs xattr (" STRVAL(CMP_LZVN_XATTR) ")";
			break;
		case CMP_LZVN_RESOURCE_FORK:
			name = "LZVN in resource fork (" STRVAL(CMP_LZVN_RESOURCE_FORK) ")";
			break;
		case CMP_LZFSE_XATTR:
			name = "LZFSE in decmpfs xattr (" STRVAL(CMP_LZFSE_XATTR) ")";
			break;
		case CMP_LZFSE_RESOURCE_FORK:
			name = "LZFSE in resource fork (" STRVAL(CMP_LZFSE_RESOURCE_FORK) ")";
			break;
	}
	return name;
}

#ifdef SUPPORT_PARALLEL
void compressFile(const char *inFile, struct stat *inFileInfo, struct folder_info *folderinfo, FileProcessor *worker )
#else
void compressFile(const char *inFile, struct stat *inFileInfo, struct folder_info *folderinfo, void *ignored)
#endif
{
	long long int maxSize = folderinfo->maxSize;
	int compressionlevel = folderinfo->compressionlevel;
	int comptype = folderinfo->compressiontype;
	bool allowLargeBlocks = folderinfo->allowLargeBlocks;
	double minSavings = folderinfo->minSavings;
	bool checkFiles = folderinfo->check_files;
	bool backupFile = folderinfo->backup_file;

	BlockMutable int fdIn;
	BlockMutable char *backupName = NULL;

	// 64Kb block size (HFS compression is "64K chunked")
	const int compblksize = 0x10000;
	unsigned int numBlocks, outdecmpfsSize = 0;
	void *inBuf = NULL, *outBuf = NULL, *outBufBlock = NULL, *outdecmpfsBuf = NULL, *currBlock = NULL, *blockStart = NULL;
	long long int inBufPos;
	off_t filesize = inFileInfo->st_size;
	unsigned long int cmpedsize;
	char *xattrnames, *curr_attr;
	ssize_t xattrnamesize, outBufSize = 0;
	UInt32 cmpf = DECMPFS_MAGIC, orig_mode;
	struct timeval times[2];
#if defined HAS_LZVN || defined HAS_LZFSE
	void *lz_WorkSpace = NULL;
#endif
	bool supportsLargeBlocks;
	bool useMmap = false;

	if (quitRequested)
	{
		return;
	}

#ifdef __APPLE__
	void (^restoreFile)() = ^{
		if (write(fdIn, inBuf, filesize) != filesize) {
			fprintf(stderr, "%s: Error restoring file (%lld bytes; %s)\n", inFile, filesize, strerror(errno));
			if (backupName) {
				fprintf(stderr, "\ta backup is available as %s\n", backupName);
				xfree(backupName);
			}
			xclose(fdIn);
		}
	};

	times[0].tv_sec = inFileInfo->st_atimespec.tv_sec;
	times[0].tv_usec = inFileInfo->st_atimespec.tv_nsec / 1000;
	times[1].tv_sec = inFileInfo->st_mtimespec.tv_sec;
	times[1].tv_usec = inFileInfo->st_mtimespec.tv_nsec / 1000;
#elif defined(linux)
	times[0].tv_sec = inFileInfo->st_atim.tv_sec;
	times[0].tv_usec = inFileInfo->st_atim.tv_nsec / 1000;
	times[1].tv_sec = inFileInfo->st_mtim.tv_sec;
	times[1].tv_usec = inFileInfo->st_mtim.tv_nsec / 1000;
#endif
	
	if (!fileIsCompressable(inFile, inFileInfo, comptype, &folderinfo->onAPFS)){
		return;
	}
	if (filesize > maxSize && maxSize != 0){
		if (folderinfo->print_info > 2)
		{
			fprintf( stderr, "Skipping file %s size %lld > max size %lld\n", inFile, (long long) filesize, maxSize );
		}
		return;
	}
	if (filesize == 0){
		if (folderinfo->print_info > 2)
		{
			fprintf( stderr, "Skipping empty file %s\n", inFile );
		}
		return;
	}
	orig_mode = inFileInfo->st_mode;
	if ((orig_mode & S_IWUSR) == 0) {
		chmod(inFile, orig_mode | S_IWUSR);
		lstat(inFile, inFileInfo);
	}
	if ((orig_mode & S_IRUSR) == 0) {
		chmod(inFile, orig_mode | S_IRUSR);
		lstat(inFile, inFileInfo);
	}

#ifdef __APPLE__
	if (chflags(inFile, UF_COMPRESSED | inFileInfo->st_flags) < 0 || chflags(inFile, inFileInfo->st_flags) < 0)
	{
		fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
		return;
	}
	
	xattrnamesize = listxattr(inFile, NULL, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
	
	if (xattrnamesize > 0)
	{
		xattrnames = (char *) malloc(xattrnamesize);
		if (xattrnames == NULL)
		{
			fprintf(stderr, "%s: malloc error, unable to get file information (%lu bytes; %s)\n",
					inFile, (unsigned long) xattrnamesize, strerror(errno));
			return;
		}
		if ((xattrnamesize = listxattr(inFile, xattrnames, xattrnamesize, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW)) <= 0)
		{
			fprintf(stderr, "%s: listxattr: %s\n", inFile, strerror(errno));
			free(xattrnames);
			return;
		}
		for (curr_attr = xattrnames; curr_attr < xattrnames + xattrnamesize; curr_attr += strlen(curr_attr) + 1)
		{
			if ((strcmp(curr_attr, XATTR_RESOURCEFORK_NAME) == 0 && strlen(curr_attr) == 22) ||
				(strcmp(curr_attr, DECMPFS_XATTR_NAME) == 0 && strlen(curr_attr) == 17))
				return;
		}
		free(xattrnames);
	}
#endif // APPLE

	numBlocks = (filesize + compblksize - 1) / compblksize;
	// TODO: make compression-type specific (as far as that's possible).
	if ((filesize + 0x13A + (numBlocks * 9)) > CMP_MAX_SUPPORTED_SIZE) {
		fprintf( stderr, "Skipping file %s with unsupportable size %lld\n", inFile, (long long) filesize );
		return;
	} else if (filesize >= 64 * 1024 * 1024) {
		// use a rather arbitrary threshold above which using mmap may be of interest
		useMmap = true;
	}

#ifdef SUPPORT_PARALLEL
	bool locked = false;
	if( exclusive_io && worker ){
		// Lock the IO lock. We'll unlock it when we're done, but we don't bother
		// when we have to return before that as our caller (a worker thread)
		// will clean up for us.
		locked = lockParallelProcessorIO(worker);
	}
#endif
	// use open() with an exclusive lock so noone can modify the file while we're at it
	fdIn = open(inFile, O_RDWR|O_EXLOCK|O_NONBLOCK);
	if (fdIn == -1)
	{
		fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
		goto bail;
	}
#ifndef NO_USE_MMAP
	if (useMmap) {
		// get a private mmap. We rewrite to the file's attributes and/or resource fork,
		// so there is no point in using a shared mapping where changes to the memory
		// are mapped back to disk. The use of NOCACHE is experimental; if I understand
		// the documentation correctly this just means that released memory can be
		// reused more easily.
		inBuf = mmap(NULL, filesize, PROT_READ, MAP_PRIVATE|MAP_NOCACHE, fdIn, 0);
		if (inBuf == MAP_FAILED) {
			fprintf(stderr, "%s: Error m'mapping file (size %lld; %s)\n", inFile, (long long) filesize, strerror(errno));
			useMmap = false;
		} else {
			madvise(inBuf, filesize, MADV_RANDOM);
		}
	}
	if (!useMmap)
#endif
	{
		inBuf = malloc(filesize);
		if (inBuf == NULL)
		{
			fprintf(stderr, "%s: malloc error, unable to allocate input buffer of %lld bytes (%s)\n", inFile, (long long) filesize, strerror(errno));
			xclose(fdIn);
			utimes(inFile, times);
			return;
		}
		madvise(inBuf, filesize, MADV_RANDOM);
		if (read(fdIn, inBuf, filesize) != filesize)
		{
			fprintf(stderr, "%s: Error reading file (%s)\n", inFile, strerror(errno));
			xclose(fdIn);
			utimes(inFile, times);
			free(inBuf);
			return;
		}
	}
	// keep our filedescriptor open to maintain the lock!
#ifdef __APPLE__
	if (backupFile)
	{ int fd, bkNameLen;
	  FILE *fp;
	  char *infile, *inname = NULL;
		if ((infile = strdup(inFile)))
		{ 
			inname = (char*) lbasename(infile);
			// avoid filename overflow; assume 32 fixed template char for mkstemps
			// just to be on the safe side (even in parallel mode).
			if (strlen(inname) > 1024 - 32)
			{
				// truncate
				inname[1024-32] = '\0';
			}
#ifdef SUPPORT_PARALLEL
			// add the processor ID for the unlikely case that 2 threads try to backup a file with the same name
			// at the same time, and mkstemps() somehow generates the same temp. name. I've seen it generate EEXIST
			// errors which suggest that might indeed happen.
			bkNameLen = asprintf(&backupName, "/tmp/afsctbk.%d.XXXXXX.%s", currentParallelProcessorID(worker), inname);
#else
			bkNameLen = asprintf(&backupName, "/tmp/afsctbk.XXXXXX.%s", inname);
#endif
		}
		if (!infile || bkNameLen < 0)
		{
			fprintf(stderr, "%s: malloc error, unable to generate temporary backup filename (%s)\n", inFile, strerror(errno));
			xfree(infile);
			goto bail;
		}
		if ((fd = mkstemps(backupName, strlen(inname)+1)) < 0 || !(fp = fdopen(fd, "w")))
		{
			fprintf(stderr, "%s: error creating temporary backup file %s (%s)\n", inFile, backupName, strerror(errno));
			xfree(infile);
			goto bail;
		}
		xfree(infile);
		if (fwrite(inBuf, filesize, 1, fp) != 1)
		{
			fprintf(stderr, "%s: Error writing to backup file %s (%lld bytes; %s)\n", inFile, backupName, filesize, strerror(errno));
			fclose(fp);
			goto bail;
		}
		fclose(fp);
		utimes(backupName, times);
		chmod(backupName, orig_mode);
	}
#endif
#ifdef SUPPORT_PARALLEL
	if( exclusive_io && worker ){
		locked = unLockParallelProcessorIO(worker);
	}
#endif

	outdecmpfsBuf = malloc(MAX_DECMPFS_XATTR_SIZE);
	if (outdecmpfsBuf == NULL)
	{
		fprintf(stderr, "%s: malloc error, unable to allocate xattr buffer (%d bytes; %s)\n",
				inFile, MAX_DECMPFS_XATTR_SIZE, strerror(errno));
		utimes(inFile, times);
		goto bail;
	}

	struct compressionType {
		UInt32 xattr, resourceFork;
	} compressionType;
	uLong zlib_EstimatedCompressedChunkSize;
#if defined HAS_LZVN || defined HAS_LZFSE
	size_t lz_EstimatedCompressedSize;

	lz_chunk_table *chunkTable = (lz_chunk_table*) outBuf;
	ssize_t chunkTableByteSize;
#endif

	switch (comptype) {
		case ZLIB: {
			cmpedsize = zlib_EstimatedCompressedChunkSize = compressBound(compblksize);
			struct compressionType t = {CMP_ZLIB_XATTR, CMP_ZLIB_RESOURCE_FORK};
			compressionType = t;
#ifdef ZLIB_SINGLESHOT_OUTBUF
			outBufSize = filesize + 0x13A + (numBlocks * 9);
#else
			outBufSize = 0x104 + sizeof(UInt32) + numBlocks * 8;
#endif
#define SET_BLOCKSTART()	blockStart = outBuf + 0x104
			outBuf = malloc(outBufSize);
			break;
		}
#ifdef HAS_LZVN
		case LZVN: {
			cmpedsize = lz_EstimatedCompressedSize = MAX(lzvn_encode_scratch_size(), compblksize);
			lz_WorkSpace = malloc(cmpedsize);
			// for this compressor we will let the outBuf grow incrementally. Slower,
			// but use only as much memory as required.

			// The chunk table stores the offset of every block, and the offset of where a next block _would_ go,
			// so we need numBlocks + 1 items
			outBuf = calloc(numBlocks + 1, sizeof(*chunkTable));
			chunkTable = outBuf;
			if (!lz_WorkSpace || !chunkTable) {
				fprintf(stderr,
						"%s: malloc error, unable to allocate %lu bytes for lzvn workspace or %u element chunk table(%s)\n",
						inFile, cmpedsize, numBlocks, strerror(errno));
				utimes(inFile, times);
				goto bail;
			}
			struct compressionType t = {CMP_LZVN_XATTR, CMP_LZVN_RESOURCE_FORK};
			compressionType = t;
			outBufSize = chunkTableByteSize = (numBlocks + 1) * sizeof(*chunkTable);
			chunkTable[0] = chunkTableByteSize;
			break;
		}
#endif
#ifdef HAS_LZFSE
		case LZFSE:
			cmpedsize = lz_EstimatedCompressedSize = MAX(lzfse_encode_scratch_size(), compblksize);
			lz_WorkSpace = lz_EstimatedCompressedSize ? malloc(lz_EstimatedCompressedSize) : 0;
			// for this compressor we will let the outBuf grow incrementally. Slower,
			// but use only as much memory as required.

			// The chunk table stores the offset of every block, and the offset of where a next block _would_ go,
			// so we need numBlocks + 1 items
			outBuf = calloc(numBlocks + 1, sizeof(*chunkTable));
			chunkTable = outBuf;
			if ((!lz_WorkSpace && lz_EstimatedCompressedSize) || !chunkTable) {
				fprintf(stderr,
						"%s: malloc error, unable to allocate %lu bytes for lzfse workspace or %u element chunk table(%s)\n",
						inFile, cmpedsize, numBlocks, strerror(errno));
				utimes(inFile, times);
				goto bail;
			}
			struct compressionType t = {CMP_LZFSE_XATTR, CMP_LZFSE_RESOURCE_FORK};
			compressionType = t;
			outBufSize = chunkTableByteSize = (numBlocks + 1) * sizeof(*chunkTable);
			chunkTable[0] = chunkTableByteSize;
			break;
#endif
		default:
			fprintf(stderr, "%s: unsupported compression type %d (%s)\n",
					inFile, comptype, compressionTypeName(comptype));
			utimes(inFile, times);
			goto bail;
			break;
	}

	if (outBuf == NULL && outBufSize != 0)
	{
		fprintf(stderr, "%s: malloc error, unable to allocate output buffer of %lu bytes (%s)\n",
				inFile, outBufSize, strerror(errno));
		utimes(inFile, times);
		goto bail;
	}

	outBufBlock = malloc(cmpedsize);
	if (outBufBlock == NULL)
	{
		fprintf(stderr, "%s: malloc error, unable to allocate compression buffer of %lu bytes (%s)\n",
				inFile, cmpedsize, strerror(errno));
		utimes(inFile, times);
		goto bail;
	}
	// The header of the compression resource fork (16 bytes):
	// compression magic number
	decmpfs_disk_header *decmpfsAttr = (decmpfs_disk_header*) outdecmpfsBuf;
	decmpfsAttr->compression_magic = OSSwapHostToLittleInt32(cmpf);
	// the compression type: 4 == compressed data in the resource fork.
	// FWIW, libarchive has the following comment in archive_write_disk_posix.c :
	//* If the compressed size is smaller than MAX_DECMPFS_XATTR_SIZE [3802]
	//* and the block count in the file is only one, store compressed
	//* data to decmpfs xattr instead of the resource fork.
	// We do the same below.
	decmpfsAttr->compression_type = OSSwapHostToLittleInt32(compressionType.resourceFork);
	// the uncompressed filesize
	decmpfsAttr->uncompressed_size = OSSwapHostToLittleInt64(filesize);
	// outdecmpfsSize = 0x10;
	outdecmpfsSize = sizeof(decmpfs_disk_header);

	unsigned long currBlockLen, currBlockOffset;
	switch (comptype) {
		case ZLIB:
			*(UInt32 *) outBuf = OSSwapHostToBigInt32(0x100);
			*(UInt32 *) (outBuf + 12) = OSSwapHostToBigInt32(0x32);
			memset(outBuf + 16, 0, 0xF0);
			SET_BLOCKSTART();
			// block table: numBlocks + offset,blocksize pairs for each of the blocks
			*(UInt32 *) blockStart = OSSwapHostToLittleInt32(numBlocks);
			// actual compressed data starts after the block table
			currBlock = blockStart + sizeof(UInt32) + (numBlocks * 8);
			currBlockLen = outBufSize - (currBlock - outBuf);
			supportsLargeBlocks = true;
			break;
#ifdef HAS_LZVN
		case LZVN:
			supportsLargeBlocks = false;
			break;
#endif
#ifdef HAS_LZFSE
		case LZFSE:
			supportsLargeBlocks = false;
			break;
#endif
		default:
			// noop
			break;
	}
	int blockNr;

	// chunking loop that compresses the 64K chunks and handles the result(s).
	// Note that LZVN appears not to be chunked like that (maybe inside lzvn_encode()?)
	// TODO: refactor and merge with the switch() above if LZFSE compression doesn't need chunking either.
	for (inBufPos = 0, blockNr = 0, currBlockOffset = 0
		; inBufPos < filesize
		; inBufPos += compblksize, currBlock += cmpedsize, currBlockOffset += cmpedsize, ++blockNr)
	{
		void *cursor = inBuf + inBufPos;
		uLong bytesAfterCursor = ((filesize - inBufPos) > compblksize) ? compblksize : filesize - inBufPos;
		switch (comptype) {
			case ZLIB:
				// reset cmpedsize; it may be changed by compress2().
				cmpedsize = zlib_EstimatedCompressedChunkSize;
				if (compress2(outBufBlock, &cmpedsize, cursor, bytesAfterCursor, compressionlevel) != Z_OK)
				{
					utimes(inFile, times);
					goto bail;
				}
#ifndef ZLIB_SINGLESHOT_OUTBUF
				if (currBlockOffset == 0) {
					currBlockOffset = outBufSize;
				} 
				outBufSize += cmpedsize;
				if (!(outBuf = reallocf(outBuf, outBufSize + 1))) {
					fprintf(stderr, "%s: malloc error, unable to increase output buffer to %lu bytes (%s)\n",
							inFile, outBufSize, strerror(errno));
					utimes(inFile, times);
					goto bail;
				}
				// update this one!
				SET_BLOCKSTART();
				currBlock = outBuf + currBlockOffset;
				currBlockLen = outBufSize;
#endif
				break;
#ifdef HAS_LZVN
			case LZVN:{
				// store the current last 4 bytes of compressed file content (bogus the 1st time we come here)
				UInt32 prevLast = ((UInt32*)outBufBlock)[cmpedsize/sizeof(UInt32)-1];
				cmpedsize =
					lzvn_encode_buffer(outBufBlock, lz_EstimatedCompressedSize, cursor, bytesAfterCursor, lz_WorkSpace);
				if (cmpedsize <= 0)
				{
					fprintf( stderr, "%s: lzvn compression failed on chunk #%d (of %u; %lu bytes)\n",
							 inFile, blockNr, numBlocks, bytesAfterCursor);
// 					if (bytesAfterCursor < LZVN_MINIMUM_COMPRESSABLE_SIZE) {
// 						cmpedsize = bytesAfterCursor;
// 						memcpy(outBufBlock, cursor, bytesAfterCursor);
// 					} else {
						utimes(inFile, times);
						goto bail;
// 					} 
				}
				// next offset will start at this offset
				if (blockNr < numBlocks) {
					chunkTable[blockNr+1] = chunkTable[blockNr] + cmpedsize;
				}
				if (currBlockOffset == 0) {
					currBlockOffset = outBufSize;
				} 
				outBufSize += cmpedsize;
				if (!(outBuf = reallocf(outBuf, outBufSize))) {
					fprintf(stderr, "%s: malloc error, unable to increase output buffer to %lu bytes (%s)\n",
							inFile, outBufSize, strerror(errno));
					utimes(inFile, times);
					goto bail;
				}
				// update this one!
				chunkTable = outBuf;
				currBlock = outBuf + currBlockOffset;
				currBlockLen = outBufSize;
				// if not the 1st time we're here, check if the 4 bytes of compressed file contents
				// just before the current position in the output buffer correspond to prevLast.
				// If the test fails we may have a chunking overlap issue.
				// It never happened to my knowledge, but this sanity check is cheap enough to keep.
				if (blockNr > 1 && ((UInt32*)currBlock)[-1] != prevLast) {
					fprintf(stderr, "%s: warning, possible chunking overlap: prevLast=%u currBlock[-1]=%u currBlock[0]=%u\n",
						inFile, prevLast, ((UInt32*)currBlock)[-1], ((UInt32*)currBlock)[0]);
				}
				break;
			}
#endif
#ifdef HAS_LZFSE
			case LZFSE:{
				cmpedsize = lz_EstimatedCompressedSize;
				while (1) {
					cmpedsize = lzfse_encode_buffer(outBufBlock, cmpedsize, cursor, bytesAfterCursor, lz_WorkSpace);
						// If output buffer was too small, grow and retry.
					if (cmpedsize == 0) {
						cmpedsize <<= 1;
						if (!(outBufBlock = reallocf(outBufBlock, cmpedsize))) {
							fprintf(stderr, "%s: malloc error, unable to increase output buffer to %lu bytes (%s)\n",
									inFile, cmpedsize, strerror(errno));
							utimes(inFile, times);
							goto bail;
						}
						continue;
					}
					break;
				}
				// next offset will start at this offset
				if (blockNr < numBlocks) {
					chunkTable[blockNr + 1] = chunkTable[blockNr] + cmpedsize;
				}
				if (currBlockOffset == 0) {
					currBlockOffset = outBufSize;
				} 
				outBufSize += cmpedsize;
				if (!(outBuf = reallocf(outBuf, outBufSize))) {
					fprintf(stderr, "%s: malloc error, unable to increase output buffer to %lu bytes (%s)\n",
							inFile, outBufSize, strerror(errno));
					utimes(inFile, times);
					goto bail;
				}
				// update this one!
				chunkTable = outBuf;
				currBlock = outBuf + currBlockOffset;
				currBlockLen = outBufSize;
				break;
			}
#endif
			default:
				// noop
				break;
		}
		if (supportsLargeBlocks && cmpedsize > (((filesize - inBufPos) > compblksize) ? compblksize : filesize - inBufPos))
		{
			if (!allowLargeBlocks && (((filesize - inBufPos) > compblksize) ? compblksize : filesize - inBufPos) == compblksize)
			{
				if (printVerbose >= 2) {
					fprintf(stderr, "%s: file has a compressed chunk that's larger than the original chunk; -L to compress\n", inFile);
				}
				utimes(inFile, times);
				goto bail;
			}
			*(unsigned char *) outBufBlock = 0xFF;
			memcpy(outBufBlock + 1, inBuf + inBufPos, ((filesize - inBufPos) > compblksize) ? compblksize : filesize - inBufPos);
			cmpedsize = ((filesize - inBufPos) > compblksize) ? compblksize : filesize - inBufPos;
			cmpedsize++;
		}
		if (((cmpedsize + outdecmpfsSize) <= MAX_DECMPFS_XATTR_SIZE) && (numBlocks <= 1))
		{
			// store in directly into the attribute instead of using the resource fork.
			// *(UInt32 *) (outdecmpfsBuf + 4) = EndianU32_NtoL(compressionType.xattr);
			decmpfsAttr->compression_type = OSSwapHostToLittleInt32(compressionType.xattr);
			memcpy(outdecmpfsBuf + outdecmpfsSize, outBufBlock, cmpedsize);
			outdecmpfsSize += cmpedsize;
			folderinfo->data_compressed_size = outdecmpfsSize;
			break;
		}
		if (currBlockOffset + cmpedsize <= currBlockLen) {
			memcpy(currBlock, outBufBlock, cmpedsize);
		} else {
			fprintf( stderr, "%s: result buffer overrun at chunk #%d (%lu >= %lu)\n", inFile, blockNr,
				currBlockOffset + cmpedsize, currBlockLen);
			utimes(inFile, times);
			goto bail;
		}
		switch (comptype) {
			case ZLIB:
				*(UInt32 *) (blockStart + ((inBufPos / compblksize) * 8) + 0x4) = OSSwapHostToLittleInt32(currBlock - blockStart);
				*(UInt32 *) (blockStart + ((inBufPos / compblksize) * 8) + 0x8) = OSSwapHostToLittleInt32(cmpedsize);
				break;
			default:
				// noop
				break;
		}
	}
	// deallocate memory that isn't needed anymore.
#if defined HAS_LZVN || defined HAS_LZFSE
	xfree(lz_WorkSpace);
#endif
	free(outBufBlock); outBufBlock = NULL;

#ifdef SUPPORT_PARALLEL
	// 20160928: the actual rewrite of the file is never done in parallel
	if( worker ){
		locked = lockParallelProcessorIO(worker);
	}
#else
	signal(SIGINT, SIG_IGN);
	signal(SIGHUP, SIG_IGN);
#endif

	// fdIn is still open
	bool isTruncated = false;

	//if (EndianU32_LtoN(*(UInt32 *) (outdecmpfsBuf + 4)) == CMP_ZLIB_RESOURCE_FORK)
	if (OSSwapLittleToHostInt32(decmpfsAttr->compression_type) == compressionType.resourceFork)
	{
		long long int newSize = currBlock - outBuf + outdecmpfsSize + 50;
		if ((minSavings != 0.0 && ((double) newSize / filesize) >= (1.0 - minSavings / 100))
			|| newSize >= filesize)
		{
			// reject the compression result: doesn't meet user criteria or the compressed
			// version is larger than the uncompressed file.
			// TODO: shouldn't this be checked for all HFS-compressed types,
			//       not just for the resource-fork based variant?
			utimes(inFile, times);
			if (printVerbose > 2) {
				fprintf(stderr,
					"%s: compressed size (%lld) doesn't give required savings (%g) or larger than original (%lld)\n",
					inFile, newSize, minSavings, (long long) filesize);
			}
			goto bail;
		}
		// create and write the resource fork:
		switch (comptype) {
			case ZLIB:
#ifndef ZLIB_SINGLESHOT_OUTBUF
				currBlockOffset = currBlock - outBuf;
				outBufSize += currBlockOffset + sizeof(decmpfs_resource_zlib_trailer);
				if (!(outBuf = reallocf(outBuf, outBufSize + 1))) {
					fprintf(stderr, "%s: malloc error, unable to increase output buffer to %lu bytes (%s)\n",
							inFile, outBufSize, strerror(errno));
					utimes(inFile, times);
					goto bail;
				}
				// update this one!
				SET_BLOCKSTART();
				currBlock = outBuf + currBlockOffset;
#endif
				*(UInt32 *) (outBuf + 4) = OSSwapHostToBigInt32(currBlock - outBuf);
				*(UInt32 *) (outBuf + 8) = OSSwapHostToBigInt32(currBlock - outBuf - 0x100);
				*(UInt32 *) (blockStart - 4) = OSSwapHostToBigInt32(currBlock - outBuf - 0x104);
				decmpfs_resource_zlib_trailer *resourceTrailer = (decmpfs_resource_zlib_trailer*) currBlock;
				memset(&resourceTrailer->empty[0], 0, 24);
				resourceTrailer->magic1 = OSSwapHostToBigInt16(0x1C);
				resourceTrailer->magic2 = OSSwapHostToBigInt16(0x32);
				resourceTrailer->spacer1 = 0;
				resourceTrailer->compression_magic = OSSwapHostToBigInt32(cmpf);
				resourceTrailer->magic3 = OSSwapHostToBigInt32(0xA);
				resourceTrailer->magic4 = OSSwapHostToLittleInt64(0xFFFF0100);
				resourceTrailer->spacer2 = 0;
#ifdef __APPLE__
				ftruncate(fdIn, 0);
				lseek(fdIn, SEEK_SET, 0);
				if (setxattr(inFile, XATTR_RESOURCEFORK_NAME, outBuf, currBlock - outBuf + 50, 0,
					XATTR_NOFOLLOW | XATTR_CREATE) < 0)
				{
					fprintf(stderr, "%s: setxattr(%d): %s (%d)\n", inFile, fdIn, strerror(errno), __LINE__);
					restoreFile();
					goto bail;
				}
				isTruncated = true;
#else
				if (printVerbose > 2) {
					fprintf(stderr, "# setxattr(XATTR_RESOURCEFORK_NAME) outBuf=%p len=%lu\n",
							outBuf, currBlock - outBuf + 50);
				}
#endif
				folderinfo->data_compressed_size = currBlock - outBuf + 50;
				break;
#ifdef HAS_LZVN
			case LZVN: {
#ifdef __APPLE__
				ftruncate(fdIn, 0);
				lseek(fdIn, SEEK_SET, 0);
				if (setxattr(inFile, XATTR_RESOURCEFORK_NAME, outBuf, outBufSize, 0,
					XATTR_NOFOLLOW | XATTR_CREATE) < 0)
				{
					fprintf(stderr, "%s: setxattr(%d): %s (%d)\n", inFile, fdIn, strerror(errno), __LINE__);
					restoreFile();
					goto bail;
				}
				isTruncated = true;
#else
				if (printVerbose > 2) {
					fprintf(stderr, "# setxattr(XATTR_RESOURCEFORK_NAME) outBuf=%p len=%lu\n",
							outBuf, outBufSize);
				}
#endif
				folderinfo->data_compressed_size = outBufSize;
				break;
			}
#endif
#ifdef HAS_LZFSE
			case LZFSE:
#ifdef __APPLE__
				ftruncate(fdIn, 0);
				lseek(fdIn, SEEK_SET, 0);
				if (setxattr(inFile, XATTR_RESOURCEFORK_NAME, outBuf, outBufSize, 0,
					XATTR_NOFOLLOW | XATTR_CREATE) < 0)
				{
					fprintf(stderr, "%s: setxattr(%d): %s (%d)\n", inFile, fdIn, strerror(errno), __LINE__);
					restoreFile();
					goto bail;
				}
				isTruncated = true;
#else
				if (printVerbose > 2) {
					fprintf(stderr, "# setxattr(XATTR_RESOURCEFORK_NAME) outBuf=%p len=%lu\n",
							outBuf, outBufSize);
				}
#endif
				folderinfo->data_compressed_size = outBufSize;
				break;
#endif
			default:
				// noop
				break;
		}
		if (outBufSize > maxOutBufSize) {
			maxOutBufSize = outBufSize;
		}
	}
#ifdef __APPLE__
	// set the decmpfs attribute, which may or may not contain compressed data.
	// This requires negligible disk space so we do not truncate the file first,
	// only (potentially) if the attribute was written successfully.
	if (setxattr(inFile, DECMPFS_XATTR_NAME, outdecmpfsBuf, outdecmpfsSize, 0, XATTR_NOFOLLOW | XATTR_CREATE) < 0)
	{
		fprintf(stderr, "%s: setxattr(%d): %s (%d)\n", inFile, fdIn, strerror(errno), __LINE__);
		goto bail;
	}
	if (!isTruncated) {
		ftruncate(fdIn, 0);
		lseek(fdIn, SEEK_SET, 0);
		isTruncated = true;
	}
#else
	if (printVerbose > 2) {
		fprintf(stderr, "# setxattr(DECMPFS_XATTR_NAME) buf=%p len=%u\n",
				outdecmpfsBuf, outdecmpfsSize);
	}
#endif

	// we rewrite the data fork - it should contain 0 bytes if compression succeeded.
#ifdef __APPLE__
	if (fchflags(fdIn, UF_COMPRESSED | inFileInfo->st_flags) < 0)
	{
		fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
		if (fremovexattr(fdIn, DECMPFS_XATTR_NAME, XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
		{
			fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
		}
// 		if (EndianU32_LtoN(*(UInt32 *) (outdecmpfsBuf + 4)) == CMP_ZLIB_RESOURCE_FORK &&
		if (OSSwapLittleToHostInt32(decmpfsAttr->compression_type) == compressionType.resourceFork &&
			fremovexattr(fdIn, XATTR_RESOURCEFORK_NAME, XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
		{
			fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
		}
		restoreFile();
		xclose(fdIn);
		utimes(inFile, times);
		goto bail;
	}
#else
	if (printVerbose > 2) {
		fprintf(stderr, "# empty datafork and set UF_COMPRESSED flag\n");
	}
#endif
// 	fsync(fdIn);
	xclose(fdIn);
	lstat(inFile, inFileInfo);
	if (checkFiles)
	{
		bool sizeMismatch = inFileInfo->st_size != filesize, readFailure = false, contentMismatch = false;
		ssize_t checkRead= -2;
		bool outBufMMapped = false;
		errno = 0;
		fdIn = open(inFile, O_RDONLY|O_EXLOCK);
		if (fdIn == -1)
		{
			fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
			// we don't bail here, we fail (= restore the backup).
			goto fail;
		}
		if (!sizeMismatch) {
#ifndef NO_USE_MMAP
			xfree(outBuf);
			outBuf = mmap(NULL, filesize, PROT_READ, MAP_PRIVATE|MAP_NOCACHE, fdIn, 0);
			outBufMMapped = true;
#else
			outBuf = reallocf(outBuf, filesize);
#endif
			if (!outBuf) {
				xclose(fdIn);
				fprintf(stderr, "%s: failure reallocating buffer for validation; %s\n", inFile, strerror(errno));
				goto fail;
			}
			// this should be appropriate for simply reading into and comparing:
			madvise(inBuf, filesize, MADV_SEQUENTIAL);
			madvise(outBuf, filesize, MADV_SEQUENTIAL);
			if (!outBufMMapped) {
				errno = 0;
				readFailure = (checkRead = read(fdIn, outBuf, filesize)) != filesize;
			} else {
				readFailure = false;
				checkRead = filesize;
			}
		}
		xclose(fdIn);
		if (sizeMismatch || readFailure
			|| (contentMismatch = memcmp(outBuf, inBuf, filesize) != 0))
		{
			fprintf(stderr, "\tsize mismatch=%d read=%zd failure=%d content mismatch=%d (%s)\n",
				sizeMismatch, checkRead, readFailure, contentMismatch, strerror(errno));
fail:;
			printf("%s: Compressed file check failed, reverting file changes\n", inFile);
			if (outBufMMapped) {
				xmunmap(outBuf, filesize);
			}
#ifdef __APPLE__
			if (backupName)
			{
				fprintf(stderr, "\tin case of further failures, a backup will be available as %s\n", backupName);
			}
			if (chflags(inFile, (~UF_COMPRESSED) & inFileInfo->st_flags) < 0)
			{
				fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
				xfree(backupName);
				goto bail;
			}
			if (removexattr(inFile, DECMPFS_XATTR_NAME, XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
			{
				fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
			}
// 			if (EndianU32_LtoN(*(UInt32 *) (outdecmpfsBuf + 4)) == CMP_ZLIB_RESOURCE_FORK && 
			if (OSSwapLittleToHostInt32(decmpfsAttr->compression_type) == compressionType.resourceFork &&
				removexattr(inFile, XATTR_RESOURCEFORK_NAME, XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
			{
				fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
			}
			FILE *in = fopen(inFile, "w");
			if (in == NULL)
			{
				fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
				xfree(backupName);
				goto bail;
			}
			if (fwrite(inBuf, filesize, 1, in) != 1)
			{
				fprintf(stderr, "%s: Error writing to file (%lld bytes; %s)\n", inFile, filesize, strerror(errno));
				xfree(backupName);
				goto bail;
			}
			fclose(in);
#endif
		}
		if (outBufMMapped) {
			xmunmap(outBuf, filesize);
		}
	}
// #ifndef NO_USE_MMAP
// 	{
// 		char *tFileName = NULL;
// 		asprintf(&tFileName, "%s.%s", inFile, ".RestoreTest");
// 		int fdTest = open(tFileName, O_WRONLY|O_CREAT|O_EXCL);
// 		if (fdTest != -1) {
// 			if (write(fdTest, inBuf, filesize) != filesize) {
// 				fprintf(stderr, "%s: Error writing to testfile %s (%lld bytes; %s)\n", inFile, tFileName, filesize, strerror(errno));
// 			}
// 			close(fdTest);
// 			chmod(tFileName, orig_mode);
// 		}
// 		xfree(tFileName)
// 	}
// #endif
bail:
#ifdef __APPLE__
	utimes(inFile, times);
	if (inFileInfo->st_mode != orig_mode) {
		chmod(inFile, orig_mode);
	}
#endif
#ifdef SUPPORT_PARALLEL
	if( worker ){
		locked = unLockParallelProcessorIO(worker);
	}
#endif
	xclose(fdIn);
	if (backupName)
	{
		// a backupName is set and hasn't been unset because of a processing failure:
		// remove the file now.
		unlink(backupName);
		free(backupName);
		backupName = NULL;
	}
#ifndef SUPPORT_PARALLEL
	signal(SIGINT, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
#endif
#ifndef NO_USE_MMAP
	if (useMmap) {
		xmunmap(inBuf, filesize);
	} else
#endif
	{
		xfree(inBuf);
	}
	xfree(outBuf);
	xfree(outdecmpfsBuf);
	xfree(outBufBlock);
#if defined HAS_LZVN || defined HAS_LZFSE
	xfree(lz_WorkSpace);
#endif
}

void decompressFile(const char *inFile, struct stat *inFileInfo, bool backupFile)
{
#ifdef __APPLE__
	FILE *in;
	int uncmpret;
	unsigned int compblksize = 0x10000, numBlocks, currBlock;
	long long int filesize;
	unsigned long int uncmpedsize;
	void *inBuf = NULL, *outBuf = NULL, *indecmpfsBuf = NULL, *blockStart;
	char *xattrnames, *curr_attr;
	ssize_t xattrnamesize, indecmpfsLen = 0, inRFLen = 0, getxattrret, RFpos = 0;
	struct timeval times[2];
	UInt32 orig_mode;

	if (quitRequested)
	{
		return;
	}

	// not implemented
	backupFile = false;

	times[0].tv_sec = inFileInfo->st_atimespec.tv_sec;
	times[0].tv_usec = inFileInfo->st_atimespec.tv_nsec / 1000;
	times[1].tv_sec = inFileInfo->st_mtimespec.tv_sec;
	times[1].tv_usec = inFileInfo->st_mtimespec.tv_nsec / 1000;
	
	if (!S_ISREG(inFileInfo->st_mode))
		return;
	if ((inFileInfo->st_flags & UF_COMPRESSED) == 0)
		return;
	orig_mode = inFileInfo->st_mode;
	if ((orig_mode & S_IWUSR) == 0) {
		chmod(inFile, orig_mode | S_IWUSR);
		lstat(inFile, inFileInfo);
	}
	if ((orig_mode & S_IRUSR) == 0) {
		chmod(inFile, orig_mode | S_IRUSR);
		lstat(inFile, inFileInfo);
	}

	xattrnamesize = listxattr(inFile, NULL, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);

	if (xattrnamesize > 0)
	{
		xattrnames = (char *) malloc(xattrnamesize);
		if (xattrnames == NULL)
		{
			fprintf(stderr, "%s: malloc error, unable to get file information\n", inFile);
			goto bail;
		}
		if ((xattrnamesize = listxattr(inFile, xattrnames, xattrnamesize, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW)) <= 0)
		{
			fprintf(stderr, "%s: listxattr: %s\n", inFile, strerror(errno));
			xfree(xattrnames);
			goto bail;
		}
		for (curr_attr = xattrnames; curr_attr < xattrnames + xattrnamesize; curr_attr += strlen(curr_attr) + 1)
		{
			if (strcmp(curr_attr, XATTR_RESOURCEFORK_NAME) == 0 && strlen(curr_attr) == 22)
			{
				inRFLen = getxattr(inFile, curr_attr, NULL, 0, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
				if (inRFLen < 0)
				{
					fprintf(stderr, "%s: getxattr: %s\n", inFile, strerror(errno));
					xfree(xattrnames);
					goto bail;
				}
				if (inRFLen != 0)
				{
					inBuf = malloc(inRFLen);
					if (inBuf == NULL)
					{
						fprintf(stderr, "%s: malloc error, unable to allocate input buffer\n", inFile);
						goto bail;
					}
					madvise(inBuf, inRFLen, MADV_SEQUENTIAL);
					do
					{
						getxattrret = getxattr(inFile, curr_attr, inBuf + RFpos, inRFLen - RFpos, RFpos, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
						if (getxattrret < 0)
						{
							fprintf(stderr, "getxattr: %s\n", strerror(errno));
							xfree(xattrnames);
							goto bail;
						}
						RFpos += getxattrret;
					} while (RFpos < inRFLen && getxattrret > 0);
				}
			}
			if (strcmp(curr_attr, DECMPFS_XATTR_NAME) == 0 && strlen(curr_attr) == 17)
			{
				indecmpfsLen = getxattr(inFile, curr_attr, NULL, 0, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
				if (indecmpfsLen < 0)
				{
					fprintf(stderr, "%s: getxattr: %s\n", inFile, strerror(errno));
					xfree(xattrnames);
					goto bail;
				}
				if (indecmpfsLen != 0)
				{
					indecmpfsBuf = malloc(indecmpfsLen);
					if (indecmpfsBuf == NULL)
					{
						fprintf(stderr, "%s: malloc error, unable to allocate xattr buffer\n", inFile);
						xfree(inBuf);
						goto bail;
					}
					if (indecmpfsLen != 0)
					{
						indecmpfsBuf = malloc(indecmpfsLen);
						if (indecmpfsBuf == NULL)
						{
							fprintf(stderr, "%s: malloc error, unable to get file information\n", inFile);
							goto bail;
						}
						indecmpfsLen = getxattr(inFile, curr_attr, indecmpfsBuf, indecmpfsLen, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
						if (indecmpfsLen < 0)
						{
							fprintf(stderr, "getxattr: %s\n", strerror(errno));
							xfree(xattrnames);
							goto bail;
						}
					}
				}
			}
		}
		xfree(xattrnames);
	}

	if (indecmpfsBuf == NULL)
	{
		fprintf(stderr, "%s: Decompression failed; file flags indicate file is compressed but it does not have a com.apple.decmpfs extended attribute\n", inFile);
		goto bail;
	}
	if (indecmpfsLen < sizeof(decmpfs_disk_header))
	{
		fprintf(stderr, "%s: Decompression failed; extended attribute com.apple.decmpfs is only %ld bytes (it is required to have a 16 byte header)\n", inFile, indecmpfsLen);
		goto bail;
	}

	decmpfs_disk_header *decmpfsAttr = (decmpfs_disk_header*) indecmpfsBuf;
	// filesize = EndianU64_LtoN(*(UInt64 *) (indecmpfsBuf + 8));
	filesize = OSSwapLittleToHostInt64(decmpfsAttr->uncompressed_size);
	if (filesize == 0)
	{
		fprintf(stderr, "%s: Decompression failed; file size given in header is 0\n", inFile);
		goto bail;
	}
	outBuf = malloc(filesize);
	if (outBuf == NULL)
	{
		fprintf(stderr, "%s: malloc error, unable to allocate output buffer (%lld bytes; %s)\n", inFile, filesize, strerror(errno));
		goto bail;
	}
	madvise(outBuf, filesize, MADV_SEQUENTIAL);

	bool removeResourceFork = false;
	bool doSimpleDecompression = false;
	int compressionType = OSSwapLittleToHostInt32(decmpfsAttr->compression_type);
	switch (compressionType) {
		// ZLIB decompression is handled in what can be seen as a reference implementation
		// that does the entire decompression explicitly in userland.
		// There is also a simplified approach which exploits the fact that the kernel
		// will decompress files transparently when reading their content. This approach
		// is used for LZFN and LZFSE decompression.
		// if (EndianU32_LtoN(*(UInt32 *) (indecmpfsBuf + 4)) == CMP_ZLIB_RESOURCE_FORK)
		case CMP_ZLIB_RESOURCE_FORK: {
			if (inBuf == NULL)
			{
				fprintf(stderr,
					"%s: Decompression failed; resource fork required for compression type %d but none exists\n",
					inFile, CMP_ZLIB_RESOURCE_FORK);
				goto bail;
			}
			if (inRFLen < 0x13A ||
				inRFLen < EndianU32_BtoN(*(UInt32 *) inBuf) + 0x4)
			{
				fprintf(stderr, "%s: Decompression failed; resource fork data is incomplete\n", inFile);
				if (inBuf != NULL)
					free(inBuf);
				if (indecmpfsBuf != NULL)
					free(indecmpfsBuf);
				goto bail;
			}

			blockStart = inBuf + EndianU32_BtoN(*(UInt32 *) inBuf) + 0x4;
			numBlocks = EndianU32_NtoL(*(UInt32 *) blockStart);
			
			if (inRFLen < EndianU32_BtoN(*(UInt32 *) inBuf) + 0x3A + (numBlocks * 8))
			{
				fprintf(stderr, "%s: Decompression failed; resource fork data is incomplete\n", inFile);
				goto bail;
			}
			if (compblksize * (numBlocks - 1) + (filesize % compblksize) > filesize)
			{
				fprintf(stderr, "%s: Decompression failed; file size given in header is incorrect\n", inFile);
				goto bail;
			}
			for (currBlock = 0; currBlock < numBlocks; currBlock++)
			{
				if (blockStart + EndianU32_LtoN(*(UInt32 *) (blockStart + 0x4 + (currBlock * 8))) + EndianU32_LtoN(*(UInt32 *) (blockStart + 0x8 + (currBlock * 8))) > inBuf + inRFLen)
				{
					fprintf(stderr, "%s: Decompression failed; resource fork data is incomplete\n", inFile);
					goto bail;
				}
				if (currBlock + 1 != numBlocks)
					uncmpedsize = compblksize;
				else
					uncmpedsize = (filesize - (currBlock * compblksize) < compblksize) ? filesize - (currBlock * compblksize) : compblksize;
				if ((compblksize * currBlock) + uncmpedsize > filesize)
				{
					fprintf(stderr, "%s: Decompression failed; file size given in header is incorrect\n", inFile);
					goto bail;
				}
				if ((*(unsigned char *) (blockStart + EndianU32_LtoN(*(UInt32 *) (blockStart + 0x4 + (currBlock * 8))))) == 0xFF)
				{
					uncmpedsize = EndianU32_LtoN(*(UInt32 *) (blockStart + 0x8 + (currBlock * 8))) - 1;
					memcpy(outBuf + (compblksize * currBlock), blockStart + EndianU32_LtoN(*(UInt32 *) (blockStart + 0x4 + (currBlock * 8))) + 1, uncmpedsize);
				}
				else
				{
					if ((uncmpret = uncompress(outBuf + (compblksize * currBlock), &uncmpedsize, blockStart + EndianU32_LtoN(*(UInt32 *) (blockStart + 0x4 + (currBlock * 8))), EndianU32_LtoN(*(UInt32 *) (blockStart + 0x8 + (currBlock * 8))))) != Z_OK)
					{
						if (uncmpret == Z_BUF_ERROR)
						{
							fprintf(stderr, "%s: Decompression failed; uncompressed data block too large\n", inFile);
							goto bail;
						}
						else if (uncmpret == Z_DATA_ERROR)
						{
							fprintf(stderr, "%s: Decompression failed; compressed data block is corrupted\n", inFile);
							goto bail;
						}
						else if (uncmpret == Z_MEM_ERROR)
						{
							fprintf(stderr, "%s: Decompression failed; out of memory\n", inFile);
							goto bail;
						}
						else
						{
							fprintf(stderr, "%s: Decompression failed; an error occurred during decompression\n", inFile);
							goto bail;
						}
					}
				}
				if (uncmpedsize != ((filesize - (currBlock * compblksize) < compblksize) ? filesize - (currBlock * compblksize) : compblksize))
				{
					fprintf(stderr, "%s: Decompression failed; uncompressed data block too small\n", inFile);
					goto bail;
				}
			}
			removeResourceFork = true;
			break;
		}
		// else if (EndianU32_LtoN(*(UInt32 *) (indecmpfsBuf + 4)) == CMP_ZLIB_XATTR)
		case CMP_ZLIB_XATTR: {
			const size_t dataStart = sizeof(decmpfs_disk_header) /* 0x10 */;
			if (indecmpfsLen == dataStart)
			{
				fprintf(stderr,
					"%s: Decompression failed; compression type %d expects compressed data in extended attribute com.apple.decmpfs but none exists\n",
					inFile, CMP_ZLIB_XATTR);
				goto bail;
			}
			uncmpedsize = filesize;
			if ((*(unsigned char *) (indecmpfsBuf + dataStart)) == 0xFF)
			{
				const size_t offset = dataStart + 1;
				uncmpedsize = indecmpfsLen - offset;
				memcpy(outBuf, indecmpfsBuf + offset, uncmpedsize);
			}
			else
			{
				if ((uncmpret = uncompress(outBuf, &uncmpedsize, indecmpfsBuf + dataStart, indecmpfsLen - dataStart)) != Z_OK)
				{
					if (uncmpret == Z_BUF_ERROR)
					{
						fprintf(stderr, "%s: Decompression failed; uncompressed data too large\n", inFile);
						goto bail;
					}
					else if (uncmpret == Z_DATA_ERROR)
					{
						fprintf(stderr, "%s: Decompression failed; compressed data is corrupted\n", inFile);
						goto bail;
					}
					else if (uncmpret == Z_MEM_ERROR)
					{
						fprintf(stderr, "%s: Decompression failed; out of memory\n", inFile);
						goto bail;
					}
					else
					{
						fprintf(stderr, "%s: Decompression failed; an error occurred during decompression\n", inFile);
						goto bail;
					}
				}
			}
			if (uncmpedsize != filesize)
			{
				fprintf(stderr, "%s: Decompression failed; uncompressed data block too small\n", inFile);
				goto bail;
			}
			break;
		}
		case CMP_LZVN_RESOURCE_FORK: {
			removeResourceFork = true;
		case CMP_LZVN_XATTR:
	        if (
#if __has_builtin(__builtin_available)
				// we can do simplified runtime OS version detection: accept LZVN on 10.9 and up.
				__builtin_available(macOS 10.9, *)
#else
				darwinMajor >= 13
#endif
			) {
				doSimpleDecompression = true;
			}
			else
			{
				fprintf(stderr, "%s: Decompression failed; unsupported compression type %s\n",
						inFile, compressionTypeName(compressionType));
				goto bail;
			}
			break;
		}
		case CMP_LZFSE_RESOURCE_FORK: {
			removeResourceFork = true;
		case CMP_LZFSE_XATTR:
	        if (
#if __has_builtin(__builtin_available)
				// we can do simplified runtime OS version detection: accept LZFSE on 10.11 and up.
				__builtin_available(macOS 10.11, *)
#else
				darwinMajor >= 15
#endif
			) {
				doSimpleDecompression = true;
			}
			else
			{
				fprintf(stderr, "%s: Decompression failed; unsupported compression type %s\n",
						inFile, compressionTypeName(compressionType));
				goto bail;
			}
			break;
		}
		default: {
			fprintf(stderr, "%s: Decompression failed; unknown compression type %s\n",
					inFile, compressionTypeName(compressionType));
			goto bail;
			break;
		}
	}

	if (doSimpleDecompression)
	{
		// the simple approach: let the kernel handle decompression
		// by reading the file content using fread(), into outBuf.
		// First, these can be freed already:
		xfree(inBuf);
		xfree(indecmpfsBuf);
		errno = 0;
		in = fopen(inFile, "r");
		if (in && fread(outBuf, filesize, 1, in) != 1) {
			fprintf(stderr, "%s: decompression failed: %s\n", inFile, strerror(errno));
			fclose(in);
			goto bail;
		}
		fclose(in);
	}

	if (chflags(inFile, (~UF_COMPRESSED) & inFileInfo->st_flags) < 0)
	{
		fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
		goto bail;
	}

	in = fopen(inFile, "r+");
	if (in == NULL)
	{
		if (chflags(inFile, UF_COMPRESSED & inFileInfo->st_flags) < 0)
			fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
		fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
		goto bail;
	}

	if (fwrite(outBuf, filesize, 1, in) != 1)
	{
		fprintf(stderr, "%s: Error writing to file (%lld bytes; %s)\n", inFile, filesize, strerror(errno));
		fclose(in);
		if (chflags(inFile, UF_COMPRESSED | inFileInfo->st_flags) < 0)
		{
			fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
		}
		goto bail;
	}

	fclose(in);

	if (removexattr(inFile, DECMPFS_XATTR_NAME, XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
	{
		fprintf(stderr, "%s: removexattr DECMPFS_XATTR: %s\n", inFile, strerror(errno));
	}
	if (removeResourceFork && 
		removexattr(inFile, XATTR_RESOURCEFORK_NAME, XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
	{
		fprintf(stderr, "%s: removexattr XATTR_RESOURCEFORK: %s\n", inFile, strerror(errno));
	}

bail:
	utimes(inFile, times);
	if (inFileInfo->st_mode != orig_mode) {
		chmod(inFile, orig_mode);
	}
	xfree(inBuf);
	xfree(indecmpfsBuf);
	xfree(outBuf);
#else
	// not much point doing a non-Apple implementation, even for testing
#endif
}

void add_extension_to_filetypeinfo(const char *filepath, struct filetype_info *filetypeinfo)
{
	long int right_pos, left_pos = 0, curr_pos = 1, i, fileextensionlen;
	const char *fileextension;
	
	for (i = strlen(filepath) - 1; i > 0; i--)
		if (filepath[i] == '.' || filepath[i] == '/')
			break;
	if (i != 0 && i != strlen(filepath) - 1 && filepath[i] != '/' && filepath[i-1] != '/')
		fileextension = &filepath[i+1];
	else
		return;
	
	if (filetypeinfo->extensions == NULL)
	{
		filetypeinfo->extensionssize = 1;
		filetypeinfo->extensions = (char **) malloc(filetypeinfo->extensionssize * sizeof(char *));
		if (filetypeinfo->extensions == NULL)
		{
			fprintf(stderr, "Malloc error allocating memory for list of file types, exiting...\n");
			exit(ENOMEM);
		}
	}
	
	if (filetypeinfo->numextensions > 0)
	{
		left_pos = 0;
		right_pos = filetypeinfo->numextensions + 1;
		
		while (strcasecmp(filetypeinfo->extensions[curr_pos-1], fileextension) != 0)
		{
			curr_pos = (right_pos - left_pos) / 2;
			if (curr_pos == 0) break;
			curr_pos += left_pos;
			if (strcasecmp(filetypeinfo->extensions[curr_pos-1], fileextension) > 0)
				right_pos = curr_pos;
			else if (strcasecmp(filetypeinfo->extensions[curr_pos-1], fileextension) < 0)
				left_pos = curr_pos;
		}
		if (curr_pos != 0 && strcasecmp(filetypeinfo->extensions[curr_pos-1], fileextension) == 0)
			return;
	}
	if (filetypeinfo->extensionssize < filetypeinfo->numextensions + 1)
	{
		filetypeinfo->extensionssize *= 2;
		filetypeinfo->extensions = (char **) realloc(filetypeinfo->extensions, filetypeinfo->extensionssize * sizeof(char *));
		if (filetypeinfo->extensions == NULL)
		{
			fprintf(stderr, "Malloc error allocating memory for list of file types, exiting...\n");
			exit(ENOMEM);
		}
	}
	if ((filetypeinfo->numextensions != 0) && ((filetypeinfo->numextensions - 1) >= left_pos))
		memmove(&filetypeinfo->extensions[left_pos+1], &filetypeinfo->extensions[left_pos], (filetypeinfo->numextensions - left_pos) * sizeof(char *));
	filetypeinfo->extensions[left_pos] = (char *) malloc(strlen(fileextension) + 1);
	strcpy(filetypeinfo->extensions[left_pos], fileextension);
	for (fileextensionlen = strlen(fileextension), i = 0; i < fileextensionlen; i++)
		filetypeinfo->extensions[left_pos][i] = tolower(filetypeinfo->extensions[left_pos][i]);
	filetypeinfo->numextensions++;
}

char* getFileType(const char *filepath)
{
#ifdef __APPLE__
	CFStringRef filepathCF = CFStringCreateWithCString(kCFAllocatorDefault, filepath, kCFStringEncodingUTF8);
	CFStringRef filetype;
	MDItemRef fileMDItem;
	char *filetypeMaxLenStr, *filetypeStr;
	UInt32 filetypeMaxLen;
	
	fileMDItem = MDItemCreate(kCFAllocatorDefault, filepathCF);
	CFRelease(filepathCF);
	if (fileMDItem == NULL)
		return NULL;
	filetype = (CFStringRef) MDItemCopyAttribute(fileMDItem, kMDItemContentType);
	CFRelease(fileMDItem);
	if (filetype == NULL)
		return NULL;
	filetypeMaxLen = CFStringGetMaximumSizeForEncoding(CFStringGetLength(filetype), kCFStringEncodingUTF8) + 1;
	filetypeMaxLenStr = (char *) malloc(filetypeMaxLen);
	if (!CFStringGetCString(filetype, filetypeMaxLenStr, filetypeMaxLen, kCFStringEncodingUTF8))
	{
		CFRelease(filetype);
		return NULL;
	}
	CFRelease(filetype);
	filetypeStr = (char *) malloc(strlen(filetypeMaxLenStr) + 1);
	if (filetypeStr == NULL) return NULL;
	strcpy(filetypeStr, filetypeMaxLenStr);
	free(filetypeMaxLenStr);
	
	return filetypeStr;
#else
	return strdup("aFile");
#endif
}

struct filetype_info* getFileTypeInfo(const char *filepath, const char *filetype, struct folder_info *folderinfo)
{
	long int right_pos, left_pos = 0, curr_pos = 1;
	
	if (filetype == NULL)
		return NULL;
	
	if (folderinfo->filetypes == NULL)
	{
		folderinfo->filetypessize = 1;
		folderinfo->filetypes = (struct filetype_info *) malloc(folderinfo->filetypessize * sizeof(struct filetype_info));
		if (folderinfo->filetypes == NULL)
		{
			fprintf(stderr, "Malloc error allocating memory for list of file types, exiting...\n");
			exit(ENOMEM);
		}
	}
	
	if (folderinfo->numfiletypes > 0)
	{
		left_pos = 0;
		right_pos = folderinfo->numfiletypes + 1;
		
		while (strcmp(folderinfo->filetypes[curr_pos-1].filetype, filetype) != 0)
		{
			curr_pos = (right_pos - left_pos) / 2;
			if (curr_pos == 0) break;
			curr_pos += left_pos;
			if (strcmp(folderinfo->filetypes[curr_pos-1].filetype, filetype) > 0)
				right_pos = curr_pos;
			else if (strcmp(folderinfo->filetypes[curr_pos-1].filetype, filetype) < 0)
				left_pos = curr_pos;
		}
		if (curr_pos != 0 && strcmp(folderinfo->filetypes[curr_pos-1].filetype, filetype) == 0)
		{
			add_extension_to_filetypeinfo(filepath, &folderinfo->filetypes[curr_pos-1]);
			return &folderinfo->filetypes[curr_pos-1];
		}
	}
	if (folderinfo->filetypessize < folderinfo->numfiletypes + 1)
	{
		folderinfo->filetypessize *= 2;
		folderinfo->filetypes = (struct filetype_info *) realloc(folderinfo->filetypes, folderinfo->filetypessize * sizeof(struct filetype_info));
		if (folderinfo->filetypes == NULL)
		{
			fprintf(stderr, "Malloc error allocating memory for list of file types, exiting...\n");
			exit(ENOMEM);
		}
	}
	if ((folderinfo->numfiletypes != 0) && ((folderinfo->numfiletypes - 1) >= left_pos))
		memmove(&folderinfo->filetypes[left_pos+1], &folderinfo->filetypes[left_pos], (folderinfo->numfiletypes - left_pos) * sizeof(struct filetype_info));
	folderinfo->filetypes[left_pos].filetype = (char *) malloc(strlen(filetype) + 1);
	strcpy(folderinfo->filetypes[left_pos].filetype, filetype);
	folderinfo->filetypes[left_pos].extensions = NULL;
	folderinfo->filetypes[left_pos].extensionssize = 0;
	folderinfo->filetypes[left_pos].numextensions = 0;
	folderinfo->filetypes[left_pos].uncompressed_size = 0;
	folderinfo->filetypes[left_pos].uncompressed_size_rounded = 0;
	folderinfo->filetypes[left_pos].compressed_size = 0;
	folderinfo->filetypes[left_pos].compressed_size_rounded = 0;
	folderinfo->filetypes[left_pos].compattr_size = 0;
	folderinfo->filetypes[left_pos].total_size = 0;
	folderinfo->filetypes[left_pos].num_compressed = 0;
	folderinfo->filetypes[left_pos].num_files = 0;
	folderinfo->filetypes[left_pos].num_hard_link_files = 0;
	add_extension_to_filetypeinfo(filepath, &folderinfo->filetypes[left_pos]);
	folderinfo->numfiletypes++;
	return &folderinfo->filetypes[left_pos];
}

void printFileInfo(const char *filepath, struct stat *fileinfo, bool appliedcomp, bool onAPFS)
{
	char *xattrnames, *curr_attr, *filetype;
	ssize_t xattrnamesize, xattrssize = 0, xattrsize, RFsize = 0, compattrsize = 0;
	long long int filesize, filesize_rounded, filesize_reported = 0;
	int numxattrs = 0, numhiddenattr = 0;
	UInt32 compressionType = 0;
	bool hasRF = FALSE;
	
	printf("%s:\n", filepath);

#ifdef __APPLE__
	xattrnamesize = listxattr(filepath, NULL, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
	
	if (xattrnamesize > 0)
	{
		xattrnames = (char *) malloc(xattrnamesize);
		if (xattrnames == NULL)
		{
			fprintf(stderr, "malloc error, unable to get file information\n");
			return;
		}
		if ((xattrnamesize = listxattr(filepath, xattrnames, xattrnamesize, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW)) <= 0)
		{
			fprintf(stderr, "listxattr: %s\n", strerror(errno));
			free(xattrnames);
			return;
		}
		for (curr_attr = xattrnames; curr_attr < xattrnames + xattrnamesize; curr_attr += strlen(curr_attr) + 1)
		{
			xattrsize = getxattr(filepath, curr_attr, NULL, 0, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
			if (xattrsize < 0)
			{
				fprintf(stderr, "getxattr: %s\n", strerror(errno));
				free(xattrnames);
				return;
			}
			numxattrs++;
			if (strcmp(curr_attr, XATTR_RESOURCEFORK_NAME) == 0 && strlen(curr_attr) == 22)
			{
				RFsize += xattrsize;
				hasRF = TRUE;
				numhiddenattr++;
			}
			else if (strcmp(curr_attr, DECMPFS_XATTR_NAME) == 0 && strlen(curr_attr) == 17)
			{
				compattrsize += xattrsize;
				numhiddenattr++;
				if (xattrsize > 0) {
					void *indecmpfsBuf = calloc(xattrsize, 1);
					if (indecmpfsBuf) {
						ssize_t indecmpfsLen = getxattr(filepath, curr_attr, indecmpfsBuf, xattrsize, 
														0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
						if (indecmpfsLen >= 0) {
							if ((*(UInt32 *) indecmpfsBuf) != EndianU32_NtoL(DECMPFS_MAGIC)) {
								fprintf( stderr, "Warning: \"%s\" has unknown compression magic 0x%x\n",
										 filepath, *(UInt32 *) indecmpfsBuf );
							}
							compressionType = EndianU32_LtoN(*(UInt32 *) (indecmpfsBuf + 4));
							filesize_reported = EndianU64_LtoN(*(UInt64 *) (indecmpfsBuf + 8));
						}
						free(indecmpfsBuf);
					}
				}
			}
			else
				xattrssize += xattrsize;
		}
		free(xattrnames);
	}
#endif

	if ((int)onAPFS == -1) {
		fileIsCompressable(filepath, fileinfo, compressionType, &onAPFS);
	}

#ifdef __APPLE__
	if ((fileinfo->st_flags & UF_COMPRESSED) == 0)
#else
	if (true)
#endif
	{
		if (appliedcomp)
			printf("Unable to compress file.\n");
		else
			printf("File is not HFS+/APFS compressed.\n");
		if ((filetype = getFileType(filepath)) != NULL)
		{
			printf("File content type: %s\n", filetype);
			free(filetype);
		}
		if (hasRF)
		{
			printf("File data fork size: %lld bytes\n", (long long) fileinfo->st_size);
			printf("File resource fork size: %ld bytes\n", RFsize);
            if (compattrsize && printVerbose > 2)
                printf("File DECMPFS attribute size: %ld bytes\n", compattrsize);
			filesize = fileinfo->st_size;
			filesize_rounded = roundToBlkSize(filesize, fileinfo);
			filesize += RFsize;
			filesize_rounded = roundToBlkSize(filesize_rounded + RFsize, fileinfo);
			printf("File size (data fork + resource fork; reported size by Mac OS X Finder): %s\n",
				   getSizeStr(filesize, filesize_rounded, 1));
		}
		else
		{
            if (compattrsize && printVerbose > 2)
                printf("File DECMPFS attribute size: %ld bytes\n", compattrsize);
			filesize = fileinfo->st_size;
			filesize_rounded = roundToBlkSize(filesize, fileinfo);
			printf("File data fork size (reported size by Mac OS X Finder): %s\n",
				   getSizeStr(filesize, filesize_rounded, 1));
		}
		printf("Number of extended attributes: %d\n", numxattrs - numhiddenattr);
		printf("Total size of extended attribute data: %ld bytes\n", xattrssize);
		filesize = roundToBlkSize(fileinfo->st_size, fileinfo);
		filesize = roundToBlkSize(filesize + RFsize, fileinfo);
		filesize += compattrsize + xattrssize;
		if (!onAPFS) {
			printf("Approximate overhead of extended HFS+ attributes: %ld bytes\n", ((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey));
			filesize += (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFile);
		}
		printf("Approximate total file size (data fork + resource fork + EA + EA overhead + file overhead): %s\n", 
			   getSizeStr(filesize, filesize, 0));

	}
	else
	{
		if (!appliedcomp)
			printf("File is HFS+/APFS compressed.\n");
		switch (compressionType) {
			case CMP_ZLIB_XATTR:
			case CMP_ZLIB_RESOURCE_FORK:
				printf("Compression type: %s\n", compressionTypeName(compressionType));
				break;
			case CMP_LZVN_XATTR:
			case CMP_LZVN_RESOURCE_FORK:
				printf("Compression type: %s\n", compressionTypeName(compressionType));
				break;
			case CMP_LZFSE_XATTR:
			case CMP_LZFSE_RESOURCE_FORK:
				printf("Compression type: %s\n", compressionTypeName(compressionType));
				break;
			default:
				printf("Unknown compression type: %u\n", compressionType);
				break;
		}
		if ((filetype = getFileType(filepath)) != NULL)
		{
			printf("File content type: %s\n", filetype);
			free(filetype);
		}
		if (printVerbose > 2)
		{
			if (RFsize)
				printf("File resource fork size: %ld bytes\n", RFsize);
	        if (compattrsize)
	            printf("File DECMPFS attribute size: %ld bytes\n", compattrsize);
		}
		filesize = fileinfo->st_size;
		printf("File size (uncompressed; reported size by Mac OS 10.6+ Finder): %s\n",
			   getSizeStr(filesize, filesize, 1));
		if (legacy_output) {
			filesize = RFsize;
			filesize_rounded = roundToBlkSize(filesize, fileinfo);
			printf("File size (compressed data fork - decmpfs xattr; reported size by Mac OS 10.0-10.5 Finder): %s\n",
				   getSizeStr(filesize, filesize_rounded, 1));
		}
		// report the actual file-on-disk size
		filesize = fileinfo->st_blocks * S_BLKSIZE;
		filesize_rounded = roundToBlkSize(filesize, fileinfo);
		printf("File size (compressed): %s\n", getSizeStr(filesize, filesize_rounded, 0));
		printf("Compression savings: %0.1f%%\n", (1.0 - (((double) filesize) / fileinfo->st_size)) * 100.0);
		printf("Number of extended attributes: %d\n", numxattrs - numhiddenattr);
		printf("Total size of extended attribute data: %ld bytes\n", xattrssize);
		if (!onAPFS) {
			printf("Approximate overhead of extended attributes: %ld bytes\n", ((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey));
		}
		if (filesize_reported) {
			printf("Uncompressed file size reported in compressed header: %lld bytes\n", filesize_reported);
		}
	}
}

long long process_file_info(const char *filepath, const char *filetype, struct stat *fileinfo, struct folder_info *folderinfo)
{
	char *xattrnames, *curr_attr;
	const char *fileextension = NULL;
	ssize_t xattrnamesize, xattrssize = 0, xattrsize, RFsize = 0, compattrsize = 0;
	long long int filesize, filesize_rounded, ret;
	int numxattrs = 0, numhiddenattr = 0, i;
	struct filetype_info *filetypeinfo = NULL;
	bool filetype_found = FALSE;
	
	if (quitRequested)
	{
		return 0;
	}

#ifdef __APPLE__
	xattrnamesize = listxattr(filepath, NULL, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
	
	if (xattrnamesize > 0)
	{
		xattrnames = (char *) malloc(xattrnamesize);
		if (xattrnames == NULL)
		{
			fprintf(stderr, "malloc error, unable to get file information\n");
			return 0;
		}
		if ((xattrnamesize = listxattr(filepath, xattrnames, xattrnamesize, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW)) <= 0)
		{
			fprintf(stderr, "listxattr: %s\n", strerror(errno));
			free(xattrnames);
			return 0;
		}
		for (curr_attr = xattrnames; curr_attr < xattrnames + xattrnamesize; curr_attr += strlen(curr_attr) + 1)
		{
			xattrsize = getxattr(filepath, curr_attr, NULL, 0, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
			if (xattrsize < 0)
			{
				fprintf(stderr, "getxattr: %s\n", strerror(errno));
				free(xattrnames);
				return 0;
			}
			numxattrs++;
			if (strcmp(curr_attr, XATTR_RESOURCEFORK_NAME) == 0 && strlen(curr_attr) == 22)
			{
				RFsize += xattrsize;
				numhiddenattr++;
			}
			else if (strcmp(curr_attr, DECMPFS_XATTR_NAME) == 0 && strlen(curr_attr) == 17)
			{
				compattrsize += xattrsize;
				numhiddenattr++;
			}
			else
				xattrssize += xattrsize;
		}
		free(xattrnames);
	}
#endif

	folderinfo->num_files++;
	
	if (folderinfo->filetypeslist != NULL && filetype != NULL)
	{
		for (i = strlen(filepath) - 1; i > 0; i--)
			if (filepath[i] == '.' || filepath[i] == '/')
				break;
		if (i != 0 && i != strlen(filepath) - 1 && filepath[i] != '/' && filepath[i-1] != '/')
			fileextension = &filepath[i+1];
		for (i = 0; i < folderinfo->filetypeslistlen; i++)
			if (strcmp(folderinfo->filetypeslist[i], filetype) == 0 ||
				strcmp("ALL", folderinfo->filetypeslist[i]) == 0 ||
				(fileextension != NULL && strcasecmp(fileextension, folderinfo->filetypeslist[i]) == 0))
				filetype_found = TRUE;
	}
	if (folderinfo->filetypeslist != NULL && filetype_found)
		filetypeinfo = getFileTypeInfo(filepath, filetype, folderinfo);
	if (filetype_found && filetypeinfo != NULL) filetypeinfo->num_files++;

#ifdef __APPLE__
	if ((fileinfo->st_flags & UF_COMPRESSED) == 0)
#endif
	{
		ret = filesize_rounded = filesize = fileinfo->st_size;
		filesize_rounded = roundToBlkSize(filesize_rounded, fileinfo);
		filesize += RFsize;
		filesize_rounded = roundToBlkSize(filesize_rounded + RFsize, fileinfo);
		folderinfo->uncompressed_size += filesize;
		folderinfo->uncompressed_size_rounded += filesize_rounded;
		folderinfo->compressed_size += filesize;
		folderinfo->compressed_size_rounded += filesize_rounded;
		if (filetypeinfo != NULL && filetype_found)
		{
			filetypeinfo->uncompressed_size += filesize;
			filetypeinfo->uncompressed_size_rounded += filesize_rounded;
			filetypeinfo->compressed_size += filesize;
			filetypeinfo->compressed_size_rounded += filesize_rounded;
		}
		filesize = roundToBlkSize(fileinfo->st_size, fileinfo);
		filesize = roundToBlkSize(filesize + RFsize, fileinfo);
		filesize += compattrsize + xattrssize;
		if (!folderinfo->onAPFS) {
			filesize += (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFile);
		}
		folderinfo->total_size += filesize;
		if (filetypeinfo != NULL && filetype_found)
			filetypeinfo->total_size += filesize;
	}
#ifdef __APPLE__
	else
	{
		if (folderinfo->print_files)
		{
			if (folderinfo->print_info > 1)
			{
				printf("%s:\n", filepath);
				if (filetype != NULL)
					printf("File content type: %s\n", filetype);
				filesize = fileinfo->st_size;
				printf("File size (uncompressed data fork; reported size by Mac OS 10.6+ Finder): %s\n",
					   getSizeStr(filesize, filesize, 1));
				if (legacy_output) {
					filesize = RFsize;
					filesize_rounded = roundToBlkSize(filesize, fileinfo);
					printf("File size (compressed data fork - decmpfs xattr; reported size by Mac OS 10.0-10.5 Finder): %s\n",
						   getSizeStr(filesize, filesize_rounded, 1));
				}
				filesize = RFsize;
				filesize_rounded = roundToBlkSize(filesize, fileinfo);
				filesize += compattrsize;
				filesize_rounded += compattrsize;
				printf("File size (compressed data fork): %s\n", getSizeStr(filesize, filesize_rounded, 0));
				// on-disk file size:
				filesize = fileinfo->st_blocks * S_BLKSIZE;
				printf("Compression savings: %0.1f%%\n", (1.0 - (((double) filesize) / fileinfo->st_size)) * 100.0);
				printf("Number of extended attributes: %d\n", numxattrs - numhiddenattr);
				printf("Total size of extended attribute data: %ld bytes\n", xattrssize);
				if (!folderinfo->onAPFS) {
					printf("Approximate overhead of extended attributes: %ld bytes\n", ((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey));
				}
			}
			else if (!folderinfo->compress_files)
			{
				printf("%s\n", filepath);
			}
		}

		filesize = fileinfo->st_size;
		filesize_rounded = roundToBlkSize(filesize, fileinfo);
		folderinfo->uncompressed_size += filesize;
		folderinfo->uncompressed_size_rounded += filesize_rounded;
		if (filetypeinfo != NULL && filetype_found)
		{
			filetypeinfo->uncompressed_size += filesize;
			filetypeinfo->uncompressed_size_rounded += filesize_rounded;
		}
		ret = filesize = fileinfo->st_blocks * S_BLKSIZE;
		filesize_rounded = roundToBlkSize(filesize, fileinfo);
		folderinfo->compressed_size += filesize;
		folderinfo->compressed_size_rounded += filesize_rounded;
		folderinfo->compattr_size += compattrsize;
		if (filetypeinfo != NULL && filetype_found)
		{
			filetypeinfo->compressed_size += filesize;
			filetypeinfo->compressed_size_rounded += filesize_rounded;
			filetypeinfo->compattr_size += compattrsize;
		}
// 		filesize = roundToBlkSize(RFsize, fileinfo);
// 		filesize += compattrsize + xattrssize;
// 		if (!folderinfo->onAPFS) {
// 			filesize += (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFile);
// 		}
		folderinfo->total_size += filesize;
		folderinfo->num_compressed++;
		if (filetypeinfo != NULL && filetype_found)
		{
			filetypeinfo->total_size += filesize;
			filetypeinfo->num_compressed++;
		}
	}
#endif
	return ret;
}

void printFolderInfo(struct folder_info *folderinfo, bool hardLinkCheck)
{
	long long foldersize, foldersize_rounded;

	printf("Total number of files: %lld\n", folderinfo->num_files);
	if (hardLinkCheck)
		printf("Total number of file hard links: %lld\n", folderinfo->num_hard_link_files);
	printf("Total number of folders: %lld\n", folderinfo->num_folders);
	if (hardLinkCheck)
		printf("Total number of folder hard links: %lld\n", folderinfo->num_hard_link_folders);
	printf("Total number of items (number of files + number of folders): %lld\n", folderinfo->num_files + folderinfo->num_folders);
	foldersize = folderinfo->uncompressed_size;
	foldersize_rounded = folderinfo->uncompressed_size_rounded;
	if ((folderinfo->num_hard_link_files == 0 && folderinfo->num_hard_link_folders == 0) || !hardLinkCheck)
		printf("Folder size (uncompressed; reported size by Mac OS 10.6+ Finder): %s\n",
			   getSizeStr(foldersize, foldersize_rounded, 1));
	else
		printf("Folder size (uncompressed): %s\n",
			   getSizeStr(foldersize, foldersize_rounded, 0));
	foldersize = folderinfo->compressed_size;
	foldersize_rounded = folderinfo->compressed_size_rounded;
	if (legacy_output
			&& ((folderinfo->num_hard_link_files == 0 && folderinfo->num_hard_link_folders == 0) || !hardLinkCheck)) {
		printf("Folder size (compressed; reported size by Mac OS 10.0-10.5 Finder): %s\n",
			   getSizeStr(foldersize, foldersize_rounded, 1));
	} else {
		printf("Folder size (compressed): %s\n", getSizeStr(foldersize, foldersize_rounded, 0));
	}
	if (folderinfo->num_compressed) {
		// There's difference between parallel mode and serial mode here: in the latter num_files
		// is always the total number of files that were seen. In the former, it's the total
		// number of compressable (i.e. processed) files.
		printf("Compression savings: %0.1f%% over %lld of %lld %sfiles\n",
			   (1.0 - ((float) (folderinfo->compressed_size) / folderinfo->uncompressed_size)) * 100.0,
			   folderinfo->num_compressed, folderinfo->num_files,
			   PP ? "processed " : "");
	}
	foldersize = folderinfo->total_size;
	printf("Approximate total folder size (files + file overhead + folder overhead): %s\n",
		   getSizeStr(foldersize, foldersize, 0));
}

void process_folder(FTS *currfolder, struct folder_info *folderinfo)
{
	FTSENT *currfile;
	char *xattrnames, *curr_attr, *filetype = NULL, *fileextension;
	ssize_t xattrnamesize, xattrssize, xattrsize;
	int numxattrs, i;
	bool volume_search, filetype_found;
	struct filetype_info *filetypeinfo = NULL;
	
	currfile = fts_read(currfolder);
	if (currfile == NULL)
	{
		fts_close(currfolder);
		return;
	}
	volume_search = (strncasecmp("/Volumes/", currfile->fts_path, 9) == 0 && strlen(currfile->fts_path) >= 8);
	
	do
	{
		if (!quitRequested
			&& (volume_search || strncasecmp("/Volumes/", currfile->fts_path, 9) != 0 || strlen(currfile->fts_path) < 9)
			&& (strncasecmp("/dev/", currfile->fts_path, 5) != 0 || strlen(currfile->fts_path) < 5))
		{
			if (S_ISDIR(currfile->fts_statp->st_mode) && currfile->fts_ino != 2)
			{
				if (currfile->fts_info & FTS_D)
				{
					if (!folderinfo->check_hard_links || !checkForHardLink(currfile->fts_path, currfile->fts_statp, folderinfo))
					{
						numxattrs = 0;
						xattrssize = 0;

#ifdef __APPLE__
						xattrnamesize = listxattr(currfile->fts_path, NULL, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
						
						if (xattrnamesize > 0)
						{
							xattrnames = (char *) malloc(xattrnamesize);
							if (xattrnames == NULL)
							{
								fprintf(stderr, "malloc error, unable to get folder information\n");
								continue;
							}
							if ((xattrnamesize = listxattr(currfile->fts_path, xattrnames, xattrnamesize, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW)) <= 0)
							{
								fprintf(stderr, "listxattr: %s\n", strerror(errno));
								free(xattrnames);
								continue;
							}
							for (curr_attr = xattrnames; curr_attr < xattrnames + xattrnamesize; curr_attr += strlen(curr_attr) + 1)
							{
								xattrsize = getxattr(currfile->fts_path, curr_attr, NULL, 0, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
								if (xattrsize < 0)
								{
									fprintf(stderr, "getxattr: %s\n", strerror(errno));
									continue;
								}
								numxattrs++;
								xattrssize += xattrsize;
							}
							free(xattrnames);
						}
						folderinfo->total_size += xattrssize;
						if (!folderinfo->onAPFS) {
							folderinfo->total_size += (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFolder);
						}
#endif
						folderinfo->num_folders++;
					}
					else
					{
						folderinfo->num_hard_link_folders++;
						fts_set(currfolder, currfile, FTS_SKIP);
						
						folderinfo->num_folders++;
#ifdef __APPLE__
						if (!folderinfo->onAPFS) {
							folderinfo->total_size += sizeof(HFSPlusCatalogFolder);
						}
#endif
					}
				}
			}
			else if (S_ISREG(currfile->fts_statp->st_mode) || S_ISLNK(currfile->fts_statp->st_mode))
			{
				filetype_found = FALSE;
				if (folderinfo->filetypeslist != NULL)
				{
					filetype = getFileType(currfile->fts_path);
					if (filetype == NULL)
					{
						filetype = (char *) malloc(10);
						strcpy(filetype, "UNDEFINED");
					}
				}
				if (filetype != NULL)
				{
					fileextension = NULL;
					for (i = strlen(currfile->fts_path) - 1; i > 0; i--)
						if (currfile->fts_path[i] == '.')
							break;
					if (i != 0 && i != strlen(currfile->fts_path) - 1 && currfile->fts_path[i] != '/' && currfile->fts_path[i-1] != '/')
						fileextension = &currfile->fts_path[i+1];
					for (i = 0; i < folderinfo->filetypeslistlen; i++)
						if (strcmp(folderinfo->filetypeslist[i], filetype) == 0 ||
							strcmp("ALL", folderinfo->filetypeslist[i]) == 0 ||
							(fileextension != NULL && strcasecmp(fileextension, folderinfo->filetypeslist[i]) == 0))
							filetype_found = TRUE;
					if (folderinfo->invert_filetypelist)
					{
						if (filetype_found)
							filetype_found = FALSE;
						else
							filetype_found = TRUE;
					}
				}
				
				if (!folderinfo->check_hard_links || !checkForHardLink(currfile->fts_path, currfile->fts_statp, folderinfo))
				{
					if (folderinfo->compress_files && S_ISREG(currfile->fts_statp->st_mode))
					{
						if (folderinfo->filetypeslist == NULL || filetype_found)
						{
#ifdef SUPPORT_PARALLEL
							if (PP)
							{
								if (fileIsCompressable(currfile->fts_path, currfile->fts_statp, folderinfo->compressiontype, &folderinfo->onAPFS))
									addFileToParallelProcessor( PP, currfile->fts_path, currfile->fts_statp, folderinfo, false );
								else
									process_file_info(currfile->fts_path, NULL, currfile->fts_statp, getParallelProcessorJobInfo(PP));
							}
							else
#endif
							{
								compressFile(currfile->fts_path, currfile->fts_statp, folderinfo, NULL);
							}
						}
#ifdef __APPLE__
						lstat(currfile->fts_path, currfile->fts_statp);
						if (((currfile->fts_statp->st_flags & UF_COMPRESSED) == 0) && folderinfo->print_files)
						{
							if (folderinfo->print_info > 0) {
								printf("Unable to compress: ");
								printf("%s\n", currfile->fts_path);
							}
						}
#endif
					}
					process_file_info(currfile->fts_path, filetype, currfile->fts_statp, folderinfo);
				}
				else
				{
					folderinfo->num_hard_link_files++;
					
					folderinfo->num_files++;
					if (!folderinfo->onAPFS) {
						folderinfo->total_size += sizeof(HFSPlusCatalogFile);
					}
					if (filetype_found && (filetypeinfo = getFileTypeInfo(currfile->fts_path, filetype, folderinfo)) != NULL)
					{
						filetypeinfo->num_hard_link_files++;
						
						filetypeinfo->num_files++;
						if (!folderinfo->onAPFS) {
							filetypeinfo->total_size += sizeof(HFSPlusCatalogFile);
						}
					}
				}
				if (filetype != NULL) free(filetype);
			}
		}
		else
			fts_set(currfolder, currfile, FTS_SKIP);
	} while (!quitRequested && (currfile = fts_read(currfolder)) != NULL);
	checkForHardLink(NULL, NULL, NULL);
	fts_close(currfolder);
}

void printUsage()
{
	printf( AFSCTOOL_PROG_NAME " %s\n"
		   "Report if file is HFS+/APFS compressed:                   " AFSCTOOL_PROG_NAME " [-v] file[s]\n"
		   "Report if folder contains HFS+/APFS compressed files:     " AFSCTOOL_PROG_NAME " [-fvv[v]i] [-t <ContentType/Extension>] folder[s]\n"
		   "List HFS+/APFS compressed files in folder:                " AFSCTOOL_PROG_NAME " -l[fvv[v]] folder\n"
		   "Decompress HFS+/APFS compressed file or folder:           " AFSCTOOL_PROG_NAME " -d[i] [-t <ContentType>] file[s]/folder[s]\n"
		   "\tNB: this removes the entire resource fork from the specified files!\n"
		   "Create archive file with compressed data in data fork:    " AFSCTOOL_PROG_NAME " -a[d] src dst [... srcN dstN]\n"
		   "Extract HFS+/APFS compression archive to file:            " AFSCTOOL_PROG_NAME " -x[d] src dst [... srcN dstN]\n"
#ifdef SUPPORT_PARALLEL
		   "Apply HFS+/APFS compression to file or folder:            " AFSCTOOL_PROG_NAME " -c[nlfvv[v]ib] [-jN|-JN] [-S [-RM] ] [-<level>] [-m <size>] [-s <percentage>] [-t <ContentType>] [-T compressor] file[s]/folder[s]\n\n"
#else
		   "Apply HFS+/APFS compression to file or folder:            " AFSCTOOL_PROG_NAME " -c[nlfvv[v]ib] [-<level>] [-m <size>] [-s <percentage>] [-t <ContentType>] [-T compressor] file[s]/folder[s]\n\n"
#endif
		   "Options:\n"
		   "-v Increase verbosity level\n"
		   "-f Detect hard links\n"
		   "-l List files that are HFS+/APFS compressed (or if the -c option is given, files which fail to compress)\n"
		   "-L Allow larger-than-raw compressed chunks (not recommended; always true for LZVN compression)\n"
		   "-n Do not verify files after compression (not recommended)\n"
		   "-m <size> Largest file size to compress, in bytes\n"
		   "-s <percentage> For compression to be applied, compression savings must be at least this percentage\n"
		   "-t <ContentType/Extension> Return statistics for files of given content type and when compressing,\n"
		   "                           if this option is given then only files of content type(s) or extension(s) specified with this option will be compressed\n"
		   "-i Compress or show statistics for files that don't have content type(s) or extension(s) given by -t <ContentType/Extension> instead of those that do\n"
		   "-b make a backup of files before compressing them\n"
#ifdef SUPPORT_PARALLEL
		   "-jN compress (only compressable) files using <N> threads (compression is concurrent, disk IO is exclusive)\n"
		   "-JN read, compress and write files (only compressable ones) using <N> threads (everything is concurrent except writing the compressed file)\n"
		   "-S sort the item list by file size (leaving the largest files to the end may be beneficial if the target volume is almost full)\n"
		   "-RM <M> of the <N> workers will work the item list (must be sorted!) in reverse order, starting with the largest files\n"
#endif
		   "-T <compressor> Compression type to use: ZLIB (= types 3,4), LZVN (= types 7,8), or LZFSE (= types 11,12)\n"
		   "-<level> Compression level to use when compressing (ZLIB only; ranging from 1 to 9, with 1 being the fastest and 9 being the best - default is 5)\n"
		  , AFSCTOOL_FULL_VERSION_STRING);
}

#ifndef SUPPORT_PARALLEL
int main (int argc, const char * argv[])
#else
int afsctool (int argc, const char * argv[])
#endif
{
	int i, j;
	struct stat fileinfo, dstfileinfo;
	struct folder_info folderinfo;
	struct filetype_info alltypesinfo;
	FTS *currfolder;
	FTSENT *currfile;
	char *folderarray[2], *fullpath = NULL, *fullpathdst = NULL, *cwd, *fileextension, *filetype = NULL;
	int compressionlevel = 5;
	compression_type compressiontype = ZLIB;
	double minSavings = 0.0;
	long long int filesize, filesize_rounded, maxSize = 0;
	bool printDir = FALSE, decomp = FALSE, createfile = FALSE, extractfile = FALSE, applycomp = FALSE,
		fileCheck = TRUE, argIsFile, hardLinkCheck = FALSE, dstIsFile, free_src = FALSE, free_dst = FALSE,
		invert_filetypelist = FALSE, allowLargeBlocks = FALSE, filetype_found, backupFile = FALSE;
	FILE *afscFile, *outFile;
	char *xattrnames, *curr_attr, header[4];
	ssize_t xattrnamesize, xattrsize, getxattrret, xattrPos;
	mode_t outFileMode;
	void *attr_buf;
	UInt16 big16;
	UInt64 big64;
	int nJobs = 0, nReverse = 0;
	bool sortQueue = false;
	bool ppJobInfoInitialised = false;

	folderinfo.filetypeslist = NULL;
	folderinfo.filetypeslistlen = 0;
	folderinfo.filetypeslistsize = 0;
	
	if (argc < 2)
	{
		printUsage();
		exit(EINVAL);
	}

#if !__has_builtin(__builtin_available)
#	warning "Please use clang 5 or newer if you can"
	// determine the Darwin major version number
	{
		FILE *uname = popen("uname -r", "r");
		if (uname)
		{
			fscanf(uname, "%d", &darwinMajor);
		}
	}
#endif

	for (i = 1; i < argc && argv[i][0] == '-'; i++)
	{
		for (j = 1; j < strlen(argv[i]); j++)
		{
			switch (argv[i][j])
			{
				case 'l':
					if (createfile || extractfile || decomp)
					{
						printUsage();
						exit(EINVAL);
					}
					printDir = TRUE;
					break;
				case 'L':
					if (createfile || extractfile || decomp)
					{
						printUsage();
						exit(EINVAL);
					}
					allowLargeBlocks = TRUE;
					break;
				case 'v':
					printVerbose++;
					break;
				case 'd':
					if (printDir || applycomp || hardLinkCheck)
					{
						printUsage();
						exit(EINVAL);
					}
					decomp = TRUE;
					break;
				case 'a':
					if (printDir || extractfile || applycomp || hardLinkCheck)
					{
						printUsage();
						exit(EINVAL);
					}
					createfile = TRUE;
					break;
				case 'x':
					if (printDir || createfile || applycomp || hardLinkCheck)
					{
						printUsage();
						exit(EINVAL);
					}
					extractfile = TRUE;
					break;
				case 'c':
					if (createfile || extractfile || decomp)
					{
						printUsage();
						exit(EINVAL);
					}
					applycomp = TRUE;
					break;
				case 'k':
					// this flag is obsolete and no longer does anything, but it is kept here for backward compatibility with scripts that use it
					break;
				case 'n':
					if (createfile || extractfile || decomp)
					{
						printUsage();
						exit(EINVAL);
					}
					fileCheck = FALSE;
					break;
				case 'f':
					if (createfile || extractfile || decomp)
					{
						printUsage();
						exit(EINVAL);
					}
					hardLinkCheck = TRUE;
					break;
				case '1':
				case '2':
				case '3':
				case '4':
				case '5':
				case '6':
				case '7':
				case '8':
				case '9':
					if (createfile || extractfile || decomp)
					{
						printUsage();
						exit(EINVAL);
					}
					compressionlevel = argv[i][j] - '0';
					break;
				case 'm':
					if (createfile || extractfile || decomp || j + 1 < strlen(argv[i]) || i + 2 > argc)
					{
						printUsage();
						exit(EINVAL);
					}
					i++;
					sscanf(argv[i], "%lld", &maxSize);
					j = strlen(argv[i]) - 1;
					break;
				case 's':
					if (createfile || extractfile || decomp || j + 1 < strlen(argv[i]) || i + 2 > argc)
					{
						printUsage();
						exit(EINVAL);
					}
					i++;
					sscanf(argv[i], "%lf", &minSavings);
					if (minSavings > 99 || minSavings < 0)
					{
						fprintf(stderr, "Invalid minimum savings percentage; must be a number from 0 to 99\n");
						exit(EINVAL);
					}
					j = strlen(argv[i]) - 1;
					break;
				case 't':
					if (j + 1 < strlen(argv[i]) || i + 2 > argc)
					{
						printUsage();
						exit(EINVAL);
					}
					i++;
					if (folderinfo.filetypeslist == NULL)
					{
						folderinfo.filetypeslistsize = 1;
						folderinfo.filetypeslist = (char **) malloc(folderinfo.filetypeslistlen * sizeof(char *));
					}
					if (folderinfo.filetypeslistlen + 1 > folderinfo.filetypeslistsize)
					{
						folderinfo.filetypeslistsize *= 2;
						folderinfo.filetypeslist = (char **) realloc(folderinfo.filetypeslist, folderinfo.filetypeslistsize * sizeof(char *));
					}
					if (folderinfo.filetypeslist == NULL)
					{
						fprintf(stderr, "malloc error, out of memory\n");
						return ENOMEM;
					}
					folderinfo.filetypeslist[folderinfo.filetypeslistlen++] = (char *) argv[i];
					j = strlen(argv[i]) - 1;
					break;
				case 'T':
					if (j + 1 < strlen(argv[i]) || i + 2 > argc)
					{
						printUsage();
						exit(EINVAL);
					}
					i++;
					if (strcasecmp(argv[i], "zlib") == 0) {
						compressiontype = ZLIB;
					} else if (strcasecmp(argv[i], "lzvn") == 0) {
#ifdef HAS_LZVN
						if(
#if __has_builtin(__builtin_available)
							// we can do simplified runtime OS version detection: accept LZVN on 10.9 and up.
							// NB: apparently this cannot be merged into a single if() with the strcasecmp().
							__builtin_available(macOS 10.9, *)
#else
							darwinMajor >= 13
#endif
						) {
							compressiontype = LZVN;
						} else {
							fprintf(stderr, "Sorry, LZVN compression is supported from OS X 10.9 and up\n");
							exit(EINVAL);
						}
#else // !HAS_LZVN
						fprintf(stderr, "Sorry, LZVN compression has not been enabled in this build.\n");
						exit(EINVAL);
#endif
					} else if (strcasecmp(argv[i], "lzfse") == 0) {
#ifdef HAS_LZFSE
						if(
#if __has_builtin(__builtin_available)
							__builtin_available(macOS 10.11, *)
#else
							darwinMajor >= 15
#endif
						) {
							// accept LZFSE on 10.11 and up.
							compressiontype = LZFSE;
						} else {
							fprintf(stderr, "Sorry, LZFSE compression is supported from OS X 10.11 and up\n");
							exit(EINVAL);
						}
#else // !HAS_LZFSE
						fprintf(stderr, "Sorry, LZFSE compression has not been enabled in this build.\n");
						exit(EINVAL);
#endif
					} else {
						fprintf(stderr, "Unsupported or unknown HFS compression requested (%s)\n", argv[i]);
						printUsage();
						exit(EINVAL);
					}
					j = strlen(argv[i]) - 1;
					break;
				case 'i':
					if (createfile || extractfile)
					{
						printUsage();
						exit(EINVAL);
					}
					invert_filetypelist = TRUE;
					break;
				case 'b':
					if (!applycomp)
					{
						printUsage();
						exit(EINVAL);
					}
					folderinfo.backup_file = backupFile = TRUE;
					break;
#ifdef SUPPORT_PARALLEL
				case 'j':
				case 'J':
				case 'R':
					if (!applycomp)
					{
						printUsage();
						exit(EINVAL);
					}
					if (argv[i][j] == 'J')
					{
						exclusive_io = false;
					}
					if (argv[i][j] == 'R')
					{
						nReverse = atoi(&argv[i][j+1]);
						if (nReverse <= 0)
						{
							fprintf( stderr, "Warning: reverse jobs must be a positive number (%s)\n", argv[i] );
							nReverse = 0;
						}
					}
					else
					{
						nJobs = atoi(&argv[i][j+1]);
						if (nJobs <= 0)
						{
							fprintf( stderr, "Warning: jobs must be a positive number (%s)\n", argv[i] );
							nJobs = 0;
						}
					}
					goto next_arg;
					break;
				case 'S':
					if (!applycomp)
					{
						printUsage();
						exit(EINVAL);
					}
					sortQueue = true;
					goto next_arg;
					break;
#endif
				default:
					printUsage();
					exit(EINVAL);
					break;
			}
		}
next_arg:;
	}
	
	if (i == argc || ((createfile || extractfile) && (argc - i < 2)))
	{
		printUsage();
		exit(EINVAL);
	}

#ifdef SUPPORT_PARALLEL
	if (nJobs > 0)
	{
		if (nReverse && !sortQueue)
		{
			fprintf( stderr, "Warning: reverse jobs are ignored when the item list is not sorted (-S)\n" );
			nReverse = 0;
		}
		PP = createParallelProcessor(nJobs, nReverse, printVerbose);
//		if (PP)
//		{
//			if (printVerbose)
//			{
//				printf( "Verbose mode switched off in parallel processing mode\n");
//			}
//			printVerbose = false;
//		}
	}
#endif

	// ignore signals due to exceeding CPU or file size limits
	signal(SIGXCPU, SIG_IGN);
	signal(SIGXFSZ, SIG_IGN);

	int N, step, n;
	if (createfile || extractfile)
	{
		N = argc - 1;
		step = 2;
	}
	else
	{
		N = argc;
		step = 1;
	}
	for ( n = 0 ; i < N ; i += step, ++n )
	{
		if (n && printVerbose > 0 && !nJobs)
		{
			printf("\n");
		}
		if (createfile || extractfile)
		{
			if (argv[i+1][0] != '/')
			{
				cwd = getcwd(NULL, 0);
				if (cwd == NULL)
				{
					fprintf(stderr, "Unable to get PWD, exiting...\n");
					exit(EACCES);
				}
				free_dst = TRUE;
				const size_t plen = strlen(cwd) + strlen(argv[i]) + 2;
				fullpath = (char *) malloc(plen);
				snprintf(fullpath, plen, "%s/%s", cwd, argv[i]);
				free(cwd);
			}
			else
				fullpathdst = (char *) argv[i+1];
		}
		
		if (argv[i][0] != '/')
		{
			cwd = getcwd(NULL, 0);
			if (cwd == NULL)
			{
				fprintf(stderr, "Unable to get PWD, exiting...\n");
				exit(EACCES);
			}
			free_src = TRUE;
			fullpath = (char *) malloc(strlen(cwd) + strlen(argv[i]) + 2);
			sprintf(fullpath, "%s/%s", cwd, argv[i]);
			free(cwd);
		}
		else
		{
			free_src = FALSE;
			fullpath = (char *) argv[i];
		}
		
		if (lstat(fullpath, &fileinfo) < 0)
		{
			fprintf(stderr, "%s: %s\n", fullpath, strerror(errno));
			continue;
		}
		
		argIsFile = ((fileinfo.st_mode & S_IFDIR) == 0);
		
		if (!argIsFile)
		{
			folderarray[0] = fullpath;
			folderarray[1] = NULL;
		}
		
		if ((createfile || extractfile) && lstat(fullpathdst, &dstfileinfo) >= 0)
		{
			dstIsFile = ((dstfileinfo.st_mode & S_IFDIR) == 0);
			fprintf(stderr, "%s: %s already exists at this path\n", fullpath, dstIsFile ? "File" : "Folder");
			continue;
		}
		
		if (applycomp && argIsFile)
		{
			struct folder_info fi;
			fi.maxSize = maxSize;
			fi.compressionlevel = compressionlevel;
			fi.compressiontype = compressiontype;
			fi.allowLargeBlocks = allowLargeBlocks;
			fi.minSavings = minSavings;
			fi.check_files = fileCheck;
			fi.backup_file = backupFile;
#ifdef SUPPORT_PARALLEL
			if (PP)
			{
				if (fileIsCompressable(fullpath, &fileinfo, compressiontype, &fi.onAPFS))
				{
					addFileToParallelProcessor( PP, fullpath, &fileinfo, &fi, true );
				}
				else
				{
					process_file_info(fullpath, NULL, &fileinfo, getParallelProcessorJobInfo(PP));
				}
			}
			else
#endif
			{
				compressFile(fullpath, &fileinfo, &fi, NULL);
			}
			lstat(fullpath, &fileinfo);
		}

#ifdef __APPLE__
		if (createfile)
		{
			if (!argIsFile)
			{
				fprintf(stderr, "%s: File required, this is a folder\n", fullpath);
				return -1;
			}
			else if ((fileinfo.st_flags & UF_COMPRESSED) == 0)
			{
				fprintf(stderr, "%s: HFS+/APFS compressed file required, this file is not\n", fullpath);
				return -1;
			}
			
			afscFile = fopen(fullpathdst, "w");
			if (afscFile == NULL)
			{
				fprintf(stderr, "%s: %s\n", fullpathdst, strerror(errno));
				continue;
			}
			else
			{
				fprintf(afscFile, "afsc");
				big16 = EndianU16_NtoB(fileinfo.st_mode);
				if (fwrite(&big16, sizeof(mode_t), 1, afscFile) != 1)
				{
					fprintf(stderr, "%s: Error writing file\n", fullpathdst);
					return -1;
				}
				xattrnamesize = listxattr(fullpath, NULL, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
				
				if (xattrnamesize > 0)
				{
					xattrnames = (char *) malloc(xattrnamesize);
					if (xattrnames == NULL)
					{
						fprintf(stderr, "malloc error, unable to get file information\n");
						return -1;
					}
					if ((xattrnamesize = listxattr(fullpath, xattrnames, xattrnamesize, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW)) <= 0)
					{
						fprintf(stderr, "listxattr: %s\n", strerror(errno));
						free(xattrnames);
						return -1;
					}
					for (curr_attr = xattrnames; curr_attr < xattrnames + xattrnamesize; curr_attr += strlen(curr_attr) + 1)
					{
						xattrsize = getxattr(fullpath, curr_attr, NULL, 0, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
						if (xattrsize < 0)
						{
							fprintf(stderr, "getxattr: %s\n", strerror(errno));
							free(xattrnames);
							return -1;
						}
						if (((strcmp(curr_attr, XATTR_RESOURCEFORK_NAME) == 0 && strlen(curr_attr) == 22) ||
							(strcmp(curr_attr, DECMPFS_XATTR_NAME) == 0 && strlen(curr_attr) == 17)) &&
							xattrsize != 0)
						{
							attr_buf = malloc(xattrsize);
							if (attr_buf == NULL)
							{
								fprintf(stderr, "malloc error, unable to get file information\n");
								return -1;
							}
							xattrPos = 0;
							do
							{
								getxattrret = getxattr(fullpath, curr_attr, attr_buf + xattrPos, xattrsize - xattrPos, xattrPos, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
								if (getxattrret < 0)
								{
									fprintf(stderr, "getxattr: %s\n", strerror(errno));
									free(xattrnames);
									return -1;
								}
								xattrPos += getxattrret;
							} while (xattrPos < xattrsize && getxattrret > 0);
							fprintf(afscFile, "%s", curr_attr);
							putc('\0', afscFile);
							big64 = EndianU64_NtoB(xattrsize);
							if (fwrite(&big64, sizeof(ssize_t), 1, afscFile) != 1 ||
								fwrite(attr_buf, xattrsize, 1, afscFile) != 1)
							{
								fprintf(stderr, "%s: Error writing file\n", fullpathdst);
								return -1;
							}
							free(attr_buf);
						}
					}
					free(xattrnames);
				}
				fclose(afscFile);
				if (decomp) {
					if (printVerbose > 1) {
						printFileInfo(fullpath, &fileinfo, false, (bool)-1);
					}
					decompressFile(fullpath, &fileinfo, backupFile);
				}
			}
		}
		else if (extractfile)
		{
			if (!argIsFile)
			{
				fprintf(stderr, "%s: File required, this is a folder\n", fullpath);
				return -1;
			}
			afscFile = fopen(fullpath, "r");
			if (afscFile == NULL)
			{
				fprintf(stderr, "%s: %s\n", fullpathdst, strerror(errno));
				continue;
			}
			else
			{
				if (fread(header, 4, 1, afscFile) != 1)
				{
					fprintf(stderr, "%s: Error reading file\n", fullpath);
					return -1;
				}
				if (header[0] != 'a' ||
					header[1] != 'f' ||
					header[2] != 's' ||
					header[3] != 'c')
				{
					fprintf(stderr, "%s: Invalid header\n", fullpath);
					return -1;
				}
				outFile = fopen(fullpathdst, "w");
				if (outFile == NULL)
				{
					fprintf(stderr, "%s: %s\n", fullpathdst, strerror(errno));
					continue;
				}
				else
				{
					fclose(outFile);
					if (fread(&outFileMode, sizeof(mode_t), 1, afscFile) != 1)
					{
						fprintf(stderr, "%s: Error reading file\n", fullpath);
						return -1;
					}
					outFileMode = EndianU16_BtoN(outFileMode);
					xattrnames = (char *) malloc(23);
					while (1)
					{
						for (j = 0; j < 23; j++)
						{
							xattrnames[j] = getc(afscFile);
							if (xattrnames[j] == '\0') break;
							if (xattrnames[j] == EOF) goto decomp_check;
						}
						if (j == 23 ||
							fread(&xattrsize, sizeof(ssize_t), 1, afscFile) != 1)
						{
							fprintf(stderr, "%s: Error reading file\n", fullpath);
							return -1;
						}
						xattrsize = EndianU64_BtoN(xattrsize);
						attr_buf = malloc(xattrsize);
						if (attr_buf == NULL)
						{
							fprintf(stderr, "malloc error, unable to set file information\n");
							return -1;
						}
						if (fread(attr_buf, xattrsize, 1, afscFile) != 1)
						{
							fprintf(stderr, "%s: Error reading file\n", fullpath);
							return -1;
						}
						if (setxattr(fullpathdst, xattrnames, attr_buf, xattrsize, 0, XATTR_NOFOLLOW | XATTR_CREATE) < 0)
						{
							fprintf(stderr, "setxattr: %s\n", strerror(errno));
							return -1;
						}
						free(attr_buf);
					}
					__builtin_unreachable();
					fprintf(stderr, "%s: Error reading file\n", fullpath);
					return -1;
				decomp_check:
					if (chflags(fullpathdst, UF_COMPRESSED) < 0)
					{
						fprintf(stderr, "chflags: %s\n", strerror(errno));
						return -1;
					}
					if (chmod(fullpathdst, outFileMode) < 0)
					{
						fprintf(stderr, "chmod: %s\n", strerror(errno));
						return -1;
					}
					if (decomp)
					{
						if (lstat(fullpathdst, &dstfileinfo) < 0)
						{
							fprintf(stderr, "%s: %s\n", fullpathdst, strerror(errno));
							continue;
						}
						if (printVerbose > 1) {
							printFileInfo(fullpathdst, &dstfileinfo, false, (bool)-1);
						}
						decompressFile(fullpathdst, &dstfileinfo, backupFile);
					}
				}
			}
		}
		else
#endif
		if (decomp && argIsFile)
		{
			if (printVerbose > 1) {
				printFileInfo(fullpath, &fileinfo, false, (bool)-1);
			}
			decompressFile(fullpath, &fileinfo, backupFile);
		}
		else if (decomp)
		{
			if ((currfolder = fts_open(folderarray, FTS_PHYSICAL, NULL)) == NULL)
			{
				fprintf(stderr, "%s: %s\n", fullpath, strerror(errno));
//				exit(EACCES);
				continue;
			}
			while ((currfile = fts_read(currfolder)) != NULL)
			{
				filetype_found = FALSE;
				if (folderinfo.filetypeslist != NULL)
				{
					filetype = getFileType(currfile->fts_path);
					if (filetype == NULL)
					{
						filetype = (char *) malloc(10);
						strcpy(filetype, "UNDEFINED");
					}
				}
				if (filetype != NULL)
				{
					fileextension = NULL;
					for (i = strlen(currfile->fts_path) - 1; i > 0; i--)
						if (currfile->fts_path[i] == '.')
							break;
					if (i != 0 && i != strlen(currfile->fts_path) - 1 && currfile->fts_path[i] != '/' && currfile->fts_path[i-1] != '/')
						fileextension = &currfile->fts_path[i+1];
					for (i = 0; i < folderinfo.filetypeslistlen; i++)
						if (strcmp(folderinfo.filetypeslist[i], filetype) == 0 ||
							strcmp("ALL", folderinfo.filetypeslist[i]) == 0 ||
							(fileextension != NULL && strcasecmp(fileextension, folderinfo.filetypeslist[i]) == 0))
							filetype_found = TRUE;
					if (invert_filetypelist)
					{
						if (filetype_found)
							filetype_found = FALSE;
						else
							filetype_found = TRUE;
					}
				}
				if ((currfile->fts_statp->st_mode & S_IFDIR) == 0 && (folderinfo.filetypeslist == NULL || filetype_found)) {
					if (printVerbose > 1) {
						printFileInfo(currfile->fts_path, currfile->fts_statp, false, (bool)-1);
					}
					decompressFile(currfile->fts_path, currfile->fts_statp, backupFile);
				}
				if (filetype != NULL) free(filetype);
			}
			fts_close(currfolder);
		}
#ifdef __APPLE__
		else if (argIsFile && printVerbose == 0)
		{
			if (applycomp)
			{
				if ((fileinfo.st_flags & UF_COMPRESSED) == 0) {
					printf("Unable to compress file %s.\n", fullpath);
				}
// at this point we could report that the file has been compressed, but that doesn't stroke with non-verbose mode.
// 				else {
// 					printf("File has been HFS+/APFS compressed.\n");
// 				}
			}
			else
			{
				if ((fileinfo.st_flags & UF_COMPRESSED) != 0)
					printf("%s is HFS+/APFS compressed.\n", fullpath);
				else
					printf("%s is not HFS+/APFS compressed.\n", fullpath);
			}
		}
#endif
		else if (argIsFile && printVerbose > 0)
		{
			bool onAPFS;
			fileIsCompressable(fullpath, &fileinfo, compressiontype, &onAPFS);
			printFileInfo(fullpath, &fileinfo, applycomp, onAPFS);
		}
		else if (!argIsFile)
		{
			if ((currfolder = fts_open(folderarray, FTS_PHYSICAL, NULL)) == NULL)
			{
				fprintf(stderr, "%s: %s\n", fullpath, strerror(errno));
//				exit(EACCES);
				continue;
			}
			folderinfo.uncompressed_size = 0;
			folderinfo.uncompressed_size_rounded = 0;
			folderinfo.compressed_size = 0;
			folderinfo.compressed_size_rounded = 0;
			folderinfo.compattr_size = 0;
			folderinfo.total_size = 0;
			folderinfo.num_compressed = 0;
			folderinfo.num_files = 0;
			folderinfo.num_hard_link_files = 0;
			folderinfo.num_folders = 0;
			folderinfo.num_hard_link_folders = 0;
			folderinfo.print_info = (nJobs)? false : printVerbose;
			folderinfo.print_files = (nJobs == 0)? printDir : 0;
			folderinfo.compress_files = applycomp;
			folderinfo.check_files = fileCheck;
			folderinfo.allowLargeBlocks = allowLargeBlocks;
			folderinfo.compressionlevel = compressionlevel;
			folderinfo.compressiontype = compressiontype;
			folderinfo.minSavings = minSavings;
			folderinfo.maxSize = maxSize;
			folderinfo.check_hard_links = hardLinkCheck;
			folderinfo.filetypes = NULL;
			folderinfo.numfiletypes = 0;
			folderinfo.filetypessize = 0;
			folderinfo.invert_filetypelist = invert_filetypelist;
			folderinfo.backup_file = backupFile;
			process_folder(currfolder, &folderinfo);
			folderinfo.num_folders--;
			if (printVerbose > 0 || !printDir)
			{
				if (!nJobs)
				{
					if (printDir) printf("\n");
					printf("%s:\n", fullpath);
				}
				else
				{
					printf("Adding %s to queue\n", fullpath);
				}
				if (folderinfo.filetypes != NULL)
				{
					alltypesinfo.filetype = NULL;
					alltypesinfo.uncompressed_size = 0;
					alltypesinfo.uncompressed_size_rounded = 0;
					alltypesinfo.compressed_size = 0;
					alltypesinfo.compressed_size_rounded = 0;
					alltypesinfo.compattr_size = 0;
					alltypesinfo.total_size = 0;
					alltypesinfo.num_compressed = 0;
					alltypesinfo.num_files = 0;
					alltypesinfo.num_hard_link_files = 0;
					for (i = 0; i < folderinfo.numfiletypes; i++)
					{
						alltypesinfo.uncompressed_size += folderinfo.filetypes[i].uncompressed_size;
						alltypesinfo.uncompressed_size_rounded += folderinfo.filetypes[i].uncompressed_size_rounded;
						alltypesinfo.compressed_size += folderinfo.filetypes[i].compressed_size;
						alltypesinfo.compressed_size_rounded += folderinfo.filetypes[i].compressed_size_rounded;
						alltypesinfo.compattr_size += folderinfo.filetypes[i].compattr_size;
						alltypesinfo.total_size += folderinfo.filetypes[i].total_size;
						alltypesinfo.num_compressed += folderinfo.filetypes[i].num_compressed;
						alltypesinfo.num_files += folderinfo.filetypes[i].num_files;
						alltypesinfo.num_hard_link_files += folderinfo.filetypes[i].num_hard_link_files;
						
						if (!folderinfo.invert_filetypelist)
							printf("\nFile content type: %s\n", folderinfo.filetypes[i].filetype);
						if (folderinfo.filetypes[i].numextensions > 0)
						{
							if (!folderinfo.invert_filetypelist)
								printf ("File extension(s): %s", folderinfo.filetypes[i].extensions[0]);
							free(folderinfo.filetypes[i].extensions[0]);
							for (j = 1; j < folderinfo.filetypes[i].numextensions; j++)
							{
								if (!folderinfo.invert_filetypelist)
									printf (", %s", folderinfo.filetypes[i].extensions[j]);
								free(folderinfo.filetypes[i].extensions[j]);
							}
							free(folderinfo.filetypes[i].extensions);
							if (!folderinfo.invert_filetypelist)
								printf("\n");
						}
						if (!folderinfo.invert_filetypelist)
							printf("Number of HFS+/APFS compressed files: %lld\n", folderinfo.filetypes[i].num_compressed);
						if (printVerbose > 0 && nJobs == 0 && (!folderinfo.invert_filetypelist))
						{
							printf("Total number of files: %lld\n", folderinfo.filetypes[i].num_files);
							if (hardLinkCheck)
								printf("Total number of file hard links: %lld\n", folderinfo.filetypes[i].num_hard_link_files);
							filesize = folderinfo.filetypes[i].uncompressed_size;
							filesize_rounded = folderinfo.filetypes[i].uncompressed_size_rounded;
							if (folderinfo.filetypes[i].num_hard_link_files == 0 || !hardLinkCheck)
								printf("File(s) size (uncompressed; reported size by Mac OS 10.6+ Finder): %s\n",
									   getSizeStr(filesize, filesize_rounded, 1));
							else
								printf("File(s) size (uncompressed): %s\n", getSizeStr(filesize, filesize_rounded, 0));
							filesize = folderinfo.filetypes[i].compressed_size;
							filesize_rounded = folderinfo.filetypes[i].compressed_size_rounded;
							if (legacy_output
									&& (folderinfo.filetypes[i].num_hard_link_files == 0 || !hardLinkCheck)) {
								printf("File(s) size (compressed - decmpfs xattr; reported size by Mac OS 10.0-10.5 Finder): %s\n",
									   getSizeStr(filesize, filesize_rounded, 1));
							} else {
								printf("File(s) size (compressed - decmpfs xattr): %s\n",
									   getSizeStr(filesize, filesize_rounded, 0));
							}
							filesize = folderinfo.filetypes[i].compressed_size + folderinfo.filetypes[i].compattr_size;
							filesize_rounded = folderinfo.filetypes[i].compressed_size_rounded + folderinfo.filetypes[i].compattr_size;
							printf("File(s) size (compressed): %s\n", getSizeStr(filesize, filesize_rounded, 0));
							printf("Compression savings: %0.1f%%\n", (1.0 - ((float) (folderinfo.filetypes[i].compressed_size + folderinfo.filetypes[i].compattr_size) / folderinfo.filetypes[i].uncompressed_size)) * 100.0);
							filesize = folderinfo.filetypes[i].total_size;
							printf("Approximate total file(s) size (files + file overhead): %s\n",
								   getSizeStr(filesize, filesize, 0));
						}
						free(folderinfo.filetypes[i].filetype);
					}
					if (folderinfo.invert_filetypelist)
					{
						alltypesinfo.uncompressed_size = folderinfo.uncompressed_size - alltypesinfo.uncompressed_size;
						alltypesinfo.uncompressed_size_rounded = folderinfo.uncompressed_size_rounded - alltypesinfo.uncompressed_size_rounded;
						alltypesinfo.compressed_size = folderinfo.compressed_size - alltypesinfo.compressed_size;
						alltypesinfo.compressed_size_rounded = folderinfo.compressed_size_rounded - alltypesinfo.compressed_size_rounded;
						alltypesinfo.compattr_size = folderinfo.compattr_size - alltypesinfo.compattr_size;
						alltypesinfo.total_size = folderinfo.total_size - alltypesinfo.total_size;
						alltypesinfo.num_compressed = folderinfo.num_compressed - alltypesinfo.num_compressed;
						alltypesinfo.num_files = folderinfo.num_files - alltypesinfo.num_files;
						alltypesinfo.num_hard_link_files = folderinfo.num_hard_link_files - alltypesinfo.num_hard_link_files;
					}
					if (folderinfo.numfiletypes > 1 || folderinfo.invert_filetypelist)
					{
						printf("\nTotals of file content types\n");
						printf("Number of HFS+/APFS compressed files: %lld\n", alltypesinfo.num_compressed);
						if (printVerbose > 0 && nJobs == 0)
						{
							printf("Total number of files: %lld\n", alltypesinfo.num_files);
							if (hardLinkCheck)
								printf("Total number of file hard links: %lld\n", alltypesinfo.num_hard_link_files);
							filesize = alltypesinfo.uncompressed_size;
							filesize_rounded = alltypesinfo.uncompressed_size_rounded;
							if (alltypesinfo.num_hard_link_files == 0 || !hardLinkCheck)
								printf("File(s) size (uncompressed; reported size by Mac OS 10.6+ Finder): %s\n",
									   getSizeStr(filesize, filesize_rounded, 1));
							else
								printf("File(s) size (uncompressed): %s\n", getSizeStr(filesize, filesize_rounded, 0));
							filesize = alltypesinfo.compressed_size;
							filesize_rounded = alltypesinfo.compressed_size_rounded;
							if (legacy_output
									 && (alltypesinfo.num_hard_link_files == 0 || !hardLinkCheck)) {
								printf("File(s) size (compressed - decmpfs xattr; reported size by Mac OS 10.0-10.5 Finder): %s\n",
									   getSizeStr(filesize, filesize_rounded, 1));
							} else {
								printf("File(s) size (compressed - decmpfs xattr): %s\n",
									   getSizeStr(filesize, filesize_rounded, 0));
							}
							filesize = alltypesinfo.compressed_size + alltypesinfo.compattr_size;
							filesize_rounded = alltypesinfo.compressed_size_rounded + alltypesinfo.compattr_size;
							printf("File(s) size (compressed): %s\n", getSizeStr(filesize, filesize_rounded, 0));
							printf("Compression savings: %0.1f%%\n", (1.0 - ((float) (alltypesinfo.compressed_size + alltypesinfo.compattr_size) / alltypesinfo.uncompressed_size)) * 100.0);
							filesize = alltypesinfo.total_size;
							printf("Approximate total file(s) size (files + file overhead): %s\n",
								   getSizeStr(filesize, filesize, 0));
						}
					}
					printf("\n");
				}
				if (nJobs == 0)
				{
					if (folderinfo.num_compressed == 0 && !applycomp)
						printf("Folder contains no compressed files\n");
					else if (folderinfo.num_compressed == 0 && applycomp)
						printf("No compressable files in folder\n");
					else
						printf("Number of HFS+/APFS compressed files: %lld\n", folderinfo.num_compressed);
					if (printVerbose > 0)
					{
						printFolderInfo( &folderinfo, hardLinkCheck );
					}
				}
			}
			if (PP && nJobs > 0)
			{
				struct folder_info *fi = getParallelProcessorJobInfo(PP);
				memcpy(fi, &folderinfo, sizeof(*fi));
				// reset certain fields
				fi->num_files = 0;
				fi->num_compressed = 0;
				fi->uncompressed_size = fi->uncompressed_size_rounded = 0;
				fi->compressed_size = fi->compressed_size_rounded = 0;
				fi->total_size = 0;
				ppJobInfoInitialised = true;
			}
		}
		
		if (free_src)
		{
			xfree(fullpath);
			free_src = false;
		}
		if (free_dst)
		{
			xfree(fullpathdst);
			free_dst = false;
		}
	}
	if (folderinfo.filetypeslist != NULL)
		free(folderinfo.filetypeslist);
	
#ifdef SUPPORT_PARALLEL
	if (PP)
	{
		if (nJobs > 0)
		{
			struct folder_info *fi = getParallelProcessorJobInfo(PP);
			if (!ppJobInfoInitialised) {
				memcpy(fi, &folderinfo, sizeof(*fi));
				// reset certain fields
				fi->num_files = 0;
				fi->num_compressed = 0;
				fi->uncompressed_size = fi->uncompressed_size_rounded = 0;
				fi->compressed_size = fi->compressed_size_rounded = 0;
				fi->total_size = 0;
				ppJobInfoInitialised = true;
			}
		}
		if (sortQueue)
		{
			sortFilesInParallelProcessorBySize(PP);
		}
		size_t nFiles = filesInParallelProcessor(PP);
		if (nFiles)
		{
			if (nJobs > nFiles) {
				nJobs = nFiles;
				if (nJobs < nReverse) {
					// user asked a certain amount of reverse jobs;
					// respect that as well if we can
					nReverse = nJobs;
				}
				changeParallelProcessorJobs(PP, nJobs, nReverse);
			}
			signal(SIGINT, signal_handler);
			signal(SIGHUP, signal_handler);
			fprintf( stderr, "Starting %d worker threads to process queue with %lu items\n",
				nJobs, nFiles );
			int processed = runParallelProcessor(PP);
			fprintf( stderr, "Processed %d entries\n", processed );
			if (printVerbose > 0)
			{
				struct folder_info *fInfo = getParallelProcessorJobInfo(PP);
				if (fInfo->num_files > 0)
				{
					printFolderInfo( getParallelProcessorJobInfo(PP), hardLinkCheck );
				}
			}
		}
		else
		{
			fprintf( stderr, "No compressable files found.\n" );
		}
		releaseParallelProcessor(PP);
	}
#endif
// 	if (maxOutBufSize) {
// 		fprintf(stderr, "maxOutBufSize: %zd\n", maxOutBufSize);
// 	}
	return 0;
}
