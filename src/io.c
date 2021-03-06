/* Copyright (c) 2006-2014 Jonas Fonseca <jonas.fonseca@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "tig/tig.h"
#include "tig/util.h"
#include "tig/io.h"

/*
 * Encoding conversion.
 */

#define ENCODING_SEP	": encoding: "
#define ENCODING_ARG	"--encoding=" ENCODING_UTF8

#define CHARSET_SEP	"; charset="

struct encoding {
	struct encoding *next;
	iconv_t cd;
	char fromcode[1];
};

char encoding_arg[] = ENCODING_ARG;
struct encoding *default_encoding;
static struct encoding *encodings;

struct encoding *
encoding_open(const char *fromcode)
{
	struct encoding *encoding;
	size_t len = strlen(fromcode);

	if (!*fromcode)
		return NULL;

	for (encoding = encodings; encoding; encoding = encoding->next) {
		if (!strcasecmp(encoding->fromcode, fromcode))
			return encoding;
	}

	encoding = calloc(1, sizeof(*encoding) + len);
	strncpy(encoding->fromcode, fromcode, len);
	encoding->cd = iconv_open(ENCODING_UTF8, fromcode);
	if (encoding->cd == ICONV_NONE) {
		free(encoding);
		return NULL;
	}

	encoding->next = encodings;
	encodings = encoding;

	return encoding;
}

static char *
encoding_convert_string(iconv_t iconv_cd, char *line, size_t linelen)
{
	static char out_buffer[BUFSIZ * 2];
	ICONV_CONST char *inbuf = line;
	size_t inlen = linelen + 1;

	char *outbuf = out_buffer;
	size_t outlen = sizeof(out_buffer);

	size_t ret = iconv(iconv_cd, &inbuf, &inlen, &outbuf, &outlen);

	return (ret != (size_t) -1) ? out_buffer : line;
}

char *
encoding_convert(struct encoding *encoding, char *line)
{
	return encoding_convert_string(encoding->cd, line, strlen(line));
}

const char *
encoding_iconv(iconv_t iconv_cd, const char *string, size_t length)
{
	char *instr = strndup(string, length);
	const char *ret = instr ? encoding_convert_string(iconv_cd, instr, length) : string;

	free(instr);
	return ret == instr ? string : ret;
}

struct encoding *
get_path_encoding(const char *path, struct encoding *default_encoding)
{
	const char *check_attr_argv[] = {
		"git", "check-attr", "encoding", "--", path, NULL
	};
	char buf[SIZEOF_STR];
	char *encoding;

	/* <path>: encoding: <encoding> */

	if (!*path || !io_run_buf(check_attr_argv, buf, sizeof(buf))
	    || !(encoding = strstr(buf, ENCODING_SEP)))
		return default_encoding;

	encoding += STRING_SIZE(ENCODING_SEP);
	if (!strcmp(encoding, ENCODING_UTF8)
	    || !strcmp(encoding, "unspecified")
	    || !strcmp(encoding, "set")) {
		const char *file_argv[] = {
			"file", "-I", "--", path, NULL
		};

		if (!*path || !io_run_buf(file_argv, buf, sizeof(buf))
		    || !(encoding = strstr(buf, CHARSET_SEP)))
			return default_encoding;

		encoding += STRING_SIZE(CHARSET_SEP);
	}

	return encoding_open(encoding);
}

/*
 * Executing external commands.
 */

static void
io_init(struct io *io)
{
	memset(io, 0, sizeof(*io));
	io->pipe = -1;
}

bool
io_open(struct io *io, const char *fmt, ...)
{
	char name[SIZEOF_STR] = "";
	int retval;

	io_init(io);

	FORMAT_BUFFER(name, sizeof(name), fmt, retval, FALSE);
	if (retval < 0) {
		io->error = ENAMETOOLONG;
		return FALSE;
	}

	io->pipe = *name ? open(name, O_RDONLY) : dup(STDIN_FILENO);
	if (io->pipe == -1)
		io->error = errno;
	return io->pipe != -1;
}

bool
io_kill(struct io *io)
{
	return io->pid == 0 || kill(io->pid, SIGKILL) != -1;
}

bool
io_done(struct io *io)
{
	pid_t pid = io->pid;

	if (io->pipe != -1)
		close(io->pipe);
	free(io->buf);
	io_init(io);

	while (pid > 0) {
		int status;
		pid_t waiting = waitpid(pid, &status, 0);

		if (waiting < 0) {
			if (errno == EINTR)
				continue;
			io->error = errno;
			return FALSE;
		}

		if (WEXITSTATUS(status)) {
			io->status = WEXITSTATUS(status);
		}

		return waiting == pid &&
		       !WIFSIGNALED(status) &&
		       !io->status;
	}

	return TRUE;
}

static int
open_trace(int devnull, const char *argv[])
{
	static const char *trace_file;

	if (!trace_file) {
		trace_file = getenv("TIG_TRACE");
		if (!trace_file)
			trace_file = "";
	}

	if (*trace_file) {
		int fd = open(trace_file, O_RDWR | O_CREAT | O_APPEND, 0666);
		int i;

		for (i = 0; argv[i]; i++) {
			if (write(fd, argv[i], strlen(argv[i])) == -1
			    || write(fd, " ", 1) == -1)
				break;
		}
		if (argv[i] || write(fd, "\n", 1) == -1) {
			close(fd);
			return devnull;
		}

		return fd;
	}

	return devnull;
}

bool
io_trace(const char *fmt, ...)
{
	static FILE *trace_out; /* Intensionally leaked. */
	va_list args;
	int retval;

	if (!trace_out) {
		const char *trace_file = getenv("TIG_TRACE");

		if (trace_file)
			trace_out = fopen(trace_file, "a");
		if (!trace_out)
			return FALSE;
	}

	va_start(args, fmt);
	retval = vfprintf(trace_out, fmt, args);
	va_end(args);
	fflush(trace_out);

	return retval != -1;
}

bool
io_exec(struct io *io, enum io_type type, const char *dir, char * const env[], const char *argv[], int custom)
{
	int pipefds[2] = { -1, -1 };
	bool read_from_stdin = type == IO_RD && (custom & IO_RD_FORWARD_STDIN);
	bool read_with_stderr = type == IO_RD && (custom & IO_RD_WITH_STDERR);

	io_init(io);

	if (dir && !strcmp(dir, argv[0]))
		return io_open(io, "%s%s", dir, argv[1]);

	if ((type == IO_RD || type == IO_WR) && pipe(pipefds) < 0) {
		io->error = errno;
		return FALSE;
	} else if (type == IO_AP) {
		pipefds[1] = custom;
	}

	if ((io->pid = fork())) {
		if (io->pid == -1)
			io->error = errno;
		if (pipefds[!(type == IO_WR)] != -1)
			close(pipefds[!(type == IO_WR)]);
		if (io->pid != -1) {
			io->pipe = pipefds[!!(type == IO_WR)];
			return TRUE;
		}

	} else {
		if (type != IO_FG) {
			int devnull = open("/dev/null", O_RDWR);
			int readfd  = type == IO_WR ? pipefds[0] : devnull;
			int writefd = (type == IO_RD || type == IO_AP)
							? pipefds[1] : devnull;
			int errorfd = open_trace(devnull, argv);

			/* Inject stdin given on the command line. */
			if (read_from_stdin)
				readfd = dup(STDIN_FILENO);

			dup2(readfd,  STDIN_FILENO);
			dup2(writefd, STDOUT_FILENO);
			if (read_with_stderr)
				dup2(writefd, STDERR_FILENO);
			else
				dup2(errorfd, STDERR_FILENO);

			if (devnull != errorfd)
				close(errorfd);
			close(devnull);
			if (pipefds[0] != -1)
				close(pipefds[0]);
			if (pipefds[1] != -1)
				close(pipefds[1]);
		}

		if (dir && *dir && chdir(dir) == -1)
			exit(errno);

		if (env) {
			int i;

			for (i = 0; env[i]; i++)
				if (*env[i])
					putenv(env[i]);
		}

		execvp(argv[0], (char *const*) argv);
		exit(errno);
	}

	if (pipefds[!!(type == IO_WR)] != -1)
		close(pipefds[!!(type == IO_WR)]);
	return FALSE;
}

bool
io_run(struct io *io, enum io_type type, const char *dir, char * const env[], const char *argv[])
{
	return io_exec(io, type, dir, env, argv, 0);
}

bool
io_complete(enum io_type type, const char **argv, const char *dir, int fd)
{
	struct io io;

	return io_exec(&io, type, dir, NULL, argv, fd) && io_done(&io);
}

bool
io_run_bg(const char **argv)
{
	return io_complete(IO_BG, argv, NULL, -1);
}

bool
io_run_fg(const char **argv, const char *dir)
{
	return io_complete(IO_FG, argv, dir, -1);
}

bool
io_run_append(const char **argv, int fd)
{
	return io_complete(IO_AP, argv, NULL, fd);
}

bool
io_eof(struct io *io)
{
	return io->eof;
}

int
io_error(struct io *io)
{
	return io->error;
}

char *
io_strerror(struct io *io)
{
	return strerror(io->error);
}

bool
io_can_read(struct io *io, bool can_block)
{
	struct timeval tv = { 0, 500 };
	fd_set fds;

	FD_ZERO(&fds);
	FD_SET(io->pipe, &fds);

	return select(io->pipe + 1, &fds, NULL, NULL, can_block ? NULL : &tv) > 0;
}

ssize_t
io_read(struct io *io, void *buf, size_t bufsize)
{
	do {
		ssize_t readsize = read(io->pipe, buf, bufsize);

		if (readsize < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		else if (readsize == -1)
			io->error = errno;
		else if (readsize == 0)
			io->eof = 1;
		return readsize;
	} while (1);
}

char *
io_memchr(struct io *io, char *data, int c)
{
	char *pos;

	if (data < io->buf || io->bufpos <= data)
		return NULL;

	pos = memchr(data, c, io->bufpos - data - 1);
	return pos ? pos + 1 : NULL;
}

DEFINE_ALLOCATOR(io_realloc_buf, char, BUFSIZ)

char *
io_get(struct io *io, int c, bool can_read)
{
	char *eol;
	ssize_t readsize;

	while (TRUE) {
		if (io->bufsize > 0) {
			eol = memchr(io->bufpos, c, io->bufsize);
			if (eol) {
				char *line = io->bufpos;

				*eol = 0;
				io->bufpos = eol + 1;
				io->bufsize -= io->bufpos - line;
				return line;
			}
		}

		if (io_eof(io)) {
			if (io->bufsize) {
				char *line = io->bufpos;

				io->bufpos[io->bufsize] = 0;
				io->bufpos += io->bufsize;
				io->bufsize = 0;
				return line;
			}
			return NULL;
		}

		if (!can_read)
			return NULL;

		if (io->bufsize > 0 && io->bufpos > io->buf)
			memmove(io->buf, io->bufpos, io->bufsize);

		if (io->bufalloc == io->bufsize) {
			if (!io_realloc_buf(&io->buf, io->bufalloc, BUFSIZ))
				return NULL;
			io->bufalloc += BUFSIZ;
		}

		io->bufpos = io->buf;
		readsize = io_read(io, io->buf + io->bufsize, io->bufalloc - io->bufsize);
		if (io_error(io))
			return NULL;
		io->bufsize += readsize;
	}
}

bool
io_write(struct io *io, const void *buf, size_t bufsize)
{
	size_t written = 0;

	while (!io_error(io) && written < bufsize) {
		ssize_t size;

		size = write(io->pipe, buf + written, bufsize - written);
		if (size < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		else if (size == -1)
			io->error = errno;
		else
			written += size;
	}

	return written == bufsize;
}

bool
io_printf(struct io *io, const char *fmt, ...)
{
	char buf[SIZEOF_STR] = "";
	int retval;

	FORMAT_BUFFER(buf, sizeof(buf), fmt, retval, FALSE);
	if (retval < 0) {
		io->error = ENAMETOOLONG;
		return FALSE;
	}

	return io_write(io, buf, retval);
}

bool
io_read_buf(struct io *io, char buf[], size_t bufsize)
{
	char *result = io_get(io, '\n', TRUE);

	if (result) {
		result = chomp_string(result);
		string_ncopy_do(buf, bufsize, result, strlen(result));
	}

	return io_done(io) && result;
}

bool
io_run_buf(const char **argv, char buf[], size_t bufsize)
{
	struct io io;

	return io_run(&io, IO_RD, NULL, NULL, argv) && io_read_buf(&io, buf, bufsize);
}

bool
io_from_string(struct io *io, const char *str)
{
	size_t len = strlen(str);

	io_init(io);

	if (!io_realloc_buf(&io->buf, io->bufalloc, len))
		return FALSE;

	io->bufsize = io->bufalloc = len;
	io->bufpos = io->buf;
	io->eof = TRUE;
	strncpy(io->buf, str, len);

	return TRUE;
}

int
io_load(struct io *io, const char *separators,
	io_read_fn read_property, void *data)
{
	char *name;
	int state = OK;

	while (state == OK && (name = io_get(io, '\n', TRUE))) {
		char *value;
		size_t namelen;
		size_t valuelen;

		name = chomp_string(name);
		namelen = strcspn(name, separators);

		if (name[namelen]) {
			name[namelen] = 0;
			value = chomp_string(name + namelen + 1);
			valuelen = strlen(value);

		} else {
			value = "";
			valuelen = 0;
		}

		state = read_property(name, namelen, value, valuelen, data);
	}

	if (state != ERR && io_error(io))
		state = ERR;
	io_done(io);

	return state;
}

int
io_run_load(const char **argv, const char *separators,
	    io_read_fn read_property, void *data)
{
	struct io io;

	if (!io_run(&io, IO_RD, NULL, NULL, argv))
		return ERR;
	return io_load(&io, separators, read_property, data);
}

const char *
get_temp_dir(void)
{
	static const char *tmp;

	if (tmp)
		return tmp;

	if (!tmp)
		tmp = getenv("TMPDIR");
	if (!tmp)
		tmp = getenv("TEMP");
	if (!tmp)
		tmp = getenv("TMP");
	if (!tmp)
		tmp = "/tmp";

	return tmp;
}

/* vim: set ts=8 sw=8 noexpandtab: */
