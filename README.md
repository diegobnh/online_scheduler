Diversos commits foram feitos nesse branch e o código não está funcional.
Para retornar para uma versão estável use os seguintes commits:


Commit quando você passou a imprimir também o numa_maps (git reset --hard 3dedfcd) Commits on Nov 23, 2021

  char cmd[30];
  sprintf(cmd, "cat /proc/%d/numa_maps > %d.trace", getpid(),g_iteration);
  system(cmd);
  
Commit antes de inserir a função migrate_pages : git reset --hard f0f4f3a
