#include <stdio.h>
#include <stdlib.h>
#include "sfs_mkfs.h"

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s <image> [inode_count]\n", prog);
	exit(1);
}

int main(int argc, char **argv)
{
	if (argc < 2 || argc > 3)
		usage(argv[0]);

	sfs_mkfs_init(argv[1], argc == 3 ? argv[2] : NULL);
	return 0;
}
