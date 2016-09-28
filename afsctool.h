// kate: auto-insert-doxygen true; backspace-indents true; indent-width 4; keep-extra-spaces true; replace-tabs true; tab-indents true; tab-width 4;

/*
 * @file afsctool.h
 * Copyright "brkirch" (https://brkirch.wordpress.com/afsctool/) 
 * This file created by and C++ sections (C) 2015 René J.V. Bertin
 * This code is made available under the GPL3 License
 * (See License.txt)
 */

#ifndef _AFSCTOOL_H

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
#include <hfs/hfs_format.h>

#ifdef __cplusplus
extern "C" {
#endif //__cplusplus

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
	bool backup_file;
#ifdef __cplusplus
public:
	folder_info()
	{
		memset( this, 0, sizeof(struct folder_info) );
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
//		   fprintf( stderr, "folder_info::init(%p): copying to this=%p\n", src, this );
		memcpy( this, src, sizeof(struct folder_info) );
		// we don't duplicate the filetypeslist!
		filetypeslist = NULL;
		filetypeslistlen = filetypeslistsize = 0;
	}
#endif
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

#ifdef SUPPORT_PARALLEL
#	ifdef __cplusplus
extern void compressFile(const char *inFile, struct stat *inFileInfo, struct folder_info *folderinfo, void *worker=NULL);
#	else
extern void compressFile(const char *inFile, struct stat *inFileInfo, struct folder_info *folderinfo, void *worker);
#	endif
#else
extern void compressFile(const char *inFile, struct stat *inFileInfo, struct folder_info *folderinfo);
#endif
extern long long process_file(const char *filepath, const char *filetype, struct stat *fileinfo, struct folder_info *folderinfo);
extern int afsctool (int argc, const char * argv[]);

#ifdef __cplusplus
}
#endif //__cplusplus

#define _AFSCTOOL_H
#endif //_AFSCTOOL_H
