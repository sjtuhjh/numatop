/*
 * Copyright (c) 2013, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Intel Corporation nor the names of its contributors
 *     may be used to endorse or promote products derived from this software
 *     without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#define _GNU_SOURCE
#include <inttypes.h>
#include <stdlib.h>
#include <sys/types.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include "../include/types.h"
#include "../include/numatop.h"
#include "../include/util.h"
#include "../include/os/os_util.h"

uint64_t g_clkofsec;
double g_nsofclk;

boolean_t
os_authorized(void)
{
	return (B_TRUE);
}

int
os_numatop_lock(boolean_t *locked)
{
	/* Not supported on Linux */
	return (0);
}

void
os_numatop_unlock(void)
{
	/* Not supported on Linux */
}

int
os_procfs_psinfo_get(pid_t pid, void *info)
{
	/* Not supported on Linux */
	return (0);
}

/*
 * Retrieve the process's executable name from '/proc'
 */
int
os_procfs_pname_get(pid_t pid, char *buf, int size)
{
	char pname[PATH_MAX];
	int procfd; /* file descriptor for /proc/nnnnn/comm */
	int len;

	snprintf(pname, sizeof (pname), "/proc/%d/comm", pid);
	if ((procfd = open(pname, O_RDONLY)) < 0) {
		return (-1);
	}

	if ((len = read(procfd, buf, size)) < 0) {
		(void) close(procfd);
		return (-1);
	}

	buf[len - 1] = 0;
	(void) close(procfd);
	return (0);
}

/*
 * Retrieve the lwpid in process from '/proc'.
 */
int
os_procfs_lwp_enum(pid_t pid, int **lwps, int *num)
{
	char path[PATH_MAX];

	(void) snprintf(path, sizeof (path), "/proc/%d/task", pid);
	return (procfs_enum_id(path, lwps, num));
}

/*
 * Check if the specified pid/lwpid can be found in '/proc'.
 */
boolean_t
os_procfs_lwp_valid(pid_t pid, int lwpid)
{
	/* Not supported on Linux */
	return (B_TRUE);
}

/*
 * Get the TSC cycles.
 */
#ifdef __x86_64__
static uint64_t
rdtsc()
{
	uint64_t var;
	uint32_t hi, lo;

	__asm volatile
	    ("rdtsc" : "=a" (lo), "=d" (hi));

	/* LINTED E_VAR_USED_BEFORE_SET */
	var = ((uint64_t)hi << 32) | lo;
	return (var);
}
#else
static uint64_t
rdtsc()
{
	uint64_t var;

	__asm volatile
	    ("rdtsc" : "=A" (var));

	return (var);
}
#endif

/*
 * Bind current thread to a cpu or unbind current thread
 * from a cpu.
 */
static int
processor_bind(int cpu)
{
	cpu_set_t cs;

	CPU_ZERO (&cs);
	CPU_SET (cpu, &cs);

	if (sched_setaffinity(0, sizeof (cs), &cs) < 0) {
		debug_print(NULL, 2, "Fail to bind to CPU%d\n", cpu);
    		return (-1);
    	}

	return (0);
}

static int
processor_unbind(void)
{
	cpu_set_t cs;
	int i;

	CPU_ZERO (&cs);
	for (i = 0; i < g_ncpus; i++) {
		CPU_SET (i, &cs);
	}

	if (sched_setaffinity(0, sizeof (cs), &cs) < 0) {
		debug_print(NULL, 2, "Fail to unbind from CPU\n");
		return (-1);
	}

	return (0);
}

/*
 * Check the cpu name in proc info. Intel CPUs always have @ x.y
 * Ghz and that is the TSC frequency.
 */
static int
calibrate_cpuinfo(double *nsofclk, uint64_t *clkofsec)
{
	FILE *f;
	char *line = NULL, unit[11];
	size_t len = 0;
	double freq = 0.0;

	if ((f = fopen(CPUINFO_PATH, "r")) == NULL) {
		return (-1);
	}

	while (getline(&line, &len, f) > 0) {
		if (strncmp(line, "model name", sizeof ("model name") - 1) != 0) {
	    		continue;
		}

		if (sscanf(line + strcspn(line, "@") + 1, "%lf%10s",
			&freq, unit) == 2) {
			if (strcasecmp(unit, "GHz") == 0) {
				freq *= GHZ;
			} else if (strcasecmp(unit, "Mhz") == 0) {
				freq *= MHZ;				
			}
			break;
		}
	}

	free(line);
	fclose(f);

	if (fabsl(freq) < 1.0E-6) {
		return (-1);
	}

	*clkofsec = freq;
	*nsofclk = (double)NS_SEC / *clkofsec;

	debug_print(NULL, 2, "calibrate_cpuinfo: nsofclk = %.4f, "
	    "clkofsec = %lu\n", *nsofclk, *clkofsec);

	return (0);
}

/*
 * On all recent Intel CPUs, the TSC frequency is always
 * the highest p-state. So get that frequency from sysfs.
 * e.g. 2262000
 */
static int
calibrate_cpufreq(double *nsofclk, uint64_t *clkofsec)
{
	int fd, i;
	char buf[32];
	uint64_t freq;

	if ((fd = open(CPU0_CPUFREQ_PATH, O_RDONLY)) < 0) {
		return (-1);
	}

	if ((i = read(fd, buf, sizeof (buf) - 1)) <= 0) {
		close(fd);
		return (-1);
	}

	close(fd);
	buf[i] = 0;
	if ((freq = atoll(buf)) == 0) {
		return (-1);
	}

	*clkofsec = freq * KHZ;
	*nsofclk = (double)NS_SEC / *clkofsec;

	debug_print(NULL, 2, "calibrate_cpufreq: nsofclk = %.4f, "
	    "clkofsec = %lu\n", *nsofclk, *clkofsec);

	return (0);
}

/*
 * Measure how many TSC cycles in a second and how many
 * nanoseconds in a TSC cycle.
 */
static void
calibrate_by_tsc(double *nsofclk, uint64_t *clkofsec)
{
	uint64_t start_ms, end_ms, diff_ms;
	uint64_t start_tsc, end_tsc;
	int i;

	for (i = 0; i < g_ncpus; i++) {
		/*
		 * Bind current thread to cpuN to ensure the
		 * thread can not be migrated to another cpu
		 * while the rdtsc runs.
		 */
		if (processor_bind(i) == 0) {
			break;
		}
	}

	if (i == g_ncpus) {
		return;
	}

	/*
	 * Make sure the start_ms is at the beginning of
	 * one millisecond.
	 */
	end_ms = current_ms();
	while ((start_ms = current_ms()) == end_ms) {}

	start_tsc = rdtsc();
	while ((end_ms = current_ms()) < (start_ms + 100)) {}
	end_tsc = rdtsc();

	diff_ms = end_ms - start_ms;
	*nsofclk = (double)(diff_ms * NS_MS) /
	    (double)(end_tsc - start_tsc);

	*clkofsec = (uint64_t)((double)NS_SEC / *nsofclk);

	/*
	 * Unbind current thread from cpu once the measurement completed.
	 */
	processor_unbind();

	debug_print(NULL, 2, "calibrate_by_tsc: nsofclk = %.4f, "
	    "clkofsec = %lu\n", *nsofclk, *clkofsec);
}

void
os_calibrate(void)
{
	if (calibrate_cpuinfo(&g_nsofclk, &g_clkofsec) == 0) {
		return;
	}
	
	if (calibrate_cpufreq(&g_nsofclk, &g_clkofsec) == 0) {
		return;	
	}

	calibrate_by_tsc(&g_nsofclk, &g_clkofsec);
}

static boolean_t
int_get(char *str, int *digit)
{
	char *end;
	long val;

	/* Distinguish success/failure after strtol */
	errno = 0;
	val = strtol(str, &end, 10);
	if (((errno == ERANGE) && ((val == LONG_MAX) || (val == LONG_MIN))) ||
		((errno != 0) && (val == 0))) {
		return (B_FALSE);
	}

	if (end == str) {
		return (B_FALSE);
	}	

	*digit = val;
	return (B_TRUE);
}

/*
 * The function is only called for processing small digits. 
 * For example, if the string is "0-9", extract the digit 0 and 9.
 */
static boolean_t
hyphen_int_extract(char *str, int *start, int *end)
{
	char tmp[DIGIT_LEN_MAX];

	if (strlen(str) >= DIGIT_LEN_MAX) {
		return (B_FALSE);
	}

	if (sscanf(str, "%511[^-]", tmp) <= 0) {
		return (B_FALSE);
	} 
   
   	if (!int_get(tmp, start)) {
   		return (B_FALSE);
   	}
   
	if (sscanf(str, "%*[^-]-%511s", tmp) <= 0) {
		return (B_FALSE);
	}

   	if (!int_get(tmp, end)) {
   		return (B_FALSE);
   	}

	return (B_TRUE);
}

static boolean_t
arrary_add(int *arr, int arr_size, int index, int value, int num)
{
	int i;
	
	if ((index >= arr_size) || ((index + num) > arr_size)) {
		return (B_FALSE);
	}

	for (i = 0; i < num; i++) {
		arr[index + i] = value + i;
	}

	return (B_TRUE);
}

/*
 * Extract the digits from string. For example:
 * "1-2,5-7"	return 1 2 5 6 7 in "arr".
 */
static boolean_t
str_int_extract(char *str, int *arr, int arr_size, int *num)
{
	char *p, *cur, *scopy;
	int start, end, total = 0;
	int len = strlen(str);
	boolean_t ret = B_FALSE;

	if ((scopy = malloc(len + 1)) == NULL) {
		return (B_FALSE);
	}

	strncpy(scopy, str, len);
	scopy[len] = 0;
	cur = scopy;

	while (cur < (scopy + len)) {
		if ((p = strchr(cur, ',')) != NULL) {
			*p = 0;
		}

		if (strchr(cur, '-') != NULL) {
			if (hyphen_int_extract(cur, &start, &end)) {
				if (arrary_add(arr, arr_size, total, start, end - start + 1)) {
					total += end - start + 1;
				} else {
					goto L_EXIT;
				}
			}
		} else {
			if (int_get(cur, &start)) {
				if (arrary_add(arr, arr_size, total, start, 1)) {
					total++;
				} else {
					goto L_EXIT;	
     				}
			}
		}

		if (p != NULL) {
			cur = p + 1;
		} else {
			break;
		}
	}

	*num = total;
	ret = B_TRUE;

L_EXIT:	
	free(scopy);
	return (ret);
}

static boolean_t
file_int_extract(char *path, int *arr, int arr_size, int *num)
{
	FILE *fp;
	char buf[LINE_SIZE];
	
	if ((fp = fopen(path, "r")) == NULL) {
		return (B_FALSE);
	}

	if (fgets(buf, LINE_SIZE, fp) == NULL) {
		fclose(fp);
		return (B_FALSE);
	}

	fclose(fp);
	return (str_int_extract(buf, arr, arr_size, num));
}

boolean_t
os_sysfs_node_enum(int *node_arr, int arr_size, int *num)
{
	return (file_int_extract(NODE_NONLINE_PATH, node_arr, arr_size, num));
}

boolean_t
os_sysfs_cpu_enum(int nid, int *cpu_arr, int arr_size, int *num)
{
	char path[PATH_MAX];
	
	snprintf(path, PATH_MAX, "%s/node%d/cpulist", NODE_INFO_ROOT, nid);
	return (file_int_extract(path, cpu_arr, arr_size, num));
}

int
os_sysfs_online_ncpus(void)
{
	int cpu_arr[NCPUS_MAX], num;
	char path[PATH_MAX];

	if (sysconf(_SC_NPROCESSORS_CONF) > NCPUS_MAX) {
		return (-1);
	}

	snprintf(path, PATH_MAX, "/sys/devices/system/cpu/online");
	if (!file_int_extract(path, cpu_arr, NCPUS_MAX, &num)) {
		return (-1);
	}

	return (num);
}

static boolean_t
memsize_parse(char *str, uint64_t *size)
{
	char *p;
	char tmp[DIGIT_LEN_MAX];
	
	if ((p = strchr(str, ':')) == NULL) {
		return (B_FALSE);
	}

	++p;
	if (sscanf(p, "%*[^0-9]%511[0-9]", tmp) <= 0) {
		return (B_FALSE);
	}

	*size = strtoll(tmp, NULL, 10) * KB_BYTES;
	return (B_TRUE);
}

boolean_t
os_sysfs_meminfo(int nid, node_meminfo_t *info)
{
	FILE *fp;
	char path[PATH_MAX];
	char *line = NULL;
	size_t len = 0;
	boolean_t ret = B_FALSE;
	int num = sizeof (node_meminfo_t) / sizeof (uint64_t), i = 0;

	memset(info, 0, sizeof (node_meminfo_t));	
	snprintf(path, PATH_MAX, "%s/node%d/meminfo", NODE_INFO_ROOT, nid);
	if ((fp = fopen(path, "r")) == NULL) {
		return (B_FALSE);
	}

	while ((getline(&line, &len, fp) > 0) && (i < num)) {
		if (strstr(line, "MemTotal:") != NULL) {
			if (!memsize_parse(line, &info->mem_total)) {
				goto L_EXIT;
			}
			i++;
			continue;
		}

		if (strstr(line, "MemFree:") != NULL) {
			if (!memsize_parse(line, &info->mem_free)) {
				goto L_EXIT;
			}
			i++;
			continue;
		}

		if (strstr(line, "Active:") != NULL) {
			if (!memsize_parse(line, &info->active)) {
				goto L_EXIT;
			}
			i++;
			continue;
		}

		if (strstr(line, "Inactive:") != NULL) {			
			if (!memsize_parse(line, &info->inactive)) {
				goto L_EXIT;
			}
			i++;
			continue;
		}

		if (strstr(line, "Dirty:") != NULL) {
			if (!memsize_parse(line, &info->dirty)) {
				goto L_EXIT;
			}
			i++;
			continue;
		}

		if (strstr(line, "Writeback:") != NULL) {
			if (!memsize_parse(line, &info->writeback)) {
				goto L_EXIT;
			}
			i++;
			continue;
		}

		if (strstr(line, "Mapped:") != NULL) {
			if (!memsize_parse(line, &info->mapped)) {
				goto L_EXIT;
			}
			i++;
			continue;
		}
	}

	ret = B_TRUE;

L_EXIT:
	if (line != NULL) {
		free(line);
	}

	fclose(fp);
	return (ret);
}

int
os_sysfs_cqm_llc_scale(const char *path, double *scale)
{
	FILE *fp;
	char buf[LINE_SIZE];

	*scale = 0.0;

	if ((fp = fopen(path, "r")) == NULL) {
		return (-1);
	}

	if (fgets(buf, LINE_SIZE, fp) == NULL) {
		fclose(fp);
		return (-1);
	}

	fclose(fp);
	*scale = strtod(buf, NULL);

	return 0;
}

int
os_sysfs_uncore_qpi_init(qpi_info_t *qpi, int num)
{
	int i, fd, qpi_num = 0;
	char path[PATH_MAX], buf[32];

	for (i = 0; i < num; i++)
	{
		snprintf(path, PATH_MAX, "/sys/devices/uncore_qpi_%d/type", i);
		if ((fd = open(path, O_RDONLY)) < 0)
			return qpi_num;

		if (read(fd, buf, sizeof(buf)) < 0) {
			close(fd);
			return qpi_num;
		}

		qpi_num++;
		qpi[i].type = atoi(buf);
		qpi[i].config = 0x600;
		qpi[i].id = i;
		qpi[i].value_scaled = 0;
		memset(qpi[i].values, 0, sizeof(qpi[i].values));
		qpi[i].fd = INVALID_FD;
		close(fd);
	}

	return qpi_num;
}

int
os_sysfs_uncore_upi_init(qpi_info_t *qpi, int num)
{
	int i, fd, qpi_num = 0;
	char path[PATH_MAX], buf[32];

	for (i = 0; i < num; i++)
	{
		snprintf(path, PATH_MAX, "/sys/devices/uncore_upi_%d/type", i);
		if ((fd = open(path, O_RDONLY)) < 0)
			return qpi_num;

		if (read(fd, buf, sizeof(buf)) < 0) {
			close(fd);
			return qpi_num;
		}

		qpi_num++;
		qpi[i].type = atoi(buf);
		qpi[i].config = 0x0f02;
		qpi[i].id = i;
		qpi[i].value_scaled = 0;
		memset(qpi[i].values, 0, sizeof(qpi[i].values));
		qpi[i].fd = INVALID_FD;
		close(fd);
	}

	return qpi_num;
}

int
os_sysfs_uncore_imc_init(imc_info_t *imc, int num)
{
	int i, fd, imc_num = 0;
	char path[PATH_MAX], buf[32];
	
	for (i = 0; i < num; i++)
	{
		snprintf(path, PATH_MAX, "/sys/devices/uncore_imc_%d/type", i);
		if ((fd = open(path, O_RDONLY)) < 0)
			return imc_num;

		if (read(fd, buf, sizeof(buf)) < 0) {
			close(fd);
			return imc_num;
		}		

		imc_num++;
		imc[i].type = atoi(buf);
		imc[i].id = i;
		imc[i].value_scaled = 0;
		memset(imc[i].values, 0, sizeof(imc[i].values));
		imc[i].fd = INVALID_FD;
		close(fd);
	}

	return imc_num;
}
