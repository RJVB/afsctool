// kate: auto-insert-doxygen true; backspace-indents true; indent-width 4; keep-extra-spaces true; replace-tabs false; tab-indents true; tab-width 4;
/*!
    @file CritSectEx.cpp
    @see CritSectEx.h
    @n
    This version of the file has been stripped down to a few helper functions
    provided under No License At All.
 */

#include "CritSectEx.h"

void cseAssertEx(bool expected, const char *fileName, int linenr, const char *title, const char *arg )
{
	cseAssertExInline( expected, fileName, linenr, title, arg );
}

void cseAssertEx(bool expected, const char *fileName, int linenr, const char *title )
{
	cseAssertExInline( expected, fileName, linenr, title );
}

void cseAssertEx(bool expected, const char *fileName, int linenr )
{
	cseAssertExInline( expected, fileName, linenr );
}


