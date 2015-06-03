// kate: auto-insert-doxygen true; backspace-indents true; indent-width 4; keep-extra-spaces true; replace-tabs false; tab-indents true; tab-width 4;
/*!
    @file CritSectEx.cpp
    A fast CriticalSection like class with timeout taken from
    @n
    http://www.codeproject.com/KB/threads/CritSectEx.aspx
    released under the CPOL license (http://www.codeproject.com/info/cpol10.aspx)
    @n
    extended and ported to Mac OS X & linux by RJVB
    This version of the file has been stripped down to a few helper functions.
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


