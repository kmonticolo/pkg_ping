/*
 * BSD 2-Clause License
 *
 * Copyright (c) 2017, 2018, 2019, Luke N Small, lukensmall@gmail.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
 * WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


/*
 * Special thanks to "Dan Mclaughlin" on misc@ for the ftp to sed idea
 *
 * "
 * ftp -o - http://www.openbsd.org/ftp.html | \
 * sed -n \
 *  -e 's:</a>$::' \
 *      -e 's:  <strong>\([^<]*\)<.*:\1:p' \
 *      -e 's:^\(       [hfr].*\):\1:p'
 * "
 */

/*
	indent pkg_ping.c -bap -br -ce -ci4 -cli0 -d0 -di0 -i8 \
	-ip -l79 -nbc -ncdb -ndj -ei -nfc1 -nlp -npcs -psl -sc -sob

	cc pkg_ping.c -pipe -o pkg_ping
 */

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

struct mirror_st {
	double diff;
	char *ftp_file;
	char *label;
};

static int
diff_cmp(const void *a, const void *b)
{
	struct mirror_st **one = (struct mirror_st **) a;
	struct mirror_st **two = (struct mirror_st **) b;

	if ((*one)->diff < (*two)->diff)
		return -1;
	if ((*one)->diff > (*two)->diff)
		return 1;
	return 0;
}

static int
ftp_cmp(const void *a, const void *b)
{
	struct mirror_st **one = (struct mirror_st **) a;
	struct mirror_st **two = (struct mirror_st **) b;

	return strcmp((*one)->ftp_file, (*two)->ftp_file);
}


static int
label_cmp(const void *a, const void *b)
{
	struct mirror_st **one = (struct mirror_st **) a;
	struct mirror_st **two = (struct mirror_st **) b;

	/* list the USA mirrors first, it will subsort correctly */
	int8_t temp = !strncmp("USA", (*one)->label, 3);
	if (temp != !strncmp("USA", (*two)->label, 3)) {
		if (temp)
			return -1;
		return 1;
	}
	return strcmp((*one)->label, (*two)->label);
}


static void
manpage(char a[])
{
	printf("%s\n", a);
	printf("[-f (don't write to File even if run as root)]\n");

	printf("[-h (print this Help message and exit)]\n");

	printf("[-O (if your kernel is a snapshot, it will Override it and ");
	printf("search for release kernel mirrors.\n");
	printf("\tIt could be used to determine whether the release is ");
	printf("present.\n");

	printf("[-S (\"Secure\" https mirrors instead. Secrecy is preserved ");
	printf("at the price of performance.\n");
	printf("\t\"insecure\" mirrors still preserve file integrity!)]\n");

	printf("[-s floating-point timeout in Seconds (eg. -s 2.3)]\n");

	printf("[-u (no USA mirrors to comply ");
	printf("with USA encryption export laws)]\n");

	printf("[-v (increase Verbosity. It recognizes up to 3 of these)]\n");
	
	printf("[-V (no Verbose output. No output but error messages)]\n");
}

int
main(int argc, char *argv[])
{
	int8_t f = (getuid() == 0) ? 1 : 0;
	int8_t num, current, insecure, u, verbose, override;
	double s, S;
	pid_t ftp_pid, sed_pid, write_pid;
	int kq, i, pos, c, n, array_max, array_length, tag_len;
	int parent_to_write[2], ftp_to_sed[2], sed_to_parent[2], block_pipe[2];
	char *tag, *tag2;
	FILE *input, *pkg_write;
	struct mirror_st **array;
	struct kevent ke;
	struct timeval tv_start, tv_end;
	struct timespec timeout, timeout0 = { 20, 0 };
	
	if (unveil("/usr/bin/ftp", "x") == -1)
		err(EXIT_FAILURE, "unveil line: %d", __LINE__);

	if (unveil("/usr/bin/sed", "x") == -1)
		err(EXIT_FAILURE, "unveil line: %d", __LINE__);

	if (f) {

		if (unveil("/etc/installurl", "crw") == -1)
			err(EXIT_FAILURE, "unveil line: %d", __LINE__);

		if (pledge("stdio proc exec cpath rpath wpath", NULL) == -1)
			err(EXIT_FAILURE, "pledge line: %d", __LINE__);
	} else if (pledge("stdio proc exec", NULL) == -1)
		err(EXIT_FAILURE, "pledge line: %d", __LINE__);

	array_max = 100;

	array = calloc(array_max, sizeof(struct mirror_st *));
	if (array == NULL) err(EXIT_FAILURE, "calloc line: %d", __LINE__);

	s = 5;
	u = 0;
	verbose = 0;
	insecure = 1;
	current = 0;
	override = 0;

	char *version;
	size_t len = 300;
	version = malloc(len);
	if (version == NULL) err(EXIT_FAILURE, "malloc line: %d\n", __LINE__);

	/* stores results of "sysctl kern.version" into 'version' */
	const int mib[2] = { CTL_KERN, KERN_VERSION };
	if (sysctl(mib, 2, version, &len, NULL, 0) == -1)
                   err(EXIT_FAILURE, "sysctl line: %d", __LINE__);
	
	/* Discovers if the kernel is not a release version */
	if (strstr(version, "beta"))
		current = 1;
	else if (strstr(version, "current"))
		current = 1;
		
	free(version);

	while ((c = getopt(argc, argv, "fhOSs:uvV")) != -1) {
		switch (c) {
		case 'f':
			if (f == 1) {
				if (pledge("stdio proc exec", NULL) == -1) {
					err(EXIT_FAILURE,
					    "pledge line: %d", __LINE__);
				}
				f = 0;
			}
			break;
		case 'h':
			manpage(argv[0]);
			return 0;
		case 'O':
			override = 1;
			break;
		case 'S':
			insecure = 0;
			break;
		case 's':
			c = -1;
			i = n = 0;
			while (optarg[++c] != '\0') {
				if (optarg[c] >= '0' && optarg[c] <= '9')
				{
					n = 1;
					continue;
				}
				if (optarg[c] == '.' && ++i == 1)
					continue;

				if (optarg[c] == '-')
					errx(EXIT_FAILURE,
					    "No negative sign.");
				errx(EXIT_FAILURE,
				    "Bad floating point format.");
			}
			if (n == 0)
				errx(EXIT_FAILURE,
				    "-s needs a numeric character.");
			errno = 0;
			s = strtod(optarg, NULL);
			if (errno == ERANGE)
				err(EXIT_FAILURE, "strtod");
			if (s > 1000.0)
				errx(EXIT_FAILURE, "-s should be <= 1000");
			if (s <= 0.01)
				errx(EXIT_FAILURE, "-s should be > 0.01");
			break;
		case 'u':
			u = 1;
			break;
		case 'v':
			if (verbose == -1)
				break;
			if (++verbose > 3)
				verbose = 3;
			break;
		case 'V':
			verbose = -1;
			break;
		default:
			manpage(argv[0]);
			return EXIT_FAILURE;
		}
	}
	if (optind < argc) {
		manpage(argv[0]);
		errx(EXIT_FAILURE, "non-option ARGV-element: %s", argv[optind]);
	}

	

	if (verbose > 1) {
		if (current == 1) {
			if (override == 0)
				printf("This is a snapshot!\n\n");
			else {
				printf("This is a snapshot, ");
				printf("but it has been overridden ");
				printf("to show release mirrors!\n\n");
				current = 0;
			}
		} else {
			if (override == 0)
				printf("This is a release.\n\n");
			else {
				printf("This is a release, ");
				printf("but it has been overridden ");
				printf("to show snapshot mirrors!\n\n");
				current = 1;
			}
		}
	}
	

	struct utsname *name = malloc(sizeof(struct utsname));
	if (name == NULL) err(EXIT_FAILURE, "malloc line: %d", __LINE__);
	if (uname(name) == -1) err(EXIT_FAILURE, "uname line: %d", __LINE__);
	
	char release[4 + 1];
	strlcpy(release, name->release, 4 + 1);

	if (current == 0) {
		tag_len = strlen("/") + strlen(release) + strlen("/") +
		    strlen(name->machine) + strlen("/SHA256");
	} else {
		tag_len = strlen("/") + strlen("snapshots") + strlen("/") +
		    strlen(name->machine) + strlen("/SHA256");
	}

	tag = calloc(tag_len - 1 + 1, sizeof(char));
	if (tag == NULL)
		err(EXIT_FAILURE, "calloc line: %d", __LINE__);

	if (current == 0)
		strlcpy(tag, release, tag_len);
	else
		strlcpy(tag, "snapshots", tag_len);

	strlcat(tag, "/", tag_len);
	strlcat(tag, name->machine, tag_len);
	strlcat(tag, "/SHA256", tag_len);

	free(name);

	if (f) {
		
		if (pipe(parent_to_write) == -1)
			err(EXIT_FAILURE, "pipe line: %d", __LINE__);

		write_pid = fork();
		if (write_pid == (pid_t) 0) {

			if (pledge("stdio cpath rpath wpath", NULL) == -1) {
				printf("pledge line: %d\n", __LINE__);
				_exit(EXIT_FAILURE);
			}
			
			close(parent_to_write[STDOUT_FILENO]);
						
			kq = kqueue();
			if (kq == -1) {
				printf("kq! line: %d\n", __LINE__);
				_exit(EXIT_FAILURE);
			}
			
			EV_SET(&ke, parent_to_write[STDIN_FILENO], EVFILT_READ,
				EV_ADD | EV_ONESHOT, 0, 0, NULL);
			if (kevent(kq, &ke, 1, &ke, 1, NULL) == -1) {
				printf("parent_to_write ");
				printf("kevent register fail.\n");
				_exit(EXIT_FAILURE);
			}
			close(kq);
			
			/* no data sent through pipe */
			if (ke.data == 0) {
				printf("/etc/installurl not written.\n");
				_exit(EXIT_FAILURE);
			}
			
			input = fdopen(parent_to_write[STDIN_FILENO], "r");
			if (input == NULL) {
				printf("input = fdopen ");
				printf("(parent_to_write[STDIN");
				printf("_FILENO], \"r\") line %d\n", __LINE__);
				_exit(EXIT_FAILURE);
			}
			
			/* provide an extra space to ALWAYS null terminate */
			tag2 = malloc(300 + 1);
			if (tag2 == NULL) {
				printf("malloc line: %d\n", __LINE__);
				_exit(EXIT_FAILURE);
			}
				
			i = 0;
			if (verbose >= 1)
				printf("\n");
			while ((c = getc(input)) != EOF) {
				if (i >= 300) {
					printf("\nmirror length ");
					printf("became too long.\n");
					
					printf("/etc/installurl");
					printf(" not written.\n");
					_exit(EXIT_FAILURE);
				}

				tag2[i++] = c;
				if (c == '\n')
					break;
			}
			fclose(input);
			tag2[i] = '\0';
			
			tag2 = realloc(tag2, i + 1);
			if (tag2 == NULL) {
				printf("realloc line: %d", __LINE__);
				_exit(EXIT_FAILURE);
			}

			pkg_write = fopen("/etc/installurl", "w");

			if (pledge("stdio", NULL) == -1) {
				printf("pledge line: %d\n", __LINE__);
				_exit(EXIT_FAILURE);
			}
			
			if (pkg_write != NULL && c == '\n') {
				fwrite(tag2, i, sizeof(char), pkg_write);
				fclose(pkg_write);
				if (verbose >= 0 && current == 1) {
					printf("Perhaps it's time to ");
					printf("get the release!\n");
				}
				if (verbose >= 0)
					printf("/etc/installurl: %s", tag2);
				_exit(EXIT_SUCCESS);
			}
			
			printf("/etc/installurl not written.\n");
			_exit(EXIT_FAILURE);
		}
		if (write_pid == -1)
			err(EXIT_FAILURE, "write fork line: %d", __LINE__);
			
		if (pledge("stdio proc exec", NULL) == -1)
			err(EXIT_FAILURE, "pledge line: %d", __LINE__);

		close(parent_to_write[STDIN_FILENO]);
	}


	timeout.tv_sec = (time_t) s;
	timeout.tv_nsec =
	    (long) ((s - (double) timeout.tv_sec) * 1000000000.0);

	kq = kqueue();
	if (kq == -1)
		err(EXIT_FAILURE, "kq! line: %d", __LINE__);


	if (pipe(ftp_to_sed) == -1)
		err(EXIT_FAILURE, "pipe line: %d", __LINE__);

	ftp_pid = fork();
	if (ftp_pid == (pid_t) 0) {

		if (pledge("stdio exec", NULL) == -1) {
			printf("ftp pledge 1 line: %d\n", __LINE__);
			_exit(EXIT_FAILURE);
		}
		
		close(ftp_to_sed[STDIN_FILENO]);

		if (dup2(ftp_to_sed[STDOUT_FILENO], STDOUT_FILENO) == -1) {
			printf("ftp STDOUT dup2 line: %d\n", __LINE__);
			_exit(EXIT_FAILURE);
		}
		
		if (verbose >= 2) {
			fprintf(stderr,
			    "fetching https://www.openbsd.org/ftp.html\n");
			execl("/usr/bin/ftp", "ftp", "-vmo", "-",
			    "https://www.openbsd.org/ftp.html", NULL);
		} else {
			execl("/usr/bin/ftp", "ftp", "-VMo", "-",
			    "https://www.openbsd.org/ftp.html", NULL);
		}

		if (pledge("stdio", NULL) == -1) {
			fprintf(stderr, "ftp pledge 2 line: %d\n", __LINE__);
			_exit(EXIT_FAILURE);
		}
		fprintf(stderr, "ftp execl() failed line: %d\n", __LINE__);
		_exit(EXIT_FAILURE);
	}
	if (ftp_pid == -1)
		err(EXIT_FAILURE, "ftp 1 fork line: %d", __LINE__);

	close(ftp_to_sed[STDOUT_FILENO]);

	if (pipe(sed_to_parent) == -1) {
		n = errno;
		kill(ftp_pid, SIGKILL);
		errno = n;
		err(EXIT_FAILURE, "pipe line: %d", __LINE__);
	}
	sed_pid = fork();
	if (sed_pid == (pid_t) 0) {

		if (pledge("stdio exec", NULL) == -1) {
			printf("sed pledge 2 line: %d\n", __LINE__);
			_exit(EXIT_FAILURE);
		}
		close(sed_to_parent[STDIN_FILENO]);

		if (dup2(ftp_to_sed[STDIN_FILENO], STDIN_FILENO) == -1) {
			printf("sed STDIN dup2 line: %d\n", __LINE__);
			_exit(EXIT_FAILURE);
		}
		if (dup2(sed_to_parent[STDOUT_FILENO], STDOUT_FILENO) == -1) {
			printf("sed STDOUT dup2 line: %d\n", __LINE__);
			_exit(EXIT_FAILURE);
		}
		execl("/usr/bin/sed", "sed", "-n",
		    "-e", "s:</a>$::",
		    "-e", "s:\t<strong>\\([^<]*\\)<.*:\\1:p",
		    "-e", "s:^\\(\t[hfr].*\\):\\1:p", NULL);

		if (pledge("stdio", NULL) == -1) {
			fprintf(stderr, "sed pledge 3 line: %d\n", __LINE__);
			_exit(EXIT_FAILURE);
		}
		fprintf(stderr, "sed execl() line: %d\n", __LINE__);
		_exit(EXIT_FAILURE);
	}
	if (sed_pid == -1) {
		n = errno;
		kill(ftp_pid, SIGKILL);
		errno = n;
		err(EXIT_FAILURE, "sed fork line: %d", __LINE__);
	}

	close(ftp_to_sed[STDIN_FILENO]);
	close(sed_to_parent[STDOUT_FILENO]);

	EV_SET(&ke, sed_to_parent[STDIN_FILENO], EVFILT_READ,
	    EV_ADD | EV_ONESHOT, 0, 0, NULL);
	i = kevent(kq, &ke, 1, &ke, 1, &timeout0);
	if (i == -1) {
		kill(ftp_pid, SIGKILL);
		kill(sed_pid, SIGKILL);
		printf("kevent, timeout0 ");
		printf("may be too large. line: %d\n", __LINE__);
		return 1;
	}
	if (i == 0) {
		kill(ftp_pid, SIGKILL);
		kill(sed_pid, SIGKILL);
		printf("timed out fetching: ");
		printf("https://www.openbsd.org/ftp.html\n");
		return EXIT_FAILURE;
	}
	input = fdopen(sed_to_parent[STDIN_FILENO], "r");
	if (input == NULL) {
		n = errno;
		kill(ftp_pid, SIGKILL);
		kill(sed_pid, SIGKILL);
		errno = n;
		err(EXIT_FAILURE,
		    "fdopen(sed_to_parent[STDIN_FILENO]...) line %d", __LINE__);
	}
	/* if the index for line[] exceeds 299, it will error out */
	char *line = calloc(300, sizeof(char));
	if (line == NULL) err(EXIT_FAILURE, "calloc line: %d", __LINE__);


	num = pos = array_length = 0;
	array[0] = malloc(sizeof(struct mirror_st));
	if (array[0] == NULL) {
		n = errno;
		kill(ftp_pid, SIGKILL);
		kill(sed_pid, SIGKILL);
		errno = n;
		err(EXIT_FAILURE, "malloc line: %d", __LINE__);
	}
	while ((c = getc(input)) != EOF) {
		if (pos >= 300) {
			kill(ftp_pid, SIGKILL);
			kill(sed_pid, SIGKILL);
			errx(EXIT_FAILURE,
			    "pos got too big! line: %d", __LINE__);
		}
		if (num == 0) {
			if (c != '\n') {
				line[pos++] = c;
				continue;
			}
			line[pos++] = '\0';
			if (u) {
				if (!strncmp("USA", line, 3)) {
					while ((c = getc(input)) != EOF) {
						if (c == '\n')
							break;
					}
					if (c == EOF)
						break;
					pos = 0;
					continue;
				}
			}
			array[array_length]->label = 
			    malloc(pos);
			if (array[array_length]->label == NULL) {
				n = errno;
				kill(ftp_pid, SIGKILL);
				kill(sed_pid, SIGKILL);
				errno = n;
				err(EXIT_FAILURE,
				    "malloc line: %d", __LINE__);
			}
			strlcpy(array[array_length]->label, line, pos);

			pos = 0;
			num = 1;
		} else {
			if (pos == 0) {
				if ((c != 'h') && (c != 'f') && (c != 'r'))
					continue;
				else if (insecure) {
					if (c == 'r')
						break;
					if (c == 'f') {
						line[pos++] = 'h';
						c = 't';
					}
				} else if (c != 'h')
					break;
			}
			if (c != '\n') {
				line[pos++] = c;
				continue;
			}
			if (!insecure) {
				if (strncmp(line, "https", 5))
					break;
			} else if (!strncmp(line, "https", 5)) {
				free(array[array_length]->label);
				num = pos = 0;
				continue;
			}
			line[pos++] = '\0';

			pos += tag_len - 1;

			array[array_length]->ftp_file = malloc(pos);
			    
			if (array[array_length]->ftp_file == NULL) {
				n = errno;
				kill(ftp_pid, SIGKILL);
				kill(sed_pid, SIGKILL);
				errno = n;
				err(EXIT_FAILURE,
				    "malloc line: %d", __LINE__);
			}
			
			strlcpy(array[array_length]->ftp_file, line, pos);
			strlcat(array[array_length]->ftp_file, tag, pos);

			if (++array_length >= array_max) {
				array_max += 20;
				array = reallocarray(array, array_max,
				    sizeof(struct mirror_st *));

				if (array == NULL) {
					n = errno;
					kill(ftp_pid, SIGKILL);
					kill(sed_pid, SIGKILL);
					errno = n;
					err(EXIT_FAILURE,
					    "reallocarray line: %d", __LINE__);
				}
			}
			array[array_length] = malloc(sizeof(struct mirror_st));

			if (array[array_length] == NULL) {
				n = errno;
				kill(ftp_pid, SIGKILL);
				kill(sed_pid, SIGKILL);
				errno = n;
				err(EXIT_FAILURE,
				    "malloc line: %d", __LINE__);
			}
			pos = num = 0;
		}
	}
	fclose(input);
	free(tag);
	free(line);

	close(sed_to_parent[STDIN_FILENO]);

	kill(ftp_pid, SIGKILL);
	kill(sed_pid, SIGKILL);
	waitpid(ftp_pid, NULL, 0);
	waitpid(sed_pid, NULL, 0);

	if (array_length == 0)
		errx(EXIT_FAILURE, "No mirror found. Is www.openbsd.org live?");

	if (num == 1)
		free(array[array_length]->label);
	free(array[array_length]);

	
	if (insecure) {
		
		qsort(array, array_length, sizeof(struct mirror_st *), ftp_cmp);
		c = 1;
		while (c < array_length) {
			if (!strcmp(array[c - 1]->ftp_file,
			    array[c]->ftp_file)) {
				free(array[c - 1]->label);
				free(array[c - 1]->ftp_file);
				free(array[c - 1]);
				for (i = c; i < array_length; ++i)
					array[i - 1] = array[i];
				--array_length;
			} else
				++c;
		}
	}

	array = reallocarray(array, array_length, sizeof(struct mirror_st *));
	if (array == NULL) err(EXIT_FAILURE, "reallocarray line: %d", __LINE__);
		
	qsort(array, array_length, sizeof(struct mirror_st *), label_cmp);
	
	S = s;

	for (c = 0; c < array_length; ++c) {
		if (verbose >= 2) {
			if (verbose == 3)
				printf("\n");
			if (array_length >= 100) {
				printf("\n%3d : %s  :  %s\n", array_length - c,
				    array[c]->label, array[c]->ftp_file);
			} else {
				printf("\n%2d : %s  :  %s\n", array_length - c,
				    array[c]->label, array[c]->ftp_file);
			}
		} else if (verbose >= 0) {
			i = array_length - c;
			if (c > 0) {
				if ((i == 9) || (i == 99))
					printf("\b \b");
				n = i;
				do {
					printf("\b");
					n /= 10;
				} while (n > 0);
			}
			printf("%d", i);
			fflush(stdout);
		}

		if (pipe(block_pipe) == -1)
			err(EXIT_FAILURE, "pipe line: %d", __LINE__);

		ftp_pid = fork();
		if (ftp_pid == (pid_t) 0) {

			if (pledge("stdio exec", NULL) == -1) {
				printf("ftp pledge 3 line: %d\n", __LINE__);
				_exit(EXIT_FAILURE);
			}
			
			close(block_pipe[STDOUT_FILENO]);
			read(block_pipe[STDIN_FILENO], &n, sizeof(int));
			close(block_pipe[STDIN_FILENO]);

			if (verbose == 3) {
				execl("/usr/bin/ftp", "ftp", "-vmo",
				    "/dev/null", array[c]->ftp_file, NULL);
			} else {
				i = open("/dev/null", O_WRONLY);
				if (i != -1)
					dup2(i, STDERR_FILENO);
				execl("/usr/bin/ftp", "ftp", "-VMo",
				    "/dev/null", array[c]->ftp_file, NULL);
			}

			if (pledge("stdio", NULL) == -1) {
				printf("ftp pledge 4 line: %d\n", __LINE__);
				_exit(EXIT_FAILURE);
			}
			printf("ftp execl() failed line: %d\n", __LINE__);
			_exit(EXIT_FAILURE);
		}
		if (ftp_pid == -1)
			err(EXIT_FAILURE, "ftp 2 fork line: %d", __LINE__);


		close(block_pipe[STDIN_FILENO]);

		EV_SET(&ke, ftp_pid, EVFILT_PROC, EV_ADD | EV_ONESHOT,
		    NOTE_EXIT, 0, NULL);
		if (kevent(kq, &ke, 1, NULL, 0, NULL) == -1) {
			n = errno;
			kill(ftp_pid, SIGKILL);
			errno = n;
			err(EXIT_FAILURE,
			    "kevent register fail line: %d", __LINE__);
		}
		gettimeofday(&tv_start, NULL);

		close(block_pipe[STDOUT_FILENO]);


		/* While we're waiting... */
		array[c]->diff = 0;
		n = strlen(array[c]->ftp_file) - tag_len;
		array[c]->ftp_file[n] = '\0';
		array[c]->ftp_file = realloc(array[c]->ftp_file, n + 1);
		if (array[c]->ftp_file == NULL) {
			n = errno;
			kill(ftp_pid, SIGKILL);
			errno = n;
			err(EXIT_FAILURE, "realloc line: %d", __LINE__);
		}


		n = 0;

		/* Loop until ftp() is dead and 'ke' is populated */
		for (;;) {
			i = kevent(kq, NULL, 0, &ke, 1, &timeout);
			if (i == -1) {
				n = errno;
				kill(ftp_pid, SIGKILL);
				errno = n;
				err(EXIT_FAILURE, "kevent line: %d", __LINE__);
			}
			if (i == 0) {
				if (verbose >= 2) {
					n = 1;
					printf("Timeout\n");
				}
				kill(ftp_pid, SIGKILL);
				array[c]->diff = s;
			} else
				break;
		}
		
		/* return value of ftp() */
		if (ke.data == 0) {
			gettimeofday(&tv_end, NULL);

			double dt =
			    (double)(tv_end.tv_sec - tv_start.tv_sec) +
			    (double)(tv_end.tv_usec - tv_start.tv_usec) /
			    1000000.0;

			if (array[c]->diff == 0)
				array[c]->diff = dt;

			if (array[c]->diff > S) 
				array[c]->diff = S;
			if (verbose >= 2) {
				if (array[c]->diff == s) {
					if (n == 0)
						printf("Timeout\n");
				} else
					printf("%f\n", array[c]->diff);
			} else if (verbose <= 0 && S > array[c]->diff) {
				S = array[c]->diff;
				timeout.tv_sec = (time_t) S;
				timeout.tv_nsec =
				    (long) ((S - (double) timeout.tv_sec)
				    * 1000000000.0);
			}
		} else if (array[c]->diff == 0) {
			array[c]->diff = s + 1;
			if (verbose >= 2)
				printf("Download Error\n");
		}
		waitpid(ftp_pid, NULL, 0);
	}

	if (pledge("stdio", NULL) == -1)
		err(EXIT_FAILURE, "pledge line: %d", __LINE__);
		
	close(kq);

	if (verbose == 0 || verbose == 1) {
		printf("\b \b");
		fflush(stdout);
	}

	qsort(array, array_length, sizeof(struct mirror_st *), diff_cmp);

	if (verbose >= 1) {
		
		int ts = -1, te = -1,   ds = -1, de = -1,   ss = -1;
		
		for (c = array_length - 1; c >= 0; --c) {
			if (array[c]->diff < s) {
				ss = 0;
				break;
			}
			else if (array[c]->diff == s) {
				if (ts == -1) 
					ts = te = c;
				else
					ts = c;
			} else {
				if(ds == -1) 
					ds = de = c;
				else
					ds = c;
			}
		}
		
		if (ts != -1) {
			qsort(array + ts, 1 + te - ts, 
			    sizeof(struct mirror_st *), label_cmp);
		}
		
		if (ds != -1) {
			qsort(array + ds, 1 + de - ds,
			    sizeof(struct mirror_st *), label_cmp);
		}
		
		c = array_length - 1;
		
		if (array[c]->diff < s)
			printf("\n\nSUCCESSFUL MIRRORS:\n\n\n");
		else if (array[c]->diff == s)
			printf("\n\nTIMEOUT MIRRORS:\n\n\n");
		else
			printf("\n\nDOWNLOAD ERROR MIRRORS:\n\n\n");

		for (; c >= 0; --c) {
			    
			if (array_length >= 100)
				printf("%3d", c + 1);
			else
				printf("%2d", c + 1);
			
			printf(" : %s:\n\techo ", array[c]->label);
			printf("\"%s\" > /etc/installurl",
			    array[c]->ftp_file);

			if (array[c]->diff < s)
				printf(" : %f\n\n", array[c]->diff);
			else if (array[c]->diff == s) {
				//~ printf(" Timeout");
				printf("\n\n");
				if (c == ts && ss != -1)
					printf("\nSUCCESSFUL MIRRORS:\n\n\n");
			} else {
				//~ printf(" Download Error");
				printf("\n\n");
				if (c == ds && ts != -1)
					printf("\nTIMEOUT MIRRORS:\n\n\n");
				else if (c == ds && ss != -1)
					printf("\nSUCCESSFUL MIRRORS:\n\n\n");
			}
			    
		}
	}

	if (array[0]->diff >= s) {
		if (current == 0) {
			printf("\n\n");
			printf("No mirrors. It doesn't appear that the ");
			printf("%s release is present yet.\n", release);
			return EXIT_FAILURE;
		} else
			errx(EXIT_FAILURE, "No successful mirrors found.");
	}
	
	if (f) {
		if (dup2(parent_to_write[STDOUT_FILENO], STDOUT_FILENO) == -1) {
			printf("dup2 line: %d\n", __LINE__);
			
			if (verbose < 0)
				return EXIT_FAILURE;
			
			printf("Since this process is root, type:\n");
			printf("echo \"%s\" > /etc/installurl\n",
			    array[0]->ftp_file);
			return EXIT_FAILURE;
		}
		
		for (c = 1; c < array_length; ++c) {
			free(array[c]->ftp_file);
			free(array[c]->label);
			free(array[c]);
		}
		
		/* sends the fastest mirror to the 'write' process */
		printf("%s\n", array[0]->ftp_file);

		waitpid(write_pid, &i, 0);

		return i;
	}
	
	if (verbose < 0)
		return EXIT_SUCCESS;
	
	if (getuid() == 0) {
		printf("If you are still root, type:\n");
		printf("echo \"%s\" > /etc/installurl\n",
		    array[0]->ftp_file);
	} else {
		printf("As root, type:\necho \"%s\" > /etc/installurl\n",
		    array[0]->ftp_file);
	}

	return EXIT_SUCCESS;
}
