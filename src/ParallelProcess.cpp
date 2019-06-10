// kate: auto-insert-doxygen true; backspace-indents true; indent-width 4; keep-extra-spaces true; replace-tabs false; tab-indents true; tab-width 4;
/*
 * @file ParallelProcess.cpp
 * Copyright 2015 René J.V. Bertin
 *  This code is made available under No License At All
 */

#if defined(__MACH__)
#include <mach/mach_init.h>
#include <mach/thread_act.h>
#include <mach/mach_port.h>
#elif defined(linux)
#include <sys/time.h>
#include <sys/resource.h>
#endif

#include <algorithm>
#include <cmath>

#include "ParallelProcess_p.hpp"
#include "ParallelProcess.h"

// ================================= FileEntry methods =================================

FileEntry::FileEntry()
	:fileName("")
	,folderInfo(NULL)
	,freeFolderInfo(false)
	,compressedSize(0)
{
	memset( &fileInfo, 0, sizeof(fileInfo) );
}

FileEntry::FileEntry( const char *name, const struct stat *finfo, FolderInfo *dinfo, const bool ownInfo )
	:fileName(name)
	,fileInfo(*finfo)
	,folderInfo((ownInfo)? new FolderInfo(dinfo) : dinfo)
	,freeFolderInfo(ownInfo)
	,compressedSize(0)
{
}

FileEntry::FileEntry( const char *name, const struct stat *finfo, FolderInfo &dinfo )
{
	FileEntry( name, finfo, new FolderInfo(dinfo), true );
}

FileEntry::FileEntry(const FileEntry &ref)
{
	*this = ref;
}

FileEntry::~FileEntry()
{
	if( freeFolderInfo && folderInfo ){
//		   fprintf( stderr, "~FileEntry(%p): freeing \"%s\" with folderInfo %p\n", this, fileName.c_str(), folderInfo );
		if( folderInfo->filetypeslist != NULL ){
			free(folderInfo->filetypeslist);
		}
		delete folderInfo;
		folderInfo = NULL;
	}
}

FileEntry &FileEntry::operator =(const FileEntry &ref)
{
	if( this == &ref ){
		return *this;
	}
	fileName = ref.fileName;
	fileInfo = ref.fileInfo;
	if( ref.freeFolderInfo ){
		folderInfo = new FolderInfo(ref.folderInfo);
//		   fprintf( stderr, "FileEntry(FileEntry %p): duplicated folderInfo %p -> %p\n", &ref, ref.folderInfo, folderInfo );
	}
	else{
		folderInfo = ref.folderInfo;
	}
	freeFolderInfo = ref.freeFolderInfo;
	return *this;
}

void FileEntry::compress(FileProcessor *worker, ParallelFileProcessor *PP)
{
	if( PP->verbose() > 2){
		fprintf( stderr, "[%d] %s", worker->processorID(), fileName.c_str() ); fflush(stderr);
	}
	folderInfo->data_compressed_size = -1;
	compressFile( fileName.c_str(), &fileInfo, folderInfo, worker );
	if( PP->verbose() > 2){
		fputs( " .", stderr ); fflush(stderr);
	}
	if( PP->verbose() ){
		compressedSize = process_file_info( fileName.c_str(), NULL, &fileInfo, &PP->jobInfo );
#ifndef __APPLE__
		if (folderInfo->data_compressed_size != -1) {
			compressedSize = folderInfo->data_compressed_size;
		}
#endif
		if( PP->verbose() > 2){
			fputs( " .\n", stderr ); fflush(stderr);
		}
	}
}

// ================================= ParallelFileProcessor methods =================================

ParallelFileProcessor::ParallelFileProcessor(int n, int r, int verbose)
{
	threadPool.clear();
	nJobs = n;
	nReverse = r;
	nProcessing = 0;
	nProcessed = 0;
	allDoneEvent = NULL;
	ioLock = new CRITSECTLOCK(4000);
	ioLockedFlag = false;
	ioLockingThread = 0;
	verboseLevel = verbose;
	memset( &jobInfo, 0, sizeof(jobInfo) );
	z_dataSetInfo.set_empty_key(std::string());
	z_dataSetInfoForFile.set_empty_key(std::string());
	// there appears to be no reason to invoke set_deleted_key();
	// let's make sure:
	z_dataSetInfo.clear();
	z_dataSetInfoForFile.clear();
}

ParallelFileProcessor::~ParallelFileProcessor()
{
	if( verboseLevel > 1 && (listLockConflicts() || ioLock->lockCounter) ){
		fprintf( stderr, "Queue lock contention: %lux ; IO lock contention %lux\n",
				 listLockConflicts(), ioLock->lockCounter );
	}
	delete ioLock;
	if( allDoneEvent ){
		CloseHandle(allDoneEvent);
	}
	// the file->info map holds copies; just empty it.
	z_dataSetInfoForFile.clear();
	// delete any remaining values from the z_dataSetInfo map
	for (auto elem : z_dataSetInfo) {
		if (elem.second->autoDelete()) {
			delete elem.second;
		}
		// the following would be valid despite the lack of a call to set_deleted_key():
		// z_dataSetInfo.erase(elem.first);
	}
	// empty the map
	z_dataSetInfo.clear();
}

ParallelFileProcessor *createParallelProcessor(int n, int r, int verboseLevel)
{
	return new ParallelFileProcessor(n, r, verboseLevel);
}

// attempt to lock the ioLock; returns a success value
bool ParallelFileProcessor::lockIO()
{ DWORD pid = GetCurrentThreadId();
	if( ioLock ){
		bool wasLocked = (ioLock->IsLocked() && (ioLockingThread != pid));
		ioLock->Lock(ioLockedFlag);
		if( wasLocked ){
			ioLock->lockCounter += 1;
		}
	}
	if( ioLockedFlag ){
		ioLockingThread = pid;
	}
	return ioLockedFlag;
}

// unlock the ioLock
bool ParallelFileProcessor::unLockIO()
{
	if( ioLock ){
		ioLock->Unlock(ioLockedFlag);
		ioLockedFlag = ioLock->IsLocked();
	}
	if( !ioLockedFlag && ioLockingThread == GetCurrentThreadId() ){
		ioLockingThread = 0;
	}
	return ioLockedFlag;
}

// change the number of jobs
bool ParallelFileProcessor::setJobs(int n, int r)
{
	if (threadPool.empty() && n > 0 && n >= r) {
		nJobs = n;
		nReverse = r;
		return true;
	}
	return false;
}

int ParallelFileProcessor::run()
{ FileEntry entry;
  int i, nRequested = nJobs;
  double N = size(), prevPerc = 0;
	if( nJobs >= 1 ){
		allDoneEvent = CreateEvent( NULL, false, false, NULL );
	}
	for( i = 0 ; i < nJobs ; ++i ){
		// workers attacking the item list rear are created last
		// (not that this makes any difference except in the stats summary print out...)
		bool fromRear = i >= nJobs - nReverse;
		FileProcessor *thread = new FileProcessor(this, fromRear, i);
		if( thread ){
			threadPool.push_back(thread);
		}
		else{
			nJobs -= 1;
		}
	}
	if( nJobs != nRequested ){
		fprintf( stderr, "Parallel processing with %ld instead of %d threads\n", nJobs, nRequested );
	}
	const double startTime = HRTime_Time();
	for( i = 0 ; i < nJobs ; ++i ){
		threadPool[i]->Start();
	}
	if( allDoneEvent ){
	 DWORD waitResult = ~WAIT_OBJECT_0;
		// contrary to what one might expect, we should NOT use size()==0 as a stopping criterium.
		// The queue size is decreased when a worker picks a new file to process, not when it's
		// finished. Using size()==0 as a stopping criterium caused the processing interrupts
		// that were observed with large files.
		while( nJobs >= 1 && !quitRequested() && waitResult != WAIT_OBJECT_0 ){
			if( nJobs ){
			 double perc = 100.0 * nProcessed / N;
				 if( perc >= prevPerc + 10 ){
					 fprintf( stderr, "%s %d%%", (prevPerc > 0)? " .." : "", int(perc + 0.5) );
					 if( verboseLevel > 1 ){
					   double avCPUUsage = 0;
						for( i = 0 ; i < nJobs ; ++i ){
							if( threadPool[i]->nProcessed && threadPool[i]->avCPUUsage > 0){
								avCPUUsage += threadPool[i]->avCPUUsage / threadPool[i]->nProcessed;
							}
						}
						if (avCPUUsage >= 0) {
							// we report the combined, not the average CPU time, so N threads
							// having run at 100% CPU will print as N00% CPU.
							fprintf( stderr, " [%0.2lf%%]", avCPUUsage );
						}
					 }
					 fflush(stderr);
					 prevPerc = perc;
				 }
			}
			waitResult = WaitForSingleObject( allDoneEvent, 2000 );
		}
		if( (quitRequested() && !threadPool.empty()) || nProcessing > 0 ){
			// the WaitForSingleObject() call above was interrupted by the signal that
			// led to quitRequested() being set and as a result the workers haven't yet
			// had the chance to exit cleanly. Give them that chance now.
			fprintf( stderr, " quitting [%ld]...", nProcessing ); fflush(stderr);
			waitResult = WaitForSingleObject( allDoneEvent, nProcessing * 500 );
			for( i = 0 ; i < nProcessing && waitResult == WAIT_TIMEOUT ; ++i ){
				fprintf( stderr, " [%ld]...", nProcessing) ; fflush(stderr);
				waitResult = WaitForSingleObject( allDoneEvent, nProcessing * 500 );
			}
		}
		fputc( '\n', stderr );
		CloseHandle(allDoneEvent);
		allDoneEvent = NULL;
	}
	i = 0;
	double totalUTime = 0, totalSTime = 0;
	// forced verbose mode: prints out statistics even if some aren't meaningful when
	// verboseLevel==0 (like compression ratios in zfsctool).
	int verbose = verboseLevel;
	if (getenv("VERBOSE")) {
		verbose = atoi(getenv("VERBOSE"));
	}
	while( !threadPool.empty() ){
	 FileProcessor *thread = threadPool.front();
		if( thread->GetExitCode() == (THREAD_RETURN)STILL_ACTIVE ){
			fprintf( stderr, "Stopping worker thread #%d that is still %s!\n", i, (thread->scope)? "processing" : "active" );
			std::string currentFileName = thread->currentFileName();
			if( currentFileName.c_str()[0] ){
				fprintf( stderr, "\tcurrent file: %s\n", currentFileName.c_str() );
			}
			thread->Stop(true);
		}
		if( thread->nProcessed ){
			if( verbose ){
				fprintf( stderr, "Worker thread #%d processed %ld files",
					i, thread->nProcessed );
				fprintf( stderr, ", %0.2lf Kb [%0.2lf Kb] -> %0.2lf Kb [%0.2lf Kb] (%0.2lf%%)",
					thread->runningTotalRaw/1024.0, thread->runningTotalRaw/1024.0/thread->nProcessed,
					thread->runningTotalCompressed/1024.0, thread->runningTotalCompressed/1024.0/thread->nProcessed,
					100.0 * double(thread->runningTotalRaw - thread->runningTotalCompressed) / double(thread->runningTotalRaw) );
				if( thread->isBackwards ){
					fprintf( stderr, " [reverse]");
				}
				if( verbose > 1 ){
					if( thread->hasInfo ){
						fprintf( stderr, "\n\t%gs user + %gs system",
							thread->userTime, thread->systemTime );
						totalUTime += thread->userTime, totalSTime += thread->systemTime;
#ifdef __MACH__
						fprintf( stderr, "; %ds slept", thread->threadInfo.sleep_time );
#elif defined(CLOCK_THREAD_CPUTIME_ID)
						fprintf(stderr, " ; %gs CPU", thread->cpuTime);
#endif
						const auto acu = thread->avCPUUsage / thread->nProcessed;
						if (thread->avCPUUsage >= 0) {
							fprintf(stderr, "; %0.2lf%%", acu);
						}
						if (thread->m_ThreadCtx.m_runTime > 0) {
							const auto rcu = (thread->userTime + thread->systemTime) * 100.0 / thread->m_ThreadCtx.m_runTime;
							if (std::fabs(rcu - acu) > 5) {
								fprintf(stderr, "; %0.2lf%% real", rcu);
							}
						}
					}
				}
				fputc( '\n', stderr );
			}
		}
		delete thread;
		threadPool.pop_front();
		i++;
	}
	const double endTime = HRTime_Time();
	if( verbose > 1 && (totalUTime || totalSTime)){
		const double totalCPUUsage = (totalUTime + totalSTime) * 100.0 / (endTime - startTime);
		fprintf(stderr, "Total %gs user + %gs system; %gs total; %0.2lf%% CPU\n",
				totalUTime, totalSTime, endTime - startTime, totalCPUUsage);
	}
	return nProcessed;
}

int ParallelFileProcessor::workerDone(FileProcessor */*worker*/)
{ CRITSECTLOCK::Scope scope(threadLock);
// 	char name[17];
// 	pthread_getname_np( (pthread_t) GetThreadId(worker->GetThread()), name, sizeof(name) );
// 	fprintf( stderr, "workerDone(): worker \"%s\" is done; %ld workers left\n", name, nJobs - 1 );
	nJobs -= 1;
	if( nJobs <= 0 ){
		if( allDoneEvent ){
			SetEvent(allDoneEvent);
		}
	}
	return nJobs;
}

iZFSDataSetCompressionInfo *ParallelFileProcessor::z_dataSetForFile(const std::string &fileName)
{
	return z_dataSetInfoForFile.count(fileName) ? z_dataSetInfoForFile[fileName] : nullptr;
}

iZFSDataSetCompressionInfo *ParallelFileProcessor::z_dataSetForFile(const char *fileName)
{
	const std::string n = fileName;
	return z_dataSetForFile(n);
}

iZFSDataSetCompressionInfo *ParallelFileProcessor::z_dataSet(const std::string &name)
{
	return z_dataSetInfo.count(name) ? z_dataSetInfo[name] : nullptr;
}

iZFSDataSetCompressionInfo *ParallelFileProcessor::z_dataSet(const char *name)
{
	const std::string n = name;
	return z_dataSet(n);
}

void ParallelFileProcessor::z_addDataSet(const std::string &fileName, iZFSDataSetCompressionInfo *info)
{
	// iZFSDataSetCompressionInfo inherits std::string so we can do this:
	auto old = z_dataSet(*info);
	if (old && old != info) {
		delete old;
	}
	z_dataSetInfo[*info] = info;
	z_dataSetInfoForFile[fileName] = info;
}

// ================================= FileProcessor methods =================================

DWORD FileProcessor::Run(LPVOID /*arg*/)
{
	if( PP ){
	 FileEntry entry;
		nProcessed = 0;
		while( !PP->quitRequested() && (isBackwards ? PP->getBack(entry) : PP->getFront(entry)) ){
		 // create a scoped lock without closing it immediately
		 CRITSECTLOCK::Scope scp(PP->ioLock, 0);
			scope = &scp;
			currentEntry = &entry;
			_InterlockedIncrement(&PP->nProcessing);
			entry.compress( this, PP );
			_InterlockedDecrement(&PP->nProcessing);
			_InterlockedIncrement(&PP->nProcessed);
			currentEntry = NULL;
			nProcessed += 1;
			scope = NULL;

			runningTotalRaw += entry.fileInfo.st_size;
			runningTotalCompressed += (entry.compressedSize > 0)? entry.compressedSize : entry.fileInfo.st_size;
			/*if( PP->verbose() > 1 )*/{
#if defined(__MACH__)
                mach_port_t thread = mach_thread_self();
				mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
				thread_basic_info_data_t info;
				int kr = thread_info(thread, THREAD_BASIC_INFO, (thread_info_t) &info, &count);
				if( kr == KERN_SUCCESS ){
					 userTime = info.user_time.seconds + info.user_time.microseconds * 1e-6;
					 systemTime = info.system_time.seconds + info.system_time.microseconds * 1e-6;
					 avCPUUsage += info.cpu_usage * 100.0 / TH_USAGE_SCALE;
					 threadInfo = info;
					 hasInfo = true;
				}
				mach_port_deallocate(mach_task_self(), thread);
#elif defined(linux)
				struct rusage rtu;
				if (!getrusage(RUSAGE_THREAD, &rtu)) {
					const auto ut = rtu.ru_utime.tv_sec + rtu.ru_utime.tv_usec * 1e-6;
					const auto st = rtu.ru_stime.tv_sec + rtu.ru_stime.tv_usec * 1e-6;
					if (ut >= 0 && st >= 0) {
						userTime = ut, systemTime = st, hasInfo = true;
					}
				}
#	ifdef CLOCK_THREAD_CPUTIME_ID
				struct timespec ts;
				if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != -1) {
					cpuTime = ts.tv_sec + ts.tv_nsec * 1e-9;
					double t = userTime + systemTime;
					if (cpuTime > 0) {
						avCPUUsage += t * 100.0 / cpuTime;
					}
				}
#	endif
#endif
			}
		}
	}
	return DWORD(nProcessed);
}

void FileProcessor::InitThread()
{
	char name[32];
	snprintf( name, sizeof(name), "FilePr #%d", procID );
#ifdef __MACH__
	pthread_setname_np(name);
	memset( &threadInfo, 0, sizeof(threadInfo) );
#else
	pthread_t thread = (pthread_t) GetThreadId(GetThread());
	pthread_setname_np(thread, name);
#endif
	hasInfo = false;
}

bool FileProcessor::lockScope()
{
	if( PP ){
		if( scope ){
			bool wasLocked = scope->IsLocked();
			PP->ioLockedFlag = scope->Lock();
			if( wasLocked && scope->Parent() ){
				scope->Parent()->lockCounter += 1;
			}
		}
		return PP->ioLockedFlag;
	}
	return false;
}

bool FileProcessor::unLockScope()
{
	if( PP ){
		if( scope ){
			scope->Unlock();
			PP->ioLockedFlag = *scope;
		}
		return PP->ioLockedFlag;
	}
	return false;
}

// ================================= C interface functions =================================

void releaseParallelProcessor(ParallelFileProcessor *p)
{
	delete p;
}

bool addFileToParallelProcessor(ParallelFileProcessor *p, const char *inFile, const struct stat *inFileInfo,
								struct folder_info *folderInfo, const bool ownInfo)
{
	if( p && inFile && inFileInfo && folderInfo ){
		if( ownInfo ){
			p->items().push_back(FileEntry( inFile, inFileInfo, new FolderInfo(*folderInfo), ownInfo ));
		}
		else{
			p->items().push_back(FileEntry( inFile, inFileInfo, folderInfo, ownInfo ));
		}
		return true;
	}
	else{
//		   fprintf( stderr, "Error: Processor=%p file=%p, finfo=%p dinfo=%p, own=%d\n", p, inFile, inFileInfo, folderInfo, ownInfo );
		return false;
	}
}

static int sizeLess(const FileEntry &a, const FileEntry &b)
{
	return a.fileInfo.st_size < b.fileInfo.st_size;
}

bool sortFilesInParallelProcessorBySize(ParallelFileProcessor *p)
{
	if( p && p->itemCount() > 0 ){
		fprintf(stderr, "Sorting %lu entries ...", p->itemCount()); fflush(stderr);
		std::sort(p->items().begin(), p->items().end(), sizeLess);
		fprintf( stderr, " done\n" );
		return true;
	}
	else{
		return false;
	}
}

size_t filesInParallelProcessor(ParallelFileProcessor *p)
{
	if( p ){
		return p->itemCount();
	}
	else{
		return 0;
	}
}

// attempt to lock the ioLock; returns a success value
bool lockParallelProcessorIO(FileProcessor *worker)
{ bool locked = false;
	if( worker ){
		locked = worker->lockScope();
	}
	return locked;
}

// unlock the ioLock
bool unLockParallelProcessorIO(FileProcessor *worker)
{ bool locked = false;
	if( worker ){
		locked = worker->unLockScope();
	}
	return locked;
}

// return the current worker ID (thread number)
int currentParallelProcessorID(FileProcessor *worker)
{ int procID = -1;
	if( worker ){
		procID = worker->processorID();
	}
	return procID;
}

bool changeParallelProcessorJobs(ParallelFileProcessor *p, const int n, const int r)
{
	if( p ){
		return p->setJobs(n, r);
	}
	return false;
}

int runParallelProcessor(ParallelFileProcessor *p)
{ int ret = -1;
	if( p ){
		ret = p->run();
	}
	return ret;
}

void stopParallelProcessor(ParallelFileProcessor *p)
{
	if( p ){
		p->setQuitRequested(true);
	}
}

struct folder_info *getParallelProcessorJobInfo(ParallelFileProcessor *p)
{
	return (p)? &p->jobInfo : NULL;
}
