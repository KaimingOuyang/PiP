/*
 * $RIKEN_copyright:$
 * $PIP_VERSION:$
 * $PIP_license:$
 */
/*
 * Written by Atsushi HORI <ahori@riken.jp>, 2016, 2017
 */

#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#include <malloc.h>
#include <signal.h>
#include <stdarg.h>

//#define PIP_CLONE_AND_DLMOPEN
#define PIP_DLMOPEN_AND_CLONE

#if      defined(PIP_CLONE_AND_DLMOPEN) &&  defined(PIP_DLMOPEN_AND_CLONE)
#error  "defined(PIP_CLONE_AND_DLMOPEN) &&  defined(PIP_DLMOPEN_AND_CLONE)"
#elif   !defined(PIP_CLONE_AND_DLMOPEN) && !defined(PIP_DLMOPEN_AND_CLONE)
#error "!defined(PIP_CLONE_AND_DLMOPEN) && !defined(PIP_DLMOPEN_AND_CLONE)"
#endif

//#define PIP_NO_MALLOPT

#define PIP_FREE(P)	free(P)

//#define DEBUG
//#define PRINT_MAPS
//#define PRINT_FDS

/* the EVAL env. is to measure the time for calling dlmopen() */
//#define EVAL

#include <pip.h>
#include <pip_internal.h>

#ifdef EVAL

#define ES(V,F)		\
  do { double __st=pip_gettime(); (F); (V) += pip_gettime()-__st; } while(0)
double time_dlmopen   = 0.0;
double time_load_dso  = 0.0;
double time_load_prog = 0.0;
#define REPORT(V)	 printf( "%s: %g\n", #V, V );

#else

#define ES(V,F)		(F)
#define REPORT(V)

#endif

/*** note that the following static variables are   ***/
/*** located at each PIP task and the root process. ***/
static pip_root_t	*pip_root = NULL;
static pip_task_t	*pip_task = NULL;

static pip_clone_t*	pip_cloneinfo = NULL;

static int (*pip_clone_mostly_pthread_ptr) (
	pthread_t *newthread,
	int clone_flags,
	int core_no,
	size_t stack_size,
	void *(*start_routine) (void *),
	void *arg,
	pid_t *pidp) = NULL;

#include <sys/syscall.h>
static pid_t pip_gettid( void ) {
  return syscall( SYS_gettid );
}

static int pip_root_p_( void ) {
  return pip_root != NULL && pip_task == NULL;
}

int pip_idstr( char *buf, size_t sz ) {
  int n;
  if( pip_root_p_() ) {
    n = snprintf( buf, sz, "<PIP_ROOT>" );
  } else if( pip_task != NULL ) {
    n = snprintf( buf, sz, "<PIPID:%d(%d)>", pip_task->pipid, getpid() );
  } else {
    n = snprintf( buf, sz, "(PID:%d)", getpid() );
  }
  return n;
}

static void pip_message( char *tag, char *format, va_list ap ) {
#define MESGLEN		(512)
#define PIPIDLEN	(64)
  char mesg[MESGLEN];
  char idstr[PIPIDLEN];
  int len;

  len = pip_idstr( idstr, PIPIDLEN );
  len = snprintf( &mesg[0], MESGLEN-len, tag, idstr );
  mesg[len++] = ' ';
  vsnprintf( &mesg[len], MESGLEN-len, format, ap );
  fprintf( stderr, "%s\n", mesg );
}

static void pip_info_mesg( char *format, ... ) __attribute__ ((unused));
static void pip_info_mesg( char *format, ... ) {
  va_list ap;
  va_start( ap, format );
  pip_message( "PIP-INFO%s: ", format, ap );
}

static void pip_warn_mesg( char *format, ... ) __attribute__ ((unused));
static void pip_warn_mesg( char *format, ... ) {
  va_list ap;
  va_start( ap, format );
  pip_message( "PIP-WARN%s: ", format, ap );
}

static void pip_err_mesg( char *format, ... ) __attribute__ ((unused));
static void pip_err_mesg( char *format, ... ) {
  va_list ap;
  va_start( ap, format );
  pip_message( "PIP-ERROR%s: ", format, ap );
}

static int pip_page_alloc( size_t sz, void **allocp ) {
  size_t pgsz;
  if( pip_root == NULL ) {	/* in pip_init(), no pip_root yet */
    pgsz = sysconf( _SC_PAGESIZE );
  } else {
    pgsz = pip_root->page_size;
  }
  sz = ( ( sz + pgsz - 1 ) / pgsz ) * pgsz;
  RETURN( posix_memalign( allocp, pgsz, sz ) );
}

static void pip_set_magic( pip_root_t *root ) {
  memcpy( root->magic, PIP_MAGIC_WORD, PIP_MAGIC_LEN );
}

static int pip_is_magic_ok( pip_root_t *root ) {
  return strncmp( root->magic, PIP_MAGIC_WORD, PIP_MAGIC_LEN ) == 0;
}

static int pip_is_version_ok( pip_root_t *root ) {
  if( root->version   == PIP_VERSION &&
      root->root_size == sizeof( pip_root_t ) ) return 1;
  return 0;
}

static void pip_init_task_struct( pip_task_t *taskp ) {
  memset( (void*) taskp, 0, sizeof(pip_task_t) );
  taskp->pipid  = PIP_PIPID_NONE;
  taskp->thread = pip_root->task_root->thread;
}

#include <elf.h>

static int pip_check_pie( char *path ) {
  Elf64_Ehdr elfh;
  int fd;
  int err = 0;

  if( ( fd = open( path, O_RDONLY ) ) < 0 ) {
    err = errno;
  } else {
    if( read( fd, &elfh, sizeof( elfh ) ) != sizeof( elfh ) ) {
      pip_warn_mesg( "Unable to read '%s'", path );
      err = EUNATCH;
    } else if( elfh.e_ident[EI_MAG0] != ELFMAG0 ||
	       elfh.e_ident[EI_MAG1] != ELFMAG1 ||
	       elfh.e_ident[EI_MAG2] != ELFMAG2 ||
	       elfh.e_ident[EI_MAG3] != ELFMAG3 ) {
      pip_warn_mesg( "'%s' is not an ELF file", path );
      err = EUNATCH;
    } else if( elfh.e_type != ET_DYN ) {
      pip_warn_mesg( "'%s' is not DYNAMIC (PIE)", path );
      err = ELIBEXEC;
    }
    (void) close( fd );
  }
  return err;
}

const char *pip_get_mode_str( void ) {
  char *mode;

  if( pip_root == NULL ) return NULL;
  switch( pip_root->opts & PIP_MODE_MASK ) {
  case PIP_MODE_PTHREAD:
    mode = PIP_ENV_MODE_PTHREAD;
    break;
  case PIP_MODE_PROCESS:
    mode = PIP_ENV_MODE_PROCESS;
    break;
  case PIP_MODE_PROCESS_PRELOAD:
    mode = PIP_ENV_MODE_PROCESS_PRELOAD;
    break;
  case PIP_MODE_PROCESS_PIPCLONE:
    mode = PIP_ENV_MODE_PROCESS_PIPCLONE;
    break;
  default:
    mode = "(unknown)";
  }
  return mode;
}

static void *pip_dlsym( void *handle, const char *name ) {
  void *addr;
  pip_spin_lock( &pip_root->lock_ldlinux );
  addr = dlsym( handle, name );
  pip_spin_unlock( &pip_root->lock_ldlinux );
  return( addr );
}

static void pip_dlclose( void *handle ) {
  pip_spin_lock( &pip_root->lock_ldlinux );
  dlclose( handle );
  pip_spin_unlock( &pip_root->lock_ldlinux );
}

static int pip_check_opt_and_env( int *optsp ) {
  int opts   = *optsp;
  int mode   = ( opts & PIP_MODE_MASK );
  int newmod = 0;
  char *env  = getenv( PIP_ENV_MODE );

  enum PIP_MODE_BITS {
    PIP_MODE_PTHREAD_BIT          = 1,
    PIP_MODE_PROCESS_PRELOAD_BIT  = 2,
    PIP_MODE_PROCESS_PIPCLONE_BIT = 4
  } desired = 0;

  if( ( opts & ~PIP_VALID_OPTS ) != 0 ) {
    /* unknown option(s) specified */
    RETURN( EINVAL );
  }

  if( opts & PIP_MODE_PTHREAD &&
      opts & PIP_MODE_PROCESS ) RETURN( EINVAL );
  if( opts & PIP_MODE_PROCESS ) {
    if( ( opts & PIP_MODE_PROCESS_PRELOAD  ) == PIP_MODE_PROCESS_PRELOAD &&
	( opts & PIP_MODE_PROCESS_PIPCLONE ) == PIP_MODE_PROCESS_PIPCLONE){
      RETURN (EINVAL );
    }
  }

  switch( mode ) {
  case 0:
    if( env == NULL ) {
      desired =
	PIP_MODE_PTHREAD_BIT |
	PIP_MODE_PROCESS_PRELOAD_BIT |
	PIP_MODE_PROCESS_PIPCLONE_BIT;
    } else if( strcasecmp( env, PIP_ENV_MODE_THREAD  ) == 0 ||
	       strcasecmp( env, PIP_ENV_MODE_PTHREAD ) == 0 ) {
      desired = PIP_MODE_PTHREAD_BIT;
    } else if( strcasecmp( env, PIP_ENV_MODE_PROCESS ) == 0 ) {
      desired =
	PIP_MODE_PROCESS_PRELOAD_BIT|
	PIP_MODE_PROCESS_PIPCLONE_BIT;
    } else if( strcasecmp( env, PIP_ENV_MODE_PROCESS_PRELOAD  ) == 0 ) {
      desired = PIP_MODE_PROCESS_PRELOAD_BIT;
    } else if( strcasecmp( env, PIP_ENV_MODE_PROCESS_PIPCLONE ) == 0 ) {
      desired = PIP_MODE_PROCESS_PIPCLONE_BIT;
    } else {
      pip_warn_mesg( "unknown environment setting PIP_MODE='%s'", env );
      RETURN( EPERM );
    }
    break;
  case PIP_MODE_PTHREAD:
    desired = PIP_MODE_PTHREAD_BIT;
    break;
  case PIP_MODE_PROCESS:
    if ( env == NULL ) {
      desired =
	PIP_MODE_PROCESS_PRELOAD_BIT|
	PIP_MODE_PROCESS_PIPCLONE_BIT;
    } else if( strcasecmp( env, PIP_ENV_MODE_PROCESS_PRELOAD  ) == 0 ) {
      desired = PIP_MODE_PROCESS_PRELOAD_BIT;
    } else if( strcasecmp( env, PIP_ENV_MODE_PROCESS_PIPCLONE ) == 0 ) {
      desired = PIP_MODE_PROCESS_PIPCLONE_BIT;
    } else if( strcasecmp( env, PIP_ENV_MODE_THREAD  ) == 0 ||
	       strcasecmp( env, PIP_ENV_MODE_PTHREAD ) == 0 ||
	       strcasecmp( env, PIP_ENV_MODE_PROCESS ) == 0 ) {
      /* ignore PIP_MODE=thread in this case */
      desired =
	PIP_MODE_PROCESS_PRELOAD_BIT|
	PIP_MODE_PROCESS_PIPCLONE_BIT;
    } else {
      pip_warn_mesg( "unknown environment setting PIP_MODE='%s'", env );
      RETURN( EPERM );
    }
    break;
  case PIP_MODE_PROCESS_PRELOAD:
    desired = PIP_MODE_PROCESS_PRELOAD_BIT;
    break;
  case PIP_MODE_PROCESS_PIPCLONE:
    desired = PIP_MODE_PROCESS_PIPCLONE_BIT;
    break;
  default:
    pip_warn_mesg( "pip_init() invalid argument opts=0x%x", opts );
    RETURN( EINVAL );
  }

  if( desired & PIP_MODE_PROCESS_PRELOAD_BIT ) {
    /* check if the __clone() systemcall wrapper exists or not */
    if( pip_cloneinfo == NULL ) {
      pip_cloneinfo = (pip_clone_t*)
	dlsym( RTLD_DEFAULT, "pip_clone_info");
    }
    DBGF( "cloneinfo-%p", pip_cloneinfo );
    if( pip_cloneinfo != NULL ) {
      newmod = PIP_MODE_PROCESS_PRELOAD;
    } else if( !( desired & ( PIP_MODE_PTHREAD_BIT |
			      PIP_MODE_PROCESS_PIPCLONE_BIT ) ) ) {
      /* no wrapper found */
      if( ( env = getenv( "LD_PRELOAD" ) ) == NULL ) {
	pip_warn_mesg( "process:preload mode is requested but "
		       "LD_PRELOAD environment variable is empty." );
      } else {
	pip_warn_mesg( "process:preload mode is requested but "
		       "LD_PRELOAD='%s'",
		       env );
      }
      RETURN( EPERM );
    }
  } else if( desired & PIP_MODE_PROCESS_PIPCLONE_BIT ) {
    if ( pip_clone_mostly_pthread_ptr == NULL )
      pip_clone_mostly_pthread_ptr =
	dlsym( RTLD_DEFAULT, "pip_clone_mostly_pthread" );
    if ( pip_clone_mostly_pthread_ptr != NULL ) {
      newmod = PIP_MODE_PROCESS_PIPCLONE;
    } else if( !( desired & PIP_MODE_PTHREAD_BIT) ) {
      if( desired & PIP_MODE_PROCESS_PRELOAD_BIT ) {
	pip_warn_mesg("process mode is requested but pip_clone_info symbol "
		      "is not found in $LD_PRELOAD and "
		      "pip_clone_mostly_pthread() symbol is not found in "
		      "glibc" );
      } else {
	pip_warn_mesg( "process:pipclone mode is requested but "
		       "pip_clone_mostly_pthread() is not found in glibc" );
      }
      RETURN( EPERM );
    }
  } else if( desired & PIP_MODE_PTHREAD_BIT ) {
    newmod = PIP_MODE_PTHREAD;
  }
  if( newmod == 0 ) {
    pip_warn_mesg( "pip_init() implemenation error. desired = 0x%x",
		   desired );
    RETURN( EPERM );
  }

  if( ( opts & ~PIP_MODE_MASK ) == 0 ) {
    if( ( env = getenv( PIP_ENV_OPTS ) ) != NULL ) {
      if( strcasecmp( env, PIP_ENV_OPTS_FORCEEXIT ) == 0 ) {
	opts |= PIP_OPT_FORCEEXIT;
      } else {
	pip_warn_mesg( "Unknown option %s=%s", PIP_ENV_OPTS, env );
	RETURN( EPERM );
      }
    }
  }

  *optsp = ( opts & ~PIP_MODE_MASK ) | newmod;
  RETURN( 0 );
}

int pip_init( int *pipidp, int *ntasksp, void **rt_expp, int opts ) {
  size_t	sz;
  char		*env = NULL;
  int		ntasks;
  int 		pipid;
  int 		i;
  int 		err = 0;

  if( pip_root != NULL ) RETURN( EBUSY ); /* already initialized */

  if( ( env = getenv( PIP_ROOT_ENV ) ) == NULL ) {
    /* root process ? */

    if( ntasksp == NULL ) {
      ntasks = PIP_NTASKS_MAX;
    } else if( *ntasksp <= 0 ) {
      RETURN( EINVAL );
    } else {
      ntasks = *ntasksp;
    }
    if( ntasks > PIP_NTASKS_MAX ) RETURN( EOVERFLOW );

    if( ( err = pip_check_opt_and_env( &opts ) ) != 0 ) RETURN( err );

    sz = sizeof( pip_root_t ) + sizeof( pip_task_t ) * ( ntasks + 1 );
    if( ( err = pip_page_alloc( sz, (void**) &pip_root ) ) != 0 ) {
      RETURN( err );
    }
    pip_task = NULL;
    (void) memset( pip_root, 0, sz );
    pip_root->root_size = sizeof( pip_root_t );
    pip_root->size      = sz;

    DBGF( "ROOTROOT (%p)", pip_root );

    pip_spin_init( &pip_root->lock_ldlinux   );
    pip_spin_init( &pip_root->lock_ulpstacks );
    /* beyond this point, we can call the       */
    /* pip_dlsymc() and pip_dlclose() functions */

    pipid = PIP_PIPID_ROOT;
    pip_set_magic( pip_root );
    pip_root->version   = PIP_VERSION;
    pip_root->ntasks    = ntasks;
    pip_root->cloneinfo = pip_cloneinfo;
    pip_root->opts      = opts;
    pip_root->page_size = sysconf( _SC_PAGESIZE );
    pip_root->task_root = &pip_root->tasks[ntasks];
    for( i=0; i<ntasks+1; i++ ) {
      pip_init_task_struct( &pip_root->tasks[i] );
    }
    pip_root->task_root->pipid  = PIP_PIPID_ROOT;
    pip_root->task_root->thread = pthread_self();
    pip_root->task_root->pid    = getpid();
    pip_root->task_root->symbols.free = (free_t) pip_dlsym( RTLD_DEFAULT, "free");
    if( rt_expp != NULL ) pip_root->task_root->export = *rt_expp;

    DBGF( "PIP_MODE=%s", pip_get_mode_str() );

  } else {
    /* child task */

    pip_root = (pip_root_t*) strtoll( env, NULL, 16 );
    if( !pip_is_magic_ok(   pip_root ) ) RETURN( EPERM );
    if( !pip_is_version_ok( pip_root ) ) {
      pip_warn_mesg( "Version miss-match between root and child" );
      RETURN( EPERM );
    }

    ntasks = pip_root->ntasks;
    pipid  = PIP_PIPID_NONE;
    for( i=0; i<ntasks; i++ ) {
      if( pthread_equal( pip_root->tasks[i].thread, pthread_self() ) ) {
	pipid = i;
	pip_task = &pip_root->tasks[pipid];
	break;
      }
    }
    DBGF( "CHILDCHILD (%p) %d", pip_root, pipid );
    if( pipid < 0 ) {
      err = ENXIO;
    } else {
      if( ntasksp != NULL ) *ntasksp = ntasks;
      if( rt_expp != NULL ) *rt_expp = (void*) pip_root->task_root->export;
    }
  }
  /* root and child */
  if( !err ) {
    if( pipidp != NULL ) *pipidp = pipid;
    DBGF( "pip_task=%p  pip_root=%p", pip_task, pip_root );
  }
  RETURN( err );
}

static int pip_if_pthread_( void ) {
  return (pip_root->opts & PIP_MODE_PTHREAD) != 0 ? CLONE_THREAD : 0;
}

int pip_if_pthread( int *flagp ) {
  if( pip_root == NULL ) RETURN( EPERM  );
  if( flagp    == NULL ) RETURN( EINVAL );
  *flagp = pip_if_pthread_();
  RETURN( 0 );
}

static int pip_if_shared_fd_( void ) {
  if( pip_root->cloneinfo == NULL )
    return (pip_root->opts & PIP_MODE_PTHREAD) != 0 ? CLONE_FILES : 0;
  return pip_root->cloneinfo->flag_clone & CLONE_FILES;
}

int pip_if_shared_fd( int *flagp ) {
  /* this function is only valid on the root process */
  if( pip_root == NULL ) RETURN( EPERM  );
  if( flagp    == NULL ) RETURN( EINVAL );
  *flagp = pip_if_shared_fd_();
  RETURN( 0 );
}

int pip_if_shared_sighand( int *flagp ) {
  /* this function is only valid on the root process */
  if( pip_root == NULL ) RETURN( EPERM  );
  if( flagp    == NULL ) RETURN( EINVAL );
  if( pip_root->cloneinfo == NULL ) {
    *flagp = (pip_root->opts & PIP_MODE_PTHREAD) != 0 ? CLONE_SIGHAND : 0;
  } else {
    *flagp = pip_root->cloneinfo->flag_clone & CLONE_SIGHAND;
  }
  RETURN( 0 );
}

int pip_isa_piptask( void ) {
  return getenv( PIP_ROOT_ENV ) != NULL;
}

static int pip_task_p_( void ) {
  return pip_task != NULL;
}

void *pip_get_dso_( void ) {
  if( pip_task != NULL ) return pip_task->loaded;
  return NULL;
}

int pip_get_pipid_( void ) {
  int pipid;
  if( pip_root == NULL ) {
    pipid = PIP_PIPID_ANY;
  } else if( pip_root_p_() ) {
    pipid = PIP_PIPID_ROOT;
  } else {
    pipid = pip_task->pipid;
  }
  return pipid;
}

int pip_get_pipid( int *pipidp ) {
  if( pipidp == NULL ) RETURN( EINVAL );
  *pipidp = pip_get_pipid_();
  RETURN( 0 );
}

int pip_get_ntasks( int *ntasksp ) {
  if( ntasksp  == NULL ) RETURN( EINVAL );
  if( pip_root == NULL ) return( EPERM  ); /* intentionally using small return */

  *ntasksp = pip_root->ntasks_curr;
  RETURN( 0 );
}

static pip_task_t *pip_get_task( int pipid ) {
  pip_task_t *task:
  if( pipid == PIP_PIPID_ROOT ) {
    task = pip_root->task_root;
  } else {
    task = &pip_root->tasks[pipid];
  }
  return task;
}

static pip_task_t *pip_get_myself( void ) {
  pip_task_t *task:
  if( pip_root_p_() ) {
    task = pip_root->task_root;
  } else {
    task = pip_task;
  }
  return task;
}

int pip_export( void *export ) {
  if( export == NULL ) RETURN( EINVAL );
  pip_get_myself()->export = export;
  RETURN( 0 );
}

static int pip_check_pipid( int *pipidp ) {
  int pipid = *pipidp;

  if( pip_root == NULL          ) RETURN( EPERM  );
  if( pipid >= pip_root->ntasks ) RETURN( ENOENT );
  if( pipid != PIP_PIPID_MYSELF &&
      pipid <  PIP_PIPID_ROOT   ) RETURN( EINVAL );
  if( pipid >= pip_root->ntasks ) RETURN( EINVAL );
  if( pipid == PIP_PIPID_MYSELF ) {
    if( pip_root_p_() ) {
      *pipidp = PIP_PIPID_ROOT;
    } else {
      if( pip_root->tasks[*pipidp].pipid != *pipidp ) RETURN( ENOENT );
      *pipidp = pip_task->pipid;
    }
  }
  return 0;
}

int pip_import( int pipid, void **exportp  ) {
  pip_task_t *task:
  int err;

  if( exportp == NULL ) RETURN( EINVAL );
  if( ( err = pip_check_pipid( &pipid ) ) != 0 ) RETURN( err );

  task = pip_get_task( pipid );
  *exportp = (void*) task->export;
  pip_memory_barrier();
  RETURN( 0 );
}

char **pip_copy_vec( char *addition, char **vecsrc ) {
  char **vecdst, *p;
  int vecln, veccc;
  int i, j;

  if( addition != NULL ) {
    vecln = 1;
    veccc = strlen( addition ) + 1;
  } else {
    vecln = 0;
    veccc = 0;
  }

  for( i=0; vecsrc[i]!=NULL; i++ ) {
    veccc += strlen( vecsrc[i] ) + 1;
  }
  vecln += i + 1;		/* plus final NULL */

  if( ( vecdst = (char**) malloc( sizeof(char*) * vecln + veccc ) ) == NULL ) {
    return NULL;
  }
  j = 0;
  p = ((char*)vecdst) + ( sizeof(char*) * vecln );
  vecdst[j++] = p;
  if( addition ) {
    p = stpcpy( p, addition ) + 1;
    i = 0;
  } else {
    p = stpcpy( p, vecsrc[0] ) + 1;
    i = 1;
  }
  for( ; vecsrc[i]!=NULL; i++ ) {
    vecdst[j++] = p;
    p = stpcpy( p, vecsrc[i] ) + 1;
  }
  vecdst[j] = NULL;
  if( 0 ) {
    int ii;
    for( ii=0; vecsrc[ii]!=NULL; ii++ ) {
      fprintf( stderr, "<<SRC>> vec[%d] %s\n", ii, vecsrc[ii] );
    }
    for( ii=0; vecdst[ii]!=NULL; ii++ ) {
      fprintf( stderr, "<<DST>> vec[%d] %s\n", ii, vecdst[ii] );
    }
  }
  return( vecdst );
}

static char **pip_copy_env( char **envsrc ) {
  char *rootenv;
  char **envdst;
  int cc;

  cc = asprintf( &rootenv, "%s=%p", PIP_ROOT_ENV, pip_root );
  if( cc == 0 ) return NULL;
  envdst = pip_copy_vec( rootenv, envsrc );
  PIP_FREE( rootenv );
  return envdst;
}

static size_t pip_stack_size( void ) {
  char 		*env, *endptr;
  size_t 	sz, scale;
  int 		i;

  if( ( env = getenv( PIP_ENV_STACKSZ ) ) == NULL &&
      ( env = getenv( "KMP_STACKSIZE" ) ) == NULL &&
      ( env = getenv( "OMP_STACKSIZE" ) ) == NULL ) {
    sz = PIP_ULP_STACK_SIZE;	/* default */
  } else {
    if( ( sz = (size_t) strtol( env, &endptr, 10 ) ) <= 0 ) {
      pip_warn_mesg( "'%s' is illegal and default size is set", env );
    } else {
      scale = 1;
      switch( *endptr ) {

      case 'G': case 'g':
	scale *= 1024;
      case 'M': case 'm':
	scale *= 1024 * 1024;
	sz *= scale;
	break;

      default:
	pip_warn_mesg( "'%s' is illegal and 'K' is assumed", env );
      case 'K': case 'k': case '\0':
	scale *= 1024;
      case 'B': case 'b':
	sz *= scale;
	for( i=PIP_ULP_MIN_STACK_SIZE; i<sz; i*=2 );
	sz = i;
	break;
      }
    }
  }
  return sz;
}

static void pip_close_on_exec( void ) {
  DIR *dir;
  struct dirent *direntp;
  int fd;
  int flags;

#ifdef PRINT_FDS
  pip_print_fds();
#endif

#define PROCFD_PATH		"/proc/self/fd"
  if( ( dir = opendir( PROCFD_PATH ) ) != NULL ) {
    int fd_dir = dirfd( dir );
    while( ( direntp = readdir( dir ) ) != NULL ) {
      if( ( fd = atoi( direntp->d_name ) ) >= 0 &&
	  fd != fd_dir                          &&
	  ( flags = fcntl( fd, F_GETFD ) ) >= 0 &&
	  flags & FD_CLOEXEC ) {
#ifdef DEBUG
	pip_print_fd( fd );
#endif
	(void) close( fd );
	DBGF( "<PID=%d> fd[%d] is closed (CLOEXEC)", getpid(), fd );
      }
    }
    (void) closedir( dir );
    (void) close( fd_dir );
  }
#ifdef PRINT_FDS
  pip_print_fds();
#endif
}

static int pip_load_dso( void **handlep, char *path ) {
  Lmid_t	lmid;
  int 		flags = RTLD_NOW | RTLD_LOCAL;
  /* RTLD_GLOBAL is NOT accepted and dlmopen() returns EINVAL */
  void 		*loaded;
  int		err;

  DBGF( "hanlde=%p", *handlep );
  if( *handlep == NULL ) {
    lmid = LM_ID_NEWLM;
  } else if( dlinfo( *handlep, RTLD_DI_LMID, (void*) &lmid ) != 0 ) {
    DBGF( "dlinfo(%p): %s", handlep, dlerror() );
    RETURN( ENXIO );
  }

  DBGF( "calling dlmopen(%s)", path );
  ES( time_dlmopen, ( loaded = dlmopen( lmid, path, flags ) ) );
  if( loaded == NULL ) {
    if( ( err = pip_check_pie( path ) ) != 0 ) RETURN( err );
    pip_warn_mesg( "dlmopen(%s): %s", path, dlerror() );
    RETURN( ENOEXEC );
  } else {
    DBGF( "dlmopen(%s): SUCCEEDED", path );
    *handlep = loaded;
  }
  RETURN( 0 );
}

static int pip_find_symbols( void *handle, pip_symbols_t *symp ) {
  int err = 0;

  //if( pip_root_p() ) pip_print_dsos();

  /* functions */
  symp->main        = pip_dlsym( handle, "main"         );
  symp->ctype_init  = pip_dlsym( handle, "__ctype_init" );
  symp->glibc_init  = pip_dlsym( handle, "glibc_init"   );
  symp->libc_fflush = pip_dlsym( handle, "fflush"       );
  symp->mallopt     = pip_dlsym( handle, "mallopt"      );
  symp->free        = pip_dlsym( handle, "free"         );
  /* variables */
  symp->libc_argvp  = pip_dlsym( handle, "__libc_argv"  );
  symp->libc_argcp  = pip_dlsym( handle, "__libc_argc"  );
  symp->environ     = pip_dlsym( handle, "environ"      );

  /* check mandatory symbols */
  if( symp->main == NULL || symp->environ == NULL ) {
    err = ENOEXEC;
  }
  RETURN( err );
}

static int pip_load_prog( char *prog, pip_task_t *task ) {
  void		*loaded = NULL;
  int 		err;

  DBGF( "prog=%s", prog );

#ifdef PRINT_MAPS
  pip_print_maps();
#endif
  ES( time_load_dso, ( err = pip_load_dso( &loaded, prog ) ) );
#ifdef PRINT_MAPS
  pip_print_maps();
#endif

  if( err == 0 ) {
    if( ( err = pip_find_symbols( loaded, &task->symbols ) ) == 0 ) {
#ifdef DEBUG
      pip_check_addr( "MAIN", task->symbols.main );
      pip_check_addr( "ENVP", task->symbols.environ   );
#endif
      task->loaded = loaded;
    }
  }
  if( err != 0 && loaded != NULL ) (void) pip_dlclose( loaded );
  RETURN( err );
}

#ifdef PIP_DLMOPEN_AND_CLONE
static int pip_do_corebind( int coreno, cpu_set_t *oldsetp ) {
  int err = 0;

  if( coreno != PIP_CPUCORE_ASIS ) {
    cpu_set_t cpuset;

    CPU_ZERO( &cpuset );
    CPU_SET( coreno, &cpuset );

    if( pip_if_pthread_() ) {
      err = pthread_getaffinity_np( pthread_self(),
				    sizeof(cpu_set_t),
				    oldsetp );
      if( err == 0 ) {
	err = pthread_setaffinity_np( pthread_self(),
				      sizeof(cpu_set_t),
				      &cpuset );
      }
    } else {
      if( sched_getaffinity( 0, sizeof(cpuset), oldsetp ) != 0 ||
	  sched_setaffinity( 0, sizeof(cpuset), &cpuset ) != 0 ) {
	err = errno;
      }
    }
  }
  RETURN( err );
}

static int pip_undo_corebind( int coreno, cpu_set_t *oldsetp ) {
  int err = 0;

  if( coreno != PIP_CPUCORE_ASIS ) {
    if( pip_if_pthread_() ) {
      err = pthread_setaffinity_np( pthread_self(),
				    sizeof(cpu_set_t),
				    oldsetp );
    } else {
      if( sched_setaffinity( 0, sizeof(cpu_set_t), oldsetp ) != 0 ) {
	err = errno;
      }
    }
  }
  RETURN( err );
}
#endif

static int pip_corebind( int coreno ) {
  cpu_set_t cpuset;

  if( coreno != PIP_CPUCORE_ASIS ) {
    CPU_ZERO( &cpuset );
    CPU_SET( coreno, &cpuset );
    if( sched_setaffinity( 0, sizeof(cpuset), &cpuset ) != 0 ) RETURN( errno );
  }
  RETURN( 0 );
}

static int pip_glibc_init( pip_symbols_t *symbols, char **argv, char **envv ) {
  int argc;

  for( argc=0; argv[argc]!=NULL; argc++ );
  if( symbols->libc_argcp != NULL ) {
    DBGF( "&__libc_argc=%p\n", symbols->libc_argcp );
    *symbols->libc_argcp = argc;
  }
  if( symbols->libc_argvp != NULL ) {
    DBGF( "&__libc_argv=%p\n", symbols-.libc_argvp );
    *symbols->libc_argvp = argv;
  }
  *symbols->environ = envv;	/* setting environment vars */

#ifndef PIP_NO_MALLOPT
  if( symbols->mallopt != NULL ) {
    DBGF( ">> mallopt()" );
    if( symbols->mallopt( M_MMAP_THRESHOLD, 0 ) == 1 ) {
      DBGF( "<< mallopt(M_MMAP_THRESHOLD): succeeded" );
    } else {
      DBGF( "<< mallopt(M_MMAP_THRESHOLD): failed !!!!!!" );
    }
    if( symbols->mallopt( M_TRIM_THRESHOLD, -1 ) == 1 ) {
      DBGF( "<< mallopt(M_TRIM_THRESHOLD): succeeded" );
    } else {
      DBGF( "<< mallopt(M_TRIM_THRESHOLD): failed !!!!!!" );
    }
  }
#endif

  if( symbols->glibc_init != NULL ) {
    DBGF( "[%d] >> glibc_init@%p()", pipid, symbols->glibc_init );
    symbols->glibc_init( argc, argv, envv );
    DBGF( "[%d] << glibc_init@%p()", pipid, symbols->glibc_init );
  } else if( symbols->ctype_init != NULL ) {
    DBGF( "[%d] >> __ctype_init@%p()", pipid, symbols->ctype_init );
    symbols->ctype_init();
    DBGF( "[%d] << __ctype_init@%p()", pipid, symbols->ctype_init );
  }
#ifdef DEBUG
  CHECK_CTYPE;
#endif
  return( argc );
}

static void pip_glibc_fin( pip_symbols_t *symbols ) {
  /* call fflush() in the target context to flush out messages */
  DBGF( "[%d] >> fflush@%p()", pipid, symbols->libc_fflush );
  symbols->libc_fflush( NULL );
  DBGF( "[%d] << fflush@%p()", pipid, symbols->libc_fflush );
}

static int pip_do_spawn( void *thargs )  {
  pip_spawn_args_t *args = (pip_spawn_args_t*) thargs;
  int 	pipid      = args->pipid;
#ifdef PIP_CLONE_AND_DLMOPEN
  char *prog       = args->prog;
#endif
  char **argv      = args->argv;
  char **envv      = args->envv;
  int coreno       = args->coreno;
  int argc;
  pip_spawnhook_t before = args->hook_before;
  pip_spawnhook_t after  = args->hook_after;
  void	*hook_arg  = args->hook_arg;
  pip_task_t *self = &pip_root->tasks[pipid];
  int err = 0;

  DBG;
  if( ( err = pip_corebind( coreno ) ) != 0 ) RETURN( err );

#ifdef DEBUG
  if( pip_if_pthread_() ) {
    pthread_attr_t attr;
    size_t sz;
    int _err;
    if( ( _err = pthread_getattr_np( self->thread, &attr      ) ) != 0 ) {
      DBGF( "pthread_getattr_np()=%d", _err );
    } else if( ( _err = pthread_attr_getstacksize( &attr, &sz ) ) != 0 ) {
      DBGF( "pthread_attr_getstacksize()=%d", _err );
    } else {
      DBGF( "stacksize = %ld [KiB]", sz/1024 );
    }
  }
#endif

  if( !pip_if_shared_fd_() ) pip_close_on_exec();

#ifdef PIP_CLONE_AND_DLMOPEN
  ES( time_load_prog, ( err = pip_load_prog( prog, self ) ) );
  if( err != 0 ) RETURN( err );
#endif

  /* calling hook, if any */
  if( before == NULL || ( err = before( hook_arg ) ) == 0 ) {
    /* argv and/or envv might be changed in the hook function */
    ucontext_t 		ctx;
    volatile int	flag_exit;	/* must be volatile */

    argc = pip_glibc_init( &self->symbols, argv, envv );

    flag_exit = 0;
    (void) getcontext( &ctx );
    if( !flag_exit ) {
      flag_exit = 1;
      self->ctx_exit = &ctx;
#ifdef DEBUG
      pip_print_maps();
#endif

      DBGF( "[%d] >> main@%p(%d,%s,%s,...)",
	    pipid, self->symbols.main, argc, argv[0], argv[1] );
      self->retval = self->symbols.main( argc, argv, envv );
      DBGF( "[%d] << main@%p(%d,%s,%s,...)",
	    pipid, self->symbols.main, argc, argv[0], argv[1] );

    } else {
      DBGF( "[%d] !! main(%d,%s,%s,...)", pipid, argc, argv[0], argv[1] );
    }
    if( after != NULL ) (void) after( hook_arg );
    DBG;
    pip_glibc_fin( &self->symbols );

  } else if( err != 0 ) {
    pip_warn_mesg( "try to spawn(%s), but the before hook at %p returns %d",
		   argv[0], before, err );
    self->retval = err;
  }
  DBG;
  if( pip_root->opts & PIP_OPT_FORCEEXIT ) {
    if( pip_if_pthread_() ) {	/* thread mode */
      pthread_exit( NULL );
    } else {			/* process mode */
      exit( self->retval );
    }
  }
  DBG;
  RETURN( 0 );
}

int pip_spawn( char *prog,
	       char **argv,
	       char **envv,
	       int  coreno,
	       int  *pipidp,
	       pip_spawnhook_t before,
	       pip_spawnhook_t after,
	       void *hookarg ) {
  extern char 		**environ;
  pthread_attr_t 	attr;
  cpu_set_t 		cpuset;
  size_t		stack_size = pip_stack_size();
  pip_spawn_args_t	*args = NULL;
  pip_task_t		*task = NULL;
  int 			pipid;
  pid_t			pid = 0;
  int 			err = 0;

  DBGF( ">> pip_spawn()" );

  if( pip_root == NULL ) RETURN( EPERM );
  if( prog == NULL ) {
    if( argv == NULL )   RETURN( EINVAL );
    prog = argv[0];
  }
  if( pipidp   == NULL ) RETURN( EINVAL );
  pipid = *pipidp;
  if( pipid < PIP_PIPID_ANY || pipid >= pip_root->ntasks ) {
    DBGF( "pipid=%d", pipid );
    RETURN( EINVAL );
  }

  DBGF( "pip_spawn(pipid=%d)", *pipidp );

  if( pip_root->ntasks_accum >= PIP_NTASKS_MAX ) RETURN( EOVERFLOW );
  if( ( err = pthread_attr_init( &attr ) ) != 0 ) RETURN( err );

#ifdef PIP_CLONE_AND_DLMOPEN
  if( coreno != PIP_CPUCORE_ASIS ) {
    CPU_ZERO( &cpuset );
    CPU_SET( coreno, &cpuset );
    err = pthread_attr_setaffinity_np( &attr, sizeof(cpuset), &cpuset );
    if( err != 0 ) RETURN( err );
  }
#endif

  err = pthread_attr_setstacksize( &attr, stack_size );
  DBGF( "pthread_attr_setstacksize( %ld )= %d", stack_size, err );
  if( err ) RETURN( err );

  args = (pip_spawn_args_t*) malloc( sizeof(pip_spawn_args_t) );
  if( args == NULL ) RETURN( ENOMEM );

  if( ( args->prog = strdup( prog ) ) == NULL ) RETURN( ENOMEM );
  args->coreno      = coreno;
  args->hook_before = before;
  args->hook_after  = after;
  args->hook_arg    = hookarg;
  if( ( args->argv = pip_copy_vec( NULL, argv ) ) == NULL ) {
    PIP_FREE( args->prog );
    PIP_FREE( args );
    RETURN( ENOMEM );
  }
  if( envv == NULL ) envv = environ;
  if( ( args->envv = pip_copy_env( envv ) ) == NULL ) {
    PIP_FREE( args->prog );
    PIP_FREE( args->argv );
    PIP_FREE( args );
    RETURN( ENOMEM );
  }

  pip_spin_lock( &pip_root->lock_ldlinux );
  /*** begin lock region ***/
  do {
    if( pipid != PIP_PIPID_ANY ) {
      if( pip_root->tasks[pipid].pipid != PIP_PIPID_NONE ) {
	DBG;
	err = EAGAIN;
	goto unlock;
      }
    } else {
      int i;

      for( i=pip_root->pipid_curr; i<pip_root->ntasks; i++ ) {
	if( pip_root->tasks[i].pipid == PIP_PIPID_NONE ) {
	  pipid = i;
	  goto found;
	}
      }
      for( i=0; i<pip_root->pipid_curr; i++ ) {
	if( pip_root->tasks[i].pipid == PIP_PIPID_NONE ) {
	  pipid = i;
	  goto found;
	}
      }
      err = EAGAIN;
      goto unlock;
    }
  found:
    pip_root->pipid_curr = pipid + 1;
    task = &pip_root->tasks[pipid];
    pip_init_task_struct( task );
    task->pipid = pipid;	/* mark it as occupied */
    args->pipid = pipid;

#ifdef PIP_DLMOPEN_AND_CLONE
    if( err == 0 ) {
      if( ( err = pip_do_corebind( coreno, &cpuset ) ) == 0 ) {
	/* corebinding should take place before loading solibs,        */
	/* hoping anon maps would be mapped ontto the closer numa node */

	ES( time_load_prog, ( err = pip_load_prog( prog, task ) ) );

	/* and of course, the corebinding must be undone */
	(void) pip_undo_corebind( coreno, &cpuset );
      }
    }
#endif
  } while( 0 );
  /*** end lock region ***/
 unlock:
  pip_spin_unlock( &pip_root->lock_ldlinux );

  /**** beyond this point, we can 'goto' the 'error' label ****/

  if( err != 0 ) goto error;

  if( ( pip_root->opts & PIP_MODE_PROCESS_PIPCLONE ) ==
      PIP_MODE_PROCESS_PIPCLONE ) {
    int flags =
      CLONE_VM |
      /* CLONE_FS | CLONE_FILES | */
      /* CLONE_SIGHAND | CLONE_THREAD | */
      CLONE_SETTLS |
      CLONE_PARENT_SETTID |
      CLONE_CHILD_CLEARTID |
      CLONE_SYSVSEM |
      CLONE_PTRACE |
      SIGCHLD;

    err = pip_clone_mostly_pthread_ptr( &task->thread,
					flags,
					coreno,
					stack_size,
					(void*(*)(void*)) pip_do_spawn,
					args,
					&pid );
  } else {
    pid_t tid = pip_gettid();
    task->args = args;
    if( pip_root->cloneinfo != NULL ) {
      /* lock is needed, because the preloaded clone()
       might also be called from outside of PiP lib. */
      pip_spin_lock_wv( &pip_root->cloneinfo->lock, tid );
    }
    do {
      err = pthread_create( &task->thread,
			    &attr,
			    (void*(*)(void*)) pip_do_spawn,
			    (void*) args );
    } while( 0 );
    if( pip_root->cloneinfo != NULL ) {
      pip_spin_unlock( &pip_root->cloneinfo->lock );
      pid = pip_root->cloneinfo->pid_clone;
    }
  }

  DBG;
  if( err == 0 ) {
    task->pid = pid;
    pip_root->ntasks_accum ++;
    pip_root->ntasks_curr  ++;
    if( pip_root->cloneinfo != NULL ) pip_root->cloneinfo->pid_clone = 0;
    *pipidp = pipid;

  } else {
  error:			/* undo */
    DBGF( "err=%d", err );
    PIP_FREE( args->prog );
    PIP_FREE( args->argv );
    PIP_FREE( args->envv );
    PIP_FREE( args );
    if( task != NULL ) pip_init_task_struct( task );
  }
  DBGF( "<< pip_spawn(pipid=%d)", *pipidp );
  RETURN( err );
}

int pip_fin( void ) {
  int ntasks;
  int i;
  int err = 0;

  fflush( NULL );
  if( pip_root_p_() ) {
    ntasks = pip_root->ntasks;
    for( i=0; i<ntasks; i++ ) {
      if( pip_root->tasks[i].pipid != PIP_PIPID_NONE ) {
	DBGF( "%d/%d [%d] -- BUSY", i, ntasks, pip_root->tasks[i].pipid );
	err = EBUSY;
	break;
      }
    }
    if( err == 0 ) {
      memset( pip_root, 0, pip_root->size );
      free( pip_root );
    }
  }
  pip_root = NULL;

  REPORT( time_load_dso   );
  REPORT( time_load_prog );
  REPORT( time_dlmopen   );

  RETURN( err );
}

int pip_get_mode( int *mode ) {
  if( pip_root == NULL ) RETURN( EPERM  );
  if( mode     == NULL ) RETURN( EINVAL );
  *mode = ( pip_root->opts & PIP_MODE_MASK );
  RETURN( 0 );
}

int pip_get_id( int pipid, intptr_t *pidp ) {
  pip_task_t *task:
  int err;

  if( ( err = pip_check_pipid( &pipid ) ) != 0 ) RETURN( err );
  if( pidp == NULL ) RETURN( EINVAL );

  task = pip_get_task( pipid );
  if( pip_if_pthread_() ) {
    /* Do not use gettid(). This is a very Linux-specific function */
    /* The reason of supporintg the thread PiP execution mode is   */
    /* some OSes other than Linux does not support clone()         */
    *pidp = (intptr_t) task->thread;
  } else {
    *pidp = (intptr_t) task->pid;
  }
  RETURN( 0 );
}

int pip_kill( int pipid, int signal ) {
  pip_task_t *task:
  int err  = 0;

  if( ( err = pip_check_pipid( &pipid ) ) != 0 ) RETURN( err );
  if( signal < 0 ) RETURN( EINVAL );

  task = pip_get_task( pipid );
  if( pip_if_pthread_() ) {
    err = pthread_kill( task->thread, signal );
    DBGF( "pthread_kill(sig=%d)=%d", signal, err );
  } else {
    if( kill( task->pid, signal ) < 0 ) err = errno;
    DBGF( "kill(sig=%d)=%d", task->pid, signal, err );
  }
  RETURN( err );
}

int pip_exit( int retval ) {
  fflush( NULL );
  if( !pip_root_p_() && !pip_task_p_() ) {
    /* since we must replace exit() with pip_exit(), pip_exit() */
    /* must be able to use even if it is NOT a PIP environment. */
    exit( retval );
  } else if( pip_if_pthread_() ) {	/* thread mode */
    pip_task->retval = retval;
    DBGF( "[PIPID=%d] pip_exit(%d)!!!", pip_task->pipid, retval );
    (void) setcontext( pip_task->ctx_exit );
    DBGF( "[PIPID=%d] pip_exit() ????", pip_task->pipid );
  } else {				/* process mode */
    exit( retval );
  }
  /* never reach here */
  return 0;
}

/*
 * The following functions must be called at root process
 */

static void pip_finalize_task( pip_task_t *task, int *retvalp ) {
  DBGF( "pipid=%d", task->pipid );

  /* dlclose() must only be called from the root process since */
  /* corresponding dlopen() calls are made by the root process */
  DBGF( "retval=%d", task->retval );

  if( retvalp      != NULL ) *retvalp = task->retval;
  if( task->loaded != NULL ) pip_dlclose( task->loaded );

  if( task->args != NULL ) {
    if( task->args->prog != NULL ) PIP_FREE( task->args->prog );
    if( task->args->argv != NULL ) PIP_FREE( task->args->argv );
    if( task->args->envv != NULL ) PIP_FREE( task->args->envv );
    PIP_FREE( task->args );
  }
  pip_init_task_struct( task );
  pip_root->ntasks_curr --;
}

#ifdef PIP_PTHREAD
static int pip_check_join_arg( int pipid, pip_task_t **taskp ) {
  int err = pip_check_pipid( &pipid );

  if( err == 0 ) {
    if( pipid     == PIP_PIPID_ROOT ) RETURN( EINVAL );
    if( pip_task != NULL            ) RETURN( EPERM );
    if( !pthread_equal( pthread_self(), pip_root->task_root->thread ) ) {
      RETURN( EPERM );
    }
    *taskp = &pip_root->tasks[pipid];
    if( *taskp == pip_task ) RETURN( EPERM );
  }
  RETURN( err );
}

int pip_get_thread( int pipid, pthread_t *threadp ) {
  pip_task_t *task;
  int err;

  if( ( err = pip_check_join_arg( pipid, &task ) ) == 0 ) {
    if( !pip_if_pthread_() ) RETURN( ENOSYS );
    if( threadp != NULL ) *threadp = task->thread;
  }
  RETURN( err );
}

int pip_join( int pipid ) {
  pip_task_t *task;
  int err;

  if( ( err = pip_check_join_arg( pipid, &task ) ) == 0 ) {
    if( !pip_if_pthread_() ) RETURN( ENOSYS );
    err = pthread_join( task->thread, NULL );
    if( !err ) pip_finalize_task( task, NULL );
  }
  RETURN( err );
}

int pip_tryjoin( int pipid ) {
  pip_task_t *task;
  int err;

  if( ( err = pip_check_join_arg( pipid, &task ) ) == 0 ) {
    if( !pip_if_pthread_() ) RETURN( ENOSYS );
    err = pthread_tryjoin_np( task->thread, NULL );
    if( !err ) pip_finalize_task( task, NULL );
  }
  RETURN( err );
}

int pip_timedjoin( int pipid, struct timespec *time ) {
  pip_task_t *task;
  int err;

  if( ( err = pip_check_join_arg( pipid, &task ) ) == 0 ) {
    if( !pip_if_pthread_() ) RETURN( ENOSYS );
    err = pthread_timedjoin_np( task->thread, NULL, time );
    if( !err ) pip_finalize_task( task, NULL );
  }
  RETURN( err );
}
#endif /* PIP_PTHREAD */

static int pip_do_wait( int pipid, int flag_try, int *retvalp ) {
  pip_task_t *task;
  int err;

  if( ( err = pip_check_pipid( &pipid ) ) != 0 ) RETURN( err );
  if( pipid == PIP_PIPID_ROOT  ) RETURN( EINVAL );
  task = &pip_root->tasks[pipid];
  if( task == pip_task ) RETURN( EPERM );

  if( pip_if_pthread_() ) { /* thread mode */
    if( flag_try ) {
      err = pthread_tryjoin_np( task->thread, NULL );
      DBGF( "pthread_tryjoin_np()=%d", err );
    } else {
      err = pthread_join( task->thread, NULL );
      DBGF( "pthread_join()=%d", err );
    }
  } else {			/* process mode */
    int status = 0;
    pid_t pid;
    int options = __WALL;
    DBG;
    if( flag_try ) options |= WNOHANG;
    while( 1 ) {
      if( ( pid = waitpid( task->pid, &status, options ) ) >= 0 ) break;
      if( errno != EINTR ) {
	err = errno;
	break;
      }
    }
    if( WIFEXITED( status ) ) {
      task->retval = WEXITSTATUS( status );
    } else if( WIFSIGNALED( status ) ) {
      pip_warn_mesg( "Signaled %s", strsignal( WTERMSIG( status ) ) );
    }
    DBGF( "wait(status=%x)=%d (errno=%d)", status, pid, err );
  }
  if( !err ) pip_finalize_task( task, retvalp );
  RETURN( err );
}

int pip_wait( int pipid, int *retvalp ) {
  RETURN( pip_do_wait( pipid, 0, retvalp );
}

int pip_trywait( int pipid, int *retvalp ) {
  RETURN( pip_do_wait( pipid, 1, retvalp );
}

pip_clone_t *pip_get_cloneinfo_( void ) {
  return pip_root->cloneinfo;
}

int pip_get_pid( int pipid, pid_t *pidp ) {
  int err = 0;

  if( pidp == NULL ) RETURN( EINVAL );
  if( pip_root->opts && PIP_MODE_PROCESS ) {
    /* only valid with the "process" execution mode */
    if( ( err = pip_check_pipid( &pipid ) ) == 0 ) {
      if( pipid == PIP_PIPID_ROOT ) {
	err = EPERM;
      } else {
	*pidp = pip_root->tasks[pipid].pid;
      }
    }
  } else {
    err = EPERM;
  }
  RETURN( err );
}

/*** The following malloc/free functions are just for functional test    ***/
/*** We should hvae the other functions allocating memory doing the same ***/

/* long long to align */
#define PIP_ALIGN_TYPE	long long

void *pip_malloc( size_t size ) {
  void *p = malloc( size + sizeof(PIP_ALIGN_TYPE) );
  int pipid;
  if( pip_get_pipid( &pipid ) == 0 ) {
    *(int*) p = pipid;
    p += sizeof(PIP_ALIGN_TYPE);
  } else {
    free( p );
    p = NULL;
  }
  return p;
}

void pip_free( void *ptr ) {
  pip_task_t *task;
  free_t free_func;
  int pipid;

  ptr  -= sizeof(PIP_ALIGN_TYPE);
  pipid = *(int*) ptr;
  task  = &pip_root->tasks[pipid];
  /* need of sanity check on pipid */
  if( ( free_func = task->symbols.free ) == NULL ) {
    pip_warn_mesg( "No free function" );
  } else {
    free_func( ptr );
  }
}

/* the following function(s) are for debugging */

void pip_print_loaded_solibs( FILE *file ) {
  struct link_map *map;
  void *handle = NULL;
  char idstr[PIPIDLEN];
  char *fname;

  /* pip_init() must be called in advance */
  (void) pip_idstr( idstr, PIPIDLEN );
  if( handle == NULL ) {
    fprintf( file, "%s (no solibs loaded)\n", idstr );
  } else {
    if( file == NULL ) file = stderr;
    map = (struct link_map*) handle;
    for( ; map!=NULL; map=map->l_next ) {
      if( *map->l_name == '\0' ) {
	fname = "(noname)";
      } else {
	fname = map->l_name;
      }
      fprintf( file, "%s %s at %p\n", idstr, fname, (void*)map->l_addr );
    }
  }
}

/*-----------------------------------------------------*/
/* ULP ULP ULP ULP ULP ULP ULP ULP ULP ULP ULP ULP ULP */
/*-----------------------------------------------------*/

static void pip_ulp_recycle_stack( pip_task_t *task, void *stack ) {
  /* the first page is protected as stack guard */
  *((void**)(stack+pip_root->page_size)) = pip_task->ulp_stack_list;
  pip_task->ulp_stack_list = stack;
}

static void *pip_ulp_reuse_stack( pip_task_t *task ) {
  void *stack = NULL;
  if( pip_task->ulp_stack_list != NULL ) {
    stack = pip_task->ulp_stack_list;
    pip_task->ulp_stack_list = *((void**)(stack+pip_root->page_size));
  }
  return stack;
}

static int pip_ulp_alloc_stack( pip_task_t *task, PIP_ULP_t *ulp ) {
  size_t	stksz = pip_stack_size();
  size_t	pgsz  = pip_root->page_size;
  void 		*region;
  int		err;

  if( ( region = pip_ulp_reuse_stack( task ) ) == NULL ) {
    /* guard pages, top and bottom, to be idependent from stack direction */
    if( ( err = pip_page_alloc( stksz + pgsz + pgsz, &region ) ) != 0 ) {
      RETURN( err );
    }
    if( mprotect( region,            pgsz, PROT_NONE ) != 0 ||
	mprotect( region+pgsz+stksz, pgsz, PROT_NONE ) != 0 ) {
      RETURN( errno );
    }
  }
  ulp->stack_sz     = stksz + pgsz + pgsz;
  ulp->stack_region = region;
  ulp->ctx_ulp.uc_stack.ss_sp   = region + pgsz;
  ulp->ctx_ulp.uc_stack.ss_size = stksz;

  RETURN( 0 );
}

static void pip_ulp_main_( int high, int low ) {
  ucontext_t	ctx;
  PIP_ULP_t	*ulp;
  volatile int 	flag;
  int 		argc;

  ulp = (PIP_ULP_t*) ( ( ((intptr_t)high) << 32 ) | ((intptr_t)low) );
  DBGF( "ulp=%p", ulp );

  for( argc=0; ulp->argv[argc]!=NULL; argc++ );
  *ulp->symbols.environ = ulp->envv;	/* setting environment vars */

  flag = 0;
  (void) getcontext( &ctx );
  if( !flag ) {
    flag = 1;
    ulp->ctx_exit = &ctx;

    DBGF( "[ULP] >> main@%p(%d,%s,%s,...)",
	  ulp->symbols.main, argc, ulp->argv[0], ulp->argv[1] );
    ulp->retval = ulp->symbols.main( argc, ulp->argv, ulp->envv );
    DBGF( "[ULP] << main@%p(%d,%s,%s,...)",
	  ulp->symbols.main, argc, ulp->argv[0], ulp->argv[1] );
  }
  if( ulp->exit_hook != NULL ) ulp->exit_hook( ulp->aux, ulp->retval );
}

static int pip_ulp_swapcontext( PIP_ULP_t *new ) {
  ucontext_t	ctx;
  PIP_ULP_t	*curr = pip_task->ulp_curr;
  int 		err = 0;

  pip_task->ulp_curr = new;
  if( ( err = swapcontext( &curr->ctx_ulp, &new->ctx_ulp ) != 0 ) ) {
    err = -err;
  }
  RETURN( err );
}

static int pip_ulp_makecontext( PIP_ULP_t *ulp ) {
  volatile int flag = 0;
  int	high, low;
  int	err = 0;

  high = ( ((intptr_t)ulp) >> 32 ) & PIP_MASK32;
  low  =   ((intptr_t)ulp)         & PIP_MASK32;

  makecontext( &ulp->ctx_ulp, (void(*)(void)) pip_ulp_main_, 2, high, low );
  if( !flag ) {
    flag = 1;
    pip_task->ulp_curr = ulp;
    setcontext( &ulp->ctx_ulp );
  }
  RETURN( err );
}

int pip_ulp_spawn( pip_ulp_t *ulpp,
		   char *prog,
		   char **argv,
		   char **envv,
		   pip_ulp_exithook_t exit_hook,
		   void *aux ) {
  extern char 	**environ;
  PIP_ULP_t	*ulp;
  void 		*loaded;
  int 		err = 0;

  DBGF( ">> pip_ulp_spawn()" );

  if( pip_root_p_() ) RETURN( EPERM );
  if( prog == NULL ) {
    if( argv == NULL ) RETURN( EINVAL );
    prog = argv[0];
  }
  if( ulpp == NULL ) RETURN( EINVAL );
  memset( ulpp, 0, sizeof(PIP_ULP_t) );
  ulp = (PIP_ULP_t*)((void*)ulpp);

  if( ( envv  = pip_copy_env( envv       ) ) == NULL ) RETURN( ENOMEM );
  if( ( argv  = pip_copy_vec( NULL, argv ) ) == NULL ) {
    free( envv );
    RETURN( ENOMEM );
  }
  if( ( err = pip_ulp_alloc_stack( pip_task, ulp ) ) != 0 ) {
    free( envv );
    free( argv );
    RETURN( err );
  }

  ES( time_load_dso, ( err = pip_load_dso( &loaded, prog ) ) );
  if( err != 0 ) goto error;

  if( ( err = pip_find_symbols( loaded, &ulp->symbols ) ) == 0 ) {
    ulp->ulp_root  = pip_task;
    ulp->loaded    = loaded;
    ulp->prog      = strdup( prog );
    ulp->argv      = argv;
    ulp->envv      = envv;
    ulp->exit_hook = exit_hook;
    if( ( err = pip_ulp_makecontext( ulp ) ) == 0 ) goto done;
  }
 error:
  dlclose( ulp->loaded );
  free( ulp->stack_region );
  if( ulp->prog != NULL ) free( ulp->prog );
  free( argv );
  free( envv );

 done:
  DBGF( "<< pip_ulp_spawn()=%d", err );
  RETURN( err );
}

int pip_ulp_yield_to( PIP_ULP_t *new ) {
  int err;

  if( pip_root_p_() ) {
    err = EPERM;
  } else {
    err = pip_ulp_swapcontext( new );
  }
  RETURN( err );
}

int pip_ulp_exit( int retval ) {
  PIP_ULP_t *ulp;
  int err = 0;

  if( pip_root_p_() ) {
    err = EPERM;
  } else {
    ulp = pip_task->ulp_curr;
    ulp->retval = retval;
    setcontext( ulp->ctx_exit );
    /* never reach here */
  }
  RETURN( err );
}

int pip_ulp_recycle( pip_ulp_t *ulpp ) {
  return 0;
}
