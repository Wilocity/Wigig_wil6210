#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>

/*
 * Dumps firmware trace.
 *
 * Uses binary representation of 'strings' file,
 * it should be named fw_strings.bin and be in the current directory
 * Periodically reads peripheral memory like in blob_fw_peri on the debugfs
 * Name of peripheral memory file passed as parameter
 */

typedef uint32_t u32;
typedef int32_t s32;
typedef unsigned int uint;

struct module_level_enable {	/* Little Endian */
      uint	error_level_enable:1;
      uint	warn_level_enable:1;
      uint	info_level_enable:1;
      uint	verbose_level_enable:1;
      uint	reserved0:4;
} __attribute__((packed));

struct log_trace_header {	/* Little Endian */
	uint	strring_offset:20;          /* the offset of the trace string in the strings sections */
	uint	module:4;                   /* module that outputs the trace */
	uint	level:2;                    /*	0 - Error
						1- WARN
						2 - INFO
						3 - VERBOSE */
	uint	parameters_num:2;           /* [0..3] */
	uint	is_string:1;				/* this bit was timestamp_present:1; and changed to indicate if the printf uses %s */
	uint	signature:3;                /* should be 5 (2'101) in valid header */
} __attribute__((packed));

union log_event {
	struct log_trace_header hdr;
	u32 param;
} __attribute__((packed));

struct log_table_header {
	u32 write_ptr;                      /* incremented by trace producer every write */
	struct module_level_enable module_level_enable[16];
	union log_event evt[0];
} __attribute__((packed));

static size_t read_all(int f, void *buf, size_t n)
{
	size_t actual = 0, r;
	do {
		r = read(f, buf + actual, n - actual);
		actual += r;
	} while ((r > 0) && (actual < n));
	return actual;
}

static void *read_strings(const char *name, size_t *size)
{
	int f = open(name, O_RDONLY);
	size_t sz = *size;
	size_t r;
	void *buf;
	if (f < 0)
		return NULL;

	if (!sz) {
		sz = lseek(f, 0, SEEK_END);
		lseek(f, 0, SEEK_SET);
	}
	buf = malloc(sz);
	if (!buf) {
		close(f);
		return NULL;
	}
	r = read_all(f, buf, sz);
	close(f);
	if (r != sz) {
		printf("Error: from %s read %zd bytes out of %zd\n",
				name, r, sz);
		free(buf);
		return NULL;
	}
	*size = sz;
	return buf;
}

static int read_log(const char *fname, void *buf, size_t off, size_t size)
{
	int f = open(fname, O_RDONLY);
	lseek(f, off, SEEK_SET);
	size_t r = read_all(f, buf, size);
	close(f);
	if (r != size) {
		printf("Error: from %s read %zd bytes out of %zd\n",
				fname, r, size);
		return -1;
	}
	return 0;
}

static char *strings_bin;
enum {
	str_mask = 0xFFFF,
};

/* log parameters */
static char *peri; /* file to read */
static size_t log_offset = 0; /* offset of log buf in the file */
static size_t log_buf_entries = 0x1000/4; /* entries in the log buf */
static int once = 0;

static void *log_buf; /* memory allocated for the log buf */

static void *str_buf;
static size_t str_sz;
static u32 rptr = 0;
static const char* levels[] = {
	"E",
	"W",
	"I",
	"V",
};

static const char *modules[16];

static inline size_t log_size(size_t entry_num)
{
	return sizeof(struct log_table_header) + entry_num * 4;
}

static void do_parse(void)
{
	struct log_table_header *h = log_buf;
	u32 wptr = h->write_ptr;
	if ((wptr - rptr) >= log_buf_entries) {
		/* overflow; try to parse last wrap */
		rptr = wptr - log_buf_entries;
	}
	for (;(s32)(wptr - rptr) > 0; rptr++) {
		int i;
		u32 p[3] = {0};
		union log_event *evt = &h->evt[rptr % log_buf_entries];
		const char *fmt;
		if (evt->hdr.signature != 5)
			continue;
		if (evt->hdr.strring_offset > str_sz)
			continue;
		if (evt->hdr.parameters_num > 3)
			continue;
		fmt = str_buf + evt->hdr.strring_offset;
		for (i = 0; i < evt->hdr.parameters_num; i++) {
			p[i] = h->evt[(rptr + i + 1) % log_buf_entries].param;
		}
		printf("[%6d] %9s %s :", rptr, modules[evt->hdr.module],
				levels[evt->hdr.level]);
		if (evt->hdr.is_string) {
			printf(fmt, str_buf + (p[0] & str_mask),
					str_buf + (p[1] & str_mask),
					str_buf + (p[2] & str_mask));
		} else {
			printf(fmt, p[0], p[1], p[2]);
		}
		printf("\n");
		rptr += evt->hdr.parameters_num;
	}
	fflush(stdout);
}

static const char *next_mod(const char *mod)
{
	mod = strchr(mod, '\0') + 1;
	while (*mod == '\0')
		mod++;
	return mod;
}

int main(int argc, char* argv[])
{
	const char *mod;
	int c, i;
	static int help = 0;
	static struct option long_options[] = {
		{"memdump", required_argument, NULL, 'm'},
		{"offset", required_argument, NULL, 'o'},
		{"logsize", required_argument, NULL, 'l'},
		{"strings", required_argument, NULL, 's'},
		{"once", no_argument, &once, 1},
		{"help", no_argument, &help, 1},
		{}
	};
	do {
		c = getopt_long(argc, argv, "m:o:l:s:1", long_options, &i);
		switch (c) {
		case 0:
			break;
		case 'm': /* memdump */
			peri = optarg;
			break;
		case 'o': /* offset */
			sscanf(optarg, "%zi", &log_offset);
			break;
		case 'l': /* logsize */
			sscanf(optarg, "%zi", &log_buf_entries);
			break;
		case 's': /* strings */
			strings_bin = optarg;
			break;
		case '1':
			once = 1;
			break;
		case -1: /* end of options */
			break;
		default:
			help = 1;
		}
	} while(c >= 0);
	if ( !(peri && strings_bin && log_offset >=0 && log_buf_entries > 0) )
		help = 1;
	if (help) {
		static char *help_str = "Usage: trace [OPTION]...\n"
"Extract trace log from FW/Ucode\n"
"\n"
"Mandatory arguments to long options are mandatory for short options too.\n"
" The following switches are mandatory:\n"
"  -m, --memdump=FILE		File to read memory dump from\n"
"  -s, --strings=FILE		File with format strings\n"
"  -l, --logsize=NUMBER		Log buffer size, entries\n"
" The following switches are optional:\n"
"  -o, --offset=NUMBER		Offset of the log buffer in memdump, bytes\n"
"				Default is 0\n"
"  -1, --once			Read and parse once and exit, otherwise\n"
"				keep reading infinitely\n";
		printf("%s", help_str);
		exit(1);
	}

	printf("Using file: <%s> offset: 0x%zx entries: %zd strings: <%s> once: %d\n",
	       peri, log_offset, log_buf_entries, strings_bin, once);
	str_buf = read_strings(strings_bin, &str_sz);
	mod = str_buf;
	log_buf = malloc(log_size(log_buf_entries));
	if (!str_buf || !log_buf)
		return -1;
	struct log_table_header *h = log_buf;
	if (read_log(peri, log_buf, log_offset, log_size(log_buf_entries)))
		exit(1);
	u32 wptr = h->write_ptr;
	if ((wptr - rptr) >= log_buf_entries) {
		/* overflow; try to parse last wrap */
		rptr = wptr - log_buf_entries;
	}
	printf("  wptr = %d rptr = %d\n", wptr, rptr);
	for (i = 0; i < 16; i++, mod=next_mod(mod)) {
		modules[i] = mod;
		struct module_level_enable *m = &h->module_level_enable[i];
		printf("  %s[%2d] : %s%s%s%s\n", modules[i], i,
				m->error_level_enable   ? "E" : " ",
				m->warn_level_enable    ? "W" : " ",
				m->info_level_enable    ? "I" : " ",
				m->verbose_level_enable ? "V" : " ");
	}
	for(;;) {
		do_parse();
		if (once)
			break;
		usleep(100*1000);
		if (read_log(peri, log_buf, log_offset, log_size(log_buf_entries)))
			break;
	}
	return 0;
}
