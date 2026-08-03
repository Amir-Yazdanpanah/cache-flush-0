#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
	char buf[256];
	int fd = open("/dev/cache_flush", O_RDWR);

	write(fd, "Hello", 5);

	int n = read(fd, buf, sizeof(buf));

	printf("read %d bytes: %.*s\n", n, n, buf);

	close(fd);

	return 0;
}

