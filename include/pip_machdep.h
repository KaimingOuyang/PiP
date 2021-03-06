/*
  * $RIKEN_copyright: 2018 Riken Center for Computational Sceience, 
  * 	  System Software Devlopment Team. All rights researved$
  * $PIP_VERSION: Version 1.0$
  * $PIP_license: <Simplified BSD License>
  * Redistribution and use in source and binary forms, with or without
  * modification, are permitted provided that the following conditions are
  * met:
  * 
  * 1. Redistributions of source code must retain the above copyright
  *    notice, this list of conditions and the following disclaimer.
  * 2. Redistributions in binary form must reproduce the above copyright
  *    notice, this list of conditions and the following disclaimer in the 
  *    documentation and/or other materials provided with the distribution.
  * 
  * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
  * 
  * The views and conclusions contained in the software and documentation
  * are those of the authors and should not be interpreted as representing
  * official policies, either expressed or implied, of the PiP project.$
*/
/*
  * Written by Atsushi HORI <ahori@riken.jp>, 2016-2017
*/

#ifndef _pip_machdep_h_
#define _pip_machdep_h_

#ifndef DOXYGEN_SHOULD_SKIP_THIS

#if defined(__x86_64__)
#include <pip_machdep_x86_64.h>
#elif defined(__aarch64__)
#include <pip_machdep_aarch64.h>
#else
#ifdef AHA
#error "Unsupported Machine Type"
#endif
#endif

#ifndef PIP_CACHE_SZ
#define PIP_CACHE_SZ		(64)
#endif

#ifndef PIP_LOCK_TYPE
#include <stdint.h>
typedef volatile uint32_t	pip_spinlock_t;
#endif

#ifndef PIP_PAUSE
inline static void pip_pause( void ) {
  /* nop */
}
#endif

#ifndef PIP_WRITE_BARRIER
inline static void pip_write_barrier(void) {
  __sync_synchronize ();
}
#endif

#ifndef PIP_MEMORY_BARRIER
inline static void pip_memory_barrier(void) {
  __sync_synchronize ();
}
#endif

#ifndef PIP_SPIN_TRYLOCK_WV
inline static pip_spinlock_t
pip_spin_trylock_wv( pip_spinlock_t *lock, pip_spinlock_t lv ) {
  pip_memory_barrier();
  return __sync_val_compare_and_swap( lock, 0, lv );
}
#endif

#ifndef PIP_SPIN_LOCK_WV
inline static void
pip_spin_lock_wv( pip_spinlock_t *lock, pip_spinlock_t lv ) {
  while( pip_spin_trylock_wv( lock, lv ) != 0 ) pip_pause();
}
#endif

#ifndef PIP_SPIN_TRYLOCK
inline static pip_spinlock_t
pip_spin_trylock( pip_spinlock_t *lock ) {
  int oldval;

  pip_memory_barrier();
  if( *lock != 0 ) return 0;
  oldval = __sync_val_compare_and_swap( lock, 0, 1 );
  return oldval == 0;
}
#endif

#ifndef PIP_SPIN_LOCK
inline static void
pip_spin_lock( pip_spinlock_t *lock ) {
  while( !pip_spin_trylock( lock ) ) pip_pause();
}
#endif

#ifndef PIP_SPIN_UNLOCK
inline static void pip_spin_unlock (pip_spinlock_t *lock) {
  __sync_lock_release(lock);
}
#endif

#ifndef PIP_SPIN_INIT
inline static int pip_spin_init (pip_spinlock_t *lock) {
  pip_spin_unlock( lock );
  return 0;
}
#endif

#ifndef PIP_SPIN_DESTROY
inline static int pip_spin_destroy (pip_spinlock_t *lock) {
  /* Nothing to do.  */
  return 0;
}
#endif

#ifndef PIP_PRINT_FSREG
inline static void pip_print_fs_segreg( void ) {}
#endif

#endif

#endif
