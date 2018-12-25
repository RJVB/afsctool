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

#include "fsctool.h"

#ifndef __APPLE__
#define FALSE false
#define TRUE true
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

#if 0
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
#endif //0

extern int zfsctool(int argc, const char *argv[]);

#ifdef __cplusplus
}
#endif //__cplusplus

#define _ZFSCTOOL_H
#endif //_ZFSCTOOL_H
