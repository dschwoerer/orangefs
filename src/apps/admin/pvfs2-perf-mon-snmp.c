/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#include <getopt.h>

#include "pvfs2.h"
#include "pvfs2-mgmt.h"
#include "pvfs2-internal.h"

#define HISTORY 1
#define CMD_BUF_SIZE 256

#define OID_REQ ".1.3.6.1.4.1.7778.1"
#define OID_READ ".1.3.6.1.4.1.7778.2"
#define OID_WRITE ".1.3.6.1.4.1.7778.3"
#define OID_MREAD ".1.3.6.1.4.1.7778.4"
#define OID_MWRITE ".1.3.6.1.4.1.7778.5"
#define OID_DSPACE ".1.3.6.1.4.1.7778.6"
#define OID_KEYVAL ".1.3.6.1.4.1.7778.7"
#define OID_REQSCHED ".1.3.6.1.4.1.7778.8"

#ifndef PVFS2_VERSION
#define PVFS2_VERSION "Unknown"
#endif

struct options
{
    char* mnt_point;
    int mnt_point_set;
    char* server_addr;
    int server_addr_set;
};

static struct options* parse_args(int argc, char* argv[]);
static void usage(int argc, char** argv);

int main(int argc, char **argv)
{
    int ret = -1;
    PVFS_fs_id cur_fs;
    struct options* user_opts = NULL;
    char pvfs_path[PVFS_NAME_MAX] = {0};
    int i,j;
    PVFS_credentials creds;
    int io_server_count;
    struct PVFS_mgmt_perf_stat** perf_matrix;
    uint64_t* end_time_ms_array;
    uint32_t* next_id_array;
    PVFS_BMI_addr_t *addr_array, server_addr;
    int tmp_type;
    uint64_t next_time;
    float bw;
    char *cmd_buffer = (char *)malloc(CMD_BUF_SIZE);

    /* look at command line arguments */
    user_opts = parse_args(argc, argv);
    if (!user_opts)
    {
        fprintf(stderr, "Error: failed to parse command line arguments.\n");
        usage(argc, argv);
        return(-1);
    }

    ret = PVFS_util_init_defaults();
    if (ret < 0)
    {
        PVFS_perror("PVFS_util_init_defaults", ret);
        return(-1);
    }
    if (user_opts->server_addr_set)
    {
        if (user_opts->server_addr &&
                (BMI_addr_lookup (&server_addr, user_opts->server_addr) == 0))
        {
            /* set up single server */
            addr_array[0] = server_addr;
            io_server_count = 1;
        }
        else
        {
            /* bad argument - address not found */
            fprintf(stderr, "Error: failed to parser server address.\n");
            usage(argc, argv);
            return(-1);
        }
    }
    else
    {
        /* will sample all servers */
        /* translate local path into pvfs2 relative path */
        ret = PVFS_util_resolve(user_opts->mnt_point,
                                &cur_fs, pvfs_path, PVFS_NAME_MAX);
        if (ret < 0)
        {
            PVFS_perror("PVFS_util_resolve", ret);
            return(-1);
        }

        PVFS_util_gen_credentials(&creds);

        /* count how many I/O servers we have */
        ret = PVFS_mgmt_count_servers(cur_fs, &creds, PVFS_MGMT_IO_SERVER,
                                    &io_server_count);
        if (ret < 0)
        {
            PVFS_perror("PVFS_mgmt_count_servers", ret);
	        return(-1);
        }
    
        /* build a list of servers to talk to */
        addr_array = (PVFS_BMI_addr_t *)
	    malloc(io_server_count * sizeof(PVFS_BMI_addr_t));
        if (addr_array == NULL)
        {
	        perror("malloc");
	        return -1;
        }
        ret = PVFS_mgmt_get_server_array(cur_fs,
				     &creds,
				     PVFS_MGMT_IO_SERVER,
				     addr_array,
				     &io_server_count);
        if (ret < 0)
        {
	        PVFS_perror("PVFS_mgmt_get_server_array", ret);
	        return -1;
        }
    }

    /* allocate a 2 dimensional array for statistics */
    perf_matrix = (struct PVFS_mgmt_perf_stat**)malloc(
                   io_server_count*sizeof(struct PVFS_mgmt_perf_stat*));
    if (!perf_matrix)
    {
        perror("malloc");
        return(-1);
    }
    for(i=0; i<io_server_count; i++)
    {
	    perf_matrix[i] = (struct PVFS_mgmt_perf_stat *)
	            malloc(HISTORY * sizeof(struct PVFS_mgmt_perf_stat));
	    if (perf_matrix[i] == NULL)
	    {
	        perror("malloc");
	        return -1;
	    }
    }

    /* allocate an array to keep up with what iteration of statistics
     * we need from each server 
     */
    next_id_array = (uint32_t *) malloc(io_server_count * sizeof(uint32_t));
    if (next_id_array == NULL)
    {
	     perror("malloc");
	     return -1;
    }
    memset(next_id_array, 0, io_server_count*sizeof(uint32_t));

    /* allocate an array to keep up with end times from each server */
    end_time_ms_array = (uint64_t *)malloc(io_server_count * sizeof(uint64_t));
    if (end_time_ms_array == NULL)
    {
	    perror("malloc");
	    return -1;
    }


    /* loop for ever, grabbing stats when requested */
    while (1)
    {
        int srv=0, smp=0;
        time_t snaptime=0;
        /* wait for a request from SNMP driver */
        ret = fgets(cmd_buffer, CMD_BUF_SIZE, stdin);

        /* if PING output PONG */
        if (!strncasecmp(cmd_buffer, "PING", 4))
        {
            fprintf(stdout,"PONG\n");
	        fflush(stdout);
            continue;
        }

        /* try to parse GET command */
        if (!strncasecmp(cmd_buffer, "GET", 3))
        {
            /* found GET read OID */
            ret = fgets(cmd_buffer, CMD_BUF_SIZE, stdin);
            /*  */
            for(c = cmd_buffer; *c != '\0'; c++)
                if (*c == '\n')
                    *c = '\0';

        }
        else
        {
            /* bad command */
            fprintf(stdout, "NONE\n");
            fflush(stdout);
            continue;
        }

        /* good command read counters */
        if (time(NULL) - snaptime > 60)
        {
            snaptime = time(NULL);
	        ret = PVFS_mgmt_perf_mon_list(cur_fs,
				          &creds,
				          perf_matrix, 
				          end_time_ms_array,
				          addr_array,
				          next_id_array,
				          io_server_count, 
				          HISTORY,
				          NULL, NULL);
	        if (ret < 0)
	        {
	            PVFS_perror("PVFS_mgmt_perf_mon_list", ret);
	            return -1;
	        }
        }

        /* format requested OID */
        if (perf_matrix[srv][smp].valid_flag)
        {
            /* valid measurement */
            if (!strcmp(cmd_buffer, OID_READ))
            {
                returnType = "INTEGER";
                returnValue = perf_matrix[srv][smp].read;
            }
            else if (!strcmp(cmd_buffer, OID_WRITE))
            {
                returnType = "INTEGER";
                returnValue = perf_matrix[srv][smp].write;
            }
            else if (!strcmp(cmd_buffer, OID_MREAD))
            {
                returnType = "COUNTER";
                returnValue = perf_matrix[srv][smp].metadata_read;
            }
            else if (!strcmp(cmd_buffer, OID_MWRITE))
            {
                returnType = "COUNTER";
                returnValue = perf_matrix[srv][smp].metadata_write;
            }
            else if (!strcmp(cmd_buffer, OID_DSPACE))
            {
                returnType = "COUNTER";
                returnValue = perf_matrix[srv][smp].dspace_queue;
            }
            else if (!strcmp(cmd_buffer, OID_KEYVAL))
            {
                returnType = "COUNTER";
                returnValue = perf_matrix[srv][smp].keyval_queue;
            }
            else if (!strcmp(cmd_buffer, OID_REQSCHED))
            {
                returnType = "INTEGER";
                returnValue = perf_matrix[srv][smp].reqsched;
            }
            else
            {
                /* invalid command */
                fprintf(stdout,"NONE\n");
                fflush(stdout);
                continue;
            }
        }
        else
        {
            /* invalid measurement */
            fprintf(stdout,"NONE\n");
            fflush(stdout);
            continue;
        }
        fprintf(stdout, "%s\n%u\n", returnType, returnValue);
        fflush(stdout);
        /* wait for next command */
    }

    PVFS_sys_finalize();

    return(ret);
}

/* parse_args()
 *
 * parses command line arguments
 *
 * returns pointer to options structure on success, NULL on failure
 */
static struct options* parse_args(int argc, char* argv[])
{
    char flags[] = "vm:";
    int one_opt = 0;
    int len = 0;

    struct options *tmp_opts = NULL;
    int ret = -1;

    /* create storage for the command line options */
    tmp_opts = (struct options *) malloc(sizeof(struct options));
    if(tmp_opts == NULL)
    {
	    return(NULL);
    }
    memset(tmp_opts, 0, sizeof(struct options));

    /* look at command line arguments */
    while((one_opt = getopt(argc, argv, flags)) != EOF)
    {
	    switch(one_opt)
        {
            case('v'):
                printf("%s\n", PVFS2_VERSION);
                exit(0);
	        case('m'):
		        len = strlen(optarg)+1;
		        tmp_opts->mnt_point = (char*)malloc(len+1);
		        if(!tmp_opts->mnt_point)
		        {
		            free(tmp_opts);
		            return(NULL);
		        }
		        memset(tmp_opts->mnt_point, 0, len+1);
		        ret = sscanf(optarg, "%s", tmp_opts->mnt_point);
		        if(ret < 1)
                {
		            free(tmp_opts);
		            return(NULL);
		        }
		/* TODO: dirty hack... fix later.  The remove_dir_prefix()
		 * function expects some trailing segments or at least
		 * a slash off of the mount point
		 */
		        strcat(tmp_opts->mnt_point, "/");
		        tmp_opts->mnt_point_set = 1;
		        break;
	        case('s'):
		        len = strlen(optarg)+1;
		        tmp_opts->server_addr = (char*)malloc(len+1);
		        if(!tmp_opts->mnt_point)
		        {
		            free(tmp_opts);
		            return(NULL);
		        }
		        memset(tmp_opts->server_addr, 0, len+1);
		        ret = sscanf(optarg, "%s", tmp_opts->server_addr);
		        if(ret < 1)
                {
		            free(tmp_opts);
		            return(NULL);
		        }
                /*
                tmp_opts->server_addr = strdup(optarg);
                */
		        tmp_opts->server_addr_set = 1;
		        break;
	        case('?'):
		        usage(argc, argv);
		        exit(EXIT_FAILURE);
	    }
    }

    if (!tmp_opts->mnt_point_set)
    {
	    free(tmp_opts);
	    return(NULL);
    }

    return(tmp_opts);
}


static void usage(int argc, char **argv)
{
    fprintf(stderr, "\n");
    fprintf(stderr, "Usage  : %s [-m fs_mount_point]\n", argv[0]);
    fprintf(stderr, "Example: %s -m /mnt/pvfs2\n", argv[0]);
    fprintf(stderr, "Usage  : %s [-s bmi_address_string]\n", argv[0]);
    fprintf(stderr, "Example: %s -s tcp://localhost:3334\n", argv[0]);
    return;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=4 sts=4 sw=4 expandtab
 */
