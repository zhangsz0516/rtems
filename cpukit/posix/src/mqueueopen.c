/**
 * @file
 *
 * @brief Creates a new POSIX Message Queue or Opens an Existing Queue 
 * @ingroup POSIXAPI
 */

/*
 *  NOTE:  The structure of the routines is identical to that of POSIX
 *         Message_queues to leave the option of having unnamed message
 *         queues at a future date.  They are currently not part of the
 *         POSIX standard but unnamed message_queues are.  This is also
 *         the reason for the apparently unnecessary tracking of
 *         the process_shared attribute.  [In addition to the fact that
 *         it would be trivial to add pshared to the mq_attr structure
 *         and have process private message queues.]
 *
 *         This code ignores the O_RDONLY/O_WRONLY/O_RDWR flag at open
 *         time.
 *
 *  COPYRIGHT (c) 1989-2009.
 *  On-Line Applications Research Corporation (OAR).
 *
 *  The license and distribution terms for this file may be
 *  found in the file LICENSE in this distribution or at
 *  http://www.rtems.org/license/LICENSE.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdarg.h>

#include <pthread.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>

#include <rtems/score/todimpl.h>
#include <rtems/score/watchdog.h>
#include <rtems/posix/mqueueimpl.h>
#include <rtems/seterr.h>

#define MQ_OPEN_FAILED ((mqd_t) -1)

static int _POSIX_Message_queue_Create_support(
  const char                    *name_arg,
  size_t                         name_len,
  struct mq_attr                *attr_ptr,
  POSIX_Message_queue_Control  **message_queue
)
{
  POSIX_Message_queue_Control   *the_mq;
  struct mq_attr                 attr;
  char                          *name;

  /* length of name has already been validated */

  /*
   *  There is no real basis for the default values.  They will work
   *  but were not compared against any existing implementation for
   *  compatibility.  See README.mqueue for an example program we
   *  think will print out the defaults.  Report anything you find with it.
   */
  if ( attr_ptr == NULL ) {
    attr.mq_maxmsg  = 10;
    attr.mq_msgsize = 16;
  } else {
    if ( attr_ptr->mq_maxmsg <= 0 ){
      rtems_set_errno_and_return_minus_one( EINVAL );
    }

    if ( attr_ptr->mq_msgsize <= 0 ){
      rtems_set_errno_and_return_minus_one( EINVAL );
    }

    attr = *attr_ptr;
  }

  the_mq = _POSIX_Message_queue_Allocate();
  if ( !the_mq ) {
    _Objects_Allocator_unlock();
    rtems_set_errno_and_return_minus_one( ENFILE );
  }

  /*
   * Make a copy of the user's string for name just in case it was
   * dynamically constructed.
   */
  name = _Workspace_String_duplicate( name_arg, name_len );
  if ( !name ) {
    _POSIX_Message_queue_Free( the_mq );
    _Objects_Allocator_unlock();
    rtems_set_errno_and_return_minus_one( ENOMEM );
  }

  the_mq->open_count = 1;
  the_mq->linked = true;

  /*
   *  NOTE: That thread blocking discipline should be based on the
   *  current scheduling policy.
   *
   *  Joel: Cite POSIX or OpenGroup on above statement so we can determine
   *        if it is a real requirement.
   */
  if ( !_CORE_message_queue_Initialize(
           &the_mq->Message_queue,
           CORE_MESSAGE_QUEUE_DISCIPLINES_FIFO,
           attr.mq_maxmsg,
           attr.mq_msgsize
      ) ) {

    _POSIX_Message_queue_Free( the_mq );
    _Workspace_Free(name);
    _Objects_Allocator_unlock();
    rtems_set_errno_and_return_minus_one( ENOSPC );
  }

  _Objects_Open_string(
    &_POSIX_Message_queue_Information,
    &the_mq->Object,
    name
  );

  *message_queue = the_mq;

  _Objects_Allocator_unlock();
  return 0;
}

/*
 *  15.2.2 Open a Message Queue, P1003.1b-1993, p. 272
 */
mqd_t mq_open(
  const char *name,
  int         oflag,
  ...
  /* mode_t mode, */
  /* struct mq_attr  attr */
)
{
  /*
   * mode is set but never used. GCC gives a warning for this
   * and we need to tell GCC not to complain. But we have to
   * have it because we have to work through the variable
   * arguments to get to attr.
   */
  mode_t                          mode RTEMS_UNUSED;

  va_list                         arg;
  struct mq_attr                 *attr = NULL;
  int                             status;
  POSIX_Message_queue_Control    *the_mq;
  POSIX_Message_queue_Control_fd *the_mq_fd;
  size_t                          name_len;
  Objects_Get_by_name_error       error;

  if ( oflag & O_CREAT ) {
    va_start(arg, oflag);
    mode = va_arg( arg, mode_t );
    attr = va_arg( arg, struct mq_attr * );
    va_end(arg);
  }

  the_mq_fd = _POSIX_Message_queue_Allocate_fd();
  if ( !the_mq_fd ) {
    _Objects_Allocator_unlock();
    rtems_set_errno_and_return_value( ENFILE, MQ_OPEN_FAILED );
  }
  the_mq_fd->oflag = oflag;

  the_mq = _POSIX_Message_queue_Get_by_name( name, &name_len, &error );

  /*
   *  If the name to id translation worked, then the message queue exists
   *  and we can just return a pointer to the id.  Otherwise we may
   *  need to check to see if this is a "message queue does not exist"
   *  or some other miscellaneous error on the name.
   */
  if ( the_mq == NULL ) {
    /*
     * Unless provided a valid name that did not already exist
     * and we are willing to create then it is an error.
     */
    if ( !( error == OBJECTS_GET_BY_NAME_NO_OBJECT && (oflag & O_CREAT) ) ) {
      _POSIX_Message_queue_Free_fd( the_mq_fd );
      _Objects_Allocator_unlock();
      rtems_set_errno_and_return_value(
        _POSIX_Get_by_name_error( error ),
        MQ_OPEN_FAILED
      );
    }

  } else {                /* name -> ID translation succeeded */
    /*
     * Check for existence with creation.
     */
    if ( (oflag & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL) ) {
      _POSIX_Message_queue_Free_fd( the_mq_fd );
      _Objects_Allocator_unlock();
      rtems_set_errno_and_return_value( EEXIST, MQ_OPEN_FAILED );
    }

    /*
     * In this case we need to do an ID->pointer conversion to
     * check the mode.
     */
    the_mq->open_count += 1;
    the_mq_fd->Queue = the_mq;
    _Objects_Open_string(
      &_POSIX_Message_queue_Information_fds,
      &the_mq_fd->Object,
      NULL
    );
    _Objects_Allocator_unlock();
    return the_mq_fd->Object.id;

  }

  /*
   *  At this point, the message queue does not exist and everything has been
   *  checked. We should go ahead and create a message queue.
   */
  status = _POSIX_Message_queue_Create_support(
    name,
    name_len,
    attr,
    &the_mq
  );

  /*
   * errno was set by Create_support, so don't set it again.
   */
  if ( status != 0 ) {
    _POSIX_Message_queue_Free_fd( the_mq_fd );
    _Objects_Allocator_unlock();
    return MQ_OPEN_FAILED;
  }

  the_mq_fd->Queue = the_mq;
  _Objects_Open_string(
    &_POSIX_Message_queue_Information_fds,
    &the_mq_fd->Object,
    NULL
  );

  _Objects_Allocator_unlock();

  return the_mq_fd->Object.id;
}
