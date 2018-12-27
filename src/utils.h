// kate: auto-insert-doxygen true; backspace-indents true; indent-width 4; keep-extra-spaces true; replace-tabs true; tab-indents true; tab-width 4;

/*
 * @file utils.h
 * @file utils.cpp
 * Copyright 2018 René J.V. Bertin
 * This code is made available under No License At All
 */

#ifndef _UTILS_H

#include "fsctool.h"

#ifdef __cplusplus
extern "C" {
#endif //__cplusplus

extern bool checkForHardLink(const char *filepath, const struct stat *fileInfo, const struct folder_info *folderinfo);

#ifdef __cplusplus
}
#endif //__cplusplus

#define _UTILS_H
#endif //_UTILS_H
