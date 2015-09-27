/*
 * @file ParallelProcess.h
 * Copyright 2015 René J.V. Bertin
 * This code is made available under the CPOL License
 * http://www.codeproject.com/info/cpol10.aspx
 */
#ifndef _PARALLELPROCESS_H

#include "afsctool.h"

// =============== C++ code =============== //
#ifndef __cplusplus

typedef struct ParallelFileProcessor ParallelFileProcessor;
typedef struct FileProcessor FileProcessor;

#endif // __cplusplus

// =============== Functions exported to C code =============== //
#ifdef __cplusplus
extern "C" {
#endif //__cplusplus

ParallelFileProcessor *createParallelProcessor(const int n);
void releaseParallelProcessor(ParallelFileProcessor *p);
bool addFileToParallelProcessor(ParallelFileProcessor *p, const char *inFile,
                                const struct stat *inFileInfo, struct folder_info *folderinfo,
                                const bool ownInfo);
size_t filesInParallelProcessor(ParallelFileProcessor *p);
// attempt to lock the ioLock; returns a success value that should be passed to unLockParallelProcessorIO()
bool lockParallelProcessorIO(FileProcessor *worker);
// unlock the ioLock if it was previously locked by a call to lockParallelProcessorIO()
bool unLockParallelProcessorIO(FileProcessor *worker);
int runParallelProcessor(ParallelFileProcessor *p);
void stopParallelProcessor(ParallelFileProcessor *p);
struct folder_info *getParallelProcessorJobInfo(ParallelFileProcessor *p);

#ifdef __cplusplus
}
#endif //__cplusplus

#define _PARALLELPROCESS_H
#endif //_PARALLELPROCESS_H