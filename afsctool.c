#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <fts.h>
#include <sys/xattr.h>
#include <hfs/hfs_format.h>
#include <unistd.h>
#include <zlib.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>

const char *sizeunit10_short[] = {"KB", "MB", "GB", "TB", "PB", "EB"};
const char *sizeunit10_long[] = {"kilobytes", "megabytes", "gigabytes", "terabytes", "petabytes", "exabytes"};
const long long int sizeunit10[] = {1000, 1000 * 1000, 1000 * 1000 * 1000, (long long int) 1000 * 1000 * 1000 * 1000, (long long int) 1000 * 1000 * 1000 * 1000 * 1000, (long long int) 1000 * 1000 * 1000 * 1000 * 1000 * 1000};
const char *sizeunit2_short[] = {"KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
const char *sizeunit2_long[] = {"kibibytes", "mebibytes", "gibibytes", "tebibytes", "pebibytes", "exbibytes"};
const long long int sizeunit2[] = {1024, 1024 * 1024, 1024 * 1024 * 1024, (long long int) 1024 * 1024 * 1024 * 1024, (long long int) 1024 * 1024 * 1024 * 1024 * 1024, (long long int) 1024 * 1024 * 1024 * 1024 * 1024 * 1024};

struct folder_info
{
	long long int uncompressed_size;
	long long int uncompressed_size_rounded;
	long long int compressed_size;
	long long int compressed_size_rounded;
	long long int compattr_size;
	long long int total_size;
	long long int num_compressed;
	long long int num_files;
	long long int num_hard_link_files;
	long long int num_folders;
	long long int num_hard_link_folders;
	long long int maxSize;
	int print_info;
	int compressionlevel;
	double minSavings;
	bool print_files;
	bool compress_files;
	bool allowLargeBlocks;
	bool check_files;
	bool check_hard_links;
	struct filetype_info *filetypes;
	long long int numfiletypes;
	long long int filetypessize;
	char **filetypeslist;
	int filetypeslistlen;
	int filetypeslistsize;
	bool invert_filetypelist;
};

struct filetype_info
{
	char *filetype;
	char **extensions;
	int extensionssize;
	int numextensions;
	long long int uncompressed_size;
	long long int uncompressed_size_rounded;
	long long int compressed_size;
	long long int compressed_size_rounded;
	long long int compattr_size;
	long long int total_size;
	long long int num_compressed;
	long long int num_files;
	long long int num_hard_link_files;
};

char* getSizeStr(long long int size, long long int size_rounded)
{
	static char sizeStr[90];
	int unit2, unit10;
	
	for (unit2 = 0; unit2 + 1 < sizeof(sizeunit2) && (size_rounded / sizeunit2[unit2 + 1]) > 0; unit2++);
	for (unit10 = 0; unit10 + 1 < sizeof(sizeunit10) && (size_rounded / sizeunit10[unit10 + 1]) > 0; unit10++);
	
	sprintf(sizeStr, "%lld bytes / ", size);

	switch (unit10)
	{
		case 0:
			sprintf(sizeStr, "%s%0.0f %s (%s) / ", sizeStr, (double) size_rounded / sizeunit10[unit10], sizeunit10_short[unit10], sizeunit10_long[unit10]);
			break;
		case 1:
			sprintf(sizeStr, "%s%.12g %s (%s) / ", sizeStr, (double) (((long long int) ((double) size_rounded / sizeunit10[unit10] * 100) + 5) / 10) / 10, sizeunit10_short[unit10], sizeunit10_long[unit10]);
			break;
		default:
			sprintf(sizeStr, "%s%0.12g %s (%s) / ", sizeStr,  (double) (((long long int) ((double) size_rounded / sizeunit10[unit10] * 1000) + 5) / 10) / 100, sizeunit10_short[unit10], sizeunit10_long[unit10]);
			break;
	}
	
	switch (unit2)
	{
		case 0:
			sprintf(sizeStr, "%s%0.0f %s (%s)", sizeStr, (double) size_rounded / sizeunit2[unit2], sizeunit2_short[unit2], sizeunit2_long[unit2]);
			break;
		case 1:
			sprintf(sizeStr, "%s%.12g %s (%s)", sizeStr, (double) (((long long int) ((double) size_rounded / sizeunit2[unit2] * 100) + 5) / 10) / 10, sizeunit2_short[unit2], sizeunit2_long[unit2]);
			break;
		default:
			sprintf(sizeStr, "%s%0.12g %s (%s)", sizeStr,  (double) (((long long int) ((double) size_rounded / sizeunit2[unit2] * 1000) + 5) / 10) / 100, sizeunit2_short[unit2], sizeunit2_long[unit2]);
			break;
	}
	
	return sizeStr;
}

void compressFile(const char *inFile, struct stat *inFileInfo, long long int maxSize, int compressionlevel, bool allowLargeBlocks, double minSavings, bool checkFiles)
{
	FILE *in;
	struct statfs fsInfo;
	unsigned int compblksize = 0x10000, numBlocks, outdecmpfsSize = 0;
	void *inBuf, *outBuf, *outBufBlock, *outdecmpfsBuf, *currBlock, *blockStart;
	long long int inBufPos, filesize = inFileInfo->st_size;
	unsigned long int cmpedsize;
	char *xattrnames, *curr_attr;
	ssize_t xattrnamesize;
	UInt32 cmpf = 'cmpf';
	struct timeval times[2];
	
	times[0].tv_sec = inFileInfo->st_atimespec.tv_sec;
	times[0].tv_usec = inFileInfo->st_atimespec.tv_nsec / 1000;
	times[1].tv_sec = inFileInfo->st_mtimespec.tv_sec;
	times[1].tv_usec = inFileInfo->st_mtimespec.tv_nsec / 1000;
	
	if (statfs(inFile, &fsInfo) < 0)
		return;
	if (fsInfo.f_type != 17)
		return;
	
	if (!S_ISREG(inFileInfo->st_mode))
		return;
	if ((inFileInfo->st_flags & UF_COMPRESSED) != 0)
		return;
	if (filesize > maxSize && maxSize != 0)
		return;
	if (filesize == 0)
		return;
	
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
			fprintf(stderr, "%s: malloc error, unable to get file information\n", inFile);
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
			if ((strcmp(curr_attr, "com.apple.ResourceFork") == 0 && strlen(curr_attr) == 22) ||
				(strcmp(curr_attr, "com.apple.decmpfs") == 0 && strlen(curr_attr) == 17))
				return;
		}
		free(xattrnames);
	}
	
	numBlocks = (filesize + compblksize - 1) / compblksize;
	if ((filesize + 0x13A + (numBlocks * 9)) > 2147483647)
		return;
	
	in = fopen(inFile, "r+");
	if (in == NULL)
	{
		fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
		return;
	}
	inBuf = malloc(filesize);
	if (inBuf == NULL)
	{
		fprintf(stderr, "%s: malloc error, unable to allocate input buffer\n", inFile);
		fclose(in);
		utimes(inFile, times);
		return;
	}
	if (fread(inBuf, filesize, 1, in) != 1)
	{
		fprintf(stderr, "%s: Error reading file\n", inFile);
		fclose(in);
		utimes(inFile, times);
		free(inBuf);
		return;
	}
	fclose(in);
	outBuf = malloc(filesize + 0x13A + (numBlocks * 9));
	if (outBuf == NULL)
	{
		fprintf(stderr, "%s: malloc error, unable to allocate output buffer\n", inFile);
		utimes(inFile, times);
		free(inBuf);
		return;
	}
	outdecmpfsBuf = malloc(3802);
	if (outdecmpfsBuf == NULL)
	{
		fprintf(stderr, "%s: malloc error, unable to allocate xattr buffer\n", inFile);
		utimes(inFile, times);
		free(inBuf);
		free(outBuf);
		return;
	}
	outBufBlock = malloc(compressBound(compblksize));
	if (outBufBlock == NULL)
	{
		fprintf(stderr, "%s: malloc error, unable to allocate compression buffer\n", inFile);
		utimes(inFile, times);
		free(inBuf);
		free(outBuf);
		free(outdecmpfsBuf);
		return;
	}
	*(UInt32 *) outdecmpfsBuf = EndianU32_NtoL(cmpf);
	*(UInt32 *) (outdecmpfsBuf + 4) = EndianU32_NtoL(4);
	*(UInt64 *) (outdecmpfsBuf + 8) = EndianU64_NtoL(filesize);
	outdecmpfsSize = 0x10;
	*(UInt32 *) outBuf = EndianU32_NtoB(0x100);
	*(UInt32 *) (outBuf + 12) = EndianU32_NtoB(0x32);
	memset(outBuf + 16, 0, 0xF0);
	blockStart = outBuf + 0x104;
	*(UInt32 *) blockStart = EndianU32_NtoL(numBlocks);
	currBlock = blockStart + 0x4 + (numBlocks * 8);
	for (inBufPos = 0; inBufPos < filesize; inBufPos += compblksize, currBlock += cmpedsize)
	{
		cmpedsize = compressBound(compblksize);
		if (compress2(outBufBlock, &cmpedsize, inBuf + inBufPos, ((filesize - inBufPos) > compblksize) ? compblksize : filesize - inBufPos, compressionlevel) != Z_OK)
		{
			utimes(inFile, times);
			free(inBuf);
			free(outBuf);
			free(outdecmpfsBuf);
			free(outBufBlock);
			return;
		}
		if (cmpedsize > (((filesize - inBufPos) > compblksize) ? compblksize : filesize - inBufPos))
		{
			if (!allowLargeBlocks && (((filesize - inBufPos) > compblksize) ? compblksize : filesize - inBufPos) == compblksize)
            {
                utimes(inFile, times);
                free(inBuf);
                free(outBuf);
                free(outdecmpfsBuf);
                free(outBufBlock);
				return;
            }
			*(unsigned char *) outBufBlock = 0xFF;
			memcpy(outBufBlock + 1, inBuf + inBufPos, ((filesize - inBufPos) > compblksize) ? compblksize : filesize - inBufPos);
			cmpedsize = ((filesize - inBufPos) > compblksize) ? compblksize : filesize - inBufPos;
			cmpedsize++;
		}
		if (((cmpedsize + outdecmpfsSize) <= 3802) && (numBlocks <= 1))
		{
			*(UInt32 *) (outdecmpfsBuf + 4) = EndianU32_NtoL(3);
			memcpy(outdecmpfsBuf + outdecmpfsSize, outBufBlock, cmpedsize);
			outdecmpfsSize += cmpedsize;
			break;
		}
		memcpy(currBlock, outBufBlock, cmpedsize);
		*(UInt32 *) (blockStart + ((inBufPos / compblksize) * 8) + 0x4) = EndianU32_NtoL(currBlock - blockStart);
		*(UInt32 *) (blockStart + ((inBufPos / compblksize) * 8) + 0x8) = EndianU32_NtoL(cmpedsize);
	}
	
	if (EndianU32_LtoN(*(UInt32 *) (outdecmpfsBuf + 4)) == 4)
	{
		if ((((double) (currBlock - outBuf + outdecmpfsSize + 50) / filesize) >= (1.0 - minSavings / 100) && minSavings != 0.0) ||
			currBlock - outBuf + outdecmpfsSize + 50 >= filesize)
		{
			utimes(inFile, times);
			free(inBuf);
			free(outBuf);
			free(outdecmpfsBuf);
			free(outBufBlock);
			return;
		}
		*(UInt32 *) (outBuf + 4) = EndianU32_NtoB(currBlock - outBuf);
		*(UInt32 *) (outBuf + 8) = EndianU32_NtoB(currBlock - outBuf - 0x100);
		*(UInt32 *) (blockStart - 4) = EndianU32_NtoB(currBlock - outBuf - 0x104);
		memset(currBlock, 0, 24);
		*(UInt16 *) (currBlock + 24) = EndianU16_NtoB(0x1C);
		*(UInt16 *) (currBlock + 26) = EndianU16_NtoB(0x32);
		*(UInt16 *) (currBlock + 28) = 0;
		*(UInt32 *) (currBlock + 30) = EndianU32_NtoB(cmpf);
		*(UInt32 *) (currBlock + 34) = EndianU32_NtoB(0xA);
		*(UInt64 *) (currBlock + 38) = EndianU64_NtoL(0xFFFF0100);
		*(UInt32 *) (currBlock + 46) = 0;
		if (setxattr(inFile, "com.apple.ResourceFork", outBuf, currBlock - outBuf + 50, 0, XATTR_NOFOLLOW | XATTR_CREATE) < 0)
		{
			fprintf(stderr, "%s: setxattr: %s\n", inFile, strerror(errno));
			free(inBuf);
			free(outBuf);
			free(outdecmpfsBuf);
			free(outBufBlock);
			return;
		}
	}
	if (setxattr(inFile, "com.apple.decmpfs", outdecmpfsBuf, outdecmpfsSize, 0, XATTR_NOFOLLOW | XATTR_CREATE) < 0)
	{
		fprintf(stderr, "%s: setxattr: %s\n", inFile, strerror(errno));
		free(inBuf);
		free(outBuf);
		free(outdecmpfsBuf);
		free(outBufBlock);
		return;
	}
	in = fopen(inFile, "w");
	if (in == NULL)
	{
		fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
		return;
	}
	fclose(in);
	if (chflags(inFile, UF_COMPRESSED | inFileInfo->st_flags) < 0)
	{
		fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
		if (removexattr(inFile, "com.apple.decmpfs", XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
		{
			fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
		}
		if (EndianU32_LtoN(*(UInt32 *) (outdecmpfsBuf + 4)) == 4 &&
			removexattr(inFile, "com.apple.ResourceFork", XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
		{
			fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
		}
		in = fopen(inFile, "w");
		if (in == NULL)
		{
			free(inBuf);
			free(outBuf);
			free(outdecmpfsBuf);
			free(outBufBlock);
			fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
			return;
		}
		if (fwrite(inBuf, filesize, 1, in) != 1)
		{
			free(inBuf);
			free(outBuf);
			free(outdecmpfsBuf);
			free(outBufBlock);
			fprintf(stderr, "%s: Error writing to file\n", inFile);
			return;
		}
		fclose(in);
		utimes(inFile, times);
		return;
	}
	if (checkFiles)
	{
		lstat(inFile, inFileInfo);
		in = fopen(inFile, "r");
		if (in == NULL)
		{
			fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
			return;
		}
		if (inFileInfo->st_size != filesize || 
			fread(outBuf, filesize, 1, in) != 1 ||
			memcmp(outBuf, inBuf, filesize) != 0)
		{
			fclose(in);
			printf("%s: Compressed file check failed, reverting file changes\n", inFile);
			if (chflags(inFile, (~UF_COMPRESSED) & inFileInfo->st_flags) < 0)
			{
				free(inBuf);
				free(outBuf);
				free(outdecmpfsBuf);
				free(outBufBlock);
				fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
				return;
			}
			if (removexattr(inFile, "com.apple.decmpfs", XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
			{
				fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
			}
			if (EndianU32_LtoN(*(UInt32 *) (outdecmpfsBuf + 4)) == 4 && 
				removexattr(inFile, "com.apple.ResourceFork", XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
			{
				fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
			}
			in = fopen(inFile, "w");
			if (in == NULL)
			{
				free(inBuf);
				free(outBuf);
				free(outdecmpfsBuf);
				free(outBufBlock);
				fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
				return;
			}
			if (fwrite(inBuf, filesize, 1, in) != 1)
			{
				free(inBuf);
				free(outBuf);
				free(outdecmpfsBuf);
				free(outBufBlock);
				fprintf(stderr, "%s: Error writing to file\n", inFile);
				return;
			}
		}
		fclose(in);
	}
	utimes(inFile, times);
	free(inBuf);
	free(outBuf);
	free(outdecmpfsBuf);
	free(outBufBlock);
}

void decompressFile(const char *inFile, struct stat *inFileInfo)
{
	FILE *in;
	int uncmpret;
	unsigned int compblksize = 0x10000, numBlocks, currBlock;
	long long int filesize;
	unsigned long int uncmpedsize;
	void *inBuf = NULL, *outBuf, *indecmpfsBuf = NULL, *blockStart;
	char *xattrnames, *curr_attr;
	ssize_t xattrnamesize, indecmpfsLen = 0, inRFLen = 0, getxattrret, RFpos = 0;
	struct timeval times[2];
	
	times[0].tv_sec = inFileInfo->st_atimespec.tv_sec;
	times[0].tv_usec = inFileInfo->st_atimespec.tv_nsec / 1000;
	times[1].tv_sec = inFileInfo->st_mtimespec.tv_sec;
	times[1].tv_usec = inFileInfo->st_mtimespec.tv_nsec / 1000;
	
	if (!S_ISREG(inFileInfo->st_mode))
		return;
	if ((inFileInfo->st_flags & UF_COMPRESSED) == 0)
		return;
	
	xattrnamesize = listxattr(inFile, NULL, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
	
	if (xattrnamesize > 0)
	{
		xattrnames = (char *) malloc(xattrnamesize);
		if (xattrnames == NULL)
		{
			fprintf(stderr, "%s: malloc error, unable to get file information\n", inFile);
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
			if (strcmp(curr_attr, "com.apple.ResourceFork") == 0 && strlen(curr_attr) == 22)
			{
				inRFLen = getxattr(inFile, curr_attr, NULL, 0, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
				if (inRFLen < 0)
				{
					fprintf(stderr, "%s: getxattr: %s\n", inFile, strerror(errno));
					free(xattrnames);
					return;
				}
				if (inRFLen != 0)
				{
					inBuf = malloc(inRFLen);
					if (inBuf == NULL)
					{
						fprintf(stderr, "%s: malloc error, unable to allocate input buffer\n", inFile);
						return;
					}
					do
					{
						getxattrret = getxattr(inFile, curr_attr, inBuf + RFpos, inRFLen - RFpos, RFpos, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
						if (getxattrret < 0)
						{
							fprintf(stderr, "getxattr: %s\n", strerror(errno));
							free(xattrnames);
							return;
						}
						RFpos += getxattrret;
					} while (RFpos < inRFLen && getxattrret > 0);
				}
			}
			if (strcmp(curr_attr, "com.apple.decmpfs") == 0 && strlen(curr_attr) == 17)
			{
				indecmpfsLen = getxattr(inFile, curr_attr, NULL, 0, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
				if (indecmpfsLen < 0)
				{
					fprintf(stderr, "%s: getxattr: %s\n", inFile, strerror(errno));
					free(xattrnames);
					return;
				}
				if (indecmpfsLen != 0)
				{
					indecmpfsBuf = malloc(indecmpfsLen);
					if (indecmpfsBuf == NULL)
					{
						fprintf(stderr, "%s: malloc error, unable to allocate xattr buffer\n", inFile);
						utimes(inFile, times);
						free(inBuf);
						return;
					}
					if (indecmpfsLen != 0)
					{
						indecmpfsBuf = malloc(indecmpfsLen);
						if (indecmpfsBuf == NULL)
						{
							fprintf(stderr, "%s: malloc error, unable to get file information\n", inFile);
							return;
						}
						indecmpfsLen = getxattr(inFile, curr_attr, indecmpfsBuf, indecmpfsLen, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
						if (indecmpfsLen < 0)
						{
							fprintf(stderr, "getxattr: %s\n", strerror(errno));
							free(xattrnames);
							return;
						}
					}
				}
			}
		}
		free(xattrnames);
	}
	
	if (indecmpfsBuf == NULL)
	{
		fprintf(stderr, "%s: Decompression failed; file flags indicate file is compressed but it does not have a com.apple.decmpfs extended attribute\n", inFile);
		if (inBuf != NULL)
			free(inBuf);
		if (indecmpfsBuf != NULL)
			free(indecmpfsBuf);
		return;
	}
	if (indecmpfsLen < 0x10)
	{
		fprintf(stderr, "%s: Decompression failed; extended attribute com.apple.decmpfs is only %ld bytes (it is required to have a 16 byte header)\n", inFile, indecmpfsLen);
		if (inBuf != NULL)
			free(inBuf);
		if (indecmpfsBuf != NULL)
			free(indecmpfsBuf);
		return;
	}
	
	filesize = EndianU64_LtoN(*(UInt64 *) (indecmpfsBuf + 8));
	if (filesize == 0)
	{
		fprintf(stderr, "%s: Decompression failed; file size given in header is 0\n", inFile);
		if (inBuf != NULL)
			free(inBuf);
		if (indecmpfsBuf != NULL)
			free(indecmpfsBuf);
		return;
	}
	outBuf = malloc(filesize);
	if (outBuf == NULL)
	{
		fprintf(stderr, "%s: malloc error, unable to allocate output buffer\n", inFile);
		if (inBuf != NULL)
			free(inBuf);
		if (indecmpfsBuf != NULL)
			free(indecmpfsBuf);
		return;
	}
	
	if (EndianU32_LtoN(*(UInt32 *) (indecmpfsBuf + 4)) == 4)
	{
		if (inBuf == NULL)
		{
			fprintf(stderr, "%s: Decompression failed; resource fork required for compression type 4 but none exists\n", inFile);
			if (inBuf != NULL)
				free(inBuf);
			if (indecmpfsBuf != NULL)
				free(indecmpfsBuf);
			free(outBuf);
			return;
		}
		if (inRFLen < 0x13A ||
			inRFLen < EndianU32_BtoN(*(UInt32 *) inBuf) + 0x4)
		{
			fprintf(stderr, "%s: Decompression failed; resource fork data is incomplete\n", inFile);
			if (inBuf != NULL)
				free(inBuf);
			if (indecmpfsBuf != NULL)
				free(indecmpfsBuf);
			free(outBuf);
			return;
		}
		
		blockStart = inBuf + EndianU32_BtoN(*(UInt32 *) inBuf) + 0x4;
		numBlocks = EndianU32_NtoL(*(UInt32 *) blockStart);
		
		if (inRFLen < EndianU32_BtoN(*(UInt32 *) inBuf) + 0x3A + (numBlocks * 8))
		{
			fprintf(stderr, "%s: Decompression failed; resource fork data is incomplete\n", inFile);
			if (inBuf != NULL)
				free(inBuf);
			if (indecmpfsBuf != NULL)
				free(indecmpfsBuf);
			free(outBuf);
			return;
		}
		if (compblksize * (numBlocks - 1) + (filesize % compblksize) > filesize)
		{
			fprintf(stderr, "%s: Decompression failed; file size given in header is incorrect\n", inFile);
			if (inBuf != NULL)
				free(inBuf);
			if (indecmpfsBuf != NULL)
				free(indecmpfsBuf);
			free(outBuf);
			return;
		}
		for (currBlock = 0; currBlock < numBlocks; currBlock++)
		{
			if (blockStart + EndianU32_LtoN(*(UInt32 *) (blockStart + 0x4 + (currBlock * 8))) + EndianU32_LtoN(*(UInt32 *) (blockStart + 0x8 + (currBlock * 8))) > inBuf + inRFLen)
			{
				fprintf(stderr, "%s: Decompression failed; resource fork data is incomplete\n", inFile);
				if (inBuf != NULL)
					free(inBuf);
				if (indecmpfsBuf != NULL)
					free(indecmpfsBuf);
				free(outBuf);
				return;
			}
			if (currBlock + 1 != numBlocks)
				uncmpedsize = compblksize;
			else
				uncmpedsize = (filesize - (currBlock * compblksize) < compblksize) ? filesize - (currBlock * compblksize) : compblksize;
			if ((compblksize * currBlock) + uncmpedsize > filesize)
			{
				fprintf(stderr, "%s: Decompression failed; file size given in header is incorrect\n", inFile);
				if (inBuf != NULL)
					free(inBuf);
				if (indecmpfsBuf != NULL)
					free(indecmpfsBuf);
				free(outBuf);
				return;
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
						if (inBuf != NULL)
							free(inBuf);
						if (indecmpfsBuf != NULL)
							free(indecmpfsBuf);
						free(outBuf);
						return;
					}
					else if (uncmpret == Z_DATA_ERROR)
					{
						fprintf(stderr, "%s: Decompression failed; compressed data block is corrupted\n", inFile);
						if (inBuf != NULL)
							free(inBuf);
						if (indecmpfsBuf != NULL)
							free(indecmpfsBuf);
						free(outBuf);
						return;
					}
					else if (uncmpret == Z_MEM_ERROR)
					{
						fprintf(stderr, "%s: Decompression failed; out of memory\n", inFile);
						if (inBuf != NULL)
							free(inBuf);
						if (indecmpfsBuf != NULL)
							free(indecmpfsBuf);
						free(outBuf);
						return;
					}
					else
					{
						fprintf(stderr, "%s: Decompression failed; an error occurred during decompression\n", inFile);
						if (inBuf != NULL)
							free(inBuf);
						if (indecmpfsBuf != NULL)
							free(indecmpfsBuf);
						free(outBuf);
						return;
					}
				}
			}
			if (uncmpedsize != ((filesize - (currBlock * compblksize) < compblksize) ? filesize - (currBlock * compblksize) : compblksize))
			{
				fprintf(stderr, "%s: Decompression failed; uncompressed data block too small\n", inFile);
				if (inBuf != NULL)
					free(inBuf);
				if (indecmpfsBuf != NULL)
					free(indecmpfsBuf);
				free(outBuf);
				return;
			}
		}
	}
	else if (EndianU32_LtoN(*(UInt32 *) (indecmpfsBuf + 4)) == 3)
	{
		if (indecmpfsLen == 0x10)
		{
			fprintf(stderr, "%s: Decompression failed; compression type 3 expects compressed data in extended attribute com.apple.decmpfs but none exists\n", inFile);
			if (inBuf != NULL)
				free(inBuf);
			if (indecmpfsBuf != NULL)
				free(indecmpfsBuf);
			free(outBuf);
			return;
		}
		uncmpedsize = filesize;
		if ((*(unsigned char *) (indecmpfsBuf + 0x10)) == 0xFF)
		{
			uncmpedsize = indecmpfsLen - 0x11;
			memcpy(outBuf, indecmpfsBuf + 0x11, uncmpedsize);
		}
		else
		{
			if ((uncmpret = uncompress(outBuf, &uncmpedsize, indecmpfsBuf + 0x10, indecmpfsLen - 0x10)) != Z_OK)
			{
				if (uncmpret == Z_BUF_ERROR)
				{
					fprintf(stderr, "%s: Decompression failed; uncompressed data too large\n", inFile);
					if (inBuf != NULL)
						free(inBuf);
					if (indecmpfsBuf != NULL)
						free(indecmpfsBuf);
					free(outBuf);
					return;
				}
				else if (uncmpret == Z_DATA_ERROR)
				{
					fprintf(stderr, "%s: Decompression failed; compressed data is corrupted\n", inFile);
					if (inBuf != NULL)
						free(inBuf);
					if (indecmpfsBuf != NULL)
						free(indecmpfsBuf);
					free(outBuf);
					return;
				}
				else if (uncmpret == Z_MEM_ERROR)
				{
					fprintf(stderr, "%s: Decompression failed; out of memory\n", inFile);
					if (inBuf != NULL)
						free(inBuf);
					if (indecmpfsBuf != NULL)
						free(indecmpfsBuf);
					free(outBuf);
					return;
				}
				else
				{
					fprintf(stderr, "%s: Decompression failed; an error occurred during decompression\n", inFile);
					if (inBuf != NULL)
						free(inBuf);
					if (indecmpfsBuf != NULL)
						free(indecmpfsBuf);
					free(outBuf);
					return;
				}
			}
		}
		if (uncmpedsize != filesize)
		{
			fprintf(stderr, "%s: Decompression failed; uncompressed data block too small\n", inFile);
			if (inBuf != NULL)
				free(inBuf);
			if (indecmpfsBuf != NULL)
				free(indecmpfsBuf);
			free(outBuf);
			return;
		}
	}
	else
	{
		fprintf(stderr, "%s: Decompression failed; unknown compression type %u\n", inFile, (unsigned int) EndianU32_LtoN(*(UInt32 *) (indecmpfsBuf + 4)));
		if (inBuf != NULL)
			free(inBuf);
		if (indecmpfsBuf != NULL)
			free(indecmpfsBuf);
		free(outBuf);
		return;
	}
	
	if (chflags(inFile, (~UF_COMPRESSED) & inFileInfo->st_flags) < 0)
	{
		fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
		if (inBuf != NULL)
			free(inBuf);
		if (indecmpfsBuf != NULL)
			free(indecmpfsBuf);
		free(outBuf);
		return;
	}
	
	in = fopen(inFile, "r+");
	if (in == NULL)
	{
		if (chflags(inFile, UF_COMPRESSED & inFileInfo->st_flags) < 0)
			fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
		fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
		if (inBuf != NULL)
			free(inBuf);
		if (indecmpfsBuf != NULL)
			free(indecmpfsBuf);
		free(outBuf);
		utimes(inFile, times);
		return;
	}
	
	if (fwrite(outBuf, filesize, 1, in) != 1)
	{
		fclose(in);
		if (chflags(inFile, UF_COMPRESSED | inFileInfo->st_flags) < 0)
		{
			fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
		}
		if (inBuf != NULL)
			free(inBuf);
		if (indecmpfsBuf != NULL)
			free(indecmpfsBuf);
		free(outBuf);
		utimes(inFile, times);
		fprintf(stderr, "%s: Error writing to file\n", inFile);
		return;
	}
	
	fclose(in);
	
	if (removexattr(inFile, "com.apple.decmpfs", XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
	{
		fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
	}
	if (EndianU32_LtoN(*(UInt32 *) (indecmpfsBuf + 4)) == 4 && 
		removexattr(inFile, "com.apple.ResourceFork", XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
	{
		fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
	}
	
	if (inBuf != NULL)
		free(inBuf);
	if (indecmpfsBuf != NULL)
		free(indecmpfsBuf);
	free(outBuf);
	utimes(inFile, times);
}

bool checkForHardLink(const char *filepath, const struct stat *fileInfo, const struct folder_info *folderinfo)
{
	static ino_t *hardLinks = NULL;
	static char **paths = NULL, *list_item;
	static long int currSize = 0, numLinks = 0;
	long int right_pos, left_pos = 0, curr_pos = 1;
	
	if (fileInfo != NULL && fileInfo->st_nlink > 1)
	{
		if (hardLinks == NULL)
		{
			currSize = 1;
			hardLinks = (ino_t *) malloc(currSize * sizeof(ino_t));
			if (hardLinks == NULL)
			{
				fprintf(stderr, "Malloc error allocating memory for list of file hard links, exiting...\n");
				exit(ENOMEM);
			}
			paths = (char **) malloc(currSize * sizeof(char *));
			if (paths == NULL)
			{
				fprintf(stderr, "Malloc error allocating memory for list of file hard links, exiting...\n");
				exit(ENOMEM);
			}
		}
		
		if (numLinks > 0)
		{
			left_pos = 0;
			right_pos = numLinks + 1;
			
			while (hardLinks[curr_pos-1] != fileInfo->st_ino)
			{
				curr_pos = (right_pos - left_pos) / 2;
				if (curr_pos == 0) break;
				curr_pos += left_pos;
				if (hardLinks[curr_pos-1] > fileInfo->st_ino)
					right_pos = curr_pos;
				else if (hardLinks[curr_pos-1] < fileInfo->st_ino)
					left_pos = curr_pos;
			}
			if (curr_pos != 0 && hardLinks[curr_pos-1] == fileInfo->st_ino)
			{
				if (strcmp(filepath, paths[curr_pos-1]) != 0 || strlen(filepath) != strlen(paths[curr_pos-1]))
				{
					if (folderinfo->print_info > 1)
						printf("%s: skipping, hard link to this %s exists at %s\n", filepath, (fileInfo->st_mode & S_IFDIR) ? "folder" : "file", paths[curr_pos-1]);
					return TRUE;
				}
				else
					return FALSE;
			}
		}
		if (currSize < numLinks + 1)
		{
			currSize *= 2;
			hardLinks = (ino_t *) realloc(hardLinks, currSize * sizeof(ino_t));
			if (hardLinks == NULL)
			{
				fprintf(stderr, "Malloc error allocating memory for list of file hard links, exiting...\n");
				exit(ENOMEM);
			}
			paths = (char **) realloc(paths, currSize * sizeof(char *));
			if (paths == NULL)
			{
				fprintf(stderr, "Malloc error allocating memory for list of file hard links, exiting...\n");
				exit(ENOMEM);
			}
		}
		if ((numLinks != 0) && ((numLinks - 1) >= left_pos))
		{
			memmove(&hardLinks[left_pos+1], &hardLinks[left_pos], (numLinks - left_pos) * sizeof(ino_t));
			if (paths != NULL)
				memmove(&paths[left_pos+1], &paths[left_pos], (numLinks - left_pos) * sizeof(char *));
		}
		hardLinks[left_pos] = fileInfo->st_ino;
		list_item = (char *) malloc(strlen(filepath) + 1);
		strcpy(list_item, filepath);
		paths[left_pos] = list_item;
		numLinks++;
	}
	else if (fileInfo == NULL && hardLinks != NULL)
	{
		free(hardLinks);
		hardLinks = NULL;
		currSize = 0;
		numLinks = 0;
		if (paths != NULL)
		{
			for (curr_pos = 0; curr_pos < numLinks; curr_pos++)
				free(paths[curr_pos]);
			free(paths);
		}
	}
	return FALSE;
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

void printFileInfo(const char *filepath, struct stat *fileinfo, bool appliedcomp)
{
	char *xattrnames, *curr_attr, *filetype;
	ssize_t xattrnamesize, xattrssize = 0, xattrsize, RFsize = 0, compattrsize = 0;
	long long int filesize, filesize_rounded;
	int numxattrs = 0, numhiddenattr = 0;
	bool hasRF = FALSE;
	
	printf("%s:\n", filepath);
	
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
			if (strcmp(curr_attr, "com.apple.ResourceFork") == 0 && strlen(curr_attr) == 22)
			{
				RFsize += xattrsize;
				hasRF = TRUE;
				numhiddenattr++;
			}
			else if (strcmp(curr_attr, "com.apple.decmpfs") == 0 && strlen(curr_attr) == 17)
			{
				compattrsize += xattrsize;
				numhiddenattr++;
			}
			else
				xattrssize += xattrsize;
		}
		free(xattrnames);
	}
	
	if ((fileinfo->st_flags & UF_COMPRESSED) == 0)
	{
		if (appliedcomp)
			printf("Unable to compress file.\n");
		else
			printf("File is not HFS+ compressed.\n");
		if ((filetype = getFileType(filepath)) != NULL)
		{
			printf("File content type: %s\n", filetype);
			free(filetype);
		}
		if (hasRF)
		{
			printf("File data fork size: %lld bytes\n", fileinfo->st_size);
			printf("File resource fork size: %ld bytes\n", RFsize);
			filesize_rounded = filesize = fileinfo->st_size;
			filesize_rounded += (filesize_rounded % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize_rounded % fileinfo->st_blksize) : 0;
			filesize += RFsize;
			filesize_rounded += RFsize;
			filesize_rounded += (filesize_rounded % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize_rounded % fileinfo->st_blksize) : 0;
			printf("File size (data fork + resource fork; reported size by Mac OS X Finder): %s\n", getSizeStr(filesize, filesize_rounded));
		}
		else
		{
			filesize_rounded = filesize = fileinfo->st_size;
			filesize_rounded += (filesize % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize % fileinfo->st_blksize) : 0;
			printf("File data fork size (reported size by Mac OS X Finder): %s\n", getSizeStr(filesize, filesize_rounded));
		}
		printf("Number of extended attributes: %d\n", numxattrs - numhiddenattr);
		printf("Total size of extended attribute data: %ld bytes\n", xattrssize);
		printf("Approximate overhead of extended attributes: %ld bytes\n", ((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey));
		filesize = fileinfo->st_size;
		filesize += (filesize % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize % fileinfo->st_blksize) : 0;
		filesize += RFsize;
		filesize += (filesize % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize % fileinfo->st_blksize) : 0;
		filesize += compattrsize + xattrssize + (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFile);
		printf("Approximate total file size (data fork + resource fork + EA + EA overhead + file overhead): %s\n", getSizeStr(filesize, filesize));
	}
	else
	{
		if (!appliedcomp)
			printf("File is HFS+ compressed.\n");
		if ((filetype = getFileType(filepath)) != NULL)
		{
			printf("File content type: %s\n", filetype);
			free(filetype);
		}
		filesize = fileinfo->st_size;
		printf("File size (uncompressed data fork; reported size by Mac OS 10.6+ Finder): %s\n", getSizeStr(filesize, filesize));
		filesize_rounded = filesize = RFsize;
		filesize_rounded += (filesize_rounded % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize_rounded % fileinfo->st_blksize) : 0;
		printf("File size (compressed data fork - decmpfs xattr; reported size by Mac OS 10.0-10.5 Finder): %s\n", getSizeStr(filesize, filesize_rounded));
		filesize_rounded = filesize = RFsize;
		filesize_rounded += (filesize_rounded % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize_rounded % fileinfo->st_blksize) : 0;
		filesize += compattrsize;
		filesize_rounded += compattrsize;
		printf("File size (compressed data fork): %s\n", getSizeStr(filesize, filesize_rounded));
		printf("Compression savings: %0.1f%%\n", (1.0 - (((double) RFsize + compattrsize) / fileinfo->st_size)) * 100.0);
		printf("Number of extended attributes: %d\n", numxattrs - numhiddenattr);
		printf("Total size of extended attribute data: %ld bytes\n", xattrssize);
		printf("Approximate overhead of extended attributes: %ld bytes\n", ((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey));
		filesize = RFsize;
		filesize += (filesize % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize % fileinfo->st_blksize) : 0;
		filesize += compattrsize + xattrssize + (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFile);
		printf("Approximate total file size (compressed data fork + EA + EA overhead + file overhead): %s\n", getSizeStr(filesize, filesize));
	}
}

void process_file(const char *filepath, const char *filetype, struct stat *fileinfo, struct folder_info *folderinfo)
{
	char *xattrnames, *curr_attr;
	const char *fileextension = NULL;
	ssize_t xattrnamesize, xattrssize = 0, xattrsize, RFsize = 0, compattrsize = 0;
	long long int filesize, filesize_rounded;
	int numxattrs = 0, numhiddenattr = 0, i;
	struct filetype_info *filetypeinfo = NULL;
	bool filetype_found = FALSE;
	
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
			if (strcmp(curr_attr, "com.apple.ResourceFork") == 0 && strlen(curr_attr) == 22)
			{
				RFsize += xattrsize;
				numhiddenattr++;
			}
			else if (strcmp(curr_attr, "com.apple.decmpfs") == 0 && strlen(curr_attr) == 17)
			{
				compattrsize += xattrsize;
				numhiddenattr++;
			}
			else
				xattrssize += xattrsize;
		}
		free(xattrnames);
	}
	
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
	
	if ((fileinfo->st_flags & UF_COMPRESSED) == 0)
	{
		filesize_rounded = filesize = fileinfo->st_size;
		filesize_rounded += (filesize_rounded % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize_rounded % fileinfo->st_blksize) : 0;
		filesize += RFsize;
		filesize_rounded += RFsize;
		filesize_rounded += (filesize_rounded % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize_rounded % fileinfo->st_blksize) : 0;
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
		filesize = fileinfo->st_size;
		filesize += (filesize % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize % fileinfo->st_blksize) : 0;
		filesize += RFsize;
		filesize += (filesize % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize % fileinfo->st_blksize) : 0;
		filesize += compattrsize + xattrssize + (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFile);
		folderinfo->total_size += filesize;
		if (filetypeinfo != NULL && filetype_found)
			filetypeinfo->total_size += filesize;
	}
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
				printf("File size (uncompressed data fork; reported size by Mac OS 10.6+ Finder): %s\n", getSizeStr(filesize, filesize));
				filesize_rounded = filesize = RFsize;
				filesize_rounded += (filesize_rounded % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize_rounded % fileinfo->st_blksize) : 0;
				printf("File size (compressed data fork - decmpfs xattr; reported size by Mac OS 10.0-10.5 Finder): %s\n", getSizeStr(filesize, filesize_rounded));
				filesize_rounded = filesize = RFsize;
				filesize_rounded += (filesize_rounded % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize_rounded % fileinfo->st_blksize) : 0;
				filesize += compattrsize;
				filesize_rounded += compattrsize;
				printf("File size (compressed data fork): %s\n", getSizeStr(filesize, filesize_rounded));
				printf("Compression savings: %0.1f%%\n", (1.0 - (((double) RFsize + compattrsize) / fileinfo->st_size)) * 100.0);
				printf("Number of extended attributes: %d\n", numxattrs - numhiddenattr);
				printf("Total size of extended attribute data: %ld bytes\n", xattrssize);
				printf("Approximate overhead of extended attributes: %ld bytes\n", ((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey));
				filesize = RFsize;
				filesize += (filesize % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize % fileinfo->st_blksize) : 0;
				filesize += compattrsize + xattrssize + (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFile);
				printf("Approximate total file size (compressed data fork + EA + EA overhead + file overhead): %s\n", getSizeStr(filesize, filesize));
			}
			else if (!folderinfo->compress_files)
			{
				printf("%s\n", filepath);
			}
		}

		filesize_rounded = filesize = fileinfo->st_size;
		filesize_rounded += (filesize_rounded % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize_rounded % fileinfo->st_blksize) : 0;
		folderinfo->uncompressed_size += filesize;
		folderinfo->uncompressed_size_rounded += filesize_rounded;
		if (filetypeinfo != NULL && filetype_found)
		{
			filetypeinfo->uncompressed_size += filesize;
			filetypeinfo->uncompressed_size_rounded += filesize_rounded;
		}
		filesize_rounded = filesize = RFsize;
		filesize_rounded += (filesize_rounded % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize_rounded % fileinfo->st_blksize) : 0;
		folderinfo->compressed_size += filesize;
		folderinfo->compressed_size_rounded += filesize_rounded;
		folderinfo->compattr_size += compattrsize;
		if (filetypeinfo != NULL && filetype_found)
		{
			filetypeinfo->compressed_size += filesize;
			filetypeinfo->compressed_size_rounded += filesize_rounded;
			filetypeinfo->compattr_size += compattrsize;
		}
		filesize = RFsize;
		filesize += (filesize % fileinfo->st_blksize) ? fileinfo->st_blksize - (filesize % fileinfo->st_blksize) : 0;
		filesize += compattrsize + xattrssize + (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFile);
		folderinfo->total_size += filesize;
		folderinfo->num_compressed++;
		if (filetypeinfo != NULL && filetype_found)
		{
			filetypeinfo->total_size += filesize;
			filetypeinfo->num_compressed++;
		}
	}
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
		if ((volume_search || strncasecmp("/Volumes/", currfile->fts_path, 9) != 0 || strlen(currfile->fts_path) < 9) &&
			(strncasecmp("/dev/", currfile->fts_path, 5) != 0 || strlen(currfile->fts_path) < 5))
		{
			if (S_ISDIR(currfile->fts_statp->st_mode) && currfile->fts_ino != 2)
			{
				if (currfile->fts_info & FTS_D)
				{
					if (!folderinfo->check_hard_links || !checkForHardLink(currfile->fts_path, currfile->fts_statp, folderinfo))
					{
						numxattrs = 0;
						xattrssize = 0;
						
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
									free(xattrnames);
									continue;
								}
								numxattrs++;
								xattrssize += xattrsize;
							}
							free(xattrnames);
						}
						folderinfo->num_folders++;
						folderinfo->total_size += xattrssize + (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFolder);
					}
					else
					{
						folderinfo->num_hard_link_folders++;
						fts_set(currfolder, currfile, FTS_SKIP);
						
						folderinfo->num_folders++;
						folderinfo->total_size += sizeof(HFSPlusCatalogFolder);
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
							compressFile(currfile->fts_path, currfile->fts_statp, folderinfo->maxSize, folderinfo->compressionlevel, folderinfo->allowLargeBlocks, folderinfo->minSavings, folderinfo->check_files);
						lstat(currfile->fts_path, currfile->fts_statp);
						if (((currfile->fts_statp->st_flags & UF_COMPRESSED) == 0) && folderinfo->print_files)
						{
							if (folderinfo->print_info > 0)
								printf("Unable to compress: ");
							printf("%s\n", currfile->fts_path);
						}
					}
					process_file(currfile->fts_path, filetype, currfile->fts_statp, folderinfo);
				}
				else
				{
					folderinfo->num_hard_link_files++;
					
					folderinfo->num_files++;
					folderinfo->total_size += sizeof(HFSPlusCatalogFile);
					if (filetype_found && (filetypeinfo = getFileTypeInfo(currfile->fts_path, filetype, folderinfo)) != NULL)
					{
						filetypeinfo->num_hard_link_files++;
						
						filetypeinfo->num_files++;
						filetypeinfo->total_size += sizeof(HFSPlusCatalogFile);
					}
				}
				if (filetype != NULL) free(filetype);
			}
		}
		else
			fts_set(currfolder, currfile, FTS_SKIP);
	} while ((currfile = fts_read(currfolder)) != NULL);
	checkForHardLink(NULL, NULL, NULL);
	fts_close(currfolder);
}

void printUsage()
{
	printf("afsctool 1.6.4 (build 34)\n"
		   "Report if file is HFS+ compressed:                        afsctool [-v] file\n"
		   "Report if folder contains HFS+ compressed files:          afsctool [-fvvi] [-t <ContentType/Extension>] folder\n"
		   "List HFS+ compressed files in folder:                     afsctool -l[fvv] folder\n"
		   "Decompress HFS+ compressed file or folder:                afsctool -d[i] [-t <ContentType>] file/folder\n"
		   "Create archive file with compressed data in data fork:    afsctool -a[d] src dst\n"
		   "Extract HFS+ compression archive to file:                 afsctool -x[d] src dst\n"
		   "Apply HFS+ compression to file or folder:                 afsctool -c[nlfvvi] [-<level>] [-m <size>] [-s <percentage>] [-t <ContentType>] file/folder\n\n"
		   "Options:\n"
		   "-v Increase verbosity level\n"
		   "-f Detect hard links\n"
		   "-l List files that are HFS+ compressed (or if the -c option is given, files which fail to compress)\n"
		   "-L Allow large compressed blocks (not recommended)\n"
		   "-n Do not verify files after compression (not recommended)\n"
		   "-m <size> Largest file size to compress, in bytes\n"
		   "-s <percentage> For compression to be applied, compression savings must be at least this percentage\n"
		   "-t <ContentType/Extension> Return statistics for files of given content type and when compressing,\n"
		   "                           if this option is given then only files of content type(s) or extension(s) specified with this option will be compressed\n"
		   "-i Compress or show statistics for files that don't have content type(s) or extension(s) given by -t <ContentType/Extension> instead of those that do\n"
		   "-<level> Compression level to use when compressing (ranging from 1 to 9, with 1 being the fastest and 9 being the best - default is 5)\n");
}

int main (int argc, const char * argv[])
{
	int i, j;
	struct stat fileinfo, dstfileinfo;
	struct folder_info folderinfo;
	struct filetype_info alltypesinfo;
	FTS *currfolder;
	FTSENT *currfile;
	char *folderarray[2], *fullpath = NULL, *fullpathdst = NULL, *cwd, *fileextension, *filetype = NULL;
	int printVerbose = 0, compressionlevel = 5;
	double minSavings = 0.0;
	long long int foldersize, foldersize_rounded, filesize, filesize_rounded, maxSize = 0;
	bool printDir = FALSE, decomp = FALSE, createfile = FALSE, extractfile = FALSE, applycomp = FALSE, fileCheck = TRUE, argIsFile, hardLinkCheck = FALSE, dstIsFile, free_src = FALSE, free_dst = FALSE, invert_filetypelist = FALSE, allowLargeBlocks = FALSE, filetype_found;
	FILE *afscFile, *outFile;
	char *xattrnames, *curr_attr, header[4];
	ssize_t xattrnamesize, xattrsize, getxattrret, xattrPos;
	mode_t outFileMode;
	void *attr_buf;
	UInt16 big16;
	UInt64 big64;
	
	folderinfo.filetypeslist = NULL;
	folderinfo.filetypeslistlen = 0;
	folderinfo.filetypeslistsize = 0;
	
	if (argc < 2)
	{
		printUsage();
		exit(EINVAL);
	}
	
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
				case 'i':
					if (createfile || extractfile)
					{
						printUsage();
						exit(EINVAL);
					}
					invert_filetypelist = TRUE;
					break;
				default:
					printUsage();
					exit(EINVAL);
					break;
			}
		}
	}
	
	if (i == argc || ((createfile || extractfile) && (argc - i < 2)))
	{
		printUsage();
		exit(EINVAL);
	}
	else if (createfile || extractfile)
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
			fullpathdst = (char *) malloc(strlen(cwd) + strlen(argv[i+1]) + 2);
			sprintf(fullpathdst, "%s/%s", cwd, argv[i+1]);
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
		fullpath = (char *) argv[i];
	
	if (lstat(fullpath, &fileinfo) < 0)
	{
		fprintf(stderr, "%s: %s\n", fullpath, strerror(errno));
		return -1;
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
		return -1;
	}
	
	if (applycomp && argIsFile)
	{
		compressFile(fullpath, &fileinfo, maxSize, compressionlevel, allowLargeBlocks, minSavings, fileCheck);
		lstat(fullpath, &fileinfo);
	}
	
	if (createfile)
	{
		if (!argIsFile)
		{
			fprintf(stderr, "%s: File required, this is a folder\n", fullpath);
			return -1;
		}
		else if ((fileinfo.st_flags & UF_COMPRESSED) == 0)
		{
			fprintf(stderr, "%s: HFS+ compressed file required, this file is not HFS+ compressed\n", fullpath);
			return -1;
		}
		
		afscFile = fopen(fullpathdst, "w");
		if (afscFile == NULL)
		{
			fprintf(stderr, "%s: %s\n", fullpathdst, strerror(errno));
			return -1;
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
					if (((strcmp(curr_attr, "com.apple.ResourceFork") == 0 && strlen(curr_attr) == 22) ||
						(strcmp(curr_attr, "com.apple.decmpfs") == 0 && strlen(curr_attr) == 17)) &&
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
			if (decomp)
				decompressFile(fullpath, &fileinfo);
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
			return -1;
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
				return -1;
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
						return -1;
					}
					decompressFile(fullpathdst, &dstfileinfo);
				}
			}
		}
	}
	else if (decomp && argIsFile)
	{
		decompressFile(fullpath, &fileinfo);
	}
	else if (decomp)
	{
		if ((currfolder = fts_open(folderarray, FTS_PHYSICAL, NULL)) == NULL)
		{
			fprintf(stderr, "%s: %s\n", fullpath, strerror(errno));
			exit(EACCES);
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
			if ((currfile->fts_statp->st_mode & S_IFDIR) == 0 && (folderinfo.filetypeslist == NULL || filetype_found))
				decompressFile(currfile->fts_path, currfile->fts_statp);
			if (filetype != NULL) free(filetype);
		}
		fts_close(currfolder);
	}
	else if (argIsFile && printVerbose == 0)
	{
		if (applycomp)
		{
			if ((fileinfo.st_flags & UF_COMPRESSED) == 0)
				printf("Unable to compress file.\n");
		}
		else
		{
			if ((fileinfo.st_flags & UF_COMPRESSED) != 0)
				printf("File is HFS+ compressed.\n");
			else
				printf("File is not HFS+ compressed.\n");
		}
	}
	else if (argIsFile && printVerbose > 0)
	{
		printFileInfo(fullpath, &fileinfo, applycomp);
	}
	else if (!argIsFile)
	{
		if ((currfolder = fts_open(folderarray, FTS_PHYSICAL, NULL)) == NULL)
		{
			fprintf(stderr, "%s: %s\n", fullpath, strerror(errno));
			exit(EACCES);
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
		folderinfo.print_info = printVerbose;
		folderinfo.print_files = printDir;
		folderinfo.compress_files = applycomp;
		folderinfo.check_files = fileCheck;
		folderinfo.allowLargeBlocks = allowLargeBlocks;
		folderinfo.compressionlevel = compressionlevel;
		folderinfo.minSavings = minSavings;
		folderinfo.maxSize = maxSize;
		folderinfo.check_hard_links = hardLinkCheck;
		folderinfo.filetypes = NULL;
		folderinfo.numfiletypes = 0;
		folderinfo.filetypessize = 0;
		folderinfo.invert_filetypelist = invert_filetypelist;
		process_folder(currfolder, &folderinfo);
		folderinfo.num_folders--;
		if (printVerbose > 0 || !printDir)
		{
			if (printDir) printf("\n");
			printf("%s:\n", fullpath);
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
						printf("Number of HFS+ compressed files: %lld\n", folderinfo.filetypes[i].num_compressed);
					if (printVerbose > 0 && (!folderinfo.invert_filetypelist))
					{
						printf("Total number of files: %lld\n", folderinfo.filetypes[i].num_files);
						if (hardLinkCheck)
							printf("Total number of file hard links: %lld\n", folderinfo.filetypes[i].num_hard_link_files);
						filesize = folderinfo.filetypes[i].uncompressed_size;
						filesize_rounded = folderinfo.filetypes[i].uncompressed_size_rounded;
						if (folderinfo.filetypes[i].num_hard_link_files == 0 || !hardLinkCheck)
							printf("File(s) size (uncompressed; reported size by Mac OS 10.6+ Finder): %s\n", getSizeStr(filesize, filesize_rounded));
						else
							printf("File(s) size (uncompressed): %s\n", getSizeStr(filesize, filesize_rounded));
						filesize = folderinfo.filetypes[i].compressed_size;
						filesize_rounded = folderinfo.filetypes[i].compressed_size_rounded;
						if (folderinfo.filetypes[i].num_hard_link_files == 0 || !hardLinkCheck)
							printf("File(s) size (compressed - decmpfs xattr; reported size by Mac OS 10.0-10.5 Finder): %s\n", getSizeStr(filesize, filesize_rounded));
						else
							printf("File(s) size (compressed - decmpfs xattr): %s\n", getSizeStr(filesize, filesize_rounded));
						filesize = folderinfo.filetypes[i].compressed_size + folderinfo.filetypes[i].compattr_size;
						filesize_rounded = folderinfo.filetypes[i].compressed_size_rounded + folderinfo.filetypes[i].compattr_size;
						printf("File(s) size (compressed): %s\n", getSizeStr(filesize, filesize_rounded));
						printf("Compression savings: %0.1f%%\n", (1.0 - ((float) (folderinfo.filetypes[i].compressed_size + folderinfo.filetypes[i].compattr_size) / folderinfo.filetypes[i].uncompressed_size)) * 100.0);
						filesize = folderinfo.filetypes[i].total_size;
						printf("Approximate total file(s) size (files + file overhead): %s\n", getSizeStr(filesize, filesize));
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
					printf("Number of HFS+ compressed files: %lld\n", alltypesinfo.num_compressed);
					if (printVerbose > 0)
					{
						printf("Total number of files: %lld\n", alltypesinfo.num_files);
						if (hardLinkCheck)
							printf("Total number of file hard links: %lld\n", alltypesinfo.num_hard_link_files);
						filesize = alltypesinfo.uncompressed_size;
						filesize_rounded = alltypesinfo.uncompressed_size_rounded;
						if (alltypesinfo.num_hard_link_files == 0 || !hardLinkCheck)
							printf("File(s) size (uncompressed; reported size by Mac OS 10.6+ Finder): %s\n", getSizeStr(filesize, filesize_rounded));
						else
							printf("File(s) size (uncompressed): %s\n", getSizeStr(filesize, filesize_rounded));
						filesize = alltypesinfo.compressed_size;
						filesize_rounded = alltypesinfo.compressed_size_rounded;
						if (alltypesinfo.num_hard_link_files == 0 || !hardLinkCheck)
							printf("File(s) size (compressed - decmpfs xattr; reported size by Mac OS 10.0-10.5 Finder): %s\n", getSizeStr(filesize, filesize_rounded));
						else
							printf("File(s) size (compressed - decmpfs xattr): %s\n", getSizeStr(filesize, filesize_rounded));
						filesize = alltypesinfo.compressed_size + alltypesinfo.compattr_size;
						filesize_rounded = alltypesinfo.compressed_size_rounded + alltypesinfo.compattr_size;
						printf("File(s) size (compressed): %s\n", getSizeStr(filesize, filesize_rounded));
						printf("Compression savings: %0.1f%%\n", (1.0 - ((float) (alltypesinfo.compressed_size + alltypesinfo.compattr_size) / alltypesinfo.uncompressed_size)) * 100.0);
						filesize = alltypesinfo.total_size;
						printf("Approximate total file(s) size (files + file overhead): %s\n", getSizeStr(filesize, filesize));
					}
				}
				printf("\n");
			}
			if (folderinfo.num_compressed == 0 && !applycomp)
				printf("Folder contains no compressed files\n");
			else if (folderinfo.num_compressed == 0 && applycomp)
				printf("No compressable files in folder\n");
			else
				printf("Number of HFS+ compressed files: %lld\n", folderinfo.num_compressed);
			if (printVerbose > 0)
			{
				printf("Total number of files: %lld\n", folderinfo.num_files);
				if (hardLinkCheck)
					printf("Total number of file hard links: %lld\n", folderinfo.num_hard_link_files);
				printf("Total number of folders: %lld\n", folderinfo.num_folders);
				if (hardLinkCheck)
					printf("Total number of folder hard links: %lld\n", folderinfo.num_hard_link_folders);
				printf("Total number of items (number of files + number of folders): %lld\n", folderinfo.num_files + folderinfo.num_folders);
				foldersize = folderinfo.uncompressed_size;
				foldersize_rounded = folderinfo.uncompressed_size_rounded;
				if ((folderinfo.num_hard_link_files == 0 && folderinfo.num_hard_link_folders == 0) || !hardLinkCheck)
					printf("Folder size (uncompressed; reported size by Mac OS 10.6+ Finder): %s\n", getSizeStr(foldersize, foldersize_rounded));
				else
					printf("Folder size (uncompressed): %s\n", getSizeStr(foldersize, foldersize_rounded));
				foldersize = folderinfo.compressed_size;
				foldersize_rounded = folderinfo.compressed_size_rounded;
				if ((folderinfo.num_hard_link_files == 0 && folderinfo.num_hard_link_folders == 0) || !hardLinkCheck)
					printf("Folder size (compressed - decmpfs xattr; reported size by Mac OS 10.0-10.5 Finder): %s\n", getSizeStr(foldersize, foldersize_rounded));
				else
					printf("Folder size (compressed - decmpfs xattr): %s\n", getSizeStr(foldersize, foldersize_rounded));
				foldersize = folderinfo.compressed_size + folderinfo.compattr_size;
				foldersize_rounded = folderinfo.compressed_size_rounded + folderinfo.compattr_size;
				printf("Folder size (compressed): %s\n", getSizeStr(foldersize, foldersize_rounded));
				printf("Compression savings: %0.1f%%\n", (1.0 - ((float) (folderinfo.compressed_size + folderinfo.compattr_size) / folderinfo.uncompressed_size)) * 100.0);
				foldersize = folderinfo.total_size;
				printf("Approximate total folder size (files + file overhead + folder overhead): %s\n", getSizeStr(foldersize, foldersize));
			}
		}
	}
	
	if (free_src)
		free(fullpath);
	if (free_dst)
		free(fullpathdst);
	if (folderinfo.filetypeslist != NULL)
		free(folderinfo.filetypeslist);
	
	return 0;
}
