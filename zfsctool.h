// kate: auto-insert-doxygen true; backspace-indents true; indent-width 4; keep-extra-spaces true; replace-tabs true; tab-indents true; tab-width 4;

/*
 * @file zfsctool.h
 * @file zfsctool.c
 * Copyright 2018 René J.V. Bertin
 * This code is made available under No License At All
 */

#ifndef _ZFSCTOOL_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <fts.h>
#include <sys/xattr.h>
#ifndef __APPLE__
#define false 0
#define FALSE 0
#define true !false
#define TRUE !FALSE
#   ifndef __cplusplus
typedef unsigned char bool;
#   endif
typedef u_int16_t UInt16;
typedef u_int32_t UInt32;
typedef u_int64_t UInt64;
#endif

#ifdef __cplusplus
extern "C" {
#endif //__cplusplus

struct folder_info {
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
	// set by compressFile():
	long long int data_compressed_size;
	int print_info;
	compression_type compressiontype;
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
	bool backup_file;
#ifdef __cplusplus
public:
	folder_info()
	{
		memset(this, 0, sizeof(struct folder_info));
	}
	folder_info(const struct folder_info *src)
	{
		init(src);
	}
	folder_info(const struct folder_info &src)
	{
		init(&src);
	}
private:
	void init(const struct folder_info *src)
	{
		memcpy(this, src, sizeof(struct folder_info));
		// we don't duplicate the filetypeslist!
		filetypeslist = NULL;
		filetypeslistlen = filetypeslistsize = 0;
	}
#endif
};

struct filetype_info {
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

#ifdef __cplusplus
extern void compressFile(const char *inFile, struct stat *inFileInfo, struct folder_info *folderinfo, void *worker = NULL);
#else
extern void compressFile(const char *inFile, struct stat *inFileInfo, struct folder_info *folderinfo, void *worker);
#endif
extern long long process_file(const char *filepath, const char *filetype, struct stat *fileinfo, struct folder_info *folderinfo);
extern int zfsctool(int argc, const char *argv[]);

#ifdef __cplusplus
}
#endif //__cplusplus

#define _ZFSCTOOL_H
#endif //_ZFSCTOOL_H
