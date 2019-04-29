#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#define MAXLEN 16384
#define MAXCONTENTLEN 4194304

int debug = 0, ipv4 = 0, ipv6 = 0, print_content = 0;
int wait_time = 5;
char check_string[MAXLEN];

struct timeval lasttv;
float dns_time, connect_time, first_time, end_time;
unsigned long int content_len;

void start_time(void)
{
	gettimeofday(&lasttv, NULL);
	if (debug)
		printf("time: %ld.%06ld\n", lasttv.tv_sec, lasttv.tv_usec);
}

float delta_time(void)
{
	float t;
	struct timeval tv;
	gettimeofday(&tv, NULL);
	if (debug)
		printf("time: %ld.%06ld\n", tv.tv_sec, tv.tv_usec);
	t = (tv.tv_sec - lasttv.tv_sec) + (tv.tv_usec - lasttv.tv_usec) / 1000000.0;
	lasttv = tv;
	return t;
}

SSL_CTX *InitCTX(void)
{
	const SSL_METHOD *method;
	SSL_CTX *ctx;

	OpenSSL_add_all_algorithms();	/* Load cryptos, et.al. */
	SSL_load_error_strings();	/* Bring in and register error messages */
	method = TLSv1_2_client_method();	/* Create new client-method instance */
	ctx = SSL_CTX_new(method);	/* Create new context */
	if (ctx == NULL) {
		ERR_print_errors_fp(stdout);
		abort();
	}
	return ctx;
}

int http_test(char *url)
{
	int sockfd = -1;
	int i = 0, n;
	int https = 0;
	char hostname[MAXLEN];
	char *uri = NULL, *p;
	struct addrinfo hints, *res;
	char buf[MAXCONTENTLEN];
	SSL_CTX *ctx;
	SSL *ssl;

	if (memcmp(url, "https://", 8) == 0) {
		p = url + 8;
		https = 1;
	} else if (memcmp(url, "http://", 7) != 0) {
		printf("only support http://\n");
		exit(-1);
	} else
		p = url + 7;
	hostname[0] = 0;
	if (*p == '[') {	// ipv6 addr
		p++;
		while (*p && (*p != ']') && i < MAXLEN - 1) {
			hostname[i] = *p;
			i++;
			p++;
		}
		if (*p != ']') {	// ipv6 add error 
			printf("url %s error\n", url);
			exit(-1);
		}
		p++;
		uri = p;
	} else {
		while (*p && (*p != '/') && i < MAXLEN - 1) {
			hostname[i] = *p;
			i++;
			p++;
		}
		uri = p;
	}

	if (uri[0] == 0)
		uri = "/";
	if (debug) {
		printf("url: %s\nhostname: %s uri: %s\n", url, hostname, uri);
		printf("begin dns lookup\n");
	}
	if (https) {
		SSL_library_init();
		ctx = InitCTX();
		ssl = SSL_new(ctx);
	}

	start_time();
	char *port;
	if (https)
		port = "443";
	else
		port = "80";
	if ((n = getaddrinfo(hostname, port, &hints, &res)) != 0) {
		printf("getaddrinfo error for %s\n", hostname);
		exit(-1);
	}
	dns_time = delta_time();
	if (debug) {
		printf("end dns lookup\n");
		printf("begin tcp connect\n");
	}

	do {
		int serport;
		char seraddr[INET6_ADDRSTRLEN];
		if (res->ai_family == AF_INET) {	// IPv4
			if (ipv6)
				continue;
			struct sockaddr_in *sinp;
			sinp = (struct sockaddr_in *)res->ai_addr;
			serport = ntohs(sinp->sin_port);
			inet_ntop(sinp->sin_family, &sinp->sin_addr, seraddr, INET6_ADDRSTRLEN);
		} else if (res->ai_family == AF_INET6) {
			if (ipv4)
				continue;
			struct sockaddr_in6 *sinp;
			sinp = (struct sockaddr_in6 *)res->ai_addr;
			serport = ntohs(sinp->sin6_port);
			inet_ntop(sinp->sin6_family, &sinp->sin6_addr, seraddr, INET6_ADDRSTRLEN);
		} else
			continue;
		if (debug) {
			printf("server address: %s\n", seraddr);
			printf("server port: %d\n", serport);
		}

		if ((sockfd = socket(res->ai_family, SOCK_STREAM, 0)) < 0) {
			printf("create socket failed: %s\n", strerror(errno));
			exit(-1);
		}

		struct timeval timeout;
		timeout.tv_sec = wait_time;
		timeout.tv_usec = 0;
		setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

		if (connect(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
			if (debug)
				printf("can't connect to %s:%d %s\n", seraddr, serport, strerror(errno));
			close(sockfd);
			sockfd = -1;
		} else {
			if (debug)
				printf("connect ok\n");
			break;
		}
	}
	while ((res = res->ai_next) != NULL);
	connect_time = delta_time();
	if (debug) {
		printf("end tcp connect\n");
		printf("begin http get request\n");
		printf("socketfd: %d\n", sockfd);
	}

	int flag = 1;
	setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

	if (https) {
		SSL_set_fd(ssl, sockfd);
		if (SSL_connect(ssl) == -1) {
			if (debug)
				ERR_print_errors_fp(stdout);
			exit(-1);
		}
	}

	snprintf(buf, MAXLEN, "GET %s HTTP/1.0\r\n" "Host: %s\r\n" "User-Agent: curl/7.29.0\r\n" "Connection: close\r\n\r\n", uri, hostname);
/*	if (debug)
		printf("send request: %s len=%d", buf, (int)strlen(buf));
*/
	if (https)
		n = SSL_write(ssl, buf, strlen(buf));
	else
		n = write(sockfd, buf, strlen(buf));
	if (debug)
		printf("end send request %d\n", n);
	if (https)
		n = SSL_read(ssl, buf, 12);
	else
		n = read(sockfd, buf, 12);
	first_time = delta_time();
	if (n <= 0) {
		if (debug)
			printf("get response: %d\n", n);
		exit(-1);
	}
/* HTTP/1.1 200 */
	if (memcmp(buf + 9, "200", 3) != 0) {
		buf[12] = 0;
		if (debug)
			printf("get response: %s\n", buf);
		exit(-1);
	}
	content_len = n;
	while (n < MAXCONTENTLEN) {
		if (https)
			n = SSL_read(ssl, buf + content_len, MAXCONTENTLEN - content_len);
		else
			n = read(sockfd, buf + content_len, MAXCONTENTLEN - content_len);
		if (n <= 0)
			break;
		content_len += n;
	}
	buf[content_len] = 0;
	end_time = delta_time();
	if (debug) {
		printf("read: %ld\n", content_len);
	}
	if (print_content)
		printf("%s\n", buf);

	printf("%.4f %.4f %.4f %.4f %.0f\n", dns_time, connect_time, first_time, end_time, (float)content_len / end_time);
	if (check_string[0]) {
		if (debug)
			printf("check string: %s\n", check_string);
		char *p = strstr(buf, check_string);
		if (p == NULL) {
			if (debug)
				printf("check string not found, return -1\n");
			exit(-1);
		}
	}
	if (debug)
		printf("All OK, return 0\n");
	exit(0);
}

void usage(void)
{
	printf("httptest: http get test\n\n");
	printf("  httptest  [ -d ] [ -4 ] [ -6 ] [ -p ] [ -w wait_time ] [ -r check_string ] url\n\n");
	printf("    -d               print debug message\n");
	printf("    -4               force ipv4\n");
	printf("    -6               force ipv6\n");
	printf("    -p               print response content\n");
	printf("    -w wait_time     max conntion time\n");
	printf("    -r check_string  check_string\n\n");
	printf("return 0 if get 200 response and match the check_string in response\n\n");
	printf("print dns_time tcp_connect_time response_time transfer_time transfer_rate\n");
	printf("             s                s             s             s        byte/s\n");
	exit(-1);
}

// httptest  [ -d ] [ -4 ] [ -6 ] [ -w wait_time ] [ -r check_string ] url 
// return 0 if got 200 response, and match the check_string
//   else return -1
// print dns_time tcp_connect_time response_time transfer_time transfer_rate
//              s                s             s             s        byte/s

int main(int argc, char *argv[])
{
	int c;
	while ((c = getopt(argc, argv, "d4p6w:r:h")) != EOF)
		switch (c) {
		case 'd':
			debug = 1;
			break;
		case '4':
			ipv4 = 1;
			break;
		case '6':
			ipv6 = 1;
			break;
		case 'p':
			print_content = 1;
			break;
		case 'w':
			wait_time = atoi(optarg);
			break;
		case 'r':
			strncpy(check_string, optarg, MAXLEN);
			break;
		case 'h':
			usage();
		};

	if (optind == argc - 1)
		http_test(argv[optind]);
	else
		usage();
	exit(-1);
}