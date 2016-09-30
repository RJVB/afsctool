// kate: auto-insert-doxygen true; backspace-indents true; indent-width 5; keep-extra-spaces true; replace-tabs false; tab-indents true; tab-width 5;
/*!
	@file CritSectEx.h
	A fast CriticalSection like class with timeout (taken from and) inspired by
	@n
	http://www.codeproject.com/KB/threads/CritSectEx.aspx
	released under the CPOL license (http://www.codeproject.com/info/cpol10.aspx)
	@n
	extended and ported to Mac OS X & linux by RJVB
	This file includes only RJVB's MutexEx version.
 */

#ifdef SWIG

%module CritSectEx
%{
#	if !(defined(WIN32) || defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__) || defined(SWIGWIN))
#		include "msemul.h"
#	endif
#	include "CritSectEx.h"
%}
%include <windows.i>
%feature("autodoc","3");

%init %{
	init_HRTime();
%}

#endif //SWIG

#ifndef _CRITSECTEX_H

#pragma once

#if defined(WIN32) || defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__) || defined(SWIGWIN)
#	if !defined(WINVER) || WINVER < 0x0501
#		define WINVER 0x0501
#	endif
#	if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0501
#		define _WIN32_WINNT 0x0501
#	endif
#	define	__windows__
#endif

#include <stdio.h>
#include <stdlib.h>

// #ifndef CRITSECT
// #	define CRITSECT	CritSectEx
// #endif

#include "msemul.h"
//#if !defined(__windows__)
#	ifdef __cplusplus
#		include <cstdlib>
#		include <exception>
		typedef class cseAssertFailure : public std::exception{
		public:
			const char *msg;
			const int errcode;
			cseAssertFailure( const char *s, int errcode=errno )
				: errcode(errcode)
			{
				msg = s;
			}
			virtual const char* what() const throw()
			{
				return msg;
			}
			virtual const int code() const throw()
			{
				return errcode;
			}
		} cseAssertFailure;
#	endif
//#endif


#if /*defined(WIN32) || */ defined(_MSC_VER)
#	define InlDebugBreak()	{ __asm { int 3 }; }
#	pragma intrinsic(_WriteBarrier)
#	pragma intrinsic(_ReadWriteBarrier)
#elif (__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 1)
#	define _WriteBarrier()		__sync_synchronize()
#	define _ReadWriteBarrier()	__sync_synchronize()
#endif

#if defined(DEBUG)
#	if defined(_MSC_VER)
#		ifndef ASSERT
//#			define ASSERT(x) do { if (!(x)) InlDebugBreak(); } while (false)
#			define ASSERT(x) cseAssertExInline((void*)(x),__FILE__,__LINE__,"ASSERT")
#		endif // ASSERT
#		ifndef VERIFY
#			define VERIFY(x) ASSERT(x)
#		endif
#	else
#		include <assert.h>
#		ifndef ASSERT
#			define ASSERT(x) assert(x)
#		endif // ASSERT
#		ifndef VERIFY
#			define VERIFY(x) ASSERT(x)
#		endif
#	endif
#else // DEBUG
#	ifndef ASSERT
#		define ASSERT(x)
#	endif // ASSERT
#	ifndef VERIFY
#		define VERIFY(x) (x)
#	endif
#endif // DEBUG

#ifndef STR
#	define STR(name)	# name
#endif
#ifndef STRING
#	define STRING(name)	STR(name)
#endif

#define EXCEPTION_FAILED_CRITSECTEX_SIGNAL	0xC20A018E

#ifdef __cplusplus
#	include <typeinfo>
#endif

#ifdef __cplusplus
	__forceinline static void cseAssertExInline(bool expected, const char *fileName, int linenr, const char *title="CritSectEx malfunction", const char *arg=NULL) throw(cseAssertFailure)
#else
	__forceinline static void cseAssertExInline(void *expected, const char *fileName, int linenr, const char *title, const char *arg)
#endif
	{
		if( !(expected) ){
#if defined(__windows__)
		  ULONG_PTR args[2];
		  int confirmation;
		  char msgBuf[1024];
#endif
		  const char *larg = (arg)? arg : "";;
			if( !title ){
				title = "CritSectEx malfunction";
			}
#if defined(__windows__)
			// error handling. Do whatever is necessary in your implementation.
#	ifdef _MSC_VER
			_snprintf_s( msgBuf, sizeof(msgBuf), sizeof(msgBuf),
					"assertion failure (cseAssertEx called from '%s' line %d%s%s) - continue execution?",
					fileName, linenr, (arg)? " with arg=" : "", larg
			);
#	else
			snprintf( msgBuf, sizeof(msgBuf),
					"assertion failure (cseAssertEx called from '%s' line %d%s%s) - continue execution?",
					fileName, linenr, (arg)? " with arg=" : "", larg
			);
#	endif
			msgBuf[sizeof(msgBuf)-1] = '\0';
			confirmation = MessageBox( NULL, msgBuf, title, MB_APPLMODAL|MB_ICONQUESTION|MB_YESNO );
			if( confirmation != IDOK && confirmation != IDYES ){
				args[0] = GetLastError();
				args[1] = (ULONG_PTR) linenr;
				RaiseException( EXCEPTION_FAILED_CRITSECTEX_SIGNAL, 0 /*EXCEPTION_NONCONTINUABLE_EXCEPTION*/, 2, args );
			}
#else
		  char msgBuf[1024];
			// error handling. Do whatever is necessary in your implementation.
			snprintf( msgBuf, sizeof(msgBuf),
					"fatal CRITSECT malfunction at '%s':%d%s%s)",
					fileName, linenr, (arg)? " with arg=" : "", larg
			);
			fprintf( stderr, "%s %s (errno=%d=%s)\n", msgBuf, title, errno, strerror(errno) );
			fflush(stderr);
#	ifdef __cplusplus
			throw cseAssertFailure(msgBuf);
#	endif
#endif
		}
	}
#ifdef __cplusplus
	extern void cseAssertEx(bool, const char *, int, const char*, const char*);
	extern void cseAssertEx(bool, const char *, int, const char*);
	extern void cseAssertEx(bool, const char *, int);
#endif

#if defined(__GNUC__) && (defined(i386) || defined(__i386__) || defined(__x86_64__) || defined(_MSEMUL_H))

#	define CRITSECTGCC

#else // WIN32?
#	ifdef DEBUG
#		include "timing.h"
#	endif
#endif // CRITSECTGCC

#ifdef __cplusplus
static inline void _InterlockedSetTrue( volatile long &atomic )
{
	if /*while*/( !atomic ){
		if( !_InterlockedIncrement(&atomic) ){
			YieldProcessor();
		}
	}
}

static inline void _InterlockedSetFalse( volatile long &atomic )
{
	while( atomic ){
		if( atomic > 0 ){
			if( _InterlockedDecrement(&atomic) ){
				YieldProcessor();
			}
		}
		else{
			if( _InterlockedIncrement(&atomic) ){
				YieldProcessor();
			}
		}
	}
}
#endif //__cplusplus

#ifndef _MSEMUL_H
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
	if /*while*/( *atomic ){
		if( _InterlockedDecrement(atomic) ){
			YieldProcessor();
		}
	}
}
#endif //_MSEMUL_H

#if defined(__cplusplus)
#if defined(MUTEXEX_CAN_TIMEOUT) && defined(__APPLE__)
#	define __MUTEXEX_CAN_TIMEOUT__
#endif

/*!
	A critical section class API-compatible with Vladislav Gelfer's CritSectEx
	This class uses a simple platform-specific mutex except where native mutexes
	don't provide a timed wait. In that case (OS X), the msemul layer is used to
	emulate CreateSemaphore/ReleaseSemaphore given that a mutex is a semaphore
	with starting value 1. Note however that this imposes the limits that come
	with pthread's sem_open et al (semaphores count to the limit of open files).
 */
class MutexEx {
	// Declare all variables volatile, so that the compiler won't
	// try to optimise something important away.
#if defined(__windows__) || defined(__MUTEXEX_CAN_TIMEOUT__)
	volatile HANDLE	m_hMutex;
	volatile DWORD	m_bIsLocked;
#else
	pthread_mutex_t	m_mMutex, *m_hMutex;
	int				m_iMutexLockError;
	volatile DWORD  m_bIsLocked;
#endif
// #ifdef DEBUG
	volatile long	m_hLockerThreadId;
	volatile bool	m_bUnlocking;
// #endif
	volatile bool	m_bTimedOut;

	// disable copy constructor and assignment
	MutexEx(const MutexEx&);
	void operator = (const MutexEx&);

	__forceinline void PerfLock(DWORD dwTimeout)
	{
#ifdef DEBUG
		if( m_bIsLocked ){
			fprintf( stderr, "Thread %lu attempting to lock mutex of thread %ld\n",
				GetCurrentThreadId(), m_hLockerThreadId
			);
		}
#endif
#if defined(__windows__) || defined(__MUTEXEX_CAN_TIMEOUT__)
		switch( WaitForSingleObject( m_hMutex, dwTimeout ) ){
			case WAIT_ABANDONED:
			case WAIT_FAILED:
				cseAssertExInline(false, __FILE__, __LINE__);
				break;
			case WAIT_TIMEOUT:
				m_bTimedOut = true;
				break;
			default:
#	ifdef DEBUG
				m_hLockerThreadId = (long) GetCurrentThreadId();
#	endif
				m_bIsLocked += 1;
				break;
		}
#elif defined(MUTEXEX_CAN_TIMEOUT)
		{ struct timespec timeout;
			clock_gettime( CLOCK_REALTIME, &timeout );
			{ time_t sec = (time_t) (dwMilliseconds/1000);
				timeout.tv_sec += sec;
				timeout.tv_nsec += (long) ( (dwMilliseconds- sec*1000)* 1000000 );
				while( timeout.tv_nsec > 999999999 ){
					timeout.tv_sec += 1;
					timeout.tv_nsec -= 1000000000;
				}
			}
			errno = 0;
			if( (m_iMutexLockError = pthread_mutex_timedlock( m_hMutex, &timeout )) ){
				if( errno == ETIMEDOUT ){
					m_bTimedOut = true;
				}
				else{
					cseAssertExInline(false, __FILE__, __LINE__, "pthread_mutex_timedlock failed");
				}
			}
#	ifdef DEBUG
			m_hLockerThreadId = (long) GetCurrentThreadId();
#	endif
			m_bIsLocked += 1;
		}
#else
		// attempt to lock m_hMutex;
		if( (m_iMutexLockError = pthread_mutex_lock(m_hMutex)) ){
			if( errno == ETIMEDOUT ){
				m_bTimedOut = true;
			}
			else{
				cseAssertExInline(false, __FILE__, __LINE__, "pthread_mutex_lock failed");
			}
		}
#	ifdef DEBUG
		fprintf( stderr, "Mutex of thread %ld locked (recurs.lock=%ld) by thread %lu at t=%gs\n",
			m_hLockerThreadId, m_bIsLocked+1, GetCurrentThreadId(), HRTime_tic()
		);
		m_hLockerThreadId = (long) GetCurrentThreadId();
#	endif
		m_bIsLocked += 1;
#endif
	}

	__forceinline void PerfUnlock()
	{
//		if( m_bIsLocked ){
#if defined(__windows__)
		ReleaseMutex(m_hMutex);
#elif defined(__MUTEXEX_CAN_TIMEOUT__)
		ReleaseSemaphore(m_hMutex, 1, NULL);
#else
		// release m_hMutex
		m_iMutexLockError = pthread_mutex_unlock(m_hMutex);
#endif
		if( m_bIsLocked > 0 ){
			m_bIsLocked -= 1;
#ifdef DEBUG
			if( !m_bUnlocking ){
				fprintf( stderr, "Mutex of thread %ld unlocked (recurs.lock=%ld) by thread %lu at t=%gs\n",
					m_hLockerThreadId, m_bIsLocked, GetCurrentThreadId(), HRTime_toc()
				);
			}
#endif
		}
#ifdef DEBUG
		m_hLockerThreadId = -1;
#endif
//		}
	}

public:
	volatile unsigned long lockCounter;
#ifdef CRITSECTEX_ALLOWSHARED
	void *operator new(size_t size)
	{ extern void *MSEreallocShared( void* ptr, size_t N, size_t oldN );
		return MSEreallocShared( NULL, size, 0 );
	}
	void operator delete(void *p)
	{ extern void MSEfreeShared(void *ptr);
		MSEfreeShared(p);
	}
#endif //CRITSECTEX_ALLOWSHARED
	size_t scopesUnlocked, scopesLocked;
	// Constructor/Destructor
	MutexEx(DWORD dwSpinMax=0)
	{
		memset(this, 0, sizeof(*this));
#if defined(__windows__)
		m_hMutex = CreateMutex( NULL, FALSE, NULL );
		cseAssertExInline( (m_hMutex!=NULL), __FILE__, __LINE__);
#elif defined(__MUTEXEX_CAN_TIMEOUT__)
		m_hMutex = CreateSemaphore( NULL, 1, -1, NULL );
		cseAssertExInline( (m_hMutex!=NULL), __FILE__, __LINE__);
#else
		// create a pthread_mutex_t
		cseAssertExInline( (pthread_mutex_init(&m_mMutex, NULL) == 0), __FILE__, __LINE__);
		m_hMutex = &m_mMutex;
		scopesUnlocked = scopesLocked = 0;
#endif
#ifdef DEBUG
		m_hLockerThreadId = -1;
		lockCounter = 0;
		init_HRTime();
#endif
	}

	~MutexEx()
	{
#if defined(__windows__) || defined(__MUTEXEX_CAN_TIMEOUT__)
		// should not be done when m_bIsLocked == TRUE ?!
		CloseHandle(m_hMutex);
#else
		// delete the m_hMutex
		m_iMutexLockError = pthread_mutex_destroy(m_hMutex);
		m_hMutex = NULL;
		if( scopesLocked || scopesUnlocked ){
			fprintf( stderr, "MutexEx: %lu scopes were destroyed still locked, %lu were already unlocked\n", 
				scopesLocked, scopesUnlocked );
		}
#endif
	}

	// Lock/Unlock
	__forceinline bool Lock(bool& bUnlockFlag, DWORD dwTimeout = INFINITE)
	{
		PerfLock(dwTimeout);
		bUnlockFlag = !m_bTimedOut;
		return true;
	}

	__forceinline void Unlock(bool bUnlockFlag)
	{
		if( bUnlockFlag ){
#ifdef DEBUG
			m_bUnlocking = true;
#endif
			PerfUnlock();
#ifdef DEBUG
			m_bUnlocking = false;
#endif
		}
	}

	__forceinline bool TimedOut() const { return m_bTimedOut; }
	__forceinline bool IsLocked() const { return (bool) m_bIsLocked; }
	__forceinline DWORD SpinMax()	const { return 0; }
	operator bool () const { return (bool) m_bIsLocked; }

	// Some extra
	void SetSpinMax(DWORD dwSpinMax)
	{
	}
	void AllocateKernelSemaphore()
	{
	}

	// Scope
	class Scope {

		// disable copy constructor and assignment
		Scope(const Scope&);
		void operator = (const Scope&);

		MutexEx *m_pCs;
		bool m_bLocked;
		bool m_bUnlockFlag;

		void InternalUnlock()
		{
			if( m_bUnlockFlag ){
				ASSERT(m_pCs);
				m_pCs->PerfUnlock();
			}
		}

		__forceinline void InternalLock(DWORD dwTimeout)
		{
			ASSERT(m_pCs);
			m_bLocked = m_pCs->Lock(m_bUnlockFlag, dwTimeout);
		}

		__forceinline void InternalLock(MutexEx &cs, DWORD dwTimeout)
		{
			m_bUnlockFlag = false;
			m_pCs = &cs;
			ASSERT(m_pCs);
			m_bLocked = m_pCs->Lock(m_bUnlockFlag, dwTimeout);
		}

		__forceinline void InternalLock(MutexEx *cs, DWORD dwTimeout)
		{
			m_bUnlockFlag = false;
			m_pCs = cs;
			ASSERT(m_pCs);
			m_bLocked = m_pCs->Lock(m_bUnlockFlag, dwTimeout);
		}

	public:
		bool verbose;
		__forceinline Scope()
			:m_pCs(NULL)
			,m_bLocked(false)
			,m_bUnlockFlag(false)
			,verbose(false)
		{
		}
		__forceinline Scope(MutexEx &cs, DWORD dwTimeout = INFINITE)
		{
			verbose = false;
			if( dwTimeout ){
				InternalLock(cs, dwTimeout);
			}
			else{
				m_pCs = &cs, m_bLocked = m_bUnlockFlag = false;
			}
		}
		__forceinline Scope(MutexEx *cs, DWORD dwTimeout = INFINITE)
			:m_pCs(NULL)
			,m_bLocked(false)
			,m_bUnlockFlag(false)
			,verbose(false)
		{
			if( cs && dwTimeout ){
				InternalLock(cs, dwTimeout);
			}
			else{
				m_pCs = cs, m_bLocked = m_bUnlockFlag = false;
			}
		}
		__forceinline ~Scope()
		{
			if( m_pCs && verbose ){
				if( m_bLocked ){
					m_pCs->scopesLocked += 1;
				}
				else{
					m_pCs->scopesUnlocked += 1;
				}
			}
			InternalUnlock();
		}

		bool Lock(MutexEx &cs, DWORD dwTimeout = INFINITE)
		{
			if (&cs == m_pCs)
				return Lock(dwTimeout);

#ifdef DEBUG
			fprintf( stderr, "InternalUnlock before InternalLock!\n" );
#endif
			InternalUnlock();
			InternalLock(cs, dwTimeout);
			return m_bLocked;
		}
		bool Lock(DWORD dwTimeout = INFINITE)
		{
			ASSERT(m_pCs);
			if (!m_bLocked)
				InternalLock(dwTimeout);
			return m_bLocked;
		}
		void Unlock()
		{
			InternalUnlock();
			m_bUnlockFlag = false;
			m_bLocked = false;
		}

		__forceinline bool TimedOut()
		{
			if( m_pCs ){
				return m_pCs->TimedOut();
			}
			else{
				return false;
			}
		}
		__forceinline bool IsLocked() const { return m_bLocked; }
		__forceinline MutexEx *Parent()
		{
			return m_pCs;
		}
		operator bool () const { return m_bLocked; }
	};
	friend class Scope;
};
#undef __MUTEXEX_CAN_TIMEOUT__
#endif

#define _CRITSECTEX_H
#endif // !_CRITSECTEX_H
