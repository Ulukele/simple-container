#define _GNU_SOURCE
#include <sched.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <syscall.h>

#define STACK_SIZE (5*1024*1024)
#define MAX_USERNAME (18) // container-<uid>
#define DIR_NAME_MAXLEN (MAX_USERNAME + 4)     // ./container-<uid>/u
                                               // ./container-<uid>/w
                                               // ./container-<uid>/m

static char child_stack[STACK_SIZE];

char * const args[] = {
    "/bin/sh",
    NULL
};

struct properties { int uid; };
char container_name[MAX_USERNAME];

int init_root(int uid) {
    int err = mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
    if (err == -1) {
        printf("mount failed: %s\n", strerror(errno));
	return 1;
    }
    printf("unmount /\n");

    if (mkdir(container_name, 0777) && errno != EEXIST) {
        perror("unable to create container dir");
        return 1;
    }
    if (chdir(container_name)) {
        printf("unable to change directory (./%s): %s\n", container_name, strerror(errno));
	return 1;
    }
    if (mkdir("./u", 0777) && errno != EEXIST) {
        perror("unable to create upper dir");
        return 1;
    }
    if (mkdir("./w", 0777) && errno != EEXIST) {
        perror("unable to create work dir");
        return 1;
    }
    if (mkdir("./m", 0777) && errno != EEXIST) {
        perror("unable to create merge dir");
        return 1;
    }

    err = mount("overlay", "./m", "overlay", MS_MGC_VAL, "lowerdir=../alpine,upperdir=./u,workdir=./w");
    printf("mount / to alpine with overlayfs\n");

    if (err == -1) {
        printf("overlay mount failed: %s\n", strerror(errno));
	return 1;
    }
    if (chdir("./m")) {
        printf("unable to change directory (./m): %s\n", strerror(errno));
	return 1;
    }

    const char *old_dir = "./old";
    if (mkdir(old_dir, 0777) && errno != EEXIST) {
        perror("unable to make ./old dir");
	return 1;
    }

    if (syscall(SYS_pivot_root, ".", old_dir)) {
        printf("unable to pivot_root\n");
	return 1;
    }

    if (chdir("/")) {
	perror("unable to chdir to new root");
	return 1;
    }
    printf("go to /\n");

    return 0;
}

void init_user(int uid) {
    sprintf(container_name, "container-%d", uid);
    sethostname(container_name, strlen(container_name) + 1);
    printf("set username: %s\n", container_name);
}

int child_func(void* arg) {

    // parse params
    struct properties* properties = (struct properties*)arg;
    int uid = properties->uid;

    printf("\n- init user...\n");
    init_user(uid);
    printf("\n- init root directory...\n");
    if (init_root(uid) != 0) {
        return 1;
    }

    printf(">----------%s----------<\n\n", container_name);
    execv(args[0], args);
    printf("exec failed: %s\n", strerror(errno));
    return 1;
}

int main() {
    int child_pid;
    int parent_pid = getpid();
    printf("self pid       %d\n", parent_pid);

    struct properties prop = {parent_pid};
    child_pid = clone(child_func, child_stack + STACK_SIZE, 
		    CLONE_NEWNET | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | SIGCHLD, &prop);
    printf("container pid  %d\n", child_pid);    
    if (child_pid == -1) {
        printf("clone failed: %s\n", strerror(errno));
        return 1;
    }

    waitpid(child_pid, NULL, 0);
    return 0;
}
