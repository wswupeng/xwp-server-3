#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/sendfile.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "connection.h"
#include "module.h"
#include "typedef.h"
#include "buf.h"
#include "array.h"
#include "upstream.h"

#define eol(c) ((c) == '\n')
#define is_character(c) (((c)|0x20) >='a' && ((c)|0x20) <= 'z')
#define HTTP_PARSE_DONE 1
#define HTTP_PARSE_FAIL 0
#define HTTP_PARSE_AGAIN -1

static char* find_next_eol(char* line)
{
	char* p = line;
	
	while(!eol(*p) && *p!='\0') p++;

	if(*p == '\0')
		return NULL;
	else 
		return p;
}

static int connection_parse_url(connection_t* thiz, str_t* url)
{
	char* schema_start = NULL;
	char* schema_end = NULL;
	char* host_start = NULL;
	char* host_end = NULL;
	char* path_start = NULL;
	char* path_end = NULL;
	char* query_start = NULL;
	char* query_end = NULL;
	char* port_start = NULL;
	char* port_end = NULL;

	enum 
	{
		STAT_START = 0,
		STAT_SCHEMA,
		STAT_SCHEMA_SLASH,
		STAT_HOST,
		STAT_PORT,
		STAT_PATH,
		STAT_QUERY_STRING,
		/*STAT_FRAGMENT_ID,*/
		STAT_DONE,
	}state = STAT_START;

	char* p = url->data;
	char* url_end = url->data + url->len;
	for(; p<url_end && *p!='\0'; p++)
	{
		switch(state)
		{
			case STAT_START:
			{
				if(isspace(*p)) break;
				
				if(is_character(*p))
				{
					schema_start = p;
					state = STAT_SCHEMA;
					break;
				}
				else if(*p == '/')
				{
					path_start = p + 1;
					state = STAT_PATH;
					break;
				}
				else 
				{
					return HTTP_PARSE_FAIL;
				}
			}
			case STAT_SCHEMA:
			{
				if(is_character(*p)) break;

				if(*p == ':') 
				{
					schema_end = p;
					state = STAT_SCHEMA_SLASH;
					break;
				}
				else
				{
					return HTTP_PARSE_FAIL;
				}
			}
			case STAT_SCHEMA_SLASH:
			{
				if(*p == '/') break;
				else if(is_character(*p)) 
				{
					host_start = p;
					state = STAT_HOST;
					break;
				}
				else 
				{
					return HTTP_PARSE_FAIL;
				}
			}
			case STAT_HOST:
			{
				if(is_character(*p)) break;
				if(isdigit(*p) || *p == '.' || *p == '-')break;

				if(*p == ':') 
				{
					host_end = p;
					port_start = p + 1;
					state = STAT_PORT;
					break;
				}
				else if(*p == '/')
				{
					host_end = p;
					path_start = p + 1;
					state = STAT_PATH;
					break;
				}
				else
				{
					return HTTP_PARSE_FAIL;
				}
			}
			case STAT_PORT:
			{
				if(isdigit(*p)) break;
				if(*p == '/') 
				{
					port_end = p;
					path_start = p + 1;		
					state = STAT_PATH;
					break;
				}
				else if(isspace(*p))
				{
					port_end = p;
					state = STAT_DONE;
					break;
				}
				else
				{
					return HTTP_PARSE_FAIL;
				}
			}
			case STAT_PATH:
			{
				if(*p == '?') 
				{
					path_end = p;
					query_start = p + 1;
					state = STAT_QUERY_STRING;
					break;
				}
				else if(*p == '#')
				{
					//ignore the fragement id.
					path_end = p;
					state = STAT_DONE;
					break;
				}
				else if(isspace(*p))
				{
					path_end = p;
					state = STAT_DONE;
					break;
				}
				else 
				{
					break;
				}
			}
			case STAT_QUERY_STRING:
			{
				if(*p == '#' || isspace(*p))
				{
					query_end = p;
					state = STAT_DONE;
					break;
				}
				else
				{
					break;
				}
			}
			/*case STAT_FRAGMENT_ID:*/
			default:
			{
				assert(0);
			}
		}
		if(state == STAT_DONE) break;
	}

	if(schema_start)
	{
		if(!schema_end) return HTTP_PARSE_FAIL;
		*schema_end = '\0';

		thiz->r.url.schema.data = schema_start;//pool_strndup(pool, schema_start, schema_end-schema_start);
		thiz->r.url.schema.len = schema_end - schema_start;
	}
	if(host_start)
	{
		if(!host_end) return HTTP_PARSE_FAIL;
		*host_end = '\0';

		thiz->r.url.host.data = host_start;//pool_strndup(pool, host_start, host_end-host_start);
		thiz->r.url.host.len = host_end - host_start;
	}
	if(port_start)
	{
		if(!port_end) return HTTP_PARSE_FAIL;
		thiz->r.url.port = strtod(port_start, &port_end);
	}
	if(path_start)
	{
		if(!path_end) path_end = url_end;
		*path_end = '\0';
		
		thiz->r.url.path.data = path_start;
		thiz->r.url.path.len = path_end - path_start;
	}
	if(query_start)
	{
		if(!query_end) query_end = url_end;
		query_end = '\0';

		thiz->r.url.query_string.data = query_start;//pool_strndup(pool, query_start, query_end-query_start);
		thiz->r.url.query_string.len = query_end - query_start;
	}

	return HTTP_PARSE_DONE;
}

static int connection_parse_request_line(connection_t* thiz)
{
	str_t method = {0};
	str_t url = {0};
	str_t version = {0};
	char* pos = thiz->r.header_buf.pos;
	char* last = thiz->r.header_buf.last;
	char* eol = NULL;

	//skip empty line first.
	while(isspace(*pos) && pos<last && *pos!='\0') pos++;
	thiz->r.header_buf.pos = pos;

	eol = find_next_eol(pos);
	if(eol == NULL)
	{
		return HTTP_PARSE_AGAIN;
	}
	
	get_token(&method, &pos, isspace, NULL);
	get_token(&url, &pos, isspace, NULL);
	get_token(&version, &pos, isspace, NULL);
	thiz->r.header_buf.pos = eol + 1;

	if(method.data==NULL || url.data==NULL || version.data==NULL) return HTTP_PARSE_FAIL;

	thiz->r.version_str = version;
	version.data = strstr(version.data, "HTTP/");
	if(version.data == NULL) return HTTP_PARSE_FAIL;
	version.data += 5; //strlen("HTTP/")
	version.len = 3;

	if(strcmp(version.data, "1.0") == 0) thiz->r.version = HTTP_VERSION_10;
	else if(strcmp(version.data, "1.1") == 0) thiz->r.version = HTTP_VERSION_11;
	else return HTTP_PARSE_FAIL; //dont support 0.9

	if(strncasecmp(method.data, "GET", method.len) == 0) thiz->r.method = HTTP_METHOD_GET;
	else if(strncasecmp(method.data, "POST", method.len) == 0) thiz->r.method = HTTP_METHOD_POST;
	else if(strncasecmp(method.data, "HEAD", method.len) == 0) thiz->r.method = HTTP_METHOD_HEAD;
	else return HTTP_PARSE_FAIL; 

	return connection_parse_url(thiz, &url);
}

//FIXME  in one header may be multilines
static int connection_parse_header_line(connection_t* thiz)
{
	int ret = HTTP_PARSE_AGAIN;
	char* pos = thiz->r.header_buf.pos;
	char* last = thiz->r.header_buf.last;
	char* eol = NULL;
	str_t name = {0};
	str_t value = {0};

	while(pos<last)	
	{
		eol = find_next_eol(pos);

		//partial line.
		if(eol == NULL) break; 

		//empty line.
		if(eol == pos || (eol==pos+1 && *pos=='\r' && *eol=='\n' ))
		{
			pos = eol + 1;
			ret = HTTP_PARSE_DONE;
			break;
		}

		//full line
		//FIXME should trim space.
		get_token(&name, &pos, NULL, ":");
		get_token(&value, &pos, isspace, NULL);
		if(name.data == NULL) 
		{
			ret = HTTP_PARSE_FAIL;
			break;
		}

		if(!http_header_set(&(thiz->r.headers), &name, &value))
		{
			ret = HTTP_PARSE_FAIL;
			break;
		}

		pos = eol + 1;
	}

	thiz->r.header_buf.pos = pos;
	if(ret == HTTP_PARSE_DONE)
	{
		//TODO we should check some crucial headers like host.
		array_t* h = &(thiz->r.headers);

		thiz->r.host = http_header_str(h, HTTP_HEADER_HOST);
		if(thiz->r.host == NULL)
		{
			return HTTP_PARSE_FAIL;
		}

		thiz->r.content_len = http_header_int(h, HTTP_HEADER_CONTENT_LEN);
		if(thiz->r.content_len < 0) thiz->r.content_len = 0;

		const str_t* keep_alive = http_header_str(h, HTTP_HEADER_KEEPALIVE);
		if(keep_alive == NULL)
		{
			if(thiz->r.version == HTTP_VERSION_11)
				thiz->r.keep_alive = 1;
			else 
				thiz->r.keep_alive = 0;
		}
		else
		{
			if(strncasecmp(keep_alive->data, "CLOSE", keep_alive->len) == 0)
				thiz->r.keep_alive = 0;
			else 
				thiz->r.keep_alive = 1;
		}
	}

	return ret;
}

static int connection_alloc_large_header_buf(connection_t* thiz)
{
	buf_create(&(thiz->r.header_buf), thiz->r.pool, thiz->conf->large_client_header_size);

	if(thiz->r.header_buf.start == NULL) return 0;
	else return 1;
}

static void connection_gen_request(connection_t* thiz)
{
	enum 
	{
		STAT_PARSE_REQUEST_LINE = 0,
		STAT_PARSE_HEADER_LINE,
		STAT_PARSE_BODY,
		STAT_PARSE_DONE,
	}parse_state = STAT_PARSE_REQUEST_LINE;

	int parse_ret = HTTP_PARSE_AGAIN;

	array_init(&(thiz->r.headers), thiz->r.pool, 24);
	array_init(&(thiz->r.response.headers), thiz->r.pool, 24);

	buf_create(&(thiz->r.header_buf), thiz->r.pool, thiz->conf->client_header_size);
	if(thiz->r.header_buf.start == NULL) 
	{
		thiz->r.response.status = HTTP_STATUS_INTERNAL_SERVER_ERROR;
		return; 
	}

	buf_t* buf = &(thiz->r.header_buf);
	int count = 0;

	for(;;)
	{
		//meet buffer end but still parse incomplete.
		//the last character of buffer should always be zero.
		if(buf->last == buf->end-1) 
		{
			assert(parse_state <= STAT_PARSE_HEADER_LINE);
			if(!connection_alloc_large_header_buf(thiz))
			{
				thiz->r.response.status = HTTP_STATUS_INTERNAL_SERVER_ERROR;
				return;	
			}
		}

		count = recv(thiz->fd, buf->last, buf->end-1-buf->last, 0);
		if(count <= 0) 
		{
			thiz->r.response.status = HTTP_STATUS_CLOSE;
			break;
		}
		buf->last += count;

PARSE:
		switch(parse_state)
		{
			case STAT_PARSE_REQUEST_LINE:
				parse_ret = connection_parse_request_line(thiz);
				if(parse_ret == HTTP_PARSE_DONE) parse_state = STAT_PARSE_HEADER_LINE;
				break;
			case STAT_PARSE_HEADER_LINE:
				parse_ret = connection_parse_header_line(thiz);
				if(parse_ret == HTTP_PARSE_DONE) 
				{
					parse_state = STAT_PARSE_BODY;

					if(thiz->r.content_len > thiz->conf->max_content_len)
					{
						thiz->r.response.status = HTTP_STATUS_BAD_REQUEST;
						return;	
					}
					else if(thiz->r.content_len > buf->end-1 - buf->pos)
					{
						size_t rest = buf->last - buf->pos;
						buf_create(&(thiz->r.body_buf), thiz->r.pool, thiz->r.content_len + 1);
						if(thiz->r.body_buf.start == NULL) 
						{
							thiz->r.response.status = HTTP_STATUS_INTERNAL_SERVER_ERROR;
							return;	
						}
						if(rest > 0)
						{
							memcpy(thiz->r.body_buf.last, buf->pos, buf->last - buf->pos);
							thiz->r.body_buf.last += buf->last - buf->pos;
						}
						buf = &(thiz->r.body_buf);
					}
				}
				
				break;
			case STAT_PARSE_BODY:
				if(buf->last - buf->pos >= thiz->r.content_len)
				{
					parse_state = STAT_PARSE_DONE;
				}
				break;
			case STAT_PARSE_DONE:
			default:
				assert(0);
				break;
		}
		
		if(parse_state!=STAT_PARSE_DONE && 
			parse_ret==HTTP_PARSE_DONE)
		{
			goto PARSE;
		}
		else if(parse_ret==HTTP_PARSE_FAIL)
		{
			thiz->r.response.status = HTTP_STATUS_BAD_REQUEST;
			return;	
		}
		if(parse_state == STAT_PARSE_DONE)
		{
			thiz->r.response.status = HTTP_STATUS_OK;
			return;	
		}
	}

	return;
}

static size_t connection_response_header_len(connection_t* thiz)
{
	size_t new_line_len = 2;
	size_t len = 0;
	assert(thiz->r.version >= HTTP_VERSION_10);
	const str_t* status_line = http_status_line(thiz->r.response.status);
	
	len += sizeof("HTTP/1.x ") - 1;
	len += status_line->len;
	len += new_line_len; 

	http_header_t** headers = (http_header_t**) thiz->r.response.headers.elts;
	int header_count = thiz->r.response.headers.count;
	int i = 0; 
	for(; i<header_count; i++)
	{
		len += headers[i]->name.len;
		len += headers[i]->value.len;
		len += 4; //strlen(\r\n) + ': ' after name.
	}

	len += new_line_len;

	return len;
}

//success return 1 else return 0.
static int connection_send_response(connection_t* thiz)
{
	int count = 0;
	size_t num = 0;
	http_response_t* response = &thiz->r.response;
	http_header_t** headers = (http_header_t**) response->headers.elts;

	//TODO add some crucial headers such as: connection, date, server...
	if(thiz->r.keep_alive)
	{
		http_header_set(&response->headers, HTTP_HEADER_CONNECTION, HTTP_HEADER_KEEPALIVE);
	}
	else
	{
		http_header_set(&response->headers, HTTP_HEADER_CONNECTION, HTTP_HEADER_CLOSE);
	}

	http_header_set(&response->headers, HTTP_HEADER_SERVER, HTTP_HEADER_XWP_VER);

	if(thiz->r.response.content_len > 0)
	{
		char clen[64] = {0};
		int count = snprintf(clen, 64, "%d", thiz->r.response.content_len);
		str_t clen_str = {clen, count};
		http_header_set(&thiz->r.response.headers, HTTP_HEADER_CONTENT_LEN, &clen_str);
	}

	int header_count = response->headers.count;
	int header_len = connection_response_header_len(thiz);
	char* buf = pool_alloc(thiz->r.pool, header_len);
	const str_t* status_line = http_status_line(response->status);

	count = snprintf(buf, header_len, "%s %s\r\n", thiz->r.version_str.data, 
										  status_line->data);
	if(count <= 0) return 0;
	num += count;

	int i = 0;
	for(; i<header_count; i++)
	{
		count = snprintf(buf+num, header_len-num, "%s: %s\r\n", 
								  headers[i]->name.data, headers[i]->value.data);
		if(count <= 0) return 0;
		num += count;
	}

	buf[num] = '\r';
	buf[num+1] = '\n';
	num += 2;

	assert(num == header_len);

	if(!nwrite(thiz->fd, buf, num)) return 0;

	assert((response->content_fd>=0 && response->content_body==NULL)
			|| (response->content_fd<0 && response->content_body!=NULL));
	if(response->content_fd >= 0)
	{
		ssize_t ret = sendfile(thiz->fd, response->content_fd, NULL, response->content_len);
		close(response->content_fd);
		response->content_fd = -1;

		if(ret < 0) return 0;
	}
	else if(response->content_body != NULL)
	{
		if(!nwrite(thiz->fd, response->content_body, response->content_len)) 
			return 0;
	}

	return 1;
}

static void connection_process_request(connection_t* thiz)
{
	vhost_conf_t** vhosts = (vhost_conf_t** )thiz->conf->vhosts.elts;
	size_t vhost_nr = thiz->conf->vhosts.count;
	size_t i = 0;
	for(; i<vhost_nr; i++)
	{
		//TODO host or url host?
		if(strncasecmp(thiz->r.host->data, 
					   vhosts[i]->name.data, 
					   thiz->r.url.host.len) == 0)
		{
			break;
		}
	}

	//cannot find matched vhost.
	if(i == vhost_nr) 
	{
		thiz->r.response.status = HTTP_STATUS_BAD_REQUEST;
		return;
	}
		
	vhost_loc_conf_t** locs = (vhost_loc_conf_t** ) vhosts[i]->locs.elts;
	size_t loc_nr = vhosts[i]->locs.count;

	int k = 0;
	for(; k<loc_nr; k++)
	{
		if(regexec(&(locs[k]->pattern_regex), thiz->r.url.path.data, 0, NULL, 0) == 0)
			break;
	}

	//TODO there should be a default handler.
	//cannot find matched location.
	if(k == loc_nr) 
	{
		thiz->r.response.status = HTTP_STATUS_BAD_REQUEST;
		return;
	}
	
	int ret = HTTP_MODULE_PROCESS_DONE;
	ret = module_process(locs[k]->handler, &(thiz->r));

	if(ret == HTTP_MODULE_PROCESS_DONE)
	{
		return;
	}
	else if(ret == HTTP_MODULE_PROCESS_UPSTREAM)
	{
		//will modify the response status inside.
		upstream_process(thiz->r.upstream, &(thiz->r));
	}
	else
	{
		assert(0);
		thiz->r.response.status = HTTP_STATUS_INTERNAL_SERVER_ERROR;
		return;
	}

	return;
}

static int connection_finalize_request(connection_t* thiz)
{
	pthread_mutex_lock(&(thiz->mutex));

	if(thiz->r.upstream != NULL) 
		upstream_abort(thiz->r.upstream);
	thiz->r.upstream = NULL;
	
	pthread_mutex_unlock(&(thiz->mutex));

	pool_t* pool = thiz->r.pool;
	pool_reset(pool);
	bzero(&(thiz->r), sizeof(http_request_t));
	thiz->r.response.content_fd = -1;
	thiz->r.pool = pool;

	thiz->start_time = time(NULL);
	//TODO set new timer value.

	return 1;
}

static void connection_special_response(connection_t* thiz)
{
	//TODO maybe need to process by filter.
	http_response_t* response = &thiz->r.response;
	str_t* error_page = http_error_page(response->status, thiz->r.pool);
	
	assert(response->content_len == 0 && response->content_body == NULL);

	if(error_page != NULL)
	{
		response->content_len = error_page->len;
		response->content_body = error_page->data;
	}

	if(thiz->r.response.status >= HTTP_STATUS_BAD_REQUEST)
	{
		thiz->r.keep_alive = 0;	
	}

	return;
}

static int connection_reusable(connection_t* thiz, int reusable)
{
	assert(thiz!=NULL);	

	pthread_mutex_lock(&thiz->mutex);
	thiz->timedout = 0;
	if(thiz->fd >= 0)
	{
		close(thiz->fd);
		while(thiz->running) sleep(1);
		thiz->fd = -1;
	}

	thiz->start_time = 0;
	if(thiz->r.upstream != NULL) 
	{
		upstream_abort(thiz->r.upstream);
		while(thiz->running) sleep(1);
		thiz->r.upstream = NULL;
	}

	if(thiz->r.pool != NULL) pool_destroy(thiz->r.pool);
	bzero(&(thiz->r), sizeof(http_request_t));
	thiz->r.response.content_fd = -1;

	pthread_mutex_unlock(&thiz->mutex);
	
	if(!reusable)
		pthread_mutex_destroy(&thiz->mutex);

	return 1;
}


static void connection_close(void* ctx)
{
	connection_t* thiz = (connection_t* ) ctx;

	connection_reusable(thiz, 0);
}

connection_t* connection_create(pool_t* pool, conf_t* conf)
{
	assert(pool!=NULL && conf!=NULL);
	connection_t* thiz = pool_calloc(pool, sizeof(connection_t));
	if(thiz == NULL) return NULL;

	pool_add_cleanup(pool, connection_close, thiz);
	
	pthread_mutex_init(&thiz->mutex, NULL);
	thiz->pool = pool;
	thiz->conf = conf;
	thiz->fd = -1;
	thiz->r.response.content_fd = -1;

	return thiz;
}

int connection_run(connection_t* thiz, int fd)
{
	assert(thiz!=NULL && fd >= 0);
	int ret = 1;

	if(thiz->running) return 0;
	assert(thiz->r.pool == NULL && thiz->fd == -1);
	thiz->r.pool = pool_create(thiz->conf->request_pool_size);
	if(thiz->r.pool == NULL) return 0;
	thiz->start_time = time(NULL);
	thiz->fd = fd;
	
	thiz->running = 1;
	while(!thiz->timedout && thiz->running)
	{
		//only this thread are permitted to change it.
		assert(thiz->running); 

		connection_gen_request(thiz);
		if(thiz->r.response.status >= HTTP_STATUS_SPECIAL_RESPONSE)
		{
			goto DONE;
		}

		if(thiz->timedout) break;
		connection_process_request(thiz);
		if(thiz->timedout) break;

DONE:
		if(thiz->r.response.status == HTTP_STATUS_CLOSE) 
		{
			break;
		}
		else if(thiz->r.response.status >= HTTP_STATUS_SPECIAL_RESPONSE)
		{
			connection_special_response(thiz);
		}

		if(!connection_send_response(thiz)) break;

		if(thiz->r.keep_alive && !thiz->timedout)
		{
			connection_finalize_request(thiz);
			continue;
		}
		else
		{
			break;
		}
	}
	
	thiz->running = 0;
	connection_reusable(thiz, 1);

	return ret;
}

int connection_check_timeout(connection_t* thiz)
{
	assert(thiz != NULL);
	if(thiz->running == 0) return 1;

	time_t now = time(NULL);
	if(now-thiz->start_time >= thiz->conf->connection_timeout)
	{
		pthread_mutex_lock(&thiz->mutex);
		if(thiz->running)
		{
			thiz->timedout = 1;
			if(thiz->fd >= 0) 
			{
				//add this can speed up the return of connection_run thread.
				shutdown(thiz->fd, SHUT_RDWR);
				close(thiz->fd);
				while(thiz->running) sleep(1);
				thiz->fd = -1;
			}
			if(thiz->r.upstream != NULL) 
			{
				upstream_abort(thiz->r.upstream);
				while(thiz->running) sleep(1);
				thiz->r.upstream = NULL;
			}
		}
		pthread_mutex_unlock(&thiz->mutex);
	}

	return 1;
}


