#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

int
main(int argc, char *argv[]){
	int sign = fork();
	if(sign == 0){
		printf("I am %s process!\n", argv[1]);
	}
	if(sign > 0){
		printf("I am %s process, and my PID is %d\n", argv[2], sign);
	}

	return 0;
}
