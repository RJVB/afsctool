#include "afsctool.h"

#include <CoreFoundation/CoreFoundation.h>

int main (int argc, const char * argv[])
{
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    if (mainBundle) {
        // get the application's Info Dictionary. For app bundles this would live in the bundle's Info.plist,
        // for regular executables it is obtained in another way.
        CFMutableDictionaryRef infoDict = (CFMutableDictionaryRef) CFBundleGetInfoDictionary(mainBundle);
        if (infoDict) {
            CFDictionarySetValue(infoDict, CFSTR("CFBundleIdentifier"), CFSTR("org.brkirch.afsctool"));
            CFDictionarySetValue(infoDict, CFSTR("CFBundleName"), CFSTR("AFSCtool"));
            CFDictionarySetValue(infoDict, CFSTR("CFBundleDisplayName"), CFSTR("AFSCtool"));
            CFDictionarySetValue(infoDict, CFSTR("NSAppSleepDisabled"), CFSTR("1"));
            CFDictionarySetValue(infoDict, CFSTR("NSSupportsAutomaticTermination"), CFSTR("0"));
        }
    }
    return afsctool(argc, argv);
}
