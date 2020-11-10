#include <stdio.h>
#include <string.h>
#include <signal.h> /* signal() */

void 
sighandler(int signo){
	fprintf(stderr, "nothing serious, continue please!\n");
}

int
main(int argc, char *argv[]){
	if(signal(SIGTERM, sighandler) == SIG_ERR)
		printf("ERROR CHAMPAGNE!");
	while(1){}
	return 0;
}

