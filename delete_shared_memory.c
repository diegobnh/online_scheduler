#include <sys/mman.h>
#define STORAGE_ID "SHM_TEST"

int main(void){

   shm_unlink(STORAGE_ID);
   return 0;
}
