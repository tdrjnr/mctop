#include <mctop.h>
#include <getopt.h>

int
main(int argc, char **argv) 
{
  // NULL for automatically loading the MCT file based on the hostname of the machine
  mctopo_t* topo = mctopo_load(NULL);
  if (topo)
    {
      mctopo_print(topo);
      mctop_run_on_node(topo, 0);
      mctopo_free(topo);
    }
  return 0;
}