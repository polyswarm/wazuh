/* Copyright (C) 2015-2020, Wazuh Inc.
 * Copyright (C) 2010 Trend Micro Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 *
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 *
 */

#include "shared.h"
#include "auth.h"
#include <pthread.h>
#include <sys/wait.h>
#include "check_cert.h"
#include "os_crypto/md5/md5_op.h"
#include "wazuhdb_op.h"
#include "os_err.h"

/* Prototypes */
static void help_authd(void) __attribute((noreturn));
static int ssl_error(const SSL *ssl, int ret);

/* Thread for dispatching connection pool */
static void* run_dispatcher(void *arg);
w_err_t w_auth_parse_data(char* buf, char *response, char *authpass, char *ip, char **agentname, char *groups);
w_err_t w_auth_validate_data (char *response, char *ip, char *agentname, char *groups);
w_err_t w_auth_add_agent(char *response, char *ip, char *agentname, char *groups, char **id, char **key);

/* Thread for writing keystore onto disk */
static void* run_writer(void *arg);

/* Signal handler */
static void handler(int signum);

/* Exit handler */
static void cleanup();

/* Shared variables */
static char *authpass = NULL;
static SSL_CTX *ctx;
static int remote_sock = -1;

char shost[512];
authd_config_t config;
keystore keys;

/* client queue */
static w_queue_t *client_queue = NULL;

volatile int write_pending = 0;
volatile int running = 1;
static struct keynode *queue_insert = NULL;
static struct keynode *queue_backup = NULL;
static struct keynode *queue_remove = NULL;
static struct keynode * volatile *insert_tail;
static struct keynode * volatile *backup_tail;
static struct keynode * volatile *remove_tail;

pthread_mutex_t mutex_keys = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond_pending = PTHREAD_COND_INITIALIZER;

/*authd working as worker on cluester*/
bool worker_node;

/* Print help statement */
static void help_authd()
{
    print_header();
    print_out("  %s: -[Vhdtfi] [-g group] [-D dir] [-p port] [-P] [-c ciphers] [-v path [-s]] [-x path] [-k path]", ARGV0);
    print_out("    -V          Version and license message.");
    print_out("    -h          This help message.");
    print_out("    -d          Debug mode. Use this parameter multiple times to increase the debug level.");
    print_out("    -t          Test configuration.");
    print_out("    -f          Run in foreground.");
    print_out("    -g <group>  Group to run as. Default: %s.", GROUPGLOBAL);
    print_out("    -D <dir>    Directory to chroot into. Default: %s.", DEFAULTDIR);
    print_out("    -p <port>   Manager port. Default: %d.", DEFAULT_PORT);
    print_out("    -P          Enable shared password authentication, at %s or random.", AUTHDPASS_PATH);
    print_out("    -c          SSL cipher list (default: %s)", DEFAULT_CIPHERS);
    print_out("    -v <path>   Full path to CA certificate used to verify clients.");
    print_out("    -s          Used with -v, enable source host verification.");
    print_out("    -x <path>   Full path to server certificate. Default: %s%s.", DEFAULTDIR, CERTFILE);
    print_out("    -k <path>   Full path to server key. Default: %s%s.", DEFAULTDIR, KEYFILE);
    print_out("    -a          Auto select SSL/TLS method. Default: TLS v1.2 only.");
    print_out("    -L          Force insertion though agent limit reached.");
    print_out(" ");
    exit(1);
}

/* Generates a random and temporary shared pass to be used by the agents. */
char *__generatetmppass()
{
    int rand1;
    int rand2;
    char *rand3;
    char *rand4;
    os_md5 md1;
    os_md5 md3;
    os_md5 md4;
    char *fstring = NULL;
    char str1[STR_SIZE +1];

    rand1 = os_random();
    rand2 = os_random();

    rand3 = GetRandomNoise();
    rand4 = GetRandomNoise();

    OS_MD5_Str(rand3, -1, md3);
    OS_MD5_Str(rand4, -1, md4);

    snprintf(str1, STR_SIZE, "%d%d%s%d%s%s",(int)time(0), rand1, getuname(), rand2, md3, md4);
    OS_MD5_Str(str1, -1, md1);
    fstring = strdup(md1);
    free(rand3);
    free(rand4);
    return(fstring);
}

/* Function to use with SSL on non blocking socket,
 * to know if SSL operation failed for good
 */
static int ssl_error(const SSL *ssl, int ret)
{
    if (ret <= 0) {
        switch (SSL_get_error(ssl, ret)) {
            case SSL_ERROR_WANT_READ:
            case SSL_ERROR_WANT_WRITE:
                usleep(100 * 1000);
                return (0);
            default:
                ERR_print_errors_fp(stderr);
                return (1);
        }
    }

    return (0);
}

int main(int argc, char **argv)
{
    FILE *fp;
    /* Count of pids we are wait()ing on */
    int debug_level = 0;
    int test_config = 0;
    int status;
    int run_foreground = 0;
    gid_t gid;
    int client_sock = 0;
    const char *dir  = DEFAULTDIR;
    const char *group = GROUPGLOBAL;
    char buf[4096 + 1];
    struct sockaddr_in _nc;
    struct timeval timeout;
    socklen_t _ncl;
    pthread_t thread_dispatcher;
    pthread_t thread_writer;
    pthread_t thread_local_server;
    fd_set fdset;

    /* Initialize some variables */
    bio_err = 0;

    /* Set the name */
    OS_SetName(ARGV0);

    // Get options

    {
        int c;
        int use_pass = 0;
        int auto_method = 0;
        int validate_host = 0;
        int no_limit = 0;
        const char *ciphers = NULL;
        const char *ca_cert = NULL;
        const char *server_cert = NULL;
        const char *server_key = NULL;
        unsigned short port = 0;

        while (c = getopt(argc, argv, "Vdhtfig:D:p:c:v:sx:k:PF:ar:L"), c != -1) {
            switch (c) {
                case 'V':
                    print_version();
                    break;

                case 'h':
                    help_authd();
                    break;

                case 'd':
                    debug_level = 1;
                    nowDebug();
                    break;

                case 'i':
                    mwarn(DEPRECATED_OPTION_WARN,"-i");
                    break;

                case 'g':
                    if (!optarg) {
                        merror_exit("-g needs an argument");
                    }
                    group = optarg;
                    break;

                case 'D':
                    if (!optarg) {
                        merror_exit("-D needs an argument");
                    }
                    dir = optarg;
                    break;

                case 't':
                    test_config = 1;
                    break;

                case 'f':
                    run_foreground = 1;
                    break;

                case 'P':
                    use_pass = 1;
                    break;

                case 'p':
                    if (!optarg) {
                        merror_exit("-%c needs an argument", c);
                    }

                    if (port = (unsigned short)atoi(optarg), port == 0) {
                        merror_exit("Invalid port: %s", optarg);
                    }
                    break;

                case 'c':
                    if (!optarg) {
                        merror_exit("-%c needs an argument", c);
                    }
                    ciphers = optarg;
                    break;

                case 'v':
                    if (!optarg) {
                        merror_exit("-%c needs an argument", c);
                    }
                    ca_cert = optarg;
                    break;

                case 's':
                    validate_host = 1;
                    break;

                case 'x':
                    if (!optarg) {
                        merror_exit("-%c needs an argument", c);
                    }
                    server_cert = optarg;
                    break;

                case 'k':
                    if (!optarg) {
                        merror_exit("-%c needs an argument", c);
                    }
                    server_key = optarg;
                    break;

                case 'F':
                    mwarn(DEPRECATED_OPTION_WARN,"-F");
                    break;

                case 'r':
                    mwarn(DEPRECATED_OPTION_WARN,"-r");
                    break;

                case 'a':
                    auto_method = 1;
                    break;

                case 'L':
                    no_limit = 1;
                    break;

                default:
                    help_authd();
                    break;
            }
        }

        // Return -1 if not configured
        if (authd_read_config(DEFAULTCPATH) < 0) {
            merror_exit(CONFIG_ERROR, DEFAULTCPATH);
        }

        // Overwrite arguments

        if (use_pass) {
            config.flags.use_password = 1;
        }

        if (auto_method) {
            config.flags.auto_negotiate = 1;
        }

        if (validate_host) {
            config.flags.verify_host = 1;
        }

        if (run_foreground) {
            config.flags.disabled = 0;
        }

        if (ciphers) {
            free(config.ciphers);
            config.ciphers = strdup(ciphers);
        }

        if (ca_cert) {
            free(config.agent_ca);
            config.agent_ca = strdup(ca_cert);
        }

        if (server_cert) {
            free(config.manager_cert);
            config.manager_cert = strdup(server_cert);
        }

        if (server_key) {
            free(config.manager_key);
            config.manager_key = strdup(server_key);
        }

        if (port) {
            config.port = port;
        }

        if (no_limit) {
            config.flags.register_limit = 0;
        }
    }

    /* Exit here if test config is set */
    if (test_config) {
        exit(0);
    }

    /* Exit here if disabled */
    if (config.flags.disabled) {
        minfo("Daemon is disabled. Closing.");
        exit(0);
    }

    if (debug_level == 0) {
        /* Get debug level */
        debug_level = getDefine_Int("authd", "debug", 0, 2);
        while (debug_level != 0) {
            nowDebug();
            debug_level--;
        }
    }

    switch(w_is_worker()){
    case -1:
        merror("Invalid option at cluster configuration");
        exit(0);
    case 1:
        worker_node = TRUE;        
        break;
    case 0:
        worker_node = FALSE;
        break;
    }

    /* Start daemon -- NB: need to double fork and setsid */
    mdebug1(STARTED_MSG);

    /* Check if the user/group given are valid */
    gid = Privsep_GetGroup(group);
    if (gid == (gid_t) - 1) {
        merror_exit(USER_ERROR, "", group);
    }

    if (!run_foreground) {
        nowDaemon();
        goDaemon();
    }

    /* Privilege separation */
    if (Privsep_SetGroup(gid) < 0) {
        merror_exit(SETGID_ERROR, group, errno, strerror(errno));
    }

    /* chroot -- TODO: this isn't a chroot. Should also close
     * unneeded open file descriptors (like stdin/stdout)
     */
    if (chdir(dir) == -1) {
        merror_exit(CHDIR_ERROR, dir, errno, strerror(errno));
    }

    /* Signal manipulation */

    {
        struct sigaction action = { .sa_handler = handler, .sa_flags = SA_RESTART };
        sigaction(SIGTERM, &action, NULL);
        sigaction(SIGHUP, &action, NULL);
        sigaction(SIGINT, &action, NULL);
    }

    /* Start up message */
    minfo(STARTUP_MSG, (int)getpid());

    if (config.flags.use_password) {

        /* Checking if there is a custom password file */
        fp = fopen(AUTHDPASS_PATH, "r");
        buf[0] = '\0';
        if (fp) {
            buf[4096] = '\0';
            char *ret = fgets(buf, 4095, fp);

            if (ret && strlen(buf) > 2) {
                /* Remove newline */
                if (buf[strlen(buf) - 1] == '\n')
                    buf[strlen(buf) - 1] = '\0';

                authpass = strdup(buf);
            }

            fclose(fp);
        }

        if (buf[0] != '\0')
            minfo("Accepting connections on port %hu. Using password specified on file: %s", config.port, AUTHDPASS_PATH);
        else {
            /* Getting temporary pass. */
            authpass = __generatetmppass();
            minfo("Accepting connections on port %hu. Random password chosen for agent authentication: %s", config.port, authpass);
        }
    } else
        minfo("Accepting connections on port %hu. No password required.", config.port);

    /* Getting SSL cert. */

    fp = fopen(KEYSFILE_PATH, "a");
    if (!fp) {
        merror("Unable to open %s (key file)", KEYSFILE_PATH);
        exit(1);
    }
    fclose(fp);
    
    /* Start SSL */
    ctx = os_ssl_keys(1, dir, config.ciphers, config.manager_cert, config.manager_key, config.agent_ca, config.flags.auto_negotiate);
    if (!ctx) {
        merror("SSL error. Exiting.");
        exit(1);
    }

    /* Connect via TCP */
    remote_sock = OS_Bindporttcp(config.port, NULL, 0);
    if (remote_sock <= 0) {
        merror(BIND_ERROR, config.port, errno, strerror(errno));
        exit(1);
    }

    /* Before chroot */
    srandom_init();
    getuname();

    if (gethostname(shost, sizeof(shost) - 1) < 0) {
        strncpy(shost, "localhost", sizeof(shost) - 1);
        shost[sizeof(shost) - 1] = '\0';
    }

    /* Load ossec uid and gid for creating backups */
    if (OS_LoadUid() < 0) {
        merror_exit("Couldn't get user and group id.");
    }

    /* Chroot */
    if (Privsep_Chroot(dir) < 0)
        merror_exit(CHROOT_ERROR, dir, errno, strerror(errno));

    nowChroot();

    if (config.timeout_sec || config.timeout_usec) {
        minfo("Setting network timeout to %.6f sec.", config.timeout_sec + config.timeout_usec / 1000000.);
    } else {
        mdebug1("Network timeout is disabled.");
    }

    /* Initialize queues */
    client_queue = queue_init(AUTH_POOL);
    insert_tail = &queue_insert;
    backup_tail = &queue_backup;
    remove_tail = &queue_remove;

    /* Start working threads */

    status = pthread_create(&thread_dispatcher, NULL, run_dispatcher, NULL);

    if (status != 0) {
        merror("Couldn't create thread: %s", strerror(status));
        return EXIT_FAILURE;
    }

    if (!worker_node) {
        status = pthread_create(&thread_writer, NULL, run_writer, NULL);

        if (status != 0) {
            merror("Couldn't create thread: %s", strerror(status));
            return EXIT_FAILURE;
        }

        if (status = pthread_create(&thread_local_server, NULL, run_local_server, NULL), status != 0) {
            merror("Couldn't create thread: %s", strerror(status));
            return EXIT_FAILURE;
        }
    }

    /* Create PID files */
    if (CreatePID(ARGV0, getpid()) < 0) {
        merror_exit(PID_ERROR);
    }

    atexit(cleanup);

    /* Main loop */

    while (running) {
        memset(&_nc, 0, sizeof(_nc));
        _ncl = sizeof(_nc);

        // Wait for socket
        FD_ZERO(&fdset);
        FD_SET(remote_sock, &fdset);
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        switch (select(remote_sock + 1, &fdset, NULL, NULL, &timeout)) {
        case -1:
            if (errno != EINTR) {
                merror_exit("at main(): select(): %s", strerror(errno));
            }

            continue;

        case 0:
            continue;
        }

        if ((client_sock = accept(remote_sock, (struct sockaddr *) &_nc, &_ncl)) > 0) {
            if (config.timeout_sec || config.timeout_usec) {
                if (OS_SetRecvTimeout(client_sock, config.timeout_sec, config.timeout_usec) < 0) {
                    static int reported = 0;

                    if (!reported) {
                        int error = errno;
                        merror("Could not set timeout to network socket: %s (%d)", strerror(error), error);
                        reported = 1;
                    }
                }
            }
            struct client *new_client;
            os_malloc(sizeof(struct client), new_client);
            new_client->socket = client_sock;
            new_client->addr = _nc.sin_addr;

            if( queue_push_ex(client_queue, new_client) == -1) {
                merror("Too many connections. Rejecting.");
                close(client_sock);
            }
        } else if ((errno == EBADF && running) || (errno != EBADF && errno != EINTR))
            merror("at main(): accept(): %s", strerror(errno));
    }

    close(remote_sock);

    /* Join threads */

    //Unblock other thread in case queue is empty
    queue_pop_ex(client_queue);

    w_mutex_lock(&mutex_keys);
    w_cond_signal(&cond_pending);
    w_mutex_unlock(&mutex_keys);

    
    pthread_join(thread_dispatcher, NULL);
    if (!worker_node) {
        pthread_join(thread_writer, NULL);
        pthread_join(thread_local_server, NULL);
    }

    queue_free(client_queue);
    minfo("Exiting...");
    return (0);
}

/* Thread for dispatching connection pool */
void* run_dispatcher(__attribute__((unused)) void *arg) {
    struct client *client;
    char ip[IPSIZE + 1];    
    int ret;
    char* buf = NULL;
    SSL *ssl;
    char response[2048];
    response[2047] = '\0';

    authd_sigblock();

    /* Initialize some variables */
    memset(ip, '\0', IPSIZE + 1);
    
    if (!worker_node) {
        OS_PassEmptyKeyfile();
        OS_ReadKeys(&keys, 0, !config.flags.clear_removed, 1);
    }
    mdebug1("Dispatch thread ready");

    while (running) {
        client = queue_pop_ex(client_queue);

        if (!running)
            break;

        strncpy(ip, inet_ntoa(client->addr), IPSIZE - 1);
        ssl = SSL_new(ctx);
        SSL_set_fd(ssl, client->socket);
        ret = SSL_accept(ssl);

        if (ssl_error(ssl, ret)) {
            mdebug1("SSL Error (%d)", ret);
            SSL_free(ssl);
            close(client->socket);
            continue;
        }

        minfo("New connection from %s", ip);

        /* Additional verification of the agent's certificate. */

        if (config.flags.verify_host && config.agent_ca) {
            if (check_x509_cert(ssl, ip) != VERIFY_TRUE) {
                merror("Unable to verify client certificate.");
                SSL_free(ssl);
                close(client->socket);
                continue;
            }
        }

        os_calloc(OS_SIZE_65536 + OS_SIZE_4096 + 1, sizeof(char), buf);
        buf[0] = '\0'; 
        ret = SSL_read(ssl, buf, OS_SIZE_65536 + OS_SIZE_4096);
        if (ret <= 0) {
            switch (ssl_error(ssl, ret)) {
            case 0:
                minfo("Client timeout from %s", ip);
                break;
            default:
                merror("SSL Error (%d)", ret);
            }
            SSL_free(ssl);
            close(client->socket);
            free(buf);
            continue;
        }      
        buf[ret] = '\0';  
             
        mdebug2("Request received: <%s>", buf);
        bool enrollment_ok = FALSE;
        char *agentname = NULL;
        char centralized_group[OS_SIZE_65536] = {0};
        char* new_id = NULL;
        char* new_key = NULL;
        if(OS_SUCCESS == w_auth_parse_data(buf, response, authpass, ip, &agentname, centralized_group)){
            if (worker_node) {
                //JJP: Using worker config (force). Is this correct?
                if( 0 == w_request_agent_add_clustered(new_id, agentname, ip, centralized_group, new_key, config.flags.force_insert?config.force_time:0, TRUE, NULL) ) {
                    enrollment_ok = TRUE;
                }                
            }
            else {
                //JJP: These mutex can be more atomic
                w_mutex_lock(&mutex_keys);                
                if(OS_SUCCESS == w_auth_validate_data(response, ip, agentname, centralized_group)){
                    if(OS_SUCCESS == w_auth_add_agent(response, ip, agentname, centralized_group, &new_id, &new_key)){
                        enrollment_ok = TRUE;
                    }
                }
                w_mutex_unlock(&mutex_keys); 
            }
        }

        if(enrollment_ok)
        {
            snprintf(response, 2048, "OSSEC K:'%s %s %s %s'\n\n", new_id, agentname,  ip, new_key);
            minfo("Agent key generated for '%s' (requested by %s)", agentname, ip);
            ret = SSL_write(ssl, response, strlen(response));

            if (worker_node) {
                if (ret < 0) {
                    merror("SSL write error (%d)", ret);
                    
                    ERR_print_errors_fp(stderr); 
                    int master_err = 0;                   
                    if (0 != w_request_agent_remove_clustered(&master_err, new_id, 1, TRUE) || master_err != 0) {
                        //JJP: Is it needed to unchain a problem handler here?
                        merror("Agent key unable to share with %s and unable to delete from master node", agentname);
                    }
                    else {
                        merror("Agent key not saved for %s", agentname);
                    }
                }
            }
            else{     
                if (ret < 0) {
                    merror("SSL write error (%d)", ret);
                    merror("Agent key not saved for %s", agentname);
                    ERR_print_errors_fp(stderr);
                    OS_DeleteKey(&keys, keys.keyentries[keys.keysize - 1]->id, 1);
                } else {
                    /* Add pending key to write */
                    add_insert(keys.keyentries[keys.keysize - 1], *centralized_group ? centralized_group : NULL);
                    write_pending = 1;
                    w_cond_signal(&cond_pending);
                }
            }  
        }
        else {
            SSL_write(ssl, response, strlen(response));
            snprintf(response, 2048, "ERROR: Unable to add agent.\n\n");
            SSL_write(ssl, response, strlen(response));  
        }
            
        SSL_free(ssl);
        close(client->socket);   
        os_free(client);
        free(buf);
    }

    SSL_CTX_free(ctx);
    mdebug1("Dispatch thread finished");
    return NULL;
}

w_err_t w_auth_parse_data(char* buf, char *response, char *authpass, char *ip, char **agentname, char *groups){
    
    bool parseok = FALSE; 
    //JJP: I prefer allocating space for agentname instead of recyclying the one in buffer. This will avoid handling buffer until end.
    // Also, I can create a struct with all info required. 
    //JJP: Bring Different return values  
    //JJP: parseok can be avoided 
    
    /* Checking for shared password authentication. */
    if(authpass) {
        /* Format is pretty simple: OSSEC PASS: PASS WHATEVERACTION */
        parseok = FALSE; 
        if (strncmp(buf, "OSSEC PASS: ", 12) == 0) {
            buf += 12;

            if (strlen(buf) > strlen(authpass) && strncmp(buf, authpass, strlen(authpass)) == 0) {
                buf += strlen(authpass);

                if (*buf == ' ') {
                    buf++;
                    parseok = 1;
                }
            }
        }

        if (parseok == 0) {
            merror("Invalid password provided by %s. Closing connection.", ip);
            //JJP: Is it ok to add a new message here?
            snprintf(response, 2048, "ERROR: Invalid password");  
            return OS_INVALID;
        }
    }
    

    /* Checking for action A (add agent) */    
    parseok = FALSE;
    if (strncmp(buf, "OSSEC A:'", 9) == 0) {
        *agentname = buf + 9;
        buf += 9;
        while (*buf != '\0') {
            if (*buf == '\'') {
                *buf = '\0';
                minfo("Received request for a new agent (%s) from: %s", *agentname, ip);
                parseok = TRUE;
                break;
            }
            buf++;
        }
    }
    buf++;
    
    if (!parseok) {
        merror("Invalid request for new agent from: %s", ip);
        //JJP: Is it ok to add a new message here?
        snprintf(response, 2048, "ERROR: Invalid request for new agent");  
        return OS_INVALID;
    }    


    if (!OS_IsValidName(*agentname)) {
        merror("Invalid agent name: %s from %s", *agentname, ip);
        snprintf(response, 2048, "ERROR: Invalid agent name: %s\n\n", *agentname);  
        return OS_INVALID;
    }

    /* Check for valid centralized group */
    
    char centralized_group_token[2] = "G:";

    if(strncmp(++buf,centralized_group_token,2)==0)
    {
        sscanf(buf," G:\'%65535[^\']\"",groups);

        /* Validate the group name */
        int valid = 0;
        valid = w_validate_group_name(groups);

        if(valid < 0) {

            merror("Invalid group name: %.255s... ,",groups);

            switch (valid) {
                case -6:
                    snprintf(response, 2048, "ERROR: Invalid group name: %.255s... cannot start or end with ','\n\n", groups);
                    break;
                case -5:
                    snprintf(response, 2048, "ERROR: Invalid group name: %.255s... consecutive ',' are not allowed \n\n, ", groups);
                    break;
                case -4:
                    snprintf(response, 2048, "ERROR: Invalid group name: %.255s... white spaces are not allowed \n\n", groups);
                    break;
                case -3:
                    snprintf(response, 2048, "ERROR: Invalid group name: %.255s... multigroup is too large \n\n", groups);
                    break;
                case -2:
                    snprintf(response, 2048, "ERROR: Invalid group name: %.255s... group is too large\n\n", groups);
                    break;
                case -1:
                    snprintf(response, 2048, "ERROR: Invalid group name: %.255s... characters '\\/:*?\"<>|,' are prohibited\n\n", groups);
                    break;
            }               
            return OS_INVALID;
        }
        char *tmp_groups = wstr_delete_repeated_groups(groups);
        if(!tmp_groups){
            return OS_MEMERR;
        }
        mdebug1("Group(s) is: %s",tmp_groups);
        
        /*Forward the string pointer G:'........' 2 for G:, 2 for ''*/
        buf+= 2+strlen(groups)+2;

        snprintf(groups,OS_SIZE_65536,"%s",tmp_groups);
        os_free(tmp_groups);
        
    }else{
        buf--;
    }

    /* Check for IP when client uses -i option */
    
    char client_source_ip[IPSIZE + 1] = {0};
    char client_source_ip_token[3] = "IP:";
    
    if(strncmp(++buf,client_source_ip_token,3)==0) {
        char format[15];
        sprintf(format, " IP:\'%%%d[^\']\"", IPSIZE);
        sscanf(buf, format ,client_source_ip);

        /* If IP: != 'src' overwrite the provided ip */
        if(strncmp(client_source_ip,"src",3) != 0)
        {
            if (!OS_IsValidIP(client_source_ip, NULL)) {
                merror("Invalid IP: '%s'", client_source_ip);
                snprintf(response, 2048, "ERROR: Invalid IP: %s\n\n", client_source_ip);                    
                return OS_INVALID;
            }
            snprintf(ip, IPSIZE, "%s", client_source_ip);
        }
        
    } 
    else if(!config.flags.use_source_ip) {
        // use_source-ip = 0 and no -I argument in agent
        snprintf(ip, IPSIZE, "any");
    }
    // else -> agent IP is already on ip         
   
    return OS_SUCCESS;
}

w_err_t w_auth_validate_data (char *response, char *ip, char *agentname, char *groups){
    //JJP: Split into validate_groups, validate_name, validate_ip
    /* Validate the group(s) name(s) */     
    int index = 0;
    char *id_exist = NULL;
    double antiquity = 0; 
    int acount = 2;
    char fname[2048];
    fname[2047] = '\0';

    if (*groups){ 
        if (OS_SUCCESS != w_auth_validate_groups(groups, response)){
            return OS_INVALID;
        }        
    }

    /* Check for duplicated IP */
    if (strcmp(ip, "any") != 0 ) {
        if (index = OS_IsAllowedIP(&keys, ip), index >= 0) {
            if (config.flags.force_insert && (antiquity = OS_AgentAntiquity(keys.keyentries[index]->name, keys.keyentries[index]->ip->ip), antiquity >= config.force_time || antiquity < 0)) {
                id_exist = keys.keyentries[index]->id;
                minfo("Duplicated IP '%s' (%s). Saving backup.", ip, id_exist);
                
                OS_RemoveAgentGroup(id_exist);
                add_backup(keys.keyentries[index]);
                OS_DeleteKey(&keys, id_exist, 0);
            } else {
                merror("Duplicated IP %s", ip);
                snprintf(response, 2048, "ERROR: Duplicated IP: %s\n\n", ip);
                return OS_INVALID;
            }
        }
    }
    
    /* Check whether the agent name is the same as the manager */

    if (!strcmp(agentname, shost)) {
        merror("Invalid agent name %s (same as manager)", agentname);
        snprintf(response, 2048, "ERROR: Invalid agent name: %s\n\n", agentname);
        return OS_INVALID;
    }

    /* Check for duplicated names */

    if (index = OS_IsAllowedName(&keys, agentname), index >= 0) {
        if (config.flags.force_insert && (antiquity = OS_AgentAntiquity(keys.keyentries[index]->name, keys.keyentries[index]->ip->ip), antiquity >= config.force_time || antiquity < 0)) {
            id_exist = keys.keyentries[index]->id;
            minfo("Duplicated name '%s' (%s). Saving backup.", agentname, id_exist);

            add_backup(keys.keyentries[index]);
            OS_DeleteKey(&keys, id_exist, 0);
        } else {
            strncpy(fname, agentname, 2048);

            while (OS_IsAllowedName(&keys, fname) >= 0) {
                snprintf(fname, 2048, "%s%d", agentname, acount);

                if (++acount > MAX_TAG_COUNTER)
                    break;
            }

            if (acount > MAX_TAG_COUNTER) {
                merror("Invalid agent name %s (duplicated)", agentname);
                snprintf(response, 2048, "ERROR: Invalid agent name: %s\n\n", agentname);                
                return OS_INVALID;
            }

            agentname = fname;
        }
    }

    /* Check for agents limit */

    if (config.flags.register_limit && keys.keysize >= (MAX_AGENTS - 2) ) {
        merror(AG_MAX_ERROR, MAX_AGENTS - 2);
        snprintf(response, 2048, "ERROR: The maximum number of agents has been reached\n\n");
        return OS_INVALID;
    }

    return OS_SUCCESS;
}

w_err_t w_auth_validate_groups(char *groups, char *response) {
    int max_multigroups = 0;    
    char *save_ptr = NULL;
    const char delim[] = {MULTIGROUP_SEPARATOR,'\0'};    
  
    char *group = strtok_r(groups, delim, &save_ptr);    

    while( group != NULL ) {
        DIR * dp;
        char dir[PATH_MAX + 1] = {0};        

        /* Check limit */
        if(max_multigroups > MAX_GROUPS_PER_MULTIGROUP){
            merror("Maximum multigroup reached: Limit is %d",MAX_GROUPS_PER_MULTIGROUP);                
            return OS_INVALID;
        }
        /* Validate the group name */
        if( 0 != w_validate_group_name(group) ){ 
            merror("Invalid group name: %.255s... ,",group); 
            if (response){
                snprintf(response, 2048, "ERROR: Invalid group name: %.255s... group is too large\n\n", group); 
            }                                   
            return OS_INVALID;
        }       

        snprintf(dir, PATH_MAX + 1,isChroot() ? SHAREDCFG_DIR"/%s" : DEFAULTDIR SHAREDCFG_DIR"/%s", group);
        dp = opendir(dir);
        if (!dp) {            
            merror("Invalid group: %.255s",group);   
            if (response){
                snprintf(response, 2048, "ERROR: Invalid group: %s\n\n", group);
            }             
            return OS_INVALID;
        }

        group = strtok_r(NULL, delim, &save_ptr);
        max_multigroups++;
        closedir(dp);
    }       
   
    return OS_SUCCESS;
}

w_err_t w_auth_add_agent(char *response, char *ip, char *agentname, char *groups, char **id, char **key){

    /* Add the new agent */
    int index;

    if (index = OS_AddNewAgent(&keys, NULL, agentname, ip, NULL), index < 0) {
        merror("Unable to add agent: %s (internal error)", agentname);
        snprintf(response, 2048, "ERROR: Internal manager error adding agent: %s\n\n", agentname);        
        return OS_INVALID;
    }

    /* Add the agent to the centralized configuration group */
    if(*groups) {
        char path[PATH_MAX];
        if (snprintf(path, PATH_MAX, isChroot() ? GROUPS_DIR "/%s" : DEFAULTDIR GROUPS_DIR "/%s", keys.keyentries[index]->id) >= PATH_MAX) {
            merror("At set_agent_group(): file path too large for agent '%s'.", keys.keyentries[index]->id);
            OS_RemoveAgent(keys.keyentries[index]->id);
            merror("Unable to set agent centralized group: %s (internal error)", groups);
            snprintf(response, 2048, "ERROR: Internal manager error setting agent centralized group: %s\n\n", groups);
            return OS_INVALID;
        }
    }

    *id = keys.keyentries[index]->id;
    *key = keys.keyentries[index]->key;

    return OS_SUCCESS;
}

/* Thread for writing keystore onto disk */
void* run_writer(__attribute__((unused)) void *arg) {
    keystore *copy_keys;
    struct keynode *copy_insert;
    struct keynode *copy_backup;
    struct keynode *copy_remove;
    struct keynode *cur;
    struct keynode *next;
    time_t cur_time;
    char wdbquery[OS_SIZE_128];
    char wdboutput[128];
    int wdb_sock = -1;

    authd_sigblock();

    while (running) {
        w_mutex_lock(&mutex_keys);

        while (!write_pending && running)
            w_cond_wait(&cond_pending, &mutex_keys);

        copy_keys = OS_DupKeys(&keys);
        copy_insert = queue_insert;
        copy_backup = queue_backup;
        copy_remove = queue_remove;
        queue_insert = NULL;
        queue_backup = NULL;
        queue_remove = NULL;
        insert_tail = &queue_insert;
        backup_tail = &queue_backup;
        remove_tail = &queue_remove;
        write_pending = 0;
        w_mutex_unlock(&mutex_keys);

        if (OS_WriteKeys(copy_keys) < 0)
            merror("Couldn't write file client.keys");

        OS_FreeKeys(copy_keys);
        free(copy_keys);
        cur_time = time(0);

        for (cur = copy_insert; cur; cur = next) {
            next = cur->next;
            OS_AddAgentTimestamp(cur->id, cur->name, cur->ip, cur_time);

            if(cur->group){
                if(set_agent_group(cur->id,cur->group) == -1){
                    merror("Unable to set agent centralized group: %s (internal error)", cur->group);
                }

                set_agent_multigroup(cur->group);
            }

            free(cur->id);
            free(cur->name);
            free(cur->ip);
            free(cur->group);
            free(cur);
        }

        for (cur = copy_backup; cur; cur = next) {
            next = cur->next;
            OS_BackupAgentInfo(cur->id, cur->name, cur->ip);

            snprintf(wdbquery, OS_SIZE_128, "agent %s remove", cur->id);
            wdbc_query_ex(&wdb_sock, wdbquery, wdboutput, sizeof(wdboutput));

            free(cur->id);
            free(cur->name);
            free(cur->ip);
            free(cur);
        }

        for (cur = copy_remove; cur; cur = next) {
            char full_name[FILE_SIZE + 1];
            next = cur->next;
            snprintf(full_name, sizeof(full_name), "%s-%s", cur->name, cur->ip);
            delete_agentinfo(cur->id, full_name);
            OS_RemoveCounter(cur->id);
            OS_RemoveAgentTimestamp(cur->id);
            OS_RemoveAgentGroup(cur->id);

            snprintf(wdbquery, OS_SIZE_128, "agent %s remove", cur->id);
            wdbc_query_ex(&wdb_sock, wdbquery, wdboutput, sizeof(wdboutput));

            free(cur->id);
            free(cur->name);
            free(cur->ip);
            free(cur);
        }
    }

    return NULL;
}

// Append key to insertion queue
void add_insert(const keyentry *entry,const char *group) {
    struct keynode *node;

    os_calloc(1, sizeof(struct keynode), node);
    node->id = strdup(entry->id);
    node->name = strdup(entry->name);
    node->ip = strdup(entry->ip->ip);
    node->group = NULL;

    if(group != NULL)
        node->group = strdup(group);

    (*insert_tail) = node;
    insert_tail = &node->next;
}

// Append key to backup queue
void add_backup(const keyentry *entry) {
    struct keynode *node;

    os_calloc(1, sizeof(struct keynode), node);
    node->id = strdup(entry->id);
    node->name = strdup(entry->name);
    node->ip = strdup(entry->ip->ip);

    (*backup_tail) = node;
    backup_tail = &node->next;
}

// Append key to deletion queue
void add_remove(const keyentry *entry) {
    struct keynode *node;

    os_calloc(1, sizeof(struct keynode), node);
    node->id = strdup(entry->id);
    node->name = strdup(entry->name);
    node->ip = strdup(entry->ip->ip);

    (*remove_tail) = node;
    remove_tail = &node->next;
}

/* To avoid hp-ux requirement of strsignal */
#ifdef __hpux
char* strsignal(int sig)
{
    static char str[12];
    sprintf(str, "%d", sig);
    return str;
}
#endif

/* Signal handler */
void handler(int signum) {
    switch (signum) {
    case SIGHUP:
    case SIGINT:
    case SIGTERM:
        minfo(SIGNAL_RECV, signum, strsignal(signum));
        running = 0;
        break;
    default:
        merror("unknown signal (%d)", signum);
    }
}

/* Exit handler */
void cleanup() {
    DeletePID(ARGV0);
}

void authd_sigblock() {
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGTERM);
    sigaddset(&sigset, SIGHUP);
    sigaddset(&sigset, SIGINT);
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);
}