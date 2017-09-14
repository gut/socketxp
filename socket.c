// Receiving socket based on:
// https://www.ibm.com/support/knowledgecenter/ssw_i5_54/rzab6/xnonblock.htm
//
// Sending socket implemented by Gustavo Scalet but tries to immitate syscalls
// used by Lua library "copas", test "largetransfer.lua"
//
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/param.h>
#include <time.h>
#include <fcntl.h>
#include <stdbool.h>

#define SERVER_PORT       12345

#define TRUE              1
#define FALSE             0
#define BUFSIZE           8192

int create_socket() {
  int sd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (sd < 0)
  {
    perror("socket() failed");
    exit(-1);
  }
  fcntl(sd, F_GETFL);
  fcntl(sd, F_SETFL, O_RDWR|O_NONBLOCK);
  return sd;
}

void usage(char* prog_name) {
  fprintf(stderr, "Usage: %s [-v] [-i] <buffer size>\n", prog_name);
  exit(-1);
}

int main (int argc, char *argv[]) {
  int    i, len, rc, on = 1;
  int    recv_sd, send_sd, max_sd, new_sd;
  int    desc_ready, end_server = FALSE;
  int    close_conn;
  size_t msg_size = BUFSIZE;
  char   buffer[BUFSIZE];
  char   *msg_send, *msg_recv, *ptr_send, *ptr_recv;
  struct sockaddr_in   addr;
  struct timeval       timeout;
  fd_set send_master_set, recv_master_set;
  fd_set send_working_set, recv_working_set;
  struct timespec interrupt_req = {0, 1000}; // 1ms

  // parsing args
  // define msg total size
  if (argc > 1) {
    msg_size = (size_t)atol(argv[1]);
  } else { // no args
    usage(argv[0]);
  }

  int opt;
  bool verbose = false, interrupt_flow = false;
  while ((opt = getopt(argc, argv, "vi")) != -1) {
    switch (opt) {
      case 'i': interrupt_flow = true; break;
      case 'v': verbose = true; break;
      default: usage(argv[0]);
    }
  }

  msg_recv = calloc(sizeof(char), msg_size);
  ptr_recv = msg_recv;

  msg_send = malloc(msg_size);
  ptr_send = msg_send;
  // fill the buffer with something
  memset(msg_send, 'A', msg_size);

  if (verbose) printf("Message initialized()...\n");

  /*************************************************************/
  /* Create an AF_INET stream socket to receive incoming       */
  /* connections on                                            */
  /*************************************************************/
  send_sd = create_socket();
  recv_sd = create_socket();

  /*************************************************************/
  /* Initialize the timeval struct.  If no activity after      */
  /* timeout, this program will end.                           */
  /*************************************************************/
  timeout.tv_sec  = 0;
  timeout.tv_usec = 5000;

  /*************************************************************/
  /* Allow socket descriptor to be reuseable                   */
  /*************************************************************/
  rc = setsockopt(recv_sd, SOL_SOCKET,  SO_REUSEADDR,
      (char *)&on, sizeof(on));
  if (rc < 0)
  {
    perror("setsockopt() failed");
    close(recv_sd);
    exit(-1);
  }

  /*************************************************************/
  /* Set socket to be nonblocking. All of the sockets for      */
  /* the incoming connections will also be nonblocking since   */
  /* they will inherit that state from the listening socket.   */
  /*************************************************************/
  // let's immitate luasocket behavior.
  fcntl(recv_sd, F_GETFL);
  fcntl(recv_sd, F_SETFL, O_RDWR);

  /*************************************************************/
  /* Bind the socket                                           */
  /*************************************************************/
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port        = htons(SERVER_PORT);
  rc = bind(recv_sd,
      (struct sockaddr *)&addr, sizeof(addr));
  if (rc < 0) {
    perror("bind() failed");
    close(recv_sd);
    exit(-1);
  }

  /*************************************************************/
  /* Set the listen back log                                   */
  /*************************************************************/
  rc = listen(recv_sd, 32);
  if (rc < 0) {
    perror("listen() failed");
    close(recv_sd);
    exit(-1);
  }

  // let's immitate luasocket behavior.
  fcntl(recv_sd, F_GETFL);
  fcntl(recv_sd, F_SETFL, O_RDWR|O_NONBLOCK);
  // connect the send socket
  do {
    rc = connect(send_sd,
        (struct sockaddr *)&addr, sizeof(addr));
  } while (rc < 0 && errno == EINPROGRESS);
  if (rc < 0) {
    perror("connect() failed");
    close(send_sd);
    exit(-1);
  }

  /*************************************************************/
  /* Initialize the master fd_set                              */
  /*************************************************************/
  FD_ZERO(&recv_master_set);
  FD_SET(recv_sd, &recv_master_set);

  FD_ZERO(&send_master_set);
  FD_SET(send_sd, &send_master_set);

  max_sd = MAX(send_sd, recv_sd);

  /*************************************************************/
  /* Loop waiting for incoming connects or for incoming data   */
  /* on any of the connected sockets.                          */
  /*************************************************************/

  do {
    /**********************************************************/
    /* Copy the master fd_set over to the working fd_set.     */
    /**********************************************************/
    memcpy(&recv_working_set, &recv_master_set, sizeof(recv_master_set));
    memcpy(&send_working_set, &send_master_set, sizeof(send_master_set));

    /**********************************************************/
    /* Call select() and wait 5 minutes for it to complete.   */
    /**********************************************************/
    if (verbose) printf("Waiting on select()...\n");
    rc = select(max_sd + 1, &recv_working_set,
        &send_working_set, NULL, &timeout);

    /**********************************************************/
    /* Check to see if the select call failed.                */
    /**********************************************************/
    if (rc < 0) {
      perror("  select() failed");
      break;
    }

    /**********************************************************/
    /* Check to see if the 3 seconds time out expired.        */
    /**********************************************************/
    if (rc == 0)
    {
      if (verbose) printf("  select() timed out.  End program.\n");
      break;
    }

    /**********************************************************/
    /* One or more descriptors are readable.  Need to         */
    /* determine which ones they are.                         */
    /**********************************************************/
    desc_ready = rc;
    for (i=0; i <= max_sd  &&  desc_ready > 0; ++i) {
      /*******************************************************/
      /* Check to see if this descriptor is ready            */
      /*******************************************************/
      if (FD_ISSET(i, &send_working_set)) {
        if (verbose) printf("  Send connection ready - %d\n", i);
        // try to send all the data
        while (ptr_send < (msg_send + msg_size)) {
          size_t remaining_bytes = msg_size - (ptr_send - msg_send);
          if (interrupt_flow) nanosleep(&interrupt_req, NULL);
          rc = send(send_sd, ptr_send, MIN(remaining_bytes, BUFSIZE), 0);
          if (rc < 0) {
            if (errno != EAGAIN) {
              perror("  send() failed");
            }
            // try again later. Select will assist on this task
            break;
          }
          else {
            len = rc;
            if (verbose) printf("  %d bytes sent (still %ld bytes to go!)\n",
                len, remaining_bytes-len);
            ptr_send += len;
          }
        }
        if (ptr_send == (msg_send + msg_size)) {
          // reached end. Close connection
          if (verbose) printf("  Sent all bytes. Closing connection - %d\n", i);
          close(i);
          FD_CLR(i, &send_master_set);
          if (i == max_sd) {
            while (
                (FD_ISSET(max_sd, &send_master_set) == FALSE) &&
                (FD_ISSET(max_sd, &recv_master_set) == FALSE)
                )
              max_sd -= 1;
          }
        }
        break; // dumb, but let's immitate luasocket behavior.
      } // if (FD_ISSET(i, &send_working_set))
      else if (FD_ISSET(i, &recv_working_set)) {
        /****************************************************/
        /* A descriptor was found that was readable - one   */
        /* less has to be looked for.  This is being done   */
        /* so that we can stop looking at the working set   */
        /* once we have found all of the descriptors that   */
        /* were ready.                                      */
        /****************************************************/
        desc_ready -= 1;

        /****************************************************/
        /* Check to see if this is the listening socket     */
        /****************************************************/
        if (i == recv_sd) {
          if (verbose) printf("  Listening socket is readable\n");
          /*************************************************/
          /* Accept all incoming connections that are      */
          /* queued up on the listening socket before we   */
          /* loop back and call select again.              */
          /*************************************************/
          do {
            /**********************************************/
            /* Accept each incoming connection.  If       */
            /* accept fails with EWOULDBLOCK, then we     */
            /* have accepted all of them.  Any other      */
            /* failure on accept will cause us to end the */
            /* server.                                    */
            /**********************************************/
            new_sd = accept(recv_sd, NULL, NULL);
            if (new_sd < 0) {
              if (errno != EWOULDBLOCK) {
                perror("  accept() failed");
                end_server = TRUE;
              }
              break;
            }

            /**********************************************/
            /* Add the new incoming connection to the     */
            /* master read set                            */
            /**********************************************/
            if (verbose) printf("  New incoming connection - %d\n", new_sd);
            FD_SET(new_sd, &recv_master_set);
            if (new_sd > max_sd)
              max_sd = new_sd;

            /**********************************************/
            /* Loop back up and accept another incoming   */
            /* connection                                 */
            /**********************************************/
          } while (new_sd != -1);
        }

        /****************************************************/
        /* This is not the listening socket, therefore an   */
        /* existing connection must be readable             */
        /****************************************************/
        else {
          if (verbose) printf("  Descriptor %d is readable\n", i);
          close_conn = FALSE;
          /*************************************************/
          /* Receive all incoming data on this socket      */
          /* before we loop back and call select again.    */
          /*************************************************/
          do {
            /**********************************************/
            /* Receive data on this connection until the  */
            /* recv fails with EWOULDBLOCK.  If any other */
            /* failure occurs, we will close the          */
            /* connection.                                */
            /**********************************************/
            if (interrupt_flow) nanosleep(&interrupt_req, NULL);
            rc = recv(i, buffer, sizeof(buffer), 0);
            if (rc < 0) {
              if (errno != EWOULDBLOCK) {
                perror("  recv() failed");
                close_conn = TRUE;
              }
              break;
            }

            /**********************************************/
            /* Check to see if the connection has been    */
            /* closed by the client                       */
            /**********************************************/
            if (rc == 0) {
              if (verbose) printf("  Connection closed - %d\n", i);
              close_conn = TRUE;
              break;
            }

            /**********************************************/
            /* Data was received                          */
            /**********************************************/
            len = rc;
            size_t remaining_bytes = msg_size - (ptr_recv - msg_recv);
            if (verbose) printf("  %d bytes received (still %ld bytes to go!)\n",
                len, remaining_bytes-len);
            if (len > remaining_bytes) {
              perror("   recv() too many bytes");
              exit(-1);
            }
            memcpy(ptr_recv, buffer, len);
            ptr_recv += len;

          } while (TRUE);

          /*************************************************/
          /* If the close_conn flag was turned on, we need */
          /* to clean up this active connection.  This     */
          /* clean up process includes removing the        */
          /* descriptor from the master set and            */
          /* determining the new maximum descriptor value  */
          /* based on the bits that are still turned on in */
          /* the master set.                               */
          /*************************************************/
          if (close_conn) {
            close(i);
            FD_CLR(i, &recv_master_set);
            if (i == max_sd) {
              while (
                  (FD_ISSET(max_sd, &send_master_set) == FALSE) &&
                  (FD_ISSET(max_sd, &recv_master_set) == FALSE)
                  )
                max_sd -= 1;
            }
          }
        } /* End of existing connection is readable */
        break; // dumb, but let's immitate luasocket behavior.
      } /* End of if (FD_ISSET(i, &working_set)) */
    } /* End of loop through selectable descriptors */

  } while (end_server == FALSE);

  /*************************************************************/
  /* Clean up all of the sockets that are open                  */
  /*************************************************************/
  for (i=0; i <= max_sd; ++i) {
    if (FD_ISSET(i, &recv_master_set) || FD_ISSET(i, &send_master_set))
      close(i);
  }

  return 0;
}
