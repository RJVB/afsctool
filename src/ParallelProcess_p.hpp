// kate: auto-insert-doxygen true; backspace-indents true; indent-width 4; keep-extra-spaces true; replace-tabs false; tab-indents true; tab-width 4;

/*
 * @file ParallelProcess_p.hpp
 * Copyright 2015 René J.V. Bertin
 *  This code is made available under No License At All
 */

#ifndef _PARALLELPROCESS_P_H

#include "fsctool.h"

#include <deque>
#include <string>

#include <sparsehash/dense_hash_map>

#undef MUTEXEX_CAN_TIMEOUT
#include "Thread/Thread.hpp"

#define CRITSECTLOCK	MutexEx

#ifdef __MACH__
#include <mach/thread_info.h>
#endif

template <typename T>
class ParallelProcessor
{
public:
	typedef T ItemType;
	typedef std::deque<ItemType> ItemQueue;
	typedef typename ItemQueue::size_type size_type;

	ParallelProcessor()
	{
		listLock = new CRITSECTLOCK(4000);
		threadLock = new CRITSECTLOCK(4000);
		quitRequestedFlag = false;
	}
	virtual ~ParallelProcessor()
	{
		if( !itemList.empty() ){
		 CRITSECTLOCK::Scope scope(listLock, 2500);
			fprintf( stderr, "~ParallelProcessor(%p): clearing itemList[%lu]\n", this, itemList.size() );
			while( !itemList.empty() ){
				itemList.pop_front();
			}
		}
		delete listLock;
		delete threadLock;
	}

	ItemQueue &items()
	{
		return itemList;
	}

	size_t itemCount()
	{
		return itemList.size();
	}

	// return the number of elements in the itemList in a thread-safe fashion
	// but with a timed wait if the underlying implementation allows it.
	size_type size()
	{ bool wasLocked = listLock->IsLocked();
		CRITSECTLOCK::Scope scope(listLock, 2500);
		if( wasLocked ){
			listLock->lockCounter += 1;
		}
		return itemList.size();
	}

	bool getFront(T &value)
	{ bool ret = false;
	  bool wasLocked = listLock->IsLocked();
		CRITSECTLOCK::Scope scope(listLock);
		if( wasLocked ){
			listLock->lockCounter += 1;
		}
		if( !itemList.empty() ){
			value = itemList.front();
			itemList.pop_front();
			ret = true;
		}
		return ret;
	}

	bool getBack(T &value)
	{ bool ret = false;
	  bool wasLocked = listLock->IsLocked();
		CRITSECTLOCK::Scope scope(listLock);
		if( wasLocked ){
			listLock->lockCounter += 1;
		}
		if( !itemList.empty() ){
			value = itemList.back();
			itemList.pop_back();
			ret = true;
		}
		return ret;
	}

	bool quitRequested()
	{
		return quitRequestedFlag;
	}

	bool setQuitRequested(bool val)
	{ bool ret = quitRequestedFlag;
		quitRequestedFlag = val;
		return ret;
	}

	inline unsigned long listLockConflicts() const
	{
		return listLock->lockCounter;
	}

protected:
	ItemQueue itemList;
	CRITSECTLOCK *listLock;
	CRITSECTLOCK *threadLock;
	bool quitRequestedFlag;
};

typedef struct folder_info FolderInfo;
class FileProcessor;
class ParallelFileProcessor;

typedef struct FileEntry {
public:
	std::string fileName;
	struct stat fileInfo;
	FolderInfo *folderInfo;
	bool freeFolderInfo;
	long long compressedSize;
	FileEntry();
	FileEntry( const char *name, const struct stat *finfo, FolderInfo *dinfo, const bool ownInfo=false );
	FileEntry( const char *name, const struct stat *finfo, FolderInfo &dinfo );
	FileEntry(const FileEntry &ref);
	~FileEntry();
	FileEntry &operator = (const FileEntry &ref);
	void compress(FileProcessor *worker, ParallelFileProcessor *PP);
} FileEntry;

// something for zfsctool to store information about ZFS datasets,
// where the base class is a std::string holding the dataset name.
// class to be implemented in zfsctool so as not to burden afsctool with it
class iZFSDataSetCompressionInfo : public std::string
{
public:
	iZFSDataSetCompressionInfo(const char *name, const char*)
		: std::string(name)
	{}
	virtual ~iZFSDataSetCompressionInfo()
	{}
};

typedef google::dense_hash_map<std::string,iZFSDataSetCompressionInfo*> iZFSDataSetCompressionInfoForName;

class ParallelFileProcessor : public ParallelProcessor<FileEntry>
{
	typedef std::deque<FileProcessor*> PoolType;

public:
	ParallelFileProcessor(int n=1, int r=0, int verboseLevel=0);
	virtual ~ParallelFileProcessor();

	// attempt to lock the ioLock; returns a success value
	bool lockIO();
	// unlock the ioLock
	bool unLockIO();

	// change the number of jobs. Can only be done before calling run()
	bool setJobs(int n, int r);

	// spawn the requested number of worker threads and let them
	// empty the queue. After spawning the workers, run() waits
	// on allDoneEvent before exiting.
	int run();

	inline int verbose() const
	{
		return verboseLevel;
	}

	// lookup the ZFS dataset info for file <name>.
	iZFSDataSetCompressionInfo *z_dataSetForFile(const std::string &fileName);
	// lookup the ZFS dataset info for file <name>
	iZFSDataSetCompressionInfo *z_dataSetForFile(const char *fileName);
	// lookup the ZFS dataset info for dataset <name>.
	iZFSDataSetCompressionInfo *z_dataSet(const std::string &name);
	// lookup the ZFS dataset info for dataset <name>
	iZFSDataSetCompressionInfo *z_dataSet(const char *name);
	// register a ZFS dataset info instance for file <name>
	// any old registration is deleted first; ownership to <info> is
	// transferred to the dataset registry.
	void z_addDataSet(const std::string &fileName, iZFSDataSetCompressionInfo *info);

	FolderInfo jobInfo;
protected:
	int workerDone(FileProcessor *worker);
	// the number of configured or active worker threads
	volatile long nJobs;
	// the number of jobs attacking the item list from the rear
	volatile long nReverse;
	// the number of processing threads
	volatile long nProcessing;
	// the number of processed items
	volatile long nProcessed;
	// a pool containing pointers to the worker threads
	PoolType threadPool;
	// the event that signals that all work has been done
	HANDLE allDoneEvent;
	CRITSECTLOCK *ioLock;
	bool ioLockedFlag;
	DWORD ioLockingThread;
	int verboseLevel;

	// a dataset name -> info map
	iZFSDataSetCompressionInfoForName z_dataSetInfo;
	// a filename -> dataset info map
	iZFSDataSetCompressionInfoForName z_dataSetInfoForFile;
friend class FileProcessor;
friend struct FileEntry;
};

class FileProcessor : public Thread
{
public:
	FileProcessor(ParallelFileProcessor *PP, bool isReverse, int procID)
		: Thread()
		, PP(PP)
		, nProcessed(-1)
		, runningTotalRaw(0)
		, runningTotalCompressed(0)
		, cpuUsage(0.0)
		, cleanedUp(false)
		, isBackwards(isReverse)
		, procID(procID)
		, scope(NULL)
		, currentEntry(NULL)
	{}
	~FileProcessor()
    {
		// better be safe than sorry
		CleanupThread();
		PP = NULL;
		scope = NULL;
		currentEntry = NULL;
    }
	bool lockScope();
	bool unLockScope();

	inline const int processorID() const
	{
		return procID;
	}

	inline std::string currentFileName() const
	{
		return (currentEntry)? currentEntry->fileName : "";
	}

	inline ParallelFileProcessor* controller()
	{
		return PP;
	}
protected:
	DWORD Run(LPVOID arg);
	void InitThread();

	void CleanupThread()
	{
		if( PP && !cleanedUp ){
			PP->workerDone(this);
			cleanedUp = true;
		}
	}

	ParallelFileProcessor *PP;
	volatile long nProcessed;
	volatile long long runningTotalRaw, runningTotalCompressed;
	volatile double cpuUsage, userTime, systemTime;
	bool cleanedUp;
	const bool isBackwards;
	const int procID;
	CRITSECTLOCK::Scope *scope;
	bool hasInfo;
#ifdef __MACH__
	thread_basic_info_data_t threadInfo;
#endif
	friend class ParallelFileProcessor;
private:
	FileEntry *currentEntry;
};


#define _PARALLELPROCESS_P_H
#endif //_PARALLELPROCESS_P_H
