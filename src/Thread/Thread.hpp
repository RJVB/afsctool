// kate: auto-insert-doxygen true; backspace-indents true; indent-width 5; keep-extra-spaces true; replace-tabs false; tab-indents true; tab-width 5;
/**
	@file Thread.hpp
	A generic thread class inspired by Arun N Kumar's CThread
	http://www.codeproject.com/Articles/1570/A-Generic-C-Thread-Class
	reimplemented and extended by RJVB (C) 2012
	This code is made available under No License At All
 */

#ifndef __THREAD_H__
#define __THREAD_H__

// #define CRITSECTEX_ALLOWSHARED
#include "CritSectEx/CritSectEx.h"

#ifdef __windows__
// to "enable" placement new in MSVC
#	include <new.h>
#	include <ostream>
#	include <sstream>
#endif
#include <iostream>

typedef enum { THREAD_SUSPEND_NOT=0,		//!< The thread runs normally after the initial creation
	THREAD_SUSPEND_BEFORE_INIT=1<<0,		//!< The thread is not unsuspended after its creation
	THREAD_SUSPEND_AFTER_INIT=1<<1,		//!< The thread is allowed to perform Init() and is suspended
	THREAD_SUSPEND_BEFORE_CLEANUP=1<<2		//!< The thread is suspended before Cleanup()
} SuspenderThreadTypes;
#define	THREAD_CREATE_SUSPENDED	THREAD_SUSPEND_BEFORE_INIT

extern DWORD thread2ThreadKey, thread2ThreadKeyClients;

#pragma mark -------------

#include <exception>

class Thread_Exception : public std::exception {
public:
	const char *errorMessage;
	Thread_Exception() throw()
	{
		errorMessage = "";
	}
	Thread_Exception(const char *msg) throw()
	{
		errorMessage = msg;
	}
	virtual const char* what() const throw()
	{
		return errorMessage;
	}
};

/**
	Thread class for easy creation of background worker threads. Standard overridable methods
	are Run() (the worker function), InitThread() and CleanupThread(). In the default version
	the worker thread is created and started with the Start() method, after which it runs to
	completion or until Stop() is called. It is possible to override the constructor to create
	a 'SuspenderThread' which will be created at once. In this case one can specify synchronisation
	points (before/after InitThread(), before CleanupThread()) at which the worker will wait
	until Continue() is called. This makes it possible to ensure all worker threads exist (and
	are initialised properly) before launching the actual workload.
 */
class Thread {
	public:
#pragma mark new Thread
		/**
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
		/**
			delete operator that frees anonymous shared memory
		 */
		void operator delete(void *p)
		{ extern void MSEfreeShared(void *ptr);
			MSEfreeShared(p);
		}

		/**
		 *	Info: Starts the thread.
		 *	
		 *	This function creates and starts the worker thread, passing arg to the worker.
		 *	When called on a SuspenderThread it will unblock the worker in case it is waiting
		 *	at a synchronisation point. (The initial invocation that creates the thread is done
		 *	through the constructor in this case.)
		 */
		DWORD Start( void* arg = NULL );

		/**
			Return the creator thread's HANDLE. This value is defined only after the thread
			has been started (i.e. immediately for SuspenderThreads, but after the call to
			Start() for regular Thread instances).
		 */
		HANDLE Creator()
		{
			return m_ThreadCtx.m_hCreator;
		}

		/**
			returns true if the worker has been started
		 */
		bool IsStarted()
		{
			return hasBeenStarted;
		}

		/**
			returns true if the worker exists and is waiting at a synchronisation point
			OR is suspended
		 */
		bool IsWaiting()
		{
			return hasBeenStarted && (startLock.IsLocked() || isSuspended);
		}

		/**
			Returns the thread's current priority level
		 */
		int ThreadPriority()
		{
			return GetThreadPriority(m_ThreadCtx.m_hThread);
		}
		/**
			Returns the thread's current priority level and sets a new level.
		 */
		int ThreadPriority(int nPriority)
		{ int ret = GetThreadPriority(m_ThreadCtx.m_hThread);
			SetThreadPriority( m_ThreadCtx.m_hThread, nPriority );
			return ret;
		}
		/**
			Returns the thread's current priority level and sets a new level
			according to refThread's priority level
		 */
		int ThreadPriority(HANDLE refThread)
		{ int ret = GetThreadPriority(m_ThreadCtx.m_hThread);
			if( refThread ){
				SetThreadPriority( m_ThreadCtx.m_hThread, GetThreadPriority(refThread) );
			}
			return ret;
		}
		/**
			unblocks a worker that is suspended or waiting at a synchronisation point
		 */
		bool Continue();

		/**
			suspends the worker thread. This can be done at any point
			in the worker cycle, contrary to blocking at synchronisation
			which the worker does itself at fixed points. The method returns
			the previous suspension state.
		 */
		bool Suspend();

		/**
			join the worker. This is pthread terminology for waiting until
			the worker thread exits ... either because it is done or because
			it has received a signal to exit (which Join does NOT give).
			It is possible to specify a timeout in milliseconds.
		 */
		//DWORD Join()
		//{ DWORD ret;
		//	if( m_ThreadCtx.m_hThread ){
		//		return WaitForSingleObject( m_ThreadCtx.m_hThread, INFINITE );
		//	}
		//	else{
		//		ret = WAIT_FAILED;
		//	}
		//	return ret;
		//}
		DWORD Join(DWORD dwMilliSeconds=INFINITE);

		/**
			Stop the worker thread. This call unlocks the worker if it is suspended or waiting
			at a synchronisation point. Currently this function does not actually stop a still
			running thread but only sets the threadShouldExit flag unless the ForceKill flag is
			true. In that case, the thread will be 1) cancelled (which will invoke CleanupThread()
			on MS Windows) and if that has no effect in 5 seconds the worker will be terminated.
			Thread cancelling is a concept from pthreads where the thread will be 'redirected'
			to a proper exit routine (possibly after executing any cleanup handlers) rather than
			killed outright.
			This is likely to change so that Stop(false) will cancel the thread which always ought to
			call the cleanup method) and Stop(true) will terminate the thread if cancelling has no
			effect.
		 */
		DWORD Stop( bool bForceKill=false, DWORD dwForceExitCode=(DWORD)-1 );

		/**
			get the worker's current exit code. This will be STILL_ACTIVE if the
			thread is still running, or else the exit code specified by the worker.
		 */
		THREAD_RETURN GetExitCode();

		/**
		 *	Info: Attaches a Thread Function
		 *	
		 *	Used primarily for porting but can serve in developing generic thread objects
		 */
		void Attach( LPTHREAD_START_ROUTINE lpThreadFunc ){
			m_pThreadFunc = lpThreadFunc;
		}

		/**
		 *	Info: Detaches the Attached Thread Function
		 *	
		 *	Detaches the Attached Thread Function, If any.
		 *	by resetting the thread function pointer to EntryPoint
		 */
		void Detach( void ){
			m_pThreadFunc = /*Thread::*/EntryPoint; 
		}

		/**
		 *	Info: Default Constructor
		 */
		Thread();

		/**
		 *	Constructor to create a thread that is launched at once but
		 *	kept suspended either before or after execution of the InitThread() method.
		 */
		Thread( SuspenderThreadTypes when, void* arg = NULL );
		Thread( int when, void* arg = NULL );
		Thread( Thread & ) throw(const char*)
		{
			throw "Thread instance copying is undefined";
		}
		Thread( const Thread & ) throw(const char*)
		{
			throw "Thread instance copying is undefined";
		}

		/**
		 *	Info: Plug Constructor
		 *
		 *	Use this to migrate/port existing worker threads to objects immediately
		 *  Although you lose the benefits of ThreadCTOR and ThreadDTOR.
		 */
		Thread(LPTHREAD_START_ROUTINE lpExternalRoutine);

		/**
			initialisation function to convert an already created Thread object
			into a SuspenderThread instance - BEFORE Start() has been called.
		 */
		DWORD SuspenderThread( SuspenderThreadTypes when, void* arg = NULL );
		/**
			initialisation function to convert an already created Thread object
			into a SuspenderThread instance - BEFORE Start() has been called.
		 */
		DWORD SuspenderThread( int when, void* arg = NULL );
		/**
			destructor. Stops the worker thread if it is still running and releases
			the thread2ThreadKey local storage object if no one is still using it.
		 */
		virtual ~Thread();

	protected:

		/**
			set the worker exit code/status
		 */
		THREAD_RETURN SetExitCode(THREAD_RETURN dwExitCode);

		/**
			the worker entry point which is responsible for all administrative
			actions that must be performed from inside the worker thread. It can
			but should not be overridden.
		 */
		static THREAD_RETURN WINAPI EntryPoint( LPVOID pArg);

		/**
		 *	Info: Initialisation function. 
		 *	
		 *	Will be called by EntryPoint before executing the thread body.
		 *  Override this function to provide your extra initialisation.
		 */
		virtual void InitThread()
		{
		}

		/**
			the actual worker function; override this method.
		 */
		virtual DWORD Run( LPVOID /* arg */ )
		{
			return m_ThreadCtx.m_dwExitCode;
		}

		/**
		 *	Info: Cleanup function. 
		 *	
		 *	Will be called by EntryPoint after executing the worker function.
		 *  Override this function to provide your extra destruction.
		 */
		virtual void CleanupThread()
		{
		}


	private:
		volatile long m_lCancelling;		//!< flag that is set when the thread is being cancelled

		/**
			private class that implements the lock used for blocking the worker thread at
			synchronisation points.
		 */
		class StartLocks {
			HANDLE lockEvent;			//!< the event HANDLE that is the actual lock
			long isLocked, isNotified;
			public:
				/**
					new() operator that allocates from anonymous shared memory - necessary to be able
					to share semaphore handles among processes
				 */
				void *operator new(size_t size)
				{ extern void *MSEreallocShared( void* ptr, size_t N, size_t oldN );
					return MSEreallocShared( NULL, size, 0 );
				}
				/**
					delete operator that frees anonymous shared memory
				 */
				void operator delete(void *p)
				{ extern void MSEfreeShared(void *ptr);
					MSEfreeShared(p);
				}
				StartLocks();
				~StartLocks();
				__forceinline bool IsLocked()
				{
					return isLocked;
				}
				/**
					notify the waiter, i.e. set the event to signalled. isNotified
					will remain set until the first waiter unlocks.
				 */
				__forceinline bool Notify()
				{
					// notified is used just in case something interrupts WaitForSingleObject
					// (i.e. a non-timeout return) not preceded by a notification.
					_InterlockedSetTrue(isNotified);
					return SetEvent(lockEvent);
				}
				/**
					block waiting for the event to be notified. During the wait, isNotified==false
					and isLocked==true. The function waits on lockEvent for periods up to 0.1s
					checking isNotified after each wait.
				 */
				__forceinline bool Wait()
				{ DWORD ret;
					if( !isLocked ){
						_InterlockedSetTrue(isLocked);
						while( (ret = WaitForSingleObject( lockEvent, 100 )) == WAIT_TIMEOUT
							 /*&& ret != WAIT_ABANDONED*/ && !isNotified
						){
							if( ret == WAIT_TIMEOUT ){
								// just in case:
								_InterlockedSetTrue(isLocked);
								YieldProcessor();
							}
						}
#if defined(DEBUG)
						if( ret != WAIT_OBJECT_0 && ret != WAIT_TIMEOUT ){
							fprintf( stderr, "@@@ %p->Wait() on %p returned %lu; notified=%ld\n",
								this, lockEvent, ret, isNotified );
						}
#endif // DEBUG
						// unset isLocked
						_InterlockedSetFalse(isLocked);
						// reset the notified state
						_InterlockedSetFalse(isNotified);
					}
					else{
						ret = WAIT_FAILED;
					}
					return (ret != WAIT_ABANDONED && ret != WAIT_FAILED);
				}
		};
	private:
		StartLocks startLock;
		SuspenderThreadTypes suspendOption;	//!< mask specifying if and how the worker thread should wait at synchronisation points
		long isSuspended;
		bool hasBeenStarted;

		/**
			lowlevel, internal initialisation
		 */
		void __init__();

		/**
		 *	Info: Thread Context Inner Class
		 *	
		 *	Every thread object needs to be associated with a set of values.
		 *	like UserData Pointer, Handle, Thread ID etc.
		 *  
		 *  NOTE: This class can be enhanced to varying functionalities
		 *		  eg.,
		 *				* Members to hold StackSize
		 *				* SECURITY_ATTRIBUTES member.
		 */
#pragma mark class Thread::ThreadContext
		class ThreadContext
		{
			public:
				/**
					new() operator that allocates from anonymous shared memory - necessary to be able
					to share semaphore handles among processes
				 */
				void *operator new(size_t size)
				{ extern void *MSEreallocShared( void* ptr, size_t N, size_t oldN );
					return MSEreallocShared( NULL, size, 0 );
				}
				/**
					delete operator that frees anonymous shared memory
				 */
				void operator delete(void *p)
				{ extern void MSEfreeShared(void *ptr);
					MSEfreeShared(p);
				}
				ThreadContext();
//#ifdef __windows__
//					:ProgName(ProgName())
//#endif
//				{
//					memset(this, 0, sizeof(this));
//				}

				/**
				 *	Attributes Section
				 */
			public:
				HANDLE m_hThread;		//!<	The Thread Handle
				DWORD  m_dwTID;		//!<	The Thread ID
				LPVOID m_pUserData;		//!<	The user data pointer
				LPVOID m_pParent;		//!<	A copy of the <this> pointer of the Thread object
#ifdef __windows__
				const char *ProgName;
#endif
				DWORD  m_dwExitCode;	//!<	The Exit Code of the thread
				bool	  m_bExitCodeSet;	//!< Whether the exit code has been set explicitly
				HANDLE m_hCreator;		//!< handle of the creator thread
				double m_startTime;		//!< start time set at the start of EntryPoint().
				double m_endTime;		//!< end time set just before EntryPoint() returns.
				double m_waitTime;		//!< time spent in THREAD_SUSPEND_AFTER_INIT and/or THREAD_SUSPEND_BEFORE_CLEANUP
				double m_runTime;		//!< endTime-startTime-waitTime
		};

		/**
			the cancel callback responsible for calling CleanupThread when the worker
			is being cancelled
		 */
		static void WINAPI HandleCancel();

		/**
			cancel the worker thread, i.e. coerce it through an 'official' exit point
			rather than killing it outright. Currently implemented on MS Win only.
		 */
		bool Cancel();

		/**
		 *	Attributes Section
		 */
	protected:
		/**
		 *	Info: Members of Thread
		 */
		ThreadContext			m_ThreadCtx;			//!<	The Thread Context member
		LPTHREAD_START_ROUTINE	m_pThreadFunc;			//!<	The Worker Thread Function Pointer
		long					threadShouldExit;		//!< flag set be Stop() to signal the worker that it should exit.
	public:
		HANDLE GetThread()
		{
			return m_ThreadCtx.m_hThread;
		}
};

#pragma mark -------------

/**
	A simple class interface to spawn a function in a Thread. Rather than defining a specific Thread instance
	for each background process, this interface allows boost::thread like statements like
	@n
	BackgroundFunction<THREAD_RETURN,int> bgFun0A(functionWithoutArguments, true);
	@n or
	@n
	BackgroundFunction<double,DWORD*> bgFun1A(functionWith1Argument, &counter, false);
	@n (see threadTest.cpp)
 */
template <typename ReturnType, typename ArgType>
class BackgroundFunction : public Thread
{
	protected:
		const bool hasArgument;
		bool done;
		ReturnType functionResult;
		union f {
			ReturnType (*function0Args)();
			ReturnType (*function1Arg)(ArgType arg);
		} backgroundFunction;
	public:
		const ArgType functionArgument;

		BackgroundFunction(ReturnType (*function)(ArgType arg),ArgType arg, bool immediate=true)
			: hasArgument(true),
				functionArgument(arg),
				Thread( THREAD_CREATE_SUSPENDED, NULL )
		{
			backgroundFunction.function1Arg = function;
			if( immediate ){
				Continue();
			}
		}
		BackgroundFunction(ReturnType (*function)(), bool immediate=true)
			: hasArgument(false),
				functionArgument((ArgType)0),
				Thread( THREAD_CREATE_SUSPENDED, NULL )
		{
			backgroundFunction.function0Args = function;
			if( immediate ){
				Continue();
			}
		}
		/**
			quick query of the function result - undefined when the function
			is still running
		 */
		ReturnType result()
		{
			return functionResult;
		}
		/**
			Returns true and the function's result value when it is done
			executing, false otherwise (in which case result is not changed).
		 */
		bool getResult(ReturnType &result)
		{
			if( done ){
				result = functionResult;
			}
			return done;
		}
	protected:
		DWORD Run( LPVOID /*arg*/ )
		{
			done = false;
			if( hasArgument ){
				functionResult = (*backgroundFunction.function1Arg)(functionArgument);
			}
			else{
				functionResult = (*backgroundFunction.function0Args)();
			}
			done = true;
			return 0;
		}
};

#endif //__THREAD_H__
