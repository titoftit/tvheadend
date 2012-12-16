/*
 *  RTSP IPTV Input
 *  Copyright (C) 2012
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "iptv_input_rtsp.h"

#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h> 
#include <netinet/in.h>
#include <arpa/inet.h>
#include <math.h>
#include <zconf.h>

#include "tvheadend.h"
#include "rtcp.h"

/**
 * Define a simple header value with name and value, without ending \r\n
 * The response code isn't included, since curl keeps it as an internal value.
 */
typedef struct curl_header
{
  char *name;
  char *value;
  LIST_ENTRY(curl_header) next;
} curl_header_t;

/**
 * The whole response, with a linked-list for headers and a raw string for data.
 */
typedef struct curl_response
{
  LIST_HEAD(, curl_header) header_head;
  char *data;
} curl_response_t;

/* the function to invoke as the data recieved */
size_t static
curl_data_write_func(void *buffer, size_t size, size_t nmemb, void *userp)
{
  curl_response_t *response_ptr = (curl_response_t *)userp;

  /* assuming the response is a string */
  response_ptr->data = strndup(buffer, (size_t)(size * nmemb));
  
  return size * nmemb;
}

size_t static
curl_header_write_func(void *buffer, size_t size, size_t nmemb, void *userp)
{
  size_t used = size * nmemb;
  char *raw_header = (char *)buffer;
  char *separator = strstr(buffer, ": ");
  if(separator)
  {
    curl_response_t *response_ptr =  (curl_response_t *)userp;

    curl_header_t *header = malloc(sizeof(curl_header_t));
    size_t position = separator - raw_header;
    header->name = strndup(buffer, position);
    
    /* Trim the value */
    separator += 2;
    while(isspace(*separator))
    {
      ++separator;
    }
    position = separator - raw_header;
    size_t value_size = (size_t)(size * nmemb) - position;
    /* Trim the ending whitespaces and return chars */
    while(isspace(*(separator + value_size - 1)))
    {
      --value_size;
    }
    header->value = strndup(separator, value_size);

    LIST_INSERT_HEAD(&response_ptr->header_head, header, next);
  }
  
  return used;
}

static void
clean_header_list(curl_response_t *response)
{
  curl_header_t *header;
  while (! LIST_EMPTY(&response->header_head))
  {
    header = LIST_FIRST(&response->header_head);
    LIST_REMOVE(header, next);
    free(header);
  }

}

static void
set_curl_result_buffer(CURL *curl, curl_response_t *buffer)
{
  if(buffer)
  {
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_data_write_func);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buffer);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_header_write_func);
    curl_easy_setopt(curl, CURLOPT_WRITEHEADER, buffer);
    clean_header_list(buffer);
    if(buffer->data)
    {
      free(buffer->data);
      buffer->data = NULL;
    }
  } else {
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, NULL);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, NULL);
    curl_easy_setopt(curl, CURLOPT_WRITEHEADER, NULL);
  }
}

 
/* send RTSP OPTIONS request */ 
static CURLcode
rtsp_options(CURL *curl, const char *uri, curl_response_t *response)
{
  CURLcode result;
  result = curl_easy_setopt(curl, CURLOPT_RTSP_STREAM_URI, uri);
  if(result != CURLE_OK)
  {
    return result;
  }
  result = curl_easy_setopt(curl, CURLOPT_RTSP_REQUEST, CURL_RTSPREQ_OPTIONS);
  if (result != CURLE_OK)
  {
    return result;
  }
  set_curl_result_buffer(curl, response);
  return curl_easy_perform(curl);
}
 
/* send RTSP DESCRIBE request */ 
static CURLcode
rtsp_describe(CURL *curl, curl_response_t *response)
{
  CURLcode result;
  result = curl_easy_setopt(curl, CURLOPT_RTSP_REQUEST, CURL_RTSPREQ_DESCRIBE);
  if(result != CURLE_OK)
  {
    return result;
  }
  set_curl_result_buffer(curl, response);
  return curl_easy_perform(curl);
}
 
/* send RTSP SETUP request */ 
static CURLcode
rtsp_setup(CURL *curl, const char *uri, const char *transport, curl_response_t *response)
{
  CURLcode result;
  result = curl_easy_setopt(curl, CURLOPT_RTSP_STREAM_URI, uri);
  if(result != CURLE_OK)
  {
    return result;
  }
  result = curl_easy_setopt(curl, CURLOPT_RTSP_TRANSPORT, transport);
  if(result != CURLE_OK)
  {
    return result;
  }
  result = curl_easy_setopt(curl, CURLOPT_RTSP_REQUEST, CURL_RTSPREQ_SETUP);
  if(result != CURLE_OK)
  {
    return result;
  }
  set_curl_result_buffer(curl, response);
  return curl_easy_perform(curl);
}
 
 
/* send RTSP PLAY request */ 
static CURLcode
rtsp_play(CURL *curl, const char *uri, const char *range, curl_response_t *response)
{
  CURLcode result;
  result = curl_easy_setopt(curl, CURLOPT_RTSP_STREAM_URI, uri);
  if(result != CURLE_OK)
  {
    return result;
  }
  result = curl_easy_setopt(curl, CURLOPT_RANGE, range);
  if(result != CURLE_OK)
  {
    return result;
  }
  result = curl_easy_setopt(curl, CURLOPT_RTSP_REQUEST, CURL_RTSPREQ_PLAY);
  if(result != CURLE_OK)
  {
    return result;
  }
  set_curl_result_buffer(curl, response);
  return curl_easy_perform(curl);
}
 
 
/* send RTSP TEARDOWN request */ 
static CURLcode
rtsp_teardown(CURL *curl, const char *uri, curl_response_t *response)
{
  CURLcode result;
  result = curl_easy_setopt(curl, CURLOPT_RTSP_REQUEST, CURL_RTSPREQ_TEARDOWN);
  if(result != CURLE_OK)
  {
    return result;
  }
  set_curl_result_buffer(curl, response);
  return curl_easy_perform(curl);
}

static CURL*
curl_init()
{
  CURL *curl = curl_easy_init();
  if (curl != NULL) {
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
  }
  return curl;
}

static curl_response_t *
create_response()
{
  curl_response_t *response = calloc(1, sizeof(curl_response_t));
  LIST_INIT(&response->header_head);
  return response;
}

static void
destroy_response(curl_response_t *response)
{
  clean_header_list(response);
  if(response->data)
  {
    free(response->data);
    response->data = NULL;
  }
  free(response);
  response = NULL;
}

static int
iptv_rtsp_bind(iptv_rtsp_info_t* rtsp_info, int *fd, const char *service)
{
  struct addrinfo hints;
  int bind_tries = 10;
  
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;  // use IPv4 or IPv6, whichever
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE;     // fill in my IP for me

  rtsp_info->client_port = -1;
  while(rtsp_info->client_port % 2 && --bind_tries >= 0)
  {
    struct addrinfo *resolved_address;
    // Try to find a free even port for RTP
    getaddrinfo(NULL, service, &hints, &resolved_address);
    
    // We need to close the descriptor first, in case we are in the second iteration
    close(*fd);

    *fd = tvh_socket(resolved_address->ai_family, resolved_address->ai_socktype, resolved_address->ai_protocol);

    int true = 1;
    setsockopt(*fd, SOL_SOCKET, SO_REUSEADDR, &true, sizeof(true));

    if (bind(*fd, resolved_address->ai_addr, resolved_address->ai_addrlen) == -1)
    {
      int err = errno;
      tvhlog(LOG_ERR, "IPTV", "RTSP cannot bind %s:%d on fd %d -- %s (%d)",
             inet_ntoa(((struct sockaddr_in *) resolved_address->ai_addr)->sin_addr),
             ntohs(((struct sockaddr_in *) resolved_address->ai_addr)->sin_port),
             *fd, strerror(err), err);

      freeaddrinfo(resolved_address);
      return -1;
    }

    // Get the bound port back
    getsockname(*fd, resolved_address->ai_addr, &resolved_address->ai_addrlen);
    
    rtsp_info->client_addr = resolved_address;

    switch (resolved_address->ai_family)
    {
    case AF_INET:
      rtsp_info->client_port = ntohs(((struct sockaddr_in *) resolved_address->ai_addr)->sin_port);
      break;
    case AF_INET6:
      rtsp_info->client_port = ntohs(((struct sockaddr_in6 *) resolved_address->ai_addr)->sin6_port);
      break;
    default:
      tvhlog(LOG_ERR, "IPTV", "RTSP unknown socket family %d", resolved_address->ai_family);
    }

    tvhlog(LOG_DEBUG, "IPTV", "RTSP bound to %s:%d on fd %d",
           inet_ntoa(((struct sockaddr_in *) resolved_address->ai_addr)->sin_addr),
           ntohs(((struct sockaddr_in *) resolved_address->ai_addr)->sin_port),
           *fd);
  }
  
  return rtsp_info->client_port % 2 ? -1 : 0;
}

static int
iptv_rtsp_check_client_port(iptv_rtsp_info_t* rtsp_info, int *fd)
{
  int result = 0;
  int current_port = -1;
  switch (rtsp_info->client_addr->ai_family)
  {
  case AF_INET:
    current_port = ntohs(((struct sockaddr_in *) rtsp_info->client_addr->ai_addr)->sin_port);
    break;
  case AF_INET6:
    current_port = ntohs(((struct sockaddr_in6 *) rtsp_info->client_addr->ai_addr)->sin6_port);
    break;
  default:
    tvhlog(LOG_ERR, "IPTV", "RTSP unknown socket family %d", rtsp_info->client_addr->ai_family);
  }
  if(rtsp_info->client_port != -1 && rtsp_info->client_port != current_port)
  {
    // Rebind to the desired port
    close(*fd);
    char *service = malloc(sizeof(char) * (log10(rtsp_info->client_port) + 2));
    sprintf(service, "%d", rtsp_info->client_port);
    result = iptv_rtsp_bind(rtsp_info, fd, service);
    free(service);
  }
  return result;
}

static int
iptv_rtsp_parse_sdp_port(const char *data, const char *needle)
{
  int port = -1;
  char *substr = strstr(data, needle);
  if(substr)
  {
    substr += strlen(needle);
    char *end = strchr(substr, '-');
    if(end)
    {
      char *str_port = strndup(substr, end - substr);
      port = atoi(str_port);
      free(str_port);
    }
  }
  return port;
}

static int
iptv_rtsp_parse_setup(iptv_rtsp_info_t* rtsp_info, const curl_response_t *response)
{
  curl_header_t *header;
  
  // First set client and server port
  LIST_FOREACH(header, &response->header_head, next)
  {
    if(strcmp(header->name, "Transport") == 0)
    {
      rtsp_info->client_port = iptv_rtsp_parse_sdp_port(header->value, "client_port=");
      rtsp_info->server_port = iptv_rtsp_parse_sdp_port(header->value, "server_port=");
      break;
    }
  }
  
  // Now remember server address
  struct addrinfo hints, *resolved_address;
  char *primary_ip;
  char *service = malloc(sizeof(char) * (log10(rtsp_info->server_port) + 2));
  
  curl_easy_getinfo(rtsp_info->curl, CURLINFO_PRIMARY_IP, &primary_ip);
  sprintf(service, "%d", rtsp_info->server_port + 1);
  
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = rtsp_info->client_addr->ai_family;  // use the same as what we already have
  hints.ai_socktype = SOCK_DGRAM;

  if(getaddrinfo(primary_ip, service, &hints, &resolved_address) != 0)
  {
    tvhlog(LOG_ERR, "IPTV", "RTSP : Unable to resolve resolve server address");
    return -1;
  }
  rtsp_info->server_addr = resolved_address;
  
  return rtsp_info->client_port > 1024;
}

iptv_rtsp_info_t *
iptv_rtsp_start(const char *uri, int *fd)
{
  /* initialize this curl session */
  CURL *curl = curl_init();
  curl_response_t *response = create_response();
  iptv_rtsp_info_t *rtsp_info;
  CURLcode result;
  
  rtsp_info = malloc(sizeof(iptv_rtsp_info_t));
  rtsp_info->curl = curl;
  rtsp_info->uri = uri;
  rtsp_info->client_addr = NULL;
  rtsp_info->server_addr = NULL;
  rtsp_info->is_initialized = -1;
  
  result = curl_easy_setopt(curl, CURLOPT_URL, uri);
  tvhlog(LOG_DEBUG, "IPTV", "cURL init : %d", result);
  
  result = rtsp_options(curl, uri, response);
  curl_header_t *header = response->header_head.lh_first->next.le_next;
  tvhlog(LOG_DEBUG, "IPTV", "RTSP OPTIONS answer : %d, %s: %s", result, header->name, header->value);
  
  result = rtsp_describe(curl, response);
  tvhlog(LOG_DEBUG, "IPTV", "RTSP DESCRIBE answer : %d, %s", result, response->data);

  // Bind with any free even port for now
  if(iptv_rtsp_bind(rtsp_info, fd, "0") == -1)
  {
    iptv_rtsp_stop(rtsp_info);
    return NULL;
  }
  
  char* transport_format = (char*) "RTP/AVP;unicast;client_port=%d-%d";
  /*
   -4 for format string '%d'
   +10 for max port size (5 digits)
   +1 for ending \0
   */
  char transport[strlen(transport_format) + 7]; 
  sprintf(transport, transport_format, rtsp_info->client_port, rtsp_info->client_port + 1);
  
  result = rtsp_setup(curl, uri, transport, response);
  header = response->header_head.lh_first->next.le_next;
  tvhlog(LOG_DEBUG, "IPTV", "RTSP SETUP answer : %d, %s: %s", result, header->name, header->value);
  
  iptv_rtsp_parse_setup(rtsp_info, response);
  
  // Now we check that the client port we received match. If not, reopen the socket
  iptv_rtsp_check_client_port(rtsp_info, fd);
  
  // TODO: check that the server answer for client_port is correct
  
  result = rtsp_play(curl, uri, "0.000-", response);
  header = response->header_head.lh_first->next.le_next;
  tvhlog(LOG_DEBUG, "IPTV", "RTSP PLAY answer : %d, %s: %s", result, header->name, header->value);
  
  destroy_response(response);
  
  if(result != CURLE_OK)
  {
    tvhlog(LOG_ERR, "IPTV", "RTSP initialization failed for %s", uri);
    iptv_rtsp_stop(rtsp_info);
    return NULL;
  }
  
  // Init RTCP
  rtcp_create(rtsp_info);
  
  rtsp_info->is_initialized = 0;
  return rtsp_info;
}

void
iptv_rtsp_stop(iptv_rtsp_info_t *rtsp_info)
{
  if(rtsp_info->is_initialized)
  { 
    // BE�CAREFUL !
    // If the response isn't set to NULL, then you'll need to wait the command completion, otherwise the callback
    // can segfault when easy_cleanup is called
    rtsp_teardown(rtsp_info->curl, rtsp_info->uri, NULL);
    rtcp_destroy(rtsp_info);
  }
  curl_easy_cleanup(rtsp_info->curl);
  
  freeaddrinfo(rtsp_info->client_addr);
  freeaddrinfo(rtsp_info->server_addr);
  free(rtsp_info);
  rtsp_info = NULL;
}

int
iptv_init_rtsp()
{
  // Init cURL
  // TODO: call curl_global_cleanup at exit
  curl_global_init(CURL_GLOBAL_ALL);
  return 0;
}