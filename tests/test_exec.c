#include <stdio.h>
#include <string.h>
#include<stdlib.h>
#include<unistd.h>
#include <stdlib.h>


#define MSG "Hello World"


int main(int argc, char *argv[])
{
    FILE *f;
    int ret;
    char *args[] = {"cat", "fixtures/testcat.data",NULL};
    char *envp[] = {NULL};

    printf("\nTest Case: test_exec*: %s\n", argv[1]);
    printf("Test Case PID: %u\n", getpid());
    printf("LD_PRELOAD: %s\n", getenv("LD_PRELOAD"));
    printf("WISK_TRACKER_DEBUGLEVEL: %s\n", getenv("WISK_TRACKER_DEBUGLEVEL"));
    printf("WISK_TRACKER_PIPE: %s\n", getenv("WISK_TRACKER_PIPE"));
    printf("WISK_TRACKER_UUID: %s\n", getenv("WISK_TRACKER_UUID"));
    f = fopen("/tmp/testfile1", "w");
    fprintf(f, MSG);
    fclose(f);
    if (argc > 1) {
		if (strcmp(argv[1], "execv")==0) {
			printf("Running: execv\n");
			ret = execv("/bin/cat", args);
		} else if (strcmp(argv[1], "execve")==0) {
			printf("Running: execve\n");
			ret = execve("/bin/cat", args, envp);
		} else if (strcmp(argv[1], "execvpe")==0) {
			printf("Running: execvpe\n");
			ret = execvpe("cat", args, envp);
		} else if (strcmp(argv[1], "execvp")==0) {
			printf("Running: execvp\n");
			ret = execvp("cat", args);
		} else if (strcmp(argv[1], "execl")==0) {
			printf("Running: execl\n");
			ret = execl("/bin/cat", "cat", "fixtures/testcat.data", NULL);
		} else if (strcmp(argv[1], "execle")==0) {
			printf("Running: execle\n");
			ret = execle("/bin/cat", "cat", "fixtures/testcat.data", NULL, envp);
		} else if (strcmp(argv[1], "execlp")==0) {
			printf("Running: execlp\n");
			ret = execlp("cat", "cat", "fixtures/testcat.data", NULL);
//		} else if (strcmp(argv[1], "execlpe")==0) {
//			printf("Running: execlpe\n");
//			ret = execlpe("cat", "cat", "fixtures/testcat.data", NULL, envp);
		}
    }

    printf("\nsub command complete\n");
    return 0;
}
