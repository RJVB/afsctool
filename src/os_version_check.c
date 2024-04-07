/* ===-- os_version_check.c - OS version checking (LLVM 5.0.2) -------------===
 *
 *                     The LLVM Compiler Infrastructure
 *
 * This file is dual licensed under the MIT and the University of Illinois Open
 * Source Licenses. See LICENSE.TXT for details.
 *
 * ===----------------------------------------------------------------------===
 *
 * This file implements the function __isOSVersionAtLeast, used by
 * Objective-C's @available
 *
 * ===----------------------------------------------------------------------===
 */

#if defined(__APPLE__)
// you'd think that we should be able to call the toolchain's __isOSVersionAtLeast() function
// but apparently we can't even if we ensure that it is linked in. The call will always return 0
// So we just provide the entire implementation, adapted from LLVM 5.0.2

#include <CoreFoundation/CoreFoundation.h>
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* These three variables hold the host's OS version. */
static int32_t GlobalMajor = 0, GlobalMinor, GlobalSubminor;

/* Find and parse the SystemVersion.plist file. */
static void parseSystemVersionPList() {
  /* Load CoreFoundation dynamically */
  // RJVB and make certain it is loaded even if it hasn't been linked in!
  static void* CoreFoundation = NULL;
  if (!CoreFoundation) {
	  CoreFoundation = dlopen("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", RTLD_GLOBAL|RTLD_NOW);
	  // NB: we're simulating that CF was linked (if it hasn't), so we don't call dlclose()!
  }
  const void *NullAllocator = dlsym(RTLD_DEFAULT, "kCFAllocatorNull");
  if (!CoreFoundation || !NullAllocator) {
    return;
  }
  const CFAllocatorRef kCFAllocatorNull =
      *(const CFAllocatorRef *)NullAllocator;
  typeof(CFDataCreateWithBytesNoCopy) *CFDataCreateWithBytesNoCopyFunc =
      (typeof(CFDataCreateWithBytesNoCopy) *)dlsym(
          RTLD_DEFAULT, "CFDataCreateWithBytesNoCopy");
  if (!CFDataCreateWithBytesNoCopyFunc)
    return;
  typeof(CFPropertyListCreateWithData) *CFPropertyListCreateWithDataFunc =
      (typeof(CFPropertyListCreateWithData) *)dlsym(
          RTLD_DEFAULT, "CFPropertyListCreateWithData");
  /* CFPropertyListCreateWithData was introduced only in macOS 10.6+, so it
   * will be NULL on earlier OS versions. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  typeof(CFPropertyListCreateFromXMLData) *CFPropertyListCreateFromXMLDataFunc =
      (typeof(CFPropertyListCreateFromXMLData) *)dlsym(
          RTLD_DEFAULT, "CFPropertyListCreateFromXMLData");
#pragma clang diagnostic pop
  /* CFPropertyListCreateFromXMLDataFunc is deprecated in macOS 10.10, so it
   * might be NULL in future OS versions. */
  if (!CFPropertyListCreateWithDataFunc && !CFPropertyListCreateFromXMLDataFunc)
    return;
  typeof(CFStringCreateWithCStringNoCopy) *CFStringCreateWithCStringNoCopyFunc =
      (typeof(CFStringCreateWithCStringNoCopy) *)dlsym(
          RTLD_DEFAULT, "CFStringCreateWithCStringNoCopy");
  if (!CFStringCreateWithCStringNoCopyFunc)
    return;
  typeof(CFDictionaryGetValue) *CFDictionaryGetValueFunc =
      (typeof(CFDictionaryGetValue) *)dlsym(RTLD_DEFAULT,
                                            "CFDictionaryGetValue");
  if (!CFDictionaryGetValueFunc)
    return;
  typeof(CFGetTypeID) *CFGetTypeIDFunc =
      (typeof(CFGetTypeID) *)dlsym(RTLD_DEFAULT, "CFGetTypeID");
  if (!CFGetTypeIDFunc)
    return;
  typeof(CFStringGetTypeID) *CFStringGetTypeIDFunc =
      (typeof(CFStringGetTypeID) *)dlsym(RTLD_DEFAULT, "CFStringGetTypeID");
  if (!CFStringGetTypeIDFunc)
    return;
  typeof(CFStringGetCString) *CFStringGetCStringFunc =
      (typeof(CFStringGetCString) *)dlsym(RTLD_DEFAULT, "CFStringGetCString");
  if (!CFStringGetCStringFunc)
    return;
  typeof(CFRelease) *CFReleaseFunc =
      (typeof(CFRelease) *)dlsym(RTLD_DEFAULT, "CFRelease");
  if (!CFReleaseFunc)
    return;

  char *PListPath = "/System/Library/CoreServices/SystemVersion.plist";

#if defined(TARGET_OS_SIMULATOR) && TARGET_OS_SIMULATOR
  char *PListPathPrefix = getenv("IPHONE_SIMULATOR_ROOT");
  if (!PListPathPrefix)
    return;
  char FullPath[strlen(PListPathPrefix) + strlen(PListPath) + 1];
  strcpy(FullPath, PListPathPrefix);
  strcat(FullPath, PListPath);
  PListPath = FullPath;
#endif
  FILE *PropertyList = fopen(PListPath, "r");
  if (!PropertyList)
    return;

  /* Dynamically allocated stuff. */
  CFDictionaryRef PListRef = NULL;
  CFDataRef FileContentsRef = NULL;
  UInt8 *PListBuf = NULL;

  fseek(PropertyList, 0, SEEK_END);
  long PListFileSize = ftell(PropertyList);
  if (PListFileSize < 0)
    goto Fail;
  rewind(PropertyList);

  PListBuf = malloc((size_t)PListFileSize);
  if (!PListBuf)
    goto Fail;

  size_t NumRead = fread(PListBuf, 1, (size_t)PListFileSize, PropertyList);
  if (NumRead != (size_t)PListFileSize)
    goto Fail;

  /* Get the file buffer into CF's format. We pass in a null allocator here *
   * because we free PListBuf ourselves */
  FileContentsRef = (*CFDataCreateWithBytesNoCopyFunc)(
      NULL, PListBuf, (CFIndex)NumRead, kCFAllocatorNull);
  if (!FileContentsRef)
    goto Fail;

  if (CFPropertyListCreateWithDataFunc)
    PListRef = (*CFPropertyListCreateWithDataFunc)(
        NULL, FileContentsRef, kCFPropertyListImmutable, NULL, NULL);
  else
    PListRef = (*CFPropertyListCreateFromXMLDataFunc)(
        NULL, FileContentsRef, kCFPropertyListImmutable, NULL);
  if (!PListRef)
    goto Fail;

  CFStringRef ProductVersion = (*CFStringCreateWithCStringNoCopyFunc)(
      NULL, "ProductVersion", kCFStringEncodingASCII, kCFAllocatorNull);
  if (!ProductVersion)
    goto Fail;
  CFTypeRef OpaqueValue = (*CFDictionaryGetValueFunc)(PListRef, ProductVersion);
  (*CFReleaseFunc)(ProductVersion);
  if (!OpaqueValue ||
      (*CFGetTypeIDFunc)(OpaqueValue) != (*CFStringGetTypeIDFunc)())
    goto Fail;

  char VersionStr[32];
  if (!(*CFStringGetCStringFunc)((CFStringRef)OpaqueValue, VersionStr,
                                 sizeof(VersionStr), kCFStringEncodingUTF8))
    goto Fail;
  sscanf(VersionStr, "%d.%d.%d", &GlobalMajor, &GlobalMinor, &GlobalSubminor);

Fail:
  if (PListRef)
    (*CFReleaseFunc)(PListRef);
  if (FileContentsRef)
    (*CFReleaseFunc)(FileContentsRef);
  free(PListBuf);
  fclose(PropertyList);
}

int32_t isMacOSVersionAtLeast(int32_t Major, int32_t Minor, int32_t Subminor)
{
  /* Populate the global version variables, if they haven't already. */
  if (GlobalMajor < 10) {
	  parseSystemVersionPList();
  }

  if (Major < GlobalMajor) return 1;
  if (Major > GlobalMajor) return 0;
  if (Minor < GlobalMinor) return 1;
  if (Minor > GlobalMinor) return 0;
  return Subminor <= GlobalSubminor;
}

#else

#include <stdint.h>

int32_t isMacOSVersionAtLeast(int32_t Major, int32_t Minor, int32_t Subminor)
{
	return 0;
}
#endif
