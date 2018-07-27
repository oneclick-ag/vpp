/*
 * Copyright (c) 2017-2018 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <vcl/vcl_test.h>
#include <sys/epoll.h>
#include <vppinfra/mem.h>

typedef struct
{
  uint8_t is_alloc;
  int fd;
  uint8_t *buf;
  uint32_t buf_size;
  sock_test_cfg_t cfg;
  sock_test_stats_t stats;
  vppcom_endpt_t endpt;
  uint8_t ip[16];
} sock_server_conn_t;

typedef struct
{
  uint32_t port;
  uint32_t address_ip6;
  uint32_t transport_udp;
} sock_server_cfg_t;

#define SOCK_SERVER_MAX_TEST_CONN  10
#define SOCK_SERVER_MAX_EPOLL_EVENTS 10
typedef struct
{
  int listen_fd;
  sock_server_cfg_t cfg;
  int epfd;
  struct epoll_event listen_ev;
  struct epoll_event wait_events[SOCK_SERVER_MAX_EPOLL_EVENTS];
  size_t num_conn;
  size_t conn_pool_size;
  sock_server_conn_t *conn_pool;
  int nfds;
  fd_set rd_fdset;
  fd_set wr_fdset;
  struct timeval timeout;
} sock_server_main_t;

sock_server_main_t sock_server_main;

static inline void
conn_pool_expand (size_t expand_size)
{
  sock_server_main_t *ssm = &sock_server_main;
  sock_server_conn_t *conn_pool;
  size_t new_size = ssm->conn_pool_size + expand_size;
  int i;

  conn_pool = realloc (ssm->conn_pool, new_size * sizeof (*ssm->conn_pool));
  if (conn_pool)
    {
      for (i = ssm->conn_pool_size; i < new_size; i++)
	{
	  sock_server_conn_t *conn = &conn_pool[i];
	  memset (conn, 0, sizeof (*conn));
	  sock_test_cfg_init (&conn->cfg);
	  sock_test_buf_alloc (&conn->cfg, 1 /* is_rxbuf */ ,
			       &conn->buf, &conn->buf_size);
	  conn->cfg.txbuf_size = conn->cfg.rxbuf_size;
	}

      ssm->conn_pool = conn_pool;
      ssm->conn_pool_size = new_size;
    }
  else
    {
      int errno_val = errno;
      perror ("ERROR in conn_pool_expand()");
      fprintf (stderr, "SERVER: ERROR: Memory allocation "
	       "failed (errno = %d)!\n", errno_val);
    }
}

static inline sock_server_conn_t *
conn_pool_alloc (void)
{
  sock_server_main_t *ssm = &sock_server_main;
  int i;

  for (i = 0; i < ssm->conn_pool_size; i++)
    {
      if (!ssm->conn_pool[i].is_alloc)
	{
	  ssm->conn_pool[i].endpt.ip = ssm->conn_pool[i].ip;
	  ssm->conn_pool[i].is_alloc = 1;
	  return (&ssm->conn_pool[i]);
	}
    }

  return 0;
}

static inline void
conn_pool_free (sock_server_conn_t * conn)
{
  conn->fd = 0;
  conn->is_alloc = 0;
}

static inline void
sync_config_and_reply (sock_server_conn_t * conn, sock_test_cfg_t * rx_cfg)
{
  conn->cfg = *rx_cfg;
  sock_test_buf_alloc (&conn->cfg, 1 /* is_rxbuf */ ,
		       &conn->buf, &conn->buf_size);
  conn->cfg.txbuf_size = conn->cfg.rxbuf_size;

  if (conn->cfg.verbose)
    {
      printf ("\nSERVER (fd %d): Replying to cfg message!\n", conn->fd);
      sock_test_cfg_dump (&conn->cfg, 0 /* is_client */ );
    }
  (void) vcl_test_write (conn->fd, (uint8_t *) & conn->cfg,
			 sizeof (conn->cfg), NULL, conn->cfg.verbose);
}

static void
stream_test_server_start_stop (sock_server_conn_t * conn,
			       sock_test_cfg_t * rx_cfg)
{
  sock_server_main_t *ssm = &sock_server_main;
  int client_fd = conn->fd;
  sock_test_t test = rx_cfg->test;

  if (rx_cfg->ctrl_handle == conn->fd)
    {
      int i;
      clock_gettime (CLOCK_REALTIME, &conn->stats.stop);

      for (i = 0; i < ssm->conn_pool_size; i++)
	{
	  sock_server_conn_t *tc = &ssm->conn_pool[i];

	  if (tc->cfg.ctrl_handle == conn->fd)
	    {
	      sock_test_stats_accumulate (&conn->stats, &tc->stats);

	      if (conn->cfg.verbose)
		{
		  static char buf[64];

		  sprintf (buf, "SERVER (fd %d) RESULTS", tc->fd);
		  sock_test_stats_dump (buf, &tc->stats, 1 /* show_rx */ ,
					test == SOCK_TEST_TYPE_BI
					/* show tx */ ,
					conn->cfg.verbose);
		}
	    }
	}

      sock_test_stats_dump ("SERVER RESULTS", &conn->stats, 1 /* show_rx */ ,
			    (test == SOCK_TEST_TYPE_BI) /* show_tx */ ,
			    conn->cfg.verbose);
      sock_test_cfg_dump (&conn->cfg, 0 /* is_client */ );
      if (conn->cfg.verbose)
	{
	  printf ("  sock server main\n"
		  SOCK_TEST_SEPARATOR_STRING
		  "       buf:  %p\n"
		  "  buf size:  %u (0x%08x)\n"
		  SOCK_TEST_SEPARATOR_STRING,
		  conn->buf, conn->buf_size, conn->buf_size);
	}

      sync_config_and_reply (conn, rx_cfg);
      printf ("\nSERVER (fd %d): %s-directional Stream Test Complete!\n"
	      SOCK_TEST_BANNER_STRING "\n", conn->fd,
	      test == SOCK_TEST_TYPE_BI ? "Bi" : "Uni");
    }
  else
    {
      printf ("\n" SOCK_TEST_BANNER_STRING
	      "SERVER (fd %d): %s-directional Stream Test!\n"
	      "  Sending client the test cfg to start streaming data...\n",
	      client_fd, test == SOCK_TEST_TYPE_BI ? "Bi" : "Uni");

      rx_cfg->ctrl_handle = (rx_cfg->ctrl_handle == ~0) ? conn->fd :
	rx_cfg->ctrl_handle;

      sync_config_and_reply (conn, rx_cfg);

      /* read the 1st chunk, record start time */
      memset (&conn->stats, 0, sizeof (conn->stats));
      clock_gettime (CLOCK_REALTIME, &conn->stats.start);
    }
}


static inline void
stream_test_server (sock_server_conn_t * conn, int rx_bytes)
{
  int client_fd = conn->fd;
  sock_test_t test = conn->cfg.test;

  if (test == SOCK_TEST_TYPE_BI)
    (void) vcl_test_write (client_fd, conn->buf, rx_bytes, &conn->stats,
			   conn->cfg.verbose);

  if (conn->stats.rx_bytes >= conn->cfg.total_bytes)
    {
      clock_gettime (CLOCK_REALTIME, &conn->stats.stop);
    }
}

static inline void
new_client (void)
{
  sock_server_main_t *ssm = &sock_server_main;
  int client_fd;
  sock_server_conn_t *conn;

  if (ssm->conn_pool_size < (ssm->num_conn + SOCK_SERVER_MAX_TEST_CONN + 1))
    conn_pool_expand (SOCK_SERVER_MAX_TEST_CONN + 1);

  conn = conn_pool_alloc ();
  if (!conn)
    {
      fprintf (stderr, "\nSERVER: ERROR: No free connections!\n");
      return;
    }

  client_fd = vppcom_session_accept (ssm->listen_fd, &conn->endpt, 0);
  if (client_fd < 0)
    {
      int errno_val;
      errno_val = errno = -client_fd;
      perror ("ERROR in new_client()");
      fprintf (stderr, "SERVER: ERROR: accept failed "
	       "(errno = %d)!\n", errno_val);
      return;
    }

  printf ("SERVER: Got a connection -- fd = %d (0x%08x)!\n",
	  client_fd, client_fd);

  conn->fd = client_fd;

  {
    struct epoll_event ev;
    int rv;

    ev.events = EPOLLIN;
    ev.data.u64 = conn - ssm->conn_pool;
    rv = vppcom_epoll_ctl (ssm->epfd, EPOLL_CTL_ADD, client_fd, &ev);
    if (rv < 0)
      {
	int errno_val;
	errno_val = errno = -rv;
	perror ("ERROR in new_client()");
	fprintf (stderr, "SERVER: ERROR: epoll_ctl failed (errno = %d)!\n",
		 errno_val);
      }
    else
      ssm->nfds++;
  }
}

void
print_usage_and_exit (void)
{
  fprintf (stderr,
	   "sock_test_server [OPTIONS] <port>\n"
	   "  OPTIONS\n"
	   "  -h               Print this message and exit.\n"
	   "  -6               Use IPv6\n"
	   "  -u               Use UDP transport layer\n");
  exit (1);
}

int
main (int argc, char **argv)
{
  sock_server_main_t *ssm = &sock_server_main;
  int client_fd, rv, main_rv = 0;
  int tx_bytes, rx_bytes, nbytes;
  sock_server_conn_t *conn;
  sock_test_cfg_t *rx_cfg;
  uint32_t xtra = 0;
  uint64_t xtra_bytes = 0;
  struct sockaddr_storage servaddr;
  int errno_val;
  int c, v, i;
  uint16_t port = SOCK_TEST_SERVER_PORT;
  vppcom_endpt_t endpt;

  clib_mem_init_thread_safe (0, 64 << 20);

  opterr = 0;
  while ((c = getopt (argc, argv, "6D")) != -1)
    switch (c)
      {
      case '6':
	ssm->cfg.address_ip6 = 1;
	break;

      case 'D':
	ssm->cfg.transport_udp = 1;
	break;

      case '?':
	switch (optopt)
	  {
	  default:
	    if (isprint (optopt))
	      fprintf (stderr, "SERVER: ERROR: Unknown "
		       "option `-%c'.\n", optopt);
	    else
	      fprintf (stderr, "SERVER: ERROR: Unknown "
		       "option character `\\x%x'.\n", optopt);
	  }
	/* fall thru */
      case 'h':
      default:
	print_usage_and_exit ();
      }

  if (argc < (optind + 1))
    {
      fprintf (stderr, "SERVER: ERROR: Insufficient number of arguments!\n");
      print_usage_and_exit ();
    }

  if (sscanf (argv[optind], "%d", &v) == 1)
    port = (uint16_t) v;
  else
    {
      fprintf (stderr, "SERVER: ERROR: Invalid port (%s)!\n", argv[optind]);
      print_usage_and_exit ();
    }

  conn_pool_expand (SOCK_SERVER_MAX_TEST_CONN + 1);

  rv = vppcom_app_create ("vcl_test_server");
  if (rv)
    {
      errno_val = errno = -rv;
      perror ("ERROR in main()");
      fprintf (stderr, "SERVER: ERROR: vppcom_app_create() failed "
	       "(errno = %d)!\n", errno_val);
      return -1;
    }
  else
    {
      ssm->listen_fd = vppcom_session_create (ssm->cfg.transport_udp ?
					      VPPCOM_PROTO_UDP :
					      VPPCOM_PROTO_TCP,
					      0 /* is_nonblocking */ );
    }
  if (ssm->listen_fd < 0)
    {
      errno_val = errno = -ssm->listen_fd;
      perror ("ERROR in main()");
      fprintf (stderr, "SERVER: ERROR: vppcom_session_create() failed "
	       "(errno = %d)!\n", errno_val);
      return -1;
    }

  memset (&servaddr, 0, sizeof (servaddr));

  if (ssm->cfg.address_ip6)
    {
      struct sockaddr_in6 *server_addr = (struct sockaddr_in6 *) &servaddr;
      server_addr->sin6_family = AF_INET6;
      server_addr->sin6_addr = in6addr_any;
      server_addr->sin6_port = htons (port);
    }
  else
    {
      struct sockaddr_in *server_addr = (struct sockaddr_in *) &servaddr;
      server_addr->sin_family = AF_INET;
      server_addr->sin_addr.s_addr = htonl (INADDR_ANY);
      server_addr->sin_port = htons (port);
    }

  if (ssm->cfg.address_ip6)
    {
      struct sockaddr_in6 *server_addr = (struct sockaddr_in6 *) &servaddr;
      endpt.is_ip4 = 0;
      endpt.ip = (uint8_t *) & server_addr->sin6_addr;
      endpt.port = (uint16_t) server_addr->sin6_port;
    }
  else
    {
      struct sockaddr_in *server_addr = (struct sockaddr_in *) &servaddr;
      endpt.is_ip4 = 1;
      endpt.ip = (uint8_t *) & server_addr->sin_addr;
      endpt.port = (uint16_t) server_addr->sin_port;
    }

  rv = vppcom_session_bind (ssm->listen_fd, &endpt);
  if (rv < 0)
    {
      errno_val = errno = -rv;
      perror ("ERROR in main()");
      fprintf (stderr, "SERVER: ERROR: bind failed (errno = %d)!\n",
	       errno_val);
      return -1;
    }

  if (!ssm->cfg.transport_udp)
    {
      rv = vppcom_session_listen (ssm->listen_fd, 10);
      if (rv < 0)
	{
	  errno_val = errno = -rv;
	  perror ("ERROR in main()");
	  fprintf (stderr, "SERVER: ERROR: listen failed "
		   "(errno = %d)!\n", errno_val);
	  return -1;
	}
    }

  ssm->epfd = vppcom_epoll_create ();
  if (ssm->epfd < 0)
    {
      errno_val = errno = -ssm->epfd;
      perror ("ERROR in main()");
      fprintf (stderr, "SERVER: ERROR: epoll_create failed (errno = %d)!\n",
	       errno_val);
      return -1;
    }

  ssm->listen_ev.events = EPOLLIN;
  ssm->listen_ev.data.u32 = ~0;
  rv = vppcom_epoll_ctl (ssm->epfd, EPOLL_CTL_ADD, ssm->listen_fd,
			 &ssm->listen_ev);
  if (rv < 0)
    {
      errno_val = errno = -rv;
      perror ("ERROR in main()");
      fprintf (stderr, "SERVER: ERROR: epoll_ctl failed "
	       "(errno = %d)!\n", errno_val);
      return -1;
    }
  printf ("\nSERVER: Waiting for a client to connect on port %d...\n", port);

  while (1)
    {
      int num_ev;
      num_ev = vppcom_epoll_wait (ssm->epfd, ssm->wait_events,
				  SOCK_SERVER_MAX_EPOLL_EVENTS, 60000.0);
      if (num_ev < 0)
	{
	  errno = -num_ev;
	  perror ("epoll_wait()");
	  fprintf (stderr, "\nSERVER: ERROR: epoll_wait() "
		   "failed -- aborting!\n");
	  main_rv = -1;
	  goto done;
	}
      else if (num_ev == 0)
	{
	  fprintf (stderr, "\nSERVER: epoll_wait() timeout!\n");
	  continue;
	}
      for (i = 0; i < num_ev; i++)
	{
	  conn = &ssm->conn_pool[ssm->wait_events[i].data.u32];
	  if (ssm->wait_events[i].events & (EPOLLHUP | EPOLLRDHUP))
	    {
	      vppcom_session_close (conn->fd);
	      continue;
	    }
	  if (ssm->wait_events[i].data.u32 == ~0)
	    {
	      new_client ();
	      continue;
	    }
	  client_fd = conn->fd;

	  if (EPOLLIN & ssm->wait_events[i].events)
	    {
	      rx_bytes = vcl_test_read (client_fd, conn->buf,
					conn->buf_size, &conn->stats);
	      if (rx_bytes > 0)
		{
		  rx_cfg = (sock_test_cfg_t *) conn->buf;
		  if (rx_cfg->magic == SOCK_TEST_CFG_CTRL_MAGIC)
		    {
		      if (rx_cfg->verbose)
			{
			  printf ("SERVER (fd %d): Received a cfg message!\n",
				  client_fd);
			  sock_test_cfg_dump (rx_cfg, 0 /* is_client */ );
			}

		      if (rx_bytes != sizeof (*rx_cfg))
			{
			  printf ("SERVER (fd %d): Invalid cfg message "
				  "size (%d)!\n  Should be %lu bytes.\n",
				  client_fd, rx_bytes, sizeof (*rx_cfg));
			  conn->cfg.rxbuf_size = 0;
			  conn->cfg.num_writes = 0;
			  if (conn->cfg.verbose)
			    {
			      printf ("SERVER (fd %d): Replying to "
				      "cfg message!\n", client_fd);
			      sock_test_cfg_dump (rx_cfg, 0 /* is_client */ );
			    }
			  vcl_test_write (client_fd, (uint8_t *) & conn->cfg,
					  sizeof (conn->cfg), NULL,
					  conn->cfg.verbose);
			  continue;
			}

		      switch (rx_cfg->test)
			{
			case SOCK_TEST_TYPE_NONE:
			case SOCK_TEST_TYPE_ECHO:
			  sync_config_and_reply (conn, rx_cfg);
			  break;

			case SOCK_TEST_TYPE_BI:
			case SOCK_TEST_TYPE_UNI:
			  stream_test_server_start_stop (conn, rx_cfg);
			  break;

			case SOCK_TEST_TYPE_EXIT:
			  printf ("SERVER: Have a great day, "
				  "connection %d!\n", client_fd);
			  vppcom_session_close (client_fd);
			  conn_pool_free (conn);
			  printf ("SERVER: Closed client fd %d\n", client_fd);
			  ssm->nfds--;
			  if (!ssm->nfds)
			    {
			      printf ("SERVER: All client connections "
				      "closed.\n\nSERVER: "
				      "May the force be with you!\n\n");
			      goto done;
			    }
			  break;

			default:
			  fprintf (stderr,
				   "SERVER: ERROR: Unknown test type!\n");
			  sock_test_cfg_dump (rx_cfg, 0 /* is_client */ );
			  break;
			}
		      continue;
		    }

		  else if ((conn->cfg.test == SOCK_TEST_TYPE_UNI) ||
			   (conn->cfg.test == SOCK_TEST_TYPE_BI))
		    {
		      stream_test_server (conn, rx_bytes);
		      continue;
		    }

		  else if (isascii (conn->buf[0]))
		    {
		      /* If it looks vaguely like a string,
		       * make sure it's terminated.
		       */
		      ((char *) conn->buf)[rx_bytes <
					   conn->buf_size ? rx_bytes :
					   conn->buf_size - 1] = 0;
		      printf ("SERVER (fd %d): RX (%d bytes) - '%s'\n",
			      conn->fd, rx_bytes, conn->buf);
		    }
		}
	      else		// rx_bytes < 0
		{
		  if (errno == ECONNRESET)
		    {
		      printf ("\nSERVER: Connection reset by remote peer.\n"
			      "  Y'all have a great day now!\n\n");
		      break;
		    }
		  else
		    continue;
		}

	      if (isascii (conn->buf[0]))
		{
		  /* If it looks vaguely like a string,
		   * make sure it's terminated
		   */
		  ((char *) conn->buf)[rx_bytes <
				       conn->buf_size ? rx_bytes :
				       conn->buf_size - 1] = 0;
		  if (xtra)
		    fprintf (stderr, "SERVER: ERROR: "
			     "FIFO not drained in previous test!\n"
			     "       extra chunks %u (0x%x)\n"
			     "        extra bytes %lu (0x%lx)\n",
			     xtra, xtra, xtra_bytes, xtra_bytes);

		  xtra = 0;
		  xtra_bytes = 0;

		  if (conn->cfg.verbose)
		    printf ("SERVER (fd %d): Echoing back\n", client_fd);

		  nbytes = strlen ((const char *) conn->buf) + 1;

		  tx_bytes = vcl_test_write (client_fd, conn->buf,
					     nbytes, &conn->stats,
					     conn->cfg.verbose);
		  if (tx_bytes >= 0)
		    printf ("SERVER (fd %d): TX (%d bytes) - '%s'\n",
			    conn->fd, tx_bytes, conn->buf);
		}

	      else		// Extraneous read data from non-echo tests???
		{
		  xtra++;
		  xtra_bytes += rx_bytes;
		}
	    }
	}
    }

done:
  vppcom_session_close (ssm->listen_fd);
  vppcom_app_destroy ();

  if (ssm->conn_pool)
    free (ssm->conn_pool);

  return main_rv;
}

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
