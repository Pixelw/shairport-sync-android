#include "worker.h"
/*
 * This file is part of Shairport Sync.
 * Copyright (c) Mike Brady 2020--2025
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "definitions.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#ifdef COMPILE_FOR_FREEBSD
#include <netinet/in.h>
#endif
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#define __STDC_FORMAT_MACROS
#include "common.h"
#include "ptp-utilities.h"
#include "ipc_channel.h"
#include "utilities/network_utilities.h"
#include <inttypes.h>
#include <unistd.h>

int shm_fd;
void *mapped_addr = NULL;

// returns a copy of the shared memory data from the nqptp
// shared memory interface, so long as it's open.
int get_nqptp_data(struct shm_structure *nqptp_data) {
  return lx_ipc_reader_snapshot(nqptp_data, get_absolute_time_in_ns());
}

int ptp_get_clock_version() {
  int response = 0; // no version number information available
  struct shm_structure nqptp_data;
  if (get_nqptp_data(&nqptp_data) == 0) {
    response = nqptp_data.version;
  }
  return response;
}

int ptp_get_clock_info(uint64_t *actual_clock_id, uint64_t *time_of_sample,
                      uint64_t *raw_offset,
                      int64_t *clock_change_offset,
                      uint64_t *mastership_start_time) {
  int response = clock_ok;
  if (actual_clock_id != NULL)
    *actual_clock_id = 0;
  if (raw_offset != NULL)
    *raw_offset = 0;
  if (time_of_sample != NULL)
    *time_of_sample = 0;
  if (mastership_start_time != NULL)
    *mastership_start_time = 0;
  //  if (ptp_shm_interface_open() == 0) {
  struct shm_structure nqptp_data;
  if (get_nqptp_data(&nqptp_data) == 0) {
    if (nqptp_data.version == NQPTP_SHM_STRUCTURES_VERSION) {
      // assuming a clock id can not be zero
      if (nqptp_data.main.master_clock_id != 0) {
        if (actual_clock_id != NULL)
          *actual_clock_id = nqptp_data.main.master_clock_id;
        if (time_of_sample != NULL)
          *time_of_sample = nqptp_data.main.local_time;
        if (raw_offset != NULL)
          *raw_offset = nqptp_data.main.local_to_master_time_offset;
        if (clock_change_offset != NULL)
          *clock_change_offset = nqptp_data.main.clock_change_offset;
        if (mastership_start_time != NULL)
          *mastership_start_time = nqptp_data.main.master_clock_start_time;
      } else {
        response = clock_no_master;
      }
    } else {
      // the version can not be zero. If zero is returned here, it means that the shared memory is
      // not yet initialised, so not availalbe
      if (nqptp_data.version == 0)
        response = clock_service_unavailable;
      else
        response = clock_version_mismatch;
    }
  } else {
    response = clock_data_unavailable;
  }
  return response;
}

int ptp_shm_interface_open() {
  return lx_ipc_reader_open();
}

int ptp_shm_interface_close() {
  return lx_ipc_reader_close();
}

void ptp_send_control_message_string(const char *msg) {
  if(msg && (msg[0]=="T"[0] || msg[0]=="B"[0]))lx_ipc_reader_begin();
  if(msg && msg[0]=="E"[0])lx_ipc_reader_pause();
  size_t full_message_size =
      strlen(config.nqptp_shared_memory_interface_name) + strlen(" ") + strlen(msg) + 1;
  char *full_message = malloc(full_message_size);
  if (full_message != NULL) {
    *full_message = '\0';
    snprintf(full_message, full_message_size, "%s %s", config.nqptp_shared_memory_interface_name,
             msg);
    debug(2, "Send control message to NQPTP: \"%s\"", full_message);
    int s;
    int ret;
    struct addrinfo hints, *info;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    /* nqptp is only controllable via localhost */
    char portstr[20];
    snprintf(portstr, 20, "%d", NQPTP_CONTROL_PORT);

    ret = getaddrinfo("localhost", portstr, &hints, &info);
    if (ret) {
      die("getaddrinfo: %s", gai_strerror(ret));
    }

    /* Create a datagram socket in the internet domain and use the
     * default protocol (UDP).
     */
    if ((s = socket(info->ai_family, info->ai_socktype, 0)) < 0) {
      die("Can't open a socket to NQPTP");
    }

    /* Send the message in buf to the server */
    if (sendto(s, full_message, full_message_size, 0, info->ai_addr, info->ai_addrlen) < 0) {
      die("error sending timing_peer_list to NQPTP");
    }
    /* Deallocate the socket */
    safe_socket_close(&s);
    freeaddrinfo(info);

    /* deallocate the message string */
    free(full_message);
  } else {
    debug(1, "Couldn't allocate memory to prepare a qualified ptp control message string.");
  }
}
