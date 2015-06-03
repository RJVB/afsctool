// kate: auto-insert-doxygen true; backspace-indents true; indent-width 5; keep-extra-spaces true; replace-tabs false; tab-indents true; tab-width 5;
/*!
 *  @file msemul.h
 *	emulation of multithreading related functions from MS Windows
 *
 *  Created by René J.V. Bertin on 20111204.
 *  Copyright 2011 RJVB. All rights reserved.
 *  This code is made available under the CPOL License
 *  http://www.codeproject.com/info/cpol10.aspx
 */

#ifdef SWIG

%module MSEmul
%{
#	if !(defined(WIN32) || defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__) || defined(SWIGWIN))
#		include "msemul.h"
#	endif
#include "CritSectEx.h"
%}
%include <windows.i>
%include "exception.i"
%exception {
	try {
		$action
	}
	catch( const std::exception& e ){
		SWIG_exception(SWIG_RuntimeError, e.what());
	}
}
#include "CritSectEx.h"

%init %{
	init_HRTime();
%}

%feature("autodoc","3");

#endif //SWIG

#if defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)
#	include "CritSectEx/msemul4win.h"
#	include "timing.h"
#	define	__windows__
#elif !defined(_MSEMUL_H)

#	include <stdio.h>
#	include <stdint.h>
#	include <stdlib.h>
#	include <unistd.h>
#	include <string.h>
#	ifdef __cplusplus
#		include <sstream>
#	endif
#	include <errno.h>
#	include <signal.h>
#	include <sys/time.h>
#	ifdef linux
#		include <fcntl.h>
#		include <sys/syscall.h>
#	endif
#	include <sys/stat.h>
#	include <semaphore.h>
#	include <pthread.h>
#	ifdef __SSE2__
#		include <xmmintrin.h>
#	endif

#	include "timing.h"

#ifndef __forceinline
#	define __forceinline	inline
#endif
#ifndef __OBJC__
#	define	BOOL		bool
#endif

/*!
	macro defining an infinite timeout wait.
 */
#define	INFINITE		-1

#ifndef __cplusplus
#	ifndef bool
#		define bool		unsigned char
#	endif
#	ifndef false
#		define false		((bool)0)
#	endif
#	ifndef true
#		define true		((bool)1)
#	endif
#	define NSSYNCHROTR		/**/
#else
#	include <string>
#	define NSSYNCHROTR		SynchrTr::
	namespace SynchrTr{}
#endif // !__cplusplus

//#	ifndef DWORD
//		typedef unsigned int	DWORD;
//#	endif

typedef void*		PVOID;
typedef void*		LPVOID;
#if !defined(__MINGW32__) && !defined(__MINGW64__)
#	ifndef DWORD
		/*!
			DWORD is MS's slang for size_t
		 */
		typedef size_t	DWORD, *LPDWORD;
#	endif
#	ifndef WINAPI
#		define WINAPI	/*WINAPI*/
#	endif //WINAPI
	typedef void*			THREAD_RETURN;
	typedef THREAD_RETURN	(WINAPI *LPTHREAD_START_ROUTINE)(LPVOID);

#	define ZeroMemory(p,s)	memset((p), 0, (s))

	/*!
	 the types of MS Windows HANDLEs that we can emulate:
	 */
	typedef enum { MSH_EMPTY, MSH_SEMAPHORE, MSH_MUTEX, MSH_EVENT, MSH_THREAD, MSH_CLOSED } MSHANDLETYPE;
#	define CREATE_SUSPENDED	0x00000004
#	define STILL_ACTIVE		259
#	define TLS_OUT_OF_INDEXES	NULL
#	define MAXIMUM_WAIT_OBJECTS	1
#	define WAIT_ABANDONED	((DWORD)0x00000080)
#	define WAIT_OBJECT_0	((DWORD)0x00000000)
#	define WAIT_TIMEOUT		((DWORD)0x00000102)
#	define WAIT_FAILED		((DWORD)0xffffffff)

	enum { THREAD_PRIORITY_ABOVE_NORMAL=1, THREAD_PRIORITY_BELOW_NORMAL=-1, THREAD_PRIORITY_HIGHEST=2,
		THREAD_PRIORITY_IDLE=-15, THREAD_PRIORITY_LOWEST=-2, THREAD_PRIORITY_NORMAL=0,
		THREAD_PRIORITY_TIME_CRITICAL=15 };

#ifdef SWIG
#	define HANDLE	MSHANDLE*
#else
	typedef struct MSHANDLE*	HANDLE;
#endif
	/*!
		Microsoft's semaphore SECURITY_ATTRIBUTES
	 */
	typedef struct _SECURITY_ATTRIBUTES {
		DWORD  nLength;			//!< set to sizeof(SECURITY_ATTRIBUTES)
		LPVOID lpSecurityDescriptor;
		BOOL   bInheritHandle;
	} SECURITY_ATTRIBUTES, *PSECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;
	/*!
		OpenSemaphore() must be called with dwDesiredAccess==DELETE|SYNCHRONIZE|SEMAPHORE_MODIFY_STATE
		in order to return an existing named semaphore object on MS Win. This mask is compatible
		with semaphores created with CreateSemaphore's lpSemaphoreAttributes==NULL or a SECURITY_ATTRIBUTES
		structure with lpSecurityDescriptor==NULL .
	 */
	enum SEMAPHORE_SECURITY_ATTRMASKS { DELETE=0x00010000L, SYNCHRONIZE=0x00100000L,
		SEMAPHORE_MODIFY_STATE=0x0002 };
	/*!
		A structure to keep track of the semaphore's count and Microsoft's maximumCount feature
		curCount is required because Mac OS X doesn't have a functional sem_getvalue().
	 */
	typedef struct MSHSEMAPHORECOUNTER {
		long curCount, maxCount;
		unsigned int *refHANDLEp;
#ifdef __cplusplus
#	include <new>
		/*!
		 new() operator that allocates from anonymous shared memory - necessary to be able
		 to share semaphore handles among processes
		 */
		void *operator new(size_t size) throw(std::bad_alloc)
		{ extern void *MSEreallocShared( void* ptr, size_t N, size_t oldN );
		  void *p = MSEreallocShared( NULL, size, 0 );
			if( p ){
				return p;
			}
			else{
				throw std::bad_alloc();
			}
		}
		/*!
		 delete operator that frees anonymous shared memory
		 */
		void operator delete(void *p)
		{ extern void MSEfreeShared(void *ptr);
			MSEfreeShared(p);
		}
		MSHSEMAPHORECOUNTER(long curCnt, long maxCnt, unsigned int *refs )
		{
			curCount = curCnt;
			maxCount = maxCnt;
			refHANDLEp = refs;
		}
#endif
	} MSHSEMAPHORECOUNTER;
	/*!
	 an emulated semaphore HANDLE
	 */
	typedef struct MSHSEMAPHORE {
		char *name;
		sem_t *sem;
		MSHSEMAPHORECOUNTER *counter;
		unsigned int refHANDLEs;
		pthread_t owner;
	} MSHSEMAPHORE;

	/*!
	 an emulated mutex HANDLE
	 */
	typedef struct MSHMUTEX {
		pthread_mutex_t mutbuf, *mutex;
		pthread_t owner;
	} MSHMUTEX;

	typedef struct MSHEVENT {
		pthread_cond_t condbuf, *cond;
		pthread_mutex_t mutbuf, *mutex;
		pthread_t waiter;
		bool isManual;
		long isSignalled;
	} MSHEVENT;

	/*!
		POSIX threads do not provide a timed join() function, allowing to wait for
		thread termination for a limited time only. It is possible to implement such a function
		using thread cancellation; this structure holds the information necessary to do that.
	 */
	typedef struct pthread_timed_t {
		pthread_t			thread;
		pthread_mutex_t	m, *mutex;
		pthread_cond_t		exit_c, *cond;
		bool				exiting, exited, detached;
		LPTHREAD_START_ROUTINE	start_routine;
		LPVOID			arg;
		THREAD_RETURN		status;
		double			startTime;
#ifdef __cplusplus
		pthread_timed_t(const pthread_attr_t *attr,
					 LPTHREAD_START_ROUTINE start_routine, void *arg);

		~pthread_timed_t();
#endif
	} pthread_timed_t;

	/*!
		an emulated thread HANDLE
	 */
	typedef struct MSHTHREAD {
		pthread_timed_t	*theThread;
		pthread_t			*pThread;
#if defined(__APPLE__) || defined(__MACH__)
		mach_port_t		machThread;
		char				name[32];
#else
		HANDLE			threadLock;
		HANDLE			lockOwner;
#endif
		THREAD_RETURN		returnValue;
		DWORD			threadId, suspendCount;
	} MSHTHREAD;

	typedef union MSHANDLEDATA {
		MSHSEMAPHORE s;
		MSHMUTEX m;
		MSHEVENT e;
		MSHTHREAD t;
	} MSHANDLEDATA;

	/*!
		 an emulated HANDLE of one of the supported types (see MSHANDLETYPE)
		 @n
		 MS Windows uses the same type for a wealth of things, here we are only concerned
		 with its use in the context of multithreaded operations.
	 */
	typedef struct MSHANDLE {
		MSHANDLETYPE type;
		MSHANDLEDATA d;
#	ifdef __cplusplus
		 private:
			DWORD NextThreadID()
			{ static DWORD Id = 1;
				return Id++;
			}
			void mmfree(void *p)
			{ extern void MSEfreeShared(void *ptr);
				MSEfreeShared(p);
			}
			void Register();
			void Unregister();
		 public:
#pragma mark new MSHANDLE
			/*!
				new() operator that allocates from anonymous shared memory - necessary to be able
				to share semaphore handles among processes
			 */
			void *operator new(size_t size)
			{ extern void *MSEreallocShared( void* ptr, size_t N, size_t oldN );
				return MSEreallocShared( NULL, size, 0 );
			}
			/*!
				delete operator that frees anonymous shared memory
			 */
			void operator delete(void *p)
			{ extern void MSEfreeShared(void *ptr);
				MSEfreeShared(p);
			}
#pragma mark CreateSemaphore
			/*!
				initialise a new semaphore HANDLE
			 */
			MSHANDLE( void* ign_lpSemaphoreAttributes, long lInitialCount, long lMaximumCount, char *lpName );
#pragma mark OpenSemaphore
			/*!
				initialise a new semaphore HANDLE that references an existing semaphore HANDLE
			 */
			MSHANDLE( sem_t *sema, MSHSEMAPHORECOUNTER *counter, char *lpName );
#pragma mark CreateMutex
			/*!
				initialise a mutex HANDLE
			 */
			MSHANDLE( void *ign_lpMutexAttributes, BOOL bInitialOwner, char *ign_lpName );
#pragma mark CreateEvent
			/*!
				initialise an event HANDLE
			 */
			MSHANDLE( void *ign_lpEventAttributes, BOOL bManualReset, BOOL bInitialState, char *ign_lpName );
#pragma mark CreateThread
			/*!
				initialise a thread HANDLE
			 */
			MSHANDLE( void *ign_lpThreadAttributes, size_t ign_dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress,
				    void *lpParameter, DWORD dwCreationFlags, DWORD *lpThreadId );
#pragma mark initialise a HANDLE from an existing pthread identifier
			/*!
				Initialise a HANDLE from an existing pthread identifier
			 */
			MSHANDLE(pthread_t fromThread);
			MSHANDLE( const MSHANDLE &h ) throw(char*)
			{
				throw "MSHANDLE instance copying is undefined";
			}
#pragma mark CloseHandle
			/*!
				HANDLE destructor
			 */
			~MSHANDLE();
			/*!
				HANDLE string representation (cf. Python's __repr__)
			 */
			const std::ostringstream& asStringStream(std::ostringstream& ret) const;
			std::string asString();
			template <typename CharT, typename Traits>
				friend std::basic_ostream<CharT, Traits>& operator <<(std::basic_ostream <CharT, Traits>& os, const struct MSHANDLE& x)
				{
					if( os.good() ){
					  typename std::basic_ostream <CharT, Traits>::sentry opfx(os);
						if( opfx ){
						  std::basic_ostringstream<CharT, Traits> s;
							s.flags(os.flags());
							s.imbue(os.getloc());
							s.precision(os.precision());
							x.asStringStream(s);
							if( s.fail() ){
								os.setstate(std::ios_base::failbit);
							}
							else{
								os << s.str();
							}
						}
					}
					return os;
				};
#	endif //cplusplus
	} MSHANDLE;
#endif // !__MINGWxx__

#if defined(__APPLE__) || defined(__MACH__) && defined(__THREADS__)
#	define GetCurrentThread()	GetCurrentThreadHANDLE()
#endif

#	ifdef __cplusplus
	extern int MSEmul_UseSharedMemory();
extern "C" {
#	endif

	extern int MSEmul_UseSharedMemory(int useShared);
	extern int MSEmul_UsesSharedMemory();

	extern DWORD WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
	extern HANDLE CreateSemaphore( void* ign_lpSemaphoreAttributes, long lInitialCount, long lMaximumCount, char *lpName );
	extern HANDLE OpenSemaphore( DWORD ign_dwDesiredAccess, BOOL ign_bInheritHandle, char *lpName );
	extern HANDLE CreateMutex( void *ign_lpMutexAttributes, BOOL bInitialOwner, char *ign_lpName );
	extern HANDLE msCreateEvent( void *ign_lpEventAttributes, BOOL bManualReset, BOOL bInitialState, char *ign_lpName );
/*!
	CreateEvent: macro interface to msCreateEvent.
 */
#	define CreateEvent(A,R,I,N)	msCreateEvent((A),(R),(I),(N))
	extern HANDLE CreateThread( void *ign_lpThreadAttributes, size_t ign_dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress,
				void *lpParameter, DWORD dwCreationFlags, DWORD *lpThreadId );
	extern DWORD ResumeThread( HANDLE hThread );
	extern DWORD SuspendThread( HANDLE hThread );
	extern HANDLE GetCurrentThread();
	extern int GetThreadPriority(HANDLE hThread);
	extern bool SetThreadPriority( HANDLE hThread, int nPriority );
	extern bool CloseHandle( HANDLE hObject );
#	ifdef __cplusplus
}
#	endif

static inline DWORD GetCurrentProcessId()
{
	return (DWORD) getpid();
}

static inline DWORD GetCurrentThreadId()
{
	return (DWORD) pthread_self();
}

static inline DWORD GetThreadId(HANDLE Thread)
{
	if( Thread && Thread->type == MSH_THREAD ){
		return (DWORD) Thread->d.t.pThread;
	}
	else{
		errno = ESRCH;
		return 0;
	}
}

static inline BOOL GetExitCodeThread( HANDLE hThread, DWORD *lpExitCode )
{ BOOL ret = false;
	if( hThread && hThread->type == MSH_THREAD && lpExitCode ){
		if( hThread->d.t.pThread ){
			if( pthread_kill(hThread->d.t.theThread->thread, 0) == 0 ){
				*lpExitCode = STILL_ACTIVE;
			}
			else{
				*lpExitCode = (DWORD) hThread->d.t.theThread->status;
			}
		}
		else{
			*lpExitCode = (DWORD) hThread->d.t.theThread->status;
		}
		ret = true;
	}
	else{
		errno = EINVAL;
	}
	return ret;
}

static inline BOOL TerminateThread( HANDLE hThread, DWORD dwExitCode )
{ BOOL ret = false;
	if( hThread && hThread->type == MSH_THREAD ){
		if( hThread->d.t.pThread ){
			if( pthread_cancel(hThread->d.t.theThread->thread) == 0 ){
			  int i;
			  DWORD code;
				for( i = 0 ; i < 5 ; ){
					if( (code = WaitForSingleObject( hThread, 1000 )) == WAIT_OBJECT_0 ){
						break;
					}
					else{
						i += 1;
					}
				}
				if( i == 5 ){
					pthread_kill( hThread->d.t.theThread->thread, SIGTERM );
				}
				ret = true;
			}
			else{
				pthread_kill( hThread->d.t.theThread->thread, SIGTERM );
				ret = true;
			}
			hThread->d.t.theThread->status = (THREAD_RETURN) dwExitCode;
		}
	}
	return ret;
}

static inline void ExitThread(THREAD_RETURN dwExitCode)
{
	pthread_exit((void*) dwExitCode);
}
				
/*!
 Emulates the Microsoft-specific intrinsic of the same name.
 @n
 compare Destination with Comparand, and replace with Exchange if equal;
 returns the entry value of *Destination.
 @n
 @param Destination	address of the value to be changed potentially
 @param Exchange	new value to store in *Destination if the condition is met
 @param Comparand	the value *Destination must have in order to be replaced with Exchange
 */
static inline long _InterlockedCompareExchange( volatile long *Destination,
							long Exchange,
							long Comparand)
{ long result, old = *Destination;

	__asm__ __volatile__ (
#ifdef __x86_64__
					  "lock; cmpxchgq %q2, %1"
#else
					  "lock; cmpxchgl %2, %1"
#endif
					  : "=a" (result), "=m" (*Destination)
					  : "r" (Exchange), "m" (*Destination), "0" (Comparand));

	return old;
}

#if !defined(__MINGW32__) && !defined(__MINGW64__)
/*!
 Performs an atomic compare-and-exchange operation on the specified values.
 The function compares two specified pointer values and exchanges with another pointer
 value based on the outcome of the comparison.
 @n
 To operate on non-pointer values, use the _InterlockedCompareExchange function.
 */
static inline PVOID InterlockedCompareExchangePointer( volatile PVOID *Destination,
							PVOID Exchange,
							PVOID Comparand)
{ PVOID result, old = *Destination;

	__asm__ __volatile__ (
#ifdef __x86_64__
					  "lock; cmpxchgq %q2, %1"
#else
					  "lock; cmpxchgl %2, %1"
#endif
					  : "=a" (result), "=m" (*Destination)
					  : "r" (Exchange), "m" (*Destination), "0" (Comparand));

	return old;
}
#endif // !__MINGWxx__

/*
 Emulates the Microsoft-specific intrinsic of the same name.
 @n
 Increments the value at the address pointed to by atomic with 1 in locked fashion,
 i.e. preempting any other access to the same memory location
 */
static inline long _InterlockedIncrement( volatile long *atomic )
{
	__asm__ __volatile__ ("lock; addl %1,%0"
			: "=m" (*atomic)
			: "ir" (1), "m" (*atomic));
	return *atomic;
}

/*
 Emulates the Microsoft-specific intrinsic of the same name.
 @n
 Decrements the value at the address pointed to by atomic with 1 in locked fashion,
 i.e. preempting any other access to the same memory location
 */
static inline long _InterlockedDecrement( volatile long *atomic )
{
	__asm__ __volatile__ ("lock; addl %1,%0"
			: "=m" (*atomic)
			: "ir" (-1), "m" (*atomic));
	return *atomic;
}

static inline long _InterlockedAnd( volatile long *atomic, long val )
{
    long i, j;

    j = *atomic;
    do {
        i = j;
        j = _InterlockedCompareExchange(atomic, i | val, i);
    }
    while (i != j);

    return j;
}

/*
 Emulates the Microsoft-specific intrinsic of the same name.
 @n
 Signals to the processor to give resources to threads that are waiting for them.
 This macro is only effective on processors that support technology allowing multiple threads
 running on a single processor, such as Intel's Hyperthreading technology.
 */
static inline void YieldProcessor()
{
#ifdef __SSE2__
	_mm_pause();
#else
	__asm__ __volatile__("rep; nop");
//	__asm__ __volatile__("pause");
#endif
}

/*!
 millisecond timer
 */
static inline DWORD GetTickCount()
{
	return (DWORD) (HRTime_Time() * 1000);
}

static inline DWORD GetLastError()
{
	return (DWORD) errno;
}

static inline void SetLastError(DWORD dwErrCode)
{
	errno = (int) dwErrCode;
}

static inline void Sleep(DWORD dwMilliseconds)
{
	usleep( dwMilliseconds * 1000 );
}

#if !defined(__MINGW32__) && !defined(__MINGW64__)

/*!
 Release the given semaphore.
 @n
 @param lReleaseCount	increase the semaphore count by this number (must be > 0)
 @param lpPreviousCount	optional: return the count on entry
 */
static inline bool ReleaseSemaphore( HANDLE hSemaphore, long lReleaseCount, long *lpPreviousCount )
{ bool ok = false;
	if( hSemaphore && hSemaphore->type == MSH_SEMAPHORE && lReleaseCount > 0
	   && hSemaphore->d.s.counter->curCount + lReleaseCount <= hSemaphore->d.s.counter->maxCount
	){
		if( lpPreviousCount ){
			*lpPreviousCount = hSemaphore->d.s.counter->curCount;
		}
		lReleaseCount += hSemaphore->d.s.counter->curCount;
		do{
			ok = (sem_post(hSemaphore->d.s.sem) == 0);
				hSemaphore->d.s.counter->curCount += 1;
		} while( ok && hSemaphore->d.s.counter->curCount < lReleaseCount );
#ifdef DEBUG
		{ int cval;
			if( sem_getvalue( hSemaphore->d.s.sem, &cval ) != -1 ){
				if( cval != hSemaphore->d.s.counter->curCount ){
					fprintf( stderr, "@@ ReleaseSemaphore(\"%s\"): value mismatch, %ld != %d\n",
						   hSemaphore->d.s.name, hSemaphore->d.s.counter->curCount, cval
					);
				}
			}
		}
#endif
		if( ok ){
			hSemaphore->d.s.owner = 0;
		}
	}
	return ok;
}

static inline bool ReleaseMutex(HANDLE hMutex)
{
	if( hMutex && hMutex->type == MSH_MUTEX ){
		if( !pthread_mutex_unlock(hMutex->d.m.mutex) ){
			hMutex->d.m.owner = 0;
			return false;
		}
	}
	return true;
}

/*!
	set the referenced state variable to True in an atomic operation
	(which avoids changing the state while another thread is reading it)
 */
static inline void _InterlockedSetTrue( volatile long *atomic )
{
	if /*while*/( !*atomic ){
		if( !_InterlockedIncrement(atomic) ){
			YieldProcessor();
		}
	}
}

/*!
	set the referenced state variable to False in an atomic operation
	(which avoids changing the state while another thread is reading it)
 */
static inline void _InterlockedSetFalse( volatile long *atomic )
{
	while( *atomic ){
		if( atomic > 0 ){
			if( _InterlockedDecrement(atomic) ){
				YieldProcessor();
			}
		}
		else{
			if( _InterlockedIncrement(atomic) ){
				YieldProcessor();
			}
		}
	}
}

static inline bool SetEvent( HANDLE hEvent )
{ bool ret = false;
	if( hEvent && hEvent->type == MSH_EVENT ){
		_InterlockedSetTrue(&hEvent->d.e.isSignalled);
		if( hEvent->d.e.isManual ){
			ret = (pthread_cond_broadcast(hEvent->d.e.cond) == 0);
		}
		else{
			ret = (pthread_cond_signal(hEvent->d.e.cond) == 0);
		}
	}
	return ret;
}

static inline bool ResetEvent( HANDLE hEvent )
{ bool ret;
	if( hEvent && hEvent->type == MSH_EVENT ){
		_InterlockedSetFalse(&hEvent->d.e.isSignalled);
		ret = true;
	}
	else{
		ret = false;
	}
	return ret;
}

static inline BOOL TlsFree(DWORD dwTlsIndex)
{ BOOL ret;
  pthread_key_t *key = (pthread_key_t*) dwTlsIndex;
  extern void MSEfreeShared(void *ptr);
	if( dwTlsIndex ){
		ret = (pthread_key_delete(*key) == 0);
		MSEfreeShared(key);
	}
	else{
		ret = false;
	}
	return ret;
}

static inline DWORD TlsAlloc(void)
{ extern void *MSEreallocShared( void* ptr, size_t N, size_t oldN );
  extern void MSEfreeShared(void *ptr);
  pthread_key_t *key = (pthread_key_t*) MSEreallocShared( NULL, sizeof(pthread_key_t), 0 );

	if( key ){
		if( pthread_key_create(key, NULL) != 0 ){
			MSEfreeShared(key);
			key = TLS_OUT_OF_INDEXES;
		}
	}
	else{
		key = TLS_OUT_OF_INDEXES;
	}
	return (DWORD) key;
}

static inline BOOL TlsSetValue( DWORD dwTlsIndex, LPVOID lpTlsValue )
{ pthread_key_t *key = (pthread_key_t*) dwTlsIndex;
  BOOL ret;
	if( key ){
		ret = (pthread_setspecific( *key, lpTlsValue ) == 0);
	}
	else{
		ret = false;
	}
	return ret;
}

static inline LPVOID TlsGetValue( DWORD dwTlsIndex )
{ pthread_key_t *key = (pthread_key_t*) dwTlsIndex;
  LPVOID ret;
	if( key ){
		ret = pthread_getspecific(*key);
		// http://msdn.microsoft.com/en-us/library/windows/desktop/ms686812(v=vs.85).aspx
		SetLastError(0);
	}
	else{
		ret = NULL;
		SetLastError(EINVAL);
	}
	return ret;
}

#endif // !__MINGWxx__

#define _MSEMUL_H
#endif
