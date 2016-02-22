#include <types.h>
#include <lib.h>
#include <synchprobs.h>
#include <synch.h>
#include <opt-A1.h>

/*
 this implementation is a hack and heavily depends on the values of Direction
 however, if the Direction enum values do change, we can insert a conversion
 before enter criticl sections, which brings no performance hit nayway
*/

//#define FAIR 2

#ifdef FAIR
// fairness related definitions
static int waiting[4] = {0};
static inline bool
is_fair(Direction origin)
{
  return ((waiting[origin] >= waiting[0]) + (waiting[origin] >= waiting[1]) + (waiting[origin] >= waiting[2]) + (waiting[origin] >= waiting[3]) > FAIR);
}
#endif

// efficiency related definitions
static struct lock *intersectionLock;
static struct cv *intersectionCv;
static volatile int intersectionCount[4][4] = {{0}};  // tracking # of cars already in intersection by origin, destination


// hacks to spare effort to write conditions
static int right[4] = {west, north, east, south};
static int opposite[4] = {south, west, north, east};

// should I make an inlined implementation instead?
// update: inline implementation does not provide performance boost, but deteriorates fairness
static bool
may_crash(Direction origin, Direction destination)
{
  Direction originRight = right[origin];
  Direction destRight = right[destination];
  Direction originOpposite = opposite[origin];
  Direction destOppo = opposite[destination];
  if (destination == originRight)
  {
    return intersectionCount[originOpposite][destination] || intersectionCount[destOppo][destination] ; 
  } 
  else if (destination == originOpposite) 
  {
    return intersectionCount[destRight][originRight] || intersectionCount[destRight][destination] || 
      intersectionCount[destination][originRight] ||
      intersectionCount[originRight][origin] || intersectionCount[originRight][destination] || intersectionCount[originRight][destRight]; 
  }
  else
  {
    return intersectionCount[destination][originOpposite] || intersectionCount[destination][destOppo] ||
      intersectionCount[originOpposite][origin] || intersectionCount[originOpposite][destination] || intersectionCount[originOpposite][destOppo] ||
      intersectionCount[destOppo][origin] || intersectionCount[destOppo][destination];
  }
}

/*
 * The simulation driver will call this function once before starting
 * the simulation
 *
 * You can use it to initialize synchronization and other variables.
 * 
 */
void
intersection_sync_init(void)
{
  /* replace this default implementation with your own implementation */

  intersectionLock = lock_create("intersectionLock");
  if (intersectionLock == NULL) {
    panic("could not create intersection lock");
  }
  intersectionCv = cv_create("intersectionLock");
  if (intersectionCv == NULL) {
    panic("could not create intersection condition variable");
  }
  return;
}

/* 
 * The simulation driver will call this function once after
 * the simulation has finished
 *
 * You can use it to clean up any synchronization and other variables.
 *
 */
void
intersection_sync_cleanup(void)
{
  KASSERT(intersectionLock != NULL);
  lock_destroy(intersectionLock);
  KASSERT(intersectionCv != NULL);
  cv_destroy(intersectionCv);
}


/*
 * The simulation driver will call this function each time a vehicle
 * tries to enter the intersection, before it enters.
 * This function should cause the calling simulation thread 
 * to block until it is OK for the vehicle to enter the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle is arriving
 *    * destination: the Direction in which the vehicle is trying to go
 *
 * return value: none
 */

void
intersection_before_entry(Direction origin, Direction destination) 
{
  KASSERT(intersectionLock != NULL);
  KASSERT(intersectionCv != NULL);
  

  //critical section
  lock_acquire(intersectionLock);
#ifdef FAIR
  waiting[origin] ++;
#endif
  while (
#ifdef FAIR
    is_fair(origin) && 
#endif
    may_crash(origin, destination)) 
  {
    cv_wait(intersectionCv, intersectionLock);
  } 
  intersectionCount[origin][destination] ++;
#ifdef FAIR
  waiting[origin] --;
#endif
  lock_release(intersectionLock);
  // end of critical section
}


/*
 * The simulation driver will call this function each time a vehicle
 * leaves the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle arrived
 *    * destination: the Direction in which the vehicle is going
 *
 * return value: none
 */

void
intersection_after_exit(Direction origin, Direction destination) 
{
  KASSERT(intersectionLock != NULL);
  KASSERT(intersectionCv != NULL);

  lock_acquire(intersectionLock);
  intersectionCount[origin][destination] --;
  cv_broadcast(intersectionCv, intersectionLock);
  lock_release(intersectionLock);

}

