/* 
 * (C) 2001 Clemson University and The University of Chicago 
 *
 * See COPYING in top-level directory.
 */

#include <stdio.h>
#include <string.h>

/* PINT_string_count_segments()
 *
 * Count number of segments in a path.
 *
 * Parameters:
 * pathname   - pointer to string
 *
 * Returns number of segments in pathname; -1 if
 * pathname is invalid or has no components
 *
 * Example inputs and return values:
 *
 * NULL              - returns -1
 * /                 - returns -1
 * filename          - returns  1
 * /filename         - returns  1
 * /filename/        - returns  1
 * /filename//       - returns  1
 * /dirname/filename - returns  2
 * dirname/filename
 *
 */
int PINT_string_count_segments(char *pathname)
{
    int count = 0;
    char *segp = (char *)0;
    void *segstate;

    while(!PINT_string_next_segment(pathname,&segp,&segstate))
    {
        count++;
    }
    return count;
}

/* PINT_get_base_dir()
 *
 * Get base (parent) dir of a absolute path
 *
 * Parameters:
 * pathname     - pointer to directory string
 * out_base_dir - pointer to out base dir string
 * max_out_len  - max length of out_base_dir buffer
 *
 * All incoming arguments must be valid and non-zero
 *
 * Returns 0 on success; -1 if args are invalid
 *
 * Example inputs and outputs/return values:
 *
 * pathname: /tmp         - out_base_dir: /         - returns  0
 * pathname: /tmp/foo     - out_base_dir: /tmp      - returns  0
 * pathname: /tmp/foo/bar - out_base_dir: /tmp/foo  - returns  0
 *
 *
 * invalid pathname input examples:
 * pathname: /            - out_base_dir: undefined - returns -1
 * pathname: NULL         - out_base_dir: undefined - returns -1
 * pathname: foo          - out_base_dir: undefined - returns -1
 *
 */
int PINT_get_base_dir(char *pathname, char *out_base_dir, int out_max_len)
{
    int ret = -1, len = 0;
    char *start, *end;

    if (pathname && out_base_dir && out_max_len)
    {
        if ((strcmp(pathname,"/") == 0) || (pathname[0] != '/'))
        {
            return ret;
        }

        start = pathname;
        end = (char *)(pathname + strlen(pathname));

        while(end && (end > start) && (*(--end) != '/'));

        /*
          get rid of trailing slash unless we're handling
          the case where parent is the root directory
          (in root dir case, len == 1)
        */
        len = (int)((char *)(++end - start));
        if (len != 1)
        {
            len--;
        }
        if (len < out_max_len)
        {
            memcpy(out_base_dir,start,len);
            out_base_dir[len] = '\0';
            ret = 0;
        }
    }
    return ret;
}

/* PINT_remove_base_dir()
 *
 * Get absolute path minus the base dir
 *
 * Parameters:
 * pathname     - pointer to directory string
 * out_base_dir - pointer to out dir string
 * max_out_len  - max length of out_base_dir buffer
 *
 * All incoming arguments must be valid and non-zero
 *
 * Returns 0 on success; -1 if args are invalid
 *
 * Example inputs and outputs/return values:
 *
 * pathname: /tmp/foo     - out_base_dir: foo       - returns  0
 * pathname: /tmp/foo/bar - out_base_dir: bar       - returns  0
 *
 *
 * invalid pathname input examples:
 * pathname: /            - out_base_dir: undefined - returns -1
 * pathname: NULL         - out_base_dir: undefined - returns -1
 * pathname: foo          - out_base_dir: undefined - returns -1
 *
 */
int PINT_remove_base_dir(char *pathname, char *out_dir, int out_max_len)
{
    int ret = -1, len = 0;
    char *start, *end, *end_ref;

    if (pathname && out_dir && out_max_len)
    {
        if ((strcmp(pathname,"/") == 0) || (pathname[0] != '/'))
        {
            return ret;
        }

        start = pathname;
        end = (char *)(pathname + strlen(pathname));
        end_ref = end;

        while(end && (end > start) && (*(--end) != '/'));

        len = (int)((char *)(end_ref - ++end));
        if (len < out_max_len)
        {
            memcpy(out_dir,end,len);
            out_dir[len] = '\0';
            ret = 0;
        }
    }
    return ret;
}

/* PINT_string_next_segment()
 *
 * Parameters:
 * pathname   - pointer to string
 * inout_segp - address of pointer; NULL to get first segment,
 *              pointer to last segment to get next segment
 * opaquep    - address of void *, used to maintain state outside
 *              the function
 *
 * Returns 0 if segment is available, -1 on end of string.
 *
 * This approach is nice because it keeps all the necessary state
 * outside the function and concisely stores it in two pointers.
 *
 * Internals:
 * We're using opaquep to store the location of where we placed a
 * '\0' separator to get a segment.  The value is undefined when
 * called with *inout_segp == NULL.  Afterwards, if we place a '\0',
 * *opaquep will point to where we placed it.  If we do not, then we
 * know that we've hit the end of the path string before we even
 * start processing.
 *
 * Note that it is possible that *opaquep != NULL and still there are
 * no more segments; a trailing '/' could cause this, for example.
 */
int PINT_string_next_segment(char *pathname,
                             char **inout_segp,
                             void **opaquep)
{
    char *ptr = (char *)0;

    /* initialize our starting position */
    if (*inout_segp == NULL) {
	ptr = pathname;
    }
    else if (*opaquep != NULL) {
	/* replace the '/', point just past it */
	ptr = (char *) *opaquep;
	*ptr = '/';
	ptr++;
    }
    else return -1; /* NULL *opaquep indicates last segment returned last time */

    /* at this point, the string is back in its original state */

    /* jump past separators */
    while ((*ptr != '\0') && (*ptr == '/')) ptr++;
    if (*ptr == '\0') return -1; /* all that was left was trailing '/'s */

    *inout_segp = ptr;

    /* find next separator */
    while ((*ptr != '\0') && (*ptr != '/')) ptr++;
    if (*ptr == '\0') *opaquep = NULL; /* indicate last segment */
    else {
	/* terminate segment and save position of terminator */
	*ptr = '\0';
	*opaquep = ptr;
    }
    return 0;
}

