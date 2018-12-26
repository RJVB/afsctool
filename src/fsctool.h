// kate: auto-insert-doxygen true; backspace-indents true; indent-width 4; keep-extra-spaces true; replace-tabs true; tab-indents true; tab-width 4;

/*
 * @file fsctool.h
 * Copyright "brkirch" (https://brkirch.wordpress.com/afsctool/) 
 * This file created by and C++ sections (C) 2018 René J.V. Bertin
 * This code is made available under the GPL3 License
 * (See License.txt)
 *
 * stuff share between afsctool, zfsctool and the parallel processing library functions
 */

#ifndef _FSCTOOL_H

#include <string.h>

#ifdef __cplusplus
#include <string>

extern "C" {
#endif //__cplusplus

// the various types of HFS/APFS compression. Not all are supported (on all OS versions).
typedef enum compression_type { NONE, ZLIB, LZVN, LZFSE,
	ZFS // used in zfsctool
} compression_type;

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
    bool onAPFS;
#ifndef __cplusplus
    void *z_compression;
#else
    std::string *z_compression;
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
	~folder_info()
    {
        delete z_compression;
    }
private:
	void init(const struct folder_info *src)
	{
//		   fprintf( stderr, "folder_info::init(%p): copying to this=%p\n", src, this );
		memcpy( this, src, sizeof(struct folder_info) );
		// we don't duplicate the filetypeslist!
		filetypeslist = NULL;
		filetypeslistlen = filetypeslistsize = 0;
        // nor the dynamic ZFS properties
        z_compression = nullptr;
	}
#endif
};

#ifdef SUPPORT_PARALLEL
#	ifdef __cplusplus
class FileProcessor;
extern void compressFile(const char *inFile, struct stat *inFileInfo, struct folder_info *folderinfo, FileProcessor *worker=NULL);
#	else
extern void compressFile(const char *inFile, struct stat *inFileInfo, struct folder_info *folderinfo, void *worker);
#	endif
#else
extern void compressFile(const char *inFile, struct stat *inFileInfo, struct folder_info *folderinfo, void *ignored);
#endif
extern long long process_file_info(const char *filepath, const char *filetype, struct stat *fileinfo, struct folder_info *folderinfo);

#ifdef __cplusplus
}
#endif //__cplusplus

#define _FSCTOOL_H
#endif //_FSCTOOL_H
