// kate: auto-insert-doxygen true; backspace-indents true; indent-width 4; keep-extra-spaces true; replace-tabs false; tab-indents true; tab-width 4;

/*
 * @file ParallelProcess_p.h
 * Copyright 2015 René J.V. Bertin
 *  This code is made available under No License At All
 */

#ifndef _PARALLELPROCESS_P_H

#include "afsctool.h"

#include <deque>
#include <string>

#undef MUTEXEX_CAN_TIMEOUT
#include "Thread/Thread.h"

#define CRITSECTLOCK	MutexEx

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

class ParallelFileProcessor : public ParallelProcessor<FileEntry>
{
	typedef std::deque<FileProcessor*> PoolType;

public:
	ParallelFileProcessor(const int n=1, const int verboseLevel=0);
	virtual ~ParallelFileProcessor()
	{
		if( verboseLevel > 1 && (listLockConflicts() || ioLock->lockCounter) ){
			fprintf( stderr, "Queue lock contention: %lux ; IO lock contention %lux\n",
					 listLockConflicts(), ioLock->lockCounter );
		}
		delete ioLock;
		if( allDoneEvent ){
			CloseHandle(allDoneEvent);
		}
	}
	// attempt to lock the ioLock; returns a success value
	bool lockIO();
	// unlock the ioLock
	bool unLockIO();

	// spawn the requested number of worker threads and let them
	// empty the queue. After spawning the workers, run() waits
	// on allDoneEvent before exiting.
	int run();

	FolderInfo jobInfo;

	inline int verbose() const
	{
		return verboseLevel;
	}
protected:
	int workerDone(FileProcessor *worker);
	// the number of configured or active worker threads
	volatile long nJobs;
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
friend class FileProcessor;
friend class FileEntry;
};

class FileProcessor : public Thread
{
public:
	FileProcessor(ParallelFileProcessor *PP, const int procID)
		: PP(PP)
		, nProcessed(-1)
		, Thread()
		, procID(procID)
		, scope(NULL)
		, runningTotalCompressed(0)
		, runningTotalRaw(0)
		, cpuUsage(0.0)
		, currentEntry(NULL)
	{}
	~FileProcessor()
    {
		// better be safe than sorry
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
protected:
	DWORD Run(LPVOID arg);
	void InitThread();

	void CleanupThread()
	{
		if( PP ){
			PP->workerDone(this);
		}
	}

	ParallelFileProcessor *PP;
	volatile long nProcessed;
	volatile long long runningTotalRaw, runningTotalCompressed;
	volatile double cpuUsage;
	const int procID;
	CRITSECTLOCK::Scope *scope;
	friend class ParallelFileProcessor;
private:
	FileEntry *currentEntry;
};


#define _PARALLELPROCESS_P_H
#endif //_PARALLELPROCESS_P_H
