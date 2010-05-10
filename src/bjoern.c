#include "bjoern.h"
#include "parsing.c"
#ifdef WANT_ROUTING
  #include "routing.c"
#endif

static PyMethodDef Bjoern_FunctionTable[] = {
    {"run", Bjoern_Run, METH_VARARGS, "Run bjoern. :-)"},
#ifdef WANT_ROUTING
    {"add_route", Bjoern_Route_Add, METH_VARARGS, "Add a URL route. :-)"},
#endif
    {NULL,  NULL, 0, NULL}
};

PyMODINIT_FUNC init_bjoern()
{
    #define INIT_PYSTRINGS
    #include "strings.h"
    #undef  INIT_PYSTRINGS

    Py_InitModule("_bjoern", Bjoern_FunctionTable);

#ifdef WANT_ROUTING
    import_re_module();
    init_routing();
#endif
}


PyObject* Bjoern_Run(PyObject* self, PyObject* args)
{
    const char* hostaddress;
    const int   port;

#ifdef WANT_ROUTING
    if(!PyArg_ParseTuple(args, "siO", &hostaddress, &port, &wsgi_layer))
        return NULL;
#else
    if(!PyArg_ParseTuple(args, "OsiO",
                         &wsgi_application, &hostaddress, &port, &wsgi_layer))
        return NULL;
    Py_INCREF(wsgi_application);
#endif

    Py_INCREF(wsgi_layer);

    sockfd = init_socket(hostaddress, port);
    if(sockfd < 0) return PyErr(PyExc_RuntimeError, SOCKET_ERROR_MESSAGES[-sockfd]);

    mainloop = ev_loop_new(0);

    /* Run the SIGINT-watch loop. */
    ev_signal sigint_watcher;
    ev_signal_init(&sigint_watcher, &on_sigint_received, SIGINT);
    ev_signal_start(mainloop, &sigint_watcher);


    /* Run the accept-watch loop. */
    ev_io accept_watcher;
    ev_io_init(&accept_watcher, on_sock_accept, sockfd, EV_READ);
    ev_io_start(mainloop, &accept_watcher);

    ev_loop(mainloop, 0);

    Py_RETURN_NONE;
}

static int
init_socket(const char* hostaddress, const int port)
{
    sockfd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(sockfd < 0) return SOCKET_FAILED;

    DEBUG("sockfd is %d", sockfd);

    union _sockaddress {
        struct sockaddr    sockaddress;
        struct sockaddr_in sockaddress_in;
    } server_address;

    server_address.sockaddress_in.sin_family = PF_INET;
    inet_aton(hostaddress, &server_address.sockaddress_in.sin_addr);
    server_address.sockaddress_in.sin_port = htons(port);

    /* Set SO_REUSEADDR to make the IP address we used available for reuse. */
    int optval = true;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    int st;
    st = bind(sockfd, &server_address.sockaddress, sizeof(server_address));
    if(st < 0) return BIND_FAILED;

    st = listen(sockfd, MAX_LISTEN_QUEUE_LENGTH);
    if(st < 0) return LISTEN_FAILED;

    return sockfd;
}


static ev_signal_callback
on_sigint_received(EV_LOOP* mainloop, ev_signal *signal, int revents)
{
    printf("\b\bReceived SIGINT, shutting down. Goodbye!\n");
    ev_unloop(mainloop, EVUNLOOP_ALL);
}


static Transaction* Transaction_new()
{
    Transaction* transaction = ALLOC(sizeof(Transaction));

    /* Allocate and initialize the http parser. */
    transaction->request_parser = ALLOC(sizeof(bjoern_http_parser));
    if(!transaction->request_parser) {
        free(transaction);
        return NULL;
    }
    transaction->request_parser->transaction = transaction;
    /* Reset the parser exit state */
    ((http_parser*)transaction->request_parser)->data = PARSER_OK;

    http_parser_init((http_parser*)transaction->request_parser, HTTP_REQUEST);

    return transaction;
}

static ev_io_callback
on_sock_accept(EV_LOOP* mainloop, ev_io* accept_watcher, int revents)
{
    Transaction* transaction = Transaction_new();

    /* Accept the socket connection. */
    struct sockaddr_in client_address;
    socklen_t client_address_length = sizeof(client_address);
    transaction->client_fd = accept(
        accept_watcher->fd,
        (struct sockaddr *)&client_address,
        &client_address_length
    );

    if(transaction->client_fd < 0) return; /* ERROR!1!1! */

    DEBUG("Accepted client on %d.", transaction->client_fd);

    /* Set the file descriptor non-blocking. */
    fcntl(transaction->client_fd, F_SETFL,
          MAX(fcntl(transaction->client_fd, F_GETFL, 0), 0) | O_NONBLOCK);

    /* Run the read-watch loop. */
    ev_io_init(&transaction->read_watcher, &on_sock_read, transaction->client_fd, EV_READ);
    ev_io_start(mainloop, &transaction->read_watcher);
}



/*
    TODO: Make sure this function is very, very fast as it is called many times.
*/
static ev_io_callback
on_sock_read(EV_LOOP* mainloop, ev_io* read_watcher_, int revents)
{
    /* Check whether we can still read. */
    if(!(revents & EV_READ))
        return;

    char    read_buffer[READ_BUFFER_SIZE];
    ssize_t bytes_read;
    size_t  bytes_parsed;

    Transaction* transaction = OFFSETOF(read_watcher, read_watcher_, Transaction);

    /* Read from the client: */
    bytes_read = read(transaction->client_fd, read_buffer, READ_BUFFER_SIZE);

    switch(bytes_read) {
        case  0: goto read_finished;
        case -1: return; /* An error happened. Try again next time libev calls
                            this function. TODO: Limit retries? */
        default:
            bytes_parsed = http_parser_execute(
                (http_parser*)transaction->request_parser,
                parser_settings,
                read_buffer,
                bytes_read
            );

            #define TRY_HANDLER(name) \
                    if(name##_handler_initialize(transaction)) {\
                        transaction->request_handler.writer = name##_handler_write; \
                        transaction->request_handler.finalizer = name##_handler_finalize; \
                        break; \
                    }

            switch(transaction->request_parser->exit_code) {
#ifdef WANT_CACHING
                /* If available, send a cached response. */
                case USE_CACHE:
                    TRY_HANDLER(cache);
#endif
                /* Send the response from the WSGI app */
                case PARSER_OK:
                    TRY_HANDLER(wsgi);

                /* Send a built-in HTTP 500 response. */
                case HTTP_INTERNAL_SERVER_ERROR:
                    if(PyErr_Occurred())
                        PyErr_Print();
                    transaction->request_handler.response = HTTP_500_MESSAGE;
                    TRY_HANDLER(raw);

                /* Send a built-in HTTP 404 response. */
                case HTTP_NOT_FOUND:
                    transaction->request_handler.response = HTTP_404_MESSAGE;
                    TRY_HANDLER(raw);

                default:
                    assert(0);
            }
    }

read_finished:
    /* Stop the read loop, we're done reading. */
    ev_io_stop(mainloop, &transaction->read_watcher);
    goto start_write;

start_write:
    ev_io_init(&transaction->write_watcher, &while_sock_canwrite,
               transaction->client_fd, EV_WRITE);
    ev_io_start(mainloop, &transaction->write_watcher);
}


/*
    Start (or continue) writing to the socket.
*/
static ev_io_callback
while_sock_canwrite(EV_LOOP* mainloop, ev_io* write_watcher_, int revents)
{
    Transaction* transaction = OFFSETOF(write_watcher, write_watcher_, Transaction);

    switch(transaction->request_handler.writer(transaction)) {
        case RESPONSE_NOT_YET_FINISHED:
            /* Please come around again. */
            return;
        case RESPONSE_FINISHED:
            goto finish;
        default:
            assert(0);
    }

finish:
    ev_io_stop(mainloop, &transaction->write_watcher);
    close(transaction->client_fd);
    transaction->request_handler.finalizer(transaction);
}
