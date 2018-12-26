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
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <fts.h>
#include <sys/xattr.h>
#ifdef __APPLE__
#include <hfs/hfs_format.h>

#include <AvailabilityMacros.h>
#include <MacTypes.h>
#else
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

#include "private/decmpfs.h"

#include "fsctool.h"

#ifdef __cplusplus
extern "C" {
#endif //__cplusplus

// some constants borrowed from libarchive
/*
 * HFS+ compression type.
 */
#define CMP_ZLIB_XATTR				3	/* ZLIB-compressed data is stored in the xattr. */
#define CMP_ZLIB_RESOURCE_FORK		4	/* ZLIB-compressed data is stored in the resource fork. */
#define CMP_LZVN_XATTR				7	/* LZVN-compressed data is stored in the xattr. */
#define CMP_LZVN_RESOURCE_FORK		8	/* LZVN-compressed data is stored in the resource fork. */
#define CMP_LZFSE_XATTR				11	/* LZVN-compressed data is stored in the xattr. */
#define CMP_LZFSE_RESOURCE_FORK		12	/* LZVN-compressed data is stored in the resource fork. */

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

typedef struct __attribute__((packed)) {
	char empty[24];
    UInt16 magic1, magic2, spacer1;
    UInt32 compression_magic;
    UInt32 magic3;
    UInt64 magic4;
	UInt32 spacer2;
} decmpfs_resource_zlib_trailer;

#ifdef HAS_LZVN
	// LZVN decmpfs compression starts with a table of 4-byte offsets into the resource
	// fork, terminated with the offset where no more compressed data is to be found.
	// The first offset is thus also sizeof(UInt32) (4) times the size of the table.
	typedef UInt32 lzvn_chunk_table;
#endif

extern int afsctool (int argc, const char * argv[]);

#ifdef __cplusplus
}
#endif //__cplusplus

#define _AFSCTOOL_H
#endif //_AFSCTOOL_H
