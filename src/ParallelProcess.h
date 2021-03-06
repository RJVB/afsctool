/*
 * @file ParallelProcess.h
 * Copyright 2015 René J.V. Bertin
 *  This code is made available under No License At All
 */
#ifndef _PARALLELPROCESS_H

#include "fsctool.h"

// =============== C++ code =============== //
#ifndef __cplusplus

typedef void ParallelFileProcessor;
typedef void FileProcessor;

#else

class ParallelFileProcessor;
class FileProcessor;

#endif // __cplusplus

// =============== Functions exported to C code =============== //
#ifdef __cplusplus
extern "C" {
#endif //__cplusplus

ParallelFileProcessor *createParallelProcessor(const int n, const int r, const int verboseLevel);
void releaseParallelProcessor(ParallelFileProcessor *p);
bool addFileToParallelProcessor(ParallelFileProcessor *p, const char *inFile,
								const struct stat *inFileInfo, struct folder_info *folderinfo,
								const bool ownInfo);
size_t filesInParallelProcessor(ParallelFileProcessor *p);
bool sortFilesInParallelProcessorBySize(ParallelFileProcessor *p);
// attempt to lock the ioLock; returns a success value that should be passed to unLockParallelProcessorIO()
bool lockParallelProcessorIO(FileProcessor *worker);
// unlock the ioLock if it was previously locked by a call to lockParallelProcessorIO()
bool unLockParallelProcessorIO(FileProcessor *worker);
int currentParallelProcessorID(FileProcessor *worker);
bool changeParallelProcessorJobs(ParallelFileProcessor *p, const int n, const int r);
int runParallelProcessor(ParallelFileProcessor *p);
void stopParallelProcessor(ParallelFileProcessor *p);
struct folder_info *getParallelProcessorJobInfo(ParallelFileProcessor *p);

#ifdef __cplusplus
}
#endif //__cplusplus

#define _PARALLELPROCESS_H
#endif //_PARALLELPROCESS_H
