/*
 * (C) 2002 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

/* bring a file into trove from some unix-accessible fs */

#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <getopt.h>

#include <trove.h>
#include "trove-test.h"

char storage_space[SSPACE_SIZE] = "/tmp/storage-space-foo";
char file_system[FS_SIZE] = "fs-foo";
char path_from_unix[PATH_SIZE];
char path_to_file[PATH_SIZE];
TROVE_handle requested_file_handle = 4095;

int parse_args(int argc, char **argv);

int main(int argc, char **argv)
{
    int ret, count, i, fd;
    struct stat u_stat;
    TROVE_op_id op_id;
    TROVE_coll_id coll_id;
    TROVE_handle file_handle, parent_handle;
    TROVE_ds_state state;
    TROVE_keyval_s key, val;
    TROVE_ds_attributes_s s_attr;
    TROVE_size f_size;
    char *method_name, *file_name;
    char path_name[PATH_SIZE];
    char *buf;

    ret = parse_args(argc, argv);
    if (ret < 0) {
	fprintf(stderr, "argument parsing failed.\n");
	return -1;
    }

    if (optind + 1 >= argc) return -1;

    strcpy(path_from_unix, argv[optind]);
    strcpy(path_to_file, argv[optind+1]);

    ret = trove_initialize(storage_space, 0, &method_name, 0);
    if (ret < 0) {
	fprintf(stderr, "initialize failed.\n");
	return -1;
    }

    /* try to look up collection used to store file system */
    ret = trove_collection_lookup(file_system, &coll_id, NULL, &op_id);
    if (ret < 0) {
	fprintf(stderr, "collection lookup failed.\n");
	return -1;
    }

    /* find the parent directory name */
    strcpy(path_name, path_to_file);
    for (i=strlen(path_name); i >= 0; i--) {
	if (path_name[i] != '/') path_name[i] = '\0';
	else break;
    }
    file_name = path_to_file + strlen(path_name);
#if 0
    printf("path is %s\n", path_name);
    printf("file is %s\n", file_name);
#endif

    /* find the parent directory handle */
    ret = path_lookup(coll_id, path_name, &parent_handle);
    if (ret < 0) {
	return -1;
    }

    /* Q: how do I know what handle to use for the new file? */
    file_handle = requested_file_handle;

    /* create the new dspace */
    ret = trove_dspace_create(coll_id,
			      &file_handle,
			      0xffffffff,
			      TROVE_TEST_FILE,
			      NULL,
			      0 /* flags */,
			      NULL,
			      &op_id);
    while (ret == 0) ret = trove_dspace_test(coll_id, op_id, &count, NULL, NULL, &state);
    if (ret < 0) {
	fprintf(stderr, "dspace create failed.\n");
	return -1;
    }

    /* TODO: set attributes of file? */
    s_attr.fs_id  = coll_id; /* for now */
    s_attr.handle = file_handle;
    s_attr.type   = TROVE_TEST_FILE; /* shouldn't need to fill this one in. */
    s_attr.uid    = getuid();
    s_attr.gid    = getgid();
    s_attr.mode   = 0755;
    s_attr.ctime  = time(NULL);
    count = 1;

    ret = trove_dspace_setattr(coll_id,
			       file_handle,
			       &s_attr,
			       0 /* flags */,
			       NULL,
			       &op_id);
    while (ret == 0) ret = trove_dspace_test(coll_id, op_id, &count, NULL, NULL, &state);
    if (ret < 0) return -1;

    /* add new file name/handle pair to parent directory */
    key.buffer = file_name;
    key.buffer_sz = strlen(file_name) + 1;
    val.buffer = &file_handle;
    val.buffer_sz = sizeof(file_handle);

    ret = trove_keyval_write(coll_id, parent_handle, &key, &val, 0, NULL, NULL, &op_id);
    count = 1;
    while (ret == 0) ret = trove_dspace_test(coll_id, op_id, &count, NULL, NULL, &state);
    if (ret < 0) {
	fprintf(stderr, "keyval write failed.\n");
	return -1;
    }

    /* open up the unix file */
    fd = open(path_from_unix, O_RDONLY);
    fstat(fd, &u_stat);

    /* get a buffer */
    buf = (char *) malloc((size_t) u_stat.st_size);
    if (buf == NULL) return -1;

    /* pull data from file */
    read(fd, buf, u_stat.st_size);

    f_size = (TROVE_size) u_stat.st_size;
    /* write data out to trove file */
    ret = trove_bstream_write_at(coll_id,
				 file_handle,
				 buf,
				 &f_size,
				 0, /* offset */
				 0, /* flags */
				 NULL, /* vtag */
				 NULL, /* user ptr */
				 &op_id);
    count = 1;
    while ( ret == 0) ret = trove_dspace_test(coll_id, op_id, &count, NULL, NULL, &state);
    if (ret < 0 ) {
	fprintf(stderr, "bstream write failed.\n");
	return -1;
    }
    trove_finalize();

#if 0
    printf("created file %s (handle = %d)\n", file_name, (int) file_handle);
#endif
    return 0;
}

int parse_args(int argc, char **argv)
{
    int c;

    while ((c = getopt(argc, argv, "s:c:h:")) != EOF) {
	switch (c) {
	    case 's':
		strncpy(storage_space, optarg, SSPACE_SIZE);
		break;
	    case 'c': /* collection */
		strncpy(file_system, optarg, FS_SIZE);
		break;
	    case 'h':
		requested_file_handle = atoll(optarg);
		break;
	    case '?':
	    default:
		return -1;
	}
    }
    return 0;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 noexpandtab
 */
