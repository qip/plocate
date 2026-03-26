#include "complete_pread.h"
#include "db.h"

#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <zstd.h>

using namespace std;

struct PathMapping {
	string prefix;  // e.g. "/catalyst/"
	string drive;   // e.g. "Z:"
};

static void usage()
{
	fprintf(stderr,
	        "Usage: plocate2csv [-d DBPATH] [--map /prefix/:DRIVE:] ...\n"
	        "\n"
	        "Read a plocate database and output WizTree-format CSV.\n"
	        "\n"
	        "  -d, --database DBPATH   path to plocate.db (default: plocate.db)\n"
	        "  -m, --map FROM:TO       replace path prefix FROM with TO\n"
	        "                          e.g. --map /catalyst/:Z:\n"
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

// Parse a mapping argument "FROM:TO" where TO is a drive letter like "Z:".
// The colon inside "Z:" makes this tricky; we split on the last two-char
// sequence matching X: where X is an ascii letter.
static bool parse_mapping(const char *arg, PathMapping *out)
{
	string s(arg);
	// Find the separator colon: look for ":X:" pattern or just split
	// at the last colon that is followed by a letter and colon.
	// Simple approach: the TO part is always the last 2 chars like "Z:"
	// Format: /prefix/:Z:  (the middle colon is separator)
	// So we find the second-to-last colon.
	if (s.size() < 4) return false;

	// Find last ':'
	size_t last = s.rfind(':');
	if (last == string::npos || last == 0) return false;
	// The drive letter is s[last-1], the separator colon is at last-2
	if (last < 2) return false;
	size_t sep = last - 2;
	if (s[sep] != ':') return false;

	out->prefix = s.substr(0, sep);
	out->drive = s.substr(sep + 1);
	return true;
}

int main(int argc, char **argv)
{
	string dbpath = "plocate.db";
	vector<PathMapping> mappings;

	static const struct option long_options[] = {
		{ "database", required_argument, 0, 'd' },
		{ "map", required_argument, 0, 'm' },
		{ "help", no_argument, 0, 'h' },
		{ 0, 0, 0, 0 }
	};

	for (;;) {
		int option_index = 0;
		int c = getopt_long(argc, argv, "d:m:h", long_options, &option_index);
		if (c == -1) break;
		switch (c) {
		case 'd':
			dbpath = optarg;
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

	ZSTD_DCtx *ctx = ZSTD_createDCtx();

	printf("File Name,Size,Allocated,Modified,Attributes,Files,Folders\n");

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

			// Each entry is "path,filesize,allocated"
			// Split on the last two commas (same logic as plocate.cpp).
			const char *last_comma = strrchr(p, ',');
			if (last_comma == nullptr || last_comma == p) {
				// No metadata, output path with defaults.
				string path(p);
				if (!mappings.empty()) path = apply_mappings(path, mappings);
				slash_to_backslash(path);
				printf("\"%s\",0,0,1970/01/01 00:00:00,0,0,0\n", path.c_str());
				continue;
			}
			const char *second_last_comma = last_comma - 1;
			while (second_last_comma > p && *second_last_comma != ',') {
				--second_last_comma;
			}
			if (*second_last_comma != ',') {
				string path(p);
				if (!mappings.empty()) path = apply_mappings(path, mappings);
				slash_to_backslash(path);
				printf("\"%s\",0,0,1970/01/01 00:00:00,0,0,0\n", path.c_str());
				continue;
			}

			string path(p, second_last_comma - p);
			string size_str(second_last_comma + 1, last_comma - second_last_comma - 1);
			string alloc_str(last_comma + 1);

			if (!mappings.empty()) path = apply_mappings(path, mappings);
			slash_to_backslash(path);

			printf("\"%s\",%s,%s,1970/01/01 00:00:00,0,0,0\n",
			       path.c_str(), size_str.c_str(), alloc_str.c_str());
		}
	}

	ZSTD_freeDCtx(ctx);
	if (ddict != nullptr) ZSTD_freeDDict(ddict);
	close(fd);
	return 0;
}
