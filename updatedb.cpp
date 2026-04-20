/* updatedb(8).

Copyright (C) 2005, 2007, 2008 Red Hat, Inc. All rights reserved.

This copyrighted material is made available to anyone wishing to use, modify,
copy, or redistribute it subject to the terms and conditions of the GNU General
Public License v.2.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
Street, Fifth Floor, Boston, MA 02110-1301, USA.

Author: Miloslav Trmac <mitr@redhat.com>


plocate modifications: Copyright (C) 2020 Steinar H. Gunderson.
plocate parts and modifications are licensed under the GPLv2 or, at your option,
any later version.
 */

#include "bind-mount.h"
#include "complete_pread.h"
#include "conf.h"
#include "database-builder.h"
#include "db.h"
#include "dprintf.h"
#include "io_uring_engine.h"
#include "lib.h"

#include <algorithm>
#include <arpa/inet.h>
#include <assert.h>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <inttypes.h>
#include <iosfwd>
#include <math.h>
#include <memory>
#include <mntent.h>
#include <random>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace std;
using namespace std::chrono;

/* Next conf_prunepaths entry */
static size_t conf_prunepaths_index; /* = 0; */

static bool path_is_excluded(const string &path)
{
	for (const string &exc : conf_exclude_paths) {
		if (path.size() >= exc.size() &&
		    path.compare(0, exc.size(), exc) == 0 &&
		    (path.size() == exc.size() || path[exc.size()] == '/')) {
			return true;
		}
	}
	return false;
}

static bool path_is_included(const string &path)
{
	if (conf_include_paths.empty()) return true;
	for (const string &inc : conf_include_paths) {
		if (path.size() >= inc.size() &&
		    path.compare(0, inc.size(), inc) == 0 &&
		    (path.size() == inc.size() || path[inc.size()] == '/')) {
			return true;
		}
	}
	return false;
}

// True if an include path is a descendant of path (must traverse to reach it).
static bool path_leads_to_included(const string &path)
{
	if (conf_include_paths.empty()) return true;
	for (const string &inc : conf_include_paths) {
		if (inc.size() > path.size() &&
		    inc.compare(0, path.size(), path) == 0 &&
		    (path.back() == '/' || inc[path.size()] == '/')) {
			return true;
		}
	}
	return false;
}

void usage()
{
	printf(
		"Usage: updatedb PLOCATE_DB\n"
		"\n"
		"Generate plocate index from mlocate.db, typically /var/lib/mlocate/mlocate.db.\n"
		"Normally, the destination should be /var/lib/mlocate/plocate.db.\n"
		"\n"
		"  -b, --block-size SIZE  number of filenames to store in each block (default 32)\n"
		"  -p, --plaintext        input is a plaintext file, not an mlocate database\n"
		"      --help             print this help\n"
		"      --version          print version information\n");
}

void version()
{
	printf("updatedb %s\n", PACKAGE_VERSION);
	printf("Copyright (C) 2007 Red Hat, Inc. All rights reserved.\n");
	printf("Copyright 2020 Steinar H. Gunderson\n");
	printf("This software is distributed under the GPL v.2.\n");
	printf("\n");
	printf("This program is provided with NO WARRANTY, to the extent permitted by law.\n");
}

int opendir_noatime(int dirfd, const char *path)
{
	static bool noatime_failed = false;

	if (!noatime_failed) {
#ifdef O_NOATIME
		int fd = openat(dirfd, path, O_RDONLY | O_DIRECTORY | O_NOATIME);
#else
		int fd = openat(dirfd, path, O_RDONLY | O_DIRECTORY);
#endif
		if (fd != -1) {
			return fd;
		} else if (errno == EPERM) {
			/* EPERM is fairly O_NOATIME-specific; missing access rights cause
			   EACCES. */
			noatime_failed = true;
			// Retry below.
		} else {
			return -1;
		}
	}
	return openat(dirfd, path, O_RDONLY | O_DIRECTORY);
}

bool time_is_current(const dir_time &t)
{
	static dir_time cache{ 0, 0 };

	/* This is more difficult than it should be because Linux uses a cheaper time
	   source for filesystem timestamps than for gettimeofday() and they can get
	   slightly out of sync, see
	   https://bugzilla.redhat.com/show_bug.cgi?id=244697 .  This affects even
	   nanosecond timestamps (and don't forget that tv_nsec existence doesn't
	   guarantee that the underlying filesystem has such resolution - it might be
	   microseconds or even coarser).

	   The worst case is probably FAT timestamps with 2-second resolution
	   (although using such a filesystem violates POSIX file times requirements).

	   So, to be on the safe side, require a >3.0 second difference (2 seconds to
	   make sure the FAT timestamp changed, 1 more to account for the Linux
	   timestamp races).  This large margin might make updatedb marginally more
	   expensive, but it only makes a difference if the directory was very
	   recently updated _and_ is will not be updated again until the next
	   updatedb run; this is not likely to happen for most directories. */

	/* Cache gettimeofday () results to rule out obviously old time stamps;
	   CACHE contains the earliest time we reject as too current. */
	if (t < cache) {
		return false;
	}

	struct timeval tv;
	gettimeofday(&tv, nullptr);
	cache.sec = tv.tv_sec - 3;
	cache.nsec = tv.tv_usec * 1000;

	return t >= cache;
}

struct entry {
	string name;
	off_t filesize = -1;
	off_t allocated = -1;
	bool is_directory;
	string checksum;
	struct timespec file_mtime = { 0, 0 };

	// For directories only:
	int fd = -1;
	dir_time dt = unknown_dir_time;
	dir_time db_modified = unknown_dir_time;
	dev_t dev;
};

bool filesystem_is_excluded(const char *path)
{
	if (conf_debug_pruning) {
		/* This is debugging output, don't mark anything for translation */
		fprintf(stderr, "Checking whether filesystem `%s' is excluded:\n", path);
	}
	FILE *f = setmntent("/proc/mounts", "r");
	if (f == nullptr) {
		return false;
	}

	struct mntent *me;
	while ((me = getmntent(f)) != nullptr) {
		if (conf_debug_pruning) {
			/* This is debugging output, don't mark anything for translation */
			fprintf(stderr, " `%s', type `%s'\n", me->mnt_dir, me->mnt_type);
		}
		string type(me->mnt_type);
		for (char &p : type) {
			p = toupper(p);
		}
		if (find(conf_prunefs.begin(), conf_prunefs.end(), type) != conf_prunefs.end()) {
			/* Paths in /proc/self/mounts contain no symbolic links.  Besides
		           avoiding a few system calls, avoiding the realpath () avoids hangs
		           if the filesystem is unavailable hard-mounted NFS. */
			char *dir = me->mnt_dir;
			if (conf_debug_pruning) {
				/* This is debugging output, don't mark anything for translation */
				fprintf(stderr, " => type matches, dir `%s'\n", dir);
			}
			bool res = (strcmp(path, dir) == 0);
			if (dir != me->mnt_dir)
				free(dir);
			if (res) {
				endmntent(f);
				return true;
			}
		}
	}
	if (conf_debug_pruning) {
		/* This is debugging output, don't mark anything for translation */
		fprintf(stderr, "...done\n");
	}
	endmntent(f);
	return false;
}

dir_time get_dirtime_from_stat(const struct stat &buf)
{
	dir_time ctime{ buf.st_ctim.tv_sec, int32_t(buf.st_ctim.tv_nsec) };
	dir_time mtime{ buf.st_mtim.tv_sec, int32_t(buf.st_mtim.tv_nsec) };
	dir_time dt = max(ctime, mtime);

	if (time_is_current(dt)) {
		/* The directory might be changing right now and we can't be sure the
		   timestamp will be changed again if more changes happen very soon, mark
		   the timestamp as invalid to force rescanning the directory next time
		   updatedb is run. */
		return unknown_dir_time;
	} else {
		return dt;
	}
}

// Represents the old database we are updating.
struct db_record {
	string path;
	dir_time dt;
	int64_t filesize = -1;
	int64_t allocated = -1;
	string checksum;
};

class ExistingDB {
public:
	explicit ExistingDB(int fd);
	~ExistingDB();

	db_record read_next();
	void unread(db_record record)
	{
		unread_record = move(record);
	}
	string read_next_dictionary() const;
	bool get_error() const { return error; }

private:
	const int fd;
	Header hdr;

	uint32_t current_docid = 0;

	string current_filename_block;
	const char *current_filename_ptr = nullptr, *current_filename_end = nullptr;

	off_t compressed_dir_time_pos;
	string compressed_dir_time;
	string current_dir_time_block;
	const char *current_dir_time_ptr = nullptr, *current_dir_time_end = nullptr;

	bool has_filesize_data = false;
	off_t compressed_filesize_pos;
	string compressed_filesize;
	string current_filesize_block;
	const char *current_filesize_ptr = nullptr, *current_filesize_end = nullptr;

	bool has_checksum_data = false;
	off_t compressed_checksum_pos;
	string compressed_checksum;
	string current_checksum_block;
	const char *current_checksum_ptr = nullptr, *current_checksum_end = nullptr;

	db_record unread_record;

	// Used in one-shot mode, repeatedly.
	ZSTD_DCtx *ctx;

	// Used in streaming mode.
	ZSTD_DCtx *dir_time_ctx;
	ZSTD_DCtx *filesize_ctx = nullptr;
	ZSTD_DCtx *checksum_dctx = nullptr;

	ZSTD_DDict *ddict = nullptr;

	bool eof = false, error = false;
};

ExistingDB::ExistingDB(int fd)
	: fd(fd)
{
	if (fd == -1) {
		error = true;
		return;
	}

	if (!try_complete_pread(fd, &hdr, sizeof(hdr), /*offset=*/0)) {
		if (conf_verbose) {
			perror("pread(header)");
		}
		error = true;
		return;
	}
	if (memcmp(hdr.magic, "\0plocate", 8) != 0) {
		if (conf_verbose) {
			fprintf(stderr, "Old database had header mismatch, ignoring.\n");
		}
		error = true;
		return;
	}
	if (hdr.version != 1 || hdr.max_version < 2) {
		if (conf_verbose) {
			fprintf(stderr, "Old database had version mismatch (version=%d max_version=%d), ignoring.\n",
			        hdr.version, hdr.max_version);
		}
		error = true;
		return;
	}

	// Compare the configuration block with our current one.
	if (hdr.conf_block_length_bytes != conf_block.size()) {
		if (conf_verbose) {
			fprintf(stderr, "Old database had different configuration block (size mismatch), ignoring.\n");
		}
		error = true;
		return;
	}
	string str;
	str.resize(hdr.conf_block_length_bytes);
	if (!try_complete_pread(fd, str.data(), hdr.conf_block_length_bytes, hdr.conf_block_offset_bytes)) {
		if (conf_verbose) {
			perror("pread(conf_block)");
		}
		error = true;
		return;
	}
	if (str != conf_block) {
		if (conf_verbose) {
			fprintf(stderr, "Old database had different configuration block (contents mismatch), ignoring.\n");
		}
		error = true;
		return;
	}

	// Read dictionary, if it exists.
	if (hdr.zstd_dictionary_length_bytes > 0) {
		string dictionary;
		dictionary.resize(hdr.zstd_dictionary_length_bytes);
		if (try_complete_pread(fd, &dictionary[0], hdr.zstd_dictionary_length_bytes, hdr.zstd_dictionary_offset_bytes)) {
			ddict = ZSTD_createDDict(dictionary.data(), dictionary.size());
		} else {
			if (conf_verbose) {
				perror("pread(dictionary)");
			}
			error = true;
			return;
		}
	}
	compressed_dir_time_pos = hdr.directory_data_offset_bytes;

	if (hdr.max_version >= 3 && hdr.filesize_data_length_bytes > 0) {
		has_filesize_data = true;
		compressed_filesize_pos = hdr.filesize_data_offset_bytes;
		filesize_ctx = ZSTD_createDCtx();
	} else {
		hdr.filesize_data_length_bytes = 0;
		hdr.filesize_data_offset_bytes = 0;
		hdr.block_size = 0;
	}

	if (hdr.max_version >= 4 && hdr.checksum_data_length_bytes > 0) {
		has_checksum_data = true;
		compressed_checksum_pos = hdr.checksum_data_offset_bytes;
		checksum_dctx = ZSTD_createDCtx();
	} else {
		hdr.checksum_data_length_bytes = 0;
		hdr.checksum_data_offset_bytes = 0;
	}

	ctx = ZSTD_createDCtx();
	dir_time_ctx = ZSTD_createDCtx();
}

ExistingDB::~ExistingDB()
{
	if (fd != -1) {
		close(fd);
	}
}

db_record ExistingDB::read_next()
{
	if (!unread_record.path.empty()) {
		auto ret = move(unread_record);
		unread_record.path.clear();
		return ret;
	}

	if (eof || error) {
		return { "", not_a_dir, -1, -1 };
	}

	// See if we need to read a new filename block.
	if (current_filename_ptr == nullptr) {
		if (current_docid >= hdr.num_docids) {
			eof = true;
			return { "", not_a_dir, -1, -1 };
		}

		// Read the file offset from this docid and the next one.
		// This is always allowed, since we have a sentinel block at the end.
		off_t offset_for_block = hdr.filename_index_offset_bytes + current_docid * sizeof(uint64_t);
		uint64_t vals[2];
		if (!try_complete_pread(fd, vals, sizeof(vals), offset_for_block)) {
			if (conf_verbose) {
				perror("pread(offset)");
			}
			error = true;
			return { "", not_a_dir, -1, -1 };
		}

		off_t offset = vals[0];
		size_t compressed_len = vals[1] - vals[0];
		unique_ptr<char[]> compressed(new char[compressed_len]);
		if (!try_complete_pread(fd, compressed.get(), compressed_len, offset)) {
			if (conf_verbose) {
				perror("pread(block)");
			}
			error = true;
			return { "", not_a_dir, -1, -1 };
		}

		unsigned long long uncompressed_len = ZSTD_getFrameContentSize(compressed.get(), compressed_len);
		if (uncompressed_len == ZSTD_CONTENTSIZE_UNKNOWN || uncompressed_len == ZSTD_CONTENTSIZE_ERROR) {
			if (conf_verbose) {
				fprintf(stderr, "ZSTD_getFrameContentSize() failed\n");
			}
			error = true;
			return { "", not_a_dir, -1, -1 };
		}

		string block;
		block.resize(uncompressed_len + 1);

		size_t err;
		if (ddict != nullptr) {
			err = ZSTD_decompress_usingDDict(ctx, &block[0], block.size(), compressed.get(),
			                                 compressed_len, ddict);
		} else {
			err = ZSTD_decompressDCtx(ctx, &block[0], block.size(), compressed.get(),
			                          compressed_len);
		}
		if (ZSTD_isError(err)) {
			if (conf_verbose) {
				fprintf(stderr, "ZSTD_decompress(): %s\n", ZSTD_getErrorName(err));
			}
			error = true;
			return { "", not_a_dir, -1, -1 };
		}
		block[block.size() - 1] = '\0';
		current_filename_block = move(block);
		current_filename_ptr = current_filename_block.data();
		current_filename_end = current_filename_block.data() + current_filename_block.size();
		++current_docid;
	}

	// See if we need to read more directory time data.
	while (current_dir_time_ptr == current_dir_time_end ||
	       (*current_dir_time_ptr != 0 &&
	        size_t(current_dir_time_end - current_dir_time_ptr) < sizeof(dir_time) + 1)) {
		if (current_dir_time_ptr != nullptr) {
			const size_t bytes_consumed = current_dir_time_ptr - current_dir_time_block.data();
			current_dir_time_block.erase(current_dir_time_block.begin(), current_dir_time_block.begin() + bytes_consumed);
		}

		const size_t existing_data = current_dir_time_block.size();
		current_dir_time_block.resize(existing_data + 4096);

		ZSTD_outBuffer outbuf;
		outbuf.dst = current_dir_time_block.data() + existing_data;
		outbuf.size = 4096;
		outbuf.pos = 0;

		ZSTD_inBuffer inbuf;
		inbuf.src = compressed_dir_time.data();
		inbuf.size = compressed_dir_time.size();
		inbuf.pos = 0;

		int err = ZSTD_decompressStream(dir_time_ctx, &outbuf, &inbuf);
		if (err < 0) {
			if (conf_verbose) {
				fprintf(stderr, "ZSTD_decompress(): %s\n", ZSTD_getErrorName(err));
			}
			error = true;
			return { "", not_a_dir, -1, -1 };
		}
		compressed_dir_time.erase(compressed_dir_time.begin(), compressed_dir_time.begin() + inbuf.pos);
		current_dir_time_block.resize(existing_data + outbuf.pos);

		if (inbuf.pos == 0 && outbuf.pos == 0) {
			char buf[4096];
			size_t bytes_to_read = min<size_t>(
				hdr.directory_data_offset_bytes + hdr.directory_data_length_bytes - compressed_dir_time_pos,
				sizeof(buf));
			if (bytes_to_read == 0) {
				error = true;
				return { "", not_a_dir, -1, -1 };
			}
			if (!try_complete_pread(fd, buf, bytes_to_read, compressed_dir_time_pos)) {
				if (conf_verbose) {
					perror("pread(dirtime)");
				}
				error = true;
				return { "", not_a_dir, -1, -1 };
			}
			compressed_dir_time_pos += bytes_to_read;
			compressed_dir_time.insert(compressed_dir_time.end(), buf, buf + bytes_to_read);
		}

		current_dir_time_ptr = current_dir_time_block.data();
		current_dir_time_end = current_dir_time_block.data() + current_dir_time_block.size();
	}

	// Read filesize data if available.
	int64_t filesize = -1, allocated = -1;
	constexpr size_t filesize_record_size = sizeof(int64_t) * 2;
	if (has_filesize_data) {
		while (current_filesize_ptr == current_filesize_end ||
		       size_t(current_filesize_end - current_filesize_ptr) < filesize_record_size) {
			if (current_filesize_ptr != nullptr) {
				const size_t bytes_consumed = current_filesize_ptr - current_filesize_block.data();
				current_filesize_block.erase(current_filesize_block.begin(), current_filesize_block.begin() + bytes_consumed);
			}

			const size_t existing_data = current_filesize_block.size();
			current_filesize_block.resize(existing_data + 4096);

			ZSTD_outBuffer outbuf;
			outbuf.dst = current_filesize_block.data() + existing_data;
			outbuf.size = 4096;
			outbuf.pos = 0;

			ZSTD_inBuffer inbuf;
			inbuf.src = compressed_filesize.data();
			inbuf.size = compressed_filesize.size();
			inbuf.pos = 0;

			int err = ZSTD_decompressStream(filesize_ctx, &outbuf, &inbuf);
			if (err < 0) {
				if (conf_verbose) {
					fprintf(stderr, "ZSTD_decompress(filesize): %s\n", ZSTD_getErrorName(err));
				}
				has_filesize_data = false;
				break;
			}
			compressed_filesize.erase(compressed_filesize.begin(), compressed_filesize.begin() + inbuf.pos);
			current_filesize_block.resize(existing_data + outbuf.pos);

			if (inbuf.pos == 0 && outbuf.pos == 0) {
				char buf[4096];
				size_t bytes_to_read = min<size_t>(
					hdr.filesize_data_offset_bytes + hdr.filesize_data_length_bytes - compressed_filesize_pos,
					sizeof(buf));
				if (bytes_to_read == 0) {
					has_filesize_data = false;
					break;
				}
				if (!try_complete_pread(fd, buf, bytes_to_read, compressed_filesize_pos)) {
					if (conf_verbose) {
						perror("pread(filesize)");
					}
					has_filesize_data = false;
					break;
				}
				compressed_filesize_pos += bytes_to_read;
				compressed_filesize.insert(compressed_filesize.end(), buf, buf + bytes_to_read);
			}

			current_filesize_ptr = current_filesize_block.data();
			current_filesize_end = current_filesize_block.data() + current_filesize_block.size();
		}

		if (has_filesize_data && size_t(current_filesize_end - current_filesize_ptr) >= filesize_record_size) {
			memcpy(&filesize, current_filesize_ptr, sizeof(filesize));
			current_filesize_ptr += sizeof(filesize);
			memcpy(&allocated, current_filesize_ptr, sizeof(allocated));
			current_filesize_ptr += sizeof(allocated);
		}
	}

	// Read checksum data if available (variable-length: uint8 len + len bytes).
	string checksum;
	if (has_checksum_data) {
		auto ensure_checksum_bytes = [&](size_t needed) -> bool {
			while (current_checksum_ptr == current_checksum_end ||
			       size_t(current_checksum_end - current_checksum_ptr) < needed) {
				if (current_checksum_ptr != nullptr) {
					const size_t bytes_consumed = current_checksum_ptr - current_checksum_block.data();
					current_checksum_block.erase(current_checksum_block.begin(), current_checksum_block.begin() + bytes_consumed);
				}

				const size_t existing_data = current_checksum_block.size();
				current_checksum_block.resize(existing_data + 4096);

				ZSTD_outBuffer outbuf;
				outbuf.dst = current_checksum_block.data() + existing_data;
				outbuf.size = 4096;
				outbuf.pos = 0;

				ZSTD_inBuffer inbuf;
				inbuf.src = compressed_checksum.data();
				inbuf.size = compressed_checksum.size();
				inbuf.pos = 0;

				int err = ZSTD_decompressStream(checksum_dctx, &outbuf, &inbuf);
				if (err < 0) {
					if (conf_verbose) {
						fprintf(stderr, "ZSTD_decompress(checksum): %s\n", ZSTD_getErrorName(err));
					}
					has_checksum_data = false;
					return false;
				}
				compressed_checksum.erase(compressed_checksum.begin(), compressed_checksum.begin() + inbuf.pos);
				current_checksum_block.resize(existing_data + outbuf.pos);

				if (inbuf.pos == 0 && outbuf.pos == 0) {
					char buf[4096];
					size_t bytes_to_read = min<size_t>(
						hdr.checksum_data_offset_bytes + hdr.checksum_data_length_bytes - compressed_checksum_pos,
						sizeof(buf));
					if (bytes_to_read == 0) {
						has_checksum_data = false;
						return false;
					}
					if (!try_complete_pread(fd, buf, bytes_to_read, compressed_checksum_pos)) {
						if (conf_verbose) {
							perror("pread(checksum)");
						}
						has_checksum_data = false;
						return false;
					}
					compressed_checksum_pos += bytes_to_read;
					compressed_checksum.insert(compressed_checksum.end(), buf, buf + bytes_to_read);
				}

				current_checksum_ptr = current_checksum_block.data();
				current_checksum_end = current_checksum_block.data() + current_checksum_block.size();
			}
			return true;
		};

		if (ensure_checksum_bytes(1)) {
			uint8_t len = static_cast<uint8_t>(*current_checksum_ptr);
			current_checksum_ptr++;
			if (len > 0 && ensure_checksum_bytes(len)) {
				checksum.assign(current_checksum_ptr, len);
				current_checksum_ptr += len;
			}
		}
	}

	string filename = current_filename_ptr;
	current_filename_ptr += filename.size() + 1;
	if (current_filename_ptr == current_filename_end) {
		current_filename_ptr = nullptr;
	}

	dir_time dt;
	if (*current_dir_time_ptr == 0) {
		++current_dir_time_ptr;
		dt = not_a_dir;
	} else {
		++current_dir_time_ptr;
		memcpy(&dt.sec, current_dir_time_ptr, sizeof(dt.sec));
		current_dir_time_ptr += sizeof(dt.sec);
		memcpy(&dt.nsec, current_dir_time_ptr, sizeof(dt.nsec));
		current_dir_time_ptr += sizeof(dt.nsec);
	}

	return { move(filename), dt, filesize, allocated, move(checksum) };
}

string ExistingDB::read_next_dictionary() const
{
	if (hdr.next_zstd_dictionary_length_bytes == 0 || hdr.next_zstd_dictionary_length_bytes > 1048576) {
		return "";
	}
	string str;
	str.resize(hdr.next_zstd_dictionary_length_bytes);
	if (!try_complete_pread(fd, str.data(), hdr.next_zstd_dictionary_length_bytes, hdr.next_zstd_dictionary_offset_bytes)) {
		if (conf_verbose) {
			perror("pread(next_dictionary)");
		}
		return "";
	}
	return str;
}

// Scans the directory with absolute path “path”, which is opened as “fd”.
// Uses relative paths and openat() only, evading any issues with PATH_MAX
// and time-of-check-time-of-use race conditions. (mlocate's updatedb
// does a much more complicated dance with changing the current working
// directory, probably in the interest of portability to old platforms.)
// “parent_dev” must be the device of the parent directory of “path”.
//
static string hex_to_bytes(const string &hex)
{
	string bytes;
	bytes.reserve(hex.size() / 2);
	for (size_t i = 0; i + 1 < hex.size(); i += 2) {
		unsigned int byte;
		if (sscanf(hex.c_str() + i, "%2x", &byte) != 1)
			return "";
		bytes.push_back(static_cast<char>(byte));
	}
	return bytes;
}

static struct timespec db_file_mtime = { 0, 0 };

static string compute_checksum(const string &command, const string &filepath)
{
	int pipefd[2];
	if (pipe(pipefd) == -1)
		return "";

	pid_t pid = fork();
	if (pid == -1) {
		close(pipefd[0]);
		close(pipefd[1]);
		return "";
	}
	if (pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[1]);
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		execlp(command.c_str(), command.c_str(), filepath.c_str(), nullptr);
		_exit(127);
	}

	close(pipefd[1]);
	string output;
	char buf[1024];
	for (;;) {
		ssize_t n = read(pipefd[0], buf, sizeof(buf));
		if (n <= 0) break;
		output.append(buf, n);
	}
	close(pipefd[0]);

	int status;
	waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return "";

	while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' '))
		output.pop_back();
	if (output.empty())
		return "";

	size_t pos = output.find_first_of(" \t");
	string hex_str = (pos != string::npos) ? output.substr(0, pos) : output;

	return hex_to_bytes(hex_str);
}

static string read_xattr_string(const string &filepath, const string &attr_name)
{
	char buf[256];
	ssize_t len = getxattr(filepath.c_str(), attr_name.c_str(), buf, sizeof(buf));
	if (len < 0) {
		if (errno == ERANGE) {
			len = getxattr(filepath.c_str(), attr_name.c_str(), nullptr, 0);
			if (len <= 0) return "";
			string val(len, '\0');
			len = getxattr(filepath.c_str(), attr_name.c_str(), val.data(), len);
			if (len <= 0) return "";
			val.resize(len);
			return val;
		}
		return "";
	}
	return string(buf, len);
}

static string read_xattr_checksum(const string &filepath)
{
	string val = read_xattr_string(filepath, conf_checksum_xattr);
	if (val.empty()) return "";

	while (!val.empty() && (val.back() == '\n' || val.back() == '\r' || val.back() == ' '))
		val.pop_back();
	if (val.empty()) return "";

	size_t pos = val.find_first_of(" \t");
	string hex_str = (pos != string::npos) ? val.substr(0, pos) : val;
	return hex_to_bytes(hex_str);
}

static struct timespec parse_xattr_mtime(const string &val)
{
	struct timespec ts = { 0, 0 };
	const char *p = val.c_str();

	const char *mtime_key = strstr(p, "mtime=");
	if (mtime_key)
		p = mtime_key + 6;

	char *end;
	ts.tv_sec = strtol(p, &end, 10);
	if (*end == '.') {
		const char *ns_start = end + 1;
		ts.tv_nsec = strtol(ns_start, &end, 10);
		int digits = end - ns_start;
		for (int i = digits; i < 9; i++)
			ts.tv_nsec *= 10;
	}
	return ts;
}

static string get_checksum_for_entry(const string &filepath,
                                     const struct timespec &file_mtime)
{
	if (!conf_checksum_xattr.empty()) {
		if (!conf_checksum_xattr_mtime.empty()) {
			string mtime_val = read_xattr_string(filepath, conf_checksum_xattr_mtime);
			if (!mtime_val.empty()) {
				struct timespec xattr_ts = parse_xattr_mtime(mtime_val);
				if (xattr_ts.tv_sec > file_mtime.tv_sec ||
				    (xattr_ts.tv_sec == file_mtime.tv_sec &&
				     xattr_ts.tv_nsec >= file_mtime.tv_nsec)) {
					return read_xattr_checksum(filepath);
				}
			}
			if (!conf_checksum_command.empty())
				return compute_checksum(conf_checksum_command, filepath);
			return read_xattr_checksum(filepath);
		}
		return read_xattr_checksum(filepath);
	}
	if (!conf_checksum_command.empty())
		return compute_checksum(conf_checksum_command, filepath);
	return "";
}

// Takes ownership of fd.
int scan(const string &path, int fd, dev_t parent_dev, dir_time modified, dir_time db_modified, ExistingDB *existing_db, DatabaseReceiver *corpus, DictionaryBuilder *dict_builder)
{
	if (string_list_contains_dir_path(&conf_prunepaths, &conf_prunepaths_index, path)) {
		if (conf_debug_pruning) {
			/* This is debugging output, don't mark anything for translation */
			fprintf(stderr, "Skipping `%s': in prunepaths\n", path.c_str());
		}
		close(fd);
		return 0;
	}
	if (path_is_excluded(path)) {
		if (conf_debug_pruning) {
			fprintf(stderr, "Skipping `%s': in exclude paths\n", path.c_str());
		}
		close(fd);
		return 0;
	}
	if (!path_is_included(path) && !path_leads_to_included(path)) {
		if (conf_debug_pruning) {
			fprintf(stderr, "Skipping `%s': not in include paths\n", path.c_str());
		}
		close(fd);
		return 0;
	}
	if (conf_prune_bind_mounts && is_bind_mount(path.c_str())) {
		if (conf_debug_pruning) {
			/* This is debugging output, don't mark anything for translation */
			fprintf(stderr, "Skipping `%s': bind mount\n", path.c_str());
		}
		close(fd);
		return 0;
	}

	// We read in the old directory no matter whether it is current or not,
	// because even if we're not going to use it, we'll need the modification directory
	// of any subdirectories.

	// Skip over anything before this directory; it is stuff that we would have
	// consumed earlier if we wanted it.
	for (;;) {
		db_record record = existing_db->read_next();
		if (record.path.empty()) {
			break;
		}
		if (dir_path_cmp(path, record.path) <= 0) {
			existing_db->unread(move(record));
			break;
		}
	}

	// Now read everything in this directory.
	vector<entry> db_entries;
	const string path_plus_slash = path.back() == '/' ? path : path + '/';
	for (;;) {
		db_record record = existing_db->read_next();
		if (record.path.empty()) {
			break;
		}

		if (record.path.rfind(path_plus_slash, 0) != 0) {
			existing_db->unread(move(record));
			break;
		}
		if (record.path.find_first_of('/', path_plus_slash.size()) != string::npos) {
			existing_db->unread(move(record));
			break;
		}

		entry e;
		e.name = record.path.substr(path_plus_slash.size());
		struct stat buf;
		if (fstatat(fd, e.name.c_str(), &buf, 0) == 0) {
			e.filesize = buf.st_size;
			e.allocated = buf.st_blocks * 512;
			e.file_mtime = buf.st_mtim;
		} else if (record.filesize >= 0) {
			e.filesize = record.filesize;
			e.allocated = record.allocated;
		}
		e.checksum = move(record.checksum);
		e.is_directory = (record.dt.sec >= 0);
		e.db_modified = record.dt;
		db_entries.push_back(e);
	}

	DIR *dir = nullptr;
	vector<entry> entries;
	if (!existing_db->get_error() && db_modified.sec > 0 &&
	    modified.sec == db_modified.sec && modified.nsec == db_modified.nsec) {
		// Not changed since the last database, so we can replace the readdir()
		// by reading from the database. (We still need to open and stat everything,
		// though, but that happens in a later step.)
		entries = move(db_entries);
		if (conf_verbose) {
			for (const entry &e : entries) {
				printf("%s/%s\n", path.c_str(), e.name.c_str());
			}
		}
	} else {
		dir = fdopendir(fd);  // Takes over ownership of fd.
		if (dir == nullptr) {
			// fdopendir() wants to fstat() the fd to verify that it's indeed
			// a directory, which can seemingly fail on at least CIFS filesystems
			// if the server feels like it. We treat this as if we had an error
			// on opening it, ie., ignore the directory.
			close(fd);
			return 0;
		}

		dirent *de;
		while ((de = readdir(dir)) != nullptr) {
			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
				continue;
			}
			if (strlen(de->d_name) == 0) {
				/* Unfortunately, this does happen, and mere assert() does not give
				   users enough information to complain to the right people. */
				fprintf(stderr, "file system error: zero-length file name in directory %s", path.c_str());
				continue;
			}

			entry e;
			e.name = de->d_name;
			if (de->d_type == DT_UNKNOWN) {
				struct stat buf;
				if (fstatat(fd, de->d_name, &buf, AT_SYMLINK_NOFOLLOW) == 0 &&
				    S_ISDIR(buf.st_mode)) {
					e.is_directory = true;
				} else {
					e.is_directory = false;
					e.filesize = buf.st_size;
					e.allocated = buf.st_blocks * 512;
					e.file_mtime = buf.st_mtim;
				}
			} else {
				e.is_directory = (de->d_type == DT_DIR);
				struct stat buf;
				if (fstatat(fd, de->d_name, &buf, 0) == 0) {
					e.filesize = buf.st_size;
					e.allocated = buf.st_blocks * 512;
					e.file_mtime = buf.st_mtim;
				}
			}

			if (conf_verbose) {
				printf("%s/%s\n", path.c_str(), de->d_name);
			}
			entries.push_back(move(e));
		}

		sort(entries.begin(), entries.end(), [](const entry &a, const entry &b) {
			return a.name < b.name;
		});

		// Load directory modification times from the old database.
		auto db_it = db_entries.begin();
		for (entry &e : entries) {
			for (; db_it != db_entries.end(); ++db_it) {
				if (e.name < db_it->name) {
					break;
				}
			if (e.name == db_it->name) {
				e.db_modified = db_it->db_modified;
				e.checksum = db_it->checksum;
				break;
			}
			}
		}
	}

	// For each entry, we want to add it to the database. but this includes the modification time
	// for directories, which means we need to open and stat it at this point.
	//
	// This means we may need to have many directories open at the same time, but it seems to be
	// the simplest (only?) way of being compatible with mlocate's notion of listing all contents
	// of a given directory before recursing, without buffering even more information. Hopefully,
	// we won't go out of file descriptors here (it could happen if someone has tens of thousands
	// of subdirectories in a single directory); if so, the admin will need to raise the limit.
	for (entry &e : entries) {
		if (!e.is_directory) {
			e.dt = not_a_dir;
			continue;
		}

		if (find(conf_prunenames.begin(), conf_prunenames.end(), e.name) != conf_prunenames.end()) {
			if (conf_debug_pruning) {
				/* This is debugging output, don't mark anything for translation */
				fprintf(stderr, "Skipping `%s': in prunenames\n", e.name.c_str());
			}
			continue;
		}

		e.fd = opendir_noatime(fd, e.name.c_str());
		if (e.fd == -1) {
			if (errno == EMFILE || errno == ENFILE) {
				// The admin probably wants to know about this.
				perror((path_plus_slash + e.name).c_str());

				rlimit rlim;
				if (getrlimit(RLIMIT_NOFILE, &rlim) == -1) {
					fprintf(stderr, "Hint: Try `ulimit -n 131072' or similar.\n");
				} else {
					fprintf(stderr, "Hint: Try `ulimit -n %" PRIu64 " or similar (current limit is %" PRIu64 ").\n",
					        static_cast<uint64_t>(rlim.rlim_cur * 2), static_cast<uint64_t>(rlim.rlim_cur));
				}
				exit(1);
			}
			continue;
		}

		struct stat buf;
		if (fstat(e.fd, &buf) != 0) {
			// It's possible that this is a filesystem that's excluded
			// (and the failure is e.g. because the network is down).
			// As a last-ditch effort, we try to check that before dying,
			// i.e., duplicate the check from further down.
			//
			// It would be better to be able to run filesystem_is_excluded()
			// for cheap on everything and just avoid the stat, but it seems
			// hard to do that without any kind of raciness.
			if (filesystem_is_excluded((path_plus_slash + e.name).c_str())) {
				close(e.fd);
				e.fd = -1;
				continue;
			}

			perror((path_plus_slash + e.name).c_str());
			exit(1);
		}

		e.dev = buf.st_dev;
		if (buf.st_dev != parent_dev) {
			if (filesystem_is_excluded((path_plus_slash + e.name).c_str())) {
				close(e.fd);
				e.fd = -1;
				continue;
			}
		}

		e.dt = get_dirtime_from_stat(buf);
	}

	// Compute checksums where needed.
	if (!conf_checksum_xattr.empty() || !conf_checksum_command.empty()) {
		for (entry &e : entries) {
			if (e.is_directory) continue;
			if (e.filesize >= 0 && e.filesize < conf_min_checksum_size) {
				e.checksum.clear();
				continue;
			}
			bool needs_recompute = e.checksum.empty() ||
				(e.file_mtime.tv_sec > db_file_mtime.tv_sec ||
				 (e.file_mtime.tv_sec == db_file_mtime.tv_sec &&
				  e.file_mtime.tv_nsec > db_file_mtime.tv_nsec));
			if (needs_recompute) {
				string filepath = path_plus_slash + e.name;
				e.checksum = get_checksum_for_entry(filepath, e.file_mtime);
			}
		}
	}

	// Actually add all the entries we figured out dates for above.
	for (const entry &e : entries) {
		string entry_path = path_plus_slash + e.name;
		if (path_is_excluded(entry_path) || !path_is_included(entry_path)) continue;
		corpus->add_file(entry_path, e.dt, e.filesize, e.allocated, e.checksum);
		dict_builder->add_file(entry_path, e.dt);
	}

	// Now scan subdirectories.
	for (const entry &e : entries) {
		if (e.is_directory && e.fd != -1) {
			int ret = scan(path_plus_slash + e.name, e.fd, e.dev, e.dt, e.db_modified, existing_db, corpus, dict_builder);
			if (ret == -1) {
				// TODO: The unscanned file descriptors will leak, but it doesn't really matter,
				// as we're about to exit.
				closedir(dir);
				return -1;
			}
		}
	}

	if (dir == nullptr) {
		close(fd);
	} else {
		closedir(dir);
	}
	return 0;
}

int main(int argc, char **argv)
{
	// We want to bump the file limit; do it if we can (usually we are root
	// and can set whatever we want). 128k should be ample for most setups.
	rlimit rlim;
	if (getrlimit(RLIMIT_NOFILE, &rlim) != -1) {
		// Even root cannot increase rlim_cur beyond rlim_max,
		// so we need to try to increase rlim_max first.
		// Ignore errors, though.
		if (rlim.rlim_max < 131072) {
			rlim.rlim_max = 131072;
			setrlimit(RLIMIT_NOFILE, &rlim);
			getrlimit(RLIMIT_NOFILE, &rlim);
		}

		rlim_t wanted = std::max<rlim_t>(rlim.rlim_cur, 131072);
		rlim.rlim_cur = std::min<rlim_t>(wanted, rlim.rlim_max);
		setrlimit(RLIMIT_NOFILE, &rlim);  // Ignore errors.
	}

	conf_prepare(argc, argv);
	if (conf_prune_bind_mounts) {
		bind_mount_init(MOUNTINFO_PATH);
	}

	int fd = open(conf_output.c_str(), O_RDONLY);
	if (fd != -1) {
		struct stat db_stat;
		if (fstat(fd, &db_stat) == 0) {
			db_file_mtime = db_stat.st_mtim;
		}
	}
	ExistingDB existing_db(fd);

	DictionaryBuilder dict_builder(/*blocks_to_keep=*/1000, conf_block_size);

	gid_t owner = -1;
	if (conf_check_visibility) {
		group *grp = getgrnam(GROUPNAME);
		if (grp == nullptr) {
			fprintf(stderr, "Unknown group %s\n", GROUPNAME);
			exit(1);
		}
		owner = grp->gr_gid;
	}

	DatabaseBuilder db(conf_output.c_str(), owner, conf_block_size, existing_db.read_next_dictionary(), conf_check_visibility);
	db.set_conf_block(conf_block);
	DatabaseReceiver *corpus = db.start_corpus(/*store_dir_times=*/true, /*store_filesizes=*/true, /*store_checksums=*/true);

	int root_fd = opendir_noatime(AT_FDCWD, conf_scan_root);
	if (root_fd == -1) {
		perror(".");
		exit(1);
	}

	struct stat buf;
	if (fstat(root_fd, &buf) == -1) {
		perror(".");
		exit(1);
	}

	scan(conf_scan_root, root_fd, buf.st_dev, get_dirtime_from_stat(buf), /*db_modified=*/unknown_dir_time, &existing_db, corpus, &dict_builder);

	// It's too late to use the dictionary for the data we already compressed,
	// unless we wanted to either scan the entire file system again (acceptable
	// for plocate-build where it's cheap, less so for us), or uncompressing
	// and recompressing. Instead, we store it for next time, assuming that the
	// data changes fairly little from time to time.
	string next_dictionary = dict_builder.train(1024);
	db.set_next_dictionary(next_dictionary);
	db.finish_corpus();

	exit(EXIT_SUCCESS);
}
