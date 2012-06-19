/*
 * Allocate and dirty memory.
 *
 * Usage: usemem size[k|m|g|t]
 *
 * gcc -lpthread -O -g -Wall  usemem.c -o usemem
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/sem.h>
#include <sys/time.h>

#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))

#define min(x, y) ({				\
	typeof(x) _min1 = (x);			\
	typeof(y) _min2 = (y);			\
	(void) (&_min1 == &_min2);		\
	_min1 < _min2 ? _min1 : _min2; })

#define max(x, y) ({				\
	typeof(x) _max1 = (x);			\
	typeof(y) _max2 = (y);			\
	(void) (&_max1 == &_max2);		\
	_max1 > _max2 ? _max1 : _max2; })


char *ourname;
int pagesize;
unsigned long bytes = 0;
unsigned long unit = 0;
unsigned long step = 0;
unsigned long *prealloc;
int sleep_secs = 0;
time_t runtime_secs = 0;
struct timeval start_time;
int reps = 1;
int do_mlock = 0;
int do_getchar = 0;
int opt_randomise = 0;
int opt_readonly;
int opt_openrw;
int opt_malloc;
int opt_detach;
int sem_id = -1;
int nr_task;
int nr_thread;
int quiet = 0;
int msync_mode;
char *filename = "/dev/zero";
char *pid_filename;
int map_shared = MAP_PRIVATE;
int map_populate;
int fd;


void usage(int ok)
{
	fprintf(stderr,
	"Usage: %s [options] size[k|m|g|t]\n"
	"    -n|--nproc N        do job in N processes\n"
	"    -t|--thread M       do job in M threads\n"
	"    -a|--malloc         obtain memory from malloc()\n"
	"    -f|--file FILE      mmap FILE, default /dev/zero\n"
	"    -F|--prefault       prefault mmap with MAP_POPULATE\n"
	"    -P|--prealloc       allocate memory before fork\n"
	"    -u|--unit SIZE      allocate memory in SIZE chunks\n"
	"    -j|--step SIZE      step size in the read/write loop\n"
	"    -r|--repeat N       repeat read/write N times\n"
	"    -o|--readonly       readonly access\n"
	"    -w|--open-rw        open() and mmap() file in RW mode\n"
	"    -R|--random         random access pattern\n"
	"    -M|--mlock          mlock() the memory\n"
	"    -S|--msync          msync(MS_SYNC) the memory\n"
	"    -A|--msync-async    msync(MS_ASYNC) the memory\n"
	"    -d|--detach         detach before go sleeping\n"
	"    -s|--sleep SEC      sleep SEC seconds when done\n"
	"    -T|--runtime SEC    terminate after SEC seconds\n"
	"    -p|--pid-file FILE  store detached pid to FILE\n"
	"    -g|--getchar        wait for <enter> before quitting\n"
	"    -q|--quiet          operate quietly\n"
	"    -h|--help           show this message\n"
	,		ourname);

	exit(ok);
}

static const struct option opts[] = {
	{ "malloc"	, 0, NULL, 'a' },
	{ "unit"	, 1, NULL, 'u' },
	{ "step"	, 1, NULL, 'j' },
	{ "mlock"	, 0, NULL, 'M' },
	{ "readonly"	, 0, NULL, 'o' },
	{ "open-rw"	, 0, NULL, 'w' },
	{ "quiet"	, 0, NULL, 'q' },
	{ "random"	, 0, NULL, 'R' },
	{ "msync"	, 0, NULL, 'S' },
	{ "msync-async"	, 0, NULL, 'A' },
	{ "nproc"	, 1, NULL, 'n' },
	{ "thread"	, 1, NULL, 't' },
	{ "prealloc"	, 0, NULL, 'P' },
	{ "prefault"	, 0, NULL, 'F' },
	{ "repeat"	, 1, NULL, 'r' },
	{ "file"	, 1, NULL, 'f' },
	{ "pid-file"	, 1, NULL, 'p' },
	{ "detach"	, 0, NULL, 'd' },
	{ "sleep"	, 1, NULL, 's' },
	{ "runtime"	, 1, NULL, 'T' },
	{ "getchar"	, 0, NULL, 'g' },
	{ "help"	, 0, NULL, 'h' },
	{ NULL		, 0, NULL, 0 }
};

/**
 *	memparse - parse a string with mem suffixes into a number
 *	@ptr: Where parse begins
 *	@retptr: (output) Optional pointer to next char after parse completes
 *
 *	Parses a string into a number.	The number stored at @ptr is
 *	potentially suffixed with %K (for kilobytes, or 1024 bytes),
 *	%M (for megabytes, or 1048576 bytes), or %G (for gigabytes, or
 *	1073741824).  If the number is suffixed with K, M, or G, then
 *	the return value is the number multiplied by one kilobyte, one
 *	megabyte, or one gigabyte, respectively.
 */

unsigned long long memparse(const char *ptr, char **retptr)
{
	char *endptr;	/* local pointer to end of parsed string */

	unsigned long long ret = strtoull(ptr, &endptr, 0);

	switch (*endptr) {
	case 'T':
	case 't':
		ret <<= 10;
	case 'G':
	case 'g':
		ret <<= 10;
	case 'M':
	case 'm':
		ret <<= 10;
	case 'K':
	case 'k':
		ret <<= 10;
		endptr++;
	default:
		break;
	}

	if (retptr)
		*retptr = endptr;

	return ret;
}

static inline void os_random_seed(unsigned long seed, struct drand48_data *rs)
{
        srand48_r(seed, rs);
}

static inline long os_random_long(unsigned long max, struct drand48_data *rs)
{
        long val;

        lrand48_r(rs, &val);
	return (unsigned long)((double)max * val / (RAND_MAX + 1.0));
}

/*
 * semaphore get/put wrapper
 */
int down(int sem_id)
{
	struct sembuf buf = {
		.sem_num = 0,
		.sem_op  = -1,
		.sem_flg = SEM_UNDO,
	};
        return semop(sem_id, &buf, 1);
}

int up(int sem_id)
{
	struct sembuf buf = {
		.sem_num = 0,
		.sem_op  = 1,
		.sem_flg = SEM_UNDO,
	};
        return semop(sem_id, &buf, 1);
}

void delete_pid_file(void)
{
	if (!pid_filename)
		return;

	if (unlink(pid_filename) < 0)
		perror(pid_filename);
}

void update_pid_file(pid_t pid)
{
	FILE *file;

	if (!pid_filename)
		return;

	file = fopen(pid_filename, "w");
	if (!file) {
		perror(pid_filename);
		exit(1);
	}

	fprintf(file, "%d\n", pid);
	fclose(file);
}


void sighandler(int sig, siginfo_t *si, void *arg)
{
	up(sem_id);
	delete_pid_file();

	fprintf(stderr, "Fatal signal %d\n", sig);
	exit(1);
}

void detach(void)
{
	pid_t pid;

	sem_id = semget(IPC_PRIVATE, 1, 0666|IPC_CREAT|IPC_EXCL);
	if (sem_id == -1) {
		perror("semget");
		exit(1);
	}
	if (semctl(sem_id, 0, SETVAL, 0) == -1) {
		perror("semctl");
		if (semctl(sem_id, 0, IPC_RMID) == -1)
			perror("sem_id IPC_RMID");
		exit(1);
	}

	pid = fork();
	if (pid < 0) {
		perror("fork");
		exit(1);
	}

	if (pid) { /* parent */
		update_pid_file(pid);
		if (down(sem_id))
			perror("down");
		semctl(sem_id, 0, IPC_RMID);
		exit(0);
	}

	if (pid == 0) { /* child */
		struct sigaction sa = {
			.sa_sigaction = sighandler,
			.sa_flags = SA_SIGINFO,
		};

		/*
		 * XXX: SIGKILL cannot be caught.
		 * The parent will wait for ever if child is OOM killed.
		 */
		sigaction(SIGBUS, &sa, NULL);

		if (atexit(delete_pid_file) != 0) {
			fprintf(stderr, "cannot set exit function\n");
			exit(1);
		}
	}
}

unsigned long * allocate(unsigned long bytes)
{
	unsigned long *p;

	if (!bytes)
		return NULL;

	if (opt_malloc) {
		p = malloc(bytes);
		if (p == NULL) {
			perror("malloc");
			exit(1);
		}
	} else {
		p = mmap(NULL, bytes, (opt_readonly && !opt_openrw) ?
			 PROT_READ : PROT_READ|PROT_WRITE,
			 map_shared|map_populate, fd, 0);
		if (p == MAP_FAILED) {
			fprintf(stderr, "%s: mmap failed: %s\n",
				ourname, strerror(errno));
			exit(1);
		}
		p = (unsigned long *)ALIGN((unsigned long )p, pagesize - 1);
	}

	if (do_mlock && mlock(p, bytes) < 0) {
		perror("mlock");
		exit(1);
	}

	return p;
}

int runtime_exceeded(void)
{
	struct timeval now;

	if (!runtime_secs)
		return 0;

	gettimeofday(&now, NULL);
	return (now.tv_sec - start_time.tv_sec > runtime_secs);
}

int do_unit(unsigned long bytes, struct drand48_data *rand_data)
{
	unsigned long i;
	unsigned long *p;
	int rep;

	p = prealloc ? : allocate(bytes);

	for (rep = 0; rep < reps; rep++) {
		unsigned long m = bytes / sizeof(*p);

		if (rep > 0 && !quiet) {
			printf(".");
			fflush(stdout);
		}

		for (i = 0; i < m; i += step / sizeof(*p)) {
			volatile long d;
			unsigned long idx = i;

			if (opt_randomise)
				idx = os_random_long(m - 1, rand_data);

			/* verify last write */
			if (rep && !opt_readonly && !opt_randomise &&
			    p[idx] != idx) {
				fprintf(stderr, "Data wrong at offset 0x%lx. "
					"Expected 0x%08lx, got 0x%08lx\n",
					idx * sizeof(*p), idx, p[idx]);
				exit(1);
			}

			/* read / write */
			if (opt_readonly)
				d = p[idx];
			else
				p[idx] = idx;
			if (!(i & 0xffff) && runtime_exceeded()) {
				rep = reps;
				break;
			}
		}
		if (msync_mode)
			msync(p, bytes, msync_mode);
	}

	if (do_getchar) {
		for ( ; ; ) {
			switch (getchar()) {
			case 'u':
				printf("munlocking\n");
				munlock(p, bytes);
				break;
			case 'l':
				printf("munlocking\n");
				mlock(p, bytes);
				break;
			}
		}
	}

	return 0;
}

long do_units(unsigned long bytes)
{
	struct drand48_data rand_data;

	if (opt_detach)
		detach();

	if (opt_randomise)
		os_random_seed(time(0) ^ getpid(), &rand_data);

	if (!unit)
		unit = bytes;
	/*
	 * Allow a bytes=0 pass for pure fork bomb:
	 * usemem -n 10000 0 --detach --sleep 10
	 */
	do {
		unsigned long size = min(bytes, unit);

		do_unit(size, &rand_data);
		bytes -= size;
		if (runtime_exceeded())
			break;
	} while (bytes);

	if (opt_detach && up(sem_id))
		perror("up");

	if (sleep_secs)
		sleep(sleep_secs);

	return 0;
}

typedef void * (*start_routine)(void *);

int do_task(void)
{
	pthread_t threads[nr_thread];
	long thread_ret;
	int ret;
	int i;

	if (!nr_thread)
		return do_units(bytes);

	for(i = 0; i < nr_thread; i++) {
		ret = pthread_create(&threads[i], NULL, (start_routine)do_units, (void *)bytes);
		if (ret) {
			perror("pthread_create");
			exit(1);
		}
	}
	for(i = 0; i < nr_thread; i++) {
		ret = pthread_join(threads[i], (void *)&thread_ret);
		if (ret) {
			perror("pthread_join");
			exit(1);
		}
	}

	return 0;
}

int do_tasks(void)
{
	int i;
	int status;

	for (i = 0; i < nr_task; i++) {
		if (fork() == 0) {
			return do_task();
		}
	}

	for (i = 0; i < nr_task; i++) {
		if (wait3(&status, 0, 0) < 0) {
			if (errno != EINTR) {
				printf("wait3 error on %dth child\n", i);
				perror("wait3");
				return 1;
			}
		}
	}

	return 0;
}

int main(int argc, char *argv[])
{
	int c;

	ourname = argv[0];
	pagesize = getpagesize();

	while ((c = getopt_long(argc, argv,
				"aAf:FPp:gqowRMm:n:t:ds:T:Sr:u:j:h", opts, NULL)) != -1) {
		switch (c) {
		case 'a':
			opt_malloc++;
			break;
		case 'u':
			unit = memparse(optarg, NULL);
			break;
		case 'j':
			step = memparse(optarg, NULL);
			break;
		case 'A':
			msync_mode = MS_ASYNC;
			break;
		case 'f':
			filename = optarg;
			map_shared = MAP_SHARED;
			break;
		case 'p':
			pid_filename = optarg;
			break;
		case 'g':
			do_getchar = 1;
			break;
		case 'm': /* kept for compatibility */
			bytes = strtol(optarg, NULL, 10);
			bytes <<= 20;
			break;
		case 'n':
			nr_task = strtol(optarg, NULL, 10);
			break;
		case 't':
			nr_thread = strtol(optarg, NULL, 10);
			break;
		case 'P':
			prealloc++;
			break;
		case 'F':
			map_populate = MAP_POPULATE;
			break;
		case 'M':
			do_mlock = 1;
			break;
		case 'q':
			quiet = 1;
			break;
		case 's':
			sleep_secs = strtol(optarg, NULL, 10);
			break;
		case 'T':
			runtime_secs = strtol(optarg, NULL, 10);
			break;
		case 'd':
			opt_detach = 1;
			break;
		case 'r':
			reps = strtol(optarg, NULL, 10);
			break;
		case 'R':
			opt_randomise++;
			break;
		case 'S':
			msync_mode = MS_SYNC;
			break;
		case 'o':
			opt_readonly++;
			break;
		case 'w':
			opt_openrw++;
			break;
		case 'h':
			usage(0);
			break;
		default:
			usage(1);
		}
	}

	if (step < sizeof(long))
		step = sizeof(long);

	if (optind != argc - 1)
		usage(0);

	if (pid_filename && nr_task) {
		fprintf(stderr, "%s: pid file is only for single process\n",
			ourname);
		exit(1);
	}

	if (runtime_secs)
		gettimeofday(&start_time, NULL);

	bytes = memparse(argv[optind], NULL);

	if (!opt_malloc)
		fd = open(filename, ((opt_readonly && !opt_openrw) ?
			  O_RDONLY : O_RDWR) | O_CREAT, 0666);
	if (fd < 0) {
		fprintf(stderr, "%s: failed to open `%s': %s\n",
			ourname, filename, strerror(errno));
		exit(1);
	}

	if (prealloc)
		prealloc = allocate(bytes);

	if (nr_task)
		return do_tasks();

	return do_task();
}
