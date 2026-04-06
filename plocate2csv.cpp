#include "complete_pread.h"
#include "db.h"

#include <algorithm>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <zstd.h>

using namespace std;

struct PathMapping {
	string prefix;  // e.g. "/catalyst/"
	string drive;   // e.g. "Z:"
};

struct StringReplace {
	string from;
	string to;
};

struct FileEntry {
	string path;
	long long size;
	long long allocated;
};

struct DirStats {
	long long total_size = 0;
	long long total_allocated = 0;
	long long file_count = 0;
	long long folder_count = 0;
	bool in_db = false;
};

static void usage()
{
	fprintf(stderr,
	        "Usage: plocate2csv [-d DBPATH] [-f PREFIX] [--map /prefix/:DRIVE:] ...\n"
	        "\n"
	        "Read a plocate database and output WizTree-format CSV.\n"
	        "\n"
	        "  -d, --database DBPATH   path to plocate.db (default: plocate.db)\n"
	        "  -f, --filter PREFIX     only include paths starting with PREFIX\n"
	        "                          e.g. --filter /catalyst\n"
	        "  -m, --map FROM:TO       replace path prefix FROM with TO\n"
	        "                          e.g. --map /catalyst/:Z:\n"
	        "  -s, --replace FROM:TO   replace all occurrences of FROM with TO in paths\n"
	        "                          e.g. --replace foo:bar\n"
	        "      --help              print this help\n");
}

static string apply_mappings(const string &path, const vector<PathMapping> &mappings)
{
	for (const auto &m : mappings) {
		if (path.size() >= m.prefix.size() &&
		    path.compare(0, m.prefix.size(), m.prefix) == 0) {
			return m.drive + "/" + path.substr(m.prefix.size());
		}
	}
	return path;
}

static void slash_to_backslash(string &s)
{
	for (char &c : s) {
		if (c == '/') c = '\\';
	}
}

static void apply_replacements(string &s, const vector<StringReplace> &replacements)
{
	for (const auto &r : replacements) {
		size_t pos = 0;
		while ((pos = s.find(r.from, pos)) != string::npos) {
			s.replace(pos, r.from.size(), r.to);
			pos += r.to.size();
		}
	}
}

static string csv_escape(const string &s)
{
	string out;
	out.reserve(s.size());
	for (char c : s) {
		if (c == '"') out += '"';
		out += c;
	}
	return out;
}

// Parse a mapping argument "FROM:TO" where TO is a drive letter like "Z:".
// Format: /prefix/:Z:  (the middle colon is separator)
static bool parse_mapping(const char *arg, PathMapping *out)
{
	string s(arg);
	if (s.size() < 4) return false;

	size_t last = s.rfind(':');
	if (last == string::npos || last == 0) return false;
	if (last < 2) return false;
	size_t sep = last - 2;
	if (s[sep] != ':') return false;

	out->prefix = s.substr(0, sep);
	out->drive = s.substr(sep + 1);
	return true;
}

// Walk all '/' positions in path and call fn(parent) for each ancestor.
template <typename Fn>
static void for_each_ancestor(const string &path, Fn fn)
{
	size_t pos = 0;
	while ((pos = path.find('/', pos + 1)) != string::npos) {
		fn(path.substr(0, pos));
	}
}

int main(int argc, char **argv)
{
	string dbpath = "plocate.db";
	string filter_prefix;
	vector<PathMapping> mappings;
	vector<StringReplace> replacements;

	static const struct option long_options[] = {
		{ "database", required_argument, 0, 'd' },
		{ "filter", required_argument, 0, 'f' },
		{ "map", required_argument, 0, 'm' },
		{ "replace", required_argument, 0, 's' },
		{ "help", no_argument, 0, 'h' },
		{ 0, 0, 0, 0 }
	};

	for (;;) {
		int option_index = 0;
		int c = getopt_long(argc, argv, "d:f:m:s:h", long_options, &option_index);
		if (c == -1) break;
		switch (c) {
		case 'd':
			dbpath = optarg;
			break;
		case 'f':
			filter_prefix = optarg;
			break;
		case 'm': {
			PathMapping m;
			if (!parse_mapping(optarg, &m)) {
				fprintf(stderr, "Invalid mapping: %s\n", optarg);
				fprintf(stderr, "Expected format: /prefix/:X:  (e.g. /catalyst/:Z:)\n");
				return 1;
			}
			mappings.push_back(move(m));
			break;
		}
		case 's': {
			string arg(optarg);
			size_t sep = arg.find(':');
			if (sep == string::npos || sep == 0) {
				fprintf(stderr, "Invalid replacement: %s\n", optarg);
				fprintf(stderr, "Expected format: FROM:TO  (e.g. foo:bar)\n");
				return 1;
			}
			replacements.push_back({ arg.substr(0, sep), arg.substr(sep + 1) });
			break;
		}
		case 'h':
			usage();
			return 0;
		default:
			usage();
			return 1;
		}
	}

	int fd = open(dbpath.c_str(), O_RDONLY);
	if (fd == -1) {
		perror(dbpath.c_str());
		return 1;
	}

	Header hdr;
	complete_pread(fd, &hdr, sizeof(hdr), 0);
	if (memcmp(hdr.magic, "\0plocate", 8) != 0) {
		fprintf(stderr, "%s: not a plocate database\n", dbpath.c_str());
		return 1;
	}
	if (hdr.version != 0 && hdr.version != 1) {
		fprintf(stderr, "%s: unsupported version %u\n", dbpath.c_str(), hdr.version);
		return 1;
	}
	if (hdr.version == 0) {
		hdr.zstd_dictionary_offset_bytes = 0;
		hdr.zstd_dictionary_length_bytes = 0;
	}
	if (hdr.max_version < 3) {
		hdr.filesize_data_length_bytes = 0;
		hdr.filesize_data_offset_bytes = 0;
		hdr.block_size = 0;
	}

	ZSTD_DDict *ddict = nullptr;
	if (hdr.zstd_dictionary_length_bytes > 0) {
		string dictionary;
		dictionary.resize(hdr.zstd_dictionary_length_bytes);
		complete_pread(fd, &dictionary[0], hdr.zstd_dictionary_length_bytes,
		               hdr.zstd_dictionary_offset_bytes);
		ddict = ZSTD_createDDict(dictionary.data(), dictionary.size());
	}

	uint32_t num_blocks = hdr.num_docids;
	vector<uint64_t> offsets(num_blocks + 1);
	complete_pread(fd, offsets.data(), (num_blocks + 1) * sizeof(uint64_t),
	               hdr.filename_index_offset_bytes);

	// Decompress the filesize stream if present.
	string filesize_data;
	if (hdr.filesize_data_length_bytes > 0) {
		string compressed_fs(hdr.filesize_data_length_bytes, '\0');
		complete_pread(fd, &compressed_fs[0], hdr.filesize_data_length_bytes,
		               hdr.filesize_data_offset_bytes);

		unsigned long long fs_uncompressed =
			ZSTD_getFrameContentSize(compressed_fs.data(), compressed_fs.size());
		if (fs_uncompressed != ZSTD_CONTENTSIZE_UNKNOWN &&
		    fs_uncompressed != ZSTD_CONTENTSIZE_ERROR) {
			filesize_data.resize(fs_uncompressed);
			ZSTD_DCtx *fs_ctx = ZSTD_createDCtx();
			size_t ret = ZSTD_decompressDCtx(fs_ctx, &filesize_data[0], filesize_data.size(),
			                                  compressed_fs.data(), compressed_fs.size());
			if (ZSTD_isError(ret)) {
				fprintf(stderr, "filesize stream: ZSTD_decompress: %s\n", ZSTD_getErrorName(ret));
				filesize_data.clear();
			}
			ZSTD_freeDCtx(fs_ctx);
		}
	}
	const int64_t *filesize_ptr = reinterpret_cast<const int64_t *>(filesize_data.data());
	const int64_t *filesize_end = reinterpret_cast<const int64_t *>(filesize_data.data() + filesize_data.size());

	ZSTD_DCtx *ctx = ZSTD_createDCtx();

	// --- Pass 1: read all entries into memory ---
	vector<FileEntry> entries;
	entries.reserve(1 << 20);

	for (uint32_t block = 0; block < num_blocks; ++block) {
		size_t compressed_len = offsets[block + 1] - offsets[block];
		string compressed(compressed_len, '\0');
		complete_pread(fd, &compressed[0], compressed_len, offsets[block]);

		unsigned long long uncompressed_len =
			ZSTD_getFrameContentSize(compressed.data(), compressed.size());
		if (uncompressed_len == ZSTD_CONTENTSIZE_UNKNOWN ||
		    uncompressed_len == ZSTD_CONTENTSIZE_ERROR) {
			fprintf(stderr, "Block %u: ZSTD_getFrameContentSize() failed\n", block);
			return 1;
		}

		string data;
		data.resize(uncompressed_len + 1);

		size_t err;
		if (ddict != nullptr) {
			err = ZSTD_decompress_usingDDict(ctx, &data[0], data.size(),
			                                  compressed.data(), compressed.size(), ddict);
		} else {
			err = ZSTD_decompressDCtx(ctx, &data[0], data.size(),
			                           compressed.data(), compressed.size());
		}
		if (ZSTD_isError(err)) {
			fprintf(stderr, "Block %u: ZSTD_decompress: %s\n", block, ZSTD_getErrorName(err));
			return 1;
		}
		data[data.size() - 1] = '\0';

		for (const char *p = data.data(); p < data.data() + data.size(); p += strlen(p) + 1) {
			if (*p == '\0') continue;
			FileEntry e;
			e.path = string(p);
			if (filesize_ptr + 1 < filesize_end) {
				e.size = filesize_ptr[0];
				e.allocated = filesize_ptr[1];
				filesize_ptr += 2;
			} else {
				e.size = 0;
				e.allocated = 0;
			}
			if (!filter_prefix.empty()) {
				bool path_under_prefix =
					e.path.size() >= filter_prefix.size() &&
					e.path.compare(0, filter_prefix.size(), filter_prefix) == 0 &&
					(e.path.size() == filter_prefix.size() || e.path[filter_prefix.size()] == '/');
				bool prefix_under_path =
					filter_prefix.size() > e.path.size() &&
					filter_prefix.compare(0, e.path.size(), e.path) == 0 &&
					filter_prefix[e.path.size()] == '/';
				if (!path_under_prefix && !prefix_under_path) continue;
			}
			entries.push_back(move(e));
		}
	}

	ZSTD_freeDCtx(ctx);
	if (ddict != nullptr) ZSTD_freeDDict(ddict);
	close(fd);

	// --- Pass 2: identify directories ---
	// A path is a directory if it appears as a proper prefix component of another entry.
	unordered_set<string> dir_set;
	for (const auto &e : entries) {
		for_each_ancestor(e.path, [&](const string &parent) {
			dir_set.insert(parent);
		});
	}

	// --- Pass 3: aggregate stats for each directory ---
	unordered_map<string, DirStats> dir_stats;
	for (const auto &d : dir_set) {
		dir_stats[d];
	}

	for (const auto &e : entries) {
		if (dir_set.count(e.path)) {
			dir_stats[e.path].in_db = true;
		}
	}

	for (const auto &e : entries) {
		bool is_dir = dir_set.count(e.path);
		long long sz = max(e.size, 0LL);
		long long alloc = max(e.allocated, 0LL);

		for_each_ancestor(e.path, [&](const string &parent) {
			auto &ds = dir_stats[parent];
			if (is_dir) {
				ds.folder_count++;
			} else {
				ds.total_size += sz;
				ds.total_allocated += alloc;
				ds.file_count++;
			}
		});
	}

	// --- Pass 4: output ---
	printf("File Name,Size,Allocated,Modified,Attributes,Files,Folders\n");

	for (const auto &e : entries) {
		bool is_dir = dir_set.count(e.path);
		string path = e.path;
		if (!mappings.empty()) path = apply_mappings(path, mappings);
		slash_to_backslash(path);
		if (!replacements.empty()) apply_replacements(path, replacements);

		if (is_dir) {
			const auto &ds = dir_stats[e.path];
			printf("\"%s\\\",%lld,%lld,1970/01/01 00:00:00,0,%lld,%lld\n",
			       csv_escape(path).c_str(), ds.total_size, ds.total_allocated,
			       ds.file_count, ds.folder_count);
		} else {
			long long sz = max(e.size, 0LL);
			long long alloc = max(e.allocated, 0LL);
			printf("\"%s\",%lld,%lld,1970/01/01 00:00:00,0,0,0\n",
			       csv_escape(path).c_str(), sz, alloc);
		}
	}

	// Output implied parent directories not present as db entries.
	for (const auto &[dirpath, ds] : dir_stats) {
		if (ds.in_db) continue;
		string path = dirpath;
		if (!mappings.empty()) path = apply_mappings(path, mappings);
		slash_to_backslash(path);
		if (!replacements.empty()) apply_replacements(path, replacements);
		printf("\"%s\\\",%lld,%lld,1970/01/01 00:00:00,0,%lld,%lld\n",
		       csv_escape(path).c_str(), ds.total_size, ds.total_allocated,
		       ds.file_count, ds.folder_count);
	}

	return 0;
}
