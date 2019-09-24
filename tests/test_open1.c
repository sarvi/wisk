#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#define MSG "Hello World"

int main(void)
{
    int fd;
    fd = open("\n/tmp/testfile1\n", O_WRONLY);
    write(fd, MSG, strlen(MSG));
    close(fd);
    fd = open("/tmp/testfile1", O_WRONLY);
    write(fd, MSG, strlen(MSG));
    close(fd);
    fd = open("/tmp/testfile1", O_RDONLY);
    write(fd, MSG, strlen(MSG));
    close(fd);
}
