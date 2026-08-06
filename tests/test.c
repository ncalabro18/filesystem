#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>

#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/vfs.h>
#include <sys/statvfs.h>
#include <sys/statfs.h>


#include "/work/sfs/sfs.h"


const char *EMPTY_STRING = "";

char* validate_open_write_close_read ();
char* validate_duplicate_detection ();
char* validate_delete ();
char* validate_unlink_fails ();
char* validate_rmdir_fails ();
char* validate_write_fails ();
char* validate_stat   ();
char* validate_mkdir  ();
char* validate_getattr();
char* validate_setattr_chmod();
char* validate_setattr_truncate();
char* validate_readdir_contents();
char* validate_nested_directories();
char* validate_rmdir_nonempty_fails();
char* validate_name_length_boundary();
char* validate_inode_table_exhaustion();
char* validate_read_directory_fails();
char* validate_max_file_size_boundary();

char* validate_delete_size();

char *validate_mkdir_exhaustion_and_reclaim();

char* validate_concurrent_create();
char* validate_name_field_no_leftover_bytes();

char* validate_mode_enforcement();
char* validate_ownership_on_create();

char* validate_statfs();

char* validate_rename_basic();
char* validate_rename_directory_and_overwrite();
char* validate_rename_into_own_subdir_fails();
char* validate_symlink_basic();

char* validate_symlink_dangling();

char* validate_hardlink_shares_inode();
char* validate_hardlink_directory_rejected();

char* validate_timestamps();

char* validate_unlink_while_open();

/* Does not validate specific behavior, hence the name difference */
char* test_randomized_operations();


static char* (*tests[]) (void) = {
	validate_open_write_close_read,
	validate_duplicate_detection,
	validate_delete,
	validate_unlink_fails,
	validate_rmdir_fails,
	validate_write_fails,
	validate_stat,
	validate_mkdir,
	validate_getattr,
	validate_setattr_chmod,
	validate_setattr_truncate,
	validate_readdir_contents,
	validate_nested_directories,
	validate_rmdir_nonempty_fails,
	validate_name_length_boundary,
	validate_inode_table_exhaustion,
	validate_read_directory_fails,
	validate_max_file_size_boundary,
	validate_delete_size,
	validate_mkdir_exhaustion_and_reclaim,
	validate_concurrent_create,
	validate_name_field_no_leftover_bytes,
	validate_mode_enforcement,
	validate_ownership_on_create,
	validate_statfs,
	validate_rename_basic,
	validate_rename_directory_and_overwrite,
	validate_rename_into_own_subdir_fails,
	validate_symlink_basic,
	validate_symlink_dangling,
	validate_hardlink_shares_inode,
	validate_hardlink_directory_rejected,
	validate_timestamps,
	validate_unlink_while_open,

	test_randomized_operations
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
	"validate_mkdir",
	"validate_getattr",
	"validate_setattr_chmod",
	"validate_setattr_truncate",
	"validate_readdir_contents",
	"validate_nested_directories",
	"validate_rmdir_nonempty_fails",
	"validate_name_length_boundary",
	"validate_inode_table_exhaustion",
	"validate_read_directory_fails",
	"validate_max_file_size_boundary",
	"validate_delete_size",
	"validate_mkdir_exhaustion_and_reclaim",
	"validate_concurrent_create",
	"validate_name_field_no_leftover_bytes",
	"validate_mode_enforcement",
	"validate_ownership_on_create",
	"validate_statfs",
	"validate_rename_basic",
	"validate_rename_directory_and_overwrite",
	"validate_rename_into_own_subdir_fails",
	"validate_symlink_basic",
	"validate_symlink_dangling",
	"validate_hardlink_shares_inode",
	"validate_hardlink_directory_rejected",
	"validate_timestamps",
	"validate_unlink_while_open",

	"test_randomized_operations"
};


static char error_message[1028];

/* driver for test functions */
int main (int argc, char **argv) {

	int error_count = 0;
	
	puts("TAP version 14");
	printf("1..%ld\n", NUM_TESTS);

	
	for (int i = 0; i < NUM_TESTS; i++) {
		if (argc > 1 && chdir(argv[1]) != 0) {
			perror("chdir to test target");
			return 1;
		}

		char* result = tests[i] ();

		if (result == NULL)
			printf(    "ok %d - %s\n", i + 1, names[i]);
		else {
			// not how the return status is usually used,
			// 		but could be useful
			error_count += 1;
			printf("not ok %d - %s\n", i + 1, names[i]);
			if (result != EMPTY_STRING)
				printf("  ---\n  errno: %d (%s);  message: %s\n  ...\n", errno, strerror(errno), result);
		}
	}

	return error_count;
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

	if (remove("test1.txt") < 0) return "remove error";

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

	if (remove("test.txt") < 0) return "remove test.txt";

	return NULL;
}


char* validate_rmdir_fails() {
	if (rmdir("nonexistent_dir") == 0) {
		return "rmdir returned 0";
	}
	return NULL;
}

char* validate_unlink_fails() {
	if (unlink("nonexistent.txt") == 0) {
		return "unlink returned 0";
	}
	return NULL;
}

char* validate_write_fails () {
	int fd = open("test124.txt", O_WRONLY | O_CREAT);
	if (fd < 0) {
		return "fd < 0";
	}

	close(fd);

	if (remove("test124.txt") < 0) return "remove test124.txt";

	
	const char *buf = "hello, world";
	if (write(fd, buf, strlen(buf)) >= 0) {
		return "write passed";
	}

	return NULL;
}
char* validate_write_fills () {
	int fd = open("test12.txt", O_WRONLY | O_CREAT);
	if (fd < 0) {
		return "fd < 0";
	}
	const char *str = "test";
	char buf[8];

	if (write(fd, str, 4) < 0) return "write fail";

	if (read(fd, buf, 4) < 0) return "read error";
	buf[4] = '\0';
	if (strcmp(str, buf) != 0) return "data mismatch";


	close(fd);
	
	if (remove("test12.txt") < 0) return "remove error";
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

char* validate_getattr() {
    int fd = open("attr_test.txt", O_CREAT | O_WRONLY, 0644);
    if (fd < 0) return "open fd < 0";
    if (write(fd, "1234567890", 10) < 0) return "write error";
    close(fd);

    struct stat s;
    if (stat("attr_test.txt", &s) < 0) return "stat failed";
    if (!S_ISREG(s.st_mode)) return "not a regular file";
    if (s.st_size != 10) return "wrong size";
    if (s.st_nlink != 1) return "wrong nlink";

	if (remove("attr_test.txt") < 0) return "remove error";

    return NULL;
}

char* validate_setattr_truncate() {
    int fd = open("trunc_test.txt", O_CREAT | O_WRONLY, 0644);
    if (fd < 0) return "open fd < 0";
    write(fd, "0123456789", 10);
    close(fd);

    if (truncate("trunc_test.txt", 4) != 0) return "truncate failed";

    struct stat s;
    if (stat("trunc_test.txt", &s) < 0) return "stat failed";
    if (s.st_size != 4) return "size not updated";

    int fd2 = open("trunc_test.txt", O_RDONLY);
    char buf[16] = {0};
    read(fd2, buf, sizeof(buf));
    close(fd2);
    if (strcmp(buf, "0123") != 0) return "content mismatch after truncate";

	if (remove("trunc_test.txt") < 0) return "remove error";

    return NULL;
}

char* validate_setattr_chmod() {
    int fd = open("chmod_test.txt", O_CREAT | O_WRONLY, 0644);
    if (fd < 0) return "open fd < 0";
    close(fd);

    if (chmod("chmod_test.txt", 0741) != 0) return "chmod failed";

    struct stat s;
    if (stat("chmod_test.txt", &s) < 0) return "stat failed";
    if ((s.st_mode & 0777) != 0741) return "mode did not persist";

	if (remove("chmod_test.txt") < 0) return "remove error";

    return NULL;
}

char* validate_exec_permission_denied() {
    int fd = open("noexec.txt", O_CREAT | O_WRONLY, 0644); // no x bit
    if (fd < 0) return "open fd < 0";
    close(fd);

    if (access("noexec.txt", X_OK) == 0)
        return "X_OK succeeded on non-executable file";

    if (chmod("noexec.txt", 0755) != 0) return "chmod failed";

    if (access("noexec.txt", X_OK) != 0)
        return "X_OK failed after chmod +x";

    return NULL;
}

char* validate_actual_exec() {
    int src = open("/tests/executable_test", O_RDONLY);
    if (src < 0) return "missing /tests/executable_test source binary";

    int dst = open("runme", O_CREAT | O_WRONLY, 0755);
    if (dst < 0) return "open dst fd < 0";

    char buf[4096];
    ssize_t n;
    while ((n = read(src, buf, sizeof(buf))) > 0)
        write(dst, buf, n);
    close(src);
    close(dst);

    pid_t pid = fork();
    if (pid == 0) {
        execl("runme", "runme", NULL);
        _exit(127);
    }
    int status;
	if (waitpid(pid, &status, 0) == -1)
    	"error waitpid";
    if (!WIFEXITED(status) || WEXITSTATUS(status) == 127)
        return "exec failed to run";

    return NULL;
}

char* validate_readdir_contents() {
	const char *expected[] = {"alpha", "beta", "gamma"};
	int seen[3] = {0};
	int dot = 0, dotdot = 0;

	if (mkdir("readdir_scratch", 0755) != 0) return "mkdir scratch failed";
	if (chdir("readdir_scratch") != 0) return "chdir into scratch failed";

	for (int i = 0; i < 3; i++) {
		int fd = open(expected[i], O_CREAT | O_WRONLY, 0644);
		if (fd < 0) { chdir(".."); return "failed to create scratch file"; }
		close(fd);
	}

	DIR *d = opendir(".");
	if (d == NULL) { chdir(".."); return "opendir failed"; }

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0) { dot = 1; continue; }
		if (strcmp(ent->d_name, "..") == 0) { dotdot = 1; continue; }

		int matched = 0;
		for (int i = 0; i < 3; i++) {
			if (strcmp(ent->d_name, expected[i]) == 0) {
				seen[i] = 1;
				matched = 1;
			}
		}
		if (!matched) {
			closedir(d);
			chdir("..");
			return "unexpected entry in readdir output";
		}
	}
	if (closedir(d) < 0) return "closedir";

	for (int i = 0; i < 3; i++) {
		if (unlink(expected[i]) < 0) return "unlink expected[i]";
		if (!seen[i]) { chdir(".."); return "missing expected entry in readdir"; }
	}
	if (!dot || !dotdot) { chdir(".."); return "missing . or .. entry"; }

	if (chdir("..") < 0) return "chdir ..";
	if (rmdir("readdir_scratch") < 0) return "rmdir readdir_scratch";

	return NULL;
}

char* validate_nested_directories() {
	if (mkdir("nest_a", 0755) != 0) return "mkdir nest_a failed";
	if (chdir("nest_a") != 0) { rmdir("nest_a"); return "chdir nest_a failed"; }

	if (mkdir("nest_b", 0755) != 0) { chdir(".."); rmdir("nest_a"); return "mkdir nest_b failed"; }
	if (chdir("nest_b") != 0) { chdir(".."); rmdir("nest_a"); return "chdir nest_b failed"; }

	int fd = open("leaf.txt", O_CREAT | O_WRONLY, 0644);
	if (fd < 0) { chdir("../.."); return "create leaf failed"; }
	write(fd, "leaf", 4);
	close(fd);

	fd = open("leaf.txt", O_RDONLY);
	if (fd < 0) { chdir("../.."); return "reopen leaf failed"; }
	char buf[8] = {0};
	if (read(fd, buf, 4) <= 0)
		return "error reading leaf.txt";
	close(fd);

	if (strncmp(buf, "leaf", 4) != 0) { chdir("../.."); return "leaf content mismatch"; }

	if (unlink("leaf.txt") < 0) return "unlink leaf.txt";
	if (chdir("..") < 0) return "chdir ..";
	if (rmdir("nest_b") < 0) return "rmdir nest_b";
	if (chdir("..") < 0) return "chdir ..";
	if (rmdir("nest_a") < 0) return "rmdir nest_a";

	return NULL;
}

char* validate_rmdir_nonempty_fails() {
	if (mkdir("nonempty_dir", 0755) != 0) return "mkdir failed";
	if (chdir("nonempty_dir") != 0) { rmdir("nonempty_dir"); return "chdir failed"; }

	int fd = open("occupant.txt", O_CREAT | O_WRONLY, 0644);
	if (fd < 0) { chdir(".."); return "create occupant failed"; }
	close(fd);
	if (chdir("..") < 0) return "erro chdir \"..\"";

	if (rmdir("nonempty_dir") == 0)
		return "rmdir succeeded on a non-empty directory";
	if (errno != ENOTEMPTY)
		return "rmdir on non-empty directory failed with wrong errno";

	if (chdir("nonempty_dir") < 0)
		return "error chdir to nonempty_dir";
	if (unlink("occupant.txt") < 0)
		return "error unlinking occupant.txt";
	if (chdir("..") < 0)
		return "error chdir to \"..\"";

	if (rmdir("nonempty_dir") != 0)
		return "rmdir failed after directory was emptied";

	return NULL;
}

char* validate_name_length_boundary() {

	struct statvfs stats;

	if (statvfs(".", &stats) != 0) return "statvfs failed";

	int namelen = stats.f_namemax;
	printf("statvfs's f_namemax: %d", namelen);

	char max_name[2048]; // 2048 should always be enough
	char too_long[2048];
	if (2048 > namelen)
		return "test design error: namelen is greater than the allocated space to test it";
	

	for(int i = 0; i < namelen; i++)
		max_name[i] = '0' + (i % 10);
		
	for(int i = 0; i < namelen + 8; i++)
		too_long[i] = '0' + (i % 10);
	
	max_name[namelen] = '\0';
	too_long[namelen + 8] = '\0';
	
	int fd = open(max_name, O_CREAT | O_WRONLY, 0644);
	if (fd < 0) return "max-length name failed to create";
	close(fd);
	if (unlink(max_name) < 0)
		return "error unlinking max_name";

	fd = open(too_long, O_CREAT | O_WRONLY, 0644);
	if (fd >= 0) {
		close(fd);
		unlink(too_long);
		return "over-length (16 char) name unexpectedly succeeded";
	}
	if (errno != ENAMETOOLONG)
		return "over-length name failed with wrong errno";

	return NULL;
}

char* validate_inode_table_exhaustion() {
	struct statvfs sv;
	if (statvfs(".", &sv) != 0) return "statvfs failed";
	unsigned long max_attempts = sv.f_ffree + 8; /* a few more than currently free */

	char name[32];
	unsigned long created = 0;

	if (mkdir("fill_scratch", 0755) != 0) return "mkdir scratch failed";
	if (chdir("fill_scratch") != 0) { rmdir("fill_scratch"); return "chdir scratch failed"; }

	char *result = NULL;
	for (; created < max_attempts; created++) {
		snprintf(name, sizeof(name), "f%lu", created);
		int fd = open(name, O_CREAT | O_EXCL | O_WRONLY, 0644);
		if (fd < 0) {
			if (errno != ENOSPC) { result = "create failed with unexpected errno before exhaustion"; }
			break;
		}
		close(fd);
	}

	if (!result && created == max_attempts) result = "table never exhausted despite using statvfs-reported capacity";
	if (!result && created == 0) result = "table exhausted immediately - no room to test reclaim";

	if (!result) {
		snprintf(name, sizeof(name), "f%lu", 0UL);
		if (unlink(name) != 0) result = "unlink failed during reclaim check";
	}
	if (!result) {
		int fd = open("reclaimed", O_CREAT | O_EXCL | O_WRONLY, 0644);
		if (fd < 0) result = "create failed after freeing a slot";
		else { close(fd); unlink("reclaimed"); }
	}

	/* ALWAYS clean up, regardless of which branch fired above */
	for (unsigned long i = 1; i < created; i++) {
		snprintf(name, sizeof(name), "f%lu", i);
		unlink(name);
	}
	chdir("..");
	rmdir("fill_scratch");

	return result;
}

char* validate_read_directory_fails() {
	if (mkdir("read_dir_test", 0755) != 0) return "mkdir failed";

	int fd = open("read_dir_test", O_RDONLY);
	if (fd < 0) { rmdir("read_dir_test"); return "open() on a directory failed unexpectedly"; }

	char buf[16];
	ssize_t ret = read(fd, buf, sizeof(buf));
	int saved_errno = errno;
	close(fd);
	if (rmdir("read_dir_test") < 0) return "error rmdir";

	if (ret >= 0) return "read() on a directory unexpectedly succeeded";
	if (saved_errno != EISDIR) return "read() on directory failed with wrong errno";

	return NULL;
}
char* validate_max_file_size_boundary() {
	const char *marker = "DO-NOT-OVERWRITE";
	const size_t target_size = (size_t)SFS_MAX_FILE_BLOCKS * SFS_BLOCK_SIZE;
	char chunk[8192];
	size_t total = 0;

	memset(chunk, 'A', sizeof(chunk));

	int fd = open("neighbor.txt", O_CREAT | O_WRONLY, 0644);
	if (fd < 0) return "create neighbor failed";
	if (write(fd, marker, strlen(marker)) != (ssize_t)strlen(marker)) {
		close(fd);
		unlink("neighbor.txt");
		return "write to neighbor failed";
	}
	close(fd);

	fd = open("full.txt", O_CREAT | O_WRONLY, 0644);
	if (fd < 0) { unlink("neighbor.txt"); return "create full.txt failed"; }

	while (total < target_size) {
		size_t remaining = target_size - total;
		size_t want = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
		ssize_t w = write(fd, chunk, want);
		if (w <= 0) break;
		total += (size_t)w;
	}
	close(fd);

	if (total != target_size) {
		unlink("neighbor.txt");
		unlink("full.txt");
		return "could not fill file to the addressing-imposed max size";
	}

	struct stat st;
	if (stat("full.txt", &st) != 0) {
		unlink("neighbor.txt");
		unlink("full.txt");
		return "stat on full.txt failed";
	}
	if ((size_t)st.st_size != target_size) {
		unlink("neighbor.txt");
		unlink("full.txt");
		return "reported size does not match target size";
	}

	fd = open("full.txt", O_WRONLY);
	if (fd < 0) {
		unlink("neighbor.txt");
		unlink("full.txt");
		return "reopen full.txt failed";
	}
	if (lseek(fd, (off_t)target_size, SEEK_SET) != (off_t)target_size) {
		close(fd);
		unlink("neighbor.txt");
		unlink("full.txt");
		return "lseek to boundary failed";
	}
	ssize_t overflow = write(fd, "X", 1);
	int overflow_errno = errno;
	close(fd);

	if (overflow > 0) {
		unlink("neighbor.txt");
		unlink("full.txt");
		return "write past the addressing-imposed max size unexpectedly succeeded";
	}
	/* Expecting -EFBIG specifically, since this boundary is imposed by
	 * the direct+indirect addressing scheme running out of pointers -
	 * not by free space. ENOSPC here would indicate the test image is
	 * too small to reach the addressing limit at all, which is a test
	 * environment problem rather than a filesystem bug, so it's called
	 * out distinctly rather than silently accepted as a pass. */
	if (overflow_errno != EFBIG) {
		unlink("neighbor.txt");
		unlink("full.txt");
		if (overflow_errno == ENOSPC)
			return "hit ENOSPC before reaching the addressing limit - image too small for this test";
		return "write past max size failed with unexpected errno (expected EFBIG)";
	}

	fd = open("neighbor.txt", O_RDONLY);
	if (fd < 0) { unlink("full.txt"); return "reopen neighbor failed"; }
	char buf[64] = {0};
	if (read(fd, buf, strlen(marker)) != (ssize_t)strlen(marker)) {
		close(fd);
		unlink("neighbor.txt");
		unlink("full.txt");
		return "read from neighbor failed";
	}
	close(fd);

	if (unlink("neighbor.txt") < 0) return "error unlink neighbor.txt";
	if (unlink("full.txt") < 0) return "error unlink full.txt";

	if (strncmp(buf, marker, strlen(marker)) != 0)
		return "neighbor file corrupted by oversized write to adjacent file";

	return NULL;
}

/* validate after a file is deleted,
	its contents are cleared via stat */
char* validate_delete_size() {
	const char *str = "Delete Size String";
	const char *filename = "del_siz_";

	int fd = open(filename, O_CREAT | O_WRONLY, 0644);
	if (fd < 0)
		return "open error";


	ssize_t write_ret = write(fd, str, strlen(str));
	if (write_ret < 0) {
		return "write_return < 0";
	}

	close(fd);

	if (remove(filename) < 0) return "error remove";

	// sanity check
	struct stat s;
	int ret = stat(filename, &s);
	if (ret == 0) {
		return "stat ret == 0";
	}

	// make a bunch of empty files, check they're all empty
	const int num_files = 128;
	for (int i = 0; i < num_files; i++){
		char full_filename[64];
		if (sprintf(full_filename, "%s%4d", filename, i) < 0)
			return "sprintf error";
		fd = open(full_filename, O_CREAT | O_RDONLY, 0644);
		if (fd < 0)
			return "open error(2)";

		char buf[1028];
		ssize_t read_ret = read(fd, buf, 1);
		if (read_ret > 0) return "read returned bytes of an empty file";
		if (read_ret < 0) return "read error";
		
		close(fd);

		ret = stat(full_filename, &s);
		if (ret < 0)
			return "stat failed when it shouldn't have";

		if (s.st_size != 0) return "size was no zeroed on delete";

		if (remove(full_filename) < 0) return "remove error";
	}

	return NULL;
}

char* validate_concurrent_create() {
	const int NUM_CHILDREN = 8;
	pid_t pids[NUM_CHILDREN];
	int seen[NUM_CHILDREN] = {};
	memset (seen, '\0', sizeof(seen));

	if (mkdir("concur_scratch", 0755) != 0) return "mkdir scratch failed";
	if (chdir("concur_scratch") != 0) { rmdir("concurrent_scratch"); return "chdir scratch failed"; }

	for (int i = 0; i < NUM_CHILDREN; i++) {
		pid_t pid = fork();
		if (pid < 0) return "fork failed";
		if (pid == 0) {
			char name[32];
			snprintf(name, sizeof(name), "child_%d", i);
			int fd = open(name, O_CREAT | O_WRONLY, 0644);
			if (fd < 0) _exit(1);
			ssize_t w = write(fd, name, strlen(name));
			if (w < 0 || (size_t)w != strlen(name)) _exit(1);
			close(fd);
			_exit(0);
		}
		pids[i] = pid;
	}

	int all_ok = 1;
	for (int i = 0; i < NUM_CHILDREN; i++) {
		int status;
		if (waitpid(pids[i], &status, 0) == -1)
			"error waitpid";
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
			all_ok = 0;
	}
	if (!all_ok) return "one or more concurrent creates failed";

	DIR *d = opendir(".");
	if (d == NULL) return "opendir failed";

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		int idx;
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;
		if (sscanf(ent->d_name, "child_%d", &idx) == 1 && idx >= 0 && idx < NUM_CHILDREN) {
			if (seen[idx]) { closedir(d); return "duplicate entry - two children landed on the same inode"; }
			seen[idx] = 1;
		} else {
			closedir(d);
			return "unexpected entry found in concurrent test dir";
		}
	}
	if (closedir(d) < 0) return "error closedir";

	for (int i = 0; i < NUM_CHILDREN; i++)
		if (!seen[i]) return "missing entry - a concurrent create was lost";

	for (int i = 0; i < NUM_CHILDREN; i++) {
		char name[32];
		snprintf(name, sizeof(name), "child_%d", i);
		if (unlink(name) < 0) return "unlink error";
	}

	if (chdir("..") != 0) return "chdir back up failed";
	if (rmdir("concur_scratch") != 0) {
		return "error removing concurrent scratch file";
	}

	return NULL;
}

char* validate_mkdir_exhaustion_and_reclaim() {
	char name[32];
	int created = 0;
	const int max_attempts = 4096;

	if (mkdir("dir_scratch", 0755) != 0) return "mkdir scratch failed";
	if (chdir("dir_scratch") != 0) { rmdir("dir_scratch"); return "chdir scratch failed"; }

	/* --- phase 1: nested + non-empty rmdir check, done FIRST while slots are plentiful --- */
	if (mkdir("probe", 0755) != 0) { chdir(".."); rmdir("dir_scratch"); return "mkdir probe failed"; }
	if (chdir("probe") != 0) return "chdir into probe failed";
	if (mkdir("nested", 0755) != 0) return "mkdir nested failed";
	if (chdir("..") != 0) return "chdir back to scratch failed";

	if (rmdir("probe") == 0) return "rmdir succeeded on a directory with a nested child";
	if (errno != ENOTEMPTY) return "rmdir on non-empty directory gave wrong errno";

	if (chdir("probe") != 0) return "re-chdir into probe failed";
	if (rmdir("nested") != 0) return "rmdir of nested child failed";
	if (chdir("..") != 0) return "chdir back to scratch failed (2)";

	if (rmdir("probe") != 0) return "rmdir failed after probe was emptied";

	/* --- phase 2: exhaustion + reclaim, done LAST since nothing more needs creating after --- */
	for (; created < max_attempts; created++) {
		snprintf(name, sizeof(name), "d%d", created);
		if (mkdir(name, 0755) != 0) {
			if (errno != ENOSPC) return "mkdir failed with unexpected errno before exhaustion";
			break;
		}
	}

	if (created == max_attempts) return "directory table never exhausted - unexpected capacity";
	if (created == 0) return "directory table exhausted immediately - no room to test reclaim";

	snprintf(name, sizeof(name), "d%d", 0);
	if (rmdir(name) != 0) return "rmdir failed during reclaim check";

	if (mkdir("reclaimed", 0755) != 0) return "mkdir failed after freeing a directory slot";

	DIR *d = opendir("reclaimed");
	if (d == NULL) return "opendir on reclaimed dir failed";
	int stale_found = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;
		stale_found = 1;
	}
	if (closedir(d) < 0) return "error closedir";
	if (stale_found) return "reclaimed directory unexpectedly contains a stale entry";

	if (rmdir("reclaimed") < 0) return "error rmdir reclaimed";
	for (int i = 1; i < created; i++) {
		snprintf(name, sizeof(name), "d%d", i);
		if (rmdir(name) < 0) return "error rmdir(name)";
	}

	if (chdir("..") != 0) return "chdir back to mount root failed";
	if (rmdir("dir_scratch") < 0) return "error rmdir dir_scratch";

	return NULL;
}

char* validate_name_field_no_leftover_bytes() {
	const char *long_name = "abcdefghijklmno"; /* 15 chars, max length */
	const char *short_name = "zz";

	if (mkdir("name_integ", 0755) != 0) return "mkdir scratch failed";
	if (chdir("name_integ") != 0) { rmdir("name_integ"); return "chdir failed"; }

	/* create a full-length name, confirm exact round-trip via readdir immediately */
	int fd = open(long_name, O_CREAT | O_WRONLY, 0644);
	if (fd < 0) { chdir(".."); return "create long_name failed"; }
	close(fd);

	DIR *d = opendir(".");
	if (d == NULL) { chdir(".."); return "opendir failed"; }
	int found_long = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
		if (strlen(ent->d_name) != strlen(long_name) || strcmp(ent->d_name, long_name) != 0) {
			closedir(d);
			chdir("..");
			return "long name did not round-trip exactly via kernel readdir";
		}
		found_long = 1;
	}
	closedir(d);
	if (!found_long) { chdir(".."); return "long name missing from readdir entirely"; }

	/* free that slot, then immediately claim it with a much shorter name -
	   repeated several times to raise the odds of hitting the same slot,
	   since userspace can't force a specific inode number directly */
	if (unlink(long_name) != 0) { chdir(".."); return "unlink long_name failed"; }

	for (int attempt = 0; attempt < 8; attempt++) {
		fd = open(short_name, O_CREAT | O_WRONLY, 0644);
		if (fd < 0) { chdir(".."); return "create short_name failed"; }
		close(fd);

		d = opendir(".");
		if (d == NULL) { chdir(".."); return "opendir failed (2)"; }
		while ((ent = readdir(d)) != NULL) {
			if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
			if (strcmp(ent->d_name, short_name) != 0) {
				closedir(d);
				chdir("..");
				return "readdir returned a name other than exactly short_name - possible leftover bytes";
			}
			if (strlen(ent->d_name) != strlen(short_name)) {
				closedir(d);
				chdir("..");
				return "short_name length mismatch - possible trailing leftover bytes from prior occupant";
			}
		}
		closedir(d);

		if (unlink(short_name) != 0) { chdir(".."); return "unlink short_name failed"; }

		/* churn a throwaway file in between to encourage slot reuse variety */
		char churn[16];
		snprintf(churn, sizeof(churn), "c%d", attempt);
		fd = open(churn, O_CREAT | O_WRONLY, 0644);
		if (fd >= 0) { close(fd); unlink(churn); }
	}

	if (chdir("..") != 0) return "chdir back failed";
	if (rmdir("name_integ") != 0) return "rmdir scratch failed";

	return NULL;
}

char* validate_mode_enforcement() {

	// make a file with read only permission for non-root users
	const char* filename_ro = "mode_enf_reado";
	int fd = open(filename_ro, O_WRONLY |  O_CREAT);
	if (write(fd, "hi", 2) < 0) return "write error";
	close(fd);
	if (chmod(filename_ro, 0744) < 0) return "chmod error";

	// make a file with no permissions for non-root users
	const char* filename_strict = "mode_enf_none";
	fd = open(filename_strict, O_WRONLY |  O_CREAT);
	if (write(fd, "hi", 2) < 0) return "write error";
	close(fd);
	if (chmod(filename_strict, 0700) < 0) return "chmod error";



	const uid_t test_uid = 1000;
	const gid_t test_gid = 1000;

	int pid = fork();
	if (pid < 0) return "fork error";
	if (pid == 0) {
		/* never return from a forked process */

		/* drop root permissions */
		if (setgroups(0, NULL) < 0) _exit(2);
		if (setgid(test_gid) < 0) _exit(3);
		if (setuid(test_uid) < 0) _exit(4);

		/* validate permissions were dropped */
		if (getuid() == 0 || geteuid() == 0) _exit(5);

		//running without root priv.

		fd = open(filename_ro, O_RDONLY);
		if (fd < 0) _exit(6);

		char buf[8];
		if (read(fd, buf, 2) < 0) _exit(7);

		if ( !(buf[0] =='h' && buf[1] == 'i') )
			_exit(8);

		close(fd);

		fd = open(filename_strict, O_RDONLY);
		if (fd > 0) {
			close(fd);
			_exit(9);
		}

		_exit(0);

	}
	/* back to parent process with root perms */
	int status;
	if (waitpid(pid, &status, 0) < 0)
		return "waitpid failure";

	if (remove(filename_ro) < 0) return "error removing ro file";
	if (remove(filename_strict) < 0)
		return "error removing strict file";


	if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
		sprintf(error_message,
			"child exited with status %d", WEXITSTATUS(status));
		return error_message;
	}

	if (WIFSIGNALED(status)) {
		sprintf(error_message,
			"child killed by signal %d", WTERMSIG(status));
		return error_message;
	}
	
	return NULL;
}

char* validate_ownership_on_create() {
	const uid_t test_uid = 1000;
	const gid_t test_gid = 1000;
	const char *scratch = "ownrshp_scratch";
	const char *fname = "owner_test.txt";
	const char *dname = "owner_test_dir";

	/* mount root is 0755 root:root - a non-root user correctly can't
	 * write there. Create a world-writable scratch dir first. */
	if (mkdir(scratch, 0777) != 0) return "mkdir scratch failed";
	if (chmod(scratch, 0777) != 0) { rmdir(scratch); return "chmod scratch failed"; }
	if (chdir(scratch) != 0) { rmdir(scratch); return "chdir scratch failed"; }

	pid_t pid = fork();
	if (pid < 0) { chdir(".."); rmdir(scratch); return "fork error"; }

	if (pid == 0) {
		if (setgroups(0, NULL) < 0) _exit(2);
		if (setgid(test_gid) < 0) _exit(3);
		if (setuid(test_uid) < 0) _exit(4);
		if (getuid() == 0 || geteuid() == 0) _exit(5);

		int fd = open(fname, O_CREAT | O_WRONLY, 0644);
		if (fd < 0) _exit(6);
		close(fd);

		if (mkdir(dname, 0755) != 0) _exit(7);

		_exit(0);
	}

	int status;
	if (waitpid(pid, &status, 0) < 0) { chdir(".."); rmdir(scratch); return "waitpid failure"; }
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		unlink(fname); rmdir(dname);
		chdir(".."); rmdir(scratch);
		return "child failed to create file/dir as non-root user";
	}

	struct stat s;
	char *fail_msg = NULL;

	if (stat(fname, &s) != 0) fail_msg = "stat on file failed";
	else if (s.st_uid != test_uid) fail_msg = "file ownership uid mismatch";
	else if (s.st_gid != test_gid) fail_msg = "file ownership gid mismatch";

	if (!fail_msg) {
		if (stat(dname, &s) != 0) fail_msg = "stat on dir failed";
		else if (!S_ISDIR(s.st_mode)) fail_msg = "dname not a directory";
		else if (s.st_uid != test_uid) fail_msg = "dir ownership uid mismatch";
		else if (s.st_gid != test_gid) fail_msg = "dir ownership gid mismatch";
	}

	unlink(fname);
	rmdir(dname);
	chdir("..");
	rmdir(scratch);

	return fail_msg;
}

char* validate_statfs() {
	struct statvfs before, after, restored;

	if (statvfs(".", &before) != 0) return "statvfs failed";
	if (before.f_bsize == 0) return "f_bsize is zero";
	if (before.f_files == 0) return "f_files is zero";
	if (before.f_namemax == 0) return "f_namemax is zero";

	int fd = open("statfs.txt", O_CREAT | O_WRONLY, 0644);
	if (fd < 0) return "create probe failed";
	close(fd);

	if (statvfs(".", &after) != 0) { unlink("statfs_probe.txt"); return "statvfs after create failed"; }
	if (before.f_ffree - after.f_ffree != 1) {
		unlink("statfs.txt");
		return "f_ffree did not decrease by exactly one after create";
	}

	unlink("statfs.txt");

	if (statvfs(".", &restored) != 0) return "statvfs after unlink failed";
	if (restored.f_ffree != before.f_ffree) return "f_ffree did not restore after unlink";

	return NULL;
}

char* validate_rename_basic() {
	const char *old_name = "rename_src.txt";
	const char *new_name = "rename_dst.txt";
	const char marker[] = "rename-marker";

	int fd = open(old_name, O_CREAT | O_WRONLY, 0644);
	if (fd < 0) return "create failed";
	write(fd, marker, sizeof(marker));
	close(fd);

	if (rename(old_name, new_name) != 0) return "rename failed";

	struct stat s;
	if (stat(old_name, &s) == 0) { unlink(new_name); return "old name still exists after rename"; }

	fd = open(new_name, O_RDONLY);
	if (fd < 0) return "open new name failed";
	char buf[32] = {0};
	ssize_t n = read(fd, buf, sizeof(marker));
	close(fd);
	unlink(new_name);

	if (n != sizeof(marker) || memcmp(buf, marker, sizeof(marker)) != 0)
		return "content mismatch after rename";

	return NULL;
}

char* validate_rename_directory_and_overwrite() {
	if (mkdir("rn_parent", 0755) != 0) return "mkdir parent failed";
	if (mkdir("rn_child", 0755) != 0) { rmdir("rn_parent"); return "mkdir child failed"; }

	if (rename("rn_child", "rn_parent/rn_child") != 0) {
		rmdir("rn_child"); rmdir("rn_parent");
		return "directory rename failed";
	}

	struct stat s;
	if (stat("rn_parent/rn_child", &s) != 0 || !S_ISDIR(s.st_mode))
		return "renamed directory not found at new location";

	if (rmdir("rn_parent/rn_child") != 0) return "rmdir moved child failed";
	if (rmdir("rn_parent") != 0) return "rmdir parent failed";

	int fd = open("rn_a.txt", O_CREAT | O_WRONLY, 0644);
	if (fd < 0) return "create a failed";
	write(fd, "AAAA", 4);
	close(fd);

	fd = open("rn_b.txt", O_CREAT | O_WRONLY, 0644);
	if (fd < 0) { unlink("rn_a.txt"); return "create b failed"; }
	write(fd, "BBBB", 4);
	close(fd);

	if (rename("rn_a.txt", "rn_b.txt") != 0) {
		unlink("rn_a.txt"); unlink("rn_b.txt");
		return "overwrite rename failed";
	}

	if (stat("rn_a.txt", &s) == 0) { unlink("rn_b.txt"); return "source still exists after overwrite rename"; }

	fd = open("rn_b.txt", O_RDONLY);
	if (fd < 0) return "open rn_b.txt after overwrite failed";
	char buf[8] = {0};
	read(fd, buf, 4);
	close(fd);
	unlink("rn_b.txt");

	if (strncmp(buf, "AAAA", 4) != 0) return "overwrite rename did not replace content";

	return NULL;
}

char* validate_rename_into_own_subdir_fails() {
	if (mkdir("rn_outer", 0755) != 0) return "mkdir outer failed";
	if (chdir("rn_outer") != 0) { rmdir("rn_outer"); return "chdir failed"; }
	if (mkdir("rn_inner", 0755) != 0) { chdir(".."); rmdir("rn_outer"); return "mkdir inner failed"; }
	if (chdir("..") != 0) return "chdir back failed";

	if (rename("rn_outer", "rn_outer/rn_inner/moved") == 0)
		return "rename into own descendant unexpectedly succeeded";

	if (chdir("rn_outer") != 0) return "re-chdir failed";
	if (rmdir("rn_inner") != 0) return "rmdir inner failed";
	if (chdir("..") != 0) return "chdir back failed (2)";
	if (rmdir("rn_outer") != 0) return "rmdir outer failed";

	return NULL;
}


char* validate_symlink_basic() {
	const char *target = "symlink_target";
	const char *link = "symlink_link";
	const char marker[] = "symlink-marker";

	int fd = open(target, O_CREAT | O_WRONLY, 0644);
	if (fd < 0) return "create target failed";
	write(fd, marker, sizeof(marker));
	close(fd);

	if (symlink(target, link) != 0) return "symlink() failed";

	struct stat lst;
	if (lstat(link, &lst) != 0) { unlink(target); unlink(link); return "lstat on link failed"; }
	if (!S_ISLNK(lst.st_mode)) { unlink(target); unlink(link); return "link is not reported as a symlink"; }

	char buf[64] = {0};
	ssize_t n = readlink(link, buf, sizeof(buf) - 1);
	if (n < 0) { unlink(target); unlink(link); return "readlink failed"; }
	if (strcmp(buf, target) != 0) { unlink(target); unlink(link); return "readlink target mismatch"; }

	fd = open(link, O_RDONLY);
	if (fd < 0) { unlink(target); unlink(link); return "open through symlink failed"; }
	char content[32] = {0};
	ssize_t r = read(fd, content, sizeof(marker));
	close(fd);
	if (r != sizeof(marker) || memcmp(content, marker, sizeof(marker)) != 0) {
		unlink(target); unlink(link);
		return "content mismatch reading through symlink";
	}

	unlink(link);
	unlink(target);
	return NULL;
}

char* validate_symlink_dangling() {
	const char *link = "dangling_link";

	if (symlink("does_not_exist.txt", link) != 0) return "symlink() to missing target failed";

	struct stat lst;
	if (lstat(link, &lst) != 0) { unlink(link); return "lstat on dangling link failed"; }
	if (!S_ISLNK(lst.st_mode)) { unlink(link); return "dangling link not reported as symlink"; }

	int fd = open(link, O_RDONLY);
	if (fd >= 0) { close(fd); unlink(link); return "open through dangling symlink unexpectedly succeeded"; }
	if (errno != ENOENT) { unlink(link); return "open through dangling symlink gave wrong errno"; }

	unlink(link);
	return NULL;
}


char* validate_hardlink_shares_inode() {
	const char *a = "hlink_a.txt";
	const char *b = "hlink_b.txt";
	const char marker[] = "hardlink-marker";

	int fd = open(a, O_CREAT | O_WRONLY, 0644);
	if (fd < 0) return "create a failed";
	write(fd, marker, sizeof(marker));
	close(fd);

	if (link(a, b) != 0) return "link() failed";

	struct stat sa, sb;
	if (stat(a, &sa) != 0 || stat(b, &sb) != 0) { unlink(a); unlink(b); return "stat failed"; }
	if (sa.st_ino != sb.st_ino) { unlink(a); unlink(b); return "hardlinked files have different inode numbers"; }
	if (sa.st_nlink != 2 || sb.st_nlink != 2) { unlink(a); unlink(b); return "nlink is not 2 after hardlink"; }

	if (unlink(a) != 0) { unlink(b); return "unlink a failed"; }

	fd = open(b, O_RDONLY);
	if (fd < 0) return "open b after unlinking a failed - data was lost prematurely";
	char buf[32] = {0};
	ssize_t n = read(fd, buf, sizeof(marker));
	close(fd);
	if (n != sizeof(marker) || memcmp(buf, marker, sizeof(marker)) != 0) {
		unlink(b);
		return "content lost after unlinking one of two hardlinks";
	}

	if (stat(b, &sb) != 0) { unlink(b); return "stat b failed after unlinking a"; }
	if (sb.st_nlink != 1) { unlink(b); return "nlink did not drop to 1 after unlinking one hardlink"; }

	unlink(b);
	return NULL;
}

char* validate_hardlink_directory_rejected() {
	if (mkdir("hlink_dir_test", 0755) != 0) return "mkdir failed";
	if (link("hlink_dir_test", "hlink_dir_test2") == 0) {
		rmdir("hlink_dir_test");
		unlink("hlink_dir_test2");
		return "link() on a directory unexpectedly succeeded";
	}
	if (errno != EPERM) { rmdir("hlink_dir_test"); return "link() on directory gave wrong errno"; }
	rmdir("hlink_dir_test");
	return NULL;
}

char* validate_timestamps() {
	const char *fname = "ts_test.txt";
	struct stat before, after_write, after_chmod;

	int fd = open(fname, O_CREAT | O_WRONLY, 0644);
	if (fd < 0) return "create failed";
	close(fd);

	if (stat(fname, &before) != 0) { unlink(fname); return "initial stat failed"; }
	if (before.st_mtime == 0 || before.st_ctime == 0) { unlink(fname); return "timestamps are zero at creation"; }

	sleep(1); /* ensure second-granularity timestamps can actually differ */

	fd = open(fname, O_WRONLY);
	if (fd < 0) { unlink(fname); return "reopen for write failed"; }
	write(fd, "x", 1);
	close(fd);

	if (stat(fname, &after_write) != 0) { unlink(fname); return "stat after write failed"; }
	if (after_write.st_mtime <= before.st_mtime) { unlink(fname); return "mtime did not advance after write"; }

	sleep(1);

	if (chmod(fname, 0600) != 0) { unlink(fname); return "chmod failed"; }
	if (stat(fname, &after_chmod) != 0) { unlink(fname); return "stat after chmod failed"; }
	if (after_chmod.st_ctime <= after_write.st_ctime) { unlink(fname); return "ctime did not advance after chmod"; }

	unlink(fname);
	return NULL;
}

char* validate_unlink_while_open() {
	const char *fname = "unlink_open_test.txt";
	const char marker[] = "still-here-after-unlink";
	int fd = open(fname, O_CREAT | O_RDWR, 0644);
	if (fd < 0) return "create failed";
	if (write(fd, marker, sizeof(marker)) != sizeof(marker)) { close(fd); unlink(fname); return "write failed"; }

	if (unlink(fname) != 0) { close(fd); return "unlink while open failed"; }

	/* file should still be fully readable via the existing fd */
	if (lseek(fd, 0, SEEK_SET) != 0) { close(fd); return "lseek failed"; }
	char buf[64] = {0};
	ssize_t n = read(fd, buf, sizeof(marker));
	if (n != sizeof(marker) || memcmp(buf, marker, sizeof(marker)) != 0) {
		close(fd);
		return "content lost immediately after unlink-while-open";
	}

	/* name should be gone even though the fd still works */
	if (open(fname, O_RDONLY) >= 0) { close(fd); return "unlinked name is still visible"; }

	close(fd); /* this is where real reclamation should finally happen */
	return NULL;
}


/*** Begin Randomized Tests and their helpers ***/

#define MAX_FILES 512
#define MAX_DATA  4100
#define ITERATIONS 10000

struct model_file {
    int exists;
    char path[PATH_MAX];
    size_t len;
    uint8_t data[MAX_DATA];
};

static int existing_file(struct model_file *m)
{
    int ids[MAX_FILES];
    int n = 0;

    for (int i = 0; i < MAX_FILES; i++)
        if (m[i].exists)
            ids[n++] = i;

    if (n == 0)
        return -1;

    return ids[rand() % n];
}

char *test_randomized_operations(void)
{
    struct model_file model[MAX_FILES] = {0};

    unsigned seed = 0x12345678;
    srand(seed);


    for (int iter = 0; iter < ITERATIONS; iter++) {

        switch (rand() % 5) {

        case 0: {                       /* CREATE */

            int slot = rand() % MAX_FILES;

            if (model[slot].exists)
                break;

            snprintf(model[slot].path,
                     sizeof(model[slot].path),
                     "f%d", slot);

            int fd = open(model[slot].path,
                          O_CREAT | O_EXCL | O_RDWR,
                          0600);

            if (fd < 0) {
                sprintf(error_message,
                        "iter=%d create failed errno=%d",
                        iter,
                        errno);
                return error_message;
            }

            close(fd);

            model[slot].exists = 1;
            model[slot].len = 0;
            break;
        }

        case 1: {                       /* WRITE */

            int id = existing_file(model);

            if (id < 0)
                break;

            size_t len = rand() % MAX_DATA;

            for (size_t i = 0; i < len; i++)
                model[id].data[i] = rand();

            model[id].len = len;

            int fd = open(model[id].path,
                          O_WRONLY | O_TRUNC);

            if (fd < 0) {
                sprintf(error_message, "write open");
                return error_message;
            }

            if (write(fd,
                      model[id].data,
                      len) != (ssize_t)len) {
                sprintf(error_message, "write");
                close(fd);
                return error_message;
            }

            close(fd);
            break;
        }

        case 2: {                       /* READ VERIFY */

            int id = existing_file(model);

            if (id < 0)
                break;

            uint8_t buf[MAX_DATA];

            int fd = open(model[id].path,
                          O_RDONLY);

            if (fd < 0) {
                sprintf(error_message, "read open");
                return error_message;
            }

            ssize_t n = read(fd,
                             buf,
                             sizeof(buf));

            close(fd);

            if (n != (ssize_t)model[id].len) {
                sprintf(error_message,
                        "length mismatch iter=%d",
                        iter);
                return error_message;
            }

            if (memcmp(buf,
                       model[id].data,
                       model[id].len)) {

                sprintf(error_message,
                        "contents mismatch iter=%d",
                        iter);
                return error_message;
            }

            break;
        }

        case 3: {                       /* RENAME */

            int id = existing_file(model);

            if (id < 0)
                break;

            char newpath[PATH_MAX];

            snprintf(newpath,
                     sizeof(newpath),
                     "r%d_%d",
                     id,
                     rand());

            if (rename(model[id].path,
                       newpath) != 0) {

                sprintf(error_message,
                        "rename failed");
                return error_message;
            }

            strcpy(model[id].path,
                   newpath);

            break;
        }

        case 4: {                       /* DELETE */

            int id = existing_file(model);

            if (id < 0)
                break;

            if (unlink(model[id].path)) {
                sprintf(error_message,
                        "unlink failed");
                return error_message;
            }

            model[id].exists = 0;
            break;
        }

        }

        /* Full consistency check */

        for (int i = 0; i < MAX_FILES; i++) {

            struct stat st;

            if (model[i].exists) {

                if (stat(model[i].path, &st)) {
                    sprintf(error_message,
                            "missing file iter=%d",
                            iter);
                    return error_message;
                }

                if ((size_t)st.st_size != model[i].len) {
                    sprintf(error_message,
                            "size mismatch iter=%d",
                            iter);
                    return error_message;
                }

            } else {

                if (stat(model[i].path, &st) == 0) {
                    sprintf(error_message,
                            "deleted file exists iter=%d",
                            iter);
                    return error_message;
                }

                if (errno != ENOENT) {
                    sprintf(error_message,
                            "unexpected errno=%d",
                            errno);
                    return error_message;
                }
            }
        }
    }

    /* cleanup */

    for (int i = 0; i < MAX_FILES; i++)
        if (model[i].exists)
            unlink(model[i].path);


    return NULL;
}