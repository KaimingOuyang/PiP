#include <stdio.h>
#include <pip.h>
#include "pmap.h"

int gvar = 12345;

#define N	(5)

pip_barrier_t barrier = PIP_BARRIER_INIT(N);

int main( int argc, char **argv ) {
  pip_barrier_t *barrp;
  int pipid, ntasks, i;

  pip_init( &pipid, &ntasks, NULL, 0 );
  if( ntasks != N ) {
    printf( "Number of PiP tasks must be %d\n", N );
    return 9;
  }

  gvar = pipid;
  pip_get_addr( 0, "barrier", (void**) &barrp );
  pip_barrier_wait( barrp );
  printf( "<%d> gvar=%d @%p\n", pipid, gvar, &gvar );
  fflush( stdout );
  pip_barrier_wait( barrp );
  for( i=0; i<ntasks; i++ ) {
    if( i == pipid ) print_maps();
    pip_barrier_wait( barrp );
  }
  pip_fin();
  return 0;
}
