// kate: auto-insert-doxygen true; backspace-indents true; indent-width 5; keep-extra-spaces true; replace-tabs false; tab-indents true; tab-width 5;
/**
 *  @file msemul.cpp
 *  emulation of certain concurrent-programming MS Windows functions for gcc.
 *  Developed/tested on Mac OS X and Debian 6
 *
 *  Created by René J.V. Bertin on 20111204.
 *  Copyright 2011 IFSTTAR / LEPSIS / R.J.V. Bertin. All rights reserved.
 *  Copyright 2012-20xx R.J.V. Bertin. 
 *
 *  This code is made available under No License At All
 */

#if !defined(_MSC_VER) && !defined(__MINGW32__) && !defined(__MINGW64__) && !defined(WIN32) && !defined(_WIN64)

#if defined(linux) || defined(__CYGWIN__)
#	include <fcntl.h>
#	include <sys/time.h>
#endif
#include <sstream>

#include <mutex>

#include "msemul.h"

#if defined(__APPLE__) || defined(__MACH__)
#	include <mach/thread_act.h>
#	include <dlfcn.h>
#	include <AvailabilityMacros.h>
#endif

#include "CritSectEx.h"
#define CRITSECTLOCK	MutexEx

static void cseUnsleep( int sig )
{
//	fprintf( stderr, "SIGALRM\n" );
}

#include <sparsehash/dense_hash_map>
#include <sys/mman.h>

int mseShFD = -1;
#define MSESHAREDMEMNAME	"/dev/zero"; //"MSEShMem-XXXXXX"
char MSESharedMemName[64] = "";
static size_t mmapCount = 0;
static BOOL theMSEShMemListReady = false;
static CRITSECTLOCK *shMemCSE = NULL;
typedef google::dense_hash_map<void*,size_t> MSEShMemLists;
static MSEShMemLists *theMSEShMemList = NULL;

typedef google::dense_hash_map<HANDLE,MSHANDLETYPE> OpenHANDLELists;
static OpenHANDLELists *theOpenHandleList = NULL;
static google::dense_hash_map<int,const char*> *HANDLETypeName = NULL;
static BOOL theOpenHandleListReady = false;
static CRITSECTLOCK *openHandleListCSE = NULL;

static void WarnLockedHandleList()
{
	fprintf( stderr, "Failure preempting access to the HANDLE registry."
		" This probably means it is being accessed from another thread; proceeding with fingers crossed\n"
		" Set a breakpoint in WarnLockedHandleList() to trace the origin of this event.\n" );
}

static void WarnLockedShMemList()
{
	fprintf( stderr, "Failure preempting access to the shared memory registry."
		" This probably means it is being accessed from another thread; proceeding with fingers crossed\n"
		" Set a breakpoint in WarnLockedShMemList() to trace the origin of this event.\n" );
}

static void WarnLockedSemaList()
{
	fprintf( stderr, "Failure preempting access to the semaphore registry."
		" This probably means it is being accessed from another thread; proceeding with fingers crossed\n"
		" Set a breakpoint in WarnLockedSemaList() to trace the origin of this event.\n" );
}

static inline int isOpenHandle(HANDLE h)
{ int ret = 0;
	if( theOpenHandleListReady ){
	  CRITSECTLOCK::Scope scope(openHandleListCSE,5000);
		if( !scope ){
			WarnLockedHandleList();
		}
		ret = (theOpenHandleList->count(h) > 0)? 1 : -1;
	}
	return ret;
}

static CRITSECTLOCK *semaListCSE = NULL;

static pthread_key_t suspendKey = 0;
static BOOL suspendKeyCreated = false;

static pthread_key_t currentThreadKey = 0;
static pthread_once_t currentThreadKeyCreated = PTHREAD_ONCE_INIT;
static bool ForceCloseHandle(HANDLE);

static pthread_key_t timedThreadKey;
static pthread_once_t timedThreadCreated = PTHREAD_ONCE_INIT;

static pthread_key_t sharedMemKey;
static pthread_once_t sharedMemKeyCreated = PTHREAD_ONCE_INIT;

static void InitMSEShMem()
{
	if( !theMSEShMemListReady ){
	  void MSEfreeAllShared();
		theMSEShMemListReady = true;
		theMSEShMemList = new google::dense_hash_map<void*,size_t>();
		theMSEShMemList->resize(4);
		theMSEShMemList->set_empty_key(NULL);
		theMSEShMemList->set_deleted_key( (HANDLE)-1 );
		atexit(MSEfreeAllShared);
	}
}

static void createSharedMemKey()
{
	pthread_key_create(&sharedMemKey, NULL);
}

/**
	A THREAD SPECIFIC selector whether shared memory should be used. In other words,
	if set by the parent process, childs (processes) will have access to memory allocated
	by the parent, but not vice versa. If the parent also has to have access to memory allocated
	by the child, the child has to call MSEmul_UseSharedMemory(true) as well.
 */
int MSEmul_UseSharedMemory(int useShared)
{ int ret;
	pthread_once( &sharedMemKeyCreated, createSharedMemKey );
	ret = (int) ((size_t)pthread_getspecific(sharedMemKey));
	pthread_setspecific( sharedMemKey, (void*) useShared );
	return ret;
}

/**
	query the thread-specific use-shared-memory flag
 */
int MSEmul_UseSharedMemory()
{
	pthread_once( &sharedMemKeyCreated, createSharedMemKey );
	return (int) ((size_t)pthread_getspecific(sharedMemKey));
}

/**
	query the thread-specific use-shared-memory flag
 */
int MSEmul_UsesSharedMemory()
{
	return MSEmul_UseSharedMemory();
}

/**
	Free memory. If the pointer is not a known shared-memory allocation by MSEreallocShared,
	the system's default deallocator is used.
 */
void MSEfreeShared(void *ptr)
{ static short calls = 0;
	if( ptr && ptr != shMemCSE ){
	  CRITSECTLOCK::Scope scope(shMemCSE,5000);
		if( shMemCSE && !scope ){
			WarnLockedShMemList();
		}
		if( ptr == openHandleListCSE ){
			openHandleListCSE->~CRITSECTLOCK();
			openHandleListCSE = NULL;
		}
		if( theMSEShMemListReady && theMSEShMemList->count(ptr) ){
		  size_t N = (*theMSEShMemList)[ptr];
			if( munmap( ptr, N ) == 0 ){
				(*theMSEShMemList)[ptr] = 0;
				theMSEShMemList->erase(ptr);
				if( ++calls >= 32 || mmapCount == 1){
					theMSEShMemList->resize(0);
					calls = 0;
				}
				mmapCount -= 1;
			}
		} 
		else{
			free(ptr);
		}
		ptr = NULL;
	}
	if( mmapCount == 0 ){
		if( mseShFD >= 0 ){
			close(mseShFD);
			if( strcmp( MSESharedMemName, "/dev/zero" ) ){
				shm_unlink(MSESharedMemName);
			}
		}
	}
}

/**
	atexit handler that frees all remaining allocated shared memory and performs
	some associated house-keeping.
 */
void MSEfreeAllShared()
{
	if( currentThreadKey ){
		// this means that at least one invocation to GetCurrentThread was made, and
		// thus that there is a corresponding entry in theOpenHandleList that will not have
		// been removed. Do that now - and of course BEFORE we release dangling memory ...
		ForceCloseHandle( GetCurrentThread() );
	}
	if( semaListCSE ){
		semaListCSE->~CRITSECTLOCK();
		MSEfreeShared(semaListCSE);
		semaListCSE = NULL;
	}
	{ CRITSECTLOCK::Scope scope(shMemCSE,5000);
	  // we don't deallocate shMemCSE here, so if it's non-null the list will be empty when
	  // only shMemCSE remains:
	  size_t empty = (shMemCSE)? 1 : 0;
		if( shMemCSE && !scope ){
			WarnLockedShMemList();
		}
		while( theMSEShMemList->size() > empty ){
		  MSEShMemLists::iterator i = theMSEShMemList->begin();
		  std::pair<void*,size_t> elem = *i;
//			fprintf( stderr, "@@ MSEfreeShared(0x%p) of %lu remaining elements\n", elem.first, theMSEShMemList->size() );
			if( elem.first == shMemCSE ){
				++i;
				if( i != theMSEShMemList->end() ){
					elem = *i;
					// MSEfreeShared() try to get another lock on shMemCSE, which
					// is not supported by all CritSectEx.h classes, so we unlock the scope first
					scope.Unlock();
					MSEfreeShared(elem.first);
					scope.Lock(5000);
				}
			}
			else{
				// MSEfreeShared() try to get another lock on shMemCSE, which
				// is not supported by all CritSectEx.h classes, so we unlock the scope first
				scope.Unlock();
				MSEfreeShared(elem.first);
				scope.Lock(5000);
			}
		}
	}
	if( theOpenHandleListReady && theOpenHandleList->size() ){
		fprintf( stderr, "@@@ Exit with %lu HANDLEs still open\n", theOpenHandleList->size() );
	}
	if( currentThreadKey ){
		pthread_key_delete(currentThreadKey);
		currentThreadKey = 0;
	}
	if( suspendKeyCreated ){
		pthread_key_delete(suspendKey);
		suspendKeyCreated = false;
	}
	if( shMemCSE ){
		shMemCSE->~CRITSECTLOCK();
		void *ptr = (void*) shMemCSE;
		shMemCSE = NULL;
		MSEfreeShared(ptr);
	}
}

/**
	Allocate or reallocate memory of size <oldN> to size <N>. If the thread-specific flag is
	set or forceShared==true, the memory is allocated in anonymous shared memory, otherwise
	the system's default (re)allocator is used.
 */
void *MSEreallocShared( void* ptr, size_t N, size_t oldN, int forceShared )
{ void *mem;
  int flags = MAP_SHARED;
	if( !forceShared && !MSEmul_UseSharedMemory() ){
		if( ptr){
			if( (ptr = realloc(ptr,N)) && N > oldN ){
				// zero out the area beyond what was allocated previously:
				memset( &((char*)ptr)[oldN], 0, N - oldN );
			}
		}
		else{
			ptr = calloc(N,1);
		}
		return ptr;
	}
#ifndef MAP_ANON
	if( mseShFD < 0 ){
		if( !MSESharedMemName[0] ){
			strcpy( MSESharedMemName, MSESHAREDMEMNAME );
// 			mktemp(MSESharedMemName);
		}
		if( (mseShFD = open( MSESharedMemName, O_RDWR )) < 0 ){
//			fprintf( stderr, "@@ MSEreallocShared(): can't open/create descriptor for allocating %s=0x%lx, size %s=%lu -> 0x%lx (%s)\n",
//				   (name)? name : "<unknown>", ptr, size, (unsigned long) N, mem, serror()
//			);
			return NULL;
		}
	}
#else
	flags |= MAP_ANON;
#endif
	mem = mmap( NULL, N, (PROT_READ|PROT_WRITE), flags, mseShFD, 0 );
	if( mem ){
		memset( mem, 0, N );
		mmapCount += 1;
		InitMSEShMem();
		if( !shMemCSE ){
		  void *buffer;
		  static bool active = false;
			if( !active ){
				active = true;
				if( (buffer = MSEreallocShared( NULL, sizeof(CRITSECTLOCK), 0, true )) ){
					shMemCSE = new (buffer) CRITSECTLOCK(4000);
				}
				active = false;
			}
		}
		{ CRITSECTLOCK::Scope scope(shMemCSE,5000);
			if( shMemCSE && !scope ){
				WarnLockedShMemList();
			}
#if DEBUG > 1
			fprintf( stderr, "@@ MSEreallocShared(%p,%lu,%lu,%d) registering %p)\n",
				   ptr, N, oldN, forceShared, mem );
#endif
			(*theMSEShMemList)[mem] = N;
		}
		if( ptr ){
			memmove( mem, ptr, oldN );
			MSEfreeShared(ptr);
		}
	}
	return( mem );
}

/**
	invokes MSEreallocShared with forceShared=false
 */
void *MSEreallocShared( void* ptr, size_t N, size_t oldN )
{
	return MSEreallocShared( ptr, N, oldN, false );
}

/**
	duplicate a string in shared memory (if the thread-specific flag is set)
 */
static char *mmstrdup( char *str )
{ char *ret = NULL;
	if( (ret = (char*) MSEreallocShared(NULL, strlen(str)+1, 0 )) ){
		strcpy( ret, str );
	}
	return ret;
}

static int pthread_timedjoin( pthread_timed_t *tt, struct timespec *timeout, void **status );

#if !defined(__MINGW32__) && !defined(__MINGW64__)

/**
	 Emulates the Microsoft function of the same name:
	 @n
	 Wait for an event to occur on hHandle, for a maximum of dwMilliseconds
	 @n
	 semaphore: wait to lock the semaphore
	 @n
	 mutex: wait to lock the mutex
	 @n
	 event: wait for the event to be signalled
	 @n
	 thread: wait for the thread to terminate
	 Returns WAIT_OBJECT_0 on success, WAIT_TIMEOUT on a timeout and WAIT_ABANDONED or WAIT_FAILED on error.
 */
DWORD WaitForSingleObject( HANDLE hHandle, DWORD dwMilliseconds )
{
	if( !hHandle ){
		return WAIT_FAILED;
	}

	if( dwMilliseconds != (DWORD) -1 ){
	  struct timespec timeout;
#if defined(__APPLE__) || defined(__CYGWIN__)
	  struct sigaction h, oh;
	  struct itimerval rtt, ortt;
		if( hHandle->type != MSH_EVENT && hHandle->type != MSH_THREAD ){
#ifdef __CYGWIN__
			h.sa_handler = cseUnsleep;
#else
			h.__sigaction_u.__sa_handler = cseUnsleep;
#endif
			sigemptyset(&h.sa_mask);
			rtt.it_value.tv_sec= (unsigned long) (dwMilliseconds/1000);
			rtt.it_value.tv_usec= (unsigned long) ( (dwMilliseconds- rtt.it_value.tv_sec*1000)* 1000 );
			rtt.it_interval.tv_sec= 0;
			rtt.it_interval.tv_usec= 0;
			if( sigaction( SIGALRM, &h, &oh ) ){
//				fprintf( stderr, "Error calling sigaction: %s\n", strerror(errno) );
				return WAIT_FAILED;
			}
			setitimer( ITIMER_REAL, &rtt, &ortt );
		}
		else{
		  struct timeval tv;
		  time_t sec = (time_t) (dwMilliseconds/1000);
			gettimeofday(&tv, NULL);
			timeout.tv_sec = tv.tv_sec + sec;
			timeout.tv_nsec = tv.tv_usec * 1000 + ( (dwMilliseconds- sec*1000)* 1000000 );
			while( timeout.tv_nsec > 999999999 ){
				timeout.tv_sec += 1;
				timeout.tv_nsec -= 1000000000;
			}
		}
#else
		clock_gettime( CLOCK_REALTIME, &timeout );
		{ time_t sec = (time_t) (dwMilliseconds/1000);
			timeout.tv_sec += sec;
			timeout.tv_nsec += (long) ( (dwMilliseconds- sec*1000)* 1000000 );
			while( timeout.tv_nsec > 999999999 ){
				timeout.tv_sec += 1;
				timeout.tv_nsec -= 1000000000;
			}
		}
#endif
		switch( hHandle->type ){
			case MSH_SEMAPHORE:
#if defined(__APPLE__) || defined(__CYGWIN__)
				if( sem_wait(hHandle->d.s.sem) ){
//					fprintf( stderr, "sem_wait error %s\n", strerror(errno) );
					setitimer( ITIMER_REAL, &ortt, &rtt );
					return (errno == EINTR)? WAIT_TIMEOUT : WAIT_FAILED;
				}
				else{
					setitimer( ITIMER_REAL, &ortt, &rtt );
					hHandle->d.s.owner = pthread_self();
					if( hHandle->d.s.counter->curCount > 0 ){
						hHandle->d.s.counter->curCount -= 1;
#	ifdef DEBUG
					{ int cval;
						if( sem_getvalue( hHandle->d.s.sem, &cval ) != -1 ){
							if( cval != hHandle->d.s.counter->curCount ){
								fprintf( stderr, "@@ WaitForSingleObject(\"%s\"): value mismatch, %ld != %d\n",
								         hHandle->d.s.name, hHandle->d.s.counter->curCount, cval );
							}
						}
					}
#	endif
					}
					return WAIT_OBJECT_0;
				}
#else // !__APPLE__
				if( sem_timedwait(hHandle->d.s.sem, &timeout) ){
//					fprintf( stderr, "sem_timedwait error %s\n", strerror(errno) );
					return (errno == ETIMEDOUT)? WAIT_TIMEOUT : WAIT_FAILED;
				}
				else{
					hHandle->d.s.owner = pthread_self();
					if( hHandle->d.s.counter->curCount > 0 ){
						hHandle->d.s.counter->curCount -= 1;
#	if defined(linux) && defined(DEBUG)
						{ int cval;
							if( sem_getvalue( hHandle->d.s.sem, &cval ) != -1 ){
								if( cval != hHandle->d.s.counter->curCount ){
									fprintf( stderr, "WaitForSingleObject(\"%s\"): value mismatch, %ld != %d\n",
										    hHandle->d.s.name, hHandle->d.s.counter->curCount, cval );
								}
							}
						}
#	endif
					}
					return WAIT_OBJECT_0;
				}
#endif
				break;
			case MSH_MUTEX:
#if defined(__APPLE__) || defined(__CYGWIN__)
				if( pthread_mutex_lock( hHandle->d.m.mutex ) ){
//					fprintf( stderr, "pthread_mutex_lock error %s\n", strerror(errno) );
					setitimer( ITIMER_REAL, &ortt, &rtt );
					switch( errno ){
						case EINTR:
							return WAIT_TIMEOUT;
							break;
						case EDEADLK:
							return WAIT_ABANDONED;
							break;
						default:
							return WAIT_FAILED;
							break;
					}
				}
				else{
					setitimer( ITIMER_REAL, &ortt, &rtt );
					hHandle->d.m.owner = pthread_self();
					return WAIT_OBJECT_0;
				}
#else
				if( pthread_mutex_timedlock( hHandle->d.m.mutex, &timeout ) ){
//					fprintf( stderr, "pthread_mutex_timedlock error %s\n", strerror(errno) );
					switch( errno ){
						case ETIMEDOUT:
							return WAIT_TIMEOUT;
							break;
						case EDEADLK:
							return WAIT_ABANDONED;
							break;
						default:
							return WAIT_FAILED;
							break;
					}
				}
				else{
					hHandle->d.m.owner = pthread_self();
					return WAIT_OBJECT_0;
				}
#endif
				break;
			case MSH_EVENT: {
			  int err;
				if( hHandle->d.e.isSignalled ){
					if( !hHandle->d.e.isManual ){
						_InterlockedSetFalse(hHandle->d.e.isSignalled);
					}
					return WAIT_OBJECT_0;
				}
				if( pthread_mutex_lock( hHandle->d.e.mutex ) == 0 ){
					hHandle->d.e.waiter = pthread_self();
					errno = 0;
					if( (err = pthread_cond_timedwait( hHandle->d.e.cond, hHandle->d.e.mutex, &timeout )) ){
						pthread_mutex_unlock( hHandle->d.e.mutex );
						hHandle->d.e.waiter = 0;
						switch( (errno = err) ){
							case ETIMEDOUT:
								return WAIT_TIMEOUT;
								break;
							case EDEADLK:
								return WAIT_ABANDONED;
								break;
							default:
								return WAIT_FAILED;
								break;
						}
					}
					else{
						pthread_mutex_unlock( hHandle->d.e.mutex );
						hHandle->d.e.waiter = 0;
						if( !hHandle->d.e.isManual ){
							_InterlockedSetFalse(hHandle->d.e.isSignalled);
						}
						return WAIT_OBJECT_0;
					}
				}
				else{
					switch( errno ){
						case EINTR:
							return WAIT_TIMEOUT;
							break;
						case EDEADLK:
							return WAIT_ABANDONED;
							break;
						default:
							return WAIT_FAILED;
							break;
					}
				}
				break;
			}
			case MSH_THREAD: {
			  int err;
				if( (err = pthread_timedjoin( hHandle->d.t.theThread, &timeout, &(hHandle->d.t.theThread->status) )) ){
//					fprintf( stderr, "pthread_timedjoin error %s\n", strerror(errno) );
					switch( (errno = err) ){
						case ETIMEDOUT:
							return WAIT_TIMEOUT;
							break;
						case EDEADLK:
							return WAIT_ABANDONED;
							break;
						default:
							return WAIT_FAILED;
							break;
					}
				}
				else{
					hHandle->d.t.pThread = NULL;
					return WAIT_OBJECT_0;
				}
				break;
			}
		}
	}
	else switch( hHandle->type ){
		case MSH_SEMAPHORE:
			if( sem_wait(hHandle->d.s.sem) ){
//				fprintf( stderr, "sem_wait error %s\n", strerror(errno) );
				return WAIT_FAILED;
			}
			else{
				hHandle->d.s.owner = pthread_self();
				if( hHandle->d.s.counter->curCount > 0 ){
					hHandle->d.s.counter->curCount -= 1;
#if defined(linux) && defined(DEBUG)
					{ int cval;
						if( sem_getvalue( hHandle->d.s.sem, &cval ) != -1 ){
							if( cval != hHandle->d.s.counter->curCount ){
								fprintf( stderr, "@@ WaitForSingleObject(\"%s\"): value mismatch, %ld != %d\n",
								         hHandle->d.s.name, hHandle->d.s.counter->curCount, cval );
							}
						}
					}
#endif
				}
				return WAIT_OBJECT_0;
			}
			break;
		case MSH_MUTEX:
			if( pthread_mutex_lock( hHandle->d.m.mutex ) ){
//				fprintf( stderr, "pthread_mutex_lock error %s\n", strerror(errno) );
				switch( errno ){
					case ETIMEDOUT:
						return WAIT_TIMEOUT;
						break;
					case EDEADLK:
						return WAIT_ABANDONED;
						break;
					default:
						return WAIT_FAILED;
						break;
				}
			}
			else{
				hHandle->d.m.owner = pthread_self();
				return WAIT_OBJECT_0;
			}
			break;
		case MSH_EVENT:
			if( hHandle->d.e.isSignalled ){
				if( !hHandle->d.e.isManual ){
					_InterlockedSetFalse(hHandle->d.e.isSignalled);
				}
				return WAIT_OBJECT_0;
			}
			// get the mutex and then wait for the condition to be signalled:
			if( pthread_mutex_lock( hHandle->d.e.mutex ) == 0 ){
				hHandle->d.e.waiter = pthread_self();
				if( pthread_cond_wait( hHandle->d.e.cond, hHandle->d.e.mutex ) ){
					pthread_mutex_unlock( hHandle->d.e.mutex );
					hHandle->d.e.waiter = 0;
					return WAIT_FAILED;
				}
				else{
					pthread_mutex_unlock( hHandle->d.e.mutex );
					hHandle->d.e.waiter = 0;
					if( !hHandle->d.e.isManual ){
						_InterlockedSetFalse(hHandle->d.e.isSignalled);
					}
					return WAIT_OBJECT_0;
				}
			}
			else{
				return WAIT_FAILED;
			}
			break;
		case MSH_THREAD:
			if( pthread_join( hHandle->d.t.theThread->thread, &hHandle->d.t.theThread->status ) ){
				return WAIT_FAILED;
			}
			else{
				hHandle->d.t.pThread = NULL;
				return WAIT_OBJECT_0;
			}
			break;
	}
	return WAIT_FAILED;
}

#ifdef linux
#	include <dlfcn.h>
#endif

#include <vector>
typedef std::vector<HANDLE> SemaLists;
static SemaLists theSemaList;
static BOOL theSemaListReady = false;

static void FreeAllSemaHandles()
{ long i;
  HANDLE h;
  CRITSECTLOCK::Scope scope(semaListCSE,5000);
	if( !scope ){
		WarnLockedSemaList();
	}
	while( !theSemaList.empty() ){
		i = 0;
		do{
			if( (h = theSemaList.at(i)) ){
				if( h->d.s.counter->refHANDLEp == &h->d.s.refHANDLEs && h->d.s.refHANDLEs > 0 ){
					// a source HANDLE that is still referenced: skip for now
//					fprintf( stderr, "Skipping referred-to semaphore %d\n", i );
					i += 1;
				}
				else{
//					fprintf( stderr, "Closing semaphore %d\n", i );
					CloseHandle(h);
					i = -1;
				}
			}
			else{
//				fprintf( stderr, "Removing stale semaphore %d\n", i );
				theSemaList.erase( theSemaList.begin() + i );
			}
		} while( i >= 0 && i < theSemaList.size() );
	}
}

static void AddSemaListEntry(HANDLE h)
{
	if( !theSemaListReady ){
	  void *buffer;
		theSemaListReady = true;
		atexit(FreeAllSemaHandles);
		if( (buffer = MSEreallocShared( NULL, sizeof(CRITSECTLOCK), 0, true )) ){
			semaListCSE = new (buffer) CRITSECTLOCK(4000);
		}
	}
	if( h->type == MSH_SEMAPHORE ){
	  CRITSECTLOCK::Scope scope(semaListCSE,5000);
		if( !scope ){
			WarnLockedSemaList();
		}
		theSemaList.push_back(h);
	}
}

static void RemoveSemaListEntry(sem_t *sem)
{ unsigned int i, N = theSemaList.size();
	for( i = 0 ; i < N ; i++ ){
	  CRITSECTLOCK::Scope scope(semaListCSE,5000);
		if( !scope ){
			WarnLockedSemaList();
		}
		if( isOpenHandle(theSemaList.at(i)) < 0 ){
			fputs( "@@ internal inconsistency: theSemaList refers to an unregistered HANDLE\n", stderr );
			theSemaList.erase( theSemaList.begin() + i );
			return;
		}
		if( theSemaList.at(i)->d.s.sem == sem ){
			theSemaList.erase( theSemaList.begin() + i );
			return;
		}
	}
}

static HANDLE FindSemaphoreHANDLE(sem_t *sem, char *name)
{ unsigned int i, N = theSemaList.size();
  HANDLE ret = NULL;
	if( sem != SEM_FAILED || name ){
	  CRITSECTLOCK::Scope scope(semaListCSE,5000);
		if( !scope ){
			WarnLockedSemaList();
		}
		for( i = 0 ; i < N && !ret; i++ ){
			if( sem != SEM_FAILED ){
				if( theSemaList.at(i)->d.s.sem == sem ){
					ret = theSemaList.at(i);
				}
			}
			else if( name ){
				ret = theSemaList.at(i);
				if( strcmp( ret->d.s.name, name ) || ret->d.s.counter->refHANDLEp != &ret->d.s.refHANDLEs ){
					// wrong name or not the source semaphore
					ret = NULL;
				}
			}
		}
	}
	return ret;
}

MSHANDLE::MSHANDLE( sem_t *sema, MSHSEMAPHORECOUNTER *counter, char *lpName )
{
	d.s.name = lpName;
	d.s.sem = sema;
	d.s.owner = 0;
	d.s.counter = counter;
	*(counter->refHANDLEp) += 1;
	type = MSH_SEMAPHORE;
	Register();
}

/**
 Opens the named semaphore that must already exist. The ign_ arguments are ignored in this emulation
 of the MS function of the same name.
 */
HANDLE OpenSemaphore( DWORD ign_dwDesiredAccess, BOOL ign_bInheritHandle, char *lpName )
{ HANDLE org, ret = NULL;
	if( lpName ){
		sem_t *sema;
		errno = 0;
          sema = sem_open( lpName, 0 );
//        cseAssertEx( ((sema = sem_open( lpName, 0 )) != SEM_FAILED),
//             __FILE__, __LINE__, "sem_open() failed to open existing semaphore", lpName );
		if( sema != SEM_FAILED ){
			// find a matching entry, first by name then by semaphore
			// (for platforms where the original descriptor is returned by sem_open)
			if( ((org = FindSemaphoreHANDLE(sema, NULL))
				|| (org = FindSemaphoreHANDLE(SEM_FAILED, lpName)) )
				&& strcmp( org->d.s.name, lpName ) == 0
			){
				ret = new MSHANDLE( sema, org->d.s.counter, mmstrdup(lpName) );
			}
		}
	}
	else{
		fprintf( stderr, "OpenSemaphore(%lu,%d,NULL): call is meaningless without a semaphore name\n",
		         ign_dwDesiredAccess, ign_bInheritHandle );
		return NULL;
	}
	return ret;
}


MSHANDLE::MSHANDLE( void* ign_lpSemaphoreAttributes, long lInitialCount, long lMaximumCount, char *lpName )
{ type = MSH_EMPTY;
	if( lpName ){
		if( lInitialCount >= 0 && lMaximumCount > 0 ){
			errno = 0;
			cseAssertEx( ((d.s.sem = sem_open( lpName, O_CREAT, S_IRWXU, lInitialCount)) != SEM_FAILED),
				__FILE__, __LINE__, "sem_open() failed to create new semaphore", lpName );
			d.s.name = mmstrdup(lpName);
			d.s.owner = 0;
			d.s.refHANDLEs = 0;
			d.s.counter = new MSHSEMAPHORECOUNTER(lInitialCount, lMaximumCount, &d.s.refHANDLEs);
			type = MSH_SEMAPHORE;
			Register();
		}
	}
}

/**
 Creates the named semaphore with the given initial count (value). The ign_ arguments are ignored in this emulation
 of the MS function of the same name.
 */
HANDLE CreateSemaphore( void* ign_lpSemaphoreAttributes, long lInitialCount, long ign_lMaximumCount, char *lpName )
{ HANDLE ret = NULL;
  bool freeName = false;
	if( !lpName ){
		if( (lpName = mmstrdup( (char*) "/CSEsemXXXXXX" )) ){
			freeName = true;
#ifdef linux
			{
				char *(*fun)(char *) = (char* (*)(char*))dlsym(RTLD_DEFAULT, (char*) "mktemp");
				lpName = (**fun)(lpName);
			}
//			{ int fd = mkstemp(lpName);
//				if( fd >= 0 ){
//					close(fd);
//					unlink(lpName);
//				}
//			}
#else
			lpName = mktemp(lpName);
#endif
		}
	}
	try{
		ret = OpenSemaphore( 0, false, lpName );
	}
	catch( cseAssertFailure &e ){
		switch( e.code() ){
			case EMFILE:
			case ENAMETOOLONG:
			case ENFILE:
				fprintf( stderr,
					"CreateSemaphore(%p,%ld,%ld,%s) fatal exception (%s) - re-throwing\n",
					ign_lpSemaphoreAttributes, lInitialCount, ign_lMaximumCount, lpName, e.what() );
				throw;
				break;
			default:
				if( e.code() != ENOENT ){
					fprintf( stderr,
						"CreateSemaphore(%p,%ld,%ld,%s) failed but not because the semaphore is inexistant (%s) - proceeding with fingers crossed\n",
						ign_lpSemaphoreAttributes, lInitialCount, ign_lMaximumCount, lpName, strerror(errno) );
				}
				ret = NULL;
				break;
		}
	}
	if( !ret ){
		if( !(ret = (HANDLE) new MSHANDLE( ign_lpSemaphoreAttributes, lInitialCount, 999 /*ign_lMaximumCount*/, lpName ))
			|| ret->type != MSH_SEMAPHORE
		){
			fprintf( stderr, "CreateSemaphore(%p,%ld,%ld,%s) failed (%s)\n",
			         ign_lpSemaphoreAttributes, lInitialCount, ign_lMaximumCount, lpName, strerror(errno) );
			delete ret;
			ret = NULL;
		}
	}
	if( freeName ){
		MSEfreeShared(lpName);
	}
	return ret;
}

MSHANDLE::MSHANDLE( void *ign_lpMutexAttributes, BOOL bInitialOwner, char *ign_lpName )
{
	if( !pthread_mutex_init( &d.m.mutbuf, NULL ) ){
		d.m.mutex = &d.m.mutbuf;
		type = MSH_MUTEX;
		if( bInitialOwner ){
			pthread_mutex_trylock(d.m.mutex);
			d.m.owner = pthread_self();
		}
		else{
			d.m.owner = 0;
		}
		Register();
	}
	else{
		type = MSH_EMPTY;
	}
}

/**
	Emulation of the Microsoft function with the same name. Creates a mutex handle, locking
	it if bInitialOwner==true. The attributes and name arguments are ignored in this implementation.
 */
HANDLE CreateMutex( void *ign_lpMutexAttributes, BOOL bInitialOwner, char *ign_lpName )
{ HANDLE ret = NULL;
	if( !(ret = (HANDLE) new MSHANDLE( ign_lpMutexAttributes, bInitialOwner, ign_lpName ))
		|| ret->type != MSH_MUTEX
	){
		fprintf( stderr, "CreateMutex(%p,%s,%s) failed with errno=%d (%s)\n",
		         ign_lpMutexAttributes, (bInitialOwner)? "TRUE" : "FALSE", ign_lpName,
		         errno, strerror(errno)
		       );
		delete ret;
		ret = NULL;
	}
	return ret;
}

MSHANDLE::MSHANDLE( void *ign_lpEventAttributes, BOOL bManualReset, BOOL bInitialState, char *ign_lpName )
{
	if( !pthread_cond_init( &d.e.condbuf, NULL ) ){
		d.e.cond = &d.e.condbuf;
	}
	else{
		d.e.cond = NULL;
	}
	if( d.e.cond && !pthread_mutex_init( &d.e.mutbuf, NULL ) ){
		d.e.mutex = &d.e.mutbuf;
		d.e.waiter = 0;
	}
	else{
		d.e.mutex = NULL;
		if( d.e.cond ){
			pthread_cond_destroy(d.e.cond);
			d.e.cond = NULL;
		}
	}
	if( d.e.cond && d.e.mutex ){
		type = MSH_EVENT;
		d.e.isManual = bManualReset;
		d.e.isSignalled = bInitialState;
		Register();
	}
	else{
		type = MSH_EMPTY;
	}
}

/**
	Create an event handle like its Microsoft CreateEvent() counterpart. Only the ManualReset argument is
	taken into account in this implementation; if false, WaitForSingleObject will reset the event as soon
	as the 1st wait on it returns, otherwise the event has to be reset manually.
	NB: I do not know if in the MS implementation auto-resetting events are guaranteed to release only a
	single wait at a time, I don't think that guarantee exists in this implementation.
 */
HANDLE msCreateEvent( void *ign_lpEventAttributes, BOOL bManualReset, BOOL ign_bInitialState, char *ign_lpName )
{ HANDLE ret = NULL;
	if( !(ret = (HANDLE) new MSHANDLE( ign_lpEventAttributes, bManualReset, ign_bInitialState, ign_lpName ))
	   || ret->type != MSH_EVENT
	){
		fprintf( stderr, "CreateEvent(%p,%s,%s,%s) failed with errno=%d (%s)\n",
		         ign_lpEventAttributes, (bManualReset)? "TRUE" : "FALSE",
		         (ign_bInitialState)? "TRUE" : "FALSE", ign_lpName,
		         errno, strerror(errno) );
		delete ret;
		ret = NULL;
	}
	return ret;
}

/**
	initialises a pthread_timed_t structure EXCEPT for the actual thread creation
 */
static int timedThreadInitialise(pthread_timed_t *tt, const pthread_attr_t *attr,
                          LPTHREAD_START_ROUTINE start_routine, void *arg )
{ int ret = 0;
	if( tt ){
		memset( tt, 0, sizeof(pthread_timed_t) );
		ret = pthread_mutex_init( &tt->m, NULL );
		if( ret ){
			return ret;
		}
		tt->mutex = &tt->m;
		ret = pthread_cond_init( &tt->exit_c, NULL );
		if( ret ){
			return ret;
		}
		tt->cond = &tt->exit_c;
		tt->start_routine = start_routine;
		tt->arg = arg;
		tt->startTime = HRTime_Time();
	}
	else{
		ret = 1;
	}
	return ret;
}

pthread_timed_t::pthread_timed_t(const pthread_attr_t *attr,
			 LPTHREAD_START_ROUTINE start_routine, void *arg)
{ extern int timedThreadInitialise(pthread_timed_t *, const pthread_attr_t *attr,
					  LPTHREAD_START_ROUTINE start_routine, void *arg );
	cseAssertEx( timedThreadInitialise(this, attr, start_routine, arg ) == 0, __FILE__, __LINE__,
			  "failure to initialise a new pthread_timed_t" );
}

/**
	The thread wrapper routine that takes care of 'unlocking' anyone trying to join
	(with or without timeout)
 */
void pthread_timedexit(void *status)
{ pthread_timed_t *tt;

	if( (tt = (pthread_timed_t*) pthread_getspecific(timedThreadKey)) ){
		pthread_mutex_lock(tt->mutex);
		// tell any joiners that we're packing up:
		tt->status = status;
		tt->exiting = true;
		pthread_cond_signal(tt->cond);
		pthread_mutex_unlock(tt->mutex);
//		fprintf( stderr, "@@ thread %p->%p (%p) calling pthread_exit() at lifeTime=%gs\n",
//			   tt, pthread_self(), tt->thread, HRTime_Time() - tt->startTime );
	}
	pthread_exit(status);
	cseAssertEx( false, __FILE__, __LINE__, "pthread_exit() returned - should never happen" );
}

/**
	release any resources contained in pthread_timed_t
	(but not the structure itself, nor the thread handle)
 */
static int timedThreadRelease(pthread_timed_t *tt)
{ int ret = 0;
	if( tt ){
		if( tt->mutex ){
			pthread_mutex_destroy(tt->mutex);
			tt->mutex = NULL;
		}
		if( tt->cond ){
			pthread_cond_destroy(tt->cond);
			tt->cond = NULL;
		}
//		fprintf( stderr, "@@ %p released thread %p->%p at lifeTime=%gs\n",
//			   pthread_self(), tt, tt->thread, HRTime_Time() - tt->startTime );
	}
	else{
		ret = 1;
	}
	return ret;
}

pthread_timed_t::~pthread_timed_t()
{ extern int timedThreadRelease(pthread_timed_t *);
//  extern void cseAssertEx(bool, const char *, int, const char*);
	cseAssertEx( timedThreadRelease(this) == 0, __FILE__, __LINE__,
			  "failure destructing a pthread_timed_t" );
}

/**
	cleanup routine that handles thread cancellation (and incidentally also
	pthread_exit() being called in the user's start_routine). It sets
	tt->exiting as pthread_timedexit would.
 */
static void threadCancelHandler(void *dum)
{ pthread_timed_t *tt;
#if DEBUG > 1
	fprintf( stderr, "@@ %p is being cancelled\n", pthread_self() );
#endif
	if( (tt = (pthread_timed_t*) pthread_getspecific(timedThreadKey)) ){
		pthread_mutex_lock(tt->mutex);
		// tell any joiners that we're packing up:
		tt->exiting = true;
		tt->status = PTHREAD_CANCELED;
		pthread_cond_signal(tt->cond);
		pthread_mutex_unlock(tt->mutex);
	}
}

/**
	pthread_once callback init_routine to create the thread-specific key
	associating the pthread_timed_t structure with the thread it belongs to
 */
static void timed_thread_init()
{
	pthread_key_create(&timedThreadKey, NULL);
}

/**
	upbeat to the thread wrapper routine. It retrieves the pthread_timed_t structure,
	associates it with the current thread handle via specific storage and then calls the
	actual wrapper routine
 */
static void *timedThreadStartRoutine( void *args )
{ pthread_timed_t *tt = (pthread_timed_t*) args;
  int old;
  void *status;
#if defined(__MACH__) || defined(__APPLE_CC__)
#if MAC_OS_X_VERSION_MIN_REQUIRED <= MAC_OS_X_VERSION_10_6
	// Register thread with Garbage Collector on Mac OS X if we're running an OS version that has GC
  void (*registerThreadWithCollector_fn)(void);
	registerThreadWithCollector_fn = (void(*)(void)) dlsym(RTLD_NEXT, "objc_registerThreadWithCollector");
	if( registerThreadWithCollector_fn ){
		(*registerThreadWithCollector_fn)();
	}
#endif // VERSION
#endif
	pthread_once( &timedThreadCreated, timed_thread_init );
	pthread_setspecific( timedThreadKey, (void*) tt );
	pthread_setcancelstate( PTHREAD_CANCEL_ENABLE, &old );
	pthread_setcanceltype( PTHREAD_CANCEL_DEFERRED, &old );
	// now call the user's start_routine, with a safety net provided
	// by threadCancelHandler
	{
		// pthread_cleanup_push & pop must be called in the same scope
		// (the extra braces are redundant though)
		pthread_cleanup_push(threadCancelHandler, NULL);
		status = (tt->start_routine)(tt->arg);
		pthread_cleanup_pop(0);
	}
	pthread_timedexit(status);
	// never here:
	return NULL;
}

/**
	timed version of pthread_join(). Upon the 1st invocation, the thread corresponding to the
	argument is detached, and a conditional wait of the specified timeout duration is
	started on the corresponding condition. This condition (and the exiting flag) is set in
	the pthread_timedexit() wrapper function. If timeout is a NULL pointer, a regular pthread_join()
	is done (instead of the pthread_detach, of course). Note that the status returned by
	pthread_timedjoin() is the one set in the actual thread payload function, copied into
	tt->status by pthread_timedexit().
	NB: don't call call pthread_exit from the thread function - return, or call pthread_timedexit() !!
	pthread_timedjoin() based on http://pubs.opengroup.org/onlinepubs/000095399/xrat/xsh_chap02.html#tag_03_02_08_21
*/
int pthread_timedjoin( pthread_timed_t *tt, struct timespec *timeout, void **status )
{ int ret = 1;
	// start by detaching the thread. The reference implementation does this in its version of
	// pthread_timedcreate (timed_thread_create). We do it here because we invoke pthread_create separately;
	// necessary because we do not know at creation time if the thread will be joined with timeout or not.
//	if( !tt ){
//	  // this doesn't make sense??
//		pthread_once( &timedThreadCreated, timed_thread_init );
//		tt = pthread_getspecific(timedThreadKey);
//	}
	if( tt ){
		SetLastError(0);
		if( timeout ){
		  int perrno;
			if( !tt->detached ){
				if( (ret = pthread_detach(tt->thread)) ){
					goto bail;
				}
				tt->detached = true;
			}
			if( (ret = pthread_mutex_lock(tt->mutex)) ){
				goto bail;
			}
			// wait until the thread announces it's exiting (it may already have...)
			// or until timeout occurs:
			perrno = 0;
			while( ret == 0 && !tt->exiting ){
				ret = pthread_cond_timedwait( tt->cond, tt->mutex, timeout );
				perrno = ret;
			}
			pthread_mutex_unlock(tt->mutex);
			// we don't really care about any unlocking errors, but we do wish to know whether the
			// wait timed out:
			SetLastError(perrno);
		}
		else{
		  void *dum;
			ret = pthread_join( tt->thread, &dum );
			if( ret == 0 ){
				tt->exiting = true;
			}
		}
		if( ret == 0 && tt->exiting ){
			*status = tt->status;
			tt->exited = true;
		}
	}
bail:
	return ret;
}

#if !defined(__APPLE__) && !defined(__MACH__)

/**
	structure for passing startup information to a suspendable thread.
 */
struct TFunParams {
	HANDLE mshThread;
	void *(*start_routine)(void *);
	void *arg;
	bool mustSuspend;
	TFunParams( HANDLE h, void *(*threadFun)(void*), void *args, bool suspended ){
		mshThread = h;
		start_routine = threadFun;
		arg = args;
		mustSuspend = suspended;
	}
};

static bool ThreadSuspender(HANDLE mshThread)
{
	if( mshThread && mshThread->type == MSH_THREAD
	    && mshThread->d.t.threadLock && mshThread->d.t.threadLock->d.m.mutex
	){
		// when we receive this signal, the mutex ought to be lock by the thread
		// trying to suspend us. If so, trying to lock the mutex will suspend us.
#if DEBUG > 1
		fprintf( stderr, "@@ %p suspending itself by locking the suspend mutex\n", mshThread );
#endif
		pthread_mutex_lock(mshThread->d.t.threadLock->d.m.mutex);
		// got the lock ... meaning we were just resumed.
		// now unlock it ASAP so that someone else can try to suspend us again.
		pthread_mutex_unlock(mshThread->d.t.threadLock->d.m.mutex);
#if DEBUG > 1
		fprintf( stderr, "@@ %p suspending unlocked the suspend mutex\n", mshThread );
#endif
		return true;
	}
	else{
		fprintf( stderr, "@@ suspend attempt on invalid thread HANDLE (%p%s)\n",
		         mshThread, (mshThread)? mshThread->asString().c_str() : "" );
		return false;
	}
}

/**
	specific USR2 signal handler that will attempt to suspend the current thread by
	locking an already locked mutex. It does this only for suspendable threads, i.e. threads
	which have a thread HANDLE stored in the suspendKey.
 */
static void pthread_u2_handler(int sig)
{
	if( suspendKey ){
		switch( sig ){
			case SIGUSR2:{
			  HANDLE mshThread;
				// get the mutex from a specific key
				ThreadSuspender( (HANDLE) pthread_getspecific(suspendKey) );
				break;
			}
		}
	}
}

/**
	entry point for a suspendable thread
 */
static void *ThreadFunStart(void *params)
{ struct TFunParams *tp = (struct TFunParams*) params;
  void *(*threadFun)(void*) = tp->start_routine;
  void *args = tp->arg;
	pthread_setspecific( suspendKey, tp->mshThread );
	if( tp->mustSuspend ){
		// now that everything has been set up properly, we can (attempt to)
		// suspend ourselves:
		ThreadSuspender(tp->mshThread);
	}
	delete tp;
	return (*threadFun)(args);
}

/**
	create a suspendable thread on platforms that don't support this by default. The 'trick' is to store
	a reference to the thread HANDLE in a specific thread key, and the thread HANDLE contains
	a dedicated mutex. To suspend the thread, we lock that mutex, and then send a signal (SIGUSR2) that
	will trigger an exception handler that will attempt to unlock that same mutex. To resume the thread,
	all we need to do is to unlock the mutex (the signal handler will also unlock immediately after
	obtaining the lock, so that the mutex remains free).
	The thread function is launched through a proxy that stores the thread HANDLE in the suspendKey.
 */
int pthread_create_suspendable( HANDLE mshThread, const pthread_attr_t *attr,
                                void *(*start_routine)(void *), void *arg, bool suspended )
{ int ret;
	if( !suspendKeyCreated ){
		cseAssertEx( pthread_key_create( &suspendKey, NULL )==0, __FILE__, __LINE__,
		             "failure to create the thread suspend key in pthread_create_suspendable()" );
		suspendKeyCreated = true;
	}

	struct TFunParams *params = new TFunParams( mshThread, start_routine, arg, suspended );

	mshThread->d.t.pThread = &mshThread->d.t.theThread->thread;
	mshThread->type = MSH_THREAD;
	// it doesn't seem to work to install the signal handler from the background thread. Since
	// it's process-wide anyway we can just as well do it here.
	signal( SIGUSR2, pthread_u2_handler );
	if( suspended ){
		// lock the suspend mutex so that ThreadFunStart() will suspend its thread when
		// it calls ThreadSuspender()
			if( !pthread_mutex_lock( mshThread->d.t.threadLock->d.m.mutex ) ){
				mshThread->d.t.lockOwner = GetCurrentThread();
				mshThread->d.t.suspendCount = 1;
			}
	}
	ret = pthread_create( &mshThread->d.t.theThread->thread, attr, ThreadFunStart, params );
	return ret;
}

#endif // !__APPLE__ && !__MACH__

static std::mutex gThreadStartMutex;

MSHANDLE::MSHANDLE( void *ign_lpThreadAttributes, size_t ign_dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress,
	    void *lpParameter, DWORD dwCreationFlags, DWORD *lpThreadId )
{ void* (*start_routine)(void*) = (void* (*)(void*)) lpStartAddress;
  extern void *timedThreadStartRoutine( void *args );
	if( !(d.t.theThread = new pthread_timed_t(NULL, start_routine, lpParameter)) ){
		type = MSH_EMPTY;
		return;
	}
	// create the new thread, one at a time
	{ std::lock_guard<std::mutex> guard(gThreadStartMutex);
#if defined(__APPLE__) || defined(__MACH__)
	if( (dwCreationFlags & CREATE_SUSPENDED) ){
		if( !pthread_create_suspended_np( &d.t.theThread->thread, NULL, timedThreadStartRoutine, d.t.theThread ) ){
			d.t.pThread = &d.t.theThread->thread;
			d.t.machThread = pthread_mach_thread_np(d.t.theThread->thread);
			pthread_getname_np( d.t.theThread->thread, d.t.name, sizeof(d.t.name) );
			d.t.name[ sizeof(d.t.name)-1 ] = '\0';
			d.t.suspendCount = 1;
		}
	}
	else{
		if( !pthread_create( &d.t.theThread->thread, NULL, timedThreadStartRoutine, d.t.theThread ) ){
			d.t.pThread = &d.t.theThread->thread;
			d.t.machThread = pthread_mach_thread_np(d.t.theThread->thread);
			pthread_getname_np( d.t.theThread->thread, d.t.name, sizeof(d.t.name) );
			d.t.name[ sizeof(d.t.name)-1 ] = '\0';
			d.t.suspendCount = 0;
		}
	}
#else
  extern int pthread_create_suspendable( HANDLE mshThread, const pthread_attr_t *attr,
				  void *(*start_routine)(void *), void *arg, bool suspended );
	d.t.threadLock = new MSHANDLE(NULL, false, NULL);
	d.t.lockOwner = NULL;
	if( d.t.threadLock
	   && !pthread_create_suspendable( this, NULL, timedThreadStartRoutine, d.t.theThread, (dwCreationFlags & CREATE_SUSPENDED) )
	){
		d.t.pThread = &d.t.theThread->thread;
	}
	else{
		if( d.t.threadLock ){
			delete d.t.threadLock;
			d.t.threadLock = NULL;
		}
	}
#endif // !__APPLE__ && !__MACH__
	} // end guarded
	if( d.t.pThread ){
		type = MSH_THREAD;
		d.t.threadId = NextThreadID();
		if( lpThreadId ){
			*lpThreadId = d.t.threadId;
		}
		d.t.theThread->status = (THREAD_RETURN) STILL_ACTIVE;
		Register();
	}
	else{
		type = MSH_EMPTY;
	}
}

/**
	Initialise a HANDLE from an existing pthread identifier
 */
MSHANDLE::MSHANDLE(pthread_t fromThread)
{
	if( !(d.t.theThread = new pthread_timed_t(NULL, (LPTHREAD_START_ROUTINE)abort, NULL)) ){
		type = MSH_EMPTY;
		return;
	}
	type = MSH_THREAD;
	d.t.pThread = &d.t.theThread->thread;
	d.t.theThread->thread = fromThread;
	d.t.theThread->status = (THREAD_RETURN) STILL_ACTIVE;
#if defined(__APPLE__) || defined(__MACH__)
	d.t.machThread = pthread_mach_thread_np(d.t.theThread->thread);
	pthread_getname_np( d.t.theThread->thread, d.t.name, sizeof(d.t.name) );
	d.t.name[ sizeof(d.t.name)-1 ] = '\0';
	if( fromThread == pthread_self() && pthread_main_np() ){
		d.t.threadId = 0;
	}
	else
#elif defined(linux)
	if( fromThread == pthread_self() && syscall(SYS_gettid) == getpid() ){
		d.t.threadId = 0;
	}
	else
#endif
	{
		d.t.threadId = NextThreadID();
	}
	Register();
}

/**
	Emulation of the Microsoft CreateThread() function. This implementation uses the start address
	(thread "payload"/worker function), the CreationFlags (only CREATE_SUSPENDED) and will return
	a thread ID in the ThreadId return variable if non-null.
 */
HANDLE CreateThread( void *ign_lpThreadAttributes, size_t ign_dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress,
                     void *lpParameter, DWORD dwCreationFlags, DWORD *lpThreadId )
{ HANDLE ret = NULL;
	if( !(ret = (HANDLE) new MSHANDLE( ign_lpThreadAttributes, ign_dwStackSize, lpStartAddress,
	                                   lpParameter, dwCreationFlags, lpThreadId ))
	    || ret->type != MSH_THREAD
	){
		fprintf( stderr, "CreateThread(%p,%lu,0x%p,0x%p,%lu,0x%p) failed with errno=%d (%s)\n",
		         ign_lpThreadAttributes, ign_dwStackSize, lpStartAddress,
		         lpParameter, dwCreationFlags, lpThreadId,
		         errno, strerror(errno) );
		delete ret;
		ret = NULL;
	}
	return ret;
}

/**
	Resume a thread after suspension.
 */
DWORD ResumeThread( HANDLE hThread )
{ DWORD prevCount = -1;
	if( hThread && hThread->type == MSH_THREAD && hThread->d.t.theThread->thread != pthread_self() ){
		prevCount = hThread->d.t.suspendCount;
		if( hThread->d.t.suspendCount ){
			if( (--hThread->d.t.suspendCount) == 0 ){
#if defined(__APPLE__) || defined(__MACH__)
				if( thread_resume( hThread->d.t.machThread ) != KERN_SUCCESS ){
					// failure ... we're still suspended
					hThread->d.t.suspendCount += 1;
				}
#else
				if( pthread_mutex_unlock( hThread->d.t.threadLock->d.m.mutex ) ){
					// failure ... we're still suspended
					hThread->d.t.suspendCount += 1;
				}
				else{
					hThread->d.t.lockOwner = NULL;
				}
#endif
			}
		}
	}
	return prevCount;
}

/**
	Suspend the given thread. On Apple/MACH, the suspend/resume feature of the underlying
	Mach threads is used. On other systems, the thread is sent an interrupt that should
	suspend it while it tries to obtain the lock on an already locked mutex.
	NB: with that approach it is thus not possible for a thread to suspend itself!
 */
DWORD SuspendThread( HANDLE hThread )
{ DWORD prevCount = -1;
  HANDLE current = GetCurrentThread();
	if( hThread && hThread->type == MSH_THREAD && hThread != current ){
		prevCount = hThread->d.t.suspendCount;
		if( hThread->d.t.suspendCount == 0 ){
#if defined(__APPLE__) || defined(__MACH__)
			if( thread_suspend( hThread->d.t.machThread ) == KERN_SUCCESS ){
				hThread->d.t.suspendCount = 1;
			}
#else
			if( !pthread_mutex_lock( hThread->d.t.threadLock->d.m.mutex ) ){
				hThread->d.t.lockOwner = current;
				if( !pthread_kill( hThread->d.t.theThread->thread, SIGUSR2 ) ){
					hThread->d.t.suspendCount = 1;
				}
			}
#endif
		}
		else{
			hThread->d.t.suspendCount += 1;
		}
	}
	return prevCount;
}

static void currentThreadKeyCreate()
{
	cseAssertEx( pthread_key_create( &currentThreadKey, (void (*)(void*))ForceCloseHandle ) == 0, __FILE__, __LINE__,
	             "failure to create the currentThreadKey for GetCurrentThread()" );
}

/**
	Return a HANDLE to the current thread.
 */
HANDLE GetCurrentThread()
{ HANDLE currentThread = NULL;
	pthread_once( &currentThreadKeyCreated, currentThreadKeyCreate );
//	if( !currentThreadKey ){
//		cseAssertEx( pthread_key_create( &currentThreadKey, (void (*)(void*))ForceCloseHandle ) == 0, __FILE__, __LINE__,
//				  "failure to create the currentThreadKey in GetCurrentThread()" );
//	}
	if( !(currentThread = (HANDLE) pthread_getspecific(currentThreadKey)) || currentThread->type != MSH_THREAD ){
		if( currentThread ){
			delete currentThread;
		}
		if( (currentThread = new MSHANDLE(pthread_self())) ){
			pthread_setspecific( currentThreadKey, currentThread );
		}
	}
	return currentThread;
}

static inline int SchedPriorityFromThreadPriority(int policy, int nPriority)
{ int sched_priority, pmin, pmax, pnormal;
	pmin = sched_get_priority_min(policy);
	pmax = sched_get_priority_max(policy);
	pnormal = (pmin + pmax) / 2;
	switch( nPriority ){
		case THREAD_PRIORITY_ABOVE_NORMAL:
			sched_priority = (pnormal + pmax) / 2;
			break;
		case THREAD_PRIORITY_BELOW_NORMAL:
			sched_priority = (pmin + pnormal) / 2;
			break;
		case THREAD_PRIORITY_HIGHEST:
		case THREAD_PRIORITY_TIME_CRITICAL:
			sched_priority = pmax;
			break;
		case THREAD_PRIORITY_IDLE:
		case THREAD_PRIORITY_LOWEST:
			sched_priority = pmin;
			break;
		case THREAD_PRIORITY_NORMAL:
			sched_priority = pnormal;
			break;
		default:
			sched_priority = nPriority;
			break;
	}
	return sched_priority;
}

/**
	set the priority of the thread identified by the hThread HANDLE to the given
	priority level, THREAD_PRIORITY_{LOWEST,BELOW_NORMAL,NORMAL,ABOVE_NORMAL,HIGHEST}
 */
bool SetThreadPriority( HANDLE hThread, int nPriority )
{ struct sched_param param;
  int policy;
	if( hThread && hThread->type == MSH_THREAD
	    && !pthread_getschedparam( hThread->d.t.theThread->thread, &policy, &param )
	){
		SetLastError(0);
		param.sched_priority = SchedPriorityFromThreadPriority( policy, nPriority );
		return (bool) pthread_setschedparam( hThread->d.t.theThread->thread, policy, &param );
	}
	return true;
}

static inline int ThreadPriorityFromSchedPriority(int policy, int sched_priority)
{ int ret, pmin, pmax, pnormal, pbelow, pabove;
	pmin = sched_get_priority_min(policy);
	pmax = sched_get_priority_max(policy);
	pnormal = (pmin + pmax) / 2;
	pbelow = (pmin + pnormal) / 2;
	pabove =(pnormal + pmax) / 2;
	if( sched_priority < (pmin + pbelow)/2 ){
		ret = THREAD_PRIORITY_LOWEST;
	}
	else if( sched_priority < (pbelow + pnormal)/2 ){
		ret = THREAD_PRIORITY_BELOW_NORMAL;
	}
	else if( sched_priority <= (pnormal + pabove)/2 ){
		ret = THREAD_PRIORITY_NORMAL;
	}
	else if( sched_priority <= (pabove + pmax)/2 ){
		ret = THREAD_PRIORITY_ABOVE_NORMAL;
	}
	else{
		ret = THREAD_PRIORITY_HIGHEST;
	}
	return ret;
}

/**
	get a thread's priority value. The current POSIX sched_priority is mapped
	onto the range of Microsoft values THREAD_PRIORITY_{LOWEST,BELOW_NORMAL,NORMAL,ABOVE_NORMAL,HIGHEST} .
	A check is then made to see if that value maps back onto the current POSIX sched_priority; if not,
	the POSIX priority is adjusted so that an expression like
	@n
	SetThreadPriority( B, GetThreadPriority(A) );
	@n
	always gives 2 threads with identical priorities.
 */
int GetThreadPriority(HANDLE hThread)
{ int ret = sched_get_priority_min(SCHED_FIFO) - sched_get_priority_min(SCHED_RR);
	if( hThread && hThread->type == MSH_THREAD ){
	  int policy, Prior;
	  struct sched_param param;
		if( !pthread_getschedparam( hThread->d.t.theThread->thread, &policy, &param ) ){
//			fprintf( stderr, "pmin=%d pmax=%d\n", sched_get_priority_min(policy), sched_get_priority_max(policy) );
			ret = ThreadPriorityFromSchedPriority( policy, param.sched_priority );
			// check if the thread priority is different from the current sched_priority
			// and correct
			if( (Prior = SchedPriorityFromThreadPriority(policy, ret)) != param.sched_priority ){
				param.sched_priority = Prior;
				pthread_setschedparam( hThread->d.t.theThread->thread, policy, &param );
			}
		}
	}
	return ret;
}

static void InitHANDLEs()
{
	if( !theOpenHandleListReady ){
		theOpenHandleListReady = true;
		theOpenHandleList = new google::dense_hash_map<HANDLE,MSHANDLETYPE>();
		theOpenHandleList->resize(4);
		theOpenHandleList->set_empty_key(NULL);
		theOpenHandleList->set_deleted_key( (HANDLE)-1 );
		HANDLETypeName = new google::dense_hash_map<int,const char*>();
		HANDLETypeName->resize(4);
		HANDLETypeName->set_empty_key(-1);
		HANDLETypeName->set_deleted_key(-2);
		(*HANDLETypeName)[MSH_EMPTY] = "MSH_EMPTY";
		(*HANDLETypeName)[MSH_SEMAPHORE] = "MSH_SEMAPHORE";
		(*HANDLETypeName)[MSH_MUTEX] = "MSH_MUTEX";
		(*HANDLETypeName)[MSH_EVENT] = "MSH_EVENT";
		(*HANDLETypeName)[MSH_THREAD] = "MSH_THREAD";
		(*HANDLETypeName)[MSH_CLOSED] = "MSH_CLOSED";
	}
}

/**
	registers the given HANDLE in the registry
 */
static void RegisterHANDLE(HANDLE h)
{
	InitHANDLEs();
	if( !openHandleListCSE ){
	  void *buffer;
		if( (buffer = MSEreallocShared( NULL, sizeof(CRITSECTLOCK), 0, true )) ){
			openHandleListCSE = new (buffer) CRITSECTLOCK(4000);
		}
	}
	switch( h->type ){
		case MSH_SEMAPHORE:
			if( h->d.s.counter->refHANDLEp != &h->d.s.refHANDLEs ){
			  HANDLE source = FindSemaphoreHANDLE( h->d.s.sem, h->d.s.name );
				// not a source, check if we have d.s.sem == source->d.s.sem
				if( source && source->d.s.sem != h->d.s.sem ){
					fprintf( stderr, "@@ registering copycat semaphore with unique d.s.sem\n" );
					AddSemaListEntry(h);
				}
			}
			else{
				AddSemaListEntry(h);
			}
			break;
		default:
			break;
	}
	{ CRITSECTLOCK::Scope scope(openHandleListCSE,5000);
		if( !scope ){
			WarnLockedHandleList();
		}
		(*theOpenHandleList)[h] = h->type;
	}
//	fprintf( stderr, "@@ Registering HANDLE 0x%p (type %d %s)\n", h, h->type, h->asString().c_str() );
}

/**
	Unregisters the given HANDLE from the registry
 */
static void UnregisterHANDLE(HANDLE h)
{
	switch( h->type ){
		case MSH_SEMAPHORE:
			RemoveSemaListEntry(h->d.s.sem);
			break;
		default:
			break;
	}
	{ CRITSECTLOCK::Scope scope(openHandleListCSE,5000);
		if( !scope ){
			WarnLockedHandleList();
		}
		// here we don't invoke isOpenHandle() because we've already locked access to the list
		if( theOpenHandleListReady && theOpenHandleList->count(h) ){
			theOpenHandleList->erase(h);
			theOpenHandleList->resize(0);
//			fprintf( stderr, "@@ Unregistering HANDLE 0x%p (type %d %s)\n", h, h->type, h->asString().c_str() );
		}
	}
}

void MSHANDLE::Register()
{ void RegisterHANDLE(HANDLE h);
	RegisterHANDLE(this);
}
void MSHANDLE::Unregister()
{ void UnregisterHANDLE(HANDLE h);
	UnregisterHANDLE(this);
}

/**
	HANDLE destructor. It will do any unlocking/releasing/terminating required and release
	all associated memory (i.e. it does most of the work for CloseHandle()).
 */
MSHANDLE::~MSHANDLE()
{ bool ret = false;
	switch( type ){
		case MSH_SEMAPHORE:{
			if( d.s.sem ){
				if( d.s.counter->refHANDLEp != &d.s.refHANDLEs ){
					// just decrement the counter of the semaphore HANDLE we're referring to:
					*(d.s.counter->refHANDLEp) -= 1;
					if( d.s.counter->curCount == 0 ){
						sem_post(d.s.sem);
						d.s.counter->curCount += 1;
					}
					ret = (sem_close(d.s.sem) == 0);
				}
				else if( d.s.refHANDLEs == 0 ){
					if( d.s.counter->curCount == 0 ){
						sem_post(d.s.sem);
						d.s.counter->curCount += 1;
					}
					ret = (sem_close(d.s.sem) == 0);
					if( d.s.name ){
						sem_unlink(d.s.name);
					}
					delete d.s.counter;
					d.s.counter = NULL;
				}
				if( d.s.name ){
					MSEfreeShared(d.s.name);
					d.s.name = NULL;
				}
				ret = true;
			}
			break;
		}
		case MSH_MUTEX:
			if( d.m.mutex ){
				ret = (pthread_mutex_destroy(d.m.mutex) == 0);
			}
			break;
		case MSH_EVENT:
			if( d.e.cond ){
				ret = (pthread_cond_destroy(d.e.cond) == 0);
			}
			if( ret && d.e.mutex ){
				ret = (pthread_mutex_destroy(d.e.mutex) == 0 );
			}
			break;
		case MSH_THREAD:
			if( d.t.pThread ){
				ret = (pthread_join(d.t.theThread->thread, &d.t.theThread->status) == 0);
			}
			else{
				// 20120625: thread already exited, we can set ret=TRUE!
				ret = true;
			}
#if !defined(__APPLE__) && !defined(__MACH__)
			if( d.t.threadLock ){
				delete d.t.threadLock;
				d.t.threadLock = NULL;
			}
#endif
			if( d.t.theThread && d.t.theThread->exited ){
				delete d.t.theThread;
			}
			break;
	}
	if( ret ){
		Unregister();
		type = MSH_EMPTY;
		memset( &d, 0, sizeof(d) );
		type = MSH_CLOSED;
	}
}

static bool CloseHandle( HANDLE hObject, bool joinThread=true )
{
	if( hObject && isOpenHandle(hObject) ){
#ifdef DEBUG
		fprintf( stderr, "CloseHandle(%p%s)\n", hObject, hObject->asString().c_str() );
#endif
		if( hObject->type == MSH_SEMAPHORE ){
			if( hObject->d.s.counter->refHANDLEp != &hObject->d.s.refHANDLEs || hObject->d.s.refHANDLEs == 0 ){
				delete hObject;
			}
		}
		else{
			if( hObject->type == MSH_THREAD && !joinThread ){
				// set d.t.pThread=NULL so ~MSHANDLE won't try to join the thread before deleting the HANDLE
				hObject->d.t.pThread = NULL;
			}
			delete hObject;
		}
		return true;
	}
	fprintf( stderr, "CloseHandle(0x%p) invalid HANDLE (%s)\n", hObject, (hObject)? hObject->asString().c_str() : "??" );
	return false;
}

/**
	Emulates the MS function of the same name:
	Closes the specified HANDLE, after closing the semaphore or mutex.
 */
bool CloseHandle( HANDLE hObject )
{
	return CloseHandle( hObject, true );
}

static bool ForceCloseHandle( HANDLE hObject )
{
	return CloseHandle( hObject, false );
}
#endif // __MINGWxx__

/**
	HANDLE string representation (cf. Python's __repr__)
 */
const std::ostringstream& MSHANDLE::asStringStream(std::ostringstream& ret) const
{
	ret.clear();
	switch( type ){
		case MSH_SEMAPHORE:{
		  char *name = (d.s.name)? d.s.name : (char*) "<NULL>";
			if( d.s.counter ){
				ret << "<MSH_SEMAPHORE \"" << name << "\" curCnt=" << d.s.counter->curCount << " " << d.s.refHANDLEs << " references owner=" << d.s.owner << ">";
			}
			else{
				ret << "<MSH_SEMAPHORE \"" << name << "\" curCnt=??" << " " << d.s.refHANDLEs << " references owner=" << d.s.owner << ">";
			}
			break;
		}
		case MSH_MUTEX:
			ret << "<MSH_MUTEX owner=" << d.m.owner << ">";
			break;
		case MSH_EVENT:
			ret << "<MSH_EVENT manual=" << d.e.isManual << " signalled=" << d.e.isSignalled << " waiter=" << d.e.waiter << ">";
			break;
		case MSH_THREAD:{
		  std::ostringstream name;
#if defined(__APPLE__) || defined(__MACH__)
			if( strlen(d.t.name) ){
				name << " \"" << d.t.name << "\" ";
			}
			else
#endif
			{
				name << "";
			}
			if( d.t.pThread ){
				ret << "<MSH_THREAD thread=" << d.t.theThread << name.str() << " Id=" << d.t.threadId << ">";
			}
			else{
				ret << "<MSH_THREAD thread=" << d.t.theThread << name.str() << " Id=" << d.t.threadId << " returned " << d.t.theThread->status << ">";
			}
			break;
		}
		default:
			ret << "<Unknown HANDLE>";
			break;
	}
	return ret;
}

std::string MSHANDLE::asString()
{ std::ostringstream ret;
	return asStringStream(ret).str();
}

//#if defined(__APPLE_CC__) || defined(__MACH__)
//__attribute__((constructor))
//static void initialiser()
//{
//	theMSEShMemListReady = theOpenHandleListReady = false;
//	delete theMSEShMemList;
//	InitMSEShMem();
//	delete theOpenHandleList;
//	delete HANDLETypeName;
//	InitHANDLEs();
//}
//#endif

#pragma mark ---end non-MSWin code---
#else // __windows__

#include "CritSectEx/CritSectEx.h"
#include "CritSectEx/msemul4win.h"
#include <sparsehash/dense_hash_map>
#include <windows.h>
#include <tchar.h>

int mseShFD = -1;
TCHAR MSESharedMemName[] = "Global\\MSEmulFileMappingObject";
static size_t mmapCount = 0;
static BOOL theMSEShMemListReady = false;
static CRITSECTLOCK *shMemCSE = NULL;

//typedef struct MSEShMemEntries {
//	HANDLE hmem;
//	size_t size;
//} MSEShMemEntries;

typedef google::dense_hash_map<void*,HANDLE> MSEShMemLists;
static MSEShMemLists *theMSEShMemList;
static DWORD sharedMemKey;
static bool sharedMemKeyCreated = false;

static void WarnLockedShMemList()
{
//	fprintf( stderr, "Failure preempting access to the shared memory registry."
//		" This probably means it is being accessed from another thread; proceeding with fingers crossed\n"
//		" Set a breakpoint in WarnLockedShMemList() to trace the origin of this event.\n" );
	MessageBox(NULL, "Failure preempting access to the shared memory registry."
		" This probably means it is being accessed from another thread; proceeding with fingers crossed\n"
		" Set a breakpoint in WarnLockedShMemList() to trace the origin of this event.",
		"Warning", MB_APPLMODAL|MB_OK|MB_ICONEXCLAMATION);
}

/**
	A THREAD SPECIFIC selector whether shared memory should be used. In other words,
	if set by the parent process, childs (processes) will have access to memory allocated
	by the parent, but not vice versa. If the parent also has to have access to memory allocated
	by the child, the child has to call MSEmul_UseSharedMemory(true) as well.
 */
int MSEmul_UseSharedMemory(int useShared)
{ int ret;
	if( !sharedMemKeyCreated ){
		sharedMemKey = TlsAlloc();
		sharedMemKeyCreated = true;
	}
	ret = (int) (TlsGetValue(sharedMemKey) != NULL);
	TlsSetValue( sharedMemKey, (LPVOID) useShared );
	return ret;
}

/**
	query the thread-specific use-shared-memory flag
 */
int MSEmul_UseSharedMemory()
{
	if( !sharedMemKeyCreated ){
		sharedMemKey = TlsAlloc();
		sharedMemKeyCreated = true;
	}
	return (int) (TlsGetValue(sharedMemKey) != NULL);
}

/**
	query the thread-specific use-shared-memory flag
 */
int MSEmul_UsesSharedMemory()
{
	return MSEmul_UseSharedMemory();
}

/**
	Free memory. If the pointer is not a known shared-memory allocation by MSEreallocShared,
	the system's default deallocator is used.
 */
void MSEfreeShared(void *ptr)
{ static short calls = 0;
	if( ptr && ptr != shMemCSE ){
	  CRITSECTLOCK::Scope scope(shMemCSE, 5000);
		if( shMemCSE && !scope ){
			WarnLockedShMemList();
		}
		if( theMSEShMemListReady && theMSEShMemList->count(ptr) ){
		  HANDLE hmem = (*theMSEShMemList)[ptr];
			UnmapViewOfFile(ptr);
			if( CloseHandle(hmem) ){
				(*theMSEShMemList)[ptr] = NULL;
				theMSEShMemList->erase(ptr);
				if( ++calls >= 32 ){
					theMSEShMemList->resize(0);
					calls = 0;
				}
				mmapCount -= 1;
			}
		} 
		else{
			free(ptr);
		}
		ptr = NULL;
	}
}

/**
	atexit handler that frees all remaining allocated shared memory and performs
	some associated house-keeping.
 */
void MSEfreeAllShared()
{
	{ CRITSECTLOCK::Scope scope(shMemCSE);
	  // we don't deallocate shMemCSE here, so if it's non-null the list will be empty when
	  // only shMemCSE remains:
	  size_t empty = (shMemCSE)? 1 : 0;
		if( shMemCSE && !scope ){
			WarnLockedShMemList();
		}
		while( theMSEShMemList->size() > empty ){
		  MSEShMemLists::iterator i = theMSEShMemList->begin();
		  std::pair<void*,HANDLE> elem = *i;
	//		fprintf( stderr, "MSEfreeShared(0x%p) of %lu remaining elements\n", elem.first, theMSEShMemList->size() );
			if( elem.first == shMemCSE ){
				i += 1;
				if( i != theMSEShMemList->end() ){
					elem = *i;
					// MSEfreeShared() try to get another lock on shMemCSE, which
					// is not supported by all CritSectEx.h classes, so we unlock the scope first
					scope.Unlock();
					MSEfreeShared(elem.first);
					scope.Lock();
				}
			}
			else{
				// MSEfreeShared() try to get another lock on shMemCSE, which
				// is not supported by all CritSectEx.h classes, so we unlock the scope first
				scope.Unlock();
				MSEfreeShared(elem.first);
				scope.Lock();
			}
		}
	}
	if( shMemCSE ){
		shMemCSE->~CRITSECTLOCK();
		void *ptr = (void*) shMemCSE;
		shMemCSE = NULL;
		MSEfreeShared(ptr);
	}
}

static void InitMSEShMem()
{
	if( !theMSEShMemListReady ){
		theMSEShMemListReady = true;
		theMSEShMemList = new google::dense_hash_map<void*,HANDLE>();
		theMSEShMemList->resize(4);
		theMSEShMemList->set_empty_key(NULL);
		theMSEShMemList->set_deleted_key( (HANDLE)-1 );
		atexit(MSEfreeAllShared);
	}
}

/**
	Allocate or reallocate memory of size <oldN> to size <N>. If the thread-specific flag is
	set or forceShared==true, the memory is allocated in anonymous shared memory, otherwise
	the system's default (re)allocator is used.
 */
void *MSEreallocShared( void* ptr, size_t N, size_t oldN, int forceShared )
{ void *mem = NULL;
  HANDLE hmem;
	if( !forceShared && !MSEmul_UseSharedMemory() ){
		return (ptr)? realloc(ptr,N) : calloc(N,1);
	}
	// split the size specified over 2 32bit values, if it is larger than UINT_MAX;
	// (N >> 32) is undefined in 32bit operation (but in that case N<=size_lo)!
	DWORD size_lo = (N & 0xffffffff), size_hi = (N > size_lo)? (N >> 32) : 0;
	// 20120713: Should send NULL for the name, or else allocated memory buffers might overlap?!
	// I guess lpName==NULL is what gives anonymous memory...
	hmem = CreateFileMapping( INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
						size_hi, size_lo, NULL );
	if( hmem ){
		mem = (void*) MapViewOfFile( hmem, FILE_MAP_ALL_ACCESS, 0, 0, N );
	}
	if( mem ){
//	  MSEShMemEntries entry;
		memset( mem, 0, N );
		mmapCount += 1;
		InitMSEShMem();
		if( !shMemCSE ){
		  void *buffer;
		  static bool active = false;
			if( !active ){
				active = true;
				if( (buffer = MSEreallocShared( NULL, sizeof(CRITSECTLOCK), 0, true )) ){
					shMemCSE = new (buffer) CRITSECTLOCK(4000);
				}
				active = false;
			}
		}
//		entry.hmem = hmem, entry.size = N;
		{ CRITSECTLOCK::Scope scope(shMemCSE);
			if( shMemCSE && !scope ){
				WarnLockedShMemList();
			}
#if DEBUG > 1
			fprintf( stderr, "@@ MSEreallocShared(%p,%lu,%lu,%d) registering %p)\n",
				   ptr, N, oldN, forceShared, mem );
#endif
			(*theMSEShMemList)[mem] = hmem;
		}
#ifdef DEBUG
//		fprintf( stderr, "MSEreallocShared(%p,%lu)=%p, file mapping handle=%p\n", ptr, N, mem, hmem );
#endif
		if( ptr ){
			memmove( mem, ptr, oldN );
			MSEfreeShared(ptr);
		}
	}
	else{
		if( hmem ){
			CloseHandle(hmem);
		}
	}
	return( mem );
}

/**
	invokes MSEreallocShared with forceShared=false
 */
void *MSEreallocShared( void* ptr, size_t N, size_t oldN )
{
	return MSEreallocShared( ptr, N, oldN, false );
}

/**
	duplicate a string in shared memory (if the thread-specific flag is set)
 */
static char *mmstrdup( char *str )
{ char *ret = NULL;
	if( (ret = (char*) MSEreallocShared(NULL, strlen(str), 0 )) ){
		strcpy( ret, str );
	}
	return ret;
}


#endif // MSWin exclusion
