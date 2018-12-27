// kate: auto-insert-doxygen true; backspace-indents true; indent-width 4; keep-extra-spaces true; replace-tabs true; tab-indents true; tab-width 4;

/*
 * @file utils.h
 * @file utils.cpp
 * Copyright 2018 René J.V. Bertin
 * This code is made available under No License At All
 */

#include <string>
#include <sparsehash/dense_hash_map>
#include <sys/types.h>
#include <sys/stat.h>

#include "utils.h"

// #include <iostream>
// #include "prettyprint.hpp"

using namespace std;

bool checkForHardLink(const char *filepath, const struct stat *fileInfo, const struct folder_info *folderinfo)
{
	static long int numLinks = -1;
	static google::dense_hash_map<ino_t,string> ino2PathMap;
    bool ret = false;

	if (numLinks == -1) {
		ino2PathMap.set_empty_key(0);
		ino2PathMap.clear();
		numLinks = 0;
	}
	if (fileInfo != nullptr && S_ISREG(fileInfo->st_mode) && fileInfo->st_nlink > 1) {
		if (ino2PathMap.count(fileInfo->st_ino)) {
			const auto linkPath = ino2PathMap[fileInfo->st_ino];
			if (linkPath != string(filepath)) {
				if (folderinfo->print_info > 1) {
					fprintf(stderr, "%s: skipping, hard link to this file exists at %s\n",
							filepath,  linkPath.c_str());
				}
				ret = true;
			}
		} else {
			ino2PathMap[fileInfo->st_ino] = string(filepath);
            numLinks++;
		}
	} else if (fileInfo == nullptr && numLinks != 0) {
// 		cerr << "ino2PathMap=" << ino2PathMap << endl;
		numLinks = 0;
		ino2PathMap.clear();
	}
	return ret;
}

