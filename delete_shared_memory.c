#include <sys/mman.h>
#define STORAGE_ID "SCHEDULER_SHM"

int main(void){

   shm_unlink(STORAGE_ID);
   return 0;
}
