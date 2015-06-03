/*!
 *  @file timing.h
 *
 *  (C) René J.V. Bertin on 20080926.
 *
 */

#ifndef TIMINGext

// we're not using dll import/export language features in this project.
#if defined(_MSC_VER) && 0
#	ifdef _TIMING_C
#		define TIMINGext __declspec(dllexport)
#	else
#		define TIMINGext __declspec(dllimport)
#	endif
#else
#	define TIMINGext /**/
#endif

#ifdef __cplusplus
extern "C"
{
#endif

TIMINGext extern void init_HRTime();
TIMINGext extern double HRTime_Time();
TIMINGext extern double HRTime_tic();
TIMINGext extern double HRTime_toc();

#ifdef __cplusplus
}
#endif

#endif //TIMINGext
