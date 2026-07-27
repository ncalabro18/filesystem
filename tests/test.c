#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>



const char *EMPTY_STRING = "";

char* validate_open_write_close_read ();
char* validate_duplicate_detection ();
char* validate_delete ();
char* validate_unlink_fails ();
char* validate_rmdir_fails ();
char* validate_write_fails ();
char* validate_stat   ();
char* validate_mkdir  ();



static char* (*tests[]) (void) = {
	validate_open_write_close_read,
	validate_duplicate_detection,
	validate_delete,
	validate_unlink_fails,
	validate_rmdir_fails,
	validate_write_fails,
	validate_stat,
	validate_mkdir
};

#define NUM_TESTS (sizeof(tests) / sizeof(tests[0]))

static const char *names[1028] = {
	"validate_open_write_close_read",
	"validate_duplicate_detection",
	"validate_delete",
	"validate_unlink_fails",
	"validate_rmdir_fails",
	"validate_write_fails",
	"validate_stat",
	"validate_mkdir"
};


static const char error_message;

/* driver for test functions */
int main () {

	puts("TAP version 14");
	printf("1..%d\n", NUM_TESTS);


	for (int i = 0; i < NUM_TESTS; i++) {

		char* result = tests[i] ();

		if (result == NULL)
			printf(    "ok %d - %s\n", i + 1, names[i]);
		else {
			printf("not ok %d - %s\n", i + 1, names[i]);
			if (result != EMPTY_STRING)
				printf("  ---\n  message: %s\n  ...\n", result);
		}
	}


	return 0;
}

// similar to _f_ but uses syscalls instead of stdio
char* validate_open_write_close_read () {
	int fd = open("test.txt", O_CREAT | O_WRONLY);
	if (fd < 0) {
		return "open fd < 0";
	}

	const char *str = "this is a string to be written!\n";
	size_t written = 0;
	int strlength = strlen(str);
	
	while (written < strlength) {
		ssize_t write_ret = write(fd, str, strlen(str));
		if (write_ret < 0) {
			return "write_return < 0";
		}
		written += write_ret;
	}

	close(fd);

	fd = open("test.txt", O_RDONLY);
	if (fd < 0) {
		return "open fd < 0(2)";
	}


	char buf[1028];
	ssize_t read_ret = read(fd, buf, strlength);

	if (strcmp(buf, str) != 0) {
		return "strcmp error";
	}

	// NULL is success; no error string to return
	return NULL;
}

char* validate_duplicate_detection () {
	int fd1 = open("test1.txt", O_CREAT | O_WRONLY);
	if (fd1 < 0) {
		return "open fd < 0";
	}
	int fd2 = open("test1.txt", O_CREAT | O_EXCL | O_WRONLY);
	if (fd2 >= 0) {
		return "open should have failed in create mode";
	}
	close(fd1);

	return NULL;
}

char* validate_delete () {
	int fd = open("test.txt", O_CREAT | O_WRONLY);
	if (fd < 0) {
		return "open fd < 0";
	}

	const char *str = "this is a string to be written!\n";
	size_t written = 0;
	int strlength = strlen(str);
	
	while (written < strlength) {
		ssize_t write_ret = write(fd, str, strlen(str));
		if (write_ret < 0) {
			return "write_return < 0";
		}
		written += write_ret;
	}

	close(fd);

	remove("test.txt");

	return NULL;
}


char* validate_rmdir_fails() {
	if (rmdir("some_non_existent_file.txt") == 0) {
		return "rmdir returned 0";
	}
	return NULL;
}

char* validate_unlink_fails() {
	if (unlink("some_non_existent_directory.txt") == 0) {
		return "unlink returned 0";
	}
	return NULL;
}

char* validate_write_fails () {
	int fd = open("test124.txt", O_WRONLY | O_CREAT);
	if (fd < 0) {
		return "fd < 0";
	}

	// not currently implemented fully, not sure how to access boundary
	//	if (write(fd, )
	remove("test124.txt");

	return NULL;
}
char* validate_write_fills () {
	int fd = open("test12.txt", O_WRONLY | O_CREAT);
	if (fd < 0) {
		return "fd < 0";
	}
	// not currently implemented fully, not sure how to access boundary
	//write();
	remove("test12.txt");
	return NULL;
}

char* validate_stat (){
	int fd = open("test_stat.txt", O_CREAT | O_WRONLY);
	if (fd < 0) {
		return "open fd < 0";
	}
	int ret = close(fd);
	if (ret != 0) {
		return "close return error";
	}

	struct stat s;
	ret = stat("test_stat.txt", &s);
	if (ret < 0) {
		return "stat ret < 0";
	}

	if ( (s.st_mode & S_IXGRP) == 0) {
		return "detected executable mode not enabled";
	}

	return NULL;
}

char* validate_mkdir (){

	if (mkdir ("some_directory", 0) != 0)
		return "failed to mkdir some_directory";

	if (rmdir("some_directory") != 0)
		return "failed to rmdir some_directory";

	return NULL;
}


