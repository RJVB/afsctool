// kate: auto-insert-doxygen true; backspace-indents true; indent-width 5; keep-extra-spaces true; replace-tabs false; tab-indents true; tab-width 5;
/*!
	@file Thread.cpp
	A generic thread class based on Arun N Kumar's CThread
	http://www.codeproject.com/Articles/1570/A-Generic-C-Thread-Class
	adapted and extended by RJVB (C) 2012
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

#include "Thread/Thread.h"

DWORD thread2ThreadKey = NULL, thread2ThreadKeyClients = 0;

#ifdef __windows__
static char *ExecPath(char **execName=NULL)
{ static char progName[260] = "";
	// obtain programme name: cf. http://support.microsoft.com/kb/126571
	if( __argv && !*progName ){
	  char *pName = strrchr(__argv[0], '\\'), *ext = strrchr(__argv[0], '.'), *c, *end;
		c = progName;
		if( pName ){
			pName++;
		}
		else{
			pName = __argv[0];
		}
		end = &progName[sizeof(progName)-1];
		while( pName < ext && c < end ){
			*c++ = *pName++;
		}
		*c++ = '\0';
	}
	if( execName ){
		*execName = progName;
	}
	return (__argv)? __argv[0] : NULL;
}
#endif

Thread::ThreadContext::ThreadContext()
#ifdef __windows__
	:ProgName(::ExecPath())
#endif
{
	m_hThread = NULL;
	m_dwTID = 0;
	m_pUserData = NULL;
	m_pParent = NULL;
	m_dwExitCode = 0;
	m_bExitCodeSet = false;
}

/*!
	lowlevel, internal initialisation
 */
void Thread::__init__()
{
	suspendOption = THREAD_SUSPEND_NOT;
	isSuspended = m_lCancelling = threadShouldExit = 0;
	hasBeenStarted = false;
}

/*!
 *	Info: Default Constructor
 */
Thread::Thread()
{
	__init__();
	Detach();
}

/*!
 *	Constructor to create a thread that is launched at once but
 *	kept suspended either before or after execution of the InitThread() method.
 */
Thread::Thread( SuspenderThreadTypes when, void* arg )
{
	__init__();
	SuspenderThread( when, arg );
}
Thread::Thread( int when, void* arg )
{
	__init__();
	SuspenderThread( (SuspenderThreadTypes)when, arg );
}

/*!
 *	Info: Plug Constructor
 *
 *	Use this to migrate/port existing worker threads to objects immediately
 *  Although you lose the benefits of ThreadCTOR and ThreadDTOR.
 */
Thread::Thread(LPTHREAD_START_ROUTINE lpExternalRoutine)
{
	__init__();
	Attach(lpExternalRoutine);
}

/*!
	initialisation function to convert an already created Thread object
	into a SuspenderThread instance - BEFORE Start() has been called.
 */
DWORD Thread::SuspenderThread( SuspenderThreadTypes when, void* arg )
{
	suspendOption = when;
	Detach();
	return Start(arg);
}

/*!
	initialisation function to convert an already created Thread object
	into a SuspenderThread instance - BEFORE Start() has been called.
 */
DWORD Thread::SuspenderThread( int when, void* arg )
{
	suspendOption = (SuspenderThreadTypes) when;
	Detach();
	return Start(arg);
}

/*!
	destructor. Stops the worker thread if it is still running and releases
	the thread2ThreadKey local storage object if no one is still using it.
 */
Thread::~Thread()
{
	if( m_ThreadCtx.m_hThread ){
		Stop(true);
		if( m_ThreadCtx.m_hThread ){
			CloseHandle(m_ThreadCtx.m_hThread);
		}
	}
	if( thread2ThreadKeyClients > 1 ){
		thread2ThreadKeyClients -= 1;
	}
	else if( thread2ThreadKeyClients == 1 ){
		thread2ThreadKeyClients = 0;
		TlsFree(thread2ThreadKey);
		thread2ThreadKey = NULL;
	}
}

/*!
 *	Info: Starts the thread.
 *	
 *	This function creates and starts the worker thread, passing arg to the worker.
 *	When called on a SuspenderThread it will unblock the worker in case it is waiting
 *	at a synchronisation point. (The initial invocation that creates the thread is done
 *	through the constructor in this case.)
 */
DWORD Thread::Start( void* arg )
{ DWORD ret = 0;
	if( !thread2ThreadKey ){
		thread2ThreadKey = TlsAlloc();
	}
	if( !startLock.IsLocked() ){
		cseAssertEx( m_ThreadCtx.m_hThread == NULL, __FILE__, __LINE__ );
		m_ThreadCtx.m_pUserData = arg;
		if( (m_ThreadCtx.m_hThread = CreateThread( NULL, 0, m_pThreadFunc, this, CREATE_SUSPENDED,
									  &m_ThreadCtx.m_dwTID ))
		){
			m_ThreadCtx.m_dwExitCode = (DWORD)-1;
			m_ThreadCtx.m_pParent = this;
			m_ThreadCtx.m_hCreator = GetCurrentThread();
			hasBeenStarted = true;
			ret = GetLastError();
			if( (suspendOption & THREAD_SUSPEND_BEFORE_INIT) == 0 ){
				ResumeThread( m_ThreadCtx.m_hThread );
				_InterlockedSetFalse(isSuspended);
			}
			else{
				_InterlockedSetTrue(isSuspended);
			}
		}
		else{
			ret = GetLastError();
		}
	}
	else{
		cseAssertEx( m_ThreadCtx.m_hThread != NULL, __FILE__, __LINE__ );
		startLock.Notify();
	}

	return ret;
}

/*!
	unblocks a worker that is suspended or waiting at a synchronisation point
 */
bool Thread::Continue()
{
	if( hasBeenStarted ){
		if( isSuspended ){
			ResumeThread(m_ThreadCtx.m_hThread);
			_InterlockedSetFalse(isSuspended);
		}
		if( startLock.IsLocked() ){
			return startLock.Notify();
		}
	}
	return false;
}

/*!
	suspends the worker thread. This can be done at any point
	in the worker cycle, contrary to blocking at synchronisation
	which the worker does itself at fixed points. The method returns
	the previous suspension state.
 */
bool Thread::Suspend()
{ bool prev = isSuspended;
	if( hasBeenStarted && !isSuspended ){
		if( SuspendThread(m_ThreadCtx.m_hThread) ){
			_InterlockedSetTrue(isSuspended);
		}
	}
	return prev;
}

/*!
	join the worker. This is pthread terminology for waiting until
	the worker thread exits ... either because it is done or because
	it has received a signal to exit (which Join does NOT give).
	It is possible to specify a timeout in milliseconds.
 */
DWORD Thread::Join(DWORD dwMilliSeconds)
{ DWORD ret;
	if( m_ThreadCtx.m_hThread ){
		return WaitForSingleObject( m_ThreadCtx.m_hThread, dwMilliSeconds );
	}
	else{
		ret = WAIT_FAILED;
	}
	return ret;
}

/*!
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
DWORD Thread::Stop( bool bForceKill, DWORD dwForceExitCode )
{
	if( m_ThreadCtx.m_hThread ){
		// set the shouldExit signal flag as the first thing
		_InterlockedSetTrue(threadShouldExit);
		if( isSuspended ){
			Continue();
		}
		DWORD temp = STILL_ACTIVE;
		if( GetExitCodeThread( m_ThreadCtx.m_hThread, &temp ) &&!m_ThreadCtx.m_bExitCodeSet ){
			m_ThreadCtx.m_dwExitCode = temp;
		}

		if( temp == STILL_ACTIVE ){
			if( IsWaiting() ){
				suspendOption = THREAD_SUSPEND_NOT;
				Continue();
			}
			if( bForceKill ){
#if !defined(__windows__)
				TerminateThread( m_ThreadCtx.m_hThread, dwForceExitCode );
#else
				// first try to do something like pthread_cancel
				if( !Cancel() ){
					TerminateThread( m_ThreadCtx.m_hThread, dwForceExitCode );
				}
				else{
					m_ThreadCtx.m_dwExitCode = dwForceExitCode;
				}
#endif
				CloseHandle(m_ThreadCtx.m_hThread);
				m_ThreadCtx.m_hThread = NULL;
				m_ThreadCtx.m_dwExitCode = dwForceExitCode;
			}
		}
		else{
			CloseHandle(m_ThreadCtx.m_hThread);
			m_ThreadCtx.m_hThread = NULL;
		}
	}

	return m_ThreadCtx.m_dwExitCode;
}

/*!
	get the worker's current exit code. This will be STILL_ACTIVE if the
	thread is still running, or else the exit code specified by the worker.
 */
THREAD_RETURN Thread::GetExitCode()
{ 
	if( m_ThreadCtx.m_hThread && !m_ThreadCtx.m_bExitCodeSet ){
	  DWORD temp;
		if( GetExitCodeThread( m_ThreadCtx.m_hThread, &temp ) ){
			m_ThreadCtx.m_dwExitCode = temp;
		}
	}
	return (THREAD_RETURN) m_ThreadCtx.m_dwExitCode;
}

/*!
	the cancel callback responsible for calling CleanupThread when the worker
	is being cancelled
 */
void WINAPI Thread::HandleCancel()
{ Thread *self = (Thread*)TlsGetValue(thread2ThreadKey);
//	fprintf( stderr, "@@ HandleCancel(%p) ...", self ); fflush(stderr);
	if( self ){
		self->CleanupThread();
		self->m_ThreadCtx.m_dwExitCode = ~STILL_ACTIVE;
		_InterlockedDecrement(&self->m_lCancelling);
	}
//	fprintf( stderr, " returning\n" ); fflush(stderr);
	ExitThread((THREAD_RETURN)~STILL_ACTIVE);
	return;
}
/*!
	cancel the worker thread, i.e. coerce it through an 'official' exit point
	rather than killing it outright. Currently implemented on MS Win only.
 */
bool Thread::Cancel()
{ bool ret;
#if !defined(__windows__)
	// to be implemented
	ret = false; 
#else
	// (cf. http://locklessinc.com/articles/pthreads_on_windows/)
  int i = 5;
  CONTEXT ctxt;
	ctxt.ContextFlags = CONTEXT_CONTROL;
	SuspendThread(m_ThreadCtx.m_hThread);
	GetThreadContext( m_ThreadCtx.m_hThread, &ctxt );
#ifdef _M_X64
	ctxt.Rip = (uintptr_t) &Thread::HandleCancel;
#else
	ctxt.Eip = (uintptr_t) &Thread::HandleCancel;
#endif
	SetThreadContext( m_ThreadCtx.m_hThread, &ctxt);
	_InterlockedIncrement(&m_lCancelling);
//	fprintf( stderr, "@@ Thread::Cancel(%p)->ResumeThread(%p)\n", this, m_ThreadCtx.m_hThread );
	ResumeThread(m_ThreadCtx.m_hThread);
	for( i = 0 ; i < 5 ; ){
		if( WaitForSingleObject( m_ThreadCtx.m_hThread, 1000 ) == WAIT_OBJECT_0 ){
			break;
		}
		else{
			i += 1;
		}
	}
	if( i == 5 ){
#ifdef DEBUG
		fprintf( stderr, "@@ %p->Cancel() thread %p didn't cancel in %ds\n",
			this, i );
#endif //DEBUG
		ret = false;
	}
	else{
		ret = true;
	}
#endif // !windows
	return ret;
}

/*!
	set the worker exit code/status
 */
THREAD_RETURN Thread::SetExitCode(THREAD_RETURN dwExitCode)
{ THREAD_RETURN ret = (THREAD_RETURN) m_ThreadCtx.m_dwExitCode;
	m_ThreadCtx.m_dwExitCode = (DWORD) dwExitCode;
	m_ThreadCtx.m_bExitCodeSet = true;
	return ret;
}

Thread::StartLocks::StartLocks()
{
	cseAssertEx( (lockEvent = CreateEvent( NULL, false, false, NULL ))!=NULL, __FILE__, __LINE__ );
	isLocked = false;
	isNotified = false;
}
Thread::StartLocks::~StartLocks()
{
	if( lockEvent ){
		CloseHandle(lockEvent);
	}
	isLocked = false;
}
