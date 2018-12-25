// kate: auto-insert-doxygen true; backspace-indents true; indent-width 4; keep-extra-spaces true; replace-tabs false; tab-indents true; tab-width 4;
/*
 * @file zfsctool.c
 * Copyright 2018 René J.V. Bertin
 * This code is made available under No License At All
 */

#ifndef __APPLE__
#define __USE_BSD
#ifndef _BSD_SOURCE
#	define _BSD_SOURCE
#endif
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <libgen.h>
#include <signal.h>

#include <sys/mman.h>

#ifdef __APPLE__
#	include <sys/attr.h>
#else
#include <bsd/stdlib.h>
#include <endian.h>
#include <sys/vfs.h>
#include <sys/stat.h>
#include <fcntl.h>
#define O_EXLOCK 0
#define MAP_NOCACHE 0
#endif

#include "zfsctool.h"
#include "ParallelProcess.h"
#include "ParallelProcess_p.h"

#include <sstream>
#include <algorithm>
#include <iterator>
#include <vector>

static ParallelFileProcessor *PP = NULL;
static bool exclusive_io = true;
#include "afsctool_fullversion.h"

#define xfree(x)		if((x)){free((void*)(x)); (x)=NULL;}
#define xclose(x)		if((x)!=-1){close((x)); (x)=-1;}
#define xmunmap(x,s)	if((x)){munmap((x),(s)); (x)=NULL;}

// use a hard-coded count so all arrays are always sized equally (and the compiler can warn better)
const int sizeunits = 6;
const char *sizeunit10_short[sizeunits] = {"KB", "MB", "GB", "TB", "PB", "EB"};
const char *sizeunit10_long[sizeunits] = {"kilobytes", "megabytes", "gigabytes", "terabytes", "petabytes", "exabytes"};
const long long int sizeunit10[sizeunits] = {1000, 1000 * 1000, 1000 * 1000 * 1000, (long long int) 1000 * 1000 * 1000 * 1000,
											 (long long int) 1000 * 1000 * 1000 * 1000 * 1000, (long long int) 1000 * 1000 * 1000 * 1000 * 1000 * 1000
											};
const char *sizeunit2_short[sizeunits] = {"KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
const char *sizeunit2_long[sizeunits] = {"kibibytes", "mebibytes", "gibibytes", "tebibytes", "pebibytes", "exbibytes"};
const long long int sizeunit2[sizeunits] = {1024, 1024 * 1024, 1024 * 1024 * 1024, (long long int) 1024 * 1024 * 1024 * 1024,
											(long long int) 1024 * 1024 * 1024 * 1024 * 1024, (long long int) 1024 * 1024 * 1024 * 1024 * 1024 * 1024
										   };

int printVerbose = 0;
static size_t maxOutBufSize = 0;
void printFileInfo(const char *filepath, struct stat *fileinfo, bool appliedcomp);

#if !__has_builtin(__builtin_available)
#	warning "Please use clang 5 or newer if you can"
// determine the Darwin major version number
static int darwinMajor = 0;
#endif

int ipcPipes[2] = {-1, -1};
char ipcPipeWriteEnd[64];

char *getSizeStr(long long int size, long long int size_rounded, int likeFinder)
{
	static char sizeStr[128];
	static int len = sizeof(sizeStr) / sizeof(char);
	int unit2, unit10;

	for (unit2 = 0; unit2 + 1 < sizeunits && (size_rounded / sizeunit2[unit2 + 1]) > 0; unit2++);
	for (unit10 = 0; unit10 + 1 < sizeunits && (size_rounded / sizeunit10[unit10 + 1]) > 0; unit10++);

	snprintf(sizeStr, len, "%lld bytes", size);

#ifdef PRINT_SI_SIZES
	int print_si_sizes = 1;
#else
	int print_si_sizes = likeFinder;
#endif
	if (print_si_sizes) {
		// the Finder will happily print "0 bytes on disk" so here we don't bother
		// determining if the human-readable value is > 0.
		switch (unit10) {
			case 0:
				snprintf(sizeStr, len, "%s / %0.0f %s (%s, base-10)", sizeStr,
						 (double) size_rounded / sizeunit10[unit10], sizeunit10_short[unit10], sizeunit10_long[unit10]);
				break;
			case 1:
				snprintf(sizeStr, len, "%s / %.12g %s (%s, base-10)", sizeStr,
						 (double)(((long long int)((double) size_rounded / sizeunit10[unit10] * 100) + 5) / 10) / 10,
						 sizeunit10_short[unit10], sizeunit10_long[unit10]);
				break;
			default:
				snprintf(sizeStr, len, "%s / %0.12g %s (%s, base-10)", sizeStr,
						 (double)(((long long int)((double) size_rounded / sizeunit10[unit10] * 1000) + 5) / 10) / 100,
						 sizeunit10_short[unit10], sizeunit10_long[unit10]);
				break;
		}
	}
	if (!likeFinder) {
		double humanReadable;
		switch (unit2) {
			case 0:
				// this should actually be the only case were we'd need
				// to check if the human readable value is sensical...
				humanReadable = (double) size_rounded / sizeunit2[unit2];
				if (humanReadable >= 1) {
					snprintf(sizeStr, len, "%s / %0.0f %s", sizeStr,
							 humanReadable, sizeunit2_short[unit2]);
				}
				break;
			case 1:
				humanReadable = (double)(((long long int)((double) size_rounded / sizeunit2[unit2] * 100) + 5) / 10) / 10;
				if (humanReadable > 0) {
					snprintf(sizeStr, len, "%s / %.12g %s", sizeStr,
							 humanReadable, sizeunit2_short[unit2]);
				}
				break;
			default:
				humanReadable = (double)(((long long int)((double) size_rounded / sizeunit2[unit2] * 1000) + 5) / 10) / 100;
				if (humanReadable > 0) {
					snprintf(sizeStr, len, "%s / %0.12g %s", sizeStr,
							 humanReadable, sizeunit2_short[unit2]);
				}
				break;
		}
	}

	return sizeStr;
}

long long int roundToBlkSize(long long int size, struct stat *fileinfo)
{
	if (size <= 0) {
		return size;
	} else if (size < fileinfo->st_blksize) {
// 		fprintf( stderr, "size=%lld -> blksize %d\n", size, fileinfo->st_blksize );
		return fileinfo->st_blksize;
	} else {
		// round up to the next multiple of st_blksize:
		long long int remainder = size % fileinfo->st_blksize;
// 		fprintf( stderr, "size=%lld -> multiple of blksize %d: %lld (%g ; %d)\n", size,
// 				 fileinfo->st_blksize, (remainder != 0) ? size + (fileinfo->st_blksize - remainder) : size,
// 				 (double) ((remainder != 0) ? size + (fileinfo->st_blksize - remainder) : size) / fileinfo->st_blocks,
// 				 fileinfo->st_blksize);
		return (remainder != 0) ? size + (fileinfo->st_blksize - remainder) : size;
	}
}

static bool quitRequested = FALSE;

static void signal_handler(int sig)
{
	fprintf(stderr, "Received signal %d: zfsctool will quit\n", sig);
	stopParallelProcessor(PP);
}

class ZFSDataSetCompressionInfo : public iZFSDataSetCompressionInfo
{
public:
	ZFSDataSetCompressionInfo(const char *name, const char *compression)
		: iZFSDataSetCompressionInfo(name, compression)
		, initialCompression(compression)
		, currentCompression(compression)
	{
		fprintf(stderr, "dataset '%s' has compression '%s'\n",
				name, compression);
	}
	ZFSDataSetCompressionInfo(std::vector<std::string>props)
		: ZFSDataSetCompressionInfo(props[0].c_str(), props[1].c_str())
	{}

	virtual ~ZFSDataSetCompressionInfo()
	{
		// do something relevant here.
		resetCompression();
	}

	bool setCompression(const std::string &newComp)
	{
		bool ret = false;
		if (currentCompression != newComp) {
			const std::string command = "zfs set compression=" + newComp + " " + *this;
			fprintf(stderr, "%s\n", command.c_str());
			// do something like
			// system(command.c_str());
			// if success:
			currentCompression = newComp;
			ret = true;
		}
		return ret;
	}
	bool setCompression(const char *compression)
	{
		const std::string newComp = compression;
		return setCompression(newComp);
	}
	bool resetCompression()
	{
		return setCompression(initialCompression);
	}

	std::string initialCompression, currentCompression;
	// we'll probably need something like a semaphore to keep track when the compression can be reset
	// (= when no more workers are rewriting files on the dataset)
};

// from http://www.martinbroadhurst.com/how-to-split-a-string-in-c.html:
template <class Container>
void split(const std::string &str, Container &cont)
{
	std::istringstream iss(str);
	std::copy(std::istream_iterator<std::string>(iss),
		std::istream_iterator<std::string>(),
		std::back_inserter(cont));
}

class ZFSCommandEngine : public Thread
{
public:
	ZFSCommandEngine(std::string command, size_t outputLen)
		: theCommand(command)
		, bufLen(outputLen)
	{}
	~ZFSCommandEngine()
	{
		delete[] buf;
	}

	std::string getOutput()
	{
		return readlen > 0? buf : std::string();
	}

	std::string& command()
	{
		return theCommand;
	}

protected:
	DWORD Run(LPVOID arg)
	{
		CRITSECTLOCK::Scope safe(lock);
		buf = new char[bufLen];
		std::string c = theCommand + " 1>&" + ipcPipeWriteEnd + " &";
		int ret = system(c.c_str());
		readlen = ret == 0? read(ipcPipes[0], buf, bufLen) : -1;
		if (readlen > 1 && buf[readlen-1] == '\n') {
			buf[readlen-1] = '\0';
			readlen -= 1;
		}
		error = errno;
		return ret;
	}
	std::string theCommand;
	static CRITSECTLOCK lock;
	char *buf = nullptr;
	size_t bufLen;
	int readlen = -1;
public:
	int error;
};
CRITSECTLOCK ZFSCommandEngine::lock(4000);

bool fileIsCompressable(const char *inFile, struct stat *inFileInfo, ParallelFileProcessor *PP = nullptr)
{
	struct statfs fsInfo;
	errno = 0;
	int ret = statfs(inFile, &fsInfo);
	bool retval = false;
	// TODO
	// this function needs to call `zfs list $inFile` to see if the file is on a ZFS dataset
	// if the same info isn't available via fsInfo.
	if (ret >= 0 && S_ISREG(inFileInfo->st_mode)) {
		if (PP && PP->z_dataSetForFile(inFile)) {
			// file already has a dataset property: OK to compress
			retval = true;
		} else {
			std::string fName;
			if (inFile[0] != '/') {
				const char *cwd = getcwd(NULL, 0);
				if (!cwd) {
					fprintf(stderr, "skipping '%s' because cannot determine $PWD (%s)\n",
							inFile, strerror(errno));
					return false;
				}
				fName = std::string(cwd) + "/" + inFile;
				xfree(cwd);
			} else {
				fName = inFile;
			}
			// obtain the dataset name by querying the 'name' property on the file. The
			// current zfs driver command will return a name if the path begins with a
			// valid dataset mountpoint, or else return an error;
			std::string dataSetName;
			auto worker = ZFSCommandEngine("zfs list -H -o name,compression " + fName, MAXPATHLEN);
			if (worker.Start() == 0) {
				if (int exitval = worker.Join()) {
					fprintf(stderr, "\t`%s` returned %d (%s)\n", worker.command().c_str(), exitval, strerror(worker.error));
				} else {
					if (worker.getOutput().size() > 0) {
						dataSetName = worker.getOutput();
					} else {
						// terse error because pclose() will probably generate an error
						fprintf(stderr, "Skipping '%s' because cannot obtain its dataset name\n", inFile);
					}
				}
			} else {
				fprintf(stderr, "Skipping '%s' because cannot obtain its dataset name; `%s` failed to start (%s)\n",
						inFile, worker.command().c_str(), strerror(errno));
			}
			if (!dataSetName.empty()) {
				// dataSetName will now contain something like "name\tcompression";
				// split that:
				std::vector<std::string> properties;
				split(dataSetName, properties);
				if (properties.size() == 2) {
					auto knownDataSet = PP->z_dataSet(properties[0]);
					PP->z_addDataSet(inFile, knownDataSet? knownDataSet : new ZFSDataSetCompressionInfo(properties));
					retval = true;
				} else {
					fprintf(stderr, "Skipping '%s' because '%s' parses to %lu items\n",
							inFile, dataSetName.c_str(), properties.size());
				}
			}
		}
	}
	return retval;
}

/*! Mac OS X basename() can modify the input string when not in 'legacy' mode on 10.6
 * and indeed it does. So we use our own which doesn't, and also doesn't require internal
 * storage.
 */
static const char *lbasename(const char *url)
{
	const char *c = NULL;
	if (url) {
		if ((c =  strrchr(url, '/'))) {
			c++;
		} else {
			c = url;
		}
	}
	return c;
}

const char *compressionTypeName(int type)
{
	char *name = (char*) "";
#ifndef STR
#define STR(v)		#v
#endif
#define STRVAL(v)	STR(v)
	return name;
}

void compressFile(const char *inFile, struct stat *inFileInfo, struct folder_info *folderinfo, FileProcessor *worker)
{
	long long int maxSize = folderinfo->maxSize;
	bool checkFiles = folderinfo->check_files;
	bool backupFile = folderinfo->backup_file;

	void *inBuf = NULL, *outBuf = NULL;
	const long long int filesize = inFileInfo->st_size;
	mode_t orig_mode;
	struct timeval times[2];
	char *backupName = NULL;
	bool useMmap = false;

	if (quitRequested) {
		return;
	}

#if defined(__APPLE__)
	times[0].tv_sec = inFileInfo->st_atimespec.tv_sec;
	times[0].tv_usec = inFileInfo->st_atimespec.tv_nsec / 1000;
	times[1].tv_sec = inFileInfo->st_mtimespec.tv_sec;
	times[1].tv_usec = inFileInfo->st_mtimespec.tv_nsec / 1000;
#elif defined(linux)
	times[0].tv_sec = inFileInfo->st_atim.tv_sec;
	times[0].tv_usec = inFileInfo->st_atim.tv_nsec / 1000;
	times[1].tv_sec = inFileInfo->st_mtim.tv_sec;
	times[1].tv_usec = inFileInfo->st_mtim.tv_nsec / 1000;
#endif

	if (!fileIsCompressable(inFile, inFileInfo, worker? worker->controller() : nullptr)) {
		return;
	}
	if (filesize > maxSize && maxSize != 0) {
		if (folderinfo->print_info > 2) {
			fprintf(stderr, "Skipping file %s size %lld > max size %lld\n", inFile, filesize, maxSize);
		}
		return;
	}
	if (filesize == 0) {
		if (folderinfo->print_info > 2) {
			fprintf(stderr, "Skipping empty file %s\n", inFile);
		}
		return;
	}
	orig_mode = inFileInfo->st_mode;
#ifdef WE_ARE_FUNCTIONAL
	if ((orig_mode & S_IWUSR) == 0) {
		chmod(inFile, orig_mode | S_IWUSR);
		lstat(inFile, inFileInfo);
	}
	if ((orig_mode & S_IRUSR) == 0) {
		chmod(inFile, orig_mode | S_IRUSR);
		lstat(inFile, inFileInfo);
	}
#endif

#if !defined(NO_USE_MMAP)
	useMmap = true;
#endif

	bool locked = false;
	// use open() with an exclusive lock so noone can modify the file while we're at it
	int fdIn = open(inFile, O_RDWR | O_EXLOCK);
	if (fdIn == -1) {
		fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
		goto bail;
	}
#ifndef NO_USE_MMAP
	if (useMmap) {
		// get a private mmap. We rewrite to the file's attributes and/or resource fork,
		// so there is no point in using a shared mapping where changes to the memory
		// are mapped back to disk. The use of NOCACHE is experimental; if I understand
		// the documentation correctly this just means that released memory can be
		// reused more easily.
		inBuf = mmap(NULL, filesize, PROT_READ, MAP_PRIVATE | MAP_NOCACHE, fdIn, 0);
		if (inBuf == MAP_FAILED) {
			fprintf(stderr, "%s: Error m'mapping file (size %lld; %s)\n", inFile, filesize, strerror(errno));
			useMmap = false;
		} else {
			madvise(inBuf, filesize, MADV_RANDOM);
		}
	}
	if (!useMmap)
#endif
	{
		inBuf = malloc(filesize);
		if (inBuf == NULL) {
			fprintf(stderr, "%s: malloc error, unable to allocate input buffer of %lld bytes (%s)\n", inFile, filesize, strerror(errno));
			xclose(fdIn);
			utimes(inFile, times);
			return;
		}
		madvise(inBuf, filesize, MADV_RANDOM);
		if (read(fdIn, inBuf, filesize) != filesize) {
			fprintf(stderr, "%s: Error reading file (%s)\n", inFile, strerror(errno));
			xclose(fdIn);
			utimes(inFile, times);
			free(inBuf);
			return;
		}
	}
	// keep our filedescriptor open to maintain the lock!
	if (backupFile) {
		int fd, bkNameLen;
		FILE *fp;
		char *infile, *inname = NULL;
		if ((infile = strdup(inFile))) {
			inname = (char *) lbasename(infile);
			// avoid filename overflow; assume 32 fixed template char for mkstemps
			// just to be on the safe side (even in parallel mode).
			if (strlen(inname) > 1024 - 32) {
				// truncate
				inname[1024 - 32] = '\0';
			}
			// add the processor ID for the unlikely case that 2 threads try to backup a file with the same name
			// at the same time, and mkstemps() somehow generates the same temp. name. I've seen it generate EEXIST
			// errors which suggest that might indeed happen.
			bkNameLen = asprintf(&backupName, "/tmp/afsctbk.%d.XXXXXX.%s", currentParallelProcessorID(worker), inname);
		}
		if (!infile || bkNameLen < 0) {
			fprintf(stderr, "%s: malloc error, unable to generate temporary backup filename (%s)\n", inFile, strerror(errno));
			xfree(infile);
			goto bail;
		}
		if ((fd = mkstemps(backupName, strlen(inname) + 1)) < 0 || !(fp = fdopen(fd, "w"))) {
			fprintf(stderr, "%s: error creating temporary backup file %s (%s)\n", inFile, backupName, strerror(errno));
			xfree(infile);
			goto bail;
		}
		xfree(infile);
		if (fwrite(inBuf, filesize, 1, fp) != 1) {
			fprintf(stderr, "%s: Error writing to backup file %s (%lld bytes; %s)\n", inFile, backupName, filesize, strerror(errno));
			fclose(fp);
			goto bail;
		}
		fclose(fp);
		utimes(backupName, times);
		chmod(backupName, orig_mode);
	}

	if (exclusive_io && worker) {
		locked = worker->lockScope();
	}

#ifdef WE_ARE_FUNCTIONAL
	// fdIn is still open
	ftruncate(fdIn, 0);
	lseek(fdIn, SEEK_SET, 0);

	if (write(fdIn, inBuf, filesize) != filesize) {
		fprintf(stderr, "%s: Error writing to file (%lld bytes; %s)\n", inFile, filesize, strerror(errno));
		if (backupName) {
			fprintf(stderr, "\ta backup is available as %s\n", backupName);
			xfree(backupName);
		}
		xclose(fdIn)
		goto bail;
	}
#endif

// 	fsync(fdIn);
	xclose(fdIn);
	if (checkFiles) {
		lstat(inFile, inFileInfo);
		bool sizeMismatch = inFileInfo->st_size != filesize, readFailure = false, contentMismatch = false;
		ssize_t checkRead = -2;
		bool outBufMMapped = false;
		errno = 0;
		fdIn = open(inFile, O_RDONLY | O_EXLOCK);
		if (fdIn == -1) {
			fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
			// we don't bail here, we fail (= restore the backup).
			goto fail;
		}
		if (!sizeMismatch) {
#ifndef NO_USE_MMAP
			xfree(outBuf);
			outBuf = mmap(NULL, filesize, PROT_READ, MAP_PRIVATE | MAP_NOCACHE, fdIn, 0);
			outBufMMapped = true;
#else
			outBuf = reallocf(outBuf, filesize);
#endif
			if (!outBuf) {
				xclose(fdIn);
				fprintf(stderr, "%s: failure reallocating buffer for validation; %s\n", inFile, strerror(errno));
				goto fail;
			}
			// this should be appropriate for simply reading into and comparing:
			madvise(inBuf, filesize, MADV_SEQUENTIAL);
			madvise(outBuf, filesize, MADV_SEQUENTIAL);
			if (!outBufMMapped) {
				errno = 0;
				readFailure = (checkRead = read(fdIn, outBuf, filesize)) != filesize;
			} else {
				readFailure = false;
				checkRead = filesize;
			}
		}
		xclose(fdIn);
		if (sizeMismatch || readFailure
				|| (contentMismatch = memcmp(outBuf, inBuf, filesize) != 0)) {
			fprintf(stderr, "\tsize mismatch=%d read=%zd failure=%d content mismatch=%d (%s)\n",
					sizeMismatch, checkRead, readFailure, contentMismatch, strerror(errno));
fail:
			;
			printf("%s: Compressed file check failed, reverting file changes\n", inFile);
			if (outBufMMapped) {
				xmunmap(outBuf, filesize);
			}
			if (backupName) {
				fprintf(stderr, "\tin case of further failures, a backup will be available as %s\n", backupName);
			}
			FILE *in = fopen(inFile, "w");
			if (in == NULL) {
				fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
				xfree(backupName);
				goto bail;
			}
			if (fwrite(inBuf, filesize, 1, in) != 1) {
				fprintf(stderr, "%s: Error writing to file (%lld bytes; %s)\n", inFile, filesize, strerror(errno));
				xfree(backupName);
				goto bail;
			}
			fclose(in);
		}
		if (outBufMMapped) {
			xmunmap(outBuf, filesize);
		}
	}
// #ifndef NO_USE_MMAP
// 	{
// 		char *tFileName = NULL;
// 		asprintf(&tFileName, "%s.%s", inFile, ".RestoreTest");
// 		int fdTest = open(tFileName, O_WRONLY|O_CREAT|O_EXCL);
// 		if (fdTest != -1) {
// 			if (write(fdTest, inBuf, filesize) != filesize) {
// 				fprintf(stderr, "%s: Error writing to testfile %s (%lld bytes; %s)\n", inFile, tFileName, filesize, strerror(errno));
// 			}
// 			close(fdTest);
// 			chmod(tFileName, orig_mode);
// 		}
// 		xfree(tFileName)
// 	}
// #endif
bail:
#ifdef WE_ARE_FUNCTIONAL
	utimes(inFile, times);
	if (inFileInfo->st_mode != orig_mode) {
		chmod(inFile, orig_mode);
	}
#endif
	if (worker && locked) {
		locked = worker->unLockScope();
	}
	xclose(fdIn);
	if (backupName) {
		// a backupName is set and hasn't been unset because of a processing failure:
		// remove the file now.
		unlink(backupName);
		free(backupName);
		backupName = NULL;
	}
#ifndef NO_USE_MMAP
	if (useMmap) {
		xmunmap(inBuf, filesize);
	} else
#endif
	{
		xfree(inBuf);
	}
	xfree(outBuf);
}

bool checkForHardLink(const char *filepath, const struct stat *fileInfo, const struct folder_info *folderinfo)
{
	static ino_t *hardLinks = NULL;
	static char **paths = NULL, *list_item;
	static long int currSize = 0, numLinks = 0;
	long int right_pos, left_pos = 0, curr_pos = 1;

	if (fileInfo != NULL && fileInfo->st_nlink > 1) {
		if (hardLinks == NULL) {
			currSize = 1;
			hardLinks = (ino_t *) malloc(currSize * sizeof(ino_t));
			if (hardLinks == NULL) {
				fprintf(stderr, "Malloc error allocating memory for list of file hard links, exiting...\n");
				exit(ENOMEM);
			}
			paths = (char **) malloc(currSize * sizeof(char *));
			if (paths == NULL) {
				fprintf(stderr, "Malloc error allocating memory for list of file hard links, exiting...\n");
				exit(ENOMEM);
			}
		}

		if (numLinks > 0) {
			left_pos = 0;
			right_pos = numLinks + 1;

			while (hardLinks[curr_pos - 1] != fileInfo->st_ino) {
				curr_pos = (right_pos - left_pos) / 2;
				if (curr_pos == 0) break;
				curr_pos += left_pos;
				if (hardLinks[curr_pos - 1] > fileInfo->st_ino)
					right_pos = curr_pos;
				else if (hardLinks[curr_pos - 1] < fileInfo->st_ino)
					left_pos = curr_pos;
			}
			if (curr_pos != 0 && hardLinks[curr_pos - 1] == fileInfo->st_ino) {
				if (strcmp(filepath, paths[curr_pos - 1]) != 0 || strlen(filepath) != strlen(paths[curr_pos - 1])) {
					if (folderinfo->print_info > 1)
						printf("%s: skipping, hard link to this %s exists at %s\n", filepath, (fileInfo->st_mode & S_IFDIR) ? "folder" : "file", paths[curr_pos - 1]);
					return TRUE;
				} else
					return FALSE;
			}
		}
		if (currSize < numLinks + 1) {
			currSize *= 2;
			hardLinks = (ino_t *) realloc(hardLinks, currSize * sizeof(ino_t));
			if (hardLinks == NULL) {
				fprintf(stderr, "Malloc error allocating memory for list of file hard links, exiting...\n");
				exit(ENOMEM);
			}
			paths = (char **) realloc(paths, currSize * sizeof(char *));
			if (paths == NULL) {
				fprintf(stderr, "Malloc error allocating memory for list of file hard links, exiting...\n");
				exit(ENOMEM);
			}
		}
		if ((numLinks != 0) && ((numLinks - 1) >= left_pos)) {
			memmove(&hardLinks[left_pos + 1], &hardLinks[left_pos], (numLinks - left_pos) * sizeof(ino_t));
			if (paths != NULL)
				memmove(&paths[left_pos + 1], &paths[left_pos], (numLinks - left_pos) * sizeof(char *));
		}
		hardLinks[left_pos] = fileInfo->st_ino;
		list_item = (char *) malloc(strlen(filepath) + 1);
		strcpy(list_item, filepath);
		paths[left_pos] = list_item;
		numLinks++;
	} else if (fileInfo == NULL && hardLinks != NULL) {
		free(hardLinks);
		hardLinks = NULL;
		currSize = 0;
		numLinks = 0;
		if (paths != NULL) {
			for (curr_pos = 0; curr_pos < numLinks; curr_pos++)
				free(paths[curr_pos]);
			free(paths);
		}
	}
	return FALSE;
}

#if 0
void add_extension_to_filetypeinfo(const char *filepath, struct filetype_info *filetypeinfo)
{
	long int right_pos, left_pos = 0, curr_pos = 1, i, fileextensionlen;
	const char *fileextension;

	for (i = strlen(filepath) - 1; i > 0; i--)
		if (filepath[i] == '.' || filepath[i] == '/')
			break;
	if (i != 0 && i != strlen(filepath) - 1 && filepath[i] != '/' && filepath[i - 1] != '/')
		fileextension = &filepath[i + 1];
	else
		return;

	if (filetypeinfo->extensions == NULL) {
		filetypeinfo->extensionssize = 1;
		filetypeinfo->extensions = (char **) malloc(filetypeinfo->extensionssize * sizeof(char *));
		if (filetypeinfo->extensions == NULL) {
			fprintf(stderr, "Malloc error allocating memory for list of file types, exiting...\n");
			exit(ENOMEM);
		}
	}

	if (filetypeinfo->numextensions > 0) {
		left_pos = 0;
		right_pos = filetypeinfo->numextensions + 1;

		while (strcasecmp(filetypeinfo->extensions[curr_pos - 1], fileextension) != 0) {
			curr_pos = (right_pos - left_pos) / 2;
			if (curr_pos == 0) break;
			curr_pos += left_pos;
			if (strcasecmp(filetypeinfo->extensions[curr_pos - 1], fileextension) > 0)
				right_pos = curr_pos;
			else if (strcasecmp(filetypeinfo->extensions[curr_pos - 1], fileextension) < 0)
				left_pos = curr_pos;
		}
		if (curr_pos != 0 && strcasecmp(filetypeinfo->extensions[curr_pos - 1], fileextension) == 0)
			return;
	}
	if (filetypeinfo->extensionssize < filetypeinfo->numextensions + 1) {
		filetypeinfo->extensionssize *= 2;
		filetypeinfo->extensions = (char **) realloc(filetypeinfo->extensions, filetypeinfo->extensionssize * sizeof(char *));
		if (filetypeinfo->extensions == NULL) {
			fprintf(stderr, "Malloc error allocating memory for list of file types, exiting...\n");
			exit(ENOMEM);
		}
	}
	if ((filetypeinfo->numextensions != 0) && ((filetypeinfo->numextensions - 1) >= left_pos))
		memmove(&filetypeinfo->extensions[left_pos + 1], &filetypeinfo->extensions[left_pos], (filetypeinfo->numextensions - left_pos) * sizeof(char *));
	filetypeinfo->extensions[left_pos] = (char *) malloc(strlen(fileextension) + 1);
	strcpy(filetypeinfo->extensions[left_pos], fileextension);
	for (fileextensionlen = strlen(fileextension), i = 0; i < fileextensionlen; i++)
		filetypeinfo->extensions[left_pos][i] = tolower(filetypeinfo->extensions[left_pos][i]);
	filetypeinfo->numextensions++;
}

struct filetype_info *getFileTypeInfo(const char *filepath, const char *filetype, struct folder_info *folderinfo)
{
	long int right_pos, left_pos = 0, curr_pos = 1;

	if (filetype == NULL)
		return NULL;

	if (folderinfo->filetypes == NULL) {
		folderinfo->filetypessize = 1;
		folderinfo->filetypes = (struct filetype_info *) malloc(folderinfo->filetypessize * sizeof(struct filetype_info));
		if (folderinfo->filetypes == NULL) {
			fprintf(stderr, "Malloc error allocating memory for list of file types, exiting...\n");
			exit(ENOMEM);
		}
	}

	if (folderinfo->numfiletypes > 0) {
		left_pos = 0;
		right_pos = folderinfo->numfiletypes + 1;

		while (strcmp(folderinfo->filetypes[curr_pos - 1].filetype, filetype) != 0) {
			curr_pos = (right_pos - left_pos) / 2;
			if (curr_pos == 0) break;
			curr_pos += left_pos;
			if (strcmp(folderinfo->filetypes[curr_pos - 1].filetype, filetype) > 0)
				right_pos = curr_pos;
			else if (strcmp(folderinfo->filetypes[curr_pos - 1].filetype, filetype) < 0)
				left_pos = curr_pos;
		}
		if (curr_pos != 0 && strcmp(folderinfo->filetypes[curr_pos - 1].filetype, filetype) == 0) {
			add_extension_to_filetypeinfo(filepath, &folderinfo->filetypes[curr_pos - 1]);
			return &folderinfo->filetypes[curr_pos - 1];
		}
	}
	if (folderinfo->filetypessize < folderinfo->numfiletypes + 1) {
		folderinfo->filetypessize *= 2;
		folderinfo->filetypes = (struct filetype_info *) realloc(folderinfo->filetypes, folderinfo->filetypessize * sizeof(struct filetype_info));
		if (folderinfo->filetypes == NULL) {
			fprintf(stderr, "Malloc error allocating memory for list of file types, exiting...\n");
			exit(ENOMEM);
		}
	}
	if ((folderinfo->numfiletypes != 0) && ((folderinfo->numfiletypes - 1) >= left_pos))
		memmove(&folderinfo->filetypes[left_pos + 1], &folderinfo->filetypes[left_pos], (folderinfo->numfiletypes - left_pos) * sizeof(struct filetype_info));
	folderinfo->filetypes[left_pos].filetype = (char *) malloc(strlen(filetype) + 1);
	strcpy(folderinfo->filetypes[left_pos].filetype, filetype);
	folderinfo->filetypes[left_pos].extensions = NULL;
	folderinfo->filetypes[left_pos].extensionssize = 0;
	folderinfo->filetypes[left_pos].numextensions = 0;
	folderinfo->filetypes[left_pos].uncompressed_size = 0;
	folderinfo->filetypes[left_pos].uncompressed_size_rounded = 0;
	folderinfo->filetypes[left_pos].compressed_size = 0;
	folderinfo->filetypes[left_pos].compressed_size_rounded = 0;
	folderinfo->filetypes[left_pos].compattr_size = 0;
	folderinfo->filetypes[left_pos].total_size = 0;
	folderinfo->filetypes[left_pos].num_compressed = 0;
	folderinfo->filetypes[left_pos].num_files = 0;
	folderinfo->filetypes[left_pos].num_hard_link_files = 0;
	add_extension_to_filetypeinfo(filepath, &folderinfo->filetypes[left_pos]);
	folderinfo->numfiletypes++;
	return &folderinfo->filetypes[left_pos];
}
#endif //0

void printFileInfo(const char *filepath, struct stat *fileinfo, bool appliedcomp)
{
	long long int filesize, filesize_rounded;

	printf("%s:\n", filepath);

	// compressed==on-disk filesize: fileinfo->st_blocks * (S_BLKSIZE)?S_BLKSIZE:512
#ifdef __APPLE__
	if ((fileinfo->st_flags & UF_COMPRESSED) == 0)
#else
	if (true)
#endif
	{
		if (appliedcomp)
			printf("Unable to compress file.\n");
		else
			printf("File is not compressed.\n");
		filesize = fileinfo->st_size;
		filesize_rounded = roundToBlkSize(filesize, fileinfo);
		printf("File size: %s\n", getSizeStr(filesize, filesize_rounded, 1));
		filesize = roundToBlkSize(fileinfo->st_size, fileinfo);

	} else {
		if (!appliedcomp)
			printf("File is compressed.\n");
		filesize = fileinfo->st_size;
		printf("File size (uncompressed): %s\n", getSizeStr(filesize, filesize, 1));
		// report the actual file-on-disk size
		filesize = fileinfo->st_blocks * S_BLKSIZE;
		filesize_rounded = roundToBlkSize(filesize, fileinfo);
		printf("File size (compressed): %s\n", getSizeStr(filesize, filesize_rounded, 0));
		printf("Compression savings: %0.1f%%\n", (1.0 - (((double) filesize) / fileinfo->st_size)) * 100.0);
	}
}

long long process_file(const char *filepath, const char* /*filetype*/, struct stat *fileinfo, struct folder_info *folderinfo)
{
	long long int filesize, filesize_rounded, ret;

	if (quitRequested) {
		return 0;
	}

	folderinfo->num_files++;

	// compressed==on-disk filesize: fileinfo->st_blocks * (S_BLKSIZE)?S_BLKSIZE:512
#ifdef __APPLE__
	if ((fileinfo->st_flags & UF_COMPRESSED) == 0)
#endif
	{
		ret = filesize_rounded = filesize = fileinfo->st_size;
		filesize_rounded = roundToBlkSize(filesize_rounded, fileinfo);
		folderinfo->uncompressed_size += filesize;
		folderinfo->uncompressed_size_rounded += filesize_rounded;
		folderinfo->compressed_size += filesize;
		folderinfo->compressed_size_rounded += filesize_rounded;
		filesize = roundToBlkSize(fileinfo->st_size, fileinfo);
		folderinfo->total_size += filesize;
	}
#ifdef __APPLE__
	else {
		if (folderinfo->print_files) {
			if (folderinfo->print_info > 1) {
				printf("%s:\n", filepath);
				filesize = fileinfo->st_size;
				printf("File size (uncompressed): %s\n", getSizeStr(filesize, filesize, 1));
				// on-disk file size:
				filesize = fileinfo->st_blocks * S_BLKSIZE;
				printf("Compression savings: %0.1f%%\n", (1.0 - (((double) filesize) / fileinfo->st_size)) * 100.0);
			} else if (!folderinfo->compress_files) {
				printf("%s\n", filepath);
			}
		}

		filesize = fileinfo->st_size;
		filesize_rounded = roundToBlkSize(filesize, fileinfo);
		folderinfo->uncompressed_size += filesize;
		folderinfo->uncompressed_size_rounded += filesize_rounded;
		ret = filesize = fileinfo->st_blocks * S_BLKSIZE;
		filesize_rounded = roundToBlkSize(filesize, fileinfo);
		folderinfo->compressed_size += filesize;
		folderinfo->compressed_size_rounded += filesize_rounded;
		folderinfo->total_size += filesize;
		folderinfo->num_compressed++;
	}
#endif
	return ret;
}

void printFolderInfo(struct folder_info *folderinfo, bool hardLinkCheck)
{
	long long foldersize, foldersize_rounded;

	printf("Total number of files: %lld\n", folderinfo->num_files);
	if (hardLinkCheck)
		printf("Total number of file hard links: %lld\n", folderinfo->num_hard_link_files);
	printf("Total number of folders: %lld\n", folderinfo->num_folders);
	if (hardLinkCheck)
		printf("Total number of folder hard links: %lld\n", folderinfo->num_hard_link_folders);
	printf("Total number of items (number of files + number of folders): %lld\n", folderinfo->num_files + folderinfo->num_folders);
	foldersize = folderinfo->uncompressed_size;
	foldersize_rounded = folderinfo->uncompressed_size_rounded;
	if ((folderinfo->num_hard_link_files == 0 && folderinfo->num_hard_link_folders == 0) || !hardLinkCheck)
		printf("Folder size (uncompressed; reported size by Mac OS 10.6+ Finder): %s\n",
			   getSizeStr(foldersize, foldersize_rounded, 1));
	else
		printf("Folder size (uncompressed): %s\n",
			   getSizeStr(foldersize, foldersize_rounded, 0));
	foldersize = folderinfo->compressed_size;
	foldersize_rounded = folderinfo->compressed_size_rounded;
	printf("Folder size (compressed): %s\n", getSizeStr(foldersize, foldersize_rounded, 0));
	printf("Compression savings: %0.1f%%\n", (1.0 - ((float)(folderinfo->compressed_size) / folderinfo->uncompressed_size)) * 100.0);
	foldersize = folderinfo->total_size;
	printf("Approximate total folder size (files + file overhead + folder overhead): %s\n",
		   getSizeStr(foldersize, foldersize, 0));
}

void process_folder(FTS *currfolder, struct folder_info *folderinfo)
{
	FTSENT *currfile;
	bool volume_search;

	currfile = fts_read(currfolder);
	if (currfile == NULL) {
		fts_close(currfolder);
		return;
	}
	volume_search = (strncasecmp("/Volumes/", currfile->fts_path, 9) == 0 && strlen(currfile->fts_path) >= 8);

	do {
		if (!quitRequested
				&& (volume_search || strncasecmp("/Volumes/", currfile->fts_path, 9) != 0 || strlen(currfile->fts_path) < 9)
				&& (strncasecmp("/dev/", currfile->fts_path, 5) != 0 || strlen(currfile->fts_path) < 5)) {
			if (S_ISDIR(currfile->fts_statp->st_mode) && currfile->fts_ino != 2) {
				if (currfile->fts_info & FTS_D) {
					if (!folderinfo->check_hard_links || !checkForHardLink(currfile->fts_path, currfile->fts_statp, folderinfo)) {
						folderinfo->num_folders++;
					} else {
						folderinfo->num_hard_link_folders++;
						fts_set(currfolder, currfile, FTS_SKIP);

						folderinfo->num_folders++;
					}
				}
			} else if (S_ISREG(currfile->fts_statp->st_mode) || S_ISLNK(currfile->fts_statp->st_mode)) {
				if (!folderinfo->check_hard_links || !checkForHardLink(currfile->fts_path, currfile->fts_statp, folderinfo)) {
					if (folderinfo->compress_files && S_ISREG(currfile->fts_statp->st_mode)) {
						if (PP) {
							if (fileIsCompressable(currfile->fts_path, currfile->fts_statp, PP)) {
								addFileToParallelProcessor(PP, currfile->fts_path, currfile->fts_statp, folderinfo, false);
							} else {
								process_file(currfile->fts_path, NULL, currfile->fts_statp, getParallelProcessorJobInfo(PP));
							}
						} else {
							compressFile(currfile->fts_path, currfile->fts_statp, folderinfo, NULL);
						}
					}
					process_file(currfile->fts_path, NULL, currfile->fts_statp, folderinfo);
				} else {
					folderinfo->num_hard_link_files++;

					folderinfo->num_files++;
				}
			}
		} else
			fts_set(currfolder, currfile, FTS_SKIP);
	} while (!quitRequested && (currfile = fts_read(currfolder)) != NULL);
	checkForHardLink(NULL, NULL, NULL);
	fts_close(currfolder);
}

void printUsage()
{
	printf("zfsctool %s\n"
	   "Apply compression to file or folder: zfsctool -c[nlfvv[v]b] [-jN|-JN] [-S [-RM] ] [-<level>] [-m <size>] [-T compressor] file[s]/folder[s]\n\n"
	   "Options:\n"
	   "-v Increase verbosity level\n"
	   "-f Detect hard links\n"
	   "-l List files which fail to compress\n"
	   "-L Allow larger-than-raw compressed chunks (not recommended; always true for LZVN compression)\n"
	   "-n Do not verify files after compression (not recommended)\n"
	   "-m <size> Largest file size to compress, in bytes\n"
	   "-b make a backup of files before compressing them\n"
	   "-jN compress (only compressable) files using <N> threads (compression is concurrent, disk IO is exclusive)\n"
	   "-JN read, compress and write files (only compressable ones) using <N> threads (everything is concurrent except writing the compressed file)\n"
	   "-S sort the item list by file size (leaving the largest files to the end may be beneficial if the target volume is almost full)\n"
	   "-RM <M> of the <N> workers will work the item list (must be sorted!) in reverse order, starting with the largest files\n"
	   "-T <compressor> Compression type to use, chosen from the supported ZFS compression types\n"
	   , AFSCTOOL_FULL_VERSION_STRING);
}

int zfsctool(int argc, const char *argv[])
{
	int i, j;
	struct stat fileinfo, dstfileinfo;
	struct folder_info folderinfo;
	FTS *currfolder;
	char *folderarray[2], *fullpath = NULL, *fullpathdst = NULL, *cwd;
	int compressionlevel = 5;
	compression_type compressiontype = ZLIB;
	double minSavings = 0.0;
	long long int maxSize = 0;
	bool printDir = FALSE, decomp = FALSE, createfile = FALSE, extractfile = FALSE, applycomp = FALSE,
		 fileCheck = TRUE, argIsFile, hardLinkCheck = FALSE, dstIsFile, free_src = FALSE, free_dst = FALSE,
		 allowLargeBlocks = FALSE, backupFile = FALSE;
	int nJobs = 0, nReverse = 0;
	bool sortQueue = false;

	if (argc < 2) {
		printUsage();
		return(EINVAL);
	}

#if !__has_builtin(__builtin_available)
#	warning "Please use clang 5 or newer if you can"
#endif

	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		for (j = 1; j < strlen(argv[i]); j++) {
			switch (argv[i][j]) {
				case 'l':
					if (createfile || extractfile || decomp) {
						printUsage();
						return(EINVAL);
					}
					printDir = TRUE;
					break;
				case 'L':
					if (createfile || extractfile || decomp) {
						printUsage();
						return(EINVAL);
					}
					allowLargeBlocks = TRUE;
					break;
				case 'v':
					printVerbose++;
					break;
				case 'd':
					if (printDir || applycomp || hardLinkCheck) {
						printUsage();
						return(EINVAL);
					}
					decomp = TRUE;
					break;
				case 'a':
					if (printDir || extractfile || applycomp || hardLinkCheck) {
						printUsage();
						return(EINVAL);
					}
					createfile = TRUE;
					break;
				case 'x':
					if (printDir || createfile || applycomp || hardLinkCheck) {
						printUsage();
						return(EINVAL);
					}
					extractfile = TRUE;
					break;
				case 'c':
					if (createfile || extractfile || decomp) {
						printUsage();
						return(EINVAL);
					}
					applycomp = TRUE;
					break;
				case 'k':
					// this flag is obsolete and no longer does anything, but it is kept here for backward compatibility with scripts that use it
					break;
				case 'n':
					if (createfile || extractfile || decomp) {
						printUsage();
						return(EINVAL);
					}
					fileCheck = FALSE;
					break;
				case 'f':
					if (createfile || extractfile || decomp) {
						printUsage();
						return(EINVAL);
					}
					hardLinkCheck = TRUE;
					break;
				case '1':
				case '2':
				case '3':
				case '4':
				case '5':
				case '6':
				case '7':
				case '8':
				case '9':
					if (createfile || extractfile || decomp) {
						printUsage();
						return(EINVAL);
					}
					compressionlevel = argv[i][j] - '0';
					break;
				case 'm':
					if (createfile || extractfile || decomp || j + 1 < strlen(argv[i]) || i + 2 > argc) {
						printUsage();
						return(EINVAL);
					}
					i++;
					sscanf(argv[i], "%lld", &maxSize);
					j = strlen(argv[i]) - 1;
					break;
				case 's':
					if (createfile || extractfile || decomp || j + 1 < strlen(argv[i]) || i + 2 > argc) {
						printUsage();
						return(EINVAL);
					}
					i++;
					sscanf(argv[i], "%lf", &minSavings);
					if (minSavings > 99 || minSavings < 0) {
						fprintf(stderr, "Invalid minimum savings percentage; must be a number from 0 to 99\n");
						return(EINVAL);
					}
					j = strlen(argv[i]) - 1;
					break;
				case 'T':
					if (j + 1 < strlen(argv[i]) || i + 2 > argc) {
						printUsage();
						return(EINVAL);
					}
					i++;
					{
						fprintf(stderr, "Unsupported or unknown HFS compression requested (%s)\n", argv[i]);
						printUsage();
						return(EINVAL);
					}
					j = strlen(argv[i]) - 1;
					break;
				case 'b':
					if (!applycomp) {
						printUsage();
						return(EINVAL);
					}
					folderinfo.backup_file = backupFile = TRUE;
					break;
				case 'j':
				case 'J':
				case 'R':
					if (!applycomp) {
						printUsage();
						return(EINVAL);
					}
					if (argv[i][j] == 'J') {
						exclusive_io = false;
					}
					if (argv[i][j] == 'R') {
						nReverse = atoi(&argv[i][j + 1]);
						if (nReverse <= 0) {
							fprintf(stderr, "Warning: reverse jobs must be a positive number (%s)\n", argv[i]);
							nReverse = 0;
						}
					} else {
						nJobs = atoi(&argv[i][j + 1]);
						if (nJobs <= 0) {
							fprintf(stderr, "Warning: jobs must be a positive number (%s)\n", argv[i]);
							nJobs = 0;
						}
					}
					goto next_arg;
					break;
				case 'S':
					if (!applycomp) {
						printUsage();
						return(EINVAL);
					}
					sortQueue = true;
					goto next_arg;
					break;
				default:
					printUsage();
					return(EINVAL);
					break;
			}
		}
next_arg:
		;
	}

	if (i == argc || ((createfile || extractfile) && (argc - i < 2))) {
		printUsage();
		return(EINVAL);
	}

	if (int ret = pipe(ipcPipes)) {
		fprintf(stderr, "Error creating IPC pipe (%s)\n", strerror(errno));
		return errno;
	}
	snprintf(ipcPipeWriteEnd, sizeof(ipcPipeWriteEnd)/sizeof(char), "%d", ipcPipes[1]);

	if (nJobs > 0) {
		if (nReverse && !sortQueue) {
			fprintf(stderr, "Warning: reverse jobs are ignored when the item list is not sorted (-S)\n");
			nReverse = 0;
		}
		PP = createParallelProcessor(nJobs, nReverse, printVerbose);
//		if (PP)
//		{
//			if (printVerbose)
//			{
//				printf( "Verbose mode switched off in parallel processing mode\n");
//			}
//			printVerbose = false;
//		}
	}

	// ignore signals due to exceeding CPU or file size limits
	signal(SIGXCPU, SIG_IGN);
	signal(SIGXFSZ, SIG_IGN);

	int N, step, n;
	if (createfile || extractfile) {
		N = argc - 1;
		step = 2;
	} else {
		N = argc;
		step = 1;
	}
	for (n = 0 ; i < N ; i += step, ++n) {
		if (n && printVerbose > 0 && !nJobs) {
			printf("\n");
		}
		if (createfile || extractfile) {
			if (argv[i + 1][0] != '/') {
				cwd = getcwd(NULL, 0);
				if (cwd == NULL) {
					fprintf(stderr, "Unable to get PWD, exiting...\n");
					return(EACCES);
				}
				free_dst = TRUE;
				fullpathdst = (char *) malloc(strlen(cwd) + strlen(argv[i + 1]) + 2);
				sprintf(fullpathdst, "%s/%s", cwd, argv[i + 1]);
				free(cwd);
			} else
				fullpathdst = (char *) argv[i + 1];
		}

		if (argv[i][0] != '/') {
			cwd = getcwd(NULL, 0);
			if (cwd == NULL) {
				fprintf(stderr, "Unable to get PWD, exiting...\n");
				return(EACCES);
			}
			free_src = TRUE;
			fullpath = (char *) malloc(strlen(cwd) + strlen(argv[i]) + 2);
			sprintf(fullpath, "%s/%s", cwd, argv[i]);
			free(cwd);
		} else {
			free_src = FALSE;
			fullpath = (char *) argv[i];
		}

		if (lstat(fullpath, &fileinfo) < 0) {
			fprintf(stderr, "%s: %s\n", fullpath, strerror(errno));
			continue;
		}

		argIsFile = ((fileinfo.st_mode & S_IFDIR) == 0);

		if (!argIsFile) {
			folderarray[0] = fullpath;
			folderarray[1] = NULL;
		}

		if ((createfile || extractfile) && lstat(fullpathdst, &dstfileinfo) >= 0) {
			dstIsFile = ((dstfileinfo.st_mode & S_IFDIR) == 0);
			fprintf(stderr, "%s: %s already exists at this path\n", fullpath, dstIsFile ? "File" : "Folder");
			continue;
		}

		if (applycomp && argIsFile) {
			struct folder_info fi;
			fi.maxSize = maxSize;
			fi.compressionlevel = compressionlevel;
			fi.compressiontype = compressiontype;
			fi.allowLargeBlocks = allowLargeBlocks;
			fi.minSavings = minSavings;
			fi.check_files = fileCheck;
			fi.backup_file = backupFile;
			if (PP) {
				if (fileIsCompressable(fullpath, &fileinfo, PP)) {
					addFileToParallelProcessor(PP, fullpath, &fileinfo, &fi, true);
				} else {
					process_file(fullpath, NULL, &fileinfo, getParallelProcessorJobInfo(PP));
				}
			} else {
				compressFile(fullpath, &fileinfo, &fi, NULL);
			}
			lstat(fullpath, &fileinfo);
		}

		if (argIsFile && printVerbose > 0) {
			fileIsCompressable(fullpath, &fileinfo);
			printFileInfo(fullpath, &fileinfo, applycomp);
		} else if (!argIsFile) {
			if ((currfolder = fts_open(folderarray, FTS_PHYSICAL, NULL)) == NULL) {
				fprintf(stderr, "%s: %s\n", fullpath, strerror(errno));
//				exit(EACCES);
				continue;
			}
			folderinfo.uncompressed_size = 0;
			folderinfo.uncompressed_size_rounded = 0;
			folderinfo.compressed_size = 0;
			folderinfo.compressed_size_rounded = 0;
			folderinfo.compattr_size = 0;
			folderinfo.total_size = 0;
			folderinfo.num_compressed = 0;
			folderinfo.num_files = 0;
			folderinfo.num_hard_link_files = 0;
			folderinfo.num_folders = 0;
			folderinfo.num_hard_link_folders = 0;
			folderinfo.print_info = (nJobs) ? false : printVerbose;
			folderinfo.print_files = (nJobs == 0) ? printDir : 0;
			folderinfo.compress_files = applycomp;
			folderinfo.check_files = fileCheck;
			folderinfo.allowLargeBlocks = allowLargeBlocks;
			folderinfo.compressionlevel = compressionlevel;
			folderinfo.compressiontype = compressiontype;
			folderinfo.minSavings = minSavings;
			folderinfo.maxSize = maxSize;
			folderinfo.check_hard_links = hardLinkCheck;
			process_folder(currfolder, &folderinfo);
			folderinfo.num_folders--;
			if (printVerbose > 0 || !printDir) {
				if (!nJobs) {
					if (printDir) printf("\n");
					printf("%s:\n", fullpath);
				} else {
					printf("Adding %s to queue\n", fullpath);
				}
#if 0
				if (folderinfo.filetypes != NULL) {
					alltypesinfo.filetype = NULL;
					alltypesinfo.uncompressed_size = 0;
					alltypesinfo.uncompressed_size_rounded = 0;
					alltypesinfo.compressed_size = 0;
					alltypesinfo.compressed_size_rounded = 0;
					alltypesinfo.compattr_size = 0;
					alltypesinfo.total_size = 0;
					alltypesinfo.num_compressed = 0;
					alltypesinfo.num_files = 0;
					alltypesinfo.num_hard_link_files = 0;
					for (i = 0; i < folderinfo.numfiletypes; i++) {
						alltypesinfo.uncompressed_size += folderinfo.filetypes[i].uncompressed_size;
						alltypesinfo.uncompressed_size_rounded += folderinfo.filetypes[i].uncompressed_size_rounded;
						alltypesinfo.compressed_size += folderinfo.filetypes[i].compressed_size;
						alltypesinfo.compressed_size_rounded += folderinfo.filetypes[i].compressed_size_rounded;
						alltypesinfo.compattr_size += folderinfo.filetypes[i].compattr_size;
						alltypesinfo.total_size += folderinfo.filetypes[i].total_size;
						alltypesinfo.num_compressed += folderinfo.filetypes[i].num_compressed;
						alltypesinfo.num_files += folderinfo.filetypes[i].num_files;
						alltypesinfo.num_hard_link_files += folderinfo.filetypes[i].num_hard_link_files;

						if (!folderinfo.invert_filetypelist)
							printf("\nFile content type: %s\n", folderinfo.filetypes[i].filetype);
						if (folderinfo.filetypes[i].numextensions > 0) {
							if (!folderinfo.invert_filetypelist)
								printf("File extension(s): %s", folderinfo.filetypes[i].extensions[0]);
							free(folderinfo.filetypes[i].extensions[0]);
							for (j = 1; j < folderinfo.filetypes[i].numextensions; j++) {
								if (!folderinfo.invert_filetypelist)
									printf(", %s", folderinfo.filetypes[i].extensions[j]);
								free(folderinfo.filetypes[i].extensions[j]);
							}
							free(folderinfo.filetypes[i].extensions);
							if (!folderinfo.invert_filetypelist)
								printf("\n");
						}
						if (!folderinfo.invert_filetypelist)
							printf("Number of compressed files: %lld\n", folderinfo.filetypes[i].num_compressed);
						if (printVerbose > 0 && nJobs == 0 && (!folderinfo.invert_filetypelist)) {
							printf("Total number of files: %lld\n", folderinfo.filetypes[i].num_files);
							if (hardLinkCheck)
								printf("Total number of file hard links: %lld\n", folderinfo.filetypes[i].num_hard_link_files);
							filesize = folderinfo.filetypes[i].uncompressed_size;
							filesize_rounded = folderinfo.filetypes[i].uncompressed_size_rounded;
							if (folderinfo.filetypes[i].num_hard_link_files == 0 || !hardLinkCheck)
								printf("File(s) size (uncompressed; reported size by Mac OS 10.6+ Finder): %s\n",
									   getSizeStr(filesize, filesize_rounded, 1));
							else
								printf("File(s) size (uncompressed): %s\n", getSizeStr(filesize, filesize_rounded, 0));
							filesize = folderinfo.filetypes[i].compressed_size;
							filesize_rounded = folderinfo.filetypes[i].compressed_size_rounded;
							printf("File(s) size (compressed - decmpfs xattr): %s\n",
								   getSizeStr(filesize, filesize_rounded, 0));
							filesize = folderinfo.filetypes[i].compressed_size + folderinfo.filetypes[i].compattr_size;
							filesize_rounded = folderinfo.filetypes[i].compressed_size_rounded + folderinfo.filetypes[i].compattr_size;
							printf("File(s) size (compressed): %s\n", getSizeStr(filesize, filesize_rounded, 0));
							printf("Compression savings: %0.1f%%\n", (1.0 - ((float)(folderinfo.filetypes[i].compressed_size + folderinfo.filetypes[i].compattr_size) / folderinfo.filetypes[i].uncompressed_size)) * 100.0);
							filesize = folderinfo.filetypes[i].total_size;
							printf("Approximate total file(s) size (files + file overhead): %s\n",
								   getSizeStr(filesize, filesize, 0));
						}
						free(folderinfo.filetypes[i].filetype);
					}
					if (folderinfo.invert_filetypelist) {
						alltypesinfo.uncompressed_size = folderinfo.uncompressed_size - alltypesinfo.uncompressed_size;
						alltypesinfo.uncompressed_size_rounded = folderinfo.uncompressed_size_rounded - alltypesinfo.uncompressed_size_rounded;
						alltypesinfo.compressed_size = folderinfo.compressed_size - alltypesinfo.compressed_size;
						alltypesinfo.compressed_size_rounded = folderinfo.compressed_size_rounded - alltypesinfo.compressed_size_rounded;
						alltypesinfo.compattr_size = folderinfo.compattr_size - alltypesinfo.compattr_size;
						alltypesinfo.total_size = folderinfo.total_size - alltypesinfo.total_size;
						alltypesinfo.num_compressed = folderinfo.num_compressed - alltypesinfo.num_compressed;
						alltypesinfo.num_files = folderinfo.num_files - alltypesinfo.num_files;
						alltypesinfo.num_hard_link_files = folderinfo.num_hard_link_files - alltypesinfo.num_hard_link_files;
					}
					if (folderinfo.numfiletypes > 1 || folderinfo.invert_filetypelist) {
						printf("\nTotals of file content types\n");
						printf("Number of compressed files: %lld\n", alltypesinfo.num_compressed);
						if (printVerbose > 0 && nJobs == 0) {
							printf("Total number of files: %lld\n", alltypesinfo.num_files);
							if (hardLinkCheck)
								printf("Total number of file hard links: %lld\n", alltypesinfo.num_hard_link_files);
							filesize = alltypesinfo.uncompressed_size;
							filesize_rounded = alltypesinfo.uncompressed_size_rounded;
							if (alltypesinfo.num_hard_link_files == 0 || !hardLinkCheck)
								printf("File(s) size (uncompressed; reported size by Mac OS 10.6+ Finder): %s\n",
									   getSizeStr(filesize, filesize_rounded, 1));
							else
								printf("File(s) size (uncompressed): %s\n", getSizeStr(filesize, filesize_rounded, 0));
							filesize = alltypesinfo.compressed_size;
							filesize_rounded = alltypesinfo.compressed_size_rounded;
							printf("File(s) size (compressed - decmpfs xattr): %s\n",
								   getSizeStr(filesize, filesize_rounded, 0));
							filesize = alltypesinfo.compressed_size + alltypesinfo.compattr_size;
							filesize_rounded = alltypesinfo.compressed_size_rounded + alltypesinfo.compattr_size;
							printf("File(s) size (compressed): %s\n", getSizeStr(filesize, filesize_rounded, 0));
							printf("Compression savings: %0.1f%%\n", (1.0 - ((float)(alltypesinfo.compressed_size + alltypesinfo.compattr_size) / alltypesinfo.uncompressed_size)) * 100.0);
							filesize = alltypesinfo.total_size;
							printf("Approximate total file(s) size (files + file overhead): %s\n",
								   getSizeStr(filesize, filesize, 0));
						}
					}
					printf("\n");
				}
#endif //0
				if (nJobs == 0) {
					if (folderinfo.num_compressed == 0 && !applycomp)
						printf("Folder contains no compressed files\n");
					else if (folderinfo.num_compressed == 0 && applycomp)
						printf("No compressable files in folder\n");
					else
						printf("Number of compressed files: %lld\n", folderinfo.num_compressed);
					if (printVerbose > 0) {
						printFolderInfo(&folderinfo, hardLinkCheck);
					}
				}
			}
		}

		if (free_src) {
			xfree(fullpath);
			free_src = false;
		}
		if (free_dst) {
			xfree(fullpathdst);
			free_dst = false;
		}
	}

	if (PP) {
		if (sortQueue) {
			sortFilesInParallelProcessorBySize(PP);
		}
		if (filesInParallelProcessor(PP)) {
			signal(SIGINT, signal_handler);
			signal(SIGHUP, signal_handler);
			fprintf(stderr, "Starting %d worker threads to process queue with %lu items\n",
					nJobs, filesInParallelProcessor(PP));
			int processed = runParallelProcessor(PP);
			fprintf(stderr, "Processed %d entries\n", processed);
			if (printVerbose > 0) {
				struct folder_info *fInfo = getParallelProcessorJobInfo(PP);
				if (fInfo->num_files > 0) {
					printFolderInfo(getParallelProcessorJobInfo(PP), hardLinkCheck);
				}
			}
		} else {
			fprintf(stderr, "No compressable files found.\n");
		}
		releaseParallelProcessor(PP);
	}
// 	if (maxOutBufSize) {
// 		fprintf(stderr, "maxOutBufSize: %zd\n", maxOutBufSize);
// 	}
	if (ipcPipes[0] != -1) {
		close(ipcPipes[0]);
	}
	if (ipcPipes[1] != -1) {
		close(ipcPipes[1]);
	}

	return 0;
}

int main (int argc, const char * argv[])
{
    return zfsctool(argc, argv);
}
