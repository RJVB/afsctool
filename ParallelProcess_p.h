// kate: auto-insert-doxygen true; backspace-indents true; indent-width 4; keep-extra-spaces true; replace-tabs true; tab-indents true; tab-width 4;

/*
 * @file ParallelProcess_p.h
 * Copyright 2015 René J.V. Bertin
 * This code is made available under the CPOL License
 * http://www.codeproject.com/info/cpol10.aspx
 */

#ifndef _PARALLELPROCESS_P_H

#include "afsctool.h"

#include <queue>
#include <deque>
#include <string>

#undef MUTEXEX_CAN_TIMEOUT
#include "Thread/Thread.h"

#define CRITSECTLOCK    MutexEx

template <typename T>
class ParallelProcessor
{
public:
    typedef T ItemType;
    typedef std::queue<ItemType> ItemQueue;
    typedef typename ItemQueue::size_type size_type;

    ParallelProcessor()
    {
        listLock = new CRITSECTLOCK(4000);
        threadLock = new CRITSECTLOCK(4000);
    }
    virtual ~ParallelProcessor()
    {
        if( !itemList.empty() ){
          CRITSECTLOCK::Scope scope(listLock, 2500);
            fprintf( stderr, "~ParallelProcessor(%p): clearing itemList[%lu]\n", this, itemList.size() );
            while( !itemList.empty() ){
                itemList.pop();
            }
        }
        delete listLock;
        delete threadLock;
    }

    ItemQueue &items()
    {
        return itemList;
    }

    // return the number of elements in the itemList in a thread-safe fashion
    // but with a timed wait if the underlying implementation allows it.
    size_type size()
    { CRITSECTLOCK::Scope scope(listLock, 2500);
        return itemList.size();
    }

    bool getFront(T &value)
    { CRITSECTLOCK::Scope scope(listLock);
      bool ret = false;
        if( !itemList.empty() ){
            value = itemList.front();
            itemList.pop();
            ret = true;
        }
        return ret;
    }
protected:
    ItemQueue itemList;
    CRITSECTLOCK *listLock, *threadLock;
};

typedef struct folder_info FolderInfo;
class FileProcessor;

typedef struct FileEntry {
public:
    std::string fileName;
    struct stat fileInfo;
    FolderInfo *folderInfo;
    bool freeFolderInfo;
    FileEntry();
    FileEntry( const char *name, const struct stat *finfo, FolderInfo *dinfo, const bool ownInfo=false );
    FileEntry( const char *name, const struct stat *finfo, FolderInfo &dinfo );
    FileEntry(const FileEntry &ref);
    ~FileEntry();
    FileEntry &operator = (const FileEntry &ref);
    void compress(FileProcessor *worker)
    {
        compressFile( fileName.c_str(), &fileInfo, folderInfo, worker );
    }
} FileEntry;

class ParallelFileProcessor : public ParallelProcessor<FileEntry>
{
    typedef std::deque<FileProcessor*> PoolType;

public:
    ParallelFileProcessor(const int n=1);
    virtual ~ParallelFileProcessor()
    {
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
protected:
    int workerDone(FileProcessor *worker);
    // the number of configured or active worker threads
    volatile long nJobs;
    // the number of processed items
    volatile long nProcessed;
    // a pool containing pointers to the worker threads
    PoolType threadPool;
    // the event that signals that all work has been done
    HANDLE allDoneEvent;
    CRITSECTLOCK *ioLock;
    bool ioLockedFlag;
    DWORD ioLockingThread;
friend class FileProcessor;
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
    {}
    bool lockScope();
    bool unLockScope();

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
    const int procID;
    CRITSECTLOCK::Scope *scope;
    friend class ParallelFileProcessor;
};


#define _PARALLELPROCESS_P_H
#endif //_PARALLELPROCESS_P_H
