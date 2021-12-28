#include <sys/mman.h>
#define STORAGE_ID "MY_SHARED_MEMORY"

int main(void){

   shm_unlink(STORAGE_ID);
   return 0;
}
